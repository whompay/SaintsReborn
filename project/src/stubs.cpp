// Saints Row - game-specific overrides of recompiled guest functions and
// kernel exports.
//
// PPC_FUNC(sub_X) replaces the recompiled function sub_X; the original body
// remains callable as __imp__sub_X. PPC_FUNC_IMPL(__imp__Name) replaces the
// SDK's implementation of a kernel import.

#include "saintsrow_config.h"
#include "saintsrow_init.h"
#include "fps_overlay.h"
#include "perf_monitor.h"
#include "wml/mod_loader.h"
#include "world_studio_bridge.h"

#include <rex/cvar.h>
#include <rex/graphics/graphics_system.h>
#include <rex/ppc/function.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xevent.h>
#include <rex/system/xsemaphore.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include <windows.h>
#include <tlhelp32.h>

// ============================================================================
// Helpers
// ============================================================================

namespace {

// Host pointer for a guest virtual address. The 0xE0000000 heap sits 4 KB
// further into host memory, matching what the recompiled code does.
inline uint8_t* GuestPtr(uint8_t* base, uint32_t addr) {
    return base + addr + (addr >= 0xE0000000u ? 0x1000u : 0u);
}

// Big-endian 32-bit guest read that honours the 0xE0000000 host offset.
inline uint32_t GRd32(uint8_t* base, uint32_t addr) {
    return __builtin_bswap32(*reinterpret_cast<volatile uint32_t*>(GuestPtr(base, addr)));
}

// True if `addr` lies inside the game executable's code/data image.
inline bool IsGuestImageAddress(uint32_t addr) {
    return addr >= 0x82000000u && addr < 0x83000000u;
}

}  // namespace

// ============================================================================
// XAM user / content stubs
// ============================================================================

// Report user 0 as signed in locally so the game proceeds past its sign-in check.
extern "C" uint32_t XamUserGetSigninState_entry(uint32_t user_index) {
    if (user_index == 0) return 1;  // eSignedInLocally
    return 0;                       // eNotSignedIn
}

// Return a zeroed sign-in info block; the game falls back to defaults.
extern "C" void XamUserGetSigninInfo_entry(uint32_t user_index, uint32_t flags, void* info_ptr) {
    if (info_ptr) {
        memset(info_ptr, 0, 0x100);
    }
}

// Report every content license bit as granted.
extern "C" uint32_t XamContentGetLicenseMask_entry(uint32_t* mask_ptr, void* overlapped) {
    if (mask_ptr) *mask_ptr = 0xFFFFFFFF;
    return 0;  // X_ERROR_SUCCESS
}

// ============================================================================
// Main loop / frame flags
// ============================================================================

// Device state that must be set for a frame to reach VdSwap: clear the
// swap-skip flag, mark the frame dirty, keep the frame counter non-zero and
// set the D3D "direct" bit that gates the ring-buffer kick.
static void ForceFrameFlags(uint8_t* base, uint32_t dev) {
    PPC_STORE_U32(dev + 19980, 0);
    PPC_STORE_U8(dev + 20424, PPC_LOAD_U8(dev + 20424) | 0x8);
    if (PPC_LOAD_U32(dev + 20080) == 0) {
        PPC_STORE_U32(dev + 20080, 1);
    }
    PPC_STORE_U8(dev + 10809, PPC_LOAD_U8(dev + 10809) | 0x2);
}

// Customization can reuse garment data while the command processor still has
// draws queued against it, briefly stretching shirts and trousers. Process
// streams synchronously here, then restore the configured queue on exit.
static void UpdateCustomizationQueue(uint8_t* base) {
    const bool customizing = PPC_LOAD_U32(0x827B04D4u) == 1 ||
                             PPC_LOAD_U32(0x839E0DF8u) == 0x82FFB7BCu ||
                             PPC_LOAD_U32(0x839E0FB0u) == 0x82FFB7BCu;
    static bool active = false;
    static std::string saved_depth;
    if (customizing && !active) {
        saved_depth = rex::cvar::GetFlagByName("gpu_async_depth");
        if (!saved_depth.empty() && rex::cvar::SetFlagByName("gpu_async_depth", "0")) {
            active = true;
            // Finish any streams submitted before the screen transition.
            REX_KERNEL_STATE()->emulator()->graphics_system()->SubmitCommandStream(nullptr, 0);
            REXLOG_INFO("Customization: synchronous command streams (saved queue depth {})", saved_depth);
        }
    } else if (!customizing && active) {
        if (rex::cvar::SetFlagByName("gpu_async_depth", saved_depth)) {
            active = false;
            REXLOG_INFO("Customization: restored command queue depth {}", saved_depth);
        }
    }
}

