// Photo mode - see photo_mode.h.
//
// Pieces reused from World Studio (world_studio_bridge.cpp): the world pause (the frame timer divides
// each frame's time by the float at 0x827AA6D4; a huge divisor stops game time), the emulator's audio
// pause, the HUD flag (0x827AEDF2, what hud_hide() writes) and the free camera (camera 0x827D9778:
// position +44 / +56, right +80, up +92, forward +104, FOV in degrees +188), written at the start of
// the game frame, at present and after the follow camera. Filters are the GPU's look pass
// (RexLookSettings, as WML set_look), started from the look a mod set (Modern Look). Photos: the GPU
// copies the finished frame (RexCaptureRequest / RexCaptureTake, command_processor.cpp) and it is
// written as a PNG with WIC on a worker thread.
#include "photo_mode.h"

#include "saintsrow_config.h"
#include "saintsrow_init.h"
#include "wml/wml_internal.h"

#include <windows.h>
#include <shlobj.h>
#include <wincodec.h>

#include <rex/audio/audio_system.h>
#include <rex/chrono/clock.h>
#include <rex/logging.h>
#include <rex/ppc/function.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

namespace sr::photo {
namespace {

constexpr uint32_t kPlayerPointer = 0x8309ABECu;
constexpr uint32_t kCamera = 0x827D9778u;
constexpr uint32_t kHudVisible = 0x827AEDF2u;
constexpr uint32_t kTimeDivisor = 0x827AA6D4u;
// World Studio's full world pause (UpdateWorldPause): besides the divisor, the frame timer's fixed step
// (flag 0x836FD673 = 1, rate 0x827D6D30 = 1e6 steps/s -> a 1 us raw frame time) and the SDK guest clock
// slowed to 0.0001, so nothing runs on real time while game time stands still.
constexpr uint32_t kFixedStepFlag = 0x836FD673u, kFixedStepRate = 0x827D6D30u;
constexpr uint32_t kActiveScreen = 0x839E0DF8u;
constexpr uint32_t kCutscene = 0x8370D991u, kScriptedCutscene = 0x8370D990u;
constexpr float kPi = 3.14159265f;

// XInput buttons.
constexpr uint16_t kUp = 0x0001, kDown = 0x0002, kLeft = 0x0004, kRight = 0x0008, kStart = 0x0010,
                   kBack = 0x0020, kLS = 0x0040, kA = 0x1000, kB = 0x2000, kX = 0x4000, kY = 0x8000;

struct V {
  float x = 0, y = 0, z = 0;
};
V operator+(V a, V b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V operator-(V a, V b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V operator*(V a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float Dot(V a, V b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float Len(V a) { return std::sqrt(Dot(a, a)); }
bool Finite(V a) { return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z); }

float ReadF32(uint8_t* base, uint32_t a) {
  const uint32_t bits = PPC_LOAD_U32(a);
  float v;
  std::memcpy(&v, &bits, 4);
  return v;
}
void WriteF32(uint8_t* base, uint32_t a, float v) {
  uint32_t bits;
  std::memcpy(&bits, &v, 4);
  PPC_STORE_U32(a, bits);
}
V ReadV(uint8_t* base, uint32_t a) { return {ReadF32(base, a), ReadF32(base, a + 4), ReadF32(base, a + 8)}; }
void WriteV(uint8_t* base, uint32_t a, V v) {
  WriteF32(base, a, v.x);
  WriteF32(base, a + 4, v.y);
  WriteF32(base, a + 8, v.z);
}

// ---- look (14 values, as WML set_look)
enum Look { kSharpen, kBloom, kBloomThreshold, kExposure, kContrast, kSaturation, kVibrance, kFilmic,
            kTemperature, kTint, kVignette, kBlackLevel, kFxaa, kAniso, kLookCount };
const float kNeutral[kLookCount] = {0, 0, 0.7f, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0};

struct Preset {
  const char* name;
  // value to set, or NAN = keep the base
  float v[kLookCount];
};
#define K NAN
const Preset kPresets[] = {
    {"None", {K, K, K, K, K, K, K, K, K, K, K, K, K, K}},
    {"Black & White", {K, K, K, K, 1.08f, 0.0f, 0.0f, K, 0.0f, 0.0f, K, K, K, K}},
    {"Noir", {0.5f, K, K, -0.15f, 1.35f, 0.0f, 0.0f, 0.4f, 0.0f, 0.0f, 0.55f, 0.04f, K, K}},
    {"Sepia", {K, K, K, K, 0.95f, 0.15f, 0.0f, K, 0.55f, 0.08f, 0.3f, 0.03f, K, K}},
    {"Vivid", {K, K, K, K, 1.12f, 1.45f, 0.5f, K, K, K, K, K, K, K}},
    {"Cinematic", {K, 0.3f, K, K, 1.12f, 0.85f, 0.1f, 0.8f, -0.12f, 0.04f, 0.35f, K, K, K}},
    {"Warm", {K, K, K, K, K, 1.1f, K, K, 0.35f, 0.03f, 0.15f, K, K, K}},
    {"Cold", {K, K, K, K, 1.05f, 0.95f, K, K, -0.35f, -0.03f, 0.15f, K, K, K}},
    {"Faded", {K, K, K, 0.05f, 0.82f, 0.75f, K, K, K, K, 0.2f, 0.08f, K, K}},
    {"Dreamy", {0.0f, 1.2f, 0.5f, 0.1f, 0.92f, 0.9f, 0.2f, 0.3f, 0.1f, 0.06f, 0.3f, 0.02f, K, K}},
};
#undef K
constexpr int kPresetCount = int(sizeof(kPresets) / sizeof(kPresets[0]));

// ---- panel rows
enum RowId { kRowFov, kRowRoll, kRowRange, kRowFilter, kRowExposure, kRowContrast, kRowSaturation,
             kRowTemperature, kRowTint, kRowVignette, kRowBloom, kRowSharpen, kRowFade, kRowReset, kRowCount };
struct RowDef {
  const char* name;
  float min, max, step;
  int look;  // -1 = not a look value
};
const RowDef kRows[kRowCount] = {
    {"Field of view", 15, 110, 1, -1},  {"Roll", -45, 45, 1, -1},
    {"Camera range", 2, 25, 0.5f, -1},  {"Filter", 0, float(kPresetCount - 1), 1, -1},
    {"Exposure", -2, 2, 0.05f, kExposure}, {"Contrast", 0.5f, 1.5f, 0.02f, kContrast},
    {"Saturation", 0, 2, 0.05f, kSaturation}, {"Temperature", -1, 1, 0.05f, kTemperature},
    {"Tint", -1, 1, 0.05f, kTint},      {"Vignette", 0, 1, 0.05f, kVignette},
    {"Bloom", 0, 2, 0.05f, kBloom},     {"Sharpen", 0, 1, 0.05f, kSharpen},
    {"Fade", 0, 0.2f, 0.01f, kBlackLevel}, {"Reset all", 0, 0, 0, -1},
};

// ---- state
std::mutex g_mutex;  // input, panel snapshot
std::atomic<bool> g_active{false};
std::atomic<bool> g_pending{false};
std::atomic<bool> g_menu_gone{false};
std::function<void(bool)> g_panel_listener;  // the pause menu's closing (co-op menu Start pulse) is over
std::chrono::steady_clock::time_point g_pending_since{};

struct Pad {
  uint16_t buttons = 0;
  float lt = 0, rt = 0, lx = 0, ly = 0, rx = 0, ry = 0;
  double mdx = 0, mdy = 0;
  int wheel = 0;
} g_pad;

V g_pos, g_start_pos;
float g_yaw = 0, g_pitch = 0;
float g_fov = 70, g_fov_game = 70, g_roll = 0, g_range = 8;
int g_preset = 0;
float g_look[kLookCount], g_base_look[kLookCount];
bool g_base_from_mod = false;
uint8_t g_saved_hud = 1;
float g_saved_divisor = 1.0f;
uint8_t g_saved_fixed_flag = 0;
float g_saved_fixed_rate = 30.0f;
double g_saved_scalar = 1.0;
bool g_show = true;
int g_sel = 0;
uint16_t g_prev_buttons = 0;
bool g_prev_keys[256] = {};
double g_repeat_at[4] = {};
std::chrono::steady_clock::time_point g_tick{};
std::string g_notice;
std::chrono::steady_clock::time_point g_notice_until{}, g_flash_at{};
bool g_capture_wait = false;
std::chrono::steady_clock::time_point g_capture_since{};
bool g_keyboard_used = false;

double Now() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// photo_mode.log next to the logs (dist): written and closed line by line, so the last step before a
// crash is on disk.
void Trace(const char* fmt, ...) {
  FILE* f = std::fopen("photo_mode.log", "ab");
  if (!f) return;
  SYSTEMTIME st;
  GetLocalTime(&st);
  std::fprintf(f, "%02d:%02d:%02d.%03d ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
  va_list ap;
  va_start(ap, fmt);
  std::vfprintf(f, fmt, ap);
  va_end(ap);
  std::fputc('\n', f);
  std::fclose(f);
}
int g_trace_frames = 0;  // frames traced after starting

void Notice(const std::string& s, double seconds = 3.0) {
  g_notice = s;
  g_notice_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(int(seconds * 1000));
}

void SetAudioPaused(bool pause) {
  static bool paused = false;
  if (paused == pause) return;
  paused = pause;
  auto* runtime = REX_KERNEL_STATE() ? REX_KERNEL_STATE()->emulator() : nullptr;
  auto* audio = runtime ? static_cast<rex::audio::AudioSystem*>(runtime->audio_system()) : nullptr;
  if (!audio) return;
  // Pause waits for the audio thread: never block the game thread on it.
  std::thread([audio, pause] {
    if (pause) audio->Pause(); else audio->Resume();
  }).detach();
}

template <class F>
F GpuFunction(const char* name) {
  HMODULE gpu = GetModuleHandleW(L"rexgpu-xenos.dll");
  return gpu ? reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress(gpu, name))) : nullptr;
}
void SendLook(const float* v) {
  static auto fn = GpuFunction<void (*)(const float*, int)>("RexLookSettings");
  if (fn) fn(v, v ? int(kLookCount) : 0);
}

void ApplyPreset(int index) {
  g_preset = std::clamp(index, 0, kPresetCount - 1);
  for (int i = 0; i < kLookCount; ++i) {
    const float p = kPresets[g_preset].v[i];
    g_look[i] = std::isnan(p) ? g_base_look[i] : p;
  }
}

float RowValue(int r) {
  switch (r) {
    case kRowFov: return g_fov;
    case kRowRoll: return g_roll;
    case kRowRange: return g_range;
    case kRowFilter: return float(g_preset);
    default: return kRows[r].look >= 0 ? g_look[kRows[r].look] : 0.0f;
  }
}
void SetRowValue(int r, float v) {
  const RowDef& d = kRows[r];
  v = std::clamp(v, d.min, d.max);
  switch (r) {
    case kRowFov: g_fov = v; break;
    case kRowRoll: g_roll = v; break;
    case kRowRange: g_range = v; break;
    case kRowFilter: ApplyPreset(int(std::lround(v))); break;
    default:
      if (d.look >= 0) g_look[d.look] = v;
  }
}
void ResetAll() {
  g_fov = g_fov_game;
  g_roll = 0;
  g_range = 8;
  ApplyPreset(0);
}

// ---- PNG (WIC), on a worker thread
std::wstring PhotoFolder() {
  PWSTR pictures = nullptr;
  std::wstring dir;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &pictures)) && pictures) dir = pictures;
  if (pictures) CoTaskMemFree(pictures);
  if (dir.empty()) dir = L".";
  dir += L"\\Saints Reborn";
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  return dir;
}

bool WritePng(const std::wstring& path, const std::vector<uint8_t>& bgr, int w, int h) {
  const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  bool ok = false;
  IWICImagingFactory* factory = nullptr;
  IWICStream* stream = nullptr;
  IWICBitmapEncoder* encoder = nullptr;
  IWICBitmapFrameEncode* frame = nullptr;
  IPropertyBag2* props = nullptr;
  do {
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) break;
    if (FAILED(factory->CreateStream(&stream))) break;
    if (FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) break;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder))) break;
    if (FAILED(encoder->Initialize(stream, WICBitmapEncoderNoCache))) break;
    if (FAILED(encoder->CreateNewFrame(&frame, &props))) break;
    if (FAILED(frame->Initialize(props))) break;
    if (FAILED(frame->SetSize(UINT(w), UINT(h)))) break;
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    if (FAILED(frame->SetPixelFormat(&format)) || format != GUID_WICPixelFormat24bppBGR) break;
    const UINT stride = UINT(w) * 3;
    if (FAILED(frame->WritePixels(UINT(h), stride, stride * UINT(h), const_cast<BYTE*>(bgr.data())))) break;
    if (FAILED(frame->Commit()) || FAILED(encoder->Commit())) break;
    ok = true;
  } while (false);
  if (props) props->Release();
  if (frame) frame->Release();
  if (encoder) encoder->Release();
  if (stream) stream->Release();
  if (factory) factory->Release();
  if (SUCCEEDED(co)) CoUninitialize();
  return ok;
}

