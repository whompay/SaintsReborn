// 27. NATIVE DRAWINDEXED WRITER (2026-10-26). Host version of the clean path of the game's DrawIndexedVertices
//    (825D2478: r3 device, r4 primitive, r5 base vertex, r6 start index, r7 index count). Taken only when nothing has
//    to be flushed first: all five dirty masks (device +0..+39) zero, direct mode (+10808 bit 1 clear), count <= 65535,
//    an index buffer set (+12180) and the ring cursor (+40) not past its kick threshold (+48). It then writes exactly
//    what the game writes: 7 dwords after the cursor (VGT_INDX_OFFSET, base vertex, DRAW_INDX header, initiator,
//    draw control, index address, index size) and the cursor (+40, address of the last dword). Everything else
//    (dirty state, ring kick, secondary buffer, split draws) stays in the game code; the check happens before any write.
//    Dist files: native_draw = use it; native_draw_verify = compare only (the game's writer stays authoritative, the
//    host packet is built into a local buffer and compared byte for byte); native_draw.off wins. Default OFF.
namespace native_draw {
enum Mode : int { kOff = 0, kOn = 1, kVerify = 2 };
inline int GetMode() {
    static const int m = [] {
        int v = kOff;
        if (!FileExists("native_draw.off")) {
            if (FileExists("native_draw")) v = kOn;
            if (FileExists("native_draw_verify")) v = kVerify;
        }
        if (v) REXLOG_INFO("NATIVE DRAW (825D2478 clean path): {}", v == kVerify ? "verify" : "on");
        return v;
    }();
    return m;
}
enum Reason : int { kOk = 0, kDirty, kIndirect, kLarge, kNoIb, kKick, kReasons };
// Per-thread counters: the render thread and the colour worker both draw, shared atomics would bounce a cache line per draw.
struct Counters { uint64_t calls = 0, counts[kReasons] = {}, compared = 0, differ = 0, dirty_word[5] = {}, dirty_only[5] = {}; };
thread_local Counters tl;
std::atomic<bool> first_diff_logged{false};
// Same guest address mapping as the generated code (REX_LOAD / REX_STORE).
inline uint8_t* G(uint8_t* b, uint32_t a) { return b + a + REX_PHYS_HOST_OFFSET(a); }
inline uint32_t R32(uint8_t* b, uint32_t a) { uint32_t v; std::memcpy(&v, G(b, a), 4); return __builtin_bswap32(v); }
inline void W32(uint8_t* b, uint32_t a, uint32_t v) { v = __builtin_bswap32(v); std::memcpy(G(b, a), &v, 4); }

inline int Build(uint8_t* base, uint32_t dev, uint32_t prim_in, uint32_t base_vertex, uint32_t start, uint32_t count,
                 uint32_t& wp, uint32_t (&w)[7]) {
    const uint64_t* masks = reinterpret_cast<const uint64_t*>(G(base, dev));  // only != 0 matters, byte order irrelevant
    if ((masks[0] | masks[1] | masks[2] | masks[3] | masks[4]) != 0) {
        int n = 0, last = 0;  // NATIVE19B: which of the five dirty words (+0 +8 +16 +24 +32) are set
        for (int i = 0; i < 5; ++i)
            if (masks[i]) { ++tl.dirty_word[i]; ++n; last = i; }
        if (n == 1) ++tl.dirty_only[last];
        return kDirty;
    }
    if (*G(base, dev + 10808) & 2) return kIndirect;
    if (count > 65535u) return kLarge;
    const uint32_t ib = R32(base, dev + 12180);
    if (!ib) return kNoIb;
    wp = R32(base, dev + 40);
    if (wp > R32(base, dev + 48)) return kKick;
    const uint32_t ib_flags = R32(base, ib + 0), ib_base = R32(base, ib + 24);
    const bool i32 = (ib_flags & 0x80000000u) != 0;
    const uint32_t prim = prim_in & 0x3Fu;
    const uint32_t addr = ib_base + start * (i32 ? 4u : 2u);
    const uint32_t gpu = (addr & 0x1FFFFFFFu) + ((((addr >> 20) & 0xFFFu) + 512u) & 0x1000u);
    uint32_t control = (count << 16) | prim;
    if (i32) control |= 0x800u;
    const uint32_t size = (i32 ? ((count << 1) & 0x00FFFFFEu) : (count & 0x00FFFFFFu)) | ((ib_flags << 1) & 0xC0000000u);
    const uint32_t sel = *G(base, dev + 11931);
    const uint32_t initiator = (*G(base, dev + 11866 + sel) & 0x3Fu) | (sel ? 0x100u : 0u);
    w[0] = 0x00002102u; w[1] = base_vertex; w[2] = 0xC0032201u; w[3] = initiator;
    w[4] = control;     w[5] = gpu;         w[6] = size;
    return kOk;
}
inline void MaybeLog() {
    if ((++tl.calls & 0x3FFFFu) != 0) return;
    REXLOG_INFO("NATIVE DRAW (this thread): calls {} native {} | fallback dirty {} indirect {} large {} no-ib {} kick {} | verify compared {} differ {}",
                tl.calls, tl.counts[kOk], tl.counts[kDirty], tl.counts[kIndirect], tl.counts[kLarge], tl.counts[kNoIb],
                tl.counts[kKick], tl.compared, tl.differ);
    REXLOG_INFO("NATIVE DRAW dirty words (this thread): +0 {} +8 {} +16 {} +24 {} +32 {} | only that word: {} {} {} {} {}",
                tl.dirty_word[0], tl.dirty_word[1], tl.dirty_word[2], tl.dirty_word[3], tl.dirty_word[4],
                tl.dirty_only[0], tl.dirty_only[1], tl.dirty_only[2], tl.dirty_only[3], tl.dirty_only[4]);
}
// true = the call was handled (native packet written, or verify ran the game's writer).
inline bool Run(PPCContext& ctx, uint8_t* base) {
    const int mode = GetMode();
    if (mode == kOff) [[likely]] return false;
    MaybeLog();
    const uint32_t dev = ctx.r3.u32;
    uint32_t wp = 0, w[7];
    const int r = Build(base, dev, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, wp, w);
    ++tl.counts[r];
    if (r != kOk) return false;
    if (mode == kOn) {
        for (int i = 0; i < 7; ++i) W32(base, wp + 4 + 4 * i, w[i]);
        W32(base, dev + 40, wp + 28);
        ctx.r3.u64 = wp + 4;
        return true;
    }
    const uint32_t prim = ctx.r4.u32, bv = ctx.r5.u32, start = ctx.r6.u32, count = ctx.r7.u32, lr = uint32_t(ctx.lr);
    __imp__sub_825D2478(ctx, base);
    ++tl.compared;
    uint32_t got[7];
    for (int i = 0; i < 7; ++i) got[i] = R32(base, wp + 4 + 4 * i);
    const uint32_t wp_after = R32(base, dev + 40);
    if (std::memcmp(got, w, sizeof(w)) != 0 || wp_after != wp + 28) {
        ++tl.differ;
        if (!first_diff_logged.exchange(true)) {
            int k = 0;
            while (k < 7 && got[k] == w[k]) ++k;
            REXLOG_INFO("NATIVE DRAW VERIFY first diff: dev {:08X} lr {:08X} prim {} base {} start {} count {} ib {:08X} "
                        "wp {:08X} -> game {:08X} (host {:08X}) dword {} game {:08X} host {:08X}",
                        dev, lr, prim, bv, start, count, R32(base, dev + 12180), wp, wp_after, wp + 28, k,
                        k < 7 ? got[k] : 0u, k < 7 ? w[k] : 0u);
        }
    }
    return true;
}
}  // namespace native_draw