// Present: force the frame flags on the device (r3) before the real call.
extern "C" void __imp__sub_825E54A8(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_825E54A8) {
    UpdateCustomizationQueue(base);
    const uint32_t dev = ctx.r3.u32;
    if (dev) {
        ForceFrameFlags(base, dev);
    }
    // The game is held to 60 frames per second at most (it normally caps
    // itself at 30; mods can raise that).
    // In the background, with a second copy on the same PC, run at 30 at
    // most, so two copies don't fight over the GPU and stall each other.
    {
        static uint32_t focus_check = 0;
        static bool background = false;
        if ((focus_check++ & 15) == 0) {
            HWND fg = GetForegroundWindow();
            DWORD pid = 0;
            if (fg) GetWindowThreadProcessId(fg, &pid);
            const bool now_background = pid != GetCurrentProcessId();
            if (now_background != background) {
                // Windows favours the focused window's GPU work, which starves
                // a second copy running side by side. Raise this process's GPU
                // scheduling class while it is in the background.
                using SetGpuClass = LONG(WINAPI*)(HANDLE, int);
                static SetGpuClass set_gpu_class = reinterpret_cast<SetGpuClass>(
                    GetProcAddress(GetModuleHandleW(L"gdi32.dll"), "D3DKMTSetProcessSchedulingPriorityClass"));
                if (set_gpu_class) set_gpu_class(GetCurrentProcess(), now_background ? 3 : 2); // above normal / normal
            }
            background = now_background;
        }
        int cap = sr::g_fps_cap.load(std::memory_order_relaxed);
        // Cutscenes: above 60 fps the player model falls behind (the camera,
        // dialogue and other actors run on real time, the player on frame
        // steps that stop adding up past 60). Hold cutscenes at 60 at most.
        // With the Havok step fix (HavokStepFix) the player keeps up, so the
        // hold only applies without it or with the file "cutscene_hold_60".
        static const bool hold60 = [] {
            FILE* f = std::fopen("cutscene_hold_60", "rb");
            if (f) std::fclose(f);
            return f != nullptr;
        }();
        extern bool g_havok_step_fix;
        if ((hold60 || !g_havok_step_fix) && (PPC_LOAD_U8(0x8370D991u) || PPC_LOAD_U8(0x8370D990u)) &&
            (cap == 0 || cap > 60))
            cap = 60;
        // Only when another copy runs on this PC: in a real co-op session the
        // host tabbing out slowed its whole world (frame times past the
        // game's own step limit), and the other player's copies of its people
        // and cars tried to keep walking and jittered.
        // The process snapshot (several ms with many processes) runs on a
        // background thread every 3 s instead of on the main thread every 512
        // frames (perf round 5b). File "copycheck_main" = old main-thread check.
        static bool other_copy = false;
        static const bool copycheck_main = [] {
            FILE* f = std::fopen("copycheck_main", "rb");
            if (f) std::fclose(f);
            return f != nullptr;
        }();
        static auto count_copies = [] {
            int copies = 0;
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            if (snap != INVALID_HANDLE_VALUE) {
                PROCESSENTRY32W pe{};
                pe.dwSize = sizeof(pe);
                for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
                    if (_wcsicmp(pe.szExeFile, L"saintsrow.exe") == 0) ++copies;
                CloseHandle(snap);
            }
            return copies;
        };
        if (copycheck_main) {
            if ((focus_check & 511) == 1) other_copy = count_copies() >= 2;
        } else {
            struct CopycheckState {
                std::atomic<bool> other_copy{false};
                std::atomic<bool> requested{false};
                std::mutex mutex;
                std::condition_variable cv;
            };
            // The worker is detached and lasts until process termination, so
            // its synchronization state deliberately has process lifetime.
            // Leaking these few bytes avoids static-destruction races at exit.
            static CopycheckState* const copycheck = new CopycheckState;
            static std::once_flag started;
            std::call_once(started, [] {
                std::thread([] {
                    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
                    std::unique_lock<std::mutex> lock(copycheck->mutex);
                    for (;;) {
                        // A second-copy count only affects the background cap.
                        // Sleep without polling while foregrounded, then scan
                        // immediately on a foreground -> background transition.
                        copycheck->cv.wait(lock, [] {
                            return copycheck->requested.load(std::memory_order_relaxed);
                        });
                        lock.unlock();
                        copycheck->other_copy.store(count_copies() >= 2, std::memory_order_relaxed);
                        lock.lock();
                        copycheck->cv.wait_for(lock, std::chrono::seconds(3), [] {
                            return !copycheck->requested.load(std::memory_order_relaxed);
                        });
                    }
                }).detach();
            });
            const bool was_requested = copycheck->requested.exchange(background, std::memory_order_relaxed);
            if (was_requested != background) {
                // Never apply a cached result from an earlier background stint
                // while the fresh transition-triggered scan is in progress.
                copycheck->other_copy.store(false, std::memory_order_relaxed);
                copycheck->cv.notify_one();
            }
            other_copy = background && copycheck->other_copy.load(std::memory_order_relaxed);
        }
        if (background && other_copy && (cap > 30 || cap == 0)) cap = 30;
        sr::LimitFrameRate(double(cap));
    }
    // Visibility sets (PVS) off everywhere, only with the file "pvs_off" next to
    // the exe (costs ~20-30 fps at street level). By default PVS stays on at
    // street level, where its data is right, and render_fixes.cpp turns it
    // fully off when the camera is high above the street. Use-PVS byte
    // 0x827D9227 (1 = on), the same switch as RenderLab Numpad 7.
    {
        static const bool pvs_off = [] {
            FILE* f = std::fopen("pvs_off", "rb");
            if (f) std::fclose(f);
            return f != nullptr;
        }();
        if (pvs_off && PPC_LOAD_U8(0x827D9227u) != 0) PPC_STORE_U8(0x827D9227u, 0);
    }
    sr::g_game_frames.fetch_add(1, std::memory_order_relaxed);
    sr::PerfFrameBegin();
    wml::OnFrame();
    sr::world_studio::OnPresent(ctx, base);
    __imp__sub_825E54A8(ctx, base);
    // Command buffers run on the GPU thread while the game carries on; keep
    // at most one frame of them queued.
    REX_KERNEL_STATE()->emulator()->graphics_system()->SubmitCommandStream(nullptr, 1);
    sr::PerfFrameEnd();
}

// WaitForSingleObjectEx (guest wrapper): timed for the performance log.
extern "C" void __imp__sub_8271C2E8(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_8271C2E8) {
    if (!sr::PerfWaitsOn()) {
        __imp__sub_8271C2E8(ctx, base);
        return;
    }
    const bool main = sr::PerfTrackWaits();
    const uint32_t caller = uint32_t(ctx.lr);
    const auto t0 = std::chrono::steady_clock::now();
    __imp__sub_8271C2E8(ctx, base);
    const uint64_t us = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                                     std::chrono::steady_clock::now() - t0).count());
    if (main) sr::PerfRecordWait(caller, us);
    sr::PerfRecordThreadWait(0x8271C2E8u, caller, us);
}