std::mutex g_saved_mutex;
std::string g_saved_name;  // set by the writer thread
bool g_saved_ok = false, g_saved_new = false;

void Sink(const uint8_t* rgba, int w, int h, void*) {
  if (!rgba || w <= 0 || h <= 0) return;
  auto bgr = std::make_shared<std::vector<uint8_t>>(size_t(w) * size_t(h) * 3);
  for (size_t i = 0, n = size_t(w) * size_t(h); i < n; ++i) {
    (*bgr)[i * 3 + 0] = rgba[i * 4 + 2];
    (*bgr)[i * 3 + 1] = rgba[i * 4 + 1];
    (*bgr)[i * 3 + 2] = rgba[i * 4 + 0];
  }
  SYSTEMTIME st;
  GetLocalTime(&st);
  wchar_t name[64];
  std::swprintf(name, 64, L"SaintsRow_%04d-%02d-%02d_%02d-%02d-%02d.png", st.wYear, st.wMonth, st.wDay, st.wHour,
                st.wMinute, st.wSecond);
  std::thread([bgr, w, h, file = std::wstring(name)] {
    const std::wstring dir = PhotoFolder();
    std::wstring path = dir + L"\\" + file;
    for (int n = 2; std::filesystem::exists(path) && n < 100; ++n)
      path = dir + L"\\" + file.substr(0, file.size() - 4) + L"_" + std::to_wstring(n) + L".png";
    const bool ok = WritePng(path, *bgr, w, h);
    std::string utf8(path.size() * 3 + 1, '\0');
    utf8.resize(size_t(std::max(0, WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, utf8.data(), int(utf8.size()), nullptr, nullptr) - 1)));
    REXLOG_INFO("Photo mode: {} {} ({}x{})", ok ? "saved" : "COULD NOT SAVE", utf8, w, h);
    std::lock_guard<std::mutex> lock(g_saved_mutex);
    g_saved_name = std::filesystem::path(path).filename().string();
    g_saved_ok = ok;
    g_saved_new = true;
  }).detach();
}

