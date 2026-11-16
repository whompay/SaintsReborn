// Whompay's Mod Loader - game side: file overlays, native (DLL) mods, hooks,
// per-frame callbacks and keyboard state.

#include "mod_loader.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <chrono>
#include <unordered_map>
#include <vector>

#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/vfs.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include <MinHook.h>

#include "wml.h"
#include "wml_internal.h"

namespace wml {

uint8_t* g_guest_base = nullptr;

namespace {

namespace fs = std::filesystem;

fs::path g_mods_dir;
std::vector<ModInfo> g_mods;  // enabled mods, in load order
std::vector<std::string> g_core_ids;  // built-in parts (core folder), also in g_mods
std::mutex g_log_mutex;
std::ofstream g_log_file;

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------

std::once_flag g_function_map_once;
std::unordered_map<uint32_t, HostFunction> g_function_map;
std::mutex g_hook_mutex;
bool g_minhook_ready = false;
// Current head of each hooked function's chain.
std::unordered_map<uint32_t, void*> g_hook_heads;

void BuildFunctionMap() {
  for (const PPCFuncMapping* m = PPCFuncMappings; m->host; ++m) {
    g_function_map.emplace(static_cast<uint32_t>(m->guest), m->host);
  }
}

// ---------------------------------------------------------------------------
// Frame callbacks and keys
// ---------------------------------------------------------------------------

struct FrameCallback {
  WmlFrameCallback callback;
  void* user;
};
std::mutex g_frame_mutex;
std::vector<FrameCallback> g_frame_callbacks;
std::vector<FrameCallback> g_game_frame_callbacks;

std::array<uint8_t, 256> g_keys_now{};
std::array<uint8_t, 256> g_keys_prev{};
// Keys some mod has asked about. GetAsyncKeyState is a system call; polling
// all 255 keys every frame cost the game thread about a tenth of its time.
std::array<std::atomic<uint8_t>, 256> g_keys_watched{};
// While the in-game chat takes typing (chat.cpp), mods see no keys.
std::atomic<bool> g_keys_suppressed{false};

bool GameWindowFocused() {
#ifdef _WIN32
  HWND foreground = GetForegroundWindow();
  if (!foreground) return false;
  DWORD pid = 0;
  GetWindowThreadProcessId(foreground, &pid);
  return pid == GetCurrentProcessId();
#else
  return false;
#endif
}

void UpdateKeys() {
  g_keys_prev = g_keys_now;
  bool focused = GameWindowFocused() && !g_keys_suppressed.load(std::memory_order_relaxed);
  for (int vk = 1; vk < 256; ++vk) {
    if (!g_keys_watched[vk].load(std::memory_order_relaxed)) continue;
#ifdef _WIN32
    g_keys_now[vk] = focused && (GetAsyncKeyState(vk) & 0x8000) ? 1 : 0;
#else
    g_keys_now[vk] = 0;
#endif
  }
}

// ---------------------------------------------------------------------------
// The API table handed to native mods
// ---------------------------------------------------------------------------

void ApiLog(const WmlMod* mod, const char* message) {
  Log(mod && mod->name ? mod->name : "?", message ? message : "");
}
void* ApiGuestPointer(uint32_t address) { return GuestPointer(address); }
int ApiHook(uint32_t function, WmlGuestFunction hook, WmlGuestFunction* original) {
  // WmlContext is PPCContext; the two function types share one ABI.
  return InstallHook(function, reinterpret_cast<HostFunction>(reinterpret_cast<void*>(hook)),
                     reinterpret_cast<HostFunction*>(original));
}
int ApiCall(WmlContext* ctx, uint32_t function) {
  HostFunction fn = FindFunction(function);
  if (!fn || !ctx) return -1;
  fn(*reinterpret_cast<PPCContext*>(ctx), g_guest_base);
  return 0;
}
uint64_t ApiGetR(WmlContext* ctx, int i) { return GetR(*reinterpret_cast<PPCContext*>(ctx), i); }
void ApiSetR(WmlContext* ctx, int i, uint64_t v) { SetR(*reinterpret_cast<PPCContext*>(ctx), i, v); }
double ApiGetF(WmlContext* ctx, int i) { return GetF(*reinterpret_cast<PPCContext*>(ctx), i); }
void ApiSetF(WmlContext* ctx, int i, double v) { SetF(*reinterpret_cast<PPCContext*>(ctx), i, v); }
uint32_t ApiGetLr(WmlContext* ctx) {
  return static_cast<uint32_t>(reinterpret_cast<PPCContext*>(ctx)->lr);
}
void ApiOnFrame(WmlFrameCallback callback, void* user) {
  if (!callback) return;
  std::lock_guard<std::mutex> lock(g_frame_mutex);
  g_frame_callbacks.push_back({callback, user});
}
void ApiOnGameFrame(WmlFrameCallback callback, void* user) {
  if (!callback) return;
  std::lock_guard<std::mutex> lock(g_frame_mutex);
  g_game_frame_callbacks.push_back({callback, user});
}
int ApiKeyDown(int vk) { return KeyDown(vk) ? 1 : 0; }
int ApiKeyPressed(int vk) { return KeyPressed(vk) ? 1 : 0; }

// Text mods show over the game (overlay_text); drawn by the game's overlay.
// Each mod (told apart by the DLL the call comes from) has its own text;
// they are shown one under the other, so mods don't overwrite each other.
std::mutex g_overlay_mutex;
std::vector<std::pair<void*, std::string>> g_overlay_texts;
std::function<void(bool)> g_overlay_listener;

bool AnyOverlayTextLocked() {
  for (const auto& t : g_overlay_texts) {
    if (!t.second.empty()) return true;
  }
  return false;
}

void ApiOverlayText(const char* text) {
  void* owner = nullptr;
#ifdef _WIN32
  HMODULE module = nullptr;
  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(__builtin_return_address(0)), &module)) {
    owner = module;
  }
#endif
  std::string value = text ? text : "";
  bool changed_visibility;
  {
    std::lock_guard<std::mutex> lock(g_overlay_mutex);
    const bool before = AnyOverlayTextLocked();
    bool found = false;
    for (auto& t : g_overlay_texts) {
      if (t.first == owner) {
        t.second = std::move(value);
        found = true;
        break;
      }
    }
    if (!found) g_overlay_texts.emplace_back(owner, std::move(value));
    changed_visibility = before != AnyOverlayTextLocked();
  }
  if (changed_visibility) {
    bool shown;
    {
      std::lock_guard<std::mutex> lock(g_overlay_mutex);
      shown = AnyOverlayTextLocked();
    }
    Log("WML", shown ? "Mod text shown" : "Mod text hidden");
    if (g_overlay_listener) g_overlay_listener(shown);
  }
}