// The game's other wait wrappers (NtWaitForSingleObjectEx 82717030,
// KeWaitForSingleObject in the D3D layer 825E28E0 / 825DF548,
// KeWaitForMultipleObjects 8260D180, NtWaitForMultipleObjectsEx 8275AFC0):
// timed per thread and caller for the PERF "THREAD WAITS" list (perf_log).
#define SR_TIMED_WAIT_HOOK(addr)                                                                  \
    extern "C" void __imp__sub_##addr(PPCContext& ctx, uint8_t* base);                           \
    PPC_FUNC(sub_##addr) {                                                                       \
        if (!sr::PerfWaitsOn()) {                                                                \
            __imp__sub_##addr(ctx, base);                                                        \
            return;                                                                              \
        }                                                                                        \
        const uint32_t caller = uint32_t(ctx.lr);                                                \
        const auto t0 = std::chrono::steady_clock::now();                                        \
        __imp__sub_##addr(ctx, base);                                                            \
        sr::PerfRecordThreadWait(0x##addr##u, caller,                                            \
                                 uint64_t(std::chrono::duration_cast<std::chrono::microseconds>( \
                                              std::chrono::steady_clock::now() - t0)             \
                                              .count()));                                        \
    }
SR_TIMED_WAIT_HOOK(82717030)
SR_TIMED_WAIT_HOOK(825E28E0)
SR_TIMED_WAIT_HOOK(8260D180)
SR_TIMED_WAIT_HOOK(8275AFC0)

// Havok physics step follows the frame rate (port of the Havok physics FPS
// fix from the Saints Row 2 Juiced Patch / xenia-canary TU1 patch by
// Clippy95, Tervel and uzis - MIT, Kobraworks Modding Group). Physics runs in
// fixed steps of Havok_step_time (float 0x8370DC9C, console var; 82255FD0
// works out how many whole steps fit in the frame). At high frame rates less
// than one step fits into a frame, so physics objects (cutscene props, the
// player in cutscenes, doors) only move every other frame and fall out of
// step with the animation. Here the step is set every frame from the real
// frame time: one step per frame at 60 fps and above (step = frame time),
// two per frame between 30 and 60 (frame time / 2, as the original patch),
// the game's own value below 30. File "havok_step_original" = off.
bool g_havok_step_fix = false;
static void HavokStepFix(uint8_t* base) {
    static const bool off = [] {
        FILE* f = std::fopen("havok_step_original", "rb");
        if (f) std::fclose(f);
        return f != nullptr;
    }();
    constexpr uint32_t kStep = 0x8370DC9Cu;
    static float original = 0.0f;
    static bool have_original = false;
    static auto last = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    const double dt = std::chrono::duration<double>(now - last).count();
    last = now;
    uint32_t bits = PPC_LOAD_U32(kStep);
    float cur;
    std::memcpy(&cur, &bits, 4);
    if (!have_original) {
        if (!(cur > 0.001f && cur < 0.1f)) return;  // not set up yet
        original = cur;
        have_original = true;
        REXLOG_INFO("Havok step: game value {:.5f} s ({:.1f} Hz), fix {}", original, 1.0 / original,
                    off ? "OFF (havok_step_original)" : "on");
    }
    g_havok_step_fix = !off;
    if (off) return;
    float step = original;
    if (dt > 1.0 / 400.0 && dt <= 1.0 / 60.0 + 0.0005) step = float(dt);
    else if (dt > 1.0 / 60.0 && dt <= 1.0 / 30.0 + 0.001) step = float(dt * 0.5);
    std::memcpy(&bits, &step, 4);
    PPC_STORE_U32(kStep, bits);
    static auto next_log = now + std::chrono::seconds(10);
    static double sum = 0;
    static int n = 0;
    sum += step;
    ++n;
    if (now >= next_log) {
        static int lines = 0;
        if (lines++ < 100)
            REXLOG_INFO("Havok step: average {:.5f} s over {} frames (game value {:.5f})", sum / n, n, original);
        sum = 0;
        n = 0;
        next_log = now + std::chrono::seconds(10);
    }
}

// Per-frame render entry (GL2_Render). The flags at 0x8370E9DF/0x8370E9F6
// gate an indirect call through [0x8370F248]; clear them while that pointer
// is not valid code. Afterwards force the frame flags on the global device.
extern "C" void __imp__sub_8262FFE0(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_8262FFE0) {
    if (!IsGuestImageAddress(PPC_LOAD_U32(0x8370F248))) {
        PPC_STORE_U8(0x8370E9DF, 0);
        PPC_STORE_U8(0x8370E9F6, 0);
    }
    __imp__sub_8262FFE0(ctx, base);
    sr::world_studio::AfterFrameTimer(base);
    HavokStepFix(base);
    ForceFrameFlags(base, 0x40001E00);
}

// Main game loop: same function-pointer gate as above, checked on entry.
extern "C" void __imp__sub_82185498(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_82185498) {
    if (!IsGuestImageAddress(PPC_LOAD_U32(0x8370F248))) {
        PPC_STORE_U8(0x8370E9DF, 0);
    }
    // Run mod callbacks at the game-loop boundary, before the render/update
    // work driven by the original loop.  Calling them from Present happened
    // after world simulation, so transforms written by mods were overwritten before the
    // next visible frame.
    if (!sr::world_studio::BeforeGameFrame(base)) return;
    wml::OnGameFrame();
    __imp__sub_82185498(ctx, base);
}

