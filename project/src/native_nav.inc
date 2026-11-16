// NATIVE NAV QUERY (2026-10-26, AGENT_NATIVE20B). Fused host translation of the main thread's nav-mesh flood fill
// 824A1318(r3 out node, r4 start node, r5 point) with its whole call tree: triangle distance 824A1220 (inside test
// 824AC0E0 = bbox 82172AA8 + three edge sides 824AB880 (+820C1CC8), else min of three 8262A5D8 edge distances),
// open-list push 824B62A8 / pop 824B63A0 (heap 824B66A0 / 824B64D0 via native_geom::SiftDown) on the global
// search state at 0x83D098B8. ~14 % of the main thread's busy samples at the air bench (main thread is fully critical).
// Every float operation is written in the recompiled form (double(float(...)) per fmuls/fadds/fsubs, fsel = >= 0.0,
// fcmpu branches: blt = lt, bge = !lt, bgt = gt, ble = !gt). Only stack scratch (below r1) is not reproduced.
// 824A1220 and 824AC0E0 are also replaced at their other call sites (pure functions).
// DEFAULT ON; dist\native_nav_off = original code; dist\native_nav_verify = 1 query in 32 runs native, the guest state it wrote is captured and
// rolled back, the original runs and everything is compared (search state block, nodes, out, r3); native_nav_off wins.
#include <array>
#include <vector>
extern "C" void __imp__sub_824A1318(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_824A1220(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_824AC0E0(PPCContext& ctx, uint8_t* base);
namespace native_nav {
using namespace native_geom;
int Mode() {
    static const int m = [] {
        int r = 1;  // default ON (exact; offline harness: 18000 queries + 144000 distance/inside calls, 0 differ)
        if (FILE* f = std::fopen("native_nav_off", "rb")) { std::fclose(f); r = 0; }
        else if (FILE* f2 = std::fopen("native_nav_verify", "rb")) { std::fclose(f2); r = 2; }
        REXLOG_INFO("Native nav query (824A1318 824A1220 824AC0E0): {}", r == 0 ? "off" : r == 1 ? "on" : "VERIFY");
        return r;
    }();
    return m;
}
std::atomic<uint64_t> v_calls{0}, v_bad{0};
inline void Report(const char* what, bool bad) {
    const uint64_t c = ++v_calls;
    if (bad && v_bad.fetch_add(1) < 16) REXLOG_INFO("NATIVE NAV differs: {}", what);
    if ((c & 1023) == 0) REXLOG_INFO("NATIVE NAV VERIFY: {} compared, {} differ", c, v_bad.load());
}
constexpr u32 kZero = 0x820875ECu;   // f31 constant (lfs 30188(0x82080000))
constexpr u32 kBig = 0x827AC214u;    // lfs -15852(0x827B0000)
inline void WF(u8* b, u32 a, double d) { W(b, a, Bits(d)); }
inline void W8(u8* b, u32 a, u32 v) { *G(b, a) = u8(v); }

// 824AB880(tri, i, p2 = {x, z}): 1 / 2 = side of edge i, 0 = on it (or NaN).
inline u32 Side(u8* b, u32 tri, u32 i, double px, double pz) {
    u32 v0 = U(b, tri + i * 4u), v1 = U(b, tri + ((i + 1u) % 3u) * 4u);
    bool flag = false;
    if (v0 > v1) { const u32 t = v0; v0 = v1; v1 = t; flag = true; }  // cmplw ble
    const double ex = R(F(b, v0) - F(b, v1));          // f2 -> [92]
    const double ez = R(F(b, v1 + 8) - F(b, v0 + 8));  // f1 -> [88]
    const double qx = R(px - F(b, v0));                // f1 -> [104]
    const double qz = R(pz - F(b, v0 + 8));            // f2 -> [108]
    const double a = R(ez * qx);
    const double c = R(ex * qz);
    const double dot = R(a + c);
    const double z = F(b, kZero);
    if (flag) {
        if (dot > z) return 1;
        if (dot < z) return 2;
        return 0;
    }
    if (dot > z) return 2;
    if (dot < z) return 1;
    return 0;
}
// 824AC0E0(tri, p): 1 = inside (x/z).
inline u32 Inside(u8* b, u32 tri, u32 p) {
    const u32 v0 = U(b, tri), v1 = U(b, tri + 4), v2 = U(b, tri + 8);
    // 82172AA8: min (r3) / max (r4) over x (+0) and z (+8)
    double mnx = F(b, v0), mnz = F(b, v0 + 8);
    { const double v = F(b, v1); mnx = R(mnx - v) >= 0.0 ? v : mnx; }
    { const double v = F(b, v1 + 8); mnz = R(mnz - v) >= 0.0 ? v : mnz; }
    { const double v = F(b, v2); mnx = R(mnx - v) >= 0.0 ? v : mnx; }
    { const double v = F(b, v2 + 8); mnz = R(mnz - v) >= 0.0 ? v : mnz; }
    double mxx = F(b, v0), mxz = F(b, v0 + 8);
    { const double v = F(b, v1); mxx = R(mxx - v) >= 0.0 ? mxx : v; }
    { const double v = F(b, v1 + 8); mxz = R(mxz - v) >= 0.0 ? mxz : v; }
    { const double v = F(b, v2); mxx = R(mxx - v) >= 0.0 ? mxx : v; }
    { const double v = F(b, v2 + 8); mxz = R(mxz - v) >= 0.0 ? mxz : v; }
    const double px = F(b, p), pz = F(b, p + 8);
    if (px < mnx) return 0;
    if (pz < mnz) return 0;
    if (px > mxx) return 0;
    if (pz > mxz) return 0;
    for (u32 i = 0; i < 3; ++i)
        if (Side(b, tri, i, px, pz) == 1) return 0;
    return 1;
}
// 824A1220(tri, p): squared distance, 0 constant when inside.
inline double Dist(u8* b, u32 tri, u32 p) {
    if (Inside(b, tri, p)) return F(b, kZero);
    double f2 = F(b, kBig);
    const double f31 = F(b, kZero);
    for (u32 i = 0; i < 3; ++i) {
        SegResult r;
        Seg(b, p, U(b, tri + i * 4u), U(b, tri + ((i + 1u) % 3u) * 4u), 0, r);
        const double f1 = r.f1;
        if (f1 < f2) f2 = f1;
        else if (f2 < f31) f2 = f1;  // bge = !lt keeps f2
    }
    return f2;
}

struct Journal {  // verify only: node words the query writes (+28..+36), first old value kept
    std::vector<std::pair<u32, std::array<u8, 8>>> nodes;
    void Touch(u8* b, u32 node) {
        for (auto& e : nodes) if (e.first == node) return;
        std::array<u8, 8> v; std::memcpy(v.data(), G(b, node + 28), 8);
        nodes.push_back({node, v});
    }
};
// 824B62A8(S, node, f1, f2, r7 = -1, r8 = 0x83710764)
template <bool J>
inline bool Push(u8* b, u32 node, double f1, double f2, Journal* jr) {
    const u32 S = kHeapS, heap = S + 0x18010u;
    const u32 idx = U(b, S + 8);
    if (int32_t(idx) >= 4096) return false;
    if constexpr (J) jr->Touch(b, node);
    W16(b, node + 30, idx);
    W(b, node + 32, U(b, S + 4));
    W(b, S + 8, idx + 1u);
    const u32 rec = S + 12u + idx * 24u;
    WF(b, rec + 0, f1);
    W8(b, rec + 21, 1);
    WF(b, rec + 4, f2);
    W8(b, rec + 20, 0xFF);
    W(b, rec + 8, U(b, 0x83710764u));
    W(b, rec + 12, U(b, 0x83710768u));
    W(b, rec + 16, U(b, 0x8371076Cu));
    const u32 hc = U(b, S + 0x1800Cu);
    W16(b, rec + 22, hc);
    W(b, heap + (hc << 2), node);
    W(b, S + 0x1800Cu, hc + 1u);
    // 824B66A0(r4 = u16 position): sift up, then 824B64D0
    u32 r4 = H16(b, rec + 22);
    const u32 count = U(b, S + 0x1800Cu);
    if (int32_t(r4) >= int32_t(count)) return true;
    u32 r30 = heap + r4 * 4u;
    const u32 r29 = U(b, r30);
    u32 r31 = u32(int32_t(r4 - 1u) >> 1);
    u32 r5 = int32_t(r31) < int32_t(count) ? heap + r31 * 4u : 0u;
    if (int32_t(r4) > 0) {
        const double knode = HeapKey(b, r29);
        for (;;) {
            const u32 par = U(b, r5);
            const double kpar = HeapKey(b, par);
            if (!(kpar > knode)) break;
            W(b, r30, par);
            const u32 prec = HeapRec(b, par);
            const u32 old = r4;
            r4 = r31;
            r31 = u32(int32_t(r31 - 1u) >> 1);
            r30 = r5;
            W16(b, prec + 22u, old);
            const u32 c = U(b, S + 0x1800Cu);
            r5 = int32_t(r31) < int32_t(c) ? heap + r31 * 4u : 0u;
            if (!(int32_t(r4) > 0)) break;
        }
    }
    W(b, r30, r29);
    W16(b, HeapRec(b, r29) + 22u, r4);
    u32 d3 = 0;
    SiftDown(b, d3, r4);
    return true;
}
// 824B63A0(S): pop the minimum (0 = empty)
inline u32 Pop(u8* b) {
    const u32 S = kHeapS, heap = S + 0x18010u;
    const u32 n = U(b, S + 0x1800Cu);
    if (!(int32_t(n) > 0)) return 0;
    const u32 top = U(b, heap);
    if (int32_t(n) > 1) {
        const u32 last = U(b, heap + (n - 1u) * 4u);
        W(b, heap, last);
        W16(b, HeapRec(b, last) + 22u, 0);
        W(b, S + 0x1800Cu, U(b, S + 0x1800Cu) - 1u);
        u32 r3 = 0, r4 = 0;
        SiftDown(b, r3, r4);
    } else {
        W(b, S + 0x1800Cu, 0);
    }
    W8(b, HeapRec(b, top) + 21u, 0);
    return top;
}
// 824A1318 body. Returns r3.
template <bool J>
inline u32 Query(u8* b, u32 out, u32 start, u32 p, Journal* jr) {
    const u32 S = kHeapS;
    const double d0 = Dist(b, start, p);
    const double f13 = D64(b, 0x82089B20u);
    double f30 = F(b, kBig);
    W(b, out, 0);
    u32 tag = U(b, S + 4) + 1u;
    W(b, S + 4, tag);
    W(b, S + 8, 0);
    W(b, S + 0x1800Cu, 0);
    const double f0 = R(d0 * F(b, 0x8203815Cu));
    const double f12 = R(f0 - F(b, 0x820875F0u));
    const double f29 = R(f12 >= 0.0 ? f0 : f13);
    if (tag == 0) W(b, S + 4, 1);
    const double f31 = F(b, kZero);
    W8(b, S + 0, 1);
    u32 r24 = 0;
    if (Push<J>(b, start, f31, d0, jr)) {
        u32 node = Pop(b);
        while (node) {
            const double d = Dist(b, node, p);
            bool improve = d < f30;
            if (!improve && f30 < f31) improve = true;  // bge cr6 (f30 vs f31) = !lt goes to the neighbours
            if (improve) {
                f30 = d;
                W(b, out, node);
                if (!(d > f31)) { r24 = 1; break; }
            }
            bool full = false;
            for (u32 k = 0; k < 3; ++k) {
                const u32 n = U(b, node + 12u + k * 4u);
                if (n == 0) continue;
                if (U(b, n + 32) == U(b, S + 4)) continue;
                const double dn = Dist(b, n, p);
                if (dn > f29) continue;
                if (!Push<J>(b, n, f31, dn, jr)) { full = true; break; }
            }
            if (full) break;
            node = Pop(b);
        }
    }
    W8(b, S + 0, 0);
    return r24;
}
}  // namespace native_nav

PPC_FUNC(sub_824A1318) {
    namespace nn = native_nav;
    using native_geom::u32; using native_geom::u8;
    const int mode = nn::Mode();
    if (mode == 0) { __imp__sub_824A1318(ctx, base); return; }
    ctx.fpscr.disableFlushMode();
    const u32 out = ctx.r3.u32, start = ctx.r4.u32, p = ctx.r5.u32;
    if (mode == 2) {
        static thread_local uint32_t tick = 0;
        if ((++tick & 31) == 0) {
            constexpr u32 kLen = 0x18010u + 4096u * 4u;
            static thread_local std::vector<u8> before, after;
            before.resize(kLen); after.resize(kLen);
            std::memcpy(before.data(), native_geom::G(base, native_geom::kHeapS), kLen);
            const u32 out_old = native_geom::U(base, out);
            nn::Journal jr;
            const u32 r_nat = nn::Query<true>(base, out, start, p, &jr);
            const u32 out_nat = native_geom::U(base, out);
            std::memcpy(after.data(), native_geom::G(base, native_geom::kHeapS), kLen);
            std::vector<std::array<u8, 8>> nodes_nat;
            for (auto& e : jr.nodes) { std::array<u8, 8> v; std::memcpy(v.data(), native_geom::G(base, e.first + 28), 8); nodes_nat.push_back(v); }
            // roll back, run the original
            for (size_t i = jr.nodes.size(); i-- > 0;) std::memcpy(native_geom::G(base, jr.nodes[i].first + 28), jr.nodes[i].second.data(), 8);
            std::memcpy(native_geom::G(base, native_geom::kHeapS), before.data(), kLen);
            native_geom::W(base, out, out_old);
            __imp__sub_824A1318(ctx, base);
            bool bad = ctx.r3.u32 != r_nat || native_geom::U(base, out) != out_nat;
            if (std::memcmp(after.data(), native_geom::G(base, native_geom::kHeapS), kLen) != 0) bad = true;
            for (size_t i = 0; i < jr.nodes.size(); ++i)
                if (std::memcmp(nodes_nat[i].data(), native_geom::G(base, jr.nodes[i].first + 28), 8) != 0) bad = true;
            nn::Report("824A1318", bad);
            return;
        }
    }
    ctx.r3.u64 = nn::Query<false>(base, out, start, p, nullptr);
}

PPC_FUNC(sub_824A1220) {
    namespace nn = native_nav;
    using native_geom::u32; using native_geom::u8;
    const int mode = nn::Mode();
    if (mode == 0) { __imp__sub_824A1220(ctx, base); return; }
    ctx.fpscr.disableFlushMode();
    const double d = nn::Dist(base, ctx.r3.u32, ctx.r4.u32);
    if (mode == 2) {
        static thread_local uint32_t tick = 0;
        if ((++tick & 31) == 0) {
            __imp__sub_824A1220(ctx, base);
            PPCRegister t; t.f64 = d;
            nn::Report("824A1220", ctx.f1.u64 != t.u64);
            return;
        }
    }
    ctx.f1.f64 = d;
}

PPC_FUNC(sub_824AC0E0) {
    namespace nn = native_nav;
    using native_geom::u32; using native_geom::u8;
    const int mode = nn::Mode();
    if (mode == 0) { __imp__sub_824AC0E0(ctx, base); return; }
    ctx.fpscr.disableFlushMode();
    const u32 r = nn::Inside(base, ctx.r3.u32, ctx.r4.u32);
    if (mode == 2) {
        static thread_local uint32_t tick = 0;
        if ((++tick & 31) == 0) {
            __imp__sub_824AC0E0(ctx, base);
            nn::Report("824AC0E0", ctx.r3.u32 != r);
            return;
        }
    }
    ctx.r3.u64 = r;
}