// ---- start / stop
bool PlayerOk(uint8_t* base) {
  const uint32_t p = PPC_LOAD_U32(kPlayerPointer);
  return p && PPC_LOAD_U32(p + 72) == 1;
}
bool Blocked(uint8_t* base) {
  return !PlayerOk(base) || PPC_LOAD_U8(kCutscene) || PPC_LOAD_U8(kScriptedCutscene);
}

void Start(uint8_t* base) {
  Trace("start: begin");
  g_pos = g_start_pos = ReadV(base, kCamera + 44);
  V f = ReadV(base, kCamera + 104);
  if (!Finite(f) || Len(f) < 0.5f) f = {0, 0, 1};
  g_pitch = std::asin(std::clamp(f.y, -1.0f, 1.0f));
  g_yaw = std::atan2(f.x, f.z);
  const float fov = ReadF32(base, kCamera + 188);
  g_fov_game = std::isfinite(fov) && fov > 10 && fov < 150 ? fov : 70.0f;
  g_fov = g_fov_game;
  g_roll = 0;
  g_range = std::clamp(g_range, 2.0f, 25.0f);
  Trace("start: camera %.1f %.1f %.1f fov %.1f", g_pos.x, g_pos.y, g_pos.z, g_fov_game);
  g_base_from_mod = wml::LastLook(g_base_look);
  if (!g_base_from_mod) std::memcpy(g_base_look, kNeutral, sizeof(g_base_look));
  ApplyPreset(0);
  g_saved_hud = PPC_LOAD_U8(kHudVisible);
  const float d = ReadF32(base, kTimeDivisor);
  g_saved_divisor = std::isfinite(d) && d > 0 && d < 100 ? d : 1.0f;
  g_saved_fixed_flag = PPC_LOAD_U8(kFixedStepFlag);
  const float rate = ReadF32(base, kFixedStepRate);
  g_saved_fixed_rate = std::isfinite(rate) && rate > 0 && rate < 100000 ? rate : 30.0f;
  g_saved_scalar = rex::chrono::Clock::guest_time_scalar();
  if (!(g_saved_scalar > 0.0) || g_saved_scalar > 100.0) g_saved_scalar = 1.0;
  Trace("start: saved divisor %.3f fixed %d rate %.1f scalar %.4f, look from %s", g_saved_divisor, int(g_saved_fixed_flag),
        g_saved_fixed_rate, g_saved_scalar, g_base_from_mod ? "a mod" : "neutral");
  SetAudioPaused(true);
  Trace("start: audio pause asked");
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_prev_buttons = g_pad.buttons;
    for (int k = 0; k < 256; ++k) g_prev_keys[k] = (GetAsyncKeyState(k) & 0x8000) != 0;
    g_pad.mdx = g_pad.mdy = 0;
  }
  g_show = true;
  g_sel = 0;
  g_tick = {};
  g_capture_wait = false;
  g_trace_frames = 0;
  g_active.store(true);
  if (g_panel_listener) g_panel_listener(true);
  Trace("start: active");
  Notice("Photo mode", 1.5);
  REXLOG_INFO("Photo mode: on (camera {:.1f} {:.1f} {:.1f}, fov {:.0f}, look from {})", g_pos.x, g_pos.y, g_pos.z,
              g_fov_game, g_base_from_mod ? "a mod" : "neutral");
}

