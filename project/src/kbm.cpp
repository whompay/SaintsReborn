// Keyboard and mouse controls, laid out like Saints Row 2 on PC.
//
// The game only knows the Xbox 360 controller, so keys are turned into
// controller input on top of whatever a real controller reports (a real
// controller works exactly as before). Some keys mean different things on
// foot and in a vehicle, the way SR2 does it (W walks forward on foot and
// accelerates in a car); the game's own "is the player in a vehicle" check is
// read from memory to tell which. The mouse turns the camera directly (see
// the camera hook at the end), and picks items in the radial menu.
//
// On foot                          In a vehicle
//   WASD      move                   W / S     accelerate / brake, reverse
//   Mouse     camera                 A / D     steer
//   LMB       primary attack         LMB       drive-by attack
//   RMB       secondary attack       Space     handbrake
//   Space     jump                   Shift     nitrous
//   Shift     sprint                 Ctrl      hydraulics
//   E         action / enter car     E         exit car
//   R         reload / grab weapon   Z / C     look left / right
//   F         kick                   X         look back
//   C         crouch
//   MMB / V   right stick click
// Everywhere
//   Q (hold)  radial menu (point at an item with the mouse or WASD)
//   Esc / M   pause / map            Tab       back
//   Arrows    D-pad (recruit, cancel activity, radio / audio track, taunt)
//   Enter     A (menus)              Backspace B (menus)
// In-game pause menu only
//   WASD / Mouse  pan map             LMB        set/remove waypoint
//   Wheel          zoom map            Q / E      LT / RT tabs
//   Y / X          Y / X (resume, ...)
// Player creation only
//   Mouse           rotate / zoom       LMB        A / select
//   Wheel           zoom                Q / E      LT / RT tabs

#include "saintsrow_config.h"
#include "saintsrow_init.h"

#include "kbm.h"
#include "glyphs.h"
#include "wml/mod_loader.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>

#include <rex/input/input.h>
#include <rex/logging.h>
#include <rex/ppc/function.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace rex::kernel::xam {
extern uint32_t XamInputGetState_entry(uint32_t user_index, uint32_t flags,
                                       ppc_ptr_t<rex::input::X_INPUT_STATE> input_state);
}