// Beams (overlay_beams): one list, drawn by the game's overlay.
std::mutex g_beam_mutex;
std::vector<float> g_beams;
std::chrono::steady_clock::time_point g_beam_time{};
std::function<void(bool)> g_beam_listener;
bool g_beams_shown = false;

// The GPU backend draws beams inside the 3D scene (depth-tested) when it has
// RexWorldBeamsCamera; the flat overlay is only the fallback. The beams go in
// camera space (x = up x forward, y = up, z = forward) from the camera read
// right now, the same moment the mod worked out the beam ends.
typedef void (*RexWorldBeamsFunction)(const float* data, int count);
RexWorldBeamsFunction WorldBeamsFunction() {
  static RexWorldBeamsFunction fn = [] {
    HMODULE gpu = GetModuleHandleW(L"rexgpu-xenos.dll");
    auto f = gpu ? reinterpret_cast<RexWorldBeamsFunction>(GetProcAddress(gpu, "RexWorldBeamsCamera")) : nullptr;
    Log("WML", f ? "Beams: drawn in the 3D scene" : "Beams: drawn as an overlay");
    return f;
  }();
  return fn;
}

void ApiOverlayBeams(const float* data, int count) {
  if (RexWorldBeamsFunction world = WorldBeamsFunction()) {
    float eye[3], R[3], U[3], F[3], fov;
    if (!data || count <= 0 || count > 64 || !GameCamera(eye, R, U, F, fov)) {
      world(nullptr, 0);
      return;
    }
    const float X[3] = {U[1] * F[2] - U[2] * F[1], U[2] * F[0] - U[0] * F[2], U[0] * F[1] - U[1] * F[0]};
    std::vector<float> out(data, data + size_t(count) * 10);
    for (int i = 0; i < count; ++i) {
      for (int e = 0; e < 2; ++e) {
        float* p = &out[size_t(i) * 10 + size_t(e) * 3];
        const float d[3] = {p[0] - eye[0], p[1] - eye[1], p[2] - eye[2]};
        p[0] = d[0] * X[0] + d[1] * X[1] + d[2] * X[2];
        p[1] = d[0] * U[0] + d[1] * U[1] + d[2] * U[2];
        p[2] = d[0] * F[0] + d[1] * F[1] + d[2] * F[2];
      }
    }
    world(out.data(), count);
    return;
  }
  bool changed = false, shown = false;
  {
    std::lock_guard<std::mutex> lock(g_beam_mutex);
    if (count > 0 && data && count < 256) g_beams.assign(data, data + size_t(count) * 10);
    else g_beams.clear();
    g_beam_time = std::chrono::steady_clock::now();
    shown = !g_beams.empty();
    // Shown once beams appear; hidden only when a mod clears them.
    if (shown && !g_beams_shown) { g_beams_shown = true; changed = true; }
    else if (!shown && g_beams_shown && count == 0) { g_beams_shown = false; changed = true; }
  }
  if (changed && g_beam_listener) g_beam_listener(shown);
}

