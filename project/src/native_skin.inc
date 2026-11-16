// NATIVE 4-BONE SKINNING (2026-10-23, perf round 5). 82232D48(r3 skeleton: +256 bone matrices (48 bytes: a 3x4 matrix,
// rows +0/+12/+24/+36 = x/y/z axis + translation), r4 -> +8 index list (+4 u16 vertex indices, +10 u16 count),
// r5 -> +8 vertex stream (+4 base, +16 u16 stride, +20 u16 format: 1 or 3, otherwise stop), r6 output float3 per
// vertex index). Each listed vertex (float3 pos, 4 weight bytes at +12, 4 bone bytes at +16) becomes
// sum_j w_j * (M_j * pos) / (w0 + w1 + w2 + w3) with exactly the original's float ops, load/store order (the output is
// re-read between bones) and guest stack writes (savegprlr_27 at r1-48..r1-8, the fcfid scratch at r1-96..r1-64).
// Job workers (shadow / character jobs, ~1.7 % of all busy samples at the air spot).
// dist\native_skin_off = original; dist\native_skin_verify = 1 call in 32 runs the native version into a scratch
// copy of the output first, then the original, and compares the touched output + stack.
extern "C" void __imp__sub_82232D48(PPCContext& ctx, uint8_t* base);
namespace native_skin {
using u8 = uint8_t; using u32 = uint32_t; using u64 = uint64_t; using s64 = int64_t;
inline u8* G(u8* b, u32 a) { return b + a + REX_PHYS_HOST_OFFSET(a); }
inline double Fl(u8* b, u32 a) { u32 v; std::memcpy(&v, G(b, a), 4); v = __builtin_bswap32(v); float f; std::memcpy(&f, &v, 4); return double(f); }
inline void St(u8* b, u32 a, double d) { const float f = float(d); u32 v; std::memcpy(&v, &f, 4); v = __builtin_bswap32(v); std::memcpy(G(b, a), &v, 4); }
inline u32 U(u8* b, u32 a) { u32 v; std::memcpy(&v, G(b, a), 4); return __builtin_bswap32(v); }
inline u32 H(u8* b, u32 a) { uint16_t v; std::memcpy(&v, G(b, a), 2); return __builtin_bswap16(v); }
inline u32 B(u8* b, u32 a) { return *G(b, a); }
inline void Wr64(u8* b, u32 a, u64 v) { v = __builtin_bswap64(v); std::memcpy(G(b, a), &v, 8); }
inline void Wr(u8* b, u32 a, u32 v) { v = __builtin_bswap32(v); std::memcpy(G(b, a), &v, 4); }
inline double R(double x) { return double(float(x)); }
int Mode() {
    static const int m = [] {
        // dist\native_skin_verify = verify, native_skin_off = off.
        int r = 1;  // default ON (verify 0 differ; c19h A/B 4/4 pairs, +3.7 %)
        if (FILE* f = std::fopen("native_skin_off", "rb")) { std::fclose(f); r = 0; }
        else if (FILE* f2 = std::fopen("native_skin_verify", "rb")) { std::fclose(f2); r = 2; }
        else if (FILE* f3 = std::fopen("native_skin", "rb")) { std::fclose(f3); r = 1; }
        REXLOG_INFO("Native 4-bone skinning (82232D48): {}", r == 0 ? "off" : r == 1 ? "on" : "VERIFY");
        return r;
    }();
    return m;
}
// One bone's contribution. first: out = t*w; else out = t*w + out (out re-read in the original's order).
// x: M24*z, +M12*y, +x*M0, +M36   y: M16*y, +M4*x, +M28*z, +M40   z: M20*y, +M8*x, +M32*z, +M44
struct Out {
    u8* b; u8* ho; u32 ob;  // ho: host scratch copy of the output (verify), indexed by (address - ob)
    double Ld(u32 a) const { if (!ho) return Fl(b, a); u32 v; std::memcpy(&v, ho + (a - ob), 4); v = __builtin_bswap32(v); float f; std::memcpy(&f, &v, 4); return double(f); }
    void St(u32 a, double d) const { if (!ho) { native_skin::St(b, a, d); return; } const float f = float(d); u32 v; std::memcpy(&v, &f, 4); v = __builtin_bswap32(v); std::memcpy(ho + (a - ob), &v, 4); }
};
inline void Bone(u8* b, const Out& O, u32 v, u32 m, u32 o, double w, bool first) {
    double t = R(Fl(b, m + 24) * Fl(b, v + 8));
    t = R(Fl(b, m + 12) * Fl(b, v + 4) + t);
    t = R(Fl(b, v + 0) * Fl(b, m + 0) + t);
    t = R(t + Fl(b, m + 36));
    if (first) O.St(o, R(t * w)); else O.St(o, R(t * w + O.Ld(o)));
    t = R(Fl(b, m + 16) * Fl(b, v + 4));
    t = R(Fl(b, m + 4) * Fl(b, v + 0) + t);
    t = R(Fl(b, m + 28) * Fl(b, v + 8) + t);
    t = R(t + Fl(b, m + 40));
    if (first) O.St(o + 4, R(t * w)); else O.St(o + 4, R(t * w + O.Ld(o + 4)));
    t = R(Fl(b, m + 20) * Fl(b, v + 4));
    t = R(Fl(b, m + 8) * Fl(b, v + 0) + t);
    t = R(Fl(b, m + 32) * Fl(b, v + 8) + t);
    t = R(t + Fl(b, m + 44));
    if (first) O.St(o + 8, R(t * w)); else O.St(o + 8, R(t * w + O.Ld(o + 8)));
}
// Runs the whole function on guest memory (out_base = r6). Writes the stack like the original.
inline void Run(PPCContext& ctx, u8* b, u32 out_base, u8* host_out = nullptr, u8* host_stack = nullptr) {
    const Out O{b, host_out, out_base};
    const u32 r1 = ctx.r1.u32;
    // host_stack (verify): 96-byte image of guest r1-96..r1-1 instead of the guest stack
    auto Wr64 = [&](u8* bb, u32 a, u64 val) { if (host_stack) { val = __builtin_bswap64(val); std::memcpy(host_stack + (a - (r1 - 96)), &val, 8); } else native_skin::Wr64(bb, a, val); };
    auto Wr = [&](u8* bb, u32 a, u32 val) { if (host_stack) { val = __builtin_bswap32(val); std::memcpy(host_stack + (a - (r1 - 96)), &val, 4); } else native_skin::Wr(bb, a, val); };
    // savegprlr_27
    Wr64(b, r1 - 48, ctx.r27.u64); Wr64(b, r1 - 40, ctx.r28.u64); Wr64(b, r1 - 32, ctx.r29.u64);
    Wr64(b, r1 - 24, ctx.r30.u64); Wr64(b, r1 - 16, ctx.r31.u64); Wr(b, r1 - 8, u32(ctx.lr));
    const u32 list = U(b, ctx.r4.u32 + 8);
    const u32 count = H(b, list + 10);
    if (int32_t(count) <= 0) return;
    const u32 r5 = ctx.r5.u32;
    const u32 stream = U(b, r5 + 8);
    const double one = Fl(b, 0x820875F8u);
    for (u32 i = 0; i < count; ++i) {
        const u32 fmt = H(b, stream + 20);
        const u32 idx = H(b, U(b, list + 4) + i * 2);
        const u32 v = H(b, stream + 16) * idx + U(b, stream + 4);
        const u32 o = idx * 12u + out_base;
        if (fmt != 1 && H(b, U(b, r5 + 8) + 20) != 3) return;
        const u32 mats = U(b, ctx.r3.u32 + 256);
        for (u32 j = 0; j < 4; ++j) {
            const u64 wb = B(b, v + 12 + j);
            Wr64(b, r1 - 96 + 8 * j, wb);
            const double w = R(double(s64(wb)));
            Bone(b, O, v, B(b, v + 16 + j) * 48u + mats, o, w, j == 0);
        }
        const s64 sum = s64(int32_t(B(b, v + 12 + 3) + B(b, v + 12 + 1) + B(b, v + 12 + 2) + B(b, v + 12 + 0)));
        Wr64(b, r1 - 64, u64(sum));
        const double x = O.Ld(o), y = O.Ld(o + 4), z = O.Ld(o + 8);
        const double f0 = R(one / R(double(sum)));
        O.St(o, R(f0 * x));
        O.St(o + 4, R(y * f0));
        O.St(o + 8, R(f0 * z));
    }
}
std::atomic<uint64_t> v_calls{0}, v_bad{0};
}  // namespace native_skin