// IDirect3DQuery::GetData. For occlusion queries (type 9) the original
// returns "1 sample visible" without reading the results whenever the device's
// "direct" bit is set, which ForceFrameFlags always does, so occluded lights
// and flares were drawn through walls. This is the same code without that
// shortcut: sum (end - begin) passed samples over the query's per-tile blocks,
// or report "not ready" while the last block still holds D3D's marker.
extern "C" void __imp__sub_825D4748(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_825D4748) {
    const uint32_t query = ctx.r3.u32;
    const uint32_t out = ctx.r4.u32;
    if (!query || GRd32(base, query + 4) != 9) {
        __imp__sub_825D4748(ctx, base);
        return;
    }
    const uint32_t dev = GRd32(base, query + 0);
    const uint32_t tiles = GRd32(base, query + 144);
    if (tiles == 0) {
        PPC_STORE_U32(out, 0);
        ctx.r3.u64 = 0;  // S_OK
        return;
    }

    constexpr uint32_t kNotFinished = 0xFFFFFEED;
    const uint32_t last = GRd32(base, query + (tiles + 5) * 4);
    if (GRd32(base, last + 0) == kNotFinished && GRd32(base, last + 8) == kNotFinished &&
        GRd32(base, last + 16) == kNotFinished && GRd32(base, last + 24) == kNotFinished) {
        // Still pending: kick the GPU if this query is in the current batch.
        if (GRd32(base, query + 20) == GRd32(base, dev + 10780)) {
            ctx.r3.u64 = dev;
            sub_825D38C0(ctx, base);
        }
        PPC_STORE_U32(out, 1);
        ctx.r3.u64 = 1;  // S_FALSE
        return;
    }

    // Each tile has a 64-byte area: the end block, then the begin block. The GPU
    // writes the counters little-endian.
    auto le32 = [base](uint32_t addr) {
        uint32_t v;
        std::memcpy(&v, GuestPtr(base, addr), sizeof(v));
        return v;
    };
    uint32_t sum = 0;
    for (uint32_t i = 0; i < tiles; ++i) {
        const uint32_t block = GRd32(base, query + 24 + i * 4);
        sum += le32(block + 16) + le32(block + 20) - le32(block + 48) - le32(block + 52);
    }
    PPC_STORE_U32(out, sum);
    ctx.r3.u64 = 0;  // S_OK
}