namespace {

using namespace rex::input;

#ifdef _WIN32
bool GameHasFocus() {
  HWND foreground = GetForegroundWindow();
  if (!foreground) return false;
  DWORD pid = 0;
  GetWindowThreadProcessId(foreground, &pid);
  return pid == GetCurrentProcessId();
}

// GetAsyncKeyState is a system call and the game polls the controller many
// times per frame, so the keys are read at most every 2 ms.
bool g_key_down[256];
std::chrono::steady_clock::time_point g_keys_read{};

void RefreshKeys() {
  auto now = std::chrono::steady_clock::now();
  if (now - g_keys_read < std::chrono::milliseconds(2)) return;
  g_keys_read = now;
  static const int kKeys[] = {'Q', 'E', 'R', 'F', 'C', 'V', 'M', 'W', 'A', 'S', 'D', 'Y', 'Z', 'X',
                              VK_ESCAPE, VK_TAB, VK_RETURN, VK_BACK, VK_UP, VK_DOWN, VK_LEFT,
                              VK_RIGHT, VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_SPACE, VK_SHIFT,
                              VK_CONTROL};
  for (int vk : kKeys) g_key_down[vk] = (GetAsyncKeyState(vk) & 0x8000) != 0;
}

bool Down(int vk) { return (g_key_down[vk & 0xFF] && !wml::KeyTaken(vk)) || wml::KeyForced(vk); }
#endif

std::atomic<double> g_sensitivity{1.0};

// ---------------------------------------------------------------------------
// Mouse
// ---------------------------------------------------------------------------
//
// While the game has focus the cursor is held in the middle of the window and
// its movement is collected here. The camera takes it directly (see the
// sub_8210D518 hook below); only if that camera isn't running (other camera
// modes) is the movement turned into right stick input instead.

struct MouseState {
  bool captured = false;
  double dx = 0, dy = 0;  // movement not yet used by the camera
  std::chrono::steady_clock::time_point last_poll{};
  std::chrono::steady_clock::time_point last_camera{};
  double vx = 0, vy = 0;  // smoothed speed for the stick fallback
  int wheel_y = 0;
  std::chrono::steady_clock::time_point wheel_until{};
  // Radial menu: where the mouse points, relative to where it was opened.
  bool radial_open = false;
  double radial_x = 0, radial_y = 0;
};
MouseState g_mouse;
std::mutex g_mouse_mutex;
std::atomic<bool> g_mouse_suspended{false};  // e.g. while the display menu is open
bool g_pause_menu_active = false;
bool g_pause_toggle_down = false;
std::chrono::steady_clock::time_point g_pause_toggled{};

#ifdef _WIN32
void ReleaseMouseLocked() {
  if (g_mouse.captured) {
    ClipCursor(nullptr);
    g_mouse.captured = false;
  }
  g_mouse.dx = g_mouse.dy = 0;
  g_mouse.vx = g_mouse.vy = 0;
  g_mouse.wheel_y = 0;
  g_mouse.wheel_until = {};
}

// Collects cursor movement since the last call and puts the cursor back in the
// middle of the game window. Returns the movement in pixels.
void PollMouseLocked(double& dx, double& dy) {
  dx = dy = 0;
  if (g_mouse_suspended.load(std::memory_order_relaxed)) {
    ReleaseMouseLocked();
    return;
  }
  HWND window = GetForegroundWindow();
  RECT client;
  if (!window || !GetClientRect(window, &client)) {
    ReleaseMouseLocked();
    return;
  }
  POINT top_left{client.left, client.top}, bottom_right{client.right, client.bottom};
  ClientToScreen(window, &top_left);
  ClientToScreen(window, &bottom_right);
  RECT screen{top_left.x, top_left.y, bottom_right.x, bottom_right.y};
  POINT center{(screen.left + screen.right) / 2, (screen.top + screen.bottom) / 2};
  POINT cursor;
  GetCursorPos(&cursor);
  ClipCursor(&screen);  // cheap, and follows window moves / resizes
  if (!g_mouse.captured) {
    g_mouse.captured = true;
    SetCursorPos(center.x, center.y);
    return;
  }
  dx = double(cursor.x - center.x);
  dy = double(cursor.y - center.y);
  if (dx != 0 || dy != 0) SetCursorPos(center.x, center.y);
}
#endif

// Right stick position for mouse speed (only used when the camera hook isn't
// running): past the stick dead zone, then proportional to speed.
void MouseToStickLocked(double dx, double dy, double dt, int& rx, int& ry) {
  dt = std::clamp(dt, 0.001, 0.1);
  double a = 1.0 - std::exp(-dt / 0.03);
  g_mouse.vx += (dx / dt - g_mouse.vx) * a;
  g_mouse.vy += (dy / dt - g_mouse.vy) * a;
  const double sensitivity = g_sensitivity.load(std::memory_order_relaxed);
  auto to_stick = [&](double v) -> int {
    double speed = std::fabs(v) * sensitivity;
    if (speed < 8.0) return 0;
    constexpr double kDeadZone = 8700.0;
    double m = kDeadZone + (32767.0 - kDeadZone) * std::min(1.0, speed / 800.0);
    return int(v < 0 ? -m : m);
  };
  rx = to_stick(g_mouse.vx);
  ry = to_stick(-g_mouse.vy);
}

float LoadF32(uint8_t* base, uint32_t address) {
  uint32_t bits = PPC_LOAD_U32(address);
  float value;
  std::memcpy(&value, &bits, 4);
  return value;
}
void StoreF32(uint8_t* base, uint32_t address, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, 4);
  PPC_STORE_U32(address, bits);
}

// The game's own test (script function is_player_in_vehicle, 0x824D0A50 ->
// sub_82448010), done with plain memory reads.
bool PlayerInVehicle(uint8_t* base) {
  const uint32_t player = PPC_LOAD_U32(0x8309ABECu);
  if (!player) return false;
  if (PPC_LOAD_U32(player + 3456) != 0) return false;
  const uint32_t vehicle_handle = PPC_LOAD_U32(player + 2496);
  // sub_82448590: getting in / out and similar transitions count as on foot.
  if (vehicle_handle && (PPC_LOAD_U8(player + 2569) & 0x10)) return false;
  const uint32_t state = PPC_LOAD_U32(player + 508);
  if (state == 9 || state == 12) return false;
  // sub_82569630: the handle must name a live vehicle object (type 5).
  if (!vehicle_handle) return false;
  const uint32_t index = vehicle_handle & 0xFFFF;
  if (index >= 4096) return false;
  const uint32_t object = PPC_LOAD_U32(0x830866C8u + 12 + index * 16);
  if (!object) return false;
  return PPC_LOAD_U32(object + 68) == vehicle_handle && PPC_LOAD_U32(object + 72) == 5;
}

