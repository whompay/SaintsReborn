// Saints Row (Xbox 360, 2006) - ReXGlue recompiled project.
// Bootstraps the runtime, creates the window and launches the XEX module.

#include <chrono>
#include <cstdio>
#include <cstring>
#include "world_studio_bridge.h"
#include "online_integrity.h"
#include "discord_presence.h"
#include "saintsrow_config.h"
#include "saintsrow_init.h"
#include "fps_overlay.h"
#include "options_menu.h"
#include "hw_profile.h"
#include "kbm.h"
#include "perf_monitor.h"
#include "profiler.h"
#include "wml/mod_loader.h"

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/runtime.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xthread.h>
#include <rex/kernel/init.h>
#if REX_HAS_D3D12
#include <rex/graphics/d3d12/graphics_system.h>
#include <rex/system/gpu_plugin.h>
#endif
#include <rex/audio/sdl/sdl_audio_system.h>
#include <rex/input/input_system.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/immediate_drawer.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>
#include <rex/ui/windowed_app.h>

#include "display_menu.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <thread>

#ifdef SR_PGO_GEN
// Instrumented (PGO "gen") build: the profile is written every 30 s and on
// close (the shutdown watchdog may end the process before atexit runs).
extern "C" int __llvm_profile_write_file(void);
extern "C" void __llvm_profile_set_filename(const char*);
namespace {
void StartPgoWriter() {
    __llvm_profile_set_filename("pgo_saintsrow.profraw");
    std::thread([] {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(30));
            __llvm_profile_write_file();
        }
    }).detach();
    REXLOG_INFO("PGO: instrumented build, profile -> pgo_saintsrow.profraw every 30 s");
}
}  // namespace
#endif

REXCVAR_DECLARE(bool, mnk_mode);
REXCVAR_DECLARE(std::string, input_backend);
REXCVAR_DECLARE(int32_t, window_width);
REXCVAR_DECLARE(int32_t, window_height);
REXCVAR_DECLARE(std::string, window_mode);
REXCVAR_DECLARE(bool, fps_counter);