void Stop(uint8_t* base, const char* why) {
  if (!g_active.exchange(false)) return;
  WriteF32(base, kTimeDivisor, g_saved_divisor);
  PPC_STORE_U8(kFixedStepFlag, g_saved_fixed_flag);
  WriteF32(base, kFixedStepRate, g_saved_fixed_rate);
  rex::chrono::Clock::set_guest_time_scalar(g_saved_scalar);
  PPC_STORE_U8(kHudVisible, g_saved_hud);
  WriteF32(base, kCamera + 188, g_fov_game);
  SetAudioPaused(false);
  // Back to the mods' look (or none).
  float last[kLookCount];
  if (wml::LastLook(last)) SendLook(last); else SendLook(nullptr);
  if (g_panel_listener) g_panel_listener(false);
  REXLOG_INFO("Photo mode: off ({})", why);
  Trace("stop: %s", why);
}

// ---- the camera
void Basis(V& forward, V& right, V& up) {
  const float cp = std::cos(g_pitch), sp = std::sin(g_pitch), cy = std::cos(g_yaw), sy = std::sin(g_yaw);
  forward = {sy * cp, sp, cy * cp};
  right = {cy, 0, -sy};
  up = {-sy * sp, cp, -cy * sp};
  if (g_roll != 0) {
    const float r = g_roll * kPi / 180.0f, c = std::cos(r), s = std::sin(r);
    const V r2 = right * c + up * s, u2 = up * c - right * s;
    right = r2;
    up = u2;
  }
}
void WriteCamera(uint8_t* base) {
  V forward, right, up;
  Basis(forward, right, up);
  WriteV(base, kCamera + 44, g_pos);
  WriteV(base, kCamera + 56, g_pos);
  WriteV(base, kCamera + 80, right);
  WriteV(base, kCamera + 92, up);
  WriteV(base, kCamera + 104, forward);
  WriteF32(base, kCamera + 188, g_fov);
}