// The character creator. Logged live across main menu, creator, gameplay
// and pause menu: 0x827B04D4 is 1 only in the creator (-1 in the front end,
// 0 in play), and 0x839E0DF8 is the active menu screen (0x82FFB7BC is the
// creator screen, 0x82FFB84C the pause menu, 0x82FFB6C0 the main menu).
// The customization slot (0x8309AD70 == 11) stays set during normal play, and
// the creator runs the gameplay camera, so neither of those can tell.
bool CharacterCreationInputActive(uint8_t* base) {
  return PPC_LOAD_U32(0x827B04D4u) == 1 || PPC_LOAD_U32(0x839E0DF8u) == 0x82FFB7BCu;
}

// Tagging (spraying a gang tag: rotate the left stick as shown). The HUD's
// "Press B again to cancel Tagging" prompt (sub_8216C560) tests the player's
// tagging spot at +3700, which is -1 when the player isn't tagging.
bool TaggingActive(uint8_t* base) {
  const uint32_t player = PPC_LOAD_U32(0x8309ABECu);
  return player != 0 && PPC_LOAD_U32(player + 3700) != 0xFFFFFFFFu;
}

struct Pad {
  uint16_t buttons = 0;
  uint8_t lt = 0, rt = 0;
  int lx = 0, ly = 0;
};

#ifdef _WIN32
// Track the in-game pause overlay from the Start-button edge that opens it.
// A live player separates it from the front end. If the overlay is dismissed
// by a menu action instead of Start, the returning gameplay camera clears the
// state below.
bool UpdatePauseMenuState(uint8_t* base, uint16_t controller_buttons) {
  const bool player_loaded = PPC_LOAD_U32(0x8309ABECu) != 0;
  // Inside the pause menu Esc is Back (B), so only M / Start close it here;
  // leaving through Back is caught by the gameplay camera returning.
  const bool toggle_down = (!g_pause_menu_active && Down(VK_ESCAPE)) || Down('M') ||
                           (controller_buttons & X_INPUT_GAMEPAD_START) != 0;
  if (!player_loaded) {
    g_pause_menu_active = false;
  } else if (toggle_down && !g_pause_toggle_down) {
    g_pause_menu_active = !g_pause_menu_active;
    g_pause_toggled = std::chrono::steady_clock::now();
  }
  g_pause_toggle_down = toggle_down;
  return g_pause_menu_active;
}

bool g_esc_released = false;