#ifdef _WIN32
#include <windows.h>
#include "wml_pagequery.h"
// Hybrid laptops (iGPU + NVIDIA / AMD dGPU): ask the drivers for the fast GPU.
// These only count when the EXE exports them (the SDK's copies live in the
// GPU plugin DLL, which the drivers don't look at).
extern "C" {
__declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

// fast_fma.h (x86-64-v2 build): file "fma_crt" = always the C runtime's fma.
extern "C" bool sr_fma_use_crt = false;

// ============================================================================
// Guest memory safety net
// ============================================================================
//
// The recompiled game dereferences null and garbage pointers in several
// places that happened to be harmless on the console. The pieces below turn
// those accesses into reads of zero / skipped instructions instead of crashes.

// Host address of the zeroed "null object" page at guest 0x0F000000. A call
// whose target lands inside it is treated like a call through a null pointer.
static uint64_t g_null_object_host_addr = 0;

// Host address of guest address 0. Usually 0x100000000, but the SDK maps the
// guest memory higher when that range is taken (some PCs have other software
// loaded there); the handler below must use the real base or the game crashes
// at startup.
static uint64_t g_guest_base = 0x100000000ull;

static void InitNullObjectPage(uint8_t* membase) {
    uint8_t* host = membase + 0x0F000000;
    g_null_object_host_addr = (uint64_t)host;
    VirtualAlloc(host, 0x10000, MEM_COMMIT, PAGE_READWRITE);
    memset(host, 0, 0x10000);
}

// Commit guest 0x00000000-0x00FFFFFF as zero-filled read-write memory, so
// near-null reads return 0 and near-null writes land in scratch memory without
// taking an exception each time. Committed in 64 KB chunks so one failure
// does not lose the whole range.
static void InitNullZeroRegion(uint8_t* membase) {
    const size_t kNullZeroSize = 0x01000000;
    const size_t kChunk = 0x10000;
    for (size_t off = 0; off < kNullZeroSize; off += kChunk) {
        VirtualAlloc(membase + off, kChunk, MEM_COMMIT, PAGE_READWRITE);
    }
}

// Length of an FF /r or F6/F7 /r instruction (opcode at `ci`, after any REX
// prefix) including its ModRM, SIB and displacement bytes. With
// `sib_base_disp32`, the disp32 implied by SIB base=5 under mod=0 is counted
// too; the call-skip paths below do not count it.
static int ModRmInstructionLength(const uint8_t* ip, int ci, bool sib_base_disp32) {
    const uint8_t modrm = ip[ci + 1];
    const int mod = modrm >> 6;
    const int rm = modrm & 7;
    int len = ci + 2;
    const bool sib = (rm == 4 && mod != 3);
    if (sib) len += 1;
    if (mod == 0 && (rm == 5 || (sib_base_disp32 && sib && (ip[ci + 2] & 7) == 5))) len += 4;
    else if (mod == 1) len += 1;
    else if (mod == 2) len += 4;
    return len;
}

static thread_local int g_veh_depth = 0;
// Main/UI thread id; faults on other threads that cannot be decoded end the
// faulting thread instead of the process.
static DWORD g_main_thread_id = 0;

static LONG WINAPI NullPageHandler(EXCEPTION_POINTERS* ep) {
    const DWORD code = ep->ExceptionRecord->ExceptionCode;

    // Writes to write-protected physical texture pages belong to the SDK's own
    // handler (it invalidates GPU caches before unprotecting them).
    if (code == EXCEPTION_ACCESS_VIOLATION &&
        ep->ExceptionRecord->NumberParameters >= 2 &&
        ep->ExceptionRecord->ExceptionInformation[0] == 1) {
        const auto address = ep->ExceptionRecord->ExceptionInformation[1];
        if (address >= g_guest_base + 0xA0000000ull && address < g_guest_base + 0x100000000ull) {
            // One-page query (wml_pagequery.h): VirtualQuery here walked the
            // whole uniform region on every write-watch fault.
            DWORD protect = 0;
            bool committed = false;
            if (WmlQueryPage(reinterpret_cast<void*>(address), &protect, &committed) && committed &&
                ((protect & 0xFF) == PAGE_READONLY || (protect & 0xFF) == PAGE_EXECUTE_READ)) {
                return EXCEPTION_CONTINUE_SEARCH;
            }
        }
    }

    if (code != EXCEPTION_ACCESS_VIOLATION &&
        code != EXCEPTION_BREAKPOINT &&
        code != EXCEPTION_ILLEGAL_INSTRUCTION) {
        // Thread naming, C++ exceptions and stack overflows go to their normal handlers.
        if (code == 0x406D1388 || code == 0xE06D7363 || code == 0xC00000FD) {
            return EXCEPTION_CONTINUE_SEARCH;
        }
        // Integer divide by zero in native code: skip the DIV/IDIV with a zero
        // result. Anything that does not decode as F6/F7 /6 or /7 is left alone.
        if (code == EXCEPTION_INT_DIVIDE_BY_ZERO) {
            uint8_t* ip = (uint8_t*)ep->ContextRecord->Rip;
            int ci = 0;
            if ((ip[ci] & 0xF0) == 0x40) ci++;  // REX
            if (ip[ci] == 0xF7 || ip[ci] == 0xF6) {
                const int reg = (ip[ci + 1] >> 3) & 7;
                if (reg == 6 || reg == 7) {
                    ep->ContextRecord->Rip += ModRmInstructionLength(ip, ci, true);
                    ep->ContextRecord->Rax = 0;
                    ep->ContextRecord->Rdx = 0;
                    return EXCEPTION_CONTINUE_EXECUTION;
                }
            }
            return EXCEPTION_CONTINUE_SEARCH;
        }
    }

    // A fault inside this handler: skip one byte rather than recurse.
    if (g_veh_depth > 0) {
        ep->ContextRecord->Rip += 1;
        ep->ContextRecord->Rax = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    // Calls through a known corrupted vtable pointer (0x8000001E): skip the
    // call and return 0.
    if (code == EXCEPTION_ACCESS_VIOLATION &&
        ep->ExceptionRecord->ExceptionInformation[1] == 0x8000001E) {
        uint8_t* ip = (uint8_t*)ep->ContextRecord->Rip;
        int ci = 0;
        if ((ip[ci] & 0xF0) == 0x40) ci++;  // REX
        if (ip[ci] == 0xFF) {
            ep->ContextRecord->Rip += ModRmInstructionLength(ip, ci, false);
        } else {
            ep->ContextRecord->Rip += 1;
        }
        ep->ContextRecord->Rax = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    g_veh_depth++;
    struct VEHGuard { ~VEHGuard() { g_veh_depth--; } } guard;

    // int 3 from SDK assertions: skip it.
    if (code == EXCEPTION_BREAKPOINT) {
        ep->ContextRecord->Rip += 1;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    // ud2 from __builtin_trap in SDK code: skip it.
    if (code == EXCEPTION_ILLEGAL_INSTRUCTION) {
        uint8_t* ip = (uint8_t*)ep->ContextRecord->Rip;
        if (ip[0] == 0x0F && ip[1] == 0x0B) {
            ep->ContextRecord->Rip += 2;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }

    if (code != EXCEPTION_ACCESS_VIOLATION) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // Execution jumped to a null function pointer (or into the null object
    // page): simulate a return with RAX=0. If the same call site keeps doing
    // this, return 1 after 20 repeats to break the caller's loop, and after
    // 100 repeats also return from the caller.
    const auto fault_rip = ep->ContextRecord->Rip;
    if (fault_rip < 0x10000 ||
        (g_null_object_host_addr && fault_rip >= g_null_object_host_addr &&
         fault_rip < g_null_object_host_addr + 0x10000)) {
        uint64_t* rsp = (uint64_t*)ep->ContextRecord->Rsp;
        ep->ContextRecord->Rip = *rsp;
        ep->ContextRecord->Rsp += 8;
        ep->ContextRecord->Rax = 0;
        static uint64_t last_ret_addr = 0;
        static int repeat_count = 0;
        const uint64_t ret_addr = ep->ContextRecord->Rip;
        if (ret_addr == last_ret_addr) {
            repeat_count++;
            if (repeat_count > 20) {
                ep->ContextRecord->Rax = 1;
                if (repeat_count > 100) {
                    uint64_t* caller_rsp = (uint64_t*)ep->ContextRecord->Rsp;
                    ep->ContextRecord->Rip = caller_rsp[0];
                    ep->ContextRecord->Rsp += 8;
                }
            }
        } else {
            last_ret_addr = ret_addr;
            repeat_count = 0;
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    const auto fault_addr = ep->ExceptionRecord->ExceptionInformation[1];

    if (fault_addr < 0x10000) {
        // Host near-null: handled by the instruction decoder below.
    } else if (fault_addr >= g_guest_base && fault_addr < g_guest_base + 0x100000000ull) {
        // Guest memory (host = guest base + guest address).
        const uint32_t guest_addr = (uint32_t)(fault_addr - g_guest_base);
        if (guest_addr < 0x10000000) {
            // Low guest addresses are never used legitimately; treat as null.
            // Inside the zero region, re-commit the 64 KB page (at most 4 times
            // per page) and retry, since the boot-time commit can be undone.
            if (guest_addr < 0x01000000) {
                constexpr uint8_t kNullZeroRetries = 4;
                static uint8_t nullzero_retry[256] = {};
                const uint32_t nz_page = guest_addr >> 16;
                if (nullzero_retry[nz_page] < kNullZeroRetries) {
                    void* nz_base = (void*)(fault_addr & ~0xFFFFull);
                    if (VirtualAlloc(nz_base, 0x10000, MEM_COMMIT, PAGE_READWRITE)) {
                        nullzero_retry[nz_page]++;
                        return EXCEPTION_CONTINUE_EXECUTION;
                    }
                }
            }
            goto null_page_handler;
        }
        // GPU MMIO range: handled by the SDK's MMIO handler.
        if (guest_addr >= 0x7FC80000 && guest_addr < 0x7FD00000) {
            return EXCEPTION_CONTINUE_SEARCH;
        }
        // Demand-commit the page and retry: 64 KB, then 4 KB, then
        // reserve+commit the enclosing 64 KB. If all fail, decode and skip.
        void* page_addr = (void*)(fault_addr & ~0xFFFull);
        void* result = VirtualAlloc(page_addr, 0x10000, MEM_COMMIT, PAGE_READWRITE);
        if (!result) {
            result = VirtualAlloc(page_addr, 0x1000, MEM_COMMIT, PAGE_READWRITE);
        }
        if (!result) {
            void* res_addr = (void*)(fault_addr & ~0xFFFFull);
            result = VirtualAlloc(res_addr, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        }
        if (!result) {
            goto null_page_handler;
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    } else {
        // Fault while reading the target of an indirect call (FF /2 or /3):
        // no return address has been pushed yet, so skip the call with RAX=0.
        uint8_t* call_ip = (uint8_t*)ep->ContextRecord->Rip;
        int ci = 0;
        if ((call_ip[ci] & 0xF0) == 0x40) ci++;  // REX
        if (call_ip[ci] == 0xFF) {
            const int reg_field = (call_ip[ci + 1] >> 3) & 7;
            if (reg_field == 2 || reg_field == 3) {
                ep->ContextRecord->Rip += ModRmInstructionLength(call_ip, ci, false);
                ep->ContextRecord->Rax = 0;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
        }
        // Other instructions: fall through to the decoder.
    }

null_page_handler:
    // Fault inside MSVCP140 (a wait on a destroyed sync object): end the thread.
    {
        HMODULE hMod = NULL;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)ep->ContextRecord->Rip, &hMod)) {
            char modName[MAX_PATH] = {0};
            GetModuleFileNameA(hMod, modName, MAX_PATH);
            if (strstr(modName, "MSVCP140") || strstr(modName, "msvcp140")) {
                ep->ContextRecord->Rip = (uint64_t)&ExitThread;
                ep->ContextRecord->Rcx = 0;
                ep->ContextRecord->Rsp &= ~0xF;
                ep->ContextRecord->Rsp -= 8;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
        }
    }

    // Decode the faulting instruction: loads get their destination register
    // zeroed; every recognized ModRM memory instruction is then skipped.
    uint8_t* rip = (uint8_t*)ep->ContextRecord->Rip;
    int rex = 0;
    int i = 0;
    bool operand_size_16 = false;
    while (rip[i] == 0x66 || rip[i] == 0x67 || rip[i] == 0xF0 || rip[i] == 0xF2 || rip[i] == 0xF3 ||
           rip[i] == 0x2E || rip[i] == 0x3E || rip[i] == 0x26 || rip[i] == 0x36 ||
           rip[i] == 0x64 || rip[i] == 0x65) {
        if (rip[i] == 0x66) operand_size_16 = true;
        i++;
        if (i > 4) break;
    }
    if ((rip[i] & 0xF0) == 0x40) {
        rex = rip[i++];
    }

    bool has_modrm = false;
    int oplen = 1;
    const uint8_t op = rip[i];
    if (op == 0x0F && (rip[i+1] == 0x38 || rip[i+1] == 0x3A)) {
        has_modrm = true; oplen = 3;  // 0F 38 xx / 0F 3A xx
    } else if (op == 0x0F && (rip[i+1] == 0xB6 || rip[i+1] == 0xB7 || rip[i+1] == 0xBE || rip[i+1] == 0xBF ||
                              rip[i+1] == 0xB0 || rip[i+1] == 0xB1 ||  // CMPXCHG
                              rip[i+1] == 0xC1 || rip[i+1] == 0xC0 ||  // XADD
                              rip[i+1] == 0xAF ||                      // IMUL
                              rip[i+1] == 0x7F || rip[i+1] == 0x11 || rip[i+1] == 0x29 ||  // SIMD stores
                              rip[i+1] == 0x6F || rip[i+1] == 0x10 || rip[i+1] == 0x28 ||  // SIMD loads
                              rip[i+1] == 0xA3 || rip[i+1] == 0xAB || rip[i+1] == 0xB3 || rip[i+1] == 0xBB)) {  // BT*
        has_modrm = true; oplen = 2;
    } else if (op == 0x8B || op == 0x89 || op == 0x8A || op == 0x88 ||  // MOV
               op == 0x3B || op == 0x39 || op == 0x3A || op == 0x38 ||  // CMP
               op == 0x85 || op == 0x84 ||                              // TEST
               op == 0x63 ||                                            // MOVSXD
               op == 0x03 || op == 0x01 || op == 0x2B || op == 0x29 ||  // ADD/SUB
               op == 0x23 || op == 0x21 || op == 0x0B || op == 0x09 ||  // AND/OR
               op == 0x33 || op == 0x31 ||                              // XOR
               op == 0x80 || op == 0x81 || op == 0x83 ||                // ALU r/m, imm
               op == 0xC6 || op == 0xC7 ||                              // MOV r/m, imm
               op == 0xF6 || op == 0xF7 ||                              // TEST/NOT/NEG/MUL/DIV
               op == 0xFE || op == 0xFF ||                              // INC/DEC/CALL/JMP
               op == 0x86 || op == 0x87) {                              // XCHG
        has_modrm = true; oplen = 1;
    }
    if (has_modrm) {
        const uint8_t modrm = rip[i + oplen];
        int reg_idx = (modrm >> 3) & 7;
        if (rex & 0x04) reg_idx += 8;  // REX.R

        DWORD64* ctx_regs[] = {
            &ep->ContextRecord->Rax, &ep->ContextRecord->Rcx,
            &ep->ContextRecord->Rdx, &ep->ContextRecord->Rbx,
            &ep->ContextRecord->Rsp, &ep->ContextRecord->Rbp,
            &ep->ContextRecord->Rsi, &ep->ContextRecord->Rdi,
            &ep->ContextRecord->R8,  &ep->ContextRecord->R9,
            &ep->ContextRecord->R10, &ep->ContextRecord->R11,
            &ep->ContextRecord->R12, &ep->ContextRecord->R13,
            &ep->ContextRecord->R14, &ep->ContextRecord->R15,
        };
        // Only loads write the ModRM reg field; for stores and immediate
        // groups it is a source/opcode extension and must be left alone.
        const bool register_load = op == 0x8B || op == 0x8A || op == 0x63 ||
            (op == 0x0F && (rip[i+1] == 0xB6 || rip[i+1] == 0xB7 ||
                            rip[i+1] == 0xBE || rip[i+1] == 0xBF));
        if (register_load && reg_idx != 4) {
            *ctx_regs[reg_idx] = 0;
        }
        if (op == 0x0F && (rip[i+1] == 0x6F || rip[i+1] == 0x10 || rip[i+1] == 0x28)) {
            (&ep->ContextRecord->Xmm0)[reg_idx] = {};
        }

        const int mod = modrm >> 6;
        const int rm = modrm & 7;
        int insn_len = i + oplen + 1;  // prefixes + opcode + ModRM
        const bool has_sib = (rm == 4 && mod != 3);
        if (has_sib) insn_len += 1;
        if (mod == 0 && rm == 5) insn_len += 4;  // RIP-relative
        else if (mod == 0 && has_sib) {
            if ((rip[i + oplen + 1] & 7) == 5) insn_len += 4;  // SIB base=5: disp32
        }
        if (mod == 1) insn_len += 1;
        else if (mod == 2) insn_len += 4;

        // Immediate operands.
        if (op == 0x80 || op == 0x83 || op == 0xC6) insn_len += 1;
        else if (op == 0x81 || op == 0xC7) insn_len += (operand_size_16 && !(rex & 8)) ? 2 : 4;
        else if (op == 0xF6) insn_len += ((modrm >> 3) & 7) < 2 ? 1 : 0;
        else if (op == 0xF7) insn_len += ((modrm >> 3) & 7) < 2 ? ((operand_size_16 && !(rex & 8)) ? 2 : 4) : 0;
        else if (op == 0x0F && rip[i + 1] == 0x3A) insn_len += 1;

        ep->ContextRecord->Rip += insn_len;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    // Unrecognized instruction: on a worker thread, end that thread instead
    // of letting the fault take down the process.
    if (g_main_thread_id != 0 && GetCurrentThreadId() != g_main_thread_id) {
        ep->ContextRecord->Rip = (uint64_t)&ExitThread;
        ep->ContextRecord->Rcx = 0;
        ep->ContextRecord->Rsp &= ~0xF;
        ep->ContextRecord->Rsp -= 8;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif  // _WIN32

class SaintsRowApp : public rex::ui::WindowedApp,
                     public rex::ui::WindowListener,
                     public rex::ui::WindowInputListener {
public:
    // F11 toggles between fullscreen and windowed, F1 shows the frame rate,
    // F10 cycles the frame rate cap (30/60/90/120/off).
    void OnKeyDown(rex::ui::KeyEvent& e) override {
        if (rex::ui::ProcessKeyEvent(e)) {
            return;
        }
        if (e.virtual_key() == rex::ui::VirtualKey::kF11 && !e.prev_state() && window_) {
            window_->SetFullscreen(!window_->IsFullscreen());
            fullscreen_ = window_->IsFullscreen();
            e.set_handled(true);
        }
        if (e.virtual_key() == rex::ui::VirtualKey::kF1 && !e.prev_state()) {
            fps_overlay_.Toggle();
            fps_shown_ = fps_overlay_.IsVisible();
            e.set_handled(true);
        }
        if (e.virtual_key() == rex::ui::VirtualKey::kF10 && !e.prev_state()) {
            fps_overlay_.ShowNotice(sr::CycleFpsCap());
            e.set_handled(true);
        }
    }
    void OnMouseWheel(rex::ui::MouseEvent& e) override {
        sr::AddMouseWheel(e.scroll_y());
        sr::world_studio::AddMouseWheel(float(e.scroll_y()) / float(rex::ui::MouseEvent::kScrollPerDetent));
    }
    // The cursor is hidden while the game has focus (the mouse moves the camera).
    void OnGotFocus(rex::ui::UISetupEvent& e) override {
        (void)e;
        if (window_ && !display_dialog_ && !sr::world_studio::EditorHostEnabled())
            window_->SetCursorVisibility(rex::ui::Window::CursorVisibility::kHidden);
    }
    void OnLostFocus(rex::ui::UISetupEvent& e) override {
        (void)e;
        if (window_) window_->SetCursorVisibility(rex::ui::Window::CursorVisibility::kVisible);
    }
    static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx) {
        return std::make_unique<SaintsRowApp>(ctx);
    }

    SaintsRowApp(rex::ui::WindowedAppContext& ctx)
        : WindowedApp(ctx, "saintsrow", "[game_directory]") {
        AddPositionalOption("game_directory");
    }

    bool OnInitialize() override {
#ifdef SR_PGO_GEN
        StartPgoWriter();
#endif
        auto exe_dir = rex::filesystem::GetExecutableFolder();

        // Saved settings (fullscreen, window_width/height, keybinds, ...).
        config_path_ = exe_dir / "saintsrow.toml";
        bool config_sets_scale = false;
        {
            std::ifstream cfg(config_path_);
            std::string text((std::istreambuf_iterator<char>(cfg)),
                             std::istreambuf_iterator<char>());
            config_sets_scale = text.find("draw_resolution_scale") != std::string::npos;
        }
        rex::cvar::LoadConfig(config_path_);

        // Game data: the command-line argument if given, otherwise a "game"
        // folder next to the exe, falling back to "extracted" two or one
        // levels above the exe.
        std::filesystem::path game_dir;
        if (auto arg = GetArgument("game_directory")) {
            game_dir = *arg;
        } else {
            auto next_to_exe = exe_dir / "game";
            auto two_up = exe_dir / ".." / ".." / "extracted";
            auto one_up = exe_dir / ".." / "extracted";
            if (std::filesystem::exists(next_to_exe)) {
                game_dir = next_to_exe;
            } else {
                game_dir = std::filesystem::exists(two_up) ? two_up : one_up;
            }
        }

        std::string log_level_str = REXCVAR_GET(log_level);
        if (REXCVAR_GET(log_verbose) && log_level_str == "info") {
            log_level_str = "trace";
        }
        // The GPU emulation warns about the same harmless things on every
        // frame; logging (and flushing) them costs frame time.
        auto log_config = rex::BuildLogConfig("saintsrow_sdk.log", log_level_str, {{"gpu", "error"}});
        log_config.flush_level = spdlog::level::err;
        rex::InitLogging(log_config);
        rex::RegisterLogLevelCallback();
        REXLOG_INFO("Saints Row starting");
        REXLOG_INFO("  Game directory: {}", game_dir.string());
        if (FILE* ff = std::fopen("fma_crt", "rb")) {
            std::fclose(ff);
            sr_fma_use_crt = true;
            REXLOG_INFO("fma: C runtime fma for every call (fma_crt)");
        }
        // Test aid: the C runtime's fma without FMA3, as on CPUs from 2012 and
        // earlier (only matters for saintsrow_compat.exe).
        if (FILE* sf = std::fopen("crt_fma_soft", "rb")) {
            std::fclose(sf);
            _set_FMA3_enable(0);
            REXLOG_INFO("fma: C runtime FMA3 path off (crt_fma_soft)");
        }
        {
            // What this PC has (weak laptops get lighter defaults, see hw_profile.h).
            const sr::HwProfile& hw = sr::GetHwProfile();
            REXLOG_INFO("Hardware: CPU {} cores / {} threads, RAM {} MB | GPU '{}' (vendor {:04X}), {} MB VRAM, "
                        "{} MB shared{} | {} Direct3D 12 adapter(s)",
                        hw.physical_cores, hw.logical_cpus, hw.total_ram_mb, hw.adapter_name, hw.vendor_id,
                        hw.dedicated_vram_mb, hw.shared_mem_mb, hw.integrated ? " (integrated)" : "",
                        hw.adapter_count);
            // Two GPUs (hybrid laptop): render on the one with the most VRAM,
            // not simply the first one DXGI lists (usually the integrated one).
            // "gpu_adapter.txt" next to the exe (a DXGI adapter index) overrides.
            int adapter = hw.adapter_count > 1 ? hw.adapter_index : -1;
            if (FILE* af = std::fopen("gpu_adapter.txt", "rb")) {
                int v = -1;
                if (std::fscanf(af, "%d", &v) == 1 && v >= -1 && v < 16) adapter = v;
                std::fclose(af);
            }
            if (adapter >= 0) {
                rex::cvar::SetFlagByName("d3d12_adapter", std::to_string(adapter));
                REXLOG_INFO("GPU adapter {} chosen", adapter);
            }
        }
        sr::StartPerfMonitor();
        sr::LoadFpsCap();
        sr::StartProfiler();

        runtime_ = std::make_unique<rex::Runtime>(game_dir);
        runtime_->set_app_context(&app_context());

        // The window must exist before runtime_->Setup() so the GPU plugin can
        // build a presenting swapchain rather than a headless provider.
        window_ = rex::ui::Window::Create(app_context(), "Saints Reborn", 1280, 720);
        if (!window_) {
            REXLOG_ERROR("Failed to create window");
            return false;
        }
        window_->AddListener(this);
        window_->AddInputListener(this, 0);
        window_->Open();
        // The World Studio editor host is shown inside the Studio window
        // (it becomes a child window there), so it stays windowed and keeps
        // the cursor: a fullscreen window keeps snapping back over the monitor.
        const bool studio_host = sr::world_studio::EditorHostEnabled();
        window_->SetCursorVisibility(studio_host ? rex::ui::Window::CursorVisibility::kVisible
                                                 : rex::ui::Window::CursorVisibility::kHidden);
        // Window mode from config ("windowed", "borderless", "fullscreen"),
        // overridden by a "start_windowed" file next to the exe.
        {
            const std::string mode = REXCVAR_GET(window_mode);
            bool start_fullscreen = mode != "windowed";
            FILE* sw = std::fopen("start_windowed", "rb");
            if (sw) {
                std::fclose(sw);
                start_fullscreen = false;
            }
            if (start_fullscreen) {
                if (mode == "fullscreen") {
                    // Restore the display mode saved by the display menu
                    // (no-op when it matches the desktop mode).
                    ApplyFullscreenDisplayMode(window_.get(), REXCVAR_GET(window_width),
                                               REXCVAR_GET(window_height));
                }
                if (!studio_host) {
                    window_->SetFullscreen(true);
                }
            }
            fullscreen_ = window_->IsFullscreen();
        }
        runtime_->set_display_window(window_.get());

        rex::RuntimeConfig config;
        // Load the GPU backend plugin and set up presentation before Setup(),
        // which would otherwise create a headless provider.
        config.graphics = rex::system::LoadGpuPlugin("xenos");
        if (!config.graphics) {
            REXLOG_ERROR("Failed to load xenos GPU plugin");
            return false;
        }
        {
            auto status = config.graphics->SetupPresentation(&app_context());
            if (XFAILED(status)) {
                REXLOG_ERROR("SetupPresentation failed: {:08X}", status);
                return false;
            }
        }
        config.audio_factory = REX_AUDIO_BACKEND(rex::audio::sdl::SDLAudioSystem);
        REXCVAR_SET(mnk_mode, true);
        {
            // Keyboard and mouse are handled in kbm.cpp, so the SDK's own key
            // bindings and mouse look are turned off. Mouse speed from
            // "mouse_sensitivity.txt" next to the exe (1.0 = default).
            rex::cvar::SetFlagByName("mnk_mouse", "false");
            for (const char* bind : {"a", "b", "x", "y", "left_trigger", "right_trigger",
                                     "left_shoulder", "right_shoulder", "lstick_up",
                                     "lstick_down", "lstick_left", "lstick_right",
                                     "lstick_press", "rstick_up", "rstick_down", "rstick_left",
                                     "rstick_right", "rstick_press", "dpad_up", "dpad_down",
                                     "dpad_left", "dpad_right", "back", "start", "guide"}) {
                rex::cvar::SetFlagByName(std::string("keybind_") + bind, "");
            }
            double sensitivity = 1.0;
            if (FILE* mf = std::fopen("mouse_sensitivity.txt", "rb")) {
                double v = 0;
                if (std::fscanf(mf, "%lf", &v) == 1 && v >= 0.05 && v <= 20.0) sensitivity = v;
                std::fclose(mf);
            }
            sr::SetMouseSensitivity(sensitivity);
        }
        // Xbox Live sign-in: the game's Xbox Live menus (Quick / Custom Match,
        // party, leaderboards) run over Epic Online Services, so the profile
        // counts as signed in whenever online play (the Epic DLL from the
        // online pack) is installed. A file named "xbox_live" forces it on,
        // "xbox_live.off" forces it off.
        {
            auto exists = [](const char* path) {
                if (FILE* f = std::fopen(path, "rb")) {
                    std::fclose(f);
                    return true;
                }
                return false;
            };
            const char* why = nullptr;
            if (exists("xbox_live.off")) {
                why = nullptr;
            } else if (exists("xbox_live")) {
                why = "xbox_live";
            } else if (exists("core/WhompaysCoop/eos/EOSSDK-Win64-Shipping.dll")) {
                why = "online play installed";
            }
            if (why) {
                rex::cvar::SetFlagByName("xam_signed_in_to_live", "true");
                REXLOG_INFO("Xbox Live sign-in: on ({})", why);
            } else {
                REXLOG_INFO("Xbox Live sign-in: off (no online play files, or xbox_live.off)");
            }
        }
        // Player name (CO-OP tab > Player Name): player_name.txt next to the exe.
        if (FILE* nf = std::fopen("player_name.txt", "rb")) {
            char nb[64] = {};
            const size_t n = std::fread(nb, 1, sizeof(nb) - 1, nf);
            std::fclose(nf);
            std::string name(nb, n);
            while (!name.empty() && (name.back() == '\n' || name.back() == '\r' || name.back() == ' ')) name.pop_back();
            if (!name.empty()) {
                rex::cvar::SetFlagByName("xam_player_name", name.substr(0, 15));
                REXLOG_INFO("Player name: {}", name.substr(0, 15));
            }
        }
        // V-Sync from the options menu: a file named "vsync" next to the exe.
        if (FILE* vf = std::fopen("vsync", "rb")) {
            std::fclose(vf);
            rex::cvar::SetFlagByName("present_vsync", "true");
        }
        {
            // Internal resolution scale (1-3): "res_scale.txt" next to the
            // exe wins over the config file (draw_resolution_scale_*).
            // Without either: 1x on weak GPUs (less than 3 GB VRAM or
            // integrated), 2x otherwise.
            const sr::HwProfile& hw = sr::GetHwProfile();
            int scale = hw.weak_gpu() ? 1 : 2;
            bool scale_from_file = false;
            if (FILE* rf = std::fopen("res_scale.txt", "rb")) {
                int v = 0;
                if (std::fscanf(rf, "%d", &v) == 1 && v >= 1 && v <= 3) {
                    scale = v;
                    scale_from_file = true;
                }
                std::fclose(rf);
            }
            if (!scale_from_file) {
                if (config_sets_scale) {
                    // The config file (display menu) sets the running scale:
                    // show it in the options menu's Resolution row too.
                    try {
                        scale = std::clamp(std::stoi(rex::cvar::GetFlagByName("draw_resolution_scale_x")), 1, 3);
                    } catch (...) {
                    }
                }
                sr::SetDefaultResScale(scale);
            }
            if (scale_from_file || !config_sets_scale) {
                const std::string sv = std::to_string(scale);
                rex::cvar::SetFlagByName("draw_resolution_scale_x", sv);
                rex::cvar::SetFlagByName("draw_resolution_scale_y", sv);
                REXLOG_INFO("Draw resolution scale: {}x ({})", scale,
                            scale_from_file ? "res_scale.txt" : "default");
            }
            // The port hands the GPU every command buffer separately; ending a
            // host GPU submission after each one costs far more than it saves.
            // Submit once per frame instead.
            rex::cvar::SetFlagByName("d3d12_submit_on_primary_buffer_end", "false");
            // Streaming while driving: keep far more textures resident than the
            // defaults (384/768 MB) instead of deleting and re-creating them,
            // and allocate the GPU copy of guest memory up front rather than
            // mapping it piece by piece (each mapping waits for the GPU).
            // Scaled to the GPU's memory: on a 1-2 GB laptop GPU these limits
            // would push textures out into system RAM over the PCIe bus.
            {
                const uint64_t budget = hw.gpu_budget_mb();
                uint64_t soft = 2048, hard = 4096;
                if (budget > 0) {
                    soft = std::clamp<uint64_t>(budget * 35 / 100, 384, 2048);
                    hard = std::clamp<uint64_t>(budget * 60 / 100, 768, 4096);
                }
                rex::cvar::SetFlagByName("texture_cache_memory_limit_soft", std::to_string(soft));
                rex::cvar::SetFlagByName("texture_cache_memory_limit_hard", std::to_string(hard));
                REXLOG_INFO("Texture cache limits: {} / {} MB (GPU budget {} MB)", soft, hard, budget);
            }
            rex::cvar::SetFlagByName("d3d12_tiled_shared_memory", "false");
            // Host RAM cache for repeated immutable packfile reads. Grow on
            // demand, keeping the Xbox guest address space and GPU budgets intact.
            int ram_cache_mb = 256;
#ifdef _WIN32
            MEMORYSTATUSEX ram{};
            ram.dwLength = sizeof(ram);
            if (GlobalMemoryStatusEx(&ram)) {
                ram_cache_mb = ram.ullTotalPhys >= 15ull * 1024 * 1024 * 1024 ? 1024 : 256;
            }
#endif
            const auto cache_setting = rex::filesystem::GetExecutableFolder() / "ram_cache_mb.txt";
            if (FILE* cf = std::fopen(cache_setting.string().c_str(), "rb")) {
                int value = 0;
                if (std::fscanf(cf, "%d", &value) == 1 && value >= 0 && value <= 4096) {
                    ram_cache_mb = value;
                }
                std::fclose(cf);
            }
#ifdef _WIN32
            if (ram.dwMemoryLoad <= 100 && ram.ullTotalPhys) {
                const int available_budget = int(ram.ullAvailPhys / (4ull * 1024 * 1024));
                if (ram_cache_mb > available_budget) ram_cache_mb = available_budget;
            }
#endif
            // World Studio editor host: cutscene packs are rebuilt while the game
            // runs; a cached copy would play the old version.
            if (sr::world_studio::EditorHostEnabled()) ram_cache_mb = 0;
            rex::cvar::SetFlagByName("host_read_cache_mb", std::to_string(ram_cache_mb));
            REXLOG_INFO("Packfile RAM read cache budget: {} MiB (fills on demand)", ram_cache_mb);
            // Let the game prepare the next command buffers while the GPU thread
            // executes earlier ones (queue depth 4; each queued buffer carries
            // copies of the command memory it uses). The GPU thread may also run
            // at most 4 ms behind: on PCs where it couldn't keep up it fell a
            // frame or more behind with 8 queued, and the game reused memory the
            // queued work still needed (garbled graphics, then a crash).
            // "gpu_queue.txt" next to the exe sets another depth,
            // "gpu_max_lag.txt" another lag in microseconds (0 = no limit); a file
            // named "sync_gpu" turns queueing off (wait for every buffer).
            {
                int max_lag_us = 4000;
                if (FILE* lf = std::fopen("gpu_max_lag.txt", "rb")) {
                    int v = 0;
                    if (std::fscanf(lf, "%d", &v) == 1 && v >= 0 && v <= 1000000) max_lag_us = v;
                    std::fclose(lf);
                }
                rex::cvar::SetFlagByName("gpu_async_max_lag_us", std::to_string(max_lag_us));
                int depth = 4;
                if (FILE* qf = std::fopen("gpu_queue.txt", "rb")) {
                    int v = 0;
                    if (std::fscanf(qf, "%d", &v) == 1 && v >= 0 && v <= 64) depth = v;
                    std::fclose(qf);
                }
                if (FILE* sf = std::fopen("sync_gpu", "rb")) {
                    std::fclose(sf);
                    depth = 0;
                }
                rex::cvar::SetFlagByName("gpu_async_depth", std::to_string(depth));
                REXLOG_INFO("GPU command queue depth: {}, max lag {} us", depth, max_lag_us);
            }
#ifdef _WIN32
            // Test aid: a file named "test_high_base" next to the exe takes the
            // usual guest memory address first, so the game runs with its
            // memory mapped higher, as on PCs where that range is in use.
            if (FILE* hf = std::fopen("test_high_base", "rb")) {
                std::fclose(hf);
                void* taken = VirtualAlloc(reinterpret_cast<void*>(0x100000000ull), 0x10000, MEM_RESERVE, PAGE_NOACCESS);
                REXLOG_INFO("test_high_base: usual guest memory address {}", taken ? "taken" : "was already in use");
            }
#endif
        }
        REXCVAR_SET(input_backend, "xinput");
        config.input_factory = REX_INPUT_BACKEND(rex::input::CreateDefaultInputSystem);
        config.kernel_init = rex::kernel::InitializeKernel;

        rex::PPCImageInfo image_info{};
        image_info.code_base = static_cast<uint32_t>(REX_CODE_BASE);
        image_info.code_size = static_cast<uint32_t>(REX_CODE_SIZE);
        image_info.image_base = static_cast<uint32_t>(REX_IMAGE_BASE);
        image_info.image_size = static_cast<uint32_t>(REX_IMAGE_SIZE);
        image_info.func_mappings = PPCFuncMappings;

        auto status = runtime_->Setup(image_info, std::move(config));
        if (XFAILED(status)) {
            REXLOG_ERROR("Runtime setup failed: {:08X}", status);
            return false;
        }
        if (runtime_->input_system()) {
            auto* input_sys = static_cast<rex::input::InputSystem*>(runtime_->input_system());
            input_sys->AttachWindow(window_.get());
            // Ignore controller/keyboard input while the game window is not focused.
            input_sys->SetActiveCallback([w = window_.get()] { return w->HasFocus(); });
            REXLOG_INFO("Input attached: controllers, keyboard and mouse");
        }

        // Whompay's Mod Loader: file replacements must be in place before the
        // game opens anything.
        wml::Initialize(exe_dir, game_dir, runtime_->file_system(), "\\Device\\Harddisk0\\Partition1");

        status = runtime_->LoadXexImage("game:\\default.xex");
        if (XFAILED(status)) {
            REXLOG_ERROR("Failed to load XEX: {:08X}", status);
            return false;
        }
        REXLOG_INFO("XEX image loaded");

        // Render without the Xbox 360's 2x MSAA. With MSAA the frame doesn't
        // fit the console's 10 MB of EDRAM, so the game draws everything twice
        // (predicated tiling: one pass per screen tile), which doubles the
        // graphics work here. The game's own "no_aa" tiling scenario draws the
        // frame once; the PC build's higher internal resolution smooths edges
        // instead. [0x827D7834] is the scenario index (r_set_tiling_scenario:
        // 0 = no_aa, 1 = 2x_aa, the default). A file named "keep_msaa" next to
        // the exe keeps the original.
        {
            FILE* km = std::fopen("keep_msaa", "rb");
            if (km) {
                std::fclose(km);
                REXLOG_INFO("Tiling scenario: 2x_aa (keep_msaa)");
            } else {
                uint8_t* membase = runtime_->memory()->virtual_membase();
                const uint32_t zero = 0;
                std::memcpy(membase + 0x827D7834u, &zero, 4);
                REXLOG_INFO("Tiling scenario: no_aa");
            }
        }
        sr::ApplyStartupGraphics(sr::GetHwProfile().weak_gpu());

#ifdef _WIN32
        // Commit the zero region before installing the handler, so near-null
        // reads never fault in the first place.
        g_guest_base = reinterpret_cast<uint64_t>(runtime_->memory()->virtual_membase());
        InitNullZeroRegion(runtime_->memory()->virtual_membase());
        InitNullObjectPage(runtime_->memory()->virtual_membase());
        g_main_thread_id = GetCurrentThreadId();
        AddVectoredExceptionHandler(1, NullPageHandler);
#endif
        // Code and script mods start once the executable is in memory.
        wml::Start(runtime_->memory()->virtual_membase());
        // Online fair play: modded or not, cheat-tool watch, online notices.
        sr::StartOnlineIntegrity(exe_dir);
        // Discord status ("Playing Saints Reborn", what and how long).
        sr::StartDiscordPresence(exe_dir, runtime_->memory()->virtual_membase());

        spdlog::default_logger()->flush();

        // Connect the GPU presenter to the window.
        auto* gs = runtime_->graphics_system();
        if (gs && gs->presenter()) {
            if (!fps_overlay_.Initialize(window_.get(), gs->provider(), gs->presenter())) {
                REXLOG_WARN("Frame rate counter unavailable");
            }
            // Text from native mods is drawn by the same overlay.
            wml::SetOverlayTextListener([this](bool shown) {
                app_context().CallInUIThread([this, shown]() { fps_overlay_.SetModTextVisible(shown); });
            });
            wml::SetOverlayBeamsListener([this](bool shown) {
                app_context().CallInUIThread([this, shown]() { fps_overlay_.SetBeamsVisible(shown); });
            });
            window_->SetPresenter(gs->presenter());

            // ImGui overlay stack: F5 opens the display settings menu.
            immediate_drawer_ = gs->provider()->CreateImmediateDrawer();
            immediate_drawer_->SetPresenter(gs->presenter());
            imgui_drawer_ = std::make_unique<rex::ui::ImGuiDrawer>(window_.get(), 64);
            imgui_drawer_->SetPresenterAndImmediateDrawer(gs->presenter(), immediate_drawer_.get());
            rex::ui::RegisterBind("bind_display_menu", "F5", "Toggle display settings menu", [this]() {
                if (display_dialog_) {
                    display_dialog_.reset();
                } else {
                    display_dialog_ = std::make_unique<DisplaySettingsDialog>(
                        imgui_drawer_.get(), window_.get(), config_path_, &fps_overlay_,
                        [this](bool visible) {
                            app_context().CallInUIThreadDeferred([this, visible] {
                                fps_overlay_.SetVisible(visible);
                                fps_shown_ = visible;
                            });
                        });
                }
            });
            if (REXCVAR_GET(fps_counter)) {
                fps_overlay_.SetVisible(true);
                fps_shown_ = true;
            }
            // PC settings rows in the game's options menu (options_menu.cpp).
            sr::OptionsHost host;
            host.is_fullscreen = [this]() { return fullscreen_.load(); };
            host.set_fullscreen = [this](bool fs) {
                app_context().CallInUIThread([this, fs]() {
                    if (!window_) return;
                    window_->SetFullscreen(fs);
                    fullscreen_ = window_->IsFullscreen();
                });
            };
            host.fps_counter_shown = [this]() { return fps_shown_.load(); };
            host.set_fps_counter = [this](bool on) {
                app_context().CallInUIThread([this, on]() {
                    if (fps_overlay_.IsVisible() != on) fps_overlay_.Toggle();
                    fps_shown_ = fps_overlay_.IsVisible();
                });
            };
            sr::SetOptionsHost(std::move(host));
            // FPS counter from the options menu: a file named "show_fps".
            if (FILE* ff = std::fopen("show_fps", "rb")) {
                std::fclose(ff);
                app_context().CallInUIThread([this]() {
                    if (!fps_overlay_.IsVisible()) fps_overlay_.Toggle();
                    fps_shown_ = fps_overlay_.IsVisible();
                });
            }
            if (sr::world_studio::EditorHostEnabled()) {
                app_context().CallInUIThread([this]() { fps_overlay_.SetStudioGizmoVisible(true); });
            }
        } else {
            REXLOG_ERROR("No presenter available to connect to window");
        }

        app_context().CallInUIThreadDeferred([this]() {
            // 1 ms Windows timer resolution (as most PC games do). The game's
            // own Sleep calls (guest 82716020 -> KeDelayExecutionThread ->
            // ::Sleep) otherwise round up to the default 15.6 ms tick since
            // Windows 10 2004 (per-process resolution); only the sampling
            // profiler raised it before. Same idea as the SR2 Juiced Patch's
            // "wait properly" fix. File "timer_resolution_default" = off.
            {
                FILE* f = std::fopen("timer_resolution_default", "rb");
                if (f) {
                    std::fclose(f);
                    REXLOG_INFO("Timer resolution: Windows default (timer_resolution_default)");
                } else {
                    using TimeBeginPeriod = unsigned(__stdcall*)(unsigned);
                    HMODULE winmm = LoadLibraryW(L"winmm.dll");
                    auto tbp = winmm ? reinterpret_cast<TimeBeginPeriod>(GetProcAddress(winmm, "timeBeginPeriod")) : nullptr;
                    const unsigned r = tbp ? tbp(1) : 1u;
                    REXLOG_INFO("Timer resolution: 1 ms {}", r == 0 ? "set" : "NOT set");
                }
            }
            auto main_thread = runtime_->PrepareModuleLaunch();
            // Persistent shader / pipeline cache (dist\cache\shaders): pipelines
            // compiled in earlier sessions are created at startup instead of
            // the first time something new is drawn (missing surfaces for a
            // few frames and hitches while flying into new areas). The first
            // start with it only records them. File "shader_cache.off" = off.
            if (main_thread) {
                auto* gs = runtime_->graphics_system();
                FILE* off = std::fopen("shader_cache.off", "rb");
                if (off) std::fclose(off);
                if (gs && !off) {
                    const auto cache_dir = rex::filesystem::GetExecutableFolder() / "cache";
                    const auto t0 = std::chrono::steady_clock::now();
                    gs->InitializeShaderStorage(cache_dir, 0x545107D1u, true);
                    REXLOG_INFO("Shader cache: {} (loaded in {:.0f} ms)", cache_dir.string(),
                                std::chrono::duration<double, std::milli>(
                                    std::chrono::steady_clock::now() - t0).count());
                }
                main_thread->Resume();
            }
            if (!main_thread) {
                REXLOG_ERROR("Failed to launch module");
                app_context().QuitFromUIThread();
                return;
            }

            module_thread_ = std::thread([this, main_thread = std::move(main_thread)]() mutable {
                main_thread->Wait(0, 0, 0, nullptr);
                REXLOG_INFO("Execution complete");
                if (!shutting_down_.load(std::memory_order_acquire)) {
                    app_context().CallInUIThread([this]() {
                        app_context().QuitFromUIThread();
                    });
                }
            });
        });

        return true;
    }

    void OnClosing(rex::ui::UIEvent& e) override {
        (void)e;
        REXLOG_INFO("Window closing, shutting down...");
#ifdef SR_PGO_GEN
        __llvm_profile_write_file();
        REXLOG_INFO("PGO: profile written");
#endif
        shutting_down_.store(true, std::memory_order_release);
        // Shutdown watchdog: closing waits for the game's main thread to end;
        // when a game or mod thread is stuck in a wait it never does, and the
        // process lived on in the background without a window. After 5 s the
        // process is ended for good.
        std::thread([] {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            REXLOG_WARN("Shutdown still not finished after 5 s - ending the process");
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            TerminateProcess(GetCurrentProcess(), 0);
        }).detach();
        if (runtime_ && runtime_->kernel_state()) {
            runtime_->kernel_state()->TerminateTitle();
        }
        app_context().QuitFromUIThread();
    }

    void OnDestroy() override {
        // Overlay teardown (dialog -> keybind -> drawer -> immediate drawer)
        // must happen before the presenter is released.
        display_dialog_.reset();
        rex::ui::UnregisterBind("bind_display_menu");
        imgui_drawer_.reset();
        immediate_drawer_.reset();
        sr::StopPerfMonitor();
        sr::StopProfiler();
        wml::SetOverlayTextListener(nullptr);
        wml::SetOverlayBeamsListener(nullptr);
        fps_overlay_.Shutdown();
        if (window_) {
            window_->SetPresenter(nullptr);
        }
        if (module_thread_.joinable()) {
            module_thread_.join();
        }
        if (window_) {
            window_->RemoveListener(this);
        }
        window_.reset();
        runtime_.reset();
    }

private:
    std::unique_ptr<rex::Runtime> runtime_;
    std::unique_ptr<rex::ui::Window> window_;
    std::unique_ptr<rex::ui::ImmediateDrawer> immediate_drawer_;
    std::unique_ptr<rex::ui::ImGuiDrawer> imgui_drawer_;
    std::unique_ptr<DisplaySettingsDialog> display_dialog_;
    std::filesystem::path config_path_;
    sr::FpsOverlay fps_overlay_;
    std::atomic<bool> fullscreen_{false};
    std::atomic<bool> fps_shown_{false};
    std::thread module_thread_;
    std::atomic<bool> shutting_down_{false};
};

XE_DEFINE_WINDOWED_APP(saintsrow, SaintsRowApp::Create)