// Picture look (set_look): the GPU backend applies it at swap time
// (RexLookSettings); anisotropic filtering is the texture cache's override.
typedef void (*RexLookSettingsFunction)(const float* data, int count);
// The last look a mod set (photo mode starts from it and goes back to it).
std::mutex g_last_look_mutex;
std::vector<float> g_last_look;
void ApiSetLook(const float* data, int count) {
  {
    std::lock_guard<std::mutex> lock(g_last_look_mutex);
    if (data && count >= 13) g_last_look.assign(data, data + std::min(count, 14));
    else g_last_look.clear();
    while (!g_last_look.empty() && g_last_look.size() < 14) g_last_look.push_back(0.0f);
  }
  static RexLookSettingsFunction fn = nullptr;
  if (!fn) {
    HMODULE gpu = GetModuleHandleW(L"rexgpu-xenos.dll");
    fn = gpu ? reinterpret_cast<RexLookSettingsFunction>(GetProcAddress(gpu, "RexLookSettings")) : nullptr;
    if (!fn) {
      static bool logged = false;
      if (!logged) Log("WML", "Look: the GPU backend has no RexLookSettings (update rexgpu-xenos.dll)");
      logged = true;
    }
  }
  const bool on = data && count >= 13;
  if (fn) fn(on ? data : nullptr, on ? count : 0);
  static bool aniso_set = false;
  static std::string aniso_before;
  const float aniso = on && count >= 14 ? data[13] : 0.0f;
  if (aniso >= 1.0f) {
    if (!aniso_set) aniso_before = rex::cvar::GetFlagByName("anisotropic_override");
    aniso_set = true;
    // override values: 1 = 1x, 2 = 2x, 3 = 4x, 4 = 8x, 5 = 16x
    const int level = aniso >= 16 ? 5 : aniso >= 8 ? 4 : aniso >= 4 ? 3 : aniso >= 2 ? 2 : 1;
    rex::cvar::SetFlagByName("anisotropic_override", std::to_string(level));
  } else if (aniso_set) {
    rex::cvar::SetFlagByName("anisotropic_override", aniso_before);
    aniso_set = false;
  }
}

const WmlApi kApi = {
    WML_API_VERSION, sizeof(WmlApi), ApiLog,  ReadU8,  ReadU16,         ReadU32,
    ReadF32,         WriteU8,        WriteU16, WriteU32, WriteF32,      ApiGuestPointer,
    ApiHook,         ApiCall,        ApiGetR,  ApiSetR,  ApiGetF,       ApiSetF,
    ApiGetLr,        ApiOnFrame,     ApiKeyDown, ApiKeyPressed,
    ApiOverlayText,  ApiOnGameFrame, ApiOverlayBeams, ApiSetLook,
};