// System-link network tick (was stubbed out as "physics update" during bring-up).
// It reads received packets, runs the host/client logic and the host's load step that
// switches the game to the multiplayer mode (6). Runs only while the game's network is
// active ([0x8370E9F6] == 1: System Link Start Game / Find Game); otherwise returns 0
// as before, so single player is unchanged.
// The DemonWare manager (sub_82356998, first call of the net tick) also has to run
// in the menus while the multiplayer GANGS connection is open (gangs_bridge.cpp),
// or the gang screens never see the server's replies.
extern "C" void __imp__sub_8234C1C0(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82356998(PPCContext& ctx, uint8_t* base);
bool SrGangsActive();
PPC_FUNC(sub_8234C1C0) {
    if (*GuestPtr(base, 0x8370E9F6u) == 1) {
        __imp__sub_8234C1C0(ctx, base);
        return;
    }
    if (SrGangsActive()) sub_82356998(ctx, base);
    ctx.r3.u64 = 0;
}

// Post-loop callback dispatch blocks on a critical section; return 0 instead.
PPC_FUNC(sub_82604C10) {
    ctx.r3.u64 = 0;
}

// ============================================================================
// GPU command submission
// ============================================================================
//
// The game's D3D writes command segments and INDIRECT_BUFFER packets into the
// primary ring. This port does not execute the ring; instead each command
// buffer is handed to the command processor with SubmitCommandStream() at the
// point D3D queues it (sub_825D3218), which preserves D3D's ordering relative
// to predicated-tiling replays. The call returns once the previous buffer has
// executed (gpu_async_depth, set in main.cpp), so the game prepares the next
// buffer while the GPU thread executes this one. The frame-boundary and
// read-back waits below keep the GPU from falling further behind.

// Ring buffer / render-state init. The main loop re-enters it every frame,
// but it must run once: later calls return the first call's result. After the
// real init, copy the primary ring's ME_INIT header to its physical backing,
// zero the rest, and register the ring and read-pointer write-back with the GPU.
extern "C" void __imp__sub_825D3DA8(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_825D3DA8) {
    static bool init_done = false;
    static uint32_t cached_ret = 0;
    if (init_done) {
        ctx.r3.u32 = cached_ret;
        return;
    }
    init_done = true;

    const uint32_t dev = ctx.r3.u32;
    __imp__sub_825D3DA8(ctx, base);
    cached_ret = ctx.r3.u32;

    const uint32_t rptr_writeback = PPC_LOAD_U32(dev + 10768);
    const uint32_t ring = PPC_LOAD_U32(dev + 13436);
    if (ring == 0) {
        return;
    }
    auto* ks = REX_KERNEL_STATE();
    auto* gs = ks->emulator()->graphics_system();
    const uint32_t ring_phys = ks->memory()->GetPhysicalAddress(ring);
    if (ring_phys == UINT32_MAX) {
        return;
    }
    constexpr uint32_t kRingBytes = 0x8000;
    constexpr uint32_t kMeInitBytes = 31 * 4;
    uint8_t* ring_virt_host = ks->memory()->TranslateVirtual(ring);
    uint8_t* ring_phys_host = ks->memory()->TranslatePhysical(ring_phys);
    if (ring_virt_host && ring_phys_host) {
        memcpy(ring_phys_host, ring_virt_host, kMeInitBytes);
        memset(ring_phys_host + kMeInitBytes, 0, kRingBytes - kMeInitBytes);
    }
    gs->InitializeRingBuffer(ring_phys, 12);
    if (rptr_writeback) {
        const uint32_t wb_phys = ks->memory()->GetPhysicalAddress(rptr_writeback);
        if (wb_phys != UINT32_MAX) {
            gs->EnableReadPointerWriteBack(wb_phys, 6);
        }
    }
}

// Segment kick: run it with the device's "direct" bit (device+10809 & 0x2)
// cleared. With the bit set, D3D writes the segment's completion fence from
// the CPU at kick time and recycles memory the GPU has not executed yet;
// cleared, the fence is written by the segment itself when it executes.
extern "C" void __imp__sub_825D3580(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_825D3580) {
    volatile uint8_t* flags = GuestPtr(base, ctx.r3.u32) + 10809;
    const uint8_t saved = *flags;
    const bool clear = (saved & 0x2u) != 0;
    if (clear) *flags = uint8_t(saved & ~0x2u);
    __imp__sub_825D3580(ctx, base);
    if (clear) *flags = uint8_t(*flags | 0x2u);
}

// Number of indirect buffers submitted so far (progress signal for the
// tiling executor spin-break below).
static std::atomic<uint32_t> g_submitted_ibs{0};

// Queue an INDIRECT_BUFFER: instead of writing it into the primary ring,
// execute it on the command processor. r4 points to {dword count, GPU address}.
PPC_FUNC(sub_825D3218) {
    sr::PerfCount(sr::kPerfCommandBuffers);
    const uint32_t desc = ctx.r4.u32;
    const uint32_t dwords = GRd32(base, desc + 0);
    const uint32_t gpu_addr = GRd32(base, desc + 4);
    auto* ks = REX_KERNEL_STATE();
    if (dwords && dwords < 0x400000u) {
        const uint8_t* host = ks->memory()->TranslatePhysical(gpu_addr & 0x1FFFFFFFu);
        ks->emulator()->graphics_system()->SubmitCommandStream(host, dwords * 4u);
    }
    g_submitted_ibs.fetch_add(1);
}

// Tile select: the deferred tiling executor writes SET_BIN_SELECT packets
// straight into the primary ring. Forward whatever this call appended to the
// ring to the command processor, in order with the submitted IBs.
extern "C" void __imp__sub_825DF0B8(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_825DF0B8) {
    const uint32_t dev = GRd32(base, GRd32(base, 0x82000658u));
    const uint32_t wptr_before = dev ? GRd32(base, dev + 10820) : 0u;
    __imp__sub_825DF0B8(ctx, base);
    if (!dev) {
        return;
    }
    const uint32_t wptr_after = GRd32(base, dev + 10820);
    if (wptr_after == wptr_before) {
        return;
    }
    const uint32_t ring = GRd32(base, dev + 13496);
    const uint32_t mask = GRd32(base, dev + 13500);
    uint32_t words[16];
    uint32_t n = 0;
    for (uint32_t w = wptr_before; w != wptr_after && n < 16; w = (w + 1) & mask) {
        words[n++] = __builtin_bswap32(GRd32(base, ring + (w << 2)));  // keep guest (BE) layout
    }
    REX_KERNEL_STATE()->emulator()->graphics_system()->SubmitCommandStream(
        reinterpret_cast<const uint8_t*>(words), n * 4u);
}

// End of a predicated-tiling pass: builds and queues the per-tile replay.
// With the "direct" bit set D3D skips the replay entirely, so run it with the
// bit cleared (restoring it afterwards).
extern "C" void __imp__sub_825DFBF8(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_825DFBF8) {
    sr::PerfCount(sr::kPerfTilingReplay);
    const uint32_t dev = ctx.r3.u32;
    if (!dev) {
        __imp__sub_825DFBF8(ctx, base);
        return;
    }
    volatile uint8_t* flags = GuestPtr(base, dev) + 10809;
    const uint8_t saved = *flags;
    *flags = uint8_t(saved & ~0x2u);
    __imp__sub_825DFBF8(ctx, base);
    *flags = uint8_t(*flags | (saved & 0x2u));
}

// ============================================================================
// Deferred tiling executor guards
// ============================================================================
//
// D3D's deferred tiling executor (sub_825DF708) walks task lists stored in
// command-buffer memory. Because the port runs D3D in "direct" mode, that
// memory can be handed out again before the executor has read it.

namespace {
std::atomic<int> g_exec_inside{0};        // threads inside sub_825DF708
std::atomic<int> g_exec_end_wait{0};      // threads inside sub_825DF548
std::atomic<uint32_t> g_exec_state{0};    // executor state struct (last seen)
thread_local int t_in_exec = 0;
}  // namespace

// New command segment: before handing out segment memory, wait (at most
// 50 ms) until the executor has taken every pending list and is idle or
// parked at the end of its list. Skipped on the executor's own thread.
extern "C" void __imp__sub_825D2F70(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_825D2F70) {
    if (t_in_exec == 0) {
        const auto t0 = std::chrono::steady_clock::now();
        uint32_t spins = 0;
        for (;;) {
            const uint32_t state = g_exec_state.load();
            const uint32_t pending = state ? GRd32(base, state + 80) : 0u;
            const int inside = g_exec_inside.load();
            const int end_wait = g_exec_end_wait.load();
            if (pending == 0 && (inside == 0 || end_wait > 0)) break;
            if (std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(50)) break;
            // The executor normally finishes within a millisecond or two, and
            // sleeping (1 ms or more on Windows) would add that much to the frame.
            if (++spins > 20000) {
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            } else {
                std::this_thread::yield();
            }
        }
    }
    __imp__sub_825D2F70(ctx, base);
}

// Executor entry: record its state struct and that it is running.
extern "C" void __imp__sub_825DF708(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_825DF708) {
    sr::PerfCount(sr::kPerfTilingExecutor);
    g_exec_state.store(GRd32(base, ctx.r3.u32));
    g_exec_inside.fetch_add(1);
    ++t_in_exec;
    __imp__sub_825DF708(ctx, base);
    --t_in_exec;
    g_exec_inside.fetch_sub(1);
}

// Executor end-of-list wait: record that the executor is parked here.
extern "C" void __imp__sub_825DF548(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_825DF548) {
    g_exec_end_wait.fetch_add(1);
    const bool timed = sr::PerfWaitsOn();
    const uint32_t caller = uint32_t(ctx.lr);
    const auto t0 = std::chrono::steady_clock::now();
    __imp__sub_825DF548(ctx, base);
    if (timed)
        sr::PerfRecordThreadWait(0x825DF548u, caller,
                                 uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                                              std::chrono::steady_clock::now() - t0).count()));
    g_exec_end_wait.fetch_sub(1);
}

// Executor step: a clobbered task list can make the executor call this
// forever at the same position. After 20000 consecutive calls with no
// progress, return a pointer to a 0xC0000000 terminator so the list ends.
extern "C" void __imp__sub_825DF2C0(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_825DF2C0) {
    const uint32_t ibs_before = g_submitted_ibs.load();
    const uint32_t in_pos = ctx.r3.u32;
    __imp__sub_825DF2C0(ctx, base);

    thread_local uint32_t last_in = 0, stuck = 0;
    const bool no_progress = (g_submitted_ibs.load() == ibs_before) && (in_pos == last_in);
    last_in = in_pos;
    stuck = no_progress ? stuck + 1 : 0;
    if (stuck >= 20000) {
        static uint32_t terminator = 0;
        if (!terminator) {
            terminator = REX_KERNEL_STATE()->memory()->SystemHeapAlloc(16);
            if (terminator) {
                *(volatile uint32_t*)(base + terminator) = __builtin_bswap32(0xC0000000u);
                *(volatile uint32_t*)(base + terminator + 4) = __builtin_bswap32(0xC0000000u);
            }
        }
        if (terminator) ctx.r3.u64 = terminator;
        stuck = 0;
    }
}

