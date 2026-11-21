// NATIVE MAIN-THREAD LEAVES (2026-10-27, whompay). Hand translations of five hot leaf functions of the main thread
// (no calls into other game code), with the exact float semantics of the recompiled code: every fmuls/fadds/fsubs/fsqrts
// rounded to float in the same order, fmadds = double(float(std::fma(a, b, c))) exactly as the generated code does it.
// Only the return registers are written back (the other volatile registers are dead for the callers); the dead guest
// stack scratch below r1 is not written.
//  - 821287B0 camera distance: f1 = sqrt(|P - Q|^2) * k (r3 P, r4 Q). Byte 0x8370D726 set -> generated code.
//  - 8263A140 spline point: r3 spline (+0 points, +4 / +8 tangent arrays, +12 count, 12-byte vec3s), f1 t, r4 out vec3.
//  - 82637C90 plane-tree point test: r3 tree (+8 planes, +12 nodes of 3 u16), r4 point -> r3 0/1.
//  - 8262E958 3x3 matrix product: r3 out, r4 A, r5 B (all inputs read before the first store, so out may alias).
//  - 82715AC0 memset (r3 dst, r4 byte, r5 count), host memset when the range is one host mapping.
// dist\native_leaf = on; dist\native_leaf_verify = native result into locals first, then the original runs for real
// and the outputs are compared (1 call in 16); dist\native_leaf_off wins. Default OFF until A/B'd.
extern "C" void __imp__sub_821287B0(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_8263A140(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_82637C90(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_8262E958(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_82715AC0(PPCContext& ctx, uint8_t* base);
namespace native_leaf {
using namespace native_geom;
int Mode() {
    static const int m = [] {
        int r = 0;
        if (FILE* f = std::fopen("native_leaf_off", "rb")) { std::fclose(f); r = 0; }
        else if (FILE* f2 = std::fopen("native_leaf_verify", "rb")) { std::fclose(f2); r = 2; }
        else if (FILE* f3 = std::fopen("native_leaf", "rb")) { std::fclose(f3); r = 1; }
        REXLOG_INFO("Native main leaves (821287B0 8263A140 82637C90 8262E958 82715AC0): {}", r == 0 ? "off" : r == 1 ? "on" : "VERIFY");
        return r;
    }();
    return m;
}
std::atomic<uint64_t> v_calls{0}, v_bad{0};
void Report(const char* what, bool bad) {
    const uint64_t n = v_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (bad) {
        const uint64_t b = v_bad.fetch_add(1, std::memory_order_relaxed) + 1;
        if (b <= 8) REXLOG_INFO("NATIVE LEAF differs: {}", what);
    }
    if ((n & 0x3FFFu) == 0)
        REXLOG_INFO("NATIVE LEAF VERIFY: {} compared, {} differ", n, v_bad.load(std::memory_order_relaxed));
}
inline bool Verify() {
    static thread_local uint32_t tick = 0;
    return (++tick & 15) == 0;
}
inline u32 H16(u8* b, u32 a) { uint16_t v; std::memcpy(&v, G(b, a), 2); return __builtin_bswap16(v); }
inline u32 FBits(double d) { const float f = float(d); u32 v; std::memcpy(&v, &f, 4); return v; }

// 821287B0
inline double Dist(u8* b, u32 p, u32 q) {
    const double x = F(b, p), y = F(b, p + 4), z = F(b, p + 8);
    double f0 = R(y - F(b, q + 4));
    double f13 = R(z - F(b, q + 8));
    const double f11 = R(f0 * f0);
    f0 = R(x - F(b, q + 0));
    f13 = R(std::fma(f13, f13, f11));
    f0 = R(std::fma(f0, f0, f13));
    f0 = R(std::sqrt(f0));
    return R(f0 * F(b, 0x82089A60u));
}

// 8263A140: writes 3 words to out[]
inline void Spline(u8* b, u32 s, double t, u32 (&out)[3]) {
    if (!(t > F(b, 0x820875ECu))) {
        const u32 p = U(b, s);
        out[0] = U(b, p); out[1] = U(b, p + 4); out[2] = U(b, p + 8);
        return;
    }
    const u32 n = U(b, s + 12);
    const double last = R(double(int64_t(int32_t(n - 1u))));
    if (!(t < last)) {
        const u32 p = U(b, s) + n * 12u - 12u;
        out[0] = U(b, p); out[1] = U(b, p + 4); out[2] = U(b, p + 8);
        return;
    }
    const int32_t i = int32_t(t);  // t is a number in (c0, n - 1): fctiwz = truncation
    const double fi = R(double(int64_t(i)));
    const u32 pts = U(b, s), tb = U(b, s + 4), ta = U(b, s + 8);
    const u32 o0 = u32(i) * 12u, o1 = (u32(i) + 1u) * 12u;
    const double k = F(b, 0x827D93B0u);
    const u32 p0 = pts + o0, p1 = pts + o1, a0 = ta + o0, t1 = tb + o1;
    const double f4 = F(b, a0), f3 = F(b, a0 + 4), f2 = F(b, a0 + 8);
    const double f7 = F(b, p0), f6 = F(b, p0 + 4), f5 = F(b, p0 + 8);
    const double f9i = F(b, p1), f8i = F(b, p1 + 4), f1i = F(b, p1 + 8);
    const double f31 = F(b, t1), f30 = F(b, t1 + 4), f29 = F(b, t1 + 8);
    const double s1 = R(t - fi);
    const double s2 = R(s1 * s1);
    const double s3 = R(s2 * s1);
    double f10 = R(s2 - s1);
    const double f28 = R(s3 - s2);
    const double f27 = R(std::fma(f10, k, -s3));
    f10 = R(f9i * s3);
    double f9 = R(f8i * s3);
    double f8 = R(f1i * s3);
    const double f26 = R(s2 - s3);
    double f1 = R(f28 - s2);
    double f0 = R(f27 + F(b, 0x820875F8u));
    double f12 = R(f26 * k);
    double f13 = R(f1 + s1);
    f13 = R(f13 * k);
    double f11 = R(f7 * f0);
    double g7 = R(f6 * f0);
    f0 = R(f5 * f0);
    const double g6 = R(f3 * f13);
    f11 = R(f11 + f10);
    f10 = R(g7 + f9);
    g7 = R(f4 * f13);
    f0 = R(f0 + f8);
    f13 = R(f2 * f13);
    f9 = R(f31 * f12);
    f8 = R(f30 * f12);
    f12 = R(f29 * f12);
    f10 = R(f10 + g6);
    f11 = R(f11 + g7);
    f0 = R(f0 + f13);
    out[0] = FBits(R(f11 + f9));
    out[1] = FBits(R(f10 + f8));
    out[2] = FBits(R(f0 + f12));
}

// 82637C90
inline u32 TreeTest(u8* b, u32 tree, u32 pt) {
    const u32 nodes = U(b, tree + 12), planes = U(b, tree + 8);
    const double pz = F(b, pt + 8), py = F(b, pt + 4), px = F(b, pt + 0);
    const double thr = F(b, 0x8208952Cu);
    u32 node = nodes;
    for (;;) {
        const u32 pl = planes + (u32(H16(b, node)) << 4);
        double d = R(F(b, pl + 8) * pz);
        d = R(std::fma(F(b, pl + 4), py, d));
        d = R(std::fma(F(b, pl + 0), px, d));
        d = R(F(b, pl + 12) + d);
        if (d < thr) {
            const u32 c = H16(b, node + 2);
            if (c == 0) return 0;
            node = nodes + c * 6u;
        } else {
            const u32 c = H16(b, node + 4);
            if (c == 0) return 1;
            node = nodes + c * 6u;
        }
    }
}

// 8262E958: out = 9 words
inline void Mat3(u8* b, u32 a4, u32 a5, u32 (&o)[9]) {
    double f0 = F(b, a4 + 28);
    double f13 = F(b, a5 + 12);
    double f10 = F(b, a5 + 16);
    double f27 = R(f13 * f0);
    double f7 = F(b, a5 + 20);
    double f26 = R(f10 * f0);
    double f5 = F(b, a4 + 16);
    f0 = R(f7 * f0);
    double f2 = F(b, a4 + 4);
    double f25 = R(f5 * f13);
    double f24 = R(f5 * f10);
    double f1 = F(b, a4 + 0);
    double f6 = F(b, a5 + 8);
    f5 = R(f5 * f7);
    f13 = R(f2 * f13);
    double f11 = F(b, a5 + 0);
    f10 = R(f2 * f10);
    double f9 = F(b, a5 + 4);
    const double f23 = R(f1 * f6);
    double f12 = F(b, a4 + 24);
    double f4 = F(b, a4 + 12);
    f27 = R(std::fma(f11, f12, f27));
    double f8 = F(b, a5 + 32);
    f26 = R(std::fma(f9, f12, f26));
    const double f3 = F(b, a4 + 8);
    f12 = R(std::fma(f6, f12, f0));
    const double f30 = F(b, a4 + 32);
    f25 = R(std::fma(f4, f11, f25));
    const double f28 = F(b, a4 + 20);
    f24 = R(std::fma(f4, f9, f24));
    const double f31 = F(b, a5 + 24);
    f6 = R(std::fma(f4, f6, f5));
    const double f29 = F(b, a5 + 28);
    f5 = R(std::fma(f1, f11, f13));
    f4 = R(std::fma(f1, f9, f10));
    f1 = R(std::fma(f3, f8, f23));
    f0 = R(std::fma(f30, f31, f27));
    f13 = R(std::fma(f29, f30, f26));
    f12 = R(std::fma(f8, f30, f12));
    f11 = R(std::fma(f28, f31, f25));
    f10 = R(std::fma(f28, f29, f24));
    f9 = R(std::fma(f28, f8, f6));
    f8 = R(std::fma(f3, f31, f5));
    o[0] = FBits(f8);
    f6 = R(std::fma(f3, f29, f4));
    o[1] = FBits(f6);
    f7 = R(std::fma(f2, f7, f1));
    o[2] = FBits(f7);
    o[3] = FBits(f11); o[4] = FBits(f10); o[5] = FBits(f9);
    o[6] = FBits(f0);  o[7] = FBits(f13); o[8] = FBits(f12);
}
}  // namespace native_leaf

PPC_FUNC(sub_821287B0) {
    namespace nl = native_leaf;
    const int mode = nl::Mode();
    if (mode == 0 || *native_geom::G(base, 0x8370D726u) != 0) { __imp__sub_821287B0(ctx, base); return; }
    ctx.fpscr.disableFlushMode();
    const double d = nl::Dist(base, ctx.r3.u32, ctx.r4.u32);
    if (mode == 2 && nl::Verify()) {
        __imp__sub_821287B0(ctx, base);
        PPCRegister t; t.f64 = d;
        nl::Report("821287B0", ctx.f1.u64 != t.u64);
        return;
    }
    ctx.f1.f64 = d;
}

PPC_FUNC(sub_8263A140) {
    namespace nl = native_leaf;
    using native_geom::u32;
    const int mode = nl::Mode();
    if (mode == 0) { __imp__sub_8263A140(ctx, base); return; }
    ctx.fpscr.disableFlushMode();
    const u32 out = ctx.r4.u32;
    u32 o[3];
    nl::Spline(base, ctx.r3.u32, ctx.f1.f64, o);
    if (mode == 2 && nl::Verify()) {
        __imp__sub_8263A140(ctx, base);
        nl::Report("8263A140", native_geom::U(base, out) != o[0] || native_geom::U(base, out + 4) != o[1] ||
                                   native_geom::U(base, out + 8) != o[2]);
        return;
    }
    for (int i = 0; i < 3; ++i) native_geom::W(base, out + 4 * i, o[i]);
}

PPC_FUNC(sub_82637C90) {
    namespace nl = native_leaf;
    using native_geom::u32;
    const int mode = nl::Mode();
    if (mode == 0) { __imp__sub_82637C90(ctx, base); return; }
    ctx.fpscr.disableFlushMode();
    const u32 r = nl::TreeTest(base, ctx.r3.u32, ctx.r4.u32);
    if (mode == 2 && nl::Verify()) {
        __imp__sub_82637C90(ctx, base);
        nl::Report("82637C90", ctx.r3.u64 != r);
        return;
    }
    ctx.r3.u64 = r;
}

PPC_FUNC(sub_8262E958) {
    namespace nl = native_leaf;
    using native_geom::u32;
    const int mode = nl::Mode();
    if (mode == 0) { __imp__sub_8262E958(ctx, base); return; }
    ctx.fpscr.disableFlushMode();
    const u32 out = ctx.r3.u32;
    u32 o[9];
    nl::Mat3(base, ctx.r4.u32, ctx.r5.u32, o);
    if (mode == 2 && nl::Verify()) {
        __imp__sub_8262E958(ctx, base);
        bool bad = false;
        for (int i = 0; i < 9; ++i) bad |= native_geom::U(base, out + 4 * i) != o[i];
        nl::Report("8262E958", bad);
        return;
    }
    for (int i = 0; i < 9; ++i) native_geom::W(base, out + 4 * i, o[i]);
}

PPC_FUNC(sub_82715AC0) {
    namespace nl = native_leaf;
    using native_geom::u32;
    const int mode = nl::Mode();
    const u32 dst = ctx.r3.u32, n = ctx.r5.u32;
    // one host mapping for the whole range (and no wrap) -> host memset; anything else: the game's code
    if (mode == 0 || n == 0 || dst + (n - 1u) < dst ||
        native_geom::G(base, dst + (n - 1u)) != native_geom::G(base, dst) + (n - 1u)) {
        __imp__sub_82715AC0(ctx, base);
        return;
    }
    const uint8_t v = uint8_t(ctx.r4.u32);
    if (mode == 2 && nl::Verify()) {
        __imp__sub_82715AC0(ctx, base);
        const uint8_t* p = native_geom::G(base, dst);
        bool bad = ctx.r3.u32 != dst;
        for (u32 i = 0; i < n && !bad; ++i) bad = p[i] != v;
        nl::Report("82715AC0", bad);
        return;
    }
    std::memset(native_geom::G(base, dst), v, n);
}