Pad ReadKeyboard(uint8_t* base, bool pause_menu, bool player_creation) {
  Pad p;
  // Game input is suspended while an interactive overlay (display menu) is
  // open: clicks and keys belong to the overlay, not the game.
  if (g_mouse_suspended.load(std::memory_order_relaxed)) {
    return p;
  }
  const bool in_vehicle = PlayerInVehicle(base);
  auto press = [&](bool down, uint16_t button) {
    if (down) p.buttons |= button;
  };

  // Everywhere.
  press(Down(VK_ESCAPE) || Down('M'), X_INPUT_GAMEPAD_START);
  press(Down(VK_TAB), X_INPUT_GAMEPAD_BACK);
  press(Down(VK_RETURN), X_INPUT_GAMEPAD_A);
  press(Down(VK_BACK), X_INPUT_GAMEPAD_B);
  press(Down(VK_UP), X_INPUT_GAMEPAD_DPAD_UP);
  press(Down(VK_DOWN), X_INPUT_GAMEPAD_DPAD_DOWN);
  press(Down(VK_LEFT), X_INPUT_GAMEPAD_DPAD_LEFT);
  press(Down(VK_RIGHT), X_INPUT_GAMEPAD_DPAD_RIGHT);

  // Front end (no player yet): X and Y keys for the menus' X / Y actions
  // (e.g. Select Device), matching the keyboard glyphs shown there.
  if (PPC_LOAD_U32(0x8309ABECu) == 0) {
    press(Down('X'), X_INPUT_GAMEPAD_X);
    press(Down('Y'), X_INPUT_GAMEPAD_Y);
  }

  const int left = Down('A') ? 1 : 0, right = Down('D') ? 1 : 0;
  p.lx = (right - left) * 32767;

  if (!pause_menu) g_esc_released = false;
  if (pause_menu) {
    // Esc is Back here (B), like Backspace; M still closes the menu.
    // (The Esc press that opened the menu must be released first.)
    if (!Down(VK_ESCAPE)) g_esc_released = true;
    if (Down(VK_ESCAPE) && g_esc_released && !Down('M')) p.buttons &= uint16_t(~X_INPUT_GAMEPAD_START);
    press(Down(VK_ESCAPE) && g_esc_released, X_INPUT_GAMEPAD_B);
    const int up = Down('W') ? 1 : 0, down = Down('S') ? 1 : 0;
    p.ly = (up - down) * 32767;
    press(Down(VK_LBUTTON), X_INPUT_GAMEPAD_A);
    press(Down('Y'), X_INPUT_GAMEPAD_Y);  // e.g. Resume
    press(Down('X'), X_INPUT_GAMEPAD_X);
    if (Down('Q')) p.lt = 0xFF;
    if (Down('E')) p.rt = 0xFF;
    return p;
  }

  if (player_creation) {
    // W/S and arrows: up/down. LMB: select. RMB, Tab, Backspace: back.
    press(Down('W'), X_INPUT_GAMEPAD_DPAD_UP);
    press(Down('S'), X_INPUT_GAMEPAD_DPAD_DOWN);
    press(Down(VK_LBUTTON), X_INPUT_GAMEPAD_A);
    press(Down(VK_RBUTTON) || Down(VK_TAB), X_INPUT_GAMEPAD_B);
    p.buttons &= uint16_t(~X_INPUT_GAMEPAD_BACK);
    press(Down('Y'), X_INPUT_GAMEPAD_Y);
    press(Down('X'), X_INPUT_GAMEPAD_X);  // randomize
    if (Down('Q')) p.lt = 0xFF;
    if (Down('E')) p.rt = 0xFF;
    return p;
  }

  // Cutscenes (0x8370D991: a cutscene is playing, see the script function
  // cutscene_play_check_done 824CB740; 0x8370D990: scripted cutscene, set by
  // scripted_cutscene_playing 824DAD78): a mouse click skips, sent as the
  // skip button (Y, or A in some scenes). Pressing every face button and
  // Back at once stopped the Y skip from registering.
  if (PPC_LOAD_U8(0x8370D991u) || PPC_LOAD_U8(0x8370D990u)) {
    if (Down(VK_LBUTTON) || Down(VK_RBUTTON)) p.buttons |= X_INPUT_GAMEPAD_Y;
    if (Down(VK_LBUTTON)) p.buttons |= X_INPUT_GAMEPAD_A;
    return p;
  }

  press(Down('Q'), X_INPUT_GAMEPAD_B);
  press(Down('E'), X_INPUT_GAMEPAD_Y);
  if (Down(VK_LBUTTON)) p.rt = 0xFF;
  press(Down(VK_MBUTTON) || Down('V'), X_INPUT_GAMEPAD_RIGHT_THUMB);

  if (in_vehicle) {
    press(Down('W'), X_INPUT_GAMEPAD_A);  // accelerate
    press(Down('S'), X_INPUT_GAMEPAD_X);  // brake / reverse
    if (Down(VK_SPACE)) p.lt = 0xFF;       // handbrake
    press(Down(VK_SHIFT), X_INPUT_GAMEPAD_RIGHT_THUMB);  // nitrous
    press(Down(VK_CONTROL), X_INPUT_GAMEPAD_LEFT_THUMB);  // hydraulics
    press(Down('Z') || Down('X'), X_INPUT_GAMEPAD_LEFT_SHOULDER);   // look left
    press(Down('C') || Down('X'), X_INPUT_GAMEPAD_RIGHT_SHOULDER);  // look right
  } else {
    const int up = Down('W') ? 1 : 0, down = Down('S') ? 1 : 0;
    p.ly = (up - down) * 32767;
    if (Down(VK_RBUTTON)) p.lt = 0xFF;                            // secondary attack
    press(Down(VK_SPACE), X_INPUT_GAMEPAD_X);                     // jump
    press(Down(VK_SHIFT), X_INPUT_GAMEPAD_RIGHT_SHOULDER);        // sprint
    press(Down('R'), X_INPUT_GAMEPAD_A);                          // reload / grab weapon
    press(Down('F'), X_INPUT_GAMEPAD_LEFT_SHOULDER);              // kick
    press(Down('C'), X_INPUT_GAMEPAD_LEFT_THUMB);                 // crouch
  }
  return p;
}
#endif

}  // namespace

void sr::SetMouseSensitivity(double sensitivity) {
  g_sensitivity.store(sensitivity, std::memory_order_relaxed);
}

void sr::SetMouseCaptureSuspended(bool suspended) {
#ifdef _WIN32
  g_mouse_suspended.store(suspended, std::memory_order_relaxed);
  if (suspended) {
    std::lock_guard<std::mutex> lock(g_mouse_mutex);
    ReleaseMouseLocked();
  }
#else
  (void)suspended;
#endif
}