// Tile-bounds consumer: walks a chain of screen-bound record blocks (r4)
// terminated by 0xC0000000.
//  1. Validate the chain first and cut it at the first bad link, since a
//     broken link would make the walk run forever; skip the call entirely if
//     the first block is bad.
//  2. Copy completed extent results from each record's physical (GPU-written)
//     view to its virtual view, which is what the CPU code reads.
extern "C" void __imp__sub_825DF608(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_825DF608) {
    // Reads results the GPU writes: let queued command buffers finish first.
    REX_KERNEL_STATE()->emulator()->graphics_system()->SubmitCommandStream(nullptr, 0);
    auto IsBadBlock = [](uint32_t block, uint32_t end) {
        return end < block + 4 || end - block > 0x100000 || ((end - block - 4) & 15);
    };

    uint32_t link = 0;
    for (uint32_t b = ctx.r4.u32, count = 0; b != 0xC0000000u; ++count) {
        const bool plausible = b >= 0x40000000u && (b & 3u) == 0 && count < 4096;
        const uint32_t end = plausible ? GRd32(base, b) : 0u;
        if (!plausible || IsBadBlock(b, end)) {
            if (!link) {
                return;  // first block is bad: nothing to walk
            }
            *(volatile uint32_t*)GuestPtr(base, link) = __builtin_bswap32(0xC0000000u);
            break;
        }
        link = end;
        b = GRd32(base, end);
    }

    auto* memory = REX_KERNEL_STATE()->memory();
    uint32_t block = ctx.r4.u32;
    for (uint32_t blocks = 0; block && block != 0xC0000000u && blocks < 256; ++blocks) {
        const uint32_t end = GRd32(base, block);
        if (IsBadBlock(block, end)) break;
        for (uint32_t record = block + 4; record < end; record += 16) {
            const uint32_t va = record + 4;
            const uint32_t pa = memory->GetPhysicalAddress(va);
            if (pa == UINT32_MAX) continue;
            auto* cpu = memory->TranslateVirtual<uint8_t*>(va);
            const auto* gpu = memory->TranslatePhysical<const uint8_t*>(pa);
            // Completed extent result: minX=0/maxX=1024/minY=0/maxY=1024 (BE halfwords).
            const uint8_t expected[12] = {0, 0, 4, 0, 0, 0, 4, 0, 0, 0, 0, 1};
            if (!std::memcmp(gpu, expected, sizeof(expected)) &&
                std::memcmp(cpu, gpu, sizeof(expected))) {
                std::memcpy(cpu, gpu, sizeof(expected));
            }
        }
        const uint32_t next = GRd32(base, end);
        if (next == block) break;
        block = next;
    }
    __imp__sub_825DF608(ctx, base);
}

// ============================================================================
// Kernel object overrides
// ============================================================================

namespace rex { namespace kernel { namespace xboxkrnl {
    extern void KeInitializeSemaphore_entry(ppc_ptr_t<rex::system::X_KSEMAPHORE>, uint32_t, uint32_t);
    extern void KeInitializeEvent_entry(ppc_ptr_t<rex::system::X_KEVENT>, uint32_t, uint32_t);
    extern uint32_t ExCreateThread_entry(mapped_u32, uint32_t, mapped_u32, uint32_t, mapped_void,
                                         mapped_void, uint32_t);
}}}

// The game re-initializes its IO semaphores and events in place, overwriting
// the dispatch header's wait-list words that the SDK uses to associate the
// guest object with its native object. The SDK would then create a new native
// object while existing threads still wait on the old one. Remember the
// wait-list words from the first initialization and restore them on re-init
// so the original native object keeps being used.
namespace {
struct TrackedObject {
    uint32_t addr;
    uint32_t wait_list_flink;
    uint32_t wait_list_blink;
};
constexpr int kMaxTrackedObjects = 32;
TrackedObject g_tracked_sems[kMaxTrackedObjects] = {};
int g_tracked_sem_count = 0;
TrackedObject g_tracked_events[kMaxTrackedObjects] = {};
int g_tracked_event_count = 0;
}  // namespace

// KeInitializeSemaphore: on re-init keep the native object and set the count
// (a requested count of 0 becomes 2, one per IO thread, so waiting IO threads
// wake up and look for new work).
PPC_FUNC_IMPL(__imp__KeInitializeSemaphore) {
    const uint32_t sem_addr = ctx.r3.u32;
    const uint32_t count = ctx.r4.u32;

    for (int i = 0; i < g_tracked_sem_count; i++) {
        if (g_tracked_sems[i].addr == sem_addr) {
            PPC_STORE_U32(sem_addr + 8, g_tracked_sems[i].wait_list_flink);
            PPC_STORE_U32(sem_addr + 12, g_tracked_sems[i].wait_list_blink);
            PPC_STORE_U32(sem_addr + 4, (count == 0) ? 2 : count);  // signal_state
            return;
        }
    }

    HostToGuestFunction<rex::kernel::xboxkrnl::KeInitializeSemaphore_entry>(ctx, base);
    if (g_tracked_sem_count < kMaxTrackedObjects) {
        g_tracked_sems[g_tracked_sem_count++] = {sem_addr, PPC_LOAD_U32(sem_addr + 8),
                                                 PPC_LOAD_U32(sem_addr + 12)};
    }
}