// Native mods keep their WmlMod strings alive here.
struct LoadedNativeMod {
  std::string name;
  std::string folder;
  WmlMod info;
};
std::vector<std::unique_ptr<LoadedNativeMod>> g_native_mods;

void StartNativeMod(const ModInfo& mod) {
#ifdef _WIN32
  for (const auto& dll : mod.dlls) {
    HMODULE module =
        LoadLibraryExW(dll.wstring().c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module) {
      Log(mod.name, "Could not load " + PathToUtf8(dll.filename()) + " (error " +
                        std::to_string(GetLastError()) + ")");
      continue;
    }
    auto init = reinterpret_cast<WmlModInitFunction>(GetProcAddress(module, "wml_mod_init"));
    if (!init) {
      Log(mod.name, PathToUtf8(dll.filename()) + " does not export wml_mod_init; skipped");
      FreeLibrary(module);
      continue;
    }
    auto loaded = std::make_unique<LoadedNativeMod>();
    loaded->name = mod.name;
    loaded->folder = PathToUtf8(mod.folder);
    loaded->info.name = loaded->name.c_str();
    loaded->info.folder = loaded->folder.c_str();
    int result = init(&kApi, &loaded->info);
    if (result != 0) {
      Log(mod.name, PathToUtf8(dll.filename()) + " failed to initialise (" +
                        std::to_string(result) + "); unloaded");
      FreeLibrary(module);
      continue;
    }
    Log(mod.name, "Loaded " + PathToUtf8(dll.filename()));
    g_native_mods.push_back(std::move(loaded));
  }
#else
  Log(mod.name, "Native mods are only supported on Windows");
#endif
}

}  // namespace

void SetLook(const float* data, int count) { ApiSetLook(data, count); }
bool LastLook(float* out14) {
  std::lock_guard<std::mutex> lock(g_last_look_mutex);
  if (g_last_look.size() < 14) return false;
  std::copy(g_last_look.begin(), g_last_look.begin() + 14, out14);
  return true;
}

// ---------------------------------------------------------------------------
// Internals shared with lua_mods.cpp
// ---------------------------------------------------------------------------