bool KeyDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

void Update(uint8_t* base) {
  const auto now = std::chrono::steady_clock::now();
  float dt = g_tick.time_since_epoch().count() ? std::chrono::duration<float>(now - g_tick).count() : 0.0f;
  g_tick = now;
  dt = std::clamp(dt, 0.0f, 0.05f);
  Pad pad;
  bool keys[256];
  uint16_t prev;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    pad = g_pad;
    g_pad.mdx = g_pad.mdy = 0;
    g_pad.wheel = 0;
    prev = g_prev_buttons;
    g_prev_buttons = pad.buttons;
  }
  const bool focus = [] {
    HWND w = GetForegroundWindow();
    DWORD pid = 0;
    if (w) GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId();
  }();
  for (int k = 0; k < 256; ++k) keys[k] = focus && KeyDown(k);
  auto key_pressed = [&](int vk) { return keys[vk] && !g_prev_keys[vk]; };
  auto pressed = [&](uint16_t b) { return (pad.buttons & b) && !(prev & b); };
  const bool any_key = std::any_of(std::begin(keys), std::end(keys), [](bool b) { return b; });
  if (any_key || pad.mdx != 0 || pad.mdy != 0) g_keyboard_used = true;
  if (pad.buttons || std::fabs(pad.lx) + std::fabs(pad.ly) + std::fabs(pad.rx) + std::fabs(pad.ry) > 0.3f) g_keyboard_used = false;

  // Leave: B / Start / Esc / Backspace.
  if (pressed(kB) || pressed(kStart) || pressed(kBack) || key_pressed(VK_ESCAPE) || key_pressed(VK_BACK)) {
    std::memcpy(g_prev_keys, keys, sizeof(keys));
    Stop(base, "left by the player");
    return;
  }
  // Panel: Y / H shows or hides it.
  if (pressed(kY) || key_pressed('H')) {
    g_show = !g_show;
    Trace("panel %s (%s)", g_show ? "shown" : "hidden", pressed(kY) ? "Y" : "H");
  }
  // Choose a row (up / down, with repeat), change it (left / right, with repeat).
  const double t = Now();
  auto repeat = [&](int i, bool down, bool edge) {
    if (!down) { g_repeat_at[i] = 0; return false; }
    if (edge) { g_repeat_at[i] = t + 0.35; return true; }
    if (g_repeat_at[i] > 0 && t >= g_repeat_at[i]) { g_repeat_at[i] = t + 0.06; return true; }
    return false;
  };
  const bool up_d = (pad.buttons & kUp) || keys[VK_UP], down_d = (pad.buttons & kDown) || keys[VK_DOWN];
  const bool left_d = (pad.buttons & kLeft) || keys[VK_LEFT], right_d = (pad.buttons & kRight) || keys[VK_RIGHT];
  const bool step_up = repeat(0, up_d, pressed(kUp) || key_pressed(VK_UP));
  const bool step_down = repeat(1, down_d, pressed(kDown) || key_pressed(VK_DOWN));
  const bool step_left = repeat(2, left_d, pressed(kLeft) || key_pressed(VK_LEFT));
  const bool step_right = repeat(3, right_d, pressed(kRight) || key_pressed(VK_RIGHT));
  if (g_show) {
    if (step_up) g_sel = (g_sel + kRowCount - 1) % kRowCount;
    if (step_down) g_sel = (g_sel + 1) % kRowCount;
    if ((step_left || step_right) && g_sel != kRowReset) {
      const RowDef& d = kRows[g_sel];
      const float fine = (keys[VK_SHIFT] || (pad.buttons & kX)) ? 4.0f : 1.0f;
      SetRowValue(g_sel, RowValue(g_sel) + (step_right ? d.step : -d.step) * fine);
    }
    if (g_sel != kRowReset && g_sel != kRowFilter) {
      // mouse wheel changes the chosen value too
      if (pad.wheel) SetRowValue(g_sel, RowValue(g_sel) + kRows[g_sel].step * float(pad.wheel));
    }
  }
  if (key_pressed('R')) { ResetAll(); Notice("Reset", 1.0); }
  // A / Enter / Space: photo (on the Reset row A resets instead).
  const bool shoot = pressed(kA) || key_pressed(VK_RETURN) || key_pressed(VK_SPACE) || key_pressed(VK_F12);
  if (shoot && g_show && g_sel == kRowReset && pressed(kA)) {
    ResetAll();
    Notice("Reset", 1.0);
  } else if (shoot && !g_capture_wait) {
    static auto request = GpuFunction<void (*)()>("RexCaptureRequest");
    if (request) {
      request();
      g_capture_wait = true;
      g_capture_since = now;
      g_flash_at = now;
    } else {
      Notice("Photos need the updated rexgpu-xenos.dll", 3.0);
    }
  }
  std::memcpy(g_prev_keys, keys, sizeof(keys));

  // Fly: LS / WASD move (along the view), LT / RT or Q / E down / up, RS / mouse look.
  // LS click / Shift faster, Ctrl slower.
  V forward, right, up;
  g_roll = std::clamp(g_roll, -45.0f, 45.0f);
  const float saved_roll = g_roll;
  g_roll = 0;
  Basis(forward, right, up);
  g_roll = saved_roll;
  float mx = pad.lx, mz = pad.ly, my = pad.rt - pad.lt;
  if (keys['W']) mz += 1;
  if (keys['S']) mz -= 1;
  if (keys['D']) mx += 1;
  if (keys['A']) mx -= 1;
  if (keys['E']) my += 1;
  if (keys['Q']) my -= 1;
  float speed = 3.0f;
  if ((pad.buttons & kLS) || keys[VK_SHIFT]) speed *= 3.0f;
  if (keys[VK_CONTROL]) speed *= 0.25f;
  g_pos = g_pos + (forward * mz + right * mx) * (speed * dt) + V{0, my * speed * dt, 0};
  // Look: the right stick turns faster at the edge; the mouse 0.0022 rad per pixel.
  const float zoom = g_fov / 70.0f;  // slower turning when zoomed in
  g_yaw += (pad.rx * 1.8f * dt + float(pad.mdx) * 0.0022f) * zoom;
  g_pitch += (pad.ry * 1.4f * dt - float(pad.mdy) * 0.0022f) * zoom;
  g_pitch = std::clamp(g_pitch, -1.50f, 1.50f);
  // Range: within g_range of the player (chest height).
  const uint32_t player = PPC_LOAD_U32(kPlayerPointer);
  if (player) {
    const V center = ReadV(base, player + 20) + V{0, 1.0f, 0};
    if (Finite(center)) {
      V off = g_pos - center;
      const float l = Len(off);
      if (l > g_range && l > 0) g_pos = center + off * (g_range / l);
    }
  }
  if (!Finite(g_pos)) g_pos = g_start_pos;

  // The photo has been copied: write it out.
  if (g_capture_wait) {
    static auto take = GpuFunction<int (*)(void (*)(const uint8_t*, int, int, void*), void*)>("RexCaptureTake");
    if (take && take(Sink, nullptr)) {
      g_capture_wait = false;
      Notice("Saving photo...", 2.0);
    } else if (now - g_capture_since > std::chrono::seconds(3)) {
      g_capture_wait = false;
      Notice("The photo could not be taken", 3.0);
    }
  }
  {
    std::lock_guard<std::mutex> lock(g_saved_mutex);
    if (g_saved_new) {
      g_saved_new = false;
      Notice(g_saved_ok ? "Saved to Pictures\\Saints Reborn: " + g_saved_name : "Could not save the photo", 4.0);
    }
  }
}

}  // namespace