PPC_FUNC(sub_82232D48) {
    using namespace native_skin;
    const int mode = Mode();
    if (mode == 0) { __imp__sub_82232D48(ctx, base); return; }
    ctx.fpscr.disableFlushMode();
    if (mode == 2) {
        static thread_local uint32_t tick = 0;
        if ((++tick & 31) == 0) {
            // Touched output range: max index + 1 vertices (indices from the list).
            const u32 list = U(base, ctx.r4.u32 + 8);
            const u32 count = H(base, list + 10);
            u32 maxi = 0;
            for (u32 i = 0; i < count; ++i) maxi = std::max(maxi, H(base, U(base, list + 4) + i * 2));
            const u32 out = ctx.r6.u32, bytes = (maxi + 1) * 12u, r1 = ctx.r1.u32;
            // Native first, on PRIVATE copies (output + stack image); guest memory untouched. Then the original
            // runs for real and the results are compared (no rollback of live memory).
            std::vector<u8> mine(G(base, out), G(base, out) + bytes);
            u8 mstack[96];
            std::memcpy(mstack, G(base, r1 - 96), 96);
            Run(ctx, base, out, mine.data(), mstack);
            __imp__sub_82232D48(ctx, base);
            const bool bo = std::memcmp(mine.data(), G(base, out), bytes) != 0;
            const bool bs = std::memcmp(mstack, G(base, r1 - 96), 96) != 0;
            const uint64_t c = ++v_calls;
            if ((bo || bs) && v_bad.fetch_add(1) < 12)
                REXLOG_INFO("NATIVE SKIN differs: count {} output {} stack {}", count, bo ? "differs" : "same", bs ? "differs" : "same");
            if ((c & 1023) == 0) REXLOG_INFO("NATIVE SKIN VERIFY: {} compared, {} differ", c, v_bad.load());
            return;
        }
    }
    Run(ctx, base, ctx.r6.u32);
}