uint8_t ReadU8(uint32_t a) { return *GuestPointer(a); }
uint16_t ReadU16(uint32_t a) {
  uint16_t v;
  std::memcpy(&v, GuestPointer(a), 2);
  return __builtin_bswap16(v);
}
uint32_t ReadU32(uint32_t a) {
  uint32_t v;
  std::memcpy(&v, GuestPointer(a), 4);
  return __builtin_bswap32(v);
}
float ReadF32(uint32_t a) {
  uint32_t v = ReadU32(a);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
void WriteU8(uint32_t a, uint8_t v) { *GuestPointer(a) = v; }
void WriteU16(uint32_t a, uint16_t v) {
  v = __builtin_bswap16(v);
  std::memcpy(GuestPointer(a), &v, 2);
}
void WriteU32(uint32_t a, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(GuestPointer(a), &v, 4);
}
void WriteF32(uint32_t a, float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  WriteU32(a, v);
}

void Log(const std::string& mod, const std::string& message) {
  std::lock_guard<std::mutex> lock(g_log_mutex);
  REXLOG_INFO("[WML] [{}] {}", mod, message);
  if (g_log_file.is_open()) {
    g_log_file << "[" << mod << "] " << message << std::endl;
  }
}

HostFunction FindFunction(uint32_t guest_address) {
  std::call_once(g_function_map_once, BuildFunctionMap);
  auto it = g_function_map.find(guest_address);
  return it == g_function_map.end() ? nullptr : it->second;
}

// While mods start up, hooks are queued and switched on together afterwards
// (one MH_ApplyQueued). MinHook suspends every thread of the process for each
// enable (and lists all threads on the PC to find them), which cost tens of ms
// per hook: ~60 hooks made mod loading take over 5 s.
bool g_hook_batch = false;
uint32_t g_hooks_queued = 0;

int InstallHook(uint32_t guest_address, HostFunction hook, HostFunction* original) {
  if (!hook || !original) return -1;
  HostFunction target = FindFunction(guest_address);
  if (!target) {
    char text[96];
    std::snprintf(text, sizeof(text), "Hook failed: no game function at %08X", guest_address);
    Log("WML", text);
    return -2;
  }
  std::lock_guard<std::mutex> lock(g_hook_mutex);
  if (!g_minhook_ready) {
    if (MH_Initialize() != MH_OK) return -3;
    g_minhook_ready = true;
  }
  // A second hook on the same function detours the previous hook, so calling
  // `original` walks down the chain to the game's code.
  auto head_it = g_hook_heads.find(guest_address);
  void* head = head_it != g_hook_heads.end() ? head_it->second : reinterpret_cast<void*>(target);
  void* trampoline = nullptr;
  if (MH_CreateHook(head, reinterpret_cast<void*>(hook), &trampoline) != MH_OK ||
      (g_hook_batch ? MH_QueueEnableHook(head) : MH_EnableHook(head)) != MH_OK) {
    char text[96];
    std::snprintf(text, sizeof(text), "Hook failed at %08X", guest_address);
    Log("WML", text);
    return -4;
  }
  *original = reinterpret_cast<HostFunction>(trampoline);
  g_hook_heads[guest_address] = reinterpret_cast<void*>(hook);
  if (g_hook_batch) ++g_hooks_queued;
  return 0;
}

std::array<std::atomic<bool>, 256> g_keys_taken{};
std::array<std::atomic<bool>, 256> g_keys_forced{};
std::atomic<double> g_camera_turn{0.0};
void TakeKey(int vk, bool taken) {
  if (vk > 0 && vk < 256) g_keys_taken[vk].store(taken, std::memory_order_relaxed);
}
void ForceKey(int vk, bool down) {
  if (vk > 0 && vk < 256) g_keys_forced[vk].store(down, std::memory_order_relaxed);
}
void TurnCamera(double radians) {
  double v = g_camera_turn.load(std::memory_order_relaxed);
  while (!g_camera_turn.compare_exchange_weak(v, v + radians, std::memory_order_relaxed)) {
  }
}
void SetOverlayTextListener(std::function<void(bool)> listener) {
  g_overlay_listener = std::move(listener);
}
bool OverlayBeams(std::vector<float>& out) {
  std::lock_guard<std::mutex> lock(g_beam_mutex);
  if (g_beams.empty() || std::chrono::steady_clock::now() - g_beam_time > std::chrono::milliseconds(150)) return false;
  out = g_beams;
  return true;
}
bool GameCamera(float eye[3], float right[3], float up[3], float forward[3], float& fov) {
  if (!g_guest_base) return false;
  constexpr uint32_t kCamera = 0x827D9778;
  for (int i = 0; i < 3; ++i) {
    eye[i] = ReadF32(kCamera + 44 + i * 4);
    right[i] = ReadF32(kCamera + 80 + i * 4);
    up[i] = ReadF32(kCamera + 92 + i * 4);
    forward[i] = ReadF32(kCamera + 104 + i * 4);
  }
  fov = ReadF32(kCamera + 188);
  if (!(fov >= 10.0f && fov <= 170.0f)) fov = 60.0f;
  return true;
}
void SetOverlayBeamsListener(std::function<void(bool)> listener) { g_beam_listener = std::move(listener); }

// The game's own text over the view (online notices); shown with the mods'.
void SetHostOverlayText(const std::string& text) {
  static int host_owner;
  bool changed_visibility;
  bool shown;
  {
    std::lock_guard<std::mutex> lock(g_overlay_mutex);
    const bool before = AnyOverlayTextLocked();
    bool found = false;
    for (auto& t : g_overlay_texts) {
      if (t.first == &host_owner) {
        t.second = text;
        found = true;
        break;
      }
    }
    if (!found) g_overlay_texts.emplace_back(&host_owner, text);
    for (auto& t : g_overlay_texts)
      if (t.first == &host_owner && !t.second.empty()) t.second.insert(0, 1, '\x02');  // not mod text
    shown = AnyOverlayTextLocked();
    changed_visibility = before != shown;
  }
  if (changed_visibility && g_overlay_listener) g_overlay_listener(shown);
}

std::vector<std::string> EnabledModIds() {
  std::vector<std::string> ids;
  for (const auto& mod : g_mods)
    if (std::find(g_core_ids.begin(), g_core_ids.end(), mod.id) == g_core_ids.end()) ids.push_back(mod.id);
  return ids;
}

// Chat (chat.cpp) keeps the overlay drawer alive while it shows; its entry
// ("\x01") is not text.
void SetChatVisible(bool visible) {
  static int chat_owner;
  bool changed_visibility;
  bool shown;
  {
    std::lock_guard<std::mutex> lock(g_overlay_mutex);
    const bool before = AnyOverlayTextLocked();
    bool found = false;
    for (auto& t : g_overlay_texts) {
      if (t.first == &chat_owner) {
        t.second = visible ? "\x01" : "";
        found = true;
        break;
      }
    }
    if (!found) g_overlay_texts.emplace_back(&chat_owner, visible ? "\x01" : "");
    shown = AnyOverlayTextLocked();
    changed_visibility = before != shown;
  }
  if (changed_visibility && g_overlay_listener) g_overlay_listener(shown);
}
void SetKeysSuppressed(bool suppressed) { g_keys_suppressed.store(suppressed, std::memory_order_relaxed); }

std::string OverlayText() {
  std::lock_guard<std::mutex> lock(g_overlay_mutex);
  std::string all;
  for (const auto& t : g_overlay_texts) {
    if (t.second.empty() || t.second[0] == '\x01' || t.second[0] == '\x02') continue;
    if (!all.empty()) all += "\n\n";
    all += t.second;
  }
  return all;
}
std::string HostOverlayText() {
  std::lock_guard<std::mutex> lock(g_overlay_mutex);
  for (const auto& t : g_overlay_texts)
    if (!t.second.empty() && t.second[0] == '\x02') return t.second.substr(1);
  return {};
}
bool KeyTaken(int vk) {
  return vk > 0 && vk < 256 && g_keys_taken[vk].load(std::memory_order_relaxed);
}
bool KeyForced(int vk) {
  return vk > 0 && vk < 256 && g_keys_forced[vk].load(std::memory_order_relaxed);
}
double TakeCameraTurn() { return g_camera_turn.exchange(0.0, std::memory_order_relaxed); }

namespace {
std::mutex g_look_mutex;
double g_look_yaw = 0, g_look_pitch = 0;
}
void AddMouseLook(double yaw, double pitch) {
  std::lock_guard<std::mutex> lock(g_look_mutex);
  // Kept small in case no mod reads it.
  g_look_yaw = std::clamp(g_look_yaw + yaw, -10.0, 10.0);
  g_look_pitch = std::clamp(g_look_pitch + pitch, -10.0, 10.0);
}
void TakeMouseLook(double& yaw, double& pitch) {
  std::lock_guard<std::mutex> lock(g_look_mutex);
  yaw = g_look_yaw;
  pitch = g_look_pitch;
  g_look_yaw = g_look_pitch = 0;
}

// Camera turn limits set by a mod each frame (wml.limit_camera): the
// current angles relative to what they are limited against, and the limits.
namespace {
std::mutex g_limit_mutex;
CameraLimit g_limit;
std::chrono::steady_clock::time_point g_limit_time{};
}
void SetCameraLimit(const CameraLimit& limit) {
  std::lock_guard<std::mutex> lock(g_limit_mutex);
  g_limit = limit;
  g_limit_time = std::chrono::steady_clock::now();
}
bool GetCameraLimit(CameraLimit& limit) {
  std::lock_guard<std::mutex> lock(g_limit_mutex);
  if (!g_limit.active || std::chrono::steady_clock::now() - g_limit_time > std::chrono::milliseconds(200)) return false;
  limit = g_limit;
  return true;
}

// A key is polled from the frame after it is first asked about.
bool KeyDown(int vk) {
  if (vk <= 0 || vk >= 256) return false;
  g_keys_watched[vk].store(1, std::memory_order_relaxed);
  return g_keys_now[vk];
}
bool KeyPressed(int vk) {
  if (vk <= 0 || vk >= 256) return false;
  g_keys_watched[vk].store(1, std::memory_order_relaxed);
  return g_keys_now[vk] && !g_keys_prev[vk];
}

namespace {
using Reg = PPCRegister PPCContext::*;
constexpr std::array<Reg, 32> kGpr = {
    &PPCContext::r0,  &PPCContext::r1,  &PPCContext::r2,  &PPCContext::r3,  &PPCContext::r4,
    &PPCContext::r5,  &PPCContext::r6,  &PPCContext::r7,  &PPCContext::r8,  &PPCContext::r9,
    &PPCContext::r10, &PPCContext::r11, &PPCContext::r12, &PPCContext::r13, &PPCContext::r14,
    &PPCContext::r15, &PPCContext::r16, &PPCContext::r17, &PPCContext::r18, &PPCContext::r19,
    &PPCContext::r20, &PPCContext::r21, &PPCContext::r22, &PPCContext::r23, &PPCContext::r24,
    &PPCContext::r25, &PPCContext::r26, &PPCContext::r27, &PPCContext::r28, &PPCContext::r29,
    &PPCContext::r30, &PPCContext::r31};
constexpr std::array<Reg, 32> kFpr = {
    &PPCContext::f0,  &PPCContext::f1,  &PPCContext::f2,  &PPCContext::f3,  &PPCContext::f4,
    &PPCContext::f5,  &PPCContext::f6,  &PPCContext::f7,  &PPCContext::f8,  &PPCContext::f9,
    &PPCContext::f10, &PPCContext::f11, &PPCContext::f12, &PPCContext::f13, &PPCContext::f14,
    &PPCContext::f15, &PPCContext::f16, &PPCContext::f17, &PPCContext::f18, &PPCContext::f19,
    &PPCContext::f20, &PPCContext::f21, &PPCContext::f22, &PPCContext::f23, &PPCContext::f24,
    &PPCContext::f25, &PPCContext::f26, &PPCContext::f27, &PPCContext::f28, &PPCContext::f29,
    &PPCContext::f30, &PPCContext::f31};
}  // namespace

uint64_t GetR(PPCContext& ctx, int i) { return i >= 0 && i < 32 ? (ctx.*kGpr[i]).u64 : 0; }
void SetR(PPCContext& ctx, int i, uint64_t v) {
  if (i >= 0 && i < 32) (ctx.*kGpr[i]).u64 = v;
}
double GetF(PPCContext& ctx, int i) { return i >= 0 && i < 32 ? (ctx.*kFpr[i]).f64 : 0.0; }
void SetF(PPCContext& ctx, int i, double v) {
  if (i >= 0 && i < 32) (ctx.*kFpr[i]).f64 = v;
}

// ---------------------------------------------------------------------------
// Public entry points
// ---------------------------------------------------------------------------

void Initialize(const fs::path& exe_dir, const fs::path& game_dir,
                rex::filesystem::VirtualFileSystem* vfs, const std::string& game_mount_path) {
  g_mods_dir = exe_dir / "mods";
  std::error_code ec;
  fs::create_directories(g_mods_dir, ec);
  g_log_file.open(g_mods_dir / "wml.log", std::ios::trunc);
  Log("WML", "Whompay's Mod Loader");

  // Built-in parts of the port (core folder next to the exe: the Saints
  // Reborn logo and the first person view): always on, loaded first, and not listed in the mod
  // manager. Like mods, they only change the player's own game files while
  // the game runs; no game files are shipped.
  std::vector<std::string> core_ids;
  for (auto& mod : LoadMods(exe_dir / "core")) {
    mod.enabled = true;
    core_ids.push_back(mod.id);
    g_core_ids.push_back(mod.id);
    g_mods.push_back(std::move(mod));
  }
  for (auto& mod : LoadMods(g_mods_dir)) {
    // A mod that is now built in (e.g. First Person) may still sit in mods\
    // from an older install: the built-in part replaces it.
    if (std::find(core_ids.begin(), core_ids.end(), mod.id) != core_ids.end()) {
      if (mod.enabled) Log("WML", mod.id + " is built in now; the copy in mods is not loaded");
      continue;
    }
    if (mod.enabled) g_mods.push_back(std::move(mod));
  }
  if (g_mods.empty()) {
    Log("WML", "No mods enabled (use WhompaysModLoader.exe to turn mods on)");
  }

  auto mount = [&](const fs::path& folder, const std::string& owner, const std::string& what) {
    if (!vfs) return;
    auto device =
        std::make_unique<rex::filesystem::HostPathDevice>(game_mount_path, folder, true);
    if (!device->Initialize() || !vfs->RegisterOverlayDevice(std::move(device))) {
      Log(owner, "Could not mount " + what);
      return;
    }
    Log(owner, "Replacing game files from " + PathToUtf8(folder));
  };

  // Later mods win file conflicts, so they are checked first.
  for (auto it = g_mods.rbegin(); it != g_mods.rend(); ++it) {
    if (it->has_files()) mount(it->files_folder, it->name, "the files folder");
  }
  // Packfiles changed by patch scripts come after whole-file replacements.
  // Always run, so outdated patched packfiles are cleaned up.
  fs::path patched = RunPatchScripts(g_mods, game_dir, g_mods_dir / ".cache");
  if (!patched.empty()) mount(patched, "WML", "the patched packfiles");
  // Which of the player's mods may be used in public online play.
  std::vector<ModInfo> player_mods;
  for (const auto& mod : g_mods)
    if (std::find(core_ids.begin(), core_ids.end(), mod.id) == core_ids.end()) player_mods.push_back(mod);
  StartFairCheck(player_mods, game_dir, g_mods_dir / ".cache");
}

void Start(uint8_t* guest_base) {
  g_guest_base = guest_base;
  const auto start_all = std::chrono::steady_clock::now();
  {
    std::lock_guard<std::mutex> lock(g_hook_mutex);
    g_hook_batch = true;
    g_hooks_queued = 0;
  }
  std::string timings;
  for (const auto& mod : g_mods) {
    const auto start_mod = std::chrono::steady_clock::now();
    std::string kinds;
    if (mod.has_files()) kinds += " files";
    if (mod.has_code()) kinds += " code";
    if (mod.has_script()) kinds += " script";
    if (mod.has_patch()) kinds += " patch";
    Log(mod.name, "Enabled (" + (kinds.empty() ? std::string(" nothing to load") : kinds.substr(1)) +
                      ")");
    if (mod.has_code()) StartNativeMod(mod);
    if (mod.has_script()) StartLuaMod(mod);
    const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start_mod)
                             .count();
    if (mod.has_code() || mod.has_script()) {
      timings += (timings.empty() ? "" : ", ") + mod.name + " " + std::to_string(ms) + " ms";
    }
  }
  const auto start_apply = std::chrono::steady_clock::now();
  uint32_t queued;
  MH_STATUS applied = MH_OK;
  {
    std::lock_guard<std::mutex> lock(g_hook_mutex);
    g_hook_batch = false;
    queued = g_hooks_queued;
    if (queued && g_minhook_ready) applied = MH_ApplyQueued();
  }
  const auto end = std::chrono::steady_clock::now();
  Log("WML", "Mods started in " +
                 std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(end - start_all)
                                    .count()) +
                 " ms (" + std::to_string(queued) + " hooks switched on together in " +
                 std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                                    end - start_apply)
                                    .count()) +
                 " ms" + (applied == MH_OK ? "" : ", FAILED") + "): " + timings);
}

void OnFrame() {
  if (g_mods.empty()) return;
  UpdateKeys();
  std::vector<FrameCallback> callbacks;
  {
    std::lock_guard<std::mutex> lock(g_frame_mutex);
    callbacks = g_frame_callbacks;
  }
  for (const auto& cb : callbacks) cb.callback(cb.user);
  LuaOnFrame();
}

void OnGameFrame() {
  if (g_mods.empty()) return;
  std::vector<FrameCallback> callbacks;
  {
    std::lock_guard<std::mutex> lock(g_frame_mutex);
    callbacks = g_game_frame_callbacks;
  }
  for (const auto& cb : callbacks) cb.callback(cb.user);
}

}  // namespace wml