void BuildPanel();

void SetPanelListener(std::function<void(bool)> listener) { g_panel_listener = std::move(listener); }

void MenuGone() {
  Trace("pause menu: closed for good");
  g_menu_gone.store(true);
}

void RequestStart() {
  Trace("pause menu: Photo Mode picked");
  g_menu_gone.store(false);
  g_pending.store(true);
  g_pending_since = std::chrono::steady_clock::now();
}
bool Active() { return g_active.load(); }
bool Pending() { return g_pending.load(); }

void Input(uint16_t buttons, uint8_t lt, uint8_t rt, int16_t lx, int16_t ly, int16_t rx, int16_t ry, double mouse_dx,
           double mouse_dy, int wheel) {
  auto stick = [](int16_t v) {
    const float s = float(v) / 32767.0f;
    return std::fabs(s) < 0.2f ? 0.0f : (s - (s > 0 ? 0.2f : -0.2f)) / 0.8f;
  };
  std::lock_guard<std::mutex> lock(g_mutex);
  g_pad.buttons = buttons;
  g_pad.lt = lt / 255.0f;
  g_pad.rt = rt / 255.0f;
  g_pad.lx = stick(lx);
  g_pad.ly = stick(ly);
  g_pad.rx = stick(rx);
  g_pad.ry = stick(ry);
  g_pad.mdx += mouse_dx;
  g_pad.mdy += mouse_dy;
  g_pad.wheel += wheel;
}