// KeInitializeEvent: on re-init let the SDK reinitialize, then restore the
// original association and the requested signal state.
PPC_FUNC_IMPL(__imp__KeInitializeEvent) {
    const uint32_t evt_addr = ctx.r3.u32;
    const uint32_t initial_state = ctx.r5.u32;

    for (int i = 0; i < g_tracked_event_count; i++) {
        if (g_tracked_events[i].addr == evt_addr) {
            HostToGuestFunction<rex::kernel::xboxkrnl::KeInitializeEvent_entry>(ctx, base);
            PPC_STORE_U32(evt_addr + 8, g_tracked_events[i].wait_list_flink);
            PPC_STORE_U32(evt_addr + 12, g_tracked_events[i].wait_list_blink);
            PPC_STORE_U32(evt_addr + 4, initial_state);  // signal_state
            return;
        }
    }

    HostToGuestFunction<rex::kernel::xboxkrnl::KeInitializeEvent_entry>(ctx, base);
    if (g_tracked_event_count < kMaxTrackedObjects) {
        g_tracked_events[g_tracked_event_count++] = {evt_addr, PPC_LOAD_U32(evt_addr + 8),
                                                     PPC_LOAD_U32(evt_addr + 12)};
    }
}

// Set while sub_8260CC50 runs in thread-reuse mode (see below).
static std::atomic<bool> g_skip_thread_creation{false};
static uint32_t g_io_thread_handles[8] = {};
static int g_io_thread_handle_count = 0;

// ExCreateThread: remember the first 8 thread handles created. While
// g_skip_thread_creation is set, hand out one of those (round-robin) instead
// of creating a thread; the caller only references, re-prioritizes and resumes
// it, which is harmless on a running thread.
PPC_FUNC_IMPL(__imp__ExCreateThread) {
    const uint32_t handle_out_addr = ctx.r3.u32;

    if (g_skip_thread_creation.load()) {
        if (g_io_thread_handle_count > 0 && handle_out_addr) {
            static int reuse_idx = 0;
            const uint32_t handle = g_io_thread_handles[reuse_idx % g_io_thread_handle_count];
            reuse_idx++;
            PPC_STORE_U32(handle_out_addr, handle);
            ctx.r3.u64 = 0;  // success
        } else {
            ctx.r3.s64 = -1;
        }
        return;
    }

    HostToGuestFunction<rex::kernel::xboxkrnl::ExCreateThread_entry>(ctx, base);
    if (ctx.r3.u32 == 0 && handle_out_addr && g_io_thread_handle_count < 8) {
        const uint32_t new_handle = PPC_LOAD_U32(handle_out_addr);
        if (new_handle) {
            g_io_thread_handles[g_io_thread_handle_count++] = new_handle;
        }
    }
}

// XAudio engine creation. It is called repeatedly and creates worker threads
// each time; after 200 re-creations, reuse existing thread handles instead of
// creating more threads, which otherwise ends in STATUS_NO_MEMORY.
extern "C" void __imp__sub_8260CC50(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_8260CC50) {
    static bool first_call_done = false;
    if (!first_call_done) {
        first_call_done = true;
        __imp__sub_8260CC50(ctx, base);
        return;
    }
    static int real_count = 0;
    const bool reuse = (real_count >= 200);
    if (reuse) {
        g_skip_thread_creation.store(true);
    } else {
        real_count++;
    }
    __imp__sub_8260CC50(ctx, base);
    if (reuse) {
        g_skip_thread_creation.store(false);
    }
}

// Route the NtAllocateVirtualMemory import thunk directly to the kernel export.
extern "C" void __imp__NtAllocateVirtualMemory(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_827893D4) {
    __imp__NtAllocateVirtualMemory(ctx, base);
}

