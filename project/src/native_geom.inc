// NATIVE GEOMETRY LEAVES (2026-10-23, perf round 5b). Hand translations of three leaf functions, exact
// float semantics of the recompiled code (every fmuls/fadds/fsubs/fdivs rounded to float; fmadds = exact product + add,
// rounded once, which equals std::fma for float operands).
//  - 8262A5D8 point-to-segment squared distance (r3 P, r4 A, r5 B, r6 optional out closest point; f1 result;
//    second path also writes the closest point to the guest stack r1-16..r1-8). Main thread (nav-mesh queries via
//    824A1220/824A1318/...), ~13 % of the main thread's busy samples at the bench spot.
//  - 824A00D0 / 824A01E8 AABB (r3 min, r4 max) vs plane list (r5: +0 count, +16+16i / +20+16i corner selector 0..7,
//    +176+16i plane a b c d): r3 = 0 as soon as one plane has !(a*x + c*z + b*y + d < [0x820875EC]), else 1.
// NOTE PPC branch semantics: bge = !lt, ble = !gt (unordered/NaN takes bge/ble), bgt = gt, blt = lt.
//    Visibility jobs on the workers (~3 % of all busy samples, all cores).
// dist\native_geom = on; dist\native_geom_verify = native computed into locals first (no guest writes), then the
// original runs for real and results + touched memory are compared (1 call in 16); dist\native_geom_off wins.
extern "C" void __imp__sub_8262A5D8(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_824A00D0(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_824A01E8(PPCContext& ctx, uint8_t* base);
namespace native_geom {
using u8 = uint8_t; using u32 = uint32_t; using u64 = uint64_t;
inline u8* G(u8* b, u32 a) { return b + a + REX_PHYS_HOST_OFFSET(a); }
inline double F(u8* b, u32 a) { u32 v; std::memcpy(&v, G(b, a), 4); v = __builtin_bswap32(v); float f; std::memcpy(&f, &v, 4); return double(f); }
inline double D64(u8* b, u32 a) { u64 v; std::memcpy(&v, G(b, a), 8); v = __builtin_bswap64(v); double d; std::memcpy(&d, &v, 8); return d; }
inline u32 U(u8* b, u32 a) { u32 v; std::memcpy(&v, G(b, a), 4); return __builtin_bswap32(v); }
inline void W(u8* b, u32 a, u32 v) { v = __builtin_bswap32(v); std::memcpy(G(b, a), &v, 4); }
inline u32 Bits(double d) { const float f = float(d); u32 v; std::memcpy(&v, &f, 4); return v; }
inline double R(double x) { return double(float(x)); }
int Mode() {
    static const int m = [] {
        int r = 1;  // default ON (verify 0 differ; c19h A/B 4/4 pairs, +3.7 %)
        if (FILE* f = std::fopen("native_geom_off", "rb")) { std::fclose(f); r = 0; }
        else if (FILE* f2 = std::fopen("native_geom_verify", "rb")) { std::fclose(f2); r = 2; }
        else if (FILE* f3 = std::fopen("native_geom", "rb")) { std::fclose(f3); r = 1; }
        REXLOG_INFO("Native geometry leaves (8262A5D8 824A00D0 824A01E8): {}", r == 0 ? "off" : r == 1 ? "on" : "VERIFY");
        return r;
    }();
    return m;
}
std::atomic<uint64_t> v_calls{0}, v_bad{0};
inline void Report(const char* what, bool bad) {
    const uint64_t c = ++v_calls;
    if (bad && v_bad.fetch_add(1) < 16) REXLOG_INFO("NATIVE GEOM differs: {}", what);
    if ((c & 65535) == 0) REXLOG_INFO("NATIVE GEOM VERIFY: {} compared, {} differ", c, v_bad.load());
}

// The heap and sort translations below were added after the round-5 handoff's
// 160k-case claim, which covered only Seg and BoxPlanes. Keep them on a
// separate experimental switch so enabling the verified geometry leaves does
// not silently opt into a wider, differently validated change.
int ExtraMode() {
    static const int m = [] {
        int r = 0;
        if (FILE* f = std::fopen("native_geom_extra_off", "rb")) { std::fclose(f); r = 0; }
        else if (FILE* f2 = std::fopen("native_geom_extra_verify", "rb")) { std::fclose(f2); r = 2; }
        else if (FILE* f3 = std::fopen("native_geom_extra", "rb")) { std::fclose(f3); r = 1; }
        REXLOG_INFO("Native geometry extras (heap/sort): {}",
                    r == 0 ? "off" : r == 1 ? "on" : "VERIFY");
        return r;
    }();
    return m;
}

// ---- 8262A5D8 ----
struct SegResult {
    double f1;
    bool far_path;    // second path (|AB|^2 >= eps): writes the stack words
    u32 c[3];         // closest point (float bits, host order): out (r6) and/or stack
};
// Computes without writing guest memory (callers refuse out overlapping P or A, so the original's re-reads after
// its out store see the same values).
inline void Seg(u8* b, u32 p, u32 a, u32 bb, u32 out, SegResult& r) {
    const double ax = F(b, a);
    const double dx = R(F(b, bb) - ax);
    const double az = F(b, a + 8);
    const double dz = R(F(b, bb + 8) - az);
    const double px = R(F(b, p) - ax);
    const double ay = F(b, a + 4);
    const double py = R(F(b, p + 4) - ay);
    const double dy = R(F(b, bb + 4) - ay);
    const double pz = R(F(b, p + 8) - az);
    const double eps = F(b, 0x82089660u);
    double l2 = R(dx * dx);
    l2 = R(dz * dz + l2);
    l2 = R(dy * dy + l2);
    if (l2 < eps) {  // bge = branch if NOT less (NaN takes the far path)
        r.far_path = false;
        if (out) { r.c[0] = U(b, a); r.c[1] = U(b, a + 4); r.c[2] = U(b, a + 8); }
        const double ex = R(F(b, p) - F(b, a));
        double t = R(ex * ex);
        const double ey = R(F(b, p + 4) - F(b, a + 4));
        const double ez = R(F(b, p + 8) - F(b, a + 8));
        t = R(ey * ey + t);
        r.f1 = R(ez * ez + t);
        return;
    }
    r.far_path = true;
    double s = R(dz * pz);
    s = R(dx * px + s);
    s = R(dy * py + s);
    const double q = R(s / l2);
    const double c0 = D64(b, 0x82089238u), c1 = D64(b, 0x82089248u);
    const double t7 = R(q >= 0.0 ? q : c0);
    const double t0 = R(q >= 0.0 ? q : c0);
    const double one = F(b, 0x820875F8u);
    const double m = R(t7 - one);
    const double t = R(m >= 0.0 ? c1 : t0);
    const double cx = R(ax + R(dx * t));
    const double cy = R(R(dy * t) + F(b, a + 4));
    const double cz = R(F(b, a + 8) + R(dz * t));
    r.c[0] = Bits(cx); r.c[1] = Bits(cy); r.c[2] = Bits(cz);
    const double ey = R(cy - F(b, p + 4));
    const double ez = R(cz - F(b, p + 8));
    const double ex = R(cx - F(b, p));
    double u = R(ey * ey);
    u = R(ez * ez + u);
    r.f1 = R(ex * ex + u);
}
inline void SegApply(PPCContext& ctx, u8* b) {
    const u32 p = ctx.r3.u32, a = ctx.r4.u32, bb = ctx.r5.u32, out = ctx.r6.u32, r1 = ctx.r1.u32;
    SegResult r;
    // Aliasing-safe order: compute with reads before the writes only when out does not overlap P/A.
    const bool alias = out && ((out < p + 12 && p < out + 12) || (out < a + 12 && a < out + 12));
    if (alias) { __imp__sub_8262A5D8(ctx, b); return; }
    Seg(b, p, a, bb, out, r);
    if (r.far_path) {
        W(b, r1 - 16, r.c[0]); W(b, r1 - 12, r.c[1]); W(b, r1 - 8, r.c[2]);
    }
    if (out) { W(b, out, r.c[0]); W(b, out + 4, r.c[1]); W(b, out + 8, r.c[2]); }
    ctx.f1.f64 = r.f1;
}

// ---- 824A00D0 / 824A01E8 ----
template <u32 SelOff>
inline u32 BoxPlanes(u8* b, u32 mn, u32 mx, u32 pl) {
    const int32_t n = int32_t(U(b, pl));
    if (n <= 0) return 1;
    const double lim = F(b, 0x820875ECu);
    for (int32_t i = 0; i < n; ++i) {
        const u32 e = pl + 16u * u32(i);
        const u32 sel = U(b, e + SelOff);
        if (sel > 7) continue;
        // selector -> corner: z from max for 4..7; x from max for 0,1,4,5; y from max for 0,3,4,7
        const u32 xs = (sel & 2) ? mn : mx;
        const u32 zs = (sel & 4) ? mx : mn;
        const u32 ys = (((sel >> 1) ^ sel) & 1) ? mn : mx;
        const double x = F(b, xs), z = F(b, zs + 8), y = F(b, ys + 4);
        const double pa = F(b, e + 176), pb = F(b, e + 180), pc = F(b, e + 184), pd = F(b, e + 188);
        double t = R(pa * x);
        t = R(pc * z + t);
        t = R(pb * y + t);
        t = R(t + pd);
        if (!(t < lim)) return 0;  // bge = !lt (NaN returns 0 like the original)
    }
    return 1;
}
}  // namespace native_geom

PPC_FUNC(sub_8262A5D8) {
    using namespace native_geom;
    const int mode = Mode();
    if (mode == 0) { __imp__sub_8262A5D8(ctx, base); return; }
    ctx.fpscr.disableFlushMode();
    if (mode == 2) {
        static thread_local uint32_t tick = 0;
        if ((++tick & 15) == 0) {
            const u32 out = ctx.r6.u32, r1 = ctx.r1.u32, p = ctx.r3.u32, a = ctx.r4.u32;
            if (out && ((out < p + 12 && p < out + 12) || (out < a + 12 && a < out + 12))) { __imp__sub_8262A5D8(ctx, base); return; }
            SegResult r;
            Seg(base, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, out, r);
            __imp__sub_8262A5D8(ctx, base);
            bool bad = ctx.f1.u64 != [&] { PPCRegister t; t.f64 = r.f1; return t.u64; }();
            if (r.far_path) bad |= U(base, r1 - 16) != r.c[0] || U(base, r1 - 12) != r.c[1] || U(base, r1 - 8) != r.c[2];
            if (out) bad |= U(base, out) != r.c[0] || U(base, out + 4) != r.c[1] || U(base, out + 8) != r.c[2];
            Report("8262A5D8", bad);
            return;
        }
    }
    SegApply(ctx, base);
}

PPC_FUNC(sub_824A00D0) {
    using namespace native_geom;
    const int mode = Mode();
    if (mode == 0) { __imp__sub_824A00D0(ctx, base); return; }
    ctx.fpscr.disableFlushMode();
    const u32 r = BoxPlanes<16>(base, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
    if (mode == 2) {
        static thread_local uint32_t tick = 0;
        if ((++tick & 15) == 0) { __imp__sub_824A00D0(ctx, base); Report("824A00D0", ctx.r3.u32 != r); return; }
    }
    ctx.r3.u64 = r;
}

PPC_FUNC(sub_824A01E8) {
    using namespace native_geom;
    const int mode = Mode();
    if (mode == 0) { __imp__sub_824A01E8(ctx, base); return; }
    ctx.fpscr.disableFlushMode();
    const u32 r = BoxPlanes<20>(base, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
    if (mode == 2) {
        static thread_local uint32_t tick = 0;
        if ((++tick & 15) == 0) { __imp__sub_824A01E8(ctx, base); Report("824A01E8", ctx.r3.u32 != r); return; }
    }
    ctx.r3.u64 = r;
}

// ---- 824B64D0 / 824B66A0: A* open-list binary heap (main thread path finding), global heap at 0x83D098B8:
// +4 tag, +12 records (24 bytes: key = [+4] + [+0], +22 u16 heap position), +0x1800C count, +0x18010 node pointers
// (node +30 u16 record index, +32 tag; position is written only when the node's tag matches [+4], else to guest 22,
// exactly like the original). 824B64D0(r4 = position) sifts down, 824B66A0(r4) sifts up then calls 824B64D0.
// Experimental and separate from native_geom: dist\native_geom_extra enables these plus 82128628 below;
// native_geom_extra_verify verifies the comparator where replay is safe; native_geom_extra_off wins.
extern "C" void __imp__sub_824B64D0(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_824B66A0(PPCContext& ctx, uint8_t* base);
namespace native_geom {
constexpr u32 kHeapS = 0x83D098B8u;
inline void W16(u8* b, u32 a, u32 v) { const uint16_t h = __builtin_bswap16(uint16_t(v)); std::memcpy(G(b, a), &h, 2); }
inline u32 H16(u8* b, u32 a) { uint16_t v; std::memcpy(&v, G(b, a), 2); return __builtin_bswap16(v); }
inline void W64(u8* b, u32 a, u64 v) { v = __builtin_bswap64(v); std::memcpy(G(b, a), &v, 8); }
inline double HeapKey(u8* b, u32 node) {
    const u32 rec = kHeapS + 12u + H16(b, node + 30) * 24u;
    return R(F(b, rec + 4) + F(b, rec));
}
inline u32 HeapRec(u8* b, u32 node) {  // record for the position store, 0 when the tag differs
    return U(b, node + 32) == U(b, kHeapS + 4) ? kHeapS + 12u + H16(b, node + 30) * 24u : 0u;
}
// Body of 824B64D0 after its prologue. Returns the final r3 / r4.
inline void SiftDown(u8* b, u32& r3, u32& r4) {
    const u32 S = kHeapS, heap = S + 0x18010u;
    r3 = U(b, S + 0x1800Cu);
    if (int32_t(r4) >= int32_t(r3)) return;
    u32 r31 = heap + r4 * 4u;  // never 0 (original tests it)
    const double f12 = HeapKey(b, U(b, r31));
    u32 r30 = r4, r29 = r31;
    for (;;) {
        double f13 = f12;
        const u32 c1 = r30 * 2u + 1u, c2 = r30 * 2u + 2u;
        if (int32_t(c1) < int32_t(r3)) {
            const u32 a = heap + c1 * 4u;
            const double k = HeapKey(b, U(b, a));
            if (k < f12) { r30 = c1; f13 = k; r29 = a; }  // bge skip = !lt
        }
        if (int32_t(c2) < int32_t(r3)) {
            const u32 a = heap + c2 * 4u;
            const double k = HeapKey(b, U(b, a));
            if (k < f13) { r30 = c2; r29 = a; }
        }
        if (r30 == r4) return;
        const u32 cur = U(b, r31), child = U(b, r29);
        W(b, r29, cur);
        W16(b, HeapRec(b, cur) + 22u, r30);
        W(b, r31, child);
        const u32 old = r4;
        r4 = r30;
        r31 = r29;
        W16(b, HeapRec(b, child) + 22u, old);
        r3 = U(b, S + 0x1800Cu);
    }
}
inline void HeapDownFn(PPCContext& ctx, u8* b) {
    const u32 r1 = ctx.r1.u32;
    // __savegprlr_29
    W64(b, r1 - 32, ctx.r29.u64); W64(b, r1 - 24, ctx.r30.u64); W64(b, r1 - 16, ctx.r31.u64); W(b, r1 - 8, u32(ctx.lr));
    u32 r3 = 0, r4 = ctx.r4.u32;
    SiftDown(b, r3, r4);
    ctx.r3.u64 = r3; ctx.r4.u64 = r4;
}
}  // namespace native_geom

PPC_FUNC(sub_824B64D0) {
    using namespace native_geom;
    const int mode = ExtraMode();
    if (mode == 0) { __imp__sub_824B64D0(ctx, base); return; }
    ctx.fpscr.disableFlushMode();
    HeapDownFn(ctx, base);  // verify: covered by the offline harness (writes guest memory, no cheap replay)
}

PPC_FUNC(sub_824B66A0) {
    using namespace native_geom;
    const int mode = ExtraMode();
    if (mode == 0) { __imp__sub_824B66A0(ctx, base); return; }
    ctx.fpscr.disableFlushMode();
    const u32 S = kHeapS, heap = S + 0x18010u;
    const u32 r1 = ctx.r1.u32;
    const u64 e29 = ctx.r29.u64, e30 = ctx.r30.u64, e31 = ctx.r31.u64, elr = ctx.lr;
    W64(base, r1 - 32, e29); W64(base, r1 - 24, e30); W64(base, r1 - 16, e31); W(base, r1 - 8, u32(elr));
    W(base, r1 - 112, r1);  // stwu back chain
    ctx.r3.s64 = int32_t(S);  // lis/addi: sign-extended like the original
    u32 r4 = ctx.r4.u32;
    const u32 count = U(base, S + 0x1800Cu);
    if (int32_t(r4) >= int32_t(count)) { ctx.r9.u64 = count; return; }
    u32 r30 = heap + r4 * 4u;
    const u32 r29 = U(base, r30);
    u32 r31 = u32(int32_t(r4 - 1u) >> 1);
    u32 r5 = int32_t(r31) < int32_t(count) ? heap + r31 * 4u : 0u;
    if (int32_t(r4) > 0) {
        for (;;) {
            const double knode = HeapKey(base, r29);
            const u32 par = U(base, r5);
            const double kpar = HeapKey(base, par);
            if (!(kpar > knode)) break;  // ble = !gt (NaN exits)
            W(base, r30, par);
            const u32 rec = HeapRec(base, par);
            const u32 old = r4;
            r4 = r31;
            r31 = u32(int32_t(r31 - 1u) >> 1);
            r30 = r5;
            W16(base, rec + 22u, old);
            const u32 c = U(base, S + 0x1800Cu);
            r5 = int32_t(r31) < int32_t(c) ? heap + r31 * 4u : 0u;
            if (!(int32_t(r4) > 0)) break;
        }
    }
    W(base, r30, r29);
    W16(base, HeapRec(base, r29) + 22u, r4);
    // bl 824B64D0 with r1 = r1 - 112, lr = return address, r29..r31 as the original has them
    const bool tag_ok = U(base, r29 + 32) == U(base, S + 4);
    ctx.r1.u64 = r1 - 112;
    // 64-bit register images as the original has them (the callee saves them to the stack): r29 from lwz (zero
    // extended), r30 an address built from lis (sign extended), r31 from srawi (sign extended).
    ctx.r29.u64 = r29; ctx.r30.s64 = int32_t(r30); ctx.r31.s64 = int32_t(r31);
    ctx.r4.u64 = r4; ctx.r3.s64 = int32_t(S);
    ctx.lr = tag_ok ? 0x824B6820u : 0x824B6838u;
    sub_824B64D0(ctx, base);
    ctx.r1.u64 = r1;
    ctx.r29.u64 = e29; ctx.r30.u64 = e30; ctx.r31.u64 = e31; ctx.lr = elr;
}

// ---- 82128628: render-list sort comparator (render thread, called through a function pointer by the list sort;
// ~2 % of the render thread's samples on 4 cores). r3/r4 objects: byte +12, word +8 (bit 0x80, bits 11..12),
// float at [[obj] + 96]. Returns r3 = 1 when a sorts before b.
extern "C" void __imp__sub_82128628(PPCContext& ctx, uint8_t* base);
namespace native_geom {
inline u32 B8(u8* b, u32 a) { return *G(b, a); }
inline u32 SortLess(PPCContext& ctx, u8* b, u32 a, u32 c) {
    const u32 fa = B8(b, a + 12), fc = B8(b, c + 12);
    if (fa == 0) { if (fc != 0) return 0; }
    else if (fc == 0) return 1;
    const u32 wa = U(b, a + 8), wc = U(b, c + 8);
    if ((wa & 0x80u) == 0) { if (wc & 0x80u) return 1; }
    else if ((wc & 0x80u) == 0) return 0;
    const int32_t ka = int32_t((wa >> 11) & 3u), kc = int32_t((wc >> 11) & 3u);
    if (ka < kc) return 1;
    if (ka > kc) return 0;
    ctx.fpscr.disableFlushMode();  // only on this path, like the original
    return F(b, U(b, a) + 96) < F(b, U(b, c) + 96) ? 1u : 0u;  // blt: NaN -> 0
}
}  // namespace native_geom

PPC_FUNC(sub_82128628) {
    using namespace native_geom;
    const int mode = ExtraMode();
    if (mode == 0) { __imp__sub_82128628(ctx, base); return; }
    const u32 r = SortLess(ctx, base, ctx.r3.u32, ctx.r4.u32);
    if (mode == 2) {
        static thread_local uint32_t tick = 0;
        if ((++tick & 15) == 0) { __imp__sub_82128628(ctx, base); Report("82128628", ctx.r3.u64 != r); return; }
    }
    ctx.r3.u64 = r;
}