// Every frame (from present: the main loop hook sub_82185498 runs only once, when the loop starts).
void Step(uint8_t* base) {
  if (g_pending.load()) {
    // The pause menu is closing: start when no menu is open (or give up after 6 s).
    const auto waited = std::chrono::steady_clock::now() - g_pending_since;
    const bool menu = PPC_LOAD_U32(kActiveScreen) != 0;
    // Only after the co-op menu's Start pulse that closes the pause menu is over (coop_menu.cpp calls
    // MenuGone): starting during it gave the game another Start = its PAUSED screen over the photo.
    if (!menu && !Blocked(base) && g_menu_gone.load()) {
      g_pending.store(false);
      Trace("menu closed: starting");
      Start(base);
    } else if (waited > std::chrono::seconds(6)) {
      g_pending.store(false);
      REXLOG_INFO("Photo mode: not started (menu {}, blocked {})", menu, Blocked(base));
    }
  }
  if (!g_active.load()) return;
  if (Blocked(base) || PPC_LOAD_U32(kActiveScreen) != 0) {
    Stop(base, Blocked(base) ? "cutscene / no player" : "a menu opened");
    return;
  }
  if (g_trace_frames < 6) Trace("frame %d: freeze", g_trace_frames);
  WriteF32(base, kTimeDivisor, 100000.0f);  // game time stands still
  PPC_STORE_U8(kFixedStepFlag, 1);
  WriteF32(base, kFixedStepRate, 1000000.0f);
  rex::chrono::Clock::set_guest_time_scalar(0.0001);
  PPC_STORE_U8(kHudVisible, 0);
  WriteCamera(base);
}