void sr::AddMouseWheel(int delta) {
#ifdef _WIN32
  if (!delta) return;
  std::lock_guard<std::mutex> lock(g_mouse_mutex);
  g_mouse.wheel_y = delta > 0 ? 1 : -1;
  // Keep the virtual stick engaged long enough for the guest's next input
  // update. Wheel events are momentary, while controller input is sampled.
  g_mouse.wheel_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(90);
#else
  (void)delta;
#endif
}

// Adds the keyboard and mouse to the controller state the game reads.
PPC_FUNC_IMPL(__imp__XamInputGetState) {
  const uint32_t user_index = ctx.r3.u32;
  const uint32_t state_addr = ctx.r5.u32;
  HostToGuestFunction<rex::kernel::xam::XamInputGetState_entry>(ctx, base);
#ifdef _WIN32
  static bool base_logged = false;
  if (!base_logged) {
    base_logged = true;
    REXLOG_INFO("KBM guest memory base: 0x{:X}", reinterpret_cast<uintptr_t>(base));
  }
  const uint32_t result = ctx.r3.u32;
  if (!state_addr || result != 0) return;
  if ((user_index & 0xFF) != 0 && (user_index & 0xFF) != 0xFF) return;
  std::lock_guard<std::mutex> lock(g_mouse_mutex);
  if (!GameHasFocus()) {
    ReleaseMouseLocked();
    return;
  }
  RefreshKeys();
  auto* state = reinterpret_cast<X_INPUT_STATE*>(base + state_addr);
  auto& pad = state->gamepad;
  if (g_mouse_suspended.load(std::memory_order_relaxed)) {
    // An interactive overlay (display menu) is open: all input belongs to
    // it, so report a neutral pad to the game (keyboard, mouse and
    // controller alike). The game ignores a state whose packet number
    // hasn't changed, so keep it ticking.
    memset(&pad, 0, sizeof(pad));
    state->packet_number = uint32_t(state->packet_number) + 1;
    return;
  }
  bool pause_menu = UpdatePauseMenuState(base, uint16_t(pad.buttons));
  const bool player_creation = CharacterCreationInputActive(base);
  {
    static std::chrono::steady_clock::time_point next_log{};
    const auto t = std::chrono::steady_clock::now();
    if (t >= next_log) {
      next_log = t + std::chrono::seconds(3);
      REXLOG_INFO("KBM state: cutscene {}/{} slot {} camera idle {} ms pause {} creator {} player {:08X} | c1 {:X} c2 {:X} c3 {:X} c4 {:X} c5 {:X} c6 {:X} c7 {:X}",
                  PPC_LOAD_U8(0x8370D991u), PPC_LOAD_U8(0x8370D990u), PPC_LOAD_U32(0x8309AD70u),
                  std::chrono::duration_cast<std::chrono::milliseconds>(t - g_mouse.last_camera).count(),
                  pause_menu, player_creation, PPC_LOAD_U32(0x8309ABECu),
                  PPC_LOAD_U32(0x827B04D4u), PPC_LOAD_U32(0x83126E20u), PPC_LOAD_U32(0x839E0DF8u),
                  PPC_LOAD_U32(0x83710258u), PPC_LOAD_U32(0x83117C1Cu), PPC_LOAD_U32(0x83710070u),
                  PPC_LOAD_U32(0x82AD5888u));
      if (const uint32_t pl = PPC_LOAD_U32(0x8309ABECu)) REXLOG_INFO("KBM state: tagging spot {:08X}", PPC_LOAD_U32(pl + 3700));
    }
  }
  if (player_creation) pause_menu = false;  // Esc in the creator is its own back key
  // In a cutscene the gameplay camera doesn't run, so a pause toggled there
  // (Esc to skip, or a stray Start edge) never got cleared and the mouse
  // clicks went to the pause menu branch instead of skipping. Trust the
  // active menu screen: only the pause menu screen (0x82FFB84C) is a pause.
  if (pause_menu && (PPC_LOAD_U8(0x8370D991u) || PPC_LOAD_U8(0x8370D990u)) &&
      PPC_LOAD_U32(0x839E0DF8u) != 0x82FFB84Cu) {
    pause_menu = false;
    g_pause_menu_active = false;
  }
  Pad k = ReadKeyboard(base, pause_menu, player_creation);

  auto now = std::chrono::steady_clock::now();
  double poll_dt = std::chrono::duration<double>(now - g_mouse.last_poll).count();
  g_mouse.last_poll = now;
  double dx, dy;
  PollMouseLocked(dx, dy);

  {
    // Button prompts follow the device used last (glyphs.cpp).
    const auto stick = [](int v) { return v > 9000 || v < -9000; };
    const bool controller_used = uint16_t(pad.buttons) != 0 || pad.left_trigger > 40 ||
                                 pad.right_trigger > 40 || stick(int16_t(pad.thumb_lx)) ||
                                 stick(int16_t(pad.thumb_ly)) || stick(int16_t(pad.thumb_rx)) ||
                                 stick(int16_t(pad.thumb_ry));
    const bool kbm_used = k.buttons || k.lt || k.rt || k.lx || k.ly ||
                          std::abs(dx) + std::abs(dy) > 3.0 || g_mouse.wheel_y != 0;
    sr::GlyphContext context = sr::GlyphContext::kOnFoot;
    if (player_creation) context = sr::GlyphContext::kCreator;
    else if (pause_menu || PPC_LOAD_U32(0x8309ABECu) == 0) context = sr::GlyphContext::kMenu;
    else if (PlayerInVehicle(base)) context = sr::GlyphContext::kVehicle;
    sr::GlyphsNoteInput(base, controller_used, kbm_used, context);
  }

  int rx = 0, ry = 0;
  const bool radial = Down('Q') && !pause_menu && !player_creation;
  if (radial) {
    // Radial menu: point at an item with the mouse, like Saints Row 2.
    if (!g_mouse.radial_open) {
      g_mouse.radial_open = true;
      g_mouse.radial_x = g_mouse.radial_y = 0;
    }
    g_mouse.radial_x += dx;
    g_mouse.radial_y += dy;
    const double len = std::hypot(g_mouse.radial_x, g_mouse.radial_y);
    constexpr double kRadius = 120.0;
    if (len > kRadius) {
      g_mouse.radial_x *= kRadius / len;
      g_mouse.radial_y *= kRadius / len;
    }
    if (len > 25.0 && !k.lx && !k.ly) {
      k.lx = int(g_mouse.radial_x / std::max(len, 1.0) * 32767.0);
      k.ly = int(-g_mouse.radial_y / std::max(len, 1.0) * 32767.0);
    }
    g_mouse.dx = g_mouse.dy = 0;  // the camera stays still meanwhile
  } else if (pause_menu) {
    // The pause map normally pans with the left stick (and therefore WASD).
    // Feed mouse motion into those same axes; held WASD wins on either axis.
    int mx = 0, my = 0;
    MouseToStickLocked(dx, dy, poll_dt, mx, my);
    if (!k.lx) k.lx = mx;
    if (!k.ly) k.ly = my;
    if (g_mouse.wheel_y && now < g_mouse.wheel_until) {
      // The original pause map zooms with the right stick: up zooms in and
      // down zooms out. The wheel is deliberately ignored outside this menu.
      ry = g_mouse.wheel_y * 32767;
    } else if (now >= g_mouse.wheel_until) {
      g_mouse.wheel_y = 0;
    }
    g_mouse.dx = g_mouse.dy = 0;
  } else if (!player_creation && TaggingActive(base)) {
    // Tagging: moving the mouse in circles turns the left stick the same way
    // (the stick points where the mouse is moving). WASD still works.
    static double tag_vx = 0, tag_vy = 0;
    const double t = std::clamp(poll_dt, 0.001, 0.1);
    const double a = 1.0 - std::exp(-t / 0.05);
    tag_vx += (dx / t - tag_vx) * a;
    tag_vy += (dy / t - tag_vy) * a;
    const double speed = std::hypot(tag_vx, tag_vy);
    if (speed * g_sensitivity.load(std::memory_order_relaxed) > 150.0 && !k.lx && !k.ly) {
      k.lx = int(tag_vx / speed * 32767.0);
      k.ly = int(-tag_vy / speed * 32767.0);
    }
    g_mouse.dx = g_mouse.dy = 0;
  } else if (player_creation) {
    // Character creation uses the right stick horizontally to rotate the
    // preview and vertically to zoom. Feed mouse motion into those axes.
    MouseToStickLocked(dx, dy, poll_dt, rx, ry);
    if (g_mouse.wheel_y && now < g_mouse.wheel_until) {
      ry = g_mouse.wheel_y * 32767;
    } else if (now >= g_mouse.wheel_until) {
      g_mouse.wheel_y = 0;
    }
    g_mouse.dx = g_mouse.dy = 0;
  } else {
    g_mouse.wheel_y = 0;
    g_mouse.radial_open = false;
    g_mouse.dx += dx;
    g_mouse.dy += dy;
    {
      // For mods that steer a view themselves (wml.mouse_look): the mouse,
      // and the controller's right stick, as turning in radians.
      const uint8_t invert = PPC_LOAD_U8(0x829B83F2u);  // bit 0x40 invert X, 0x20 invert Y
      const double k = 0.001 * g_sensitivity.load(std::memory_order_relaxed);
      double yaw = dx * k, pitch = -dy * k;
      auto stick = [](int v) {
        const double s = double(v) / 32767.0;
        return std::abs(s) < 0.24 ? 0.0 : (s - (s > 0 ? 0.24 : -0.24)) / 0.76;
      };
      const double t = std::clamp(poll_dt, 0.0, 0.1);
      yaw += stick(int16_t(pad.thumb_rx)) * 2.5 * t;
      pitch += stick(int16_t(pad.thumb_ry)) * 1.8 * t;
      if (invert & 0x40) yaw = -yaw;
      if (invert & 0x20) pitch = -pitch;
      if (yaw != 0 || pitch != 0) wml::AddMouseLook(yaw, pitch);
    }
    // Fallback for cameras that don't go through the hook below.
    double since_camera = std::chrono::duration<double>(now - g_mouse.last_camera).count();
    if (since_camera > 0.25) {
      MouseToStickLocked(g_mouse.dx, g_mouse.dy, poll_dt, rx, ry);
      g_mouse.dx = g_mouse.dy = 0;
    }
  }

  if (!k.buttons && !k.lt && !k.rt && !k.lx && !k.ly && !rx && !ry) return;

  pad.buttons = uint16_t(pad.buttons) | k.buttons;
  if (k.lt > pad.left_trigger) pad.left_trigger = k.lt;
  if (k.rt > pad.right_trigger) pad.right_trigger = k.rt;
  if (k.lx) pad.thumb_lx = int16_t(std::clamp(k.lx, -32767, 32767));
  if (k.ly) pad.thumb_ly = int16_t(std::clamp(k.ly, -32767, 32767));
  if (rx) pad.thumb_rx = int16_t(rx);
  if (ry) pad.thumb_ry = int16_t(ry);
  // The game ignores a state whose packet number hasn't changed.
  state->packet_number = uint32_t(state->packet_number) + 1;
#endif
}

