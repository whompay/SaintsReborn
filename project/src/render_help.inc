// 28. RENDER-THREAD HELP-WHILE-WAIT (AGENT_NATIVE19B, 2026-10-26). The render thread's list prep 821938B8 -> 82190490
//    posts the render-list prep batches (job fn 82190188, class 4 -> queue 0x840A1680) with the dependency group r23 and
//    a final no-op job 826BA8E0 that completes the sync object r17; 821938B8 then does its own work and joins r17 at
//    826375A8 (return 821939F0) -> render thread waits in 826370E8 (82637174) for the workers (+ two wake-up hops:
//    worker for the no-op, then the render thread). Help: at that join, while r17 is not complete, the render thread
//    pops jobs from the class-4 queue itself - ONLY when the queue head is a list-prep batch 82190188 or the no-op
//    826BA8E0 (FIFO order kept, nothing else is taken) - and runs each exactly as the worker loop 82636BB0 does:
//    fn(job, [job+20], [job+24]); event [job+12] -> 82716D20 (+82716CE0 when it returns 0); group [job+16] ->
//    vtable+12; job back to the free ring 0x840A26A8. 82190188 is built for non-worker threads (82637670 finds no
//    worker slot -> local stack allocator, the same path the game's synchronous mode uses on the render thread).
//    Then the normal join runs (returns at once when r17 is complete).
//    Dist files: render_help = on; render_help_stats = only measure the join (no help); render_help.off wins.
//    Log 'RENDER HELP' every 4096 joins. Default OFF.
extern "C" void __imp__sub_826375A8(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_82188E98(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_82716CE0(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_82716D20(PPCContext& ctx, uint8_t* base);
namespace render_help {
constexpr uint32_t kQueue4 = 0x840A1680u, kPool = 0x840A26A8u;  // ring: [1024] entries, +4096 read, +4100 write, +4104 lock (1 = free)
constexpr uint32_t kJobList = 0x82190188u, kJobNop = 0x826BA8E0u, kJoinLr = 0x821939F0u;
enum Mode : int { kOff = 0, kOn = 1, kStats = 2 };
inline int GetMode() {
    static const int m = [] {
        int v = kOff;
        if (!FileExists("render_help.off")) {
            if (FileExists("render_help_stats")) v = kStats;
            if (FileExists("render_help")) v = kOn;
        }
        if (v) REXLOG_INFO("RENDER HELP (821938B8 list-prep join): {}", v == kOn ? "on" : "stats only");
        return v;
    }();
    return m;
}
inline uint8_t* G(uint8_t* b, uint32_t a) { return b + a + REX_PHYS_HOST_OFFSET(a); }
inline uint32_t R32(uint8_t* b, uint32_t a) { uint32_t v; std::memcpy(&v, G(b, a), 4); return __builtin_bswap32(v); }
inline void W32(uint8_t* b, uint32_t a, uint32_t v) { v = __builtin_bswap32(v); std::memcpy(G(b, a), &v, 4); }
inline void Lock(PPCContext& ctx, uint8_t* base, uint32_t ring) {
    ctx.r3.u64 = ring + 4104;
    ctx.lr = 0x82188E44;  // as 82188E20
    __imp__sub_82188E98(ctx, base);
}
inline void Unlock(uint8_t* base, uint32_t ring) {
    std::atomic_thread_fence(std::memory_order_seq_cst);
    W32(base, ring + 4104, 1);
}
// Head of the class-4 queue if it is one of ours (removed from the queue), else 0.
inline uint32_t PopOurs(PPCContext& ctx, uint8_t* base) {
    Lock(ctx, base, kQueue4);
    uint32_t job = 0;
    const uint32_t r = R32(base, kQueue4 + 4096), w = R32(base, kQueue4 + 4100);
    if (r != w) {
        const uint32_t cand = R32(base, kQueue4 + ((r & 0x3FFu) << 2));
        const uint32_t fn = cand ? R32(base, cand + 8) : 0;
        if (fn == kJobList || fn == kJobNop) {
            job = cand;
            W32(base, kQueue4 + 4096, (r + 1) & 0x3FFu);
        }
    }
    Unlock(base, kQueue4);
    return job;
}
// 82636BB0 loop body (82636C64..82636CF0) without the worker-slot bookkeeping (+44 current job, +48 state).
inline void RunJob(PPCContext& ctx, uint8_t* base, uint32_t job) {
    const uint32_t fn = R32(base, job + 8);
    ctx.r3.u64 = job;
    ctx.r4.u64 = R32(base, job + 20);
    ctx.r5.u64 = R32(base, job + 24);
    ctx.lr = 0x82636C7C;
    REX_CALL_INDIRECT_FUNC(fn);
    const uint32_t ev = R32(base, job + 12);
    if (ev) {
        ctx.r3.u64 = ev;
        ctx.lr = 0x82636C8C;
        __imp__sub_82716D20(ctx, base);
        if (ctx.r3.s32 == 0) {
            ctx.lr = 0x82636C98;
            __imp__sub_82716CE0(ctx, base);
        }
    }
    const uint32_t grp = R32(base, job + 16);
    if (grp) {
        ctx.r3.u64 = grp;
        const uint32_t f = R32(base, R32(base, grp) + 12);
        ctx.lr = 0x82636CB8;
        REX_CALL_INDIRECT_FUNC(f);
    }
    Lock(ctx, base, kPool);
    const uint32_t w = R32(base, kPool + 4100);
    W32(base, kPool + ((w & 0x3FFu) << 2), job);
    W32(base, kPool + 4100, (w + 1) & 0x3FFu);
    Unlock(base, kPool);
}
struct Stats { uint64_t joins = 0, jobs = 0, nops = 0, done_before_wait = 0, depth = 0, help_ticks = 0, wait_ticks = 0; };
Stats st;  // render thread only
inline void Join(PPCContext& ctx, uint8_t* base) {
    const int mode = GetMode();
    const uint32_t sync = ctx.r3.u32, arg = ctx.r4.u32, lr = uint32_t(ctx.lr);
    const uint64_t t0 = __rdtsc();
    {
        const uint32_t r = R32(base, kQueue4 + 4096), w = R32(base, kQueue4 + 4100);
        st.depth += (w - r) & 0x3FFu;
    }
    if (mode == kOn && sync) {
        while (R32(base, sync + 4) != 0) {
            const uint32_t job = PopOurs(ctx, base);
            if (!job) break;
            if (R32(base, job + 8) == kJobNop) ++st.nops; else ++st.jobs;
            RunJob(ctx, base, job);
        }
    }
    const uint64_t t1 = __rdtsc();
    if (!sync || R32(base, sync + 4) == 0) ++st.done_before_wait;
    ctx.r3.u64 = sync;
    ctx.r4.u64 = arg;
    ctx.lr = lr;
    __imp__sub_826375A8(ctx, base);
    const uint64_t t2 = __rdtsc();
    st.help_ticks += t1 - t0;
    st.wait_ticks += t2 - t1;
    if ((++st.joins & 4095u) == 0) {
        const double us = mesh_census::TickUs(), n = double(st.joins);
        REXLOG_INFO("RENDER HELP ({}): joins {} | jobs helped {:.2f}/join, no-op {:.2f}/join | queue depth at join {:.1f} | "
                    "complete before wait {:.1f} % | help {:.3f} ms/join, join wait {:.3f} ms/join",
                    mode == kOn ? "on" : "stats", st.joins, st.jobs / n, st.nops / n, st.depth / n,
                    100.0 * st.done_before_wait / n, st.help_ticks * us / 1000.0 / n, st.wait_ticks * us / 1000.0 / n);
    }
}
}  // namespace render_help
PPC_FUNC(sub_826375A8) {
    if (uint32_t(ctx.lr) == render_help::kJoinLr && render_help::GetMode() != render_help::kOff) [[unlikely]] {
        render_help::Join(ctx, base);
        return;
    }
    __imp__sub_826375A8(ctx, base);
}