void BeforeGameFrame(uint8_t* base) { Step(base); }

void OnPresent(uint8_t* base) {
  Step(base);
  if (!g_active.load()) return;
  const bool tr = g_trace_frames < 6;
  if (tr) Trace("frame %d: update", g_trace_frames);
  Update(base);
  if (!g_active.load()) return;
  if (tr) Trace("frame %d: look", g_trace_frames);
  SendLook(g_look);
  PPC_STORE_U8(kHudVisible, 0);
  WriteCamera(base);
  if (tr) Trace("frame %d: panel", g_trace_frames);
  BuildPanel();
  if (tr) Trace("frame %d: done", g_trace_frames);
  ++g_trace_frames;
}

void AfterCamera(uint8_t* base) {
  if (g_active.load()) WriteCamera(base);
}

std::mutex g_panel_mutex;
Panel g_panel;

// Game thread, every present: the panel for the overlay (drawn on another thread).
void BuildPanel() {
  const auto now = std::chrono::steady_clock::now();
  Panel out;
  out.show = g_show;
  out.selected = g_sel;
  out.keyboard = g_keyboard_used;
  char buf[64];
  for (int r = 0; r < kRowCount; ++r) {
    PanelRow row;
    row.name = kRows[r].name;
    const RowDef& d = kRows[r];
    const float v = RowValue(r);
    if (r == kRowReset) {
      row.action = true;
    } else if (r == kRowFilter) {
      row.value = kPresets[g_preset].name;
    } else {
      if (r == kRowFov) std::snprintf(buf, sizeof(buf), "%.0f\xC2\xB0", v);
      else if (r == kRowRoll) std::snprintf(buf, sizeof(buf), "%+.0f\xC2\xB0", v);
      else if (r == kRowRange) std::snprintf(buf, sizeof(buf), "%.1f m", v);
      else std::snprintf(buf, sizeof(buf), "%+.2f", v);
      row.value = buf;
      row.fill = d.max > d.min ? (v - d.min) / (d.max - d.min) : -1.0f;
    }
    out.rows.push_back(row);
  }
  if (now < g_notice_until) {
    out.notice = g_notice;
    out.notice_alpha = std::min(1.0f, std::chrono::duration<float>(g_notice_until - now).count() / 0.4f);
  }
  const float since = std::chrono::duration<float>(now - g_flash_at).count();
  out.flash = g_flash_at.time_since_epoch().count() && since < 0.35f ? 1.0f - since / 0.35f : 0.0f;
  std::lock_guard<std::mutex> lock(g_panel_mutex);
  g_panel = std::move(out);
}

bool GetPanel(Panel& out) {
  if (!g_active.load()) return false;
  static int traced = 0;
  if (traced < 3 && g_trace_frames < 6) { ++traced; Trace("overlay: drawing the panel"); }
  std::lock_guard<std::mutex> lock(g_panel_mutex);
  out = g_panel;
  return true;
}

}  // namespace sr::photo

// The game's camera manager (it runs the follow camera and writes the camera block): skipped while photo
// mode is on, so the photo camera is the only camera all frame. Before, the game camera was in the block
// part of the frame: the skateboard (drawn from camera hooks) jumped between two places and lights
// (their visibility tests) flickered.
extern "C" void __imp__sub_82109608(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_82109608) {
  if (sr::photo::Active()) {
    sr::photo::AfterCamera(base);
    return;
  }
  __imp__sub_82109608(ctx, base);
}
