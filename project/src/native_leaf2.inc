#include <climits>
// NATIVE MAIN/WORKER LEAVES, batch 2 (AGENT_NATIVE22B 2026-10-27). Candidates from codexMT3b; every function checked offline
// against its generated body (random + NaN/inf/denormal inputs, outputs aliasing inputs, mutation tests).
//   8262A748 point-segment parameter + dist2   82683030 float->int helper     826FCC98 trig polynomial
//   8249FCB0 plane build (+ 826444F8 corner selectors)   82643CA0 AABB through 3x3   826445E8 AABB corner select
//   8262B850 sphere-vs-AABB   826442F8 transformed AABB.  (82644800 arena alloc skipped: generated code is already a CAS loop.)
// Only the return registers are written (all call sites checked: no caller reads another volatile register the original
// leaves behind). Natives that write guest memory keep the original load/store order (outputs may alias inputs).
// dist\native_leaf2 = on; native_leaf2_verify = 1 call in 16 compared against the original; native_leaf2_off wins. Default OFF.
extern "C" void __imp__sub_8262A748(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_82683030(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_8249FCB0(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_826FCC98(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_82643CA0(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_826445E8(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_8262B850(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_826442F8(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_826444F8(PPCContext& ctx, uint8_t* base);
namespace native_leaf2 {
using namespace native_geom;
int Mode() {
    static const int m = [] {
        int r = 0;
        if (FILE* f = std::fopen("native_leaf2_off", "rb")) { std::fclose(f); r = 0; }
        else if (FILE* f2 = std::fopen("native_leaf2_verify", "rb")) { std::fclose(f2); r = 2; }
        else if (FILE* f3 = std::fopen("native_leaf2", "rb")) { std::fclose(f3); r = 1; }
        REXLOG_INFO("Native leaves 2 (8262A748 82683030 826FCC98 8249FCB0 826444F8 82643CA0 826445E8 8262B850 826442F8): {}", r == 0 ? "off" : r == 1 ? "on" : "VERIFY");
        return r;
    }();
    return m;
}
std::atomic<uint64_t> v_calls{0}, v_bad{0};
void Report(const char* what, bool bad) {
    const uint64_t n = v_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (bad) {
        const uint64_t b = v_bad.fetch_add(1, std::memory_order_relaxed) + 1;
        if (b <= 8) REXLOG_INFO("NATIVE LEAF2 differs: {}", what);
    }
    if ((n & 0x3FFFu) == 0)
        REXLOG_INFO("NATIVE LEAF2 VERIFY: {} compared, {} differ", n, v_bad.load(std::memory_order_relaxed));
}
inline bool Verify() {
    static thread_local uint32_t tick = 0;
    return (++tick & 15) == 0;
}
inline u32 FBits(double d) { const float f = float(d); u32 v; std::memcpy(&v, &f, 4); return v; }
inline double D64(u8* b, u32 a) { u64 v; std::memcpy(&v, G(b, a), 8); v = __builtin_bswap64(v); double d; std::memcpy(&d, &v, 8); return d; }
inline void WF(u8* b, u32 a, double d) { W(b, a, FBits(d)); }
inline u64 DBits(double d) {
    u64 v;
    std::memcpy(&v, &d, sizeof(v));
    return v;
}

inline double DFromBits(u64 v) {
    double d;
    std::memcpy(&d, &v, sizeof(d));
    return d;
}

inline double AbsD(double d) {
    return DFromBits(DBits(d) & 0x7FFFFFFFFFFFFFFFull);
}

inline double NegD(double d) {
    return DFromBits(DBits(d) ^ 0x8000000000000000ull);
}

// 8262A748
struct SegmentResult {
    u32 parameter;
    double distance2;
};

inline SegmentResult SegmentParameter(u8* b, u32 point, u32 a, u32 c) {
    const double ax = F(b, a + 0), ay = F(b, a + 4), az = F(b, a + 8);
    const double cx = F(b, c + 0), cy = F(b, c + 4), cz = F(b, c + 8);
    const double px = F(b, point + 0), py = F(b, point + 4);
    const double pz = F(b, point + 8);

    const double dx = R(cx - ax);
    const double dz = R(cz - az);
    const double dy = R(cy - ay);
    const double qx = R(px - ax);
    const double qz = R(pz - az);
    const double qy = R(py - ay);

    double length2 = R(dx * dx);
    length2 = R(std::fma(dz, dz, length2));
    length2 = R(std::fma(dy, dy, length2));

    if (length2 < F(b, 0x82089660u)) {
        double d2 = R(qx * qx);
        d2 = R(std::fma(qy, qy, d2));
        d2 = R(std::fma(qz, qz, d2));
        return {FBits(F(b, 0x8202073Cu)), d2};
    }

    double dot = R(dx * qx);
    dot = R(std::fma(dz, qz, dot));
    dot = R(std::fma(dy, qy, dot));
    double t = R(dot / length2);

    const double below_zero = D64(b, 0x82089238u);
    const double above_one = D64(b, 0x82089248u);
    const double one = F(b, 0x820875F8u);

    double f10 = R(t >= 0.0 ? t : below_zero);
    double f5 = R(t >= 0.0 ? t : below_zero);
    const double test = R(f10 - one);
    t = R(test >= 0.0 ? above_one : f5);

    const double cy2 = R(R(dy * t) + ay);
    const double cz2 = R(az + R(dz * t));
    const double cx2 = R(ax + R(dx * t));

    const double ey = R(cy2 - py);
    const double ez = R(cz2 - pz);
    const double ex = R(cx2 - px);

    double d2 = R(ey * ey);
    d2 = R(std::fma(ez, ez, d2));
    d2 = R(std::fma(ex, ex, d2));
    return {FBits(t), d2};
}

// 826FCC98
inline int64_t FctidRound(double x) {
    return std::isnan(x)
        ? int64_t(0x8000000000000000ull)
        : (x > double(LLONG_MAX))
              ? LLONG_MAX
              : simde_mm_cvtsd_si64(simde_mm_load_sd(&x));
}

inline int64_t FctidTrunc(double x) {
    return std::isnan(x)
        ? int64_t(0x8000000000000000ull)
        : (x > double(LLONG_MAX))
              ? LLONG_MAX
              : simde_mm_cvttsd_si64(simde_mm_load_sd(&x));
}

inline double Trig826FCC98(u8* b, double input) {
    const u32 table = 0x827A7EA8u;
    const double a = AbsD(input);

    const double sum = D64(b, table + 0) + a;
    double nvalue = D64(b, table + 8) * sum;
    const int64_t n = FctidRound(nvalue);
    nvalue = double(n);

    const double adjusted = nvalue - F(b, table + 36);
    const int64_t parity = FctidTrunc(nvalue);

    double reduced = -std::fma(D64(b, table + 40), adjusted, -a);
    reduced = -std::fma(D64(b, table + 48), adjusted, -reduced);
    const double square = reduced * reduced;

    double poly = std::fma(D64(b, table + 112), square,
                           D64(b, table + 104));
    poly = std::fma(poly, square, D64(b, table + 96));
    poly = std::fma(poly, square, D64(b, table + 88));
    poly = std::fma(poly, square, D64(b, table + 80));
    poly = std::fma(poly, square, D64(b, table + 72));
    poly = std::fma(poly, square, D64(b, table + 64));
    poly = std::fma(poly, square, D64(b, table + 56));
    double value = std::fma(poly, square, D64(b, 0x82089248u));
    value *= reduced;

    if (u64(parity) & 1u)
        value = NegD(value);

    const double range_test = sum - D64(b, table + 16);
    const double selected =
        range_test >= 0.0 ? D64(b, 0x827A8500u) : value;

    return a == F(b, table + 24) ? F(b, table + 28) : selected;
}

// 826445E8
inline bool SelectCorner(u8* b, u32 selector, u32 lo, u32 hi,
                         u32 (&out)[3]) {
    if (selector > 7)
        return false;

    u32 l[3], h[3];
    for (int i = 0; i < 3; ++i) {
        l[i] = U(b, lo + 4u * i);
        h[i] = U(b, hi + 4u * i);
    }

    switch (selector) {
    case 0: out[0] = h[0]; out[1] = h[1]; out[2] = l[2]; break;
    case 1: out[0] = h[0]; out[1] = l[1]; out[2] = l[2]; break;
    case 2: out[0] = l[0]; out[1] = l[1]; out[2] = l[2]; break;
    case 3: out[0] = l[0]; out[1] = h[1]; out[2] = l[2]; break;
    case 4: out[0] = h[0]; out[1] = h[1]; out[2] = h[2]; break;
    case 5: out[0] = h[0]; out[1] = l[1]; out[2] = h[2]; break;
    case 6: out[0] = l[0]; out[1] = l[1]; out[2] = h[2]; break;
    default: out[0] = l[0]; out[1] = h[1]; out[2] = h[2]; break;
    }
    return true;
}

// 8262B850
inline u32 SphereAabb(u8* b, u32 point, double radius,
                      u32 lo, u32 hi) {
    double distance2 = F(b, 0x820875ECu);
    for (int i = 0; i < 3; ++i) {
        const double p = F(b, point + 4u * i);
        const double l = F(b, lo + 4u * i);
        double edge = l;

        if (!(p < l)) {
            const double h = F(b, hi + 4u * i);
            if (!(p > h))
                continue;
            edge = h;
        }

        const double d = R(p - edge);
        if (i == 0)
            distance2 = R(d * d);
        else
            distance2 = R(std::fma(d, d, distance2));
    }

    const double radius2 = R(radius * radius);
    return distance2 > radius2 ? 0u : 1u;
}

// 82683030: float -> int conversion helper, transliterated with the generated code's 64-bit register arithmetic.
inline u64 SrawiS(u64 v, u32 sh) { return u64(int64_t(int32_t(u32(v)) >> sh)); }
inline u64 Sraw(u64 v, u64 amount) { u32 sh = u32(amount) & 0x3Fu; if (sh > 31) sh = 31; return u64(int64_t(int32_t(u32(v)) >> sh)); }
inline u64 FloatInteger(double input) {
    const float sf = float(input);
    u32 bits; std::memcpy(&bits, &sf, 4);
    u64 r9 = bits;
    u64 r11 = (r9 >> 23) & 0xFFu;
    u64 r10 = r9 & 0x7FFFFFFFu;
    r11 = r11 - 127u;
    u64 r7 = r10 - 1u;
    u64 r8 = r11 - 23u;
    r10 = r8 - 1u;
    r10 = SrawiS(r10, 31);
    u64 r6 = 23u - r11;
    r7 = SrawiS(r7, 31);
    r9 = r9 & ~r7;
    r7 = r6 & r10;
    r6 = ~r10;
    const u64 r3 = (r9 & 0xFFFFFFFF007FFFFFull) | 0x00800000u;
    r10 = r10 & 23u;
    const u64 r4 = r10 - r7;
    r10 = r6 & r8;
    r8 = Sraw(0xFFFFFFFFFF800000ull, r4);
    r9 = SrawiS(r9, 31);
    r8 = r8 | r6;
    r6 = SrawiS(r11, 31);
    r11 = r3 & r8;
    r8 = u64((u32(r11) << 1) & 0xFFFFFFFEu);
    r8 = r8 - 1u;
    r9 = r8 | r9;
    r11 = r9 - r11;
    r11 = r11 + 1u;
    r11 = r11 & ~r6;
    r11 = Sraw(r11, r7);
    return (u8(r10) & 0x20u) ? 0u : u64(u32(u32(r11) << (u8(r10) & 0x3Fu)));
}

// 826444F8: corner selectors of a plane normal (stores the SECOND output first, like the generated code).
struct OctantResult { u32 first; u32 second; };
inline OctantResult Octants(double x, double y, double z, double zero) {
    if (x > zero) {
        if (y > zero) return z > zero ? OctantResult{2, 4} : OctantResult{6, 0};
        return z > zero ? OctantResult{3, 5} : OctantResult{7, 1};
    }
    if (y > zero) return z > zero ? OctantResult{1, 7} : OctantResult{5, 3};
    return z > zero ? OctantResult{0, 6} : OctantResult{4, 2};
}
inline void OctantsStore(u8* b, u32 out_first, u32 out_second, u32 v) {
    const double x = F(b, v);
    const double zero = F(b, 0x820875ECu);
    const double y = F(b, v + 4), z = F(b, v + 8);
    const OctantResult o = Octants(x, y, z, zero);
    W(b, out_second, o.second);
    W(b, out_first, o.first);
}

// 8249FCB0: plane through p (r5), a (r6), c (r7) -> plane at base + (index+11)*16, selectors at base + index*16 + 16/20.
// Same load/store order as the generated code (the plane is written and read back word by word; p is re-read after the stores).
inline void BuildPlane(u8* b, u32 base_addr, u32 index, u32 p, u32 a, u32 c) {
    double f12 = F(b, a + 8), f9 = F(b, c + 8);
    double f13 = F(b, a + 4);
    f9 = R(f9 - f12);
    double f10 = F(b, c + 4);
    double f0 = F(b, a + 0);
    f10 = R(f10 - f13);
    double f6 = F(b, p + 8);
    const u32 pl = base_addr + ((index + 11u) << 4);
    double f11 = F(b, c + 0);
    f12 = R(f12 - f6);
    double f8 = F(b, p + 0);
    f11 = R(f11 - f0);
    f0 = R(f0 - f8);
    double f7 = F(b, p + 4);
    f13 = R(f13 - f7);
    const u32 sel = base_addr + (index << 4);
    f8 = R(f12 * f10);
    f7 = R(f0 * f9);
    f6 = R(f11 * f13);
    f13 = R(std::fma(f13, f9, -f8));
    const u32 w0 = FBits(f13);
    f13 = R(std::fma(f11, f12, -f7));
    const u32 w1 = FBits(f13);
    f0 = R(std::fma(f0, f10, -f6));
    const u32 w2 = FBits(f0);
    W(b, pl + 0, w0);
    f12 = F(b, pl + 0);
    f10 = f12;
    W(b, pl + 4, w1);
    f0 = F(b, pl + 4);
    f11 = R(f0 * f0);
    W(b, pl + 8, w2);
    f13 = F(b, pl + 8);
    f12 = R(std::fma(f12, f12, f11));
    f11 = F(b, 0x820875F8u);
    f12 = R(std::fma(f13, f13, f12));
    f12 = R(std::sqrt(f12));
    f12 = R(f11 / f12);
    f0 = R(f0 * f12);
    WF(b, pl + 4, f0);
    f13 = R(f13 * f12);
    WF(b, pl + 8, f13);
    f11 = R(f10 * f12);
    WF(b, pl + 0, f11);
    f12 = F(b, p + 4);
    f10 = F(b, p + 0);
    f0 = R(f0 * f12);
    f12 = F(b, p + 8);
    f0 = R(std::fma(f13, f12, f0));
    f13 = R(std::fma(f10, f11, f0));
    f0 = F(b, 0x827AC214u);
    f0 = R(f13 * f0);
    WF(b, pl + 12, f0);
    OctantsStore(b, sel + 16, sel + 20, pl);
}

// 82643CA0: AABB (lo r5, hi r6) through a 3x3 matrix (r4, column stride 12) -> out[0..2] low, out[3..5] high.
// The generated code stores the running sums after every column and only then loads the next inputs (aliasing-exact).
inline void MatrixRange(u8* b, u32 out, u32 m, u32 lo, u32 hi) {
    for (u32 i = 0; i < 3; ++i) {
        double low = 0, high = 0;
        for (u32 j = 0; j < 3; ++j) {
            const double mm = F(b, m + 4u * i + 12u * j);
            const double l = F(b, lo + 4u * j);
            const double h = F(b, hi + 4u * j);
            const double p = (j == 2) ? R(l * mm) : R(mm * l);
            const double q = (j == 1) ? R(h * mm) : R(mm * h);
            const double d = R(p - q);
            const double tl = d >= 0.0 ? q : p;
            const double th = d >= 0.0 ? p : q;
            if (j == 0) { low = tl; high = th; }
            else if (j == 1) { low = R(tl + low); high = R(th + high); }
            else { low = R(low + tl); high = R(high + th); }
            WF(b, out + 4u * i, low);
            WF(b, out + 12u + 4u * i, high);
        }
    }
}
// Same on host arrays (826442F8's stack locals cannot alias anything).
inline void MatrixRangeLocal(u8* b, u32 m, const double (&l)[3], const double (&h)[3], double (&o)[6]) {
    for (u32 i = 0; i < 3; ++i) {
        double low = 0, high = 0;
        for (u32 j = 0; j < 3; ++j) {
            const double mm = F(b, m + 4u * i + 12u * j);
            const double p = (j == 2) ? R(l[j] * mm) : R(mm * l[j]);
            const double q = (j == 1) ? R(h[j] * mm) : R(mm * h[j]);
            const double d = R(p - q);
            const double tl = d >= 0.0 ? q : p;
            const double th = d >= 0.0 ? p : q;
            if (j == 0) { low = tl; high = th; }
            else if (j == 1) { low = R(tl + low); high = R(th + high); }
            else { low = R(low + tl); high = R(high + th); }
        }
        o[i] = low; o[i + 3] = high;
    }
}

// 826442F8: AABB (a r7, c r8) relative to origin r6 through matrix r5, back to world -> out_lo r3, out_hi r4.
// The origin is re-read after the matrix step and between the two output stores exactly like the generated code.
inline void TransformedRange(u8* b, u32 out_lo, u32 out_hi, u32 m, u32 org, u32 a, u32 c) {
    double f13 = F(b, a + 0), f12 = F(b, a + 4);
    const double f9 = F(b, c + 4);
    const double f11 = F(b, a + 8);
    const double f10 = F(b, c + 0);
    const double f8 = F(b, org + 0);
    double f0 = F(b, org + 4);
    double l[3], h[3];
    l[0] = R(f13 - f8);
    l[1] = R(f12 - f0);
    f13 = F(b, org + 8);
    h[1] = R(f9 - f0);
    const double f7 = F(b, c + 8);
    h[0] = R(f10 - f8);
    l[2] = R(f11 - f13);
    h[2] = R(f7 - f13);
    double o[6];
    MatrixRangeLocal(b, m, l, h, o);
    f0 = F(b, org + 4);
    const u32 w1 = FBits(R(o[1] + f0));
    const u32 w0 = FBits(R(f8 + o[0]));
    f13 = F(b, org + 8);
    const u32 w2 = FBits(R(f13 + o[2]));
    W(b, out_lo + 0, w0); W(b, out_lo + 4, w1); W(b, out_lo + 8, w2);
    const double x = F(b, org + 0);
    const u32 v0 = FBits(R(x + o[3]));
    const double y = F(b, org + 4);
    const u32 v1 = FBits(R(o[4] + y));
    const double z = F(b, org + 8);
    const u32 v2 = FBits(R(z + o[5]));
    W(b, out_hi + 0, v0); W(b, out_hi + 4, v1); W(b, out_hi + 8, v2);
}

// Verify for natives that write guest memory directly: save the output bytes, run the native, keep its bytes,
// restore, run the original for real, compare.
struct Span { u32 a, n; };
template <int N, class Fn>
inline void SandboxVerify(u8* b, const Span (&sp)[N], Fn&& native, PPCContext& ctx, void (*orig)(PPCContext&, uint8_t*), const char* what) {
    uint8_t save[N][32], mine[N][32];
    for (int k = 0; k < N; ++k) std::memcpy(save[k], G(b, sp[k].a), sp[k].n);
    native();
    for (int k = 0; k < N; ++k) std::memcpy(mine[k], G(b, sp[k].a), sp[k].n);
    for (int k = N - 1; k >= 0; --k) std::memcpy(G(b, sp[k].a), save[k], sp[k].n);
    orig(ctx, b);
    bool bad = false;
    for (int k = 0; k < N; ++k) bad |= std::memcmp(mine[k], G(b, sp[k].a), sp[k].n) != 0;
    Report(what, bad);
}
}  // namespace native_leaf2

PPC_FUNC(sub_8262A748) {
    namespace nl = native_leaf2;
    const int mode = nl::Mode();
    if (mode == 0) {
        __imp__sub_8262A748(ctx, base);
        return;
    }

    ctx.fpscr.disableFlushMode();
    const uint32_t out = ctx.r3.u32;
    const nl::SegmentResult result =
        nl::SegmentParameter(base, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);

    if (mode == 2 && nl::Verify()) {
        __imp__sub_8262A748(ctx, base);
        PPCRegister expected;
        expected.f64 = result.distance2;
        nl::Report("8262A748",
                   native_geom::U(base, out) != result.parameter ||
                   ctx.f1.u64 != expected.u64);
        return;
    }

    native_geom::W(base, out, result.parameter);
    ctx.f1.f64 = result.distance2;
}

PPC_FUNC(sub_826FCC98) {
    namespace nl = native_leaf2;
    const int mode = nl::Mode();
    if (mode == 0) {
        __imp__sub_826FCC98(ctx, base);
        return;
    }

    ctx.fpscr.disableFlushMode();
    const double result = nl::Trig826FCC98(base, ctx.f1.f64);
    if (mode == 2 && nl::Verify()) {
        __imp__sub_826FCC98(ctx, base);
        PPCRegister expected;
        expected.f64 = result;
        nl::Report("826FCC98", ctx.f1.u64 != expected.u64);
        return;
    }
    ctx.f1.f64 = result;
}

PPC_FUNC(sub_826445E8) {
    namespace nl = native_leaf2;
    using native_geom::u32;
    const int mode = nl::Mode();
    if (mode == 0) {
        __imp__sub_826445E8(ctx, base);
        return;
    }

    const u32 out = ctx.r3.u32;
    u32 words[3];
    const bool active =
        nl::SelectCorner(base, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, words);

    if (mode == 2 && nl::Verify()) {
        __imp__sub_826445E8(ctx, base);
        bool bad = false;
        if (active) {
            for (int i = 0; i < 3; ++i)
                bad |= native_geom::U(base, out + 4u * i) != words[i];
        }
        nl::Report("826445E8", bad);
        return;
    }

    if (active) {
        for (int i = 0; i < 3; ++i)
            native_geom::W(base, out + 4u * i, words[i]);
    }
}

PPC_FUNC(sub_8262B850) {
    namespace nl = native_leaf2;
    const int mode = nl::Mode();
    if (mode == 0) {
        __imp__sub_8262B850(ctx, base);
        return;
    }

    ctx.fpscr.disableFlushMode();
    const uint32_t result =
        nl::SphereAabb(base, ctx.r3.u32, ctx.f1.f64,
                       ctx.r5.u32, ctx.r6.u32);

    if (mode == 2 && nl::Verify()) {
        __imp__sub_8262B850(ctx, base);
        nl::Report("8262B850", ctx.r3.u64 != result);
        return;
    }
    ctx.r3.u64 = result;
}

PPC_FUNC(sub_82683030) {
    namespace nl = native_leaf2;
    const int mode = nl::Mode();
    if (mode == 0) { __imp__sub_82683030(ctx, base); return; }
    const uint64_t result = nl::FloatInteger(ctx.f1.f64);
    if (mode == 2 && nl::Verify()) {
        __imp__sub_82683030(ctx, base);
        nl::Report("82683030", ctx.r3.u64 != result);
        return;
    }
    ctx.r3.u64 = result;
}

PPC_FUNC(sub_8249FCB0) {
    namespace nl = native_leaf2;
    using native_geom::u32;
    const int mode = nl::Mode();
    if (mode == 0) { __imp__sub_8249FCB0(ctx, base); return; }
    const u32 b0 = ctx.r3.u32, idx = ctx.r4.u32, p = ctx.r5.u32, a = ctx.r6.u32, c = ctx.r7.u32;
    if (mode == 2 && nl::Verify()) {
        const nl::Span sp[2] = {{b0 + ((idx + 11u) << 4), 16}, {b0 + (idx << 4) + 16u, 8}};
        nl::SandboxVerify(base, sp, [&] { nl::BuildPlane(base, b0, idx, p, a, c); }, ctx, __imp__sub_8249FCB0, "8249FCB0");
        return;
    }
    nl::BuildPlane(base, b0, idx, p, a, c);
}

PPC_FUNC(sub_82643CA0) {
    namespace nl = native_leaf2;
    using native_geom::u32;
    const int mode = nl::Mode();
    if (mode == 0) { __imp__sub_82643CA0(ctx, base); return; }
    const u32 out = ctx.r3.u32, m = ctx.r4.u32, lo = ctx.r5.u32, hi = ctx.r6.u32;
    if (mode == 2 && nl::Verify()) {
        const nl::Span sp[1] = {{out, 24}};
        nl::SandboxVerify(base, sp, [&] { nl::MatrixRange(base, out, m, lo, hi); }, ctx, __imp__sub_82643CA0, "82643CA0");
        return;
    }
    nl::MatrixRange(base, out, m, lo, hi);
}

PPC_FUNC(sub_826442F8) {
    namespace nl = native_leaf2;
    using native_geom::u32;
    const int mode = nl::Mode();
    if (mode == 0) { __imp__sub_826442F8(ctx, base); return; }
    const u32 olo = ctx.r3.u32, ohi = ctx.r4.u32, m = ctx.r5.u32, org = ctx.r6.u32, a = ctx.r7.u32, c = ctx.r8.u32;
    if (mode == 2 && nl::Verify()) {
        const nl::Span sp[2] = {{olo, 12}, {ohi, 12}};
        nl::SandboxVerify(base, sp, [&] { nl::TransformedRange(base, olo, ohi, m, org, a, c); }, ctx, __imp__sub_826442F8, "826442F8");
        return;
    }
    nl::TransformedRange(base, olo, ohi, m, org, a, c);
}

PPC_FUNC(sub_826444F8) {
    namespace nl = native_leaf2;
    using native_geom::u32;
    const int mode = nl::Mode();
    if (mode == 0) { __imp__sub_826444F8(ctx, base); return; }
    const u32 o1 = ctx.r3.u32, o2 = ctx.r4.u32, v = ctx.r5.u32;
    if (mode == 2 && nl::Verify()) {
        const nl::Span sp[2] = {{o1, 4}, {o2, 4}};
        nl::SandboxVerify(base, sp, [&] { nl::OctantsStore(base, o1, o2, v); }, ctx, __imp__sub_826444F8, "826444F8");
        return;
    }
    nl::OctantsStore(base, o1, o2, v);
}
