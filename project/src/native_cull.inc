// NATIVE BOX-vs-FRUSTUM TEST 2 (2026-10-23, perf round 5). 82125E30(r3 corners: float3 x 8, r4 frustum: +0 plane
// count 3..9, +20+16*i corner index of plane i, +176+16*i plane (a, b, c, d)) is 82125A98's twin (that one uses the
// corner index at +16+16*i and allows 10 planes): per plane d = ((b*y) + c*z) + a*x + d of the chosen corner,
// fsel(d >= 0 ? [0x82089248] : [0x82089238]) rounded to float, summed from the last plane down (count 9 starts
// from plane 8's value, otherwise from the float at 0x820875EC); returns (sum > that float) ? 0 : 1, count outside
// 3..9 -> 1. Callers 82126B40 / 82126EA0 / 82127138 (visibility walk on the job workers, ~2.2 % of all busy samples
// at the air spot). Also its guest stack writes (lr at r1-8, savefpr_25 = f25..f31 at r1-64..r1-16).
// dist\native_cull_off = original; dist\native_cull_verify = 1 call in 32 runs both and compares (r3 + stack).
extern "C" void __imp__sub_82125E30(PPCContext& ctx, uint8_t* base);
namespace native_cull {
using u8 = uint8_t; using u32 = uint32_t; using u64 = uint64_t;
inline u8* G(u8* b, u32 a) { return b + a + REX_PHYS_HOST_OFFSET(a); }
inline float Fl(u8* b, u32 a) { u32 v; std::memcpy(&v, G(b, a), 4); v = __builtin_bswap32(v); float f; std::memcpy(&f, &v, 4); return f; }
inline u32 U(u8* b, u32 a) { u32 v; std::memcpy(&v, G(b, a), 4); return __builtin_bswap32(v); }
inline double D64(u8* b, u32 a) { u64 v; std::memcpy(&v, G(b, a), 8); v = __builtin_bswap64(v); double d; std::memcpy(&d, &v, 8); return d; }
inline void Wr64(u8* b, u32 a, u64 v) { v = __builtin_bswap64(v); std::memcpy(G(b, a), &v, 8); }
inline void Wr(u8* b, u32 a, u32 v) { v = __builtin_bswap32(v); std::memcpy(G(b, a), &v, 4); }
inline double R(double x) { return double(float(x)); }
int Mode() {
    static const int m = [] {
        // dist\native_cull_verify = verify, native_cull_off = off.
        int r = 1;  // default ON (verify 0 differ; c19h A/B 4/4 pairs, +3.7 %)
        if (FILE* f = std::fopen("native_cull_off", "rb")) { std::fclose(f); r = 0; }
        else if (FILE* f2 = std::fopen("native_cull_verify", "rb")) { std::fclose(f2); r = 2; }
        else if (FILE* f3 = std::fopen("native_cull", "rb")) { std::fclose(f3); r = 1; }
        REXLOG_INFO("Native frustum test 2 (82125E30): {}", r == 0 ? "off" : r == 1 ? "on" : "VERIFY");
        return r;
    }();
    return m;
}
inline u32 Frustum2(u8* b, u32 corners, u32 fr) {
    const u32 count = U(b, fr);
    const double f9 = double(Fl(b, 0x820875ECu));
    if (count - 3u > 6u) return 1;
    const double f12 = D64(b, 0x82089238u), f13 = D64(b, 0x82089248u);
    auto val = [&](u32 i) {
        const u32 c = corners + U(b, fr + 20 + 16 * i) * 12u;
        const u32 p = fr + 176 + 16 * i;
        const double a = double(Fl(b, p)), bb = double(Fl(b, p + 4)), cc = double(Fl(b, p + 8)), d = double(Fl(b, p + 12));
        const double x = double(Fl(b, c)), y = double(Fl(b, c + 4)), z = double(Fl(b, c + 8));
        double t = R(bb * y);
        t = R(cc * z + t);
        t = R(a * x + t);
        t = R(t + d);
        return R(t >= 0.0 ? f13 : f12);
    };
    double s;
    int i = int(count) - 1;
    if (count == 9) { s = val(8); i = 7; } else { s = f9; }
    for (; i >= 3; --i) s = R(val(u32(i)) + s);
    s = R(val(2) + s);
    s = R(s + val(1));
    s = R(s + val(0));
    return s > f9 ? 0u : 1u;
}
inline void StackWrites(PPCContext& ctx, u8* b) {
    const u32 r1 = ctx.r1.u32;
    Wr(b, r1 - 8, u32(ctx.lr));
    Wr64(b, r1 - 64, ctx.f25.u64); Wr64(b, r1 - 56, ctx.f26.u64); Wr64(b, r1 - 48, ctx.f27.u64);
    Wr64(b, r1 - 40, ctx.f28.u64); Wr64(b, r1 - 32, ctx.f29.u64); Wr64(b, r1 - 24, ctx.f30.u64);
    Wr64(b, r1 - 16, ctx.f31.u64);
}
std::atomic<uint64_t> v_calls{0}, v_bad{0};
}  // namespace native_cull

PPC_FUNC(sub_82125E30) {
    using namespace native_cull;
    const int mode = Mode();
    if (mode == 0) { __imp__sub_82125E30(ctx, base); return; }
    ctx.fpscr.disableFlushMode();
    const u32 r = Frustum2(base, ctx.r3.u32, ctx.r4.u32);
    if (mode == 2) {
        static thread_local uint32_t tick = 0;
        if ((++tick & 31) == 0) {
            const u32 r1 = ctx.r1.u32;
            u8 mine[64];
            StackWrites(ctx, base);
            std::memcpy(mine, G(base, r1 - 64), 64);
            __imp__sub_82125E30(ctx, base);
            const bool bad = ctx.r3.u32 != r || std::memcmp(mine, G(base, r1 - 64), 64) != 0;
            const uint64_t c = ++v_calls;
            if (bad && v_bad.fetch_add(1) < 12)
                REXLOG_INFO("NATIVE CULL differs: native {} original {} (stack {})", r, ctx.r3.u32,
                            std::memcmp(mine, G(base, r1 - 64), 64) != 0 ? "differs" : "same");
            if ((c & 16383) == 0) REXLOG_INFO("NATIVE CULL VERIFY: {} compared, {} differ", c, v_bad.load());
            return;
        }
    }
    StackWrites(ctx, base);
    ctx.r3.u64 = r;
}
