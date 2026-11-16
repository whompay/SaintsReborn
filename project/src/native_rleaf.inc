// NATIVE RENDER-CHAIN LEAVES (AGENT_NATIVE22B, 2026-10-27). Exact host versions of hot generated functions of the guest
// render thread / colour worker (draw-state flush under 825D2478 and its siblings).
//  - 825DB228 register-run packet writer: r3 device, r4 64-bit dirty mask (bit 63 = first register), r5 first register,
//    r6 shadow array (u32 per register). For every run of set bits (MSB first) it writes the type-0 header
//    ((run - 1) << 16) | reg at ++wp, then the run's shadow dwords, wp = device+40 (pre-increment cursor), and stores the
//    final cursor to device+40. When a run does not fit (wp + run*4 >= device+44) the original grows the buffer through
//    825DAF48 -> then the whole call runs the generated code. Mask 0 -> generated code. All callers overwrite the volatile
//    registers after the call (checked at all 64 call sites), so only memory is written: packet words, device+40 and the
//    savegprlr_26 / stwu -144 stack frame exactly as the original leaves it.
//  - 825C6018 bitstream read, 8251C1E8 hash-slot probe, 826446F8 push point into box (+ f1 = squared span sum),
//    82629CD0 in-place vec3 x mat3 (codexRT5b candidates, re-transcribed in the original load/store order; verified offline).
//    Only the return registers are written (all call sites checked: no caller reads another volatile register).
// dist\native_rleaf = on; dist\native_rleaf_verify = native result computed into a local buffer, then the original runs
// for real and the written words + cursor are compared (1 call in 16); dist\native_rleaf_off wins. Default OFF until A/B'd.
extern "C" void __imp__sub_825DB228(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_825C6018(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_8251C1E8(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_826446F8(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_82629CD0(PPCContext& ctx, uint8_t* base);
namespace native_rleaf {
using namespace native_geom;
int Mode() {
    static const int m = [] {
        int r = 0;
        if (FILE* f = std::fopen("native_rleaf_off", "rb")) { std::fclose(f); r = 0; }
        else if (FILE* f2 = std::fopen("native_rleaf_verify", "rb")) { std::fclose(f2); r = 2; }
        else if (FILE* f3 = std::fopen("native_rleaf", "rb")) { std::fclose(f3); r = 1; }
        REXLOG_INFO("Native render leaves (825DB228 825C6018 8251C1E8 826446F8 82629CD0): {}", r == 0 ? "off" : r == 1 ? "on" : "VERIFY");
        return r;
    }();
    return m;
}
std::atomic<uint64_t> v_calls{0}, v_bad{0}, v_grow{0};
void Report(const char* what, bool bad) {
    const uint64_t n = v_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (bad) {
        const uint64_t b = v_bad.fetch_add(1, std::memory_order_relaxed) + 1;
        if (b <= 8) REXLOG_INFO("NATIVE RLEAF differs: {}", what);
    }
    if ((n & 0x3FFFu) == 0)
        REXLOG_INFO("NATIVE RLEAF VERIFY: {} compared, {} differ, {} grew (original)", n, v_bad.load(std::memory_order_relaxed),
                    v_grow.load(std::memory_order_relaxed));
}
inline bool Verify() {
    static thread_local uint32_t tick = 0;
    return (++tick & 15) == 0;
}
inline void RW64(u8* b, u32 a, u64 v) { v = __builtin_bswap64(v); std::memcpy(G(b, a), &v, 8); }

// 825DB228 fit test: the original compares wp + run*4 < limit (unsigned 32-bit) before every run.
inline bool RegRunsFit(u64 mask, u32 wp, u32 limit) {
    while (mask) {
        const int lz = __builtin_clzll(mask);
        mask <<= lz;
        const u64 inv = ~mask;
        const u32 run = inv ? u32(__builtin_clzll(inv)) : 64u;
        if (!(u32(wp + run * 4u) < limit)) return false;
        wp += 4u + run * 4u;
        mask = run >= 64 ? 0 : mask << run;
    }
    return true;
}
// Emits the packets. out == nullptr -> writes guest memory at ++wp; else writes the dwords to out[] (verify).
// Returns the final cursor.
inline u32 RegRuns(u8* b, u64 mask, u32 wp, u32 reg0, u32 src, u32* out) {
    u32 pos = 0, k = 0;
    while (mask) {
        const int lz = __builtin_clzll(mask);
        pos += u32(lz);
        mask <<= lz;
        const u64 inv = ~mask;
        const u32 run = inv ? u32(__builtin_clzll(inv)) : 64u;
        const u32 hdr = ((run - 1u) << 16) | (reg0 + pos);
        wp += 4;
        if (out) out[k++] = hdr; else W(b, wp, hdr);
        u32 s = src + pos * 4u;
        for (u32 i = 0; i < run; ++i) {
            const u32 v = U(b, s);
            s += 4;
            wp += 4;
            if (out) out[k++] = v; else W(b, wp, v);
        }
        pos += run;
        mask = run >= 64 ? 0 : mask << run;
    }
    return wp;
}
inline void RegRunsFrame(PPCContext& ctx, u8* b) {
    const u32 r1 = ctx.r1.u32;
    RW64(b, r1 - 56, ctx.r26.u64); RW64(b, r1 - 48, ctx.r27.u64); RW64(b, r1 - 40, ctx.r28.u64);
    RW64(b, r1 - 32, ctx.r29.u64); RW64(b, r1 - 24, ctx.r30.u64); RW64(b, r1 - 16, ctx.r31.u64);
    W(b, r1 - 8, u32(ctx.lr));
    W(b, r1 - 144, r1);
}
// ---- batch 2 (codexRT5b candidates, re-transcribed by AGENT_NATIVE22B; same load/store order as the generated code) ----
inline double FD(u8* b, u32 a) { u32 v = U(b, a); float f; std::memcpy(&f, &v, 4); return double(f); }
inline double RR(double x) { return double(float(x)); }
inline u32 FB(double d) { const float f = float(d); u32 v; std::memcpy(&v, &f, 4); return v; }
inline double D64(u8* b, u32 a) { u64 v; std::memcpy(&v, G(b, a), 8); v = __builtin_bswap64(v); double d; std::memcpy(&d, &v, 8); return d; }
inline double NegD(double d) { u64 v; std::memcpy(&v, &d, 8); v ^= 0x8000000000000000ull; std::memcpy(&d, &v, 8); return d; }

// 825C6018: little-endian bitstream read. r3 state (+0 source: +4 byte pointer; +4 bit cursor), r4 bit count -> r3 value.
// The cursor is stored after every byte, before the next byte is loaded (like the original).
inline u64 ReadBits(u8* b, u32 st, u64 r4) {
    u64 r9 = 0, r3 = 0;
    if (!(int32_t(u32(r4)) > 0)) return 0;
    const u32 src = U(b, st);
    for (;;) {
        u64 r10 = U(b, st + 4);
        const u64 r8 = r10 & 7u;
        const u32 r6 = u32(r10) >> 3;
        u64 r11 = 8u - r8;
        if (!(int32_t(u32(r11)) < int32_t(u32(r4)))) r11 = r4;
        const u32 bytes = U(b, src + 4);
        r10 = r10 + r11;
        r4 = r4 - r11;
        const u32 byte = *G(b, bytes + r6);
        W(b, st + 4, u32(r10));
        const u64 m = (u8(r11) & 0x20u) ? 0 : u64(u32(0xFFFFFFFFu << (u8(r11) & 0x3Fu)));
        const u64 piece = (u8(r8) & 0x20u) ? 0 : u64(byte >> (u8(r8) & 0x3Fu));
        u64 v = piece & ~m;
        v = (u8(r9) & 0x20u) ? 0 : u64(u32(u32(v) << (u8(r9) & 0x3Fu)));
        r9 = r11 + r9;
        r3 = v | r3;
        if (!(int32_t(u32(r4)) > 0)) break;
    }
    return r3;
}

// 8251C1E8: open-addressing slot probe. r3 table (+4 capacity, +8 slot bytes, +16 max probes), r4 out (slot address or 0),
// r5 key pointer, r6 records (+4 array of 16-byte records, key first) -> r3 1/0. Capacity 0 -> start index = key (as the original).
inline u64 FindSlot(u8* b, u32 table, u32 out, u32 keyp, u32 recs) {
    const u32 key = U(b, keyp);
    const u32 cap = U(b, table + 4);
    const u32 probes = U(b, table + 16);
    const u32 q = cap ? key / cap : 0u;
    u32 idx = key - u32(uint64_t(int64_t(int32_t(q)) * int64_t(int32_t(cap))));
    if (probes != 0) {
        const u32 slots = U(b, table + 8);
        u32 n = 0;
        for (;;) {
            const u32 a = slots + idx;
            const u32 s = *G(b, a);
            if (s != 255u) {
                const u32 ra = U(b, recs + 4);
                if (U(b, (s << 4) + ra) == key) { W(b, out, a); return 1; }
            }
            idx += 1;
            if (idx == cap) idx = 0;
            n += 1;
            if (!(n < probes)) break;
        }
    }
    W(b, out, 0);
    return 0;
}

// 826446F8: copy point r6 -> out r3, push it into the box [r5 low, r4 high] per axis, return squared span sum in f1.
inline double ClampAxis(double p, double lo, double hi, double z, double& span) {
    const double f13 = RR(p - lo);
    const double f12 = RR(hi - p);
    const double neg = NegD(f13);
    double f7 = f12 >= 0.0 ? f12 : z;
    double f12b = f12 >= 0.0 ? f12 : z;
    double f8 = f13 >= 0.0 ? neg : z;
    double f13b = f13 >= 0.0 ? f13 : z;
    f7 = RR(f7); f12b = RR(f12b); f8 = RR(f8); f13b = RR(f13b);
    f7 = RR(f7 + f8);
    span = RR(f12b + f13b);
    return f7;
}
inline double PushIntoBox(u8* b, u32 out, u32 hi, u32 lo, u32 pt) {
    const u32 w0 = U(b, pt + 0);
    const double z = D64(b, 0x82089238u);
    W(b, out + 0, w0);
    const u32 w1 = U(b, pt + 4);
    const double px = FD(b, out + 0);
    W(b, out + 4, w1);
    const double py = FD(b, out + 4);
    const u32 w2 = U(b, pt + 8);
    W(b, out + 8, w2);
    double sx, sy, sz;
    // x
    const double lx = FD(b, lo + 0), hx = FD(b, hi + 0);
    const double pz = FD(b, out + 8);
    const double mx = ClampAxis(px, lx, hx, z, sx);
    W(b, out + 0, FB(RR(mx + px)));
    // y
    const double ly = FD(b, lo + 4), hy = FD(b, hi + 4);
    const double my = ClampAxis(py, ly, hy, z, sy);
    W(b, out + 4, FB(RR(my + py)));
    // z
    const double lz = FD(b, lo + 8);
    const double sy2 = RR(sy * sy);
    const double hz = FD(b, hi + 8);
    {
        const double f13 = RR(pz - lz);
        const double f12 = RR(hz - pz);
        const double neg = NegD(f13);
        double f7 = f12 >= 0.0 ? f12 : z;
        double f10 = f13 >= 0.0 ? neg : z;
        double f13b = f13 >= 0.0 ? f13 : z;
        double f0 = f12 >= 0.0 ? f12 : z;
        f7 = RR(f7); f10 = RR(f10); f13b = RR(f13b); f0 = RR(f0);
        f10 = RR(f7 + f10);
        sz = RR(f0 + f13b);
        W(b, out + 8, FB(RR(f10 + pz)));
    }
    const double f0 = RR(std::fma(sz, sz, sy2));
    return RR(std::fma(sx, sx, f0));
}

// 82629CD0: in-place vec3 (r3) times 3x3 matrix (r4, rows of 3 floats); matrix entries are loaded between the stores.
inline void MulVecMat(u8* b, u32 v, u32 m) {
    double f11 = FD(b, m + 12);
    const double y = FD(b, v + 4);
    f11 = RR(f11 * y);
    double f10 = FD(b, m + 24);
    const double z = FD(b, v + 8);
    double f9 = FD(b, m + 0);
    const double x = FD(b, v + 0);
    f11 = RR(std::fma(f10, z, f11));
    f11 = RR(std::fma(f9, x, f11));
    W(b, v + 0, FB(f11));
    f11 = FD(b, m + 16);
    f11 = RR(f11 * y);
    f10 = FD(b, m + 28);
    f9 = FD(b, m + 4);
    f11 = RR(std::fma(f10, z, f11));
    f11 = RR(std::fma(f9, x, f11));
    W(b, v + 4, FB(f11));
    f11 = FD(b, m + 20);
    double f13 = RR(f11 * y);
    f11 = FD(b, m + 32);
    f10 = FD(b, m + 8);
    f13 = RR(std::fma(f11, z, f13));
    const double f0 = RR(std::fma(f10, x, f13));
    W(b, v + 8, FB(f0));
}

struct Span { u32 a, n; };
// Runs the native on the real memory, keeps its bytes, restores, runs the original for real; true = memory differs.
template <int N, class Fn>
inline bool SandboxMem(u8* b, const Span (&sp)[N], Fn&& native, PPCContext& ctx, void (*orig)(PPCContext&, uint8_t*)) {
    uint8_t save[N][16], mine[N][16];
    for (int k = 0; k < N; ++k) std::memcpy(save[k], G(b, sp[k].a), sp[k].n);
    native();
    for (int k = 0; k < N; ++k) std::memcpy(mine[k], G(b, sp[k].a), sp[k].n);
    for (int k = N - 1; k >= 0; --k) std::memcpy(G(b, sp[k].a), save[k], sp[k].n);
    orig(ctx, b);
    bool bad = false;
    for (int k = 0; k < N; ++k) bad |= std::memcmp(mine[k], G(b, sp[k].a), sp[k].n) != 0;
    return bad;
}
}  // namespace native_rleaf

PPC_FUNC(sub_825DB228) {
    namespace nr = native_rleaf;
    using native_geom::u32; using native_geom::u64;
    const int mode = nr::Mode();
    const u32 dev = ctx.r3.u32;
    const u64 mask = ctx.r4.u64;
    if (mode == 0 || mask == 0) { __imp__sub_825DB228(ctx, base); return; }
    const u32 wp0 = native_geom::U(base, dev + 40), limit = native_geom::U(base, dev + 44);
    if (!nr::RegRunsFit(mask, wp0, limit)) {
        if (mode == 2) nr::v_grow.fetch_add(1, std::memory_order_relaxed);
        __imp__sub_825DB228(ctx, base);
        return;
    }
    const u32 reg0 = ctx.r5.u32, src = ctx.r6.u32;
    if (mode == 2 && nr::Verify()) {
        u32 tmp[64 * 2 + 4];
        const u32 wpn = nr::RegRuns(base, mask, wp0, reg0, src, tmp);
        __imp__sub_825DB228(ctx, base);
        bool bad = native_geom::U(base, dev + 40) != wpn;
        for (u32 a = wp0 + 4, k = 0; !bad && a <= wpn; a += 4, ++k) bad = native_geom::U(base, a) != tmp[k];
        nr::Report("825DB228", bad);
        return;
    }
    nr::RegRunsFrame(ctx, base);
    const u32 wpn = nr::RegRuns(base, mask, wp0, reg0, src, nullptr);
    native_geom::W(base, dev + 40, wpn);
}

PPC_FUNC(sub_825C6018) {
    namespace nr = native_rleaf;
    using native_geom::u32; using native_geom::u64;
    const int mode = nr::Mode();
    if (mode == 0) { __imp__sub_825C6018(ctx, base); return; }
    const u32 st = ctx.r3.u32;
    const u64 cnt = ctx.r4.u64;
    if (mode == 2 && nr::Verify()) {
        u64 mine = 0;
        const nr::Span sp[1] = {{st + 4, 4}};
        const bool bad = nr::SandboxMem(base, sp, [&] { mine = nr::ReadBits(base, st, cnt); }, ctx, __imp__sub_825C6018);
        nr::Report("825C6018", bad || ctx.r3.u64 != mine);
        return;
    }
    ctx.r3.u64 = nr::ReadBits(base, st, cnt);
}

PPC_FUNC(sub_8251C1E8) {
    namespace nr = native_rleaf;
    using native_geom::u32; using native_geom::u64;
    const int mode = nr::Mode();
    if (mode == 0) { __imp__sub_8251C1E8(ctx, base); return; }
    const u32 t = ctx.r3.u32, out = ctx.r4.u32, kp = ctx.r5.u32, rc = ctx.r6.u32;
    if (mode == 2 && nr::Verify()) {
        u64 mine = 0;
        const nr::Span sp[1] = {{out, 4}};
        const bool bad = nr::SandboxMem(base, sp, [&] { mine = nr::FindSlot(base, t, out, kp, rc); }, ctx, __imp__sub_8251C1E8);
        nr::Report("8251C1E8", bad || ctx.r3.u64 != mine);
        return;
    }
    ctx.r3.u64 = nr::FindSlot(base, t, out, kp, rc);
}

// 826446F8 is ALSO hooked by render_fixes.cpp SR_RT_TIMED(826446F8, 8223E8D8) -> no PPC_FUNC here (was a duplicate symbol in cpu22_x,
// lld kept one silently). The timed hook's plain path calls this; returns false when native_rleaf is off (caller runs the original).
bool sr_rleaf_826446F8(PPCContext& ctx, uint8_t* base) {
    namespace nr = native_rleaf;
    using native_geom::u32;
    const int mode = nr::Mode();
    if (mode == 0) return false;
    ctx.fpscr.disableFlushMode();
    const u32 out = ctx.r3.u32, hi = ctx.r4.u32, lo = ctx.r5.u32, pt = ctx.r6.u32;
    if (mode == 2 && nr::Verify()) {
        double mine = 0;
        const nr::Span sp[1] = {{out, 12}};
        const bool bad = nr::SandboxMem(base, sp, [&] { mine = nr::PushIntoBox(base, out, hi, lo, pt); }, ctx, __imp__sub_826446F8);
        PPCRegister e; e.f64 = mine;
        nr::Report("826446F8", bad || ctx.f1.u64 != e.u64);
        return true;
    }
    ctx.f1.f64 = nr::PushIntoBox(base, out, hi, lo, pt);
    return true;
}

PPC_FUNC(sub_82629CD0) {
    namespace nr = native_rleaf;
    using native_geom::u32;
    const int mode = nr::Mode();
    if (mode == 0) { __imp__sub_82629CD0(ctx, base); return; }
    ctx.fpscr.disableFlushMode();
    const u32 v = ctx.r3.u32, m = ctx.r4.u32;
    if (mode == 2 && nr::Verify()) {
        const nr::Span sp[1] = {{v, 12}};
        nr::Report("82629CD0", nr::SandboxMem(base, sp, [&] { nr::MulVecMat(base, v, m); }, ctx, __imp__sub_82629CD0));
        return;
    }
    nr::MulVecMat(base, v, m);
}