// XamLoaderTerminateTitle: never let the game terminate the title itself; the
// calling thread is parked instead.
PPC_FUNC(sub_82789084) {
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

// ============================================================================
// Streaming / loading
// ============================================================================

// Builds a filtered copy of the streaming list. Some callers pass a small sentinel value (e.g.
// 0x0000000F, 0x04000000) instead of a pointer, which the real function
// dereferences and hangs on: return 0 for anything below 0x10000000.
// Throttle only the invalid-input path. Valid calls allocate and copy a list;
// sleeping on every call adds latency even when useful work is available.
extern "C" void __imp__sub_8265F4F0(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_8265F4F0) {
    const uint32_t filter = ctx.r3.u32;
    if (filter != 0 && filter < 0x10000000u) {
        ctx.r3.u64 = 0;
        static thread_local uint32_t rejected_calls = 0;
        if (rejected_calls < 50) {
            ++rejected_calls;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return;
    }

    __imp__sub_8265F4F0(ctx, base);
}

// "Is this object finished?" check used by the post-"Begin Game" loader loop.
// The objects are sounds whose playback never completes in this port, so the
// loader would wait forever: once an object has reported busy for more than
// 6 seconds straight, report it as done.
extern "C" void __imp__sub_82105B18(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_82105B18) {
    const uint32_t obj = ctx.r3.u32;
    __imp__sub_82105B18(ctx, base);
    const bool done = (ctx.r3.u32 & 0xFF) != 0;

    static std::mutex m;
    static std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> busy_since;
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(m);
    if (done) {
        busy_since.erase(obj);
        return;
    }
    auto it = busy_since.find(obj);
    if (it == busy_since.end()) {
        busy_since[obj] = now;
    } else if (now - it->second > std::chrono::seconds(6)) {
        ctx.r3.u64 = 1;
    }
}

// ============================================================================
// Null-pointer guards
// ============================================================================
// These guest functions are regularly handed null (or near-null) pointers.
// The original code would read through them, which the null-page exception
// handler recovers from at the cost of an OS exception per access -- often
// thousands per second. Each guard returns the same result the recovered
// read would have produced, without taking the fault.

// strcmp-style compare (r3, r4): a null operand compares as "not equal".
extern "C" void __imp__sub_826FE350(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_826FE350) {
    if (ctx.r3.u32 < 0x10000u || ctx.r4.u32 < 0x10000u) {
        ctx.r3.u64 = 1;
        return;
    }
    __imp__sub_826FE350(ctx, base);
}

// strncpy(dest = r3, src = r4, n = r5): a null source copies an empty string,
// i.e. zero-fills the destination. Returns dest in r3, as the original does.
extern "C" void __imp__sub_826FE160(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_826FE160) {
    if (ctx.r5.u32 != 0 && ctx.r4.u32 < 0x10000u) {
        std::memset(GuestPtr(base, ctx.r3.u32), 0, ctx.r5.u32);
        return;
    }
    __imp__sub_826FE160(ctx, base);
}

// strchr-style scan of r3: a null string yields "not found".
extern "C" void __imp__sub_826FFD30(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_826FFD30) {
    if (ctx.r3.u32 < 0x10000u) {
        ctx.r3.u64 = 0;
        return;
    }
    __imp__sub_826FFD30(ctx, base);
}

// String append helper (object r3, string r4): a null string appends nothing
// and returns 0, the same result as its early-out path.
extern "C" void __imp__sub_82643170(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_82643170) {
    if (ctx.r4.u32 == 0) {
        ctx.r3.u64 = 0;
        return;
    }
    __imp__sub_82643170(ctx, base);
}

// Resource-cache find-or-insert keyed by r3. Key 0 is never a valid id, but
// the original would insert it by rewriting the tree's shared sentinel node,
// corrupting the cache for every other user. Report "no slot" instead.
extern "C" void __imp__sub_82106580(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_82106580) {
    if (ctx.r3.u32 == 0) {
        ctx.r3.u64 = 0;
        return;
    }
    __imp__sub_82106580(ctx, base);
}

// Huffman/inflate decoder (state r3). With null decode tables it makes no
// progress and spins forever; bail out with 0 when the state or its tables are
// clearly not valid pointers.
extern "C" void __imp__sub_82656CA8(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_82656CA8) {
    const uint32_t state = ctx.r3.u32;
    bool valid = state >= 0x10000u;
    if (valid) {
        const uint32_t inner = GRd32(base, state + 28);
        valid = inner >= 0x10000u && GRd32(base, inner + 68) >= 0x10000u &&
                GRd32(base, inner + 72) >= 0x10000u;
    }
    if (!valid) {
        ctx.r3.u64 = 0;
        return;
    }
    __imp__sub_82656CA8(ctx, base);
}

// ----------------------------------------------------------------------------
// Mid-function hooks (wired up by [[midasm_hook]] entries in the manifest)
// ----------------------------------------------------------------------------

// sub_82105FE8 @ 0x821061BC: per-voice virtual call. Slots whose voice failed
// to be created are null; skip the dispatch (jumps to 0x821061D8) for those.
bool SrSkipNullVoiceDispatch(PPCRegister& r28) {
    return r28.u32 == 0;
}

// sub_8265F4F0 @ 0x8265F50C / 0x8265F554 / 0x8265F660: byte loads that bound
// the following loops, whose 8-bit counters could otherwise never reach a
// value above 255 left behind by a recovered fault. Keep them 8-bit.
void SrClampByteR10(PPCRegister& r10) { r10.u64 &= 0xFFu; }
void SrClampByteR7(PPCRegister& r7) { r7.u64 &= 0xFFu; }
void SrClampByteR8(PPCRegister& r8) { r8.u64 &= 0xFFu; }

// Guest CRT memset(dst = r3, byte = r4, len = r5) and memcpy(dst = r3, src = r4,
// len = r5). Both are hot; run them natively. r3 stays dst, as in the originals.
PPC_FUNC(sub_82702DD0) {
    if (ctx.r5.u32)
        std::memset(GuestPtr(base, ctx.r3.u32), int(ctx.r4.u32 & 0xFF), ctx.r5.u32);
}
PPC_FUNC(sub_82702900) {
    if (ctx.r5.u32 && ctx.r3.u32 != ctx.r4.u32)
        std::memmove(GuestPtr(base, ctx.r3.u32), GuestPtr(base, ctx.r4.u32), ctx.r5.u32);
}

// Guest Sleep (82716020: r3 = milliseconds, -> KeDelayExecutionThread). Every
// 10 s: per caller, how often it sleeps, the milliseconds it asked for and the
// time it really took (a 1 ms sleep took up to 15.6 ms before the 1 ms timer
// resolution in main.cpp). Diagnostic, only with the file "sleep_stats".
namespace {
struct SleepStat {
    uint32_t lr = 0;
    uint64_t calls = 0, asked_ms = 0, took_us = 0, zero_calls = 0;
};
std::mutex g_sleep_mutex;
SleepStat g_sleep_stats[24];
}  // namespace
extern "C" void __imp__sub_82716020(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_82716020) {
    static const bool on = [] {
        FILE* f = std::fopen("sleep_stats", "rb");
        if (f) std::fclose(f);
        return f != nullptr;
    }();
    if (!on) {
        __imp__sub_82716020(ctx, base);
        return;
    }
    const uint32_t lr = uint32_t(ctx.lr), ms = ctx.r3.u32;
    const auto t0 = std::chrono::steady_clock::now();
    __imp__sub_82716020(ctx, base);
    const auto t1 = std::chrono::steady_clock::now();
    const uint64_t us = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
    static auto next = t1 + std::chrono::seconds(10);
    std::lock_guard lock(g_sleep_mutex);
    SleepStat* s = nullptr;
    for (auto& e : g_sleep_stats)
        if (e.lr == lr || e.lr == 0) { s = &e; break; }
    if (s) {
        s->lr = lr;
        ++s->calls;
        s->asked_ms += ms;
        s->took_us += us;
        if (ms == 0) ++s->zero_calls;
    }
    if (t1 >= next) {
        next = t1 + std::chrono::seconds(10);
        std::string line;
        for (auto& e : g_sleep_stats) {
            if (!e.lr) continue;
            char b[160];
            std::snprintf(b, sizeof(b), " [%08X: %llu calls (%llu of 0 ms), asked %llu ms, took %.1f ms]", e.lr,
                          (unsigned long long)e.calls, (unsigned long long)e.zero_calls,
                          (unsigned long long)e.asked_ms, e.took_us / 1000.0);
            line += b;
            e = SleepStat{};
        }
        static int lines = 0;
        if (lines++ < 200) REXLOG_INFO("GUEST SLEEPS 10 s:{}", line);
    }
}
