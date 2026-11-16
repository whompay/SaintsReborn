// NATIVE LEAVES, batch 3 (AGENT_NATIVE23B2 2026-10-27). Candidates from codexMT4_profile_leaf_batch (fresh c21p profile);
// re-transcribed from the generated bodies (the Codex version copied the whole PPCContext twice per call = slower than the
// original). Each native leaves the PPCContext BIT-IDENTICAL to the generated body (every volatile register, cr0/cr6, msr,
// reserved, lr) and writes the same stack-frame words, so no call-site (LTCG) assumption is needed at all.
//   82637080  counter++ at +16, then atomic ++ at +4 (lwarx/stwcx. loop)            [main 0-1.0 %, indirect calls only]
//   82636980  state CAS at +48: state 2 -> 3 + signal (rare: runs the original); else r3 = 0  [main 0.5-1.6 %, render 0.1-0.6 %]
// The generated bodies wrap the lwarx/stwcx. pair in four seq_cst fences (mfmsr / mtmsrd); on x86 the lock cmpxchg is itself a
// full barrier, so the native keeps only the CAS. REX_GUEST_GLOBAL_LOCK=1 (old shared guest lock) -> natives disabled.
// dist\native_leaf3 = on; native_leaf3_verify = 1 call in 16 runs the original and checks the native register model against
// it; native_leaf3_off wins. Default OFF.
// NOT ported (see native23b_handoff.md): 825C4030 / 825C4770 (render bitstream decoders, ~250 PPC instr each, call 82118D68 +
// hooked 825C29E8/825C2F38 per element), 825C2FA8 (40-byte record append: native saves only the call frame), 8271F010
// (recursive interpreter), 8251AA00 (8-callee traversal).
#include <cstdlib>
extern "C" void __imp__sub_82637080(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_82636980(PPCContext& ctx, uint8_t* base);

// ppc_global_lock_depth_() (guest "interrupts off" depth; mfmsr reads 0 when > 0) comes from the generated saintsrow_pch.h,
// which shadow_probe.cpp sees: the very function the generated mfmsr/mtmsrd code uses.

namespace native_leaf3 {
using namespace native_geom;
int Mode() {
    static const int m = [] {
        int r = 0;
        const char* gl = std::getenv("REX_GUEST_GLOBAL_LOCK");
        if (gl && gl[0] == '1') r = 0;
        else if (FILE* f = std::fopen("native_leaf3_off", "rb")) { std::fclose(f); r = 0; }
        else if (FILE* f2 = std::fopen("native_leaf3_verify", "rb")) { std::fclose(f2); r = 2; }
        else if (FILE* f3 = std::fopen("native_leaf3", "rb")) { std::fclose(f3); r = 1; }
        REXLOG_INFO("Native leaves 3 (82637080 82636980): {}", r == 0 ? "off" : r == 1 ? "on" : "VERIFY");
        return r;
    }();
    return m;
}
std::atomic<uint64_t> v_calls{0}, v_bad{0};
void Report(const char* what, bool bad) {
    const uint64_t n = v_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (bad) {
        const uint64_t b = v_bad.fetch_add(1, std::memory_order_relaxed) + 1;
        if (b <= 8) REXLOG_INFO("NATIVE LEAF3 differs: {}", what);
    }
    if ((n & 0x3FFFu) == 0)
        REXLOG_INFO("NATIVE LEAF3 VERIFY: {} compared, {} differ", n, v_bad.load(std::memory_order_relaxed));
}
inline bool Verify() {
    static thread_local uint32_t tick = 0;
    return (++tick & 15) == 0;
}
inline u32* Raw(u8* b, u32 a) { return reinterpret_cast<u32*>(G(b, a)); }
inline void W64(u8* b, u32 a, u64 v) { v = __builtin_bswap64(v); std::memcpy(G(b, a), &v, 8); }
inline u64 U64(u8* b, u32 a) { u64 v; std::memcpy(&v, G(b, a), 8); return __builtin_bswap64(v); }
inline u64 MsrCheck() { return ppc_global_lock_depth_() ? 0u : 0x8000u; }
inline u32 MsrAfter(u32 msr0, u64 saved) { return (u32(saved) & 0x8020u) | (msr0 & ~0x8020u); }

// 82637080: lwz r11,16(r3); addi r10,r3,4; addi r11,r11,1; stw r11,16(r3); loop { mfmsr r8; lwarx r9,0,r10; addi r9,r9,1;
// stwcx. r9,0,r10 } until success. Registers left: r8 r9 r10 r11 cr0 msr reserved.
inline void IncrementPair(PPCContext& ctx, u8* base) {
    const u64 r11 = u64(U(base, ctx.r3.u32 + 16)) + 1;
    W(base, ctx.r3.u32 + 16, u32(r11));
    const u64 r10 = ctx.r3.u64 + 4;
    u32* const p = Raw(base, u32(r10));
    const u64 r8 = MsrCheck();
    u32 raw; u64 r9; bool ok;
    do {
        raw = __atomic_load_n(p, __ATOMIC_RELAXED);
        r9 = u64(__builtin_bswap32(raw)) + 1;
        ok = __sync_bool_compare_and_swap(p, raw, __builtin_bswap32(u32(r9)));
    } while (!ok);
    ctx.r8.u64 = r8; ctx.r9.u64 = r9; ctx.r10.u64 = r10; ctx.r11.u64 = r11;
    ctx.reserved.u32 = raw;
    ctx.cr0.lt = 0; ctx.cr0.gt = 0; ctx.cr0.eq = 1; ctx.cr0.so = ctx.xer.so;
    ctx.msr = MsrAfter(ctx.msr, r8);
}

// 82636980 fast path (state != 2): savegprlr_28 frame + stwu -128, one stwcx. of the unchanged value (no retry), r3 = 0.
// Returns false (nothing written) when the state reads 2: the caller then runs the generated body (CAS 2->3, critical
// section, 82716D20/82716CE0) - an exact interleaving, since the generated code re-reads the word itself.
inline bool SignalFast(PPCContext& ctx, u8* base) {
    const u32 r31 = ctx.r3.u32;
    const u64 r30 = ctx.r3.u64 + 48;
    u32* const p = Raw(base, u32(r30));
    const u32 raw = __atomic_load_n(p, __ATOMIC_RELAXED);
    const u32 obs = __builtin_bswap32(raw);
    if (obs == 2) return false;
    (void)r31;
    const u32 sp = ctx.r1.u32;
    const u64 r12 = ctx.lr;
    // __savegprlr_28 then stwu r1,-128(r1) (same store order as the generated code)
    W64(base, sp - 40, ctx.r28.u64); W64(base, sp - 32, ctx.r29.u64);
    W64(base, sp - 24, ctx.r30.u64); W64(base, sp - 16, ctx.r31.u64);
    W(base, sp - 8, u32(r12));
    W(base, sp - 128, sp);
    const u64 r9 = MsrCheck();
    const bool ok = __sync_bool_compare_and_swap(p, raw, raw);
    ctx.reserved.u32 = raw;
    ctx.r3.u64 = 0;
    ctx.r9.u64 = r9;
    ctx.r10.u64 = 2;
    ctx.r11.u64 = obs;
    ctx.cr0.lt = 0; ctx.cr0.gt = 0; ctx.cr0.eq = ok ? 1 : 0; ctx.cr0.so = ctx.xer.so;
    ctx.cr6.compare<int32_t>(int32_t(obs), 2, ctx.xer);
    ctx.msr = MsrAfter(ctx.msr, r9);
    // __restgprlr_28 (reloads from the frame, exactly like the generated epilogue)
    ctx.r28.u64 = U64(base, sp - 40); ctx.r29.u64 = U64(base, sp - 32);
    ctx.r30.u64 = U64(base, sp - 24); ctx.r31.u64 = U64(base, sp - 16);
    ctx.r12.u64 = U(base, sp - 8);
    ctx.lr = ctx.r12.u64;
    return true;
}
}  // namespace native_leaf3

PPC_FUNC(sub_82637080) {
    using native_geom::u32; using native_geom::u64;
    namespace nl = native_leaf3;
    const int mode = nl::Mode();
    if (mode == 0) { __imp__sub_82637080(ctx, base); return; }
    if (mode == 2 && nl::Verify()) {
        // register model check: predict from the entry state, run the original, compare (memory is shared/atomic: not re-run)
        const u64 r3 = ctx.r3.u64; const u32 msr0 = ctx.msr;
        // (+16 is a plain, racy counter in the original too -> r11 is not predicted here; the offline harness checks it)
        __imp__sub_82637080(ctx, base);
        const bool bad = ctx.r11.u64 == 0 || ctx.r10.u64 != r3 + 4 || ctx.r8.u64 != nl::MsrCheck() ||
                         ctx.r9.u64 != u64(__builtin_bswap32(ctx.reserved.u32)) + 1 || !ctx.cr0.eq || ctx.cr0.lt || ctx.cr0.gt ||
                         ctx.msr != nl::MsrAfter(msr0, nl::MsrCheck());
        nl::Report("82637080", bad);
        return;
    }
    nl::IncrementPair(ctx, base);
}

PPC_FUNC(sub_82636980) {
    using native_geom::u32; using native_geom::u64;
    namespace nl = native_leaf3;
    const int mode = nl::Mode();
    if (mode == 0) { __imp__sub_82636980(ctx, base); return; }
    if (mode == 2 && nl::Verify()) {
        const u64 r3 = ctx.r3.u64; const u64 lr = ctx.lr; const u32 msr0 = ctx.msr;
        const u64 r28 = ctx.r28.u64, r29 = ctx.r29.u64, r30 = ctx.r30.u64, r31 = ctx.r31.u64, r1 = ctx.r1.u64;
        __imp__sub_82636980(ctx, base);
        const u32 obs = u32(ctx.r11.u64);  // fast path only (the slow path returns r3 = 1 and leaves r11 = 0)
        bool bad = ctx.r1.u64 != r1 || ctx.r28.u64 != r28 || ctx.r29.u64 != r29 || ctx.r30.u64 != r30 || ctx.r31.u64 != r31 ||
                   ctx.lr != u64(u32(lr)) || ctx.r12.u64 != u64(u32(lr)) || ctx.r3.u64 > 1;
        if (ctx.r3.u64 == 0)
            bad = bad || ctx.r3.u64 != 0 || ctx.r10.u64 != 2 || ctx.r9.u64 != nl::MsrCheck() || ctx.r11.u64 != u64(__builtin_bswap32(ctx.reserved.u32)) ||
                  ctx.msr != nl::MsrAfter(msr0, nl::MsrCheck()) || ctx.cr6.eq || ctx.cr6.lt != (int32_t(obs) < 2);
        (void)r3;
        nl::Report("82636980", bad);
        return;
    }
    if (!nl::SignalFast(ctx, base)) __imp__sub_82636980(ctx, base);
}