// ---------------------------------------------------------------------------
// Mouse look
// ---------------------------------------------------------------------------
//
// sub_8210D518 updates the third-person camera once per frame (f1 = frame
// time). Its camera state is at 0x827D9778: +320 is the horizontal and +336
// the vertical turn input, which the player controls fill in from the right
// stick. The turn code (sub_8210CFE0 / sub_8210C9E0, tuned by
// camera_free.xtbl) uses a "slow pan" zone that turns at input x multiplier,
// immediately, and a "fast pan" zone with acceleration for a pegged stick.
// For the mouse the fast zone is switched off and the slow multipliers set to
// 1 for the duration of the call, so the camera turns exactly by the mouse
// movement of this frame.
extern "C" void __imp__sub_8210D518(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_8210D518) {
  constexpr uint32_t kCamera = 0x827D9778;
  constexpr uint32_t kSlowPanH = 0x827D9348, kSlowPanV = 0x827D934C;
  constexpr uint32_t kFastPanThreshold = 0x827D9350;
  constexpr uint32_t kInvertFlags = 0x829B83F2;  // bit 0x40 invert X, 0x20 invert Y

  double dx = 0, dy = 0;
  {
    std::lock_guard<std::mutex> lock(g_mouse_mutex);
    const auto now = std::chrono::steady_clock::now();
    g_mouse.last_camera = now;
    // Choosing Resume can close the overlay without another Start edge. The
    // gameplay camera returning is the authoritative signal in that path. A
    // short grace period keeps the last gameplay-camera call in the opening
    // frame from immediately undoing the pause state.
    if (g_pause_menu_active && now - g_pause_toggled > std::chrono::milliseconds(250)) {
      g_pause_menu_active = false;
    }
    dx = g_mouse.dx;
    dy = g_mouse.dy;
    g_mouse.dx = g_mouse.dy = 0;
  }
  const double dt = ctx.f1.f64;
  const double mod_turn = wml::TakeCameraTurn();

  // In vehicles the game doesn't clear the turn inputs (+320 / +336)
  // between calls, so a turn from the mouse kept going after the mouse
  // stopped (the view swung off to the side). Clear them when there is no
  // mouse input.
  // Only a turn the mouse wrote is cleared (once): the controller's right
  // stick writes these inputs itself, and clearing them every call stopped
  // the stick from looking around in vehicles.
  const bool in_vehicle = PlayerInVehicle(base);
  static bool mouse_turn_written = false;
  if (dx == 0 && dy == 0 && mod_turn == 0) {
    if (in_vehicle && mouse_turn_written) {
      StoreF32(base, kCamera + 320, 0.0f);
      StoreF32(base, kCamera + 324, 0.0f);
      StoreF32(base, kCamera + 336, 0.0f);
    }
    mouse_turn_written = false;
  }

  if ((dx == 0 && dy == 0 && mod_turn == 0) || !(dt > 0.0001) || dt > 0.5) {
    __imp__sub_8210D518(ctx, base);
    return;
  }

  // Radians per pixel at sensitivity 1.
  constexpr double kRadiansPerPixel = 0.001;
  const double k = kRadiansPerPixel * g_sensitivity.load(std::memory_order_relaxed) / dt;
  const uint8_t invert = PPC_LOAD_U8(kInvertFlags);
  double yaw = dx * k, pitch = -dy * k;
  if (invert & 0x40) yaw = -yaw;
  if (invert & 0x20) pitch = -pitch;
  yaw += mod_turn / dt;
  // A short camera step (dt) made one mouse movement a huge turn speed;
  // in vehicles that threw the view far round in one frame.
  if (in_vehicle) {
    constexpr double kMaxTurn = 6.0;  // radians per second
    yaw = std::clamp(yaw, -kMaxTurn, kMaxTurn);
    pitch = std::clamp(pitch, -kMaxTurn, kMaxTurn);
  }

  // Turn limits from mods (e.g. first person in a vehicle). Which way an
  // input turns the view is learned by comparing the view direction (final
  // camera orientation, row 3 at +104) with the one at the previous call.
  static int yaw_sign = 0, pitch_sign = 0;
  static double last_yaw_in = 0, last_pitch_in = 0, last_view_yaw = 0, last_view_pitch = 0;
  static bool have_last = false;
  {
    const float fx = LoadF32(base, kCamera + 104), fy = LoadF32(base, kCamera + 108),
                fz = LoadF32(base, kCamera + 112);
    const double view_yaw = std::atan2(fx, fz), view_pitch = std::asin(std::clamp(fy, -1.0f, 1.0f));
    if (have_last) {
      double dyaw = view_yaw - last_view_yaw;
      while (dyaw > 3.14159265) dyaw -= 6.2831853;
      while (dyaw < -3.14159265) dyaw += 6.2831853;
      const double dpitch = view_pitch - last_view_pitch;
      // Votes rather than the last sample: in a vehicle the view also turns
      // with the vehicle, and a single wrong sample flipped the sign, which
      // then blocked turning back from the limit (the view locked up).
      static int yaw_votes = 0, pitch_votes = 0;
      if (std::abs(last_yaw_in) > 1e-3 && std::abs(dyaw) > 1e-3)
        yaw_votes = std::clamp(yaw_votes + (((dyaw > 0) == (last_yaw_in > 0)) ? 1 : -1), -200, 200);
      if (std::abs(last_pitch_in) > 1e-3 && std::abs(dpitch) > 1e-3)
        pitch_votes = std::clamp(pitch_votes + (((dpitch > 0) == (last_pitch_in > 0)) ? 1 : -1), -200, 200);
      yaw_sign = yaw_votes > 0 ? 1 : yaw_votes < 0 ? -1 : 0;
      pitch_sign = pitch_votes > 0 ? 1 : pitch_votes < 0 ? -1 : 0;
    }
    last_view_yaw = view_yaw; last_view_pitch = view_pitch; have_last = true;
  }
  wml::CameraLimit limit;
  if (wml::GetCameraLimit(limit)) {
    if (yaw_sign && ((limit.yaw > limit.yaw_limit && yaw * yaw_sign > 0) ||
                     (limit.yaw < -limit.yaw_limit && yaw * yaw_sign < 0))) yaw = 0;
    if (pitch_sign && ((limit.pitch > limit.pitch_up && pitch * pitch_sign > 0) ||
                       (limit.pitch < -limit.pitch_down && pitch * pitch_sign < 0))) pitch = 0;
  }
  last_yaw_in = yaw; last_pitch_in = pitch;

  const float slow_h = LoadF32(base, kSlowPanH), slow_v = LoadF32(base, kSlowPanV);
  const float threshold = LoadF32(base, kFastPanThreshold);
  StoreF32(base, kSlowPanH, 1.0f);
  StoreF32(base, kSlowPanV, 1.0f);
  StoreF32(base, kFastPanThreshold, 1.0e6f);
  StoreF32(base, kCamera + 320, float(yaw));
  StoreF32(base, kCamera + 324, 0.0f);
  StoreF32(base, kCamera + 336, float(pitch));
  mouse_turn_written = true;
  __imp__sub_8210D518(ctx, base);
  StoreF32(base, kSlowPanH, slow_h);
  StoreF32(base, kSlowPanV, slow_v);
  StoreF32(base, kFastPanThreshold, threshold);
}
