// Co-op in the game's own menus.
//
// Story pause menu, OPTIONS tab (built by sub_822B7328: Controls, Display,
// Audio, Quit Game; its update sub_822B7D68 acts on A for rows 0-3 through a
// jump table and moves the cursor with the generic list code): rows are added
// after Quit Game - "Host Co-op" and "Join Co-op", or "End Co-op" / "Leave
// Co-op" during a session. Rows past 3 are ignored by the game's A handler, so
// ours are acted on here.
//
// Main menu (built by sub_822852B0, 7 rows; its update sub_822861F0 keeps the
// cursor at menu object +8 and dispatches A on it): "Join Co-op" is added as
// the last row. It asks for the host's join code or IP, then opens the load
// screen the way LOAD GAME does (0x827AF780 = 0, 0x8300FD3C/40 = 4, front-end
// state 0x8370DDB0 = 44, see kbm.cpp); once the picked save has loaded, the
// co-op mod joins.
//
// Both lists are finished with sub_8228CAD8; our rows are added right before
// that (recognised by the return address), with the generic row adder
// sub_8228BAB0 like the PC rows in options_menu.cpp.

#include "coop_menu.h"
#include "online_players.h"

#include <sr_reserved_names.h>

#include "saintsrow_config.h"
#include "saintsrow_init.h"

#include <algorithm>
#include <map>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/function.h>
#include <rex/system/kernel_state.h>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

constexpr uint32_t kMenuPointer = 0x8370DDC4u;       // current list menu (+58 rows, +62 cursor)
constexpr uint32_t kPauseOptionsDone = 0x822B7C04u;  // return address of sub_8228CAD8 in sub_822B7328
constexpr uint32_t kMainMenuDone = 0x822861B8u;      // return address of sub_8228CAD8 in sub_822852B0
constexpr uint32_t kPlayer = 0x8309ABECu;
constexpr uint32_t kActiveScreen = 0x839E0DF8u;

enum Action { kHost, kJoin, kStop, kName, kZombies };
// 0-3: rows of the pause menu's CO-OP tab (mixed case like the game's own
// rows), 4: the tab's name, 5: the main menu row (upper case like its rows).
const char* const kLabels[] = {"Host Co-op", "Join Co-op", "End Co-op", "Leave Co-op", "CO-OP", "JOIN CO-OP"};
constexpr int kLabelCount = 6;
constexpr int kTabLabel = 4, kMainLabel = 5;

// Pause menu CO-OP tab. The pause menu builder sub_822BFA88 adds its tabs
// with sub_822903E8(r3 = name, r4 = selected, r5 = menu id): MAP 26, INFO 32,
// HELP 42, PHONE 29, MP3 27, SAVE/LOAD 44, OPTIONS 43. A menu id indexes the
// table at 0x827B05F8 (16 bytes: build, exit, update, draw); ids 46 and 47
// are unused (no-op functions). The menu manager at 0x8370DDAC (+0 current id,
// +4 requested id) calls exit of the current menu and build of the
// requested one, then update of the current one every frame; the tabs'
// LT/RT switching (sub_822C06B8, called by each tab's update) requests ids.
// Id 46 gets the OPTIONS functions; their overrides below see the id and
// build / run the CO-OP list instead.
constexpr uint32_t kMenuCurrent = 0x8370DDACu, kMenuRequested = 0x8370DDB0u;
constexpr uint32_t kMenuTable = 0x827B05F8u;
constexpr uint32_t kOptionsId = 43, kCoopId = 46;
// Main menu > SYSTEM LINK (id 19, sub_82331070: tabs 20 SYSTEM LINK, 21
// OPTIONS, 30 HELP) and XBOX LIVE (id 6, sub_82330E48: its own tabs plus the
// same 21 OPTIONS and 30 HELP). The OPTIONS tab (id 21, build sub_823487B8,
// update sub_82349018) lists Controls / Display / Audio (A requests menu 22 /
// 24 / 25 for cursor 0 / 1 / 2; other rows only play the select sound); a
// "Player Name: X" row is added after Audio.
constexpr uint32_t kMpOptionsId = 21;
constexpr uint32_t kMpOptionsDone = 0x82348F50u;  // return address of sub_8228CAD8 in sub_823487B8
int g_mp_name_row = -1;                           // our row in that list
int g_mp_add_row = -1;                            // "Add Friend" row (friend code row above it)
std::string g_code_shown;                         // friend code in the row now
bool g_mp_reselect = false;                       // put the cursor back on our row after a rebuild
bool g_mp_reselect_minp = false;                  // same for the Players to Start row
constexpr uint32_t kOptionsTabAdded = 0x822BFCB0u;  // return address of the OPTIONS tab add in sub_822BFA88
constexpr uint32_t kMultiplayerFlag = 0x8370E9F6u;   // byte, nonzero in multiplayer (other tab set)

std::recursive_mutex g_mutex;
uint32_t g_text[kLabelCount] = {};  // guest UTF-16BE label texts
uint32_t g_info_text = 0;       // guest UTF-16BE: join code / IP row (kInfoChars)
uint32_t g_name_text = 0;       // guest UTF-16BE: "Player Name: X" row (kInfoChars)
uint32_t g_code_text = 0;       // guest UTF-16BE: "Friend Code: XXXX-XXXX" row (kInfoChars)
uint32_t g_addfriend_text = 0;  // guest UTF-16BE: "Add Friend" row (kInfoChars)
uint32_t g_minp_text = 0;       // guest UTF-16BE: "Players to Start: N" row (kInfoChars)
uint32_t g_lobby_minp_text = 0; // same, for the lobby GAME list
uint32_t g_zombies_text = 0;    // "Start Zombies" / "End Zombies" (pause CO-OP tab)
int g_lobby_minp_row = -1;      // our row in the lobby GAME list (-1 = not added)
uint32_t g_lobby_minp_menu = 0; // the list object it was added to
int g_mp_minp_row = -1;         // that row in the multiplayer OPTIONS list
constexpr uint32_t kInfoChars = 64;
int g_pause_first = -1;         // first of our rows in the pause OPTIONS list
std::vector<Action> g_pause_actions;
// Main menu: our row sits under LOAD GAME (list row 2); the game's own rows
// from XBOX LIVE on move down one. menu +62 (the list cursor) counts our row,
// the menu object's +8 (the game's own row number, used by its A handler and
// when the list is built again) does not: our row is 0xFFFF there.
constexpr uint32_t kMainMenuObject = 0x82816CC0u;  // the main menu screen
constexpr int kMainJoinRow = 2;
int g_main_row = -1;            // our row in the main menu list
uint32_t g_main_object = 0;
uint16_t g_main_cursor_before = 0;

// Main menu join: dialog -> load screen -> join after the save loaded.
bool g_open_load = false;
bool g_join_pending = false;
std::string g_join_code;
std::chrono::steady_clock::time_point g_join_since{}, g_join_ready{};

// The code box is the game's own message dialog (the screen behind the debug
// command message_test, sub_8229ED60): vtable 0x8205FD28, +8 title and +12
// text (UTF-16BE; the text is copied and word-wrapped when it opens,
// sub_8229EF58; button pictures are the font's button characters, 0x7F A and
// 0x81 B, the codes the game turns "<btn A>" / "<btn B>" into (table at
// 0x8205F6C0); glyphs.cpp swaps their pictures for keyboard/mouse), +588/+592 close callback
// (0 / -1), +596 keeps the menus behind it from taking input, +597 shown
// (0 fades it out), +600 fade (float). Pushed with sub_8229B6C8(object, 0).
// Its update sub_8229F288 returns 0 once faded out, which closes the screen.
constexpr uint32_t kMessageDialogVtable = 0x8205FD28u;
constexpr uint32_t kDialogSize = 640, kTitleSize = 64, kTextSize = 512;
std::atomic<bool> g_dialog_open{false};
enum class DialogState { kClosed, kOpen, kClosing, kWaitRelease } g_dialog_state = DialogState::kClosed;
uint32_t g_dialog = 0, g_dialog_title = 0, g_dialog_text = 0;
std::string g_typed;
bool g_typed_changed = false;
bool g_name_changed = false;  // rebuild the CO-OP tab (new name row text)
bool g_key_was_down[256] = {};
enum class DialogFor { kNone, kPause, kMainMenu, kName, kFriend } g_dialog_for = DialogFor::kNone;

// From the pause menu, hosting / joining starts once the game runs again
// (hosting online right away with the pause menu up crashed): the pause
// menu is closed with a short Start press (sent by kbm.cpp), then the action
// runs when no menu is open (or after 4 s regardless).
int g_resume_action = 0;  // 1 host, 2 join
std::string g_resume_code;
std::chrono::steady_clock::time_point g_resume_since{};
bool g_resume_clock = false;

void StartAfterResume(int action, const std::string& code) {
  g_resume_action = action;
  g_resume_code = code;
  g_resume_clock = false;
}

uint8_t* Host(uint8_t* base, uint32_t a) { return base + a + (a >= 0xE0000000u ? 0x1000u : 0u); }
uint32_t R32(uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, Host(base, a), 4);
  return __builtin_bswap32(v);
}
void W32(uint8_t* base, uint32_t a, uint32_t v) {
  const uint32_t be = __builtin_bswap32(v);
  std::memcpy(Host(base, a), &be, 4);
}
uint16_t R16(uint8_t* base, uint32_t a) {
  const uint8_t* p = Host(base, a);
  return uint16_t(p[0] << 8 | p[1]);
}
void W16(uint8_t* base, uint32_t a, uint16_t v) {
  uint8_t* p = Host(base, a);
  p[0] = uint8_t(v >> 8);
  p[1] = uint8_t(v);
}

// The co-op mod's exports (nullptr when it isn't loaded).
template <class F>
F CoopFunction(const char* name) {
#ifdef _WIN32
  HMODULE m = GetModuleHandleW(L"WhompaysCoop.dll");
  return m ? reinterpret_cast<F>(GetProcAddress(m, name)) : nullptr;
#else
  (void)name;
  return nullptr;
#endif
}
bool CoopLoaded() { return CoopFunction<int (*)()>("WhompaysCoopState") != nullptr; }
int CoopState() {
  auto f = CoopFunction<int (*)()>("WhompaysCoopState");
  return f ? f() : 0;
}
void CoopHost() {
  if (auto f = CoopFunction<void (*)()>("WhompaysCoopHost")) f();
}
void CoopStop() {
  if (auto f = CoopFunction<void (*)()>("WhompaysCoopStop")) f();
}
// The Zombies mod (core\\Zombies): a zombie game in the story world.
template <class F>
F ZombiesFunction(const char* name) {
#ifdef _WIN32
  HMODULE m = GetModuleHandleW(L"Zombies.dll");
  return m ? reinterpret_cast<F>(GetProcAddress(m, name)) : nullptr;
#else
  (void)name;
  return nullptr;
#endif
}
bool ZombiesLoaded() { return ZombiesFunction<int (*)()>("ZombiesState") != nullptr; }
bool ZombiesRunning() {
  auto f = ZombiesFunction<int (*)()>("ZombiesState");
  return f && f() != 0;
}
void ZombiesToggle() {
  if (auto f = ZombiesFunction<void (*)()>("ZombiesToggle")) f();
}
void CoopJoin(const std::string& code) {
  if (auto f = CoopFunction<void (*)(const char*)>("WhompaysCoopJoin")) f(code.c_str());
}
// The session line for the pause menu ("JOIN CODE: X  (CONNECTED)").
std::string CoopInfo() {
  char buf[128] = {};
  if (auto f = CoopFunction<int (*)(char*, int)>("WhompaysCoopInfo")) f(buf, int(sizeof buf));
  return buf;
}
std::string CoopLastJoin() {
  char buf[128] = {};
  if (auto f = CoopFunction<int (*)(char*, int)>("WhompaysCoopLastJoin")) f(buf, int(sizeof buf));
  std::string s = buf;
  return s == "127.0.0.1" ? std::string() : s;
}

// Player name (player_name.txt next to the exe; main.cpp puts it into the SDK
// cvar xam_player_name at start): the name other players see in co-op, System
// Link and the Xbox Live menus. Up to 15 characters.
std::string PlayerName() {
  std::string n = rex::cvar::GetFlagByName("xam_player_name");
  return n;
}
// Reserved names ("Whompay" and look-alikes, sr_reserved_names.h) only on
// the owner's PC (owner.key next to the exe; other players' games check the
// owner's signature, online_identity.cpp in the runtime).
std::chrono::steady_clock::time_point g_name_refused_until{};
bool NameAllowed(const std::string& name) {
  if (!sr_names::IsReserved(name)) return true;
  FILE* f = std::fopen("owner.key", "rb");
  if (f) std::fclose(f);
  return f != nullptr;
}
std::string NameRowText() {
  if (std::chrono::steady_clock::now() < g_name_refused_until) return "Player Name: that name is taken";
  const std::string name = PlayerName();
  return "Player Name: " + (name.empty() ? std::string("(not set)") : name);
}
void SetPlayerName(const std::string& name) {
  rex::cvar::SetFlagByName("xam_player_name", name);
  if (FILE* f = std::fopen("player_name.txt", "wb")) {
    std::fwrite(name.data(), 1, name.size(), f);
    std::fclose(f);
  }
  REXLOG_INFO("Player name set to '{}'", name);
}

// Players to start a matchmade game (Ranked / Player Match / Quick Match):
// the host's countdown starts once this many players are connected
// (console var mp_auto_mm_conn_needed, int 0x827ADF04, game default 4;
// sub_82362108). Set in MULTIPLAYER > OPTIONS, kept in mp_min_players.txt
// next to the exe. Only the host's value counts. Default 2 since 2026-10-28
// (with few players online, hosts on 4 or 7 waited forever); and a host with
// 2+ players starts after 60 s whatever this says (kMatchWaitMs below).
constexpr uint32_t kMinPlayersVar = 0x827ADF04u;
constexpr int kMinPlayersLow = 1, kMinPlayersHigh = 12;  // 1 = start alone
int g_min_players = 0;  // 0 = not loaded yet
int MinPlayers() {
  if (g_min_players) return g_min_players;
  int v = 2;  // Saints Reborn default (the game's own mp_auto_mm_conn_needed is 4)
  if (FILE* f = std::fopen("mp_min_players.txt", "rb")) {
    int x = 0;
    if (std::fscanf(f, "%d", &x) == 1 && x >= kMinPlayersLow && x <= kMinPlayersHigh) v = x;
    std::fclose(f);
  }
  g_min_players = v;
  REXLOG_INFO("Multiplayer: matchmade games start with {} player(s) (MULTIPLAYER > OPTIONS)", v);
  return v;
}
void SetMinPlayers(int v) {
  g_min_players = v;
  if (FILE* f = std::fopen("mp_min_players.txt", "wb")) {
    std::fprintf(f, "%d\n", v);
    std::fclose(f);
  }
  REXLOG_INFO("Multiplayer: players to start set to {}", v);
}
// Ranked never starts with fewer than 2 players, whatever "Players to Start"
// says (1 is for trying Player Match / Zombies alone).
constexpr uint32_t kLobbyKindVar = 0x827AD564u;  // 0 custom, 1 player match, 2 ranked, 3 system link
constexpr int kRankedMinPlayers = 2;
bool RankedLobby(uint8_t* base) { return R32(base, kLobbyKindVar) == 2; }
// Set by SoloStartPoll once a hosted matchmade lobby has had 2+ players for
// kMatchWaitMs: "Players to Start" no longer holds the match back.
std::atomic<int> g_mm_need_override{0};
int EffectiveMinPlayers(uint8_t* base) {
  const int forced = g_mm_need_override.load();
  const int v = forced ? std::min(MinPlayers(), forced) : MinPlayers();
  return RankedLobby(base) ? std::max(v, kRankedMinPlayers) : v;
}
void ApplyMinPlayers(uint8_t* base) {
  const uint32_t v = uint32_t(EffectiveMinPlayers(base));
  if (R32(base, kMinPlayersVar) != v) W32(base, kMinPlayersVar, v);
}
std::string MinPlayersRowText() {
  return "Players to Start: " + std::to_string(MinPlayers());
}

bool EnsureTexts(uint8_t* base) {
  if (g_text[0]) return true;
  const uint32_t block = REX_KERNEL_STATE()->memory()->SystemHeapAlloc(256 + kInfoChars * 14);
  if (!block) return false;
  g_info_text = block + 256;
  W16(base, g_info_text, 0);
  g_name_text = g_info_text + kInfoChars * 2;
  W16(base, g_name_text, 0);
  g_code_text = g_name_text + kInfoChars * 2;
  W16(base, g_code_text, 0);
  g_addfriend_text = g_code_text + kInfoChars * 2;
  W16(base, g_addfriend_text, 0);
  g_minp_text = g_addfriend_text + kInfoChars * 2;
  W16(base, g_minp_text, 0);
  g_lobby_minp_text = g_minp_text + kInfoChars * 2;
  W16(base, g_lobby_minp_text, 0);
  g_zombies_text = g_lobby_minp_text + kInfoChars * 2;
  W16(base, g_zombies_text, 0);
  uint32_t at = block;
  for (int i = 0; i < kLabelCount; ++i) {
    g_text[i] = at;
    for (const char* c = kLabels[i]; *c; ++c, at += 2) W16(base, at, uint16_t(uint8_t(*c)));
    W16(base, at, 0);
    at += 2;
  }
  return true;
}

bool GuestBool(PPCContext& parent, uint8_t* base, void (*fn)(PPCContext&, uint8_t*), uint32_t arg) {
  rex::CallFrame frame(parent);
  frame.ctx.r3.u64 = arg;
  fn(frame.ctx, base);
  return (frame.ctx.r3.u32 & 0xFF) != 0;
}

void WriteText(uint8_t* base, uint32_t at, const std::string& text, uint32_t size) {
  uint32_t n = 0;
  for (char c : text) {
    if (n + 2 >= size / 2) break;
    W16(base, at + n * 2, uint16_t(uint8_t(c)));
    ++n;
  }
  W16(base, at + n * 2, 0);
}

std::string DialogBody() {
  if (g_dialog_for == DialogFor::kFriend)
    return "Type your friend's friend code\n(they find theirs under MULTIPLAYER > OPTIONS)\n\n" + g_typed +
           "_\n\n\x7F Add     \x81 Cancel";
  if (g_dialog_for == DialogFor::kName)
    return "Type the name other players see\n\n" + g_typed + "_\n\n\x7F Save     \x81 Cancel";
  return "Type the host's join code or IP\n\n" + g_typed + "_\n\n\x7F Join     \x81 Cancel";
}

bool KeyDown(int vk) {
#ifdef _WIN32
  return (GetAsyncKeyState(vk) & 0x8000) != 0;
#else
  (void)vk;
  return false;
#endif
}

bool WindowFocused() {
#ifdef _WIN32
  HWND foreground = GetForegroundWindow();
  DWORD pid = 0;
  if (foreground) GetWindowThreadProcessId(foreground, &pid);
  return pid == GetCurrentProcessId();
#else
  return false;
#endif
}

// New presses since the last call (keys held when the box opened don't count).
bool Pressed(int vk) {
  const bool down = KeyDown(vk);
  const bool was = g_key_was_down[vk & 0xFF];
  g_key_was_down[vk & 0xFF] = down;
  return down && !was;
}

std::string Clipboard() {
  std::string out;
#ifdef _WIN32
  if (!OpenClipboard(nullptr)) return out;
  if (HANDLE h = GetClipboardData(CF_TEXT)) {
    if (const char* p = static_cast<const char*>(GlobalLock(h))) {
      out = p;
      GlobalUnlock(h);
    }
  }
  CloseClipboard();
#endif
  return out;
}

void AddTyped(const std::string& text) {
  const bool name = g_dialog_for == DialogFor::kName;
  const bool friend_code = g_dialog_for == DialogFor::kFriend;
  for (char c : text) {
    if (friend_code) {
      if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
      if (((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-') && g_typed.size() < 9u) {
        g_typed += c;
        g_typed_changed = true;
      }
      continue;
    }
    if (!name && c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
    const bool ok = name ? ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' ' ||
                            c == '_' || c == '-' || c == '.')
                         : ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == ':' || c == '-');
    if (ok && g_typed.size() < (name ? 15u : 40u)) {
      g_typed += c;
      g_typed_changed = true;
    }
  }
}

// Keyboard typing into the box: letters, digits, '.', ':', '-', Backspace,
// Ctrl+V. Returns 1 for Enter, -1 for Esc, 0 otherwise.
int PollTyping() {
  if (!WindowFocused()) {
    for (int vk = 0; vk < 256; ++vk) g_key_was_down[vk] = KeyDown(vk);
    return 0;
  }
  const bool ctrl = KeyDown(VK_CONTROL);
  const bool upper = KeyDown(VK_SHIFT) != ((GetKeyState(VK_CAPITAL) & 1) != 0);
  for (int vk = 'A'; vk <= 'Z'; ++vk)
    if (Pressed(vk)) {
      if (ctrl && vk == 'V') AddTyped(Clipboard());
      else if (!ctrl) AddTyped(std::string(1, char(upper ? vk : vk - 'A' + 'a')));
    }
  if (Pressed(VK_SPACE)) AddTyped(" ");
  for (int vk = '0'; vk <= '9'; ++vk)
    if (Pressed(vk) && !ctrl) AddTyped(std::string(1, char(vk)));
  for (int vk = VK_NUMPAD0; vk <= VK_NUMPAD9; ++vk)
    if (Pressed(vk)) AddTyped(std::string(1, char('0' + vk - VK_NUMPAD0)));
  const bool shift = KeyDown(VK_SHIFT);
  if (Pressed(VK_OEM_PERIOD) || Pressed(VK_DECIMAL)) AddTyped(".");
  if (Pressed(VK_OEM_1) && shift) AddTyped(":");
  if (Pressed(VK_OEM_MINUS) || Pressed(VK_SUBTRACT)) AddTyped(KeyDown(VK_SHIFT) ? "_" : "-");
  if (Pressed(VK_BACK) && !g_typed.empty()) {
    g_typed.pop_back();
    g_typed_changed = true;
  }
  if (Pressed(VK_RETURN)) return 1;
  if (Pressed(VK_ESCAPE)) return -1;
  return 0;
}

void ShowDialog(PPCContext& ctx, uint8_t* base, DialogFor why);

}  // namespace

extern "C" void __imp__sub_8229B6C8(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_8229EF58(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_8229F288(PPCContext& ctx, uint8_t* base);

namespace {
void ShowDialog(PPCContext& ctx, uint8_t* base, DialogFor why) {
  if (g_dialog_state != DialogState::kClosed) return;
  if (!g_dialog) {
    const uint32_t block = REX_KERNEL_STATE()->memory()->SystemHeapAlloc(kDialogSize + kTitleSize + kTextSize);
    if (!block) return;
    g_dialog = block;
    g_dialog_title = block + kDialogSize;
    g_dialog_text = g_dialog_title + kTitleSize;
  }
  std::memset(Host(base, g_dialog), 0, kDialogSize);
  W32(base, g_dialog + 0, kMessageDialogVtable);
  W32(base, g_dialog + 8, g_dialog_title);
  W32(base, g_dialog + 12, g_dialog_text);
  W32(base, g_dialog + 588, 0);
  W32(base, g_dialog + 592, 0xFFFFFFFFu);
  Host(base, g_dialog + 596)[0] = 1;
  g_dialog_for = why;
  if (why == DialogFor::kName) {
    g_typed = PlayerName().substr(0, 15);
  } else if (why == DialogFor::kFriend) {
    g_typed.clear();
  } else {
    g_typed = CoopLastJoin();
    for (char& c : g_typed)
      if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
  }
  g_typed_changed = false;
  WriteText(base, g_dialog_title,
            why == DialogFor::kName ? "PLAYER NAME" : why == DialogFor::kFriend ? "ADD FRIEND" : "JOIN CO-OP", kTitleSize);
  WriteText(base, g_dialog_text, DialogBody(), kTextSize);
  for (int vk = 0; vk < 256; ++vk) g_key_was_down[vk] = KeyDown(vk);
  g_dialog_for = why;
  g_dialog_state = DialogState::kOpen;
  g_dialog_open.store(true);
  rex::CallFrame frame(ctx);
  frame.ctx.r3.u64 = g_dialog;
  frame.ctx.r4.u64 = 0;
  __imp__sub_8229B6C8(frame.ctx, base);
}
}  // namespace

extern "C" void __imp__sub_8228BAB0(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_8228CAD8(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_8228BE98(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_8216EE80(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_82288750(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_8216FCA0(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_822B7D68(PPCContext& ctx, uint8_t* base);

namespace {
void AddRow(PPCContext& ctx, uint8_t* base, uint32_t text, bool selectable = true) {
  rex::CallFrame frame(ctx);
  frame.ctx.r3.u64 = text;
  frame.ctx.r4.u64 = 0;  // plain text row
  frame.ctx.r5.u64 = 0;
  frame.ctx.r6.u64 = 0xFFFFFFFFFFFFFFFFull;
  frame.ctx.r7.u64 = selectable ? 1 : 0;
  frame.ctx.r8.u64 = 0;
  __imp__sub_8228BAB0(frame.ctx, base);
}
// A (Enter / left click) this frame, the test the OPTIONS list uses.
bool ConfirmPressed(PPCContext& ctx, uint8_t* base) {
  return GuestBool(ctx, base, __imp__sub_8216EE80, 55) && GuestBool(ctx, base, __imp__sub_82288750, 0);
}
// The main menu reads the pads itself; this is the general confirm test the
// PC option rows use (options_menu.cpp).
bool MainConfirmPressed(PPCContext& ctx, uint8_t* base) {
  return GuestBool(ctx, base, __imp__sub_82288750, 0) && GuestBool(ctx, base, __imp__sub_8216FCA0, 0);
}
}  // namespace

// Moves the row just added (the last, `last`) to list row `to`: relinks its
// node and moves the per-row selectable bytes (menu +64) along. The list is
// circular from menu +0 (the first row); rows follow each other through node
// +4, node +0 points back (sub_8228BAB0 links each new row in before the
// first one).
bool MoveLastRow(uint8_t* base, uint32_t menu, int last, int to) {
  if (to <= 0 || to >= last) return false;
  const uint32_t head = R32(base, menu + 0);
  if (!head) return false;
  auto next = [&](uint32_t n) { return R32(base, n + 4); };
  auto prev = [&](uint32_t n) { return R32(base, n + 0); };
  const uint32_t node = prev(head);  // the last row
  uint32_t before = head;            // row `to - 1`
  for (int i = 0; i < to - 1; ++i) before = next(before);
  if (node == head || node == before) return false;
  // Unlink.
  const uint32_t p = prev(node), n = next(node);
  W32(base, p + 4, n);
  W32(base, n + 0, p);
  // Link in after `before`.
  const uint32_t after = next(before);
  W32(base, before + 4, node);
  W32(base, node + 0, before);
  W32(base, node + 4, after);
  W32(base, after + 0, node);
  uint8_t* sel = Host(base, menu + 64);
  const uint8_t ours = sel[last];
  for (int i = last; i > to; --i) sel[i] = sel[i - 1];
  sel[to] = ours;
  return true;
}

// Game row number (menu object +8) <-> list row.
uint16_t ListRow(uint16_t game_row) {
  if (g_main_row < 0) return game_row;
  if (game_row == 0xFFFF) return uint16_t(g_main_row);
  return game_row >= g_main_row ? uint16_t(game_row + 1) : game_row;
}
uint16_t GameRow(uint16_t list_row) {
  if (g_main_row < 0) return list_row;
  if (list_row == g_main_row) return 0xFFFF;
  return list_row > g_main_row ? uint16_t(list_row - 1) : list_row;
}

void Select(PPCContext& ctx, uint8_t* base, uint32_t row) {
  rex::CallFrame frame(ctx);
  frame.ctx.r3.u64 = row;
  __imp__sub_8228BE98(frame.ctx, base);
}

// List finisher: add our row to the main menu.
PPC_FUNC(sub_8228CAD8) {
  const uint32_t lr = uint32_t(ctx.lr);
  ApplyMinPlayers(base);  // every menu list build (matchmaking starts from menus)
  if (lr == kMpOptionsDone && sr::PlayersBuilding(base)) {
    // MULTIPLAYER > PLAYERS (online_players.cpp) uses the OPTIONS build.
    sr::PlayersFillList(ctx, base);
    __imp__sub_8228CAD8(ctx, base);
    sr::PlayersAfterFinish(ctx, base);
    return;
  }
  if (lr == kMpOptionsDone) {
    // Multiplayer OPTIONS tab: "Player Name: X" after Audio.
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    g_mp_name_row = -1;
    g_mp_add_row = -1;
    g_mp_minp_row = -1;
    const uint32_t menu = R32(base, kMenuPointer);
    if (menu && EnsureTexts(base)) {
      WriteText(base, g_name_text, NameRowText(), kInfoChars * 2);
      g_mp_name_row = int(R16(base, menu + 58));
      AddRow(ctx, base, g_name_text);
      // (Players to Start is in the lobby's GAME list only.)
      // Friends (runtime eos_lan.cpp): this player's code, and Add Friend.
      rex::cvar::SetFlagByName("online_wanted", "true");
      g_code_shown = rex::cvar::GetFlagByName("online_friend_code");
      WriteText(base, g_code_text,
                "Friend Code: " + (g_code_shown.empty() ? std::string("connecting...") : g_code_shown), kInfoChars * 2);
      AddRow(ctx, base, g_code_text, false);
      WriteText(base, g_addfriend_text, "Add Friend", kInfoChars * 2);
      g_mp_add_row = int(R16(base, menu + 58));
      AddRow(ctx, base, g_addfriend_text);
    }
    __imp__sub_8228CAD8(ctx, base);
    if (g_mp_reselect_minp && g_mp_minp_row >= 0) Select(ctx, base, uint32_t(g_mp_minp_row));
    else if (g_mp_reselect && g_mp_name_row >= 0) Select(ctx, base, uint32_t(g_mp_name_row));
    g_mp_reselect = false;
    g_mp_reselect_minp = false;
    return;
  }
  if (lr == kMainMenuDone && CoopLoaded()) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    const uint32_t menu = R32(base, kMenuPointer);
    if (menu && EnsureTexts(base)) {
      const int first = int(R16(base, menu + 58));
      AddRow(ctx, base, g_text[kMainLabel]);
      if (MoveLastRow(base, menu, first, kMainJoinRow)) g_main_row = kMainJoinRow;
      else g_main_row = first;
    }
  }
  __imp__sub_8228CAD8(ctx, base);
  if (lr == kMainMenuDone && g_main_row >= 0) {
    // The builder put the cursor on the game's remembered row before ours
    // was there: put it on the same row in the new order.
    const uint32_t object = g_main_object ? g_main_object : kMainMenuObject;
    Select(ctx, base, ListRow(R16(base, object + 8)));
  }
}

extern "C" void __imp__sub_822903E8(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_822B7328(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_82348750(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_8228F878(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_8228B9D0(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_8216ED48(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_8228CBA0(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_8228CC90(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_82287FC0(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_822C06B8(PPCContext& ctx, uint8_t* base);

namespace {
bool CoopTabOn(uint8_t* base) { return Host(base, kMultiplayerFlag)[0] == 0 && CoopLoaded(); }

void Call0(PPCContext& parent, uint8_t* base, void (*fn)(PPCContext&, uint8_t*)) {
  rex::CallFrame frame(parent);
  fn(frame.ctx, base);
}
void Call1(PPCContext& parent, uint8_t* base, void (*fn)(PPCContext&, uint8_t*), uint64_t r3) {
  rex::CallFrame frame(parent);
  frame.ctx.r3.u64 = r3;
  fn(frame.ctx, base);
}

// The CO-OP tab's list, built the way the OPTIONS tab builds its own
// (sub_822B7328): list start, rows, finisher, first row selected, input on.
void BuildCoopTab(PPCContext& ctx, uint8_t* base) {
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  Call0(ctx, base, __imp__sub_82348750);
  Call0(ctx, base, __imp__sub_8228F878);
  W32(base, 0x82FFE3E0u, 0x8203C19Cu);  // as OPTIONS in the story (no subtitle)
  Host(base, 0x82FFE3DCu)[0] = 0;
  {
    rex::CallFrame frame(ctx);
    frame.ctx.r3.u64 = 0;
    frame.ctx.r4.u64 = 0;
    frame.ctx.r5.u64 = 0;
    frame.ctx.r6.u64 = 1;
    frame.ctx.r7.u64 = 0;
    __imp__sub_8228B9D0(frame.ctx, base);
  }
  const uint32_t menu = R32(base, kMenuPointer);
  g_pause_first = menu ? int(R16(base, menu + 58)) : 0;
  const int s = CoopState();
  g_pause_actions.clear();
  if (s & 3) g_pause_actions = {kStop};
  else g_pause_actions = {kHost, kJoin};
  // Player name row (only while no session: the name is sent when joining).
  if (!(s & 3)) g_pause_actions.push_back(kName);
  // (Zombies is a multiplayer mode now: lobby GAME list, not the story CO-OP tab.)
  for (Action a : g_pause_actions) {
    if (a == kZombies) {
      WriteText(base, g_zombies_text, ZombiesRunning() ? "End Zombies" : "Start Zombies", kInfoChars * 2);
      AddRow(ctx, base, g_zombies_text);
      continue;
    }
    if (a == kName) {
      WriteText(base, g_name_text, NameRowText(), kInfoChars * 2);
      AddRow(ctx, base, g_name_text);
      continue;
    }
    AddRow(ctx, base, g_text[a == kHost ? 0 : a == kJoin ? 1 : (s & 1) ? 2 : 3]);
  }
  // During a session: the join code / IP (and whether the other player is
  // in) as a greyed row under End Co-op.
  const std::string info = (s & 3) ? CoopInfo() : std::string();
  if (!info.empty() && g_info_text) {
    WriteText(base, g_info_text, info, kInfoChars * 2);
    AddRow(ctx, base, g_info_text, false);
  }
  {
    float f;
    const uint32_t bits = R32(base, 0x820875ECu);
    std::memcpy(&f, &bits, 4);
    rex::CallFrame frame(ctx);
    frame.ctx.r3.u64 = 0xFFFFFFFFFFFFFFFFull;
    frame.ctx.r4.u64 = 0xFFFFFFFFFFFFFFFFull;
    frame.ctx.r6.u64 = 1;
    frame.ctx.f1.f64 = double(f);
    __imp__sub_8228CAD8(frame.ctx, base);
  }
  Call1(ctx, base, __imp__sub_8228BE98, 0);
  W32(base, 0x8370DDCCu, 1);
  const uint32_t input = 0x83710690u;
  W32(base, input + 88, 0);
  W32(base, input + 96, 0);
  for (uint32_t o : {32u, 33u, 34u, 35u, 36u, 37u, 72u, 73u, 74u, 75u, 76u, 77u}) Host(base, input + o)[0] = 1;
  REXLOG_INFO("Co-op menu: CO-OP tab built ({} rows)", g_pause_actions.size() + (info.empty() ? 0 : 1));
}

// The CO-OP tab's update, as the OPTIONS tab's (sub_822B7D68): A on a row,
// up / down, then the pause menu's shared update (tabs, back).
void UpdateCoopTab(PPCContext& ctx, uint8_t* base) {
  if (g_name_changed && !g_dialog_open.load()) {
    g_name_changed = false;
    W32(base, kMenuRequested, kCoopId);  // build the tab again (new name)
  }
  {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    const uint32_t menu = R32(base, kMenuPointer);
    bool acted = false;
    if (menu && !g_dialog_open.load() && g_pause_first >= 0 &&
        g_pause_first + int(g_pause_actions.size()) <= int(R16(base, menu + 58))) {
      // A row that no longer fits the session state is greyed out.
      const bool active = (CoopState() & 3) != 0;
      for (size_t i = 0; i < g_pause_actions.size(); ++i)
        Host(base, menu + 64 + uint32_t(g_pause_first) + uint32_t(i))[0] =
            g_pause_actions[i] == kName      ? uint8_t(!active)
            : g_pause_actions[i] == kZombies ? uint8_t(!(CoopState() & 2))  // not while joined to someone
                                             : uint8_t((g_pause_actions[i] == kStop) == active);
      if (ConfirmPressed(ctx, base)) {
        acted = true;
        const int row = int(R16(base, menu + 62)) - g_pause_first;
        if (row >= 0 && row < int(g_pause_actions.size()) &&
            Host(base, menu + 64 + uint32_t(g_pause_first + row))[0]) {
          const Action a = g_pause_actions[size_t(row)];
          REXLOG_INFO("Co-op menu: {}", a == kHost ? "host" : a == kJoin ? "join" : a == kName ? "name" : a == kZombies ? "zombies" : "end");
          {
            rex::CallFrame frame(ctx);
            frame.ctx.r3.u64 = R32(base, 0x827B05B8u);  // the menu's select sound
            frame.ctx.r4.u64 = 1;
            frame.ctx.r5.u64 = 1;
            __imp__sub_82287FC0(frame.ctx, base);
          }
          if (a == kHost) StartAfterResume(1, {});
          else if (a == kStop) {
            CoopStop();
            W32(base, kMenuRequested, kCoopId);  // build the tab again (Host / Join)
          } else if (a == kZombies) {
            ZombiesToggle();
            W32(base, kMenuRequested, kCoopId);  // build the tab again (Start / End Zombies)
          } else if (a == kName) {
            ShowDialog(ctx, base, DialogFor::kName);
          } else ShowDialog(ctx, base, DialogFor::kPause);
        }
      }
    }
    if (!acted && !g_dialog_open.load()) {
      if (GuestBool(ctx, base, __imp__sub_8216ED48, 37) && GuestBool(ctx, base, __imp__sub_82288750, 0))
        Call1(ctx, base, __imp__sub_8228CBA0, 1);
      else if (GuestBool(ctx, base, __imp__sub_8216ED48, 38) && GuestBool(ctx, base, __imp__sub_82288750, 0))
        Call1(ctx, base, __imp__sub_8228CC90, 1);
    }
  }
  Call0(ctx, base, __imp__sub_822C06B8);
}
}  // namespace

// Tab adder: after the story pause menu's OPTIONS tab, add CO-OP (menu id 46
// with the OPTIONS functions, see above).
PPC_FUNC(sub_822903E8) {
  const uint32_t lr = uint32_t(ctx.lr);
  const uint32_t id = ctx.r5.u32;
  __imp__sub_822903E8(ctx, base);
  sr::PlayersAfterTabAdded(ctx, base, lr, id);  // MULTIPLAYER: PLAYERS after LEADERBOARDS
  if (id != kOptionsId || lr != kOptionsTabAdded || !CoopTabOn(base)) return;
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  if (!EnsureTexts(base)) return;
  for (uint32_t i = 0; i < 4; ++i)
    W32(base, kMenuTable + kCoopId * 16 + i * 4, R32(base, kMenuTable + kOptionsId * 16 + i * 4));
  rex::CallFrame frame(ctx);
  frame.ctx.r3.u64 = g_text[kTabLabel];
  frame.ctx.r4.u64 = 0;
  frame.ctx.r5.u64 = kCoopId;
  __imp__sub_822903E8(frame.ctx, base);
}

// OPTIONS build, also used by the CO-OP tab.
PPC_FUNC(sub_822B7328) {
  const uint32_t requested = R32(base, kMenuRequested);
  if (requested == kCoopId && CoopLoaded()) {
    BuildCoopTab(ctx, base);
    return;
  }
  __imp__sub_822B7328(ctx, base);
}

// OPTIONS update, also used by the CO-OP tab.
PPC_FUNC(sub_822B7D68) {
  const uint32_t current = R32(base, kMenuCurrent);
  if (current == kCoopId && CoopLoaded()) {
    UpdateCoopTab(ctx, base);
    return;
  }
  __imp__sub_822B7D68(ctx, base);
}

// Multiplayer OPTIONS update: A on our row opens the name box (the game's
// handler would only play the select sound for it).
extern "C" void __imp__sub_82349018(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_82349018) {
  if (sr::LobbiesCurrent(base)) {
    // MULTIPLAYER > LOBBIES: refresh the list; A joins the lobby under the
    // cursor (the OPTIONS handler would open Controls / Display / Audio).
    sr::LobbiesUpdate(ctx, base);
    if (MainConfirmPressed(ctx, base)) {
      sr::LobbiesConfirm(base);
      return;
    }
    __imp__sub_82349018(ctx, base);
    return;
  }
  if (sr::PlayersCurrent(base)) {
    // MULTIPLAYER > PLAYERS: refresh the list; A on a greyed info row would
    // open the OPTIONS sub menus (Controls / Display / Audio), so it's eaten.
    sr::PlayersUpdate(ctx, base);
    const uint32_t menu = R32(base, kMenuPointer);
    if (menu && R16(base, menu + 62) < 3 && MainConfirmPressed(ctx, base)) return;
    __imp__sub_82349018(ctx, base);
    return;
  }
  {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    const uint32_t menu = R32(base, kMenuPointer);
    if (R32(base, kMenuCurrent) == kMpOptionsId && menu && g_mp_name_row >= 0 &&
        g_mp_name_row < int(R16(base, menu + 58)) && !g_dialog_open.load()) {
      if (g_name_changed) {
        g_name_changed = false;
        g_mp_reselect = true;
        W32(base, kMenuRequested, kMpOptionsId);  // build the list again (new name)
      } else if (rex::cvar::GetFlagByName("online_friend_code") != g_code_shown) {
        W32(base, kMenuRequested, kMpOptionsId);  // the friend code arrived: show it
      } else if (g_mp_minp_row >= 0 && R16(base, menu + 62) == uint16_t(g_mp_minp_row) &&
                 MainConfirmPressed(ctx, base)) {
        // A: 2 -> 3 -> ... -> 12 -> 2. The row's text is written in place;
        // the list is built again so the new text shows.
        int v = MinPlayers() + 1;
        if (v > kMinPlayersHigh) v = kMinPlayersLow;
        SetMinPlayers(v);
        ApplyMinPlayers(base);
        {
          rex::CallFrame frame(ctx);
          frame.ctx.r3.u64 = R32(base, 0x827B05B8u);  // the menu's select sound
          frame.ctx.r4.u64 = 1;
          frame.ctx.r5.u64 = 1;
          __imp__sub_82287FC0(frame.ctx, base);
        }
        WriteText(base, g_minp_text, MinPlayersRowText(), kInfoChars * 2);
        g_mp_reselect_minp = true;
        W32(base, kMenuRequested, kMpOptionsId);
        return;
      } else if (g_mp_add_row >= 0 && R16(base, menu + 62) == uint16_t(g_mp_add_row) && MainConfirmPressed(ctx, base)) {
        REXLOG_INFO("Co-op menu: add friend (multiplayer options)");
        {
          rex::CallFrame frame(ctx);
          frame.ctx.r3.u64 = R32(base, 0x827B05B8u);  // the menu's select sound
          frame.ctx.r4.u64 = 1;
          frame.ctx.r5.u64 = 1;
          __imp__sub_82287FC0(frame.ctx, base);
        }
        ShowDialog(ctx, base, DialogFor::kFriend);
        return;
      } else if (R16(base, menu + 62) == uint16_t(g_mp_name_row) && MainConfirmPressed(ctx, base)) {
        REXLOG_INFO("Co-op menu: name (multiplayer options)");
        {
          rex::CallFrame frame(ctx);
          frame.ctx.r3.u64 = R32(base, 0x827B05B8u);  // the menu's select sound
          frame.ctx.r4.u64 = 1;
          frame.ctx.r5.u64 = 1;
          __imp__sub_82287FC0(frame.ctx, base);
        }
        ShowDialog(ctx, base, DialogFor::kName);
        return;
      }
    }
  }
  __imp__sub_82349018(ctx, base);
}

// Multiplayer lobby, GAME list (Ranked / Player Match lobbies): "Players to
// Start: N" as the last row. The list is built by sub_82399828 (rows from
// the row-type table 0x8307B318, 4 bytes per row, count byte 0x8370F292) and
// run by the lobby update sub_823963D0 (A on a row looks its type up by the
// cursor: 5 / 11 open sub pages; 3 does nothing anywhere, so our row gets
// type 3 in the slot after the game's rows). A on our row is handled here.
constexpr uint32_t kLobbyKind = 0x827AD564u;      // 0 custom, 1 player match, 2 ranked, 3 system link
constexpr uint32_t kLobbyRowTypes = 0x8307B318u;  // u32 per row (room for 64)
bool MatchmadeLobby(uint8_t* base) {
  const uint32_t kind = R32(base, kLobbyKind);
  return kind == 1 || kind == 2;
}
// Our rows are selectors like the game's own value rows (Mode etc.): an item
// of the selector class (vtable 0x82068024, see options_menu.cpp) in guest
// memory, +16 choice texts, +784 count, +788 the chosen one; left / right go
// to the selector's routines sub_82292CC8 / sub_82292D68 (what the lobby uses
// for its own value rows).
//  - "Players to Start" (Player Match / Ranked lobbies): 2 .. 12.
//  - "Zombies" (every lobby): Off / On, handed to the Zombies mod
//    (core\Zombies, export ZombiesSetMatch); the host's choice turns the
//    match into a zombie game.
constexpr uint32_t kSelectorVtable = 0x82068024u;
constexpr uint32_t kDefaultChoiceColour = 0x82816CF0u;
constexpr uint32_t kSelectorSize = 1024;
struct LobbySelector {
  const char* label;
  std::vector<std::string> choices;
  int (*get)();          // chosen index now
  void (*set)(int);      // a new index was chosen
  bool (*shown)(uint8_t*);
  uint32_t item = 0, label_text = 0;
  int row = -1;
};
void ZombiesSetMatch(bool on) {
  if (auto f = ZombiesFunction<void (*)(int)>("ZombiesSetMatch")) f(on ? 1 : 0);
}
LobbySelector g_lobby_selectors[] = {
    {"Players to Start", {"1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12"},
     [] { return MinPlayers() - kMinPlayersLow; },
     [](int i) {
       SetMinPlayers(i + kMinPlayersLow);
       REXLOG_INFO("Multiplayer lobby: players to start {}", i + kMinPlayersLow);
     },
     [](uint8_t* base) { return MatchmadeLobby(base); }},
};
uint32_t g_lobby_selectors_menu = 0;
bool EnsureLobbyItems(uint8_t* base) {
  for (auto& sel : g_lobby_selectors) {
    if (sel.item) continue;
    const uint32_t block = REX_KERNEL_STATE()->memory()->SystemHeapAlloc(kSelectorSize + 64 + uint32_t(sel.choices.size()) * 16);
    if (!block) return false;
    sel.item = block;
    sel.label_text = block + kSelectorSize;
    WriteText(base, sel.label_text, sel.label, 64);
  }
  return true;
}
void BuildLobbyItem(uint8_t* base, LobbySelector& sel) {
  const uint32_t item = sel.item;
  std::memset(Host(base, item), 0, kSelectorSize);
  W32(base, item + 0, kSelectorVtable);
  const uint32_t colour = R32(base, kDefaultChoiceColour);
  for (uint32_t i = 0; i < 64; ++i) {
    W32(base, item + 272 + i * 4, 0xFFFFFFFFu);
    W32(base, item + 528 + i * 4, colour);
  }
  const uint32_t texts = sel.label_text + 64;
  for (size_t i = 0; i < sel.choices.size(); ++i) {
    WriteText(base, texts + uint32_t(i) * 16, sel.choices[i], 16);
    W32(base, item + 16 + uint32_t(i) * 4, texts + uint32_t(i) * 16);
  }
  W32(base, item + 784, uint32_t(sel.choices.size()));
  W32(base, item + 788, uint32_t(std::clamp(sel.get(), 0, int(sel.choices.size()) - 1)));
  Host(base, item + 792)[0] = 1;
}
extern "C" void __imp__sub_82399828(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_82399828) {
  __imp__sub_82399828(ctx, base);
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  g_lobby_selectors_menu = 0;
  for (auto& sel : g_lobby_selectors) sel.row = -1;
  const uint32_t menu = R32(base, kMenuPointer);
  if (!menu || !EnsureLobbyItems(base)) return;
  for (auto& sel : g_lobby_selectors) {
    if (!sel.shown(base)) continue;
    const int row = int(R16(base, menu + 58));
    if (row >= 64) break;
    BuildLobbyItem(base, sel);
    W32(base, kLobbyRowTypes + uint32_t(row) * 4, 3);
    rex::CallFrame frame(ctx);
    frame.ctx.r3.u64 = sel.label_text;
    frame.ctx.r4.u64 = sel.item;
    frame.ctx.r5.u64 = 0;
    frame.ctx.r6.u64 = 0xFFFFFFFFFFFFFFFFull;
    frame.ctx.r7.u64 = 1;
    frame.ctx.r8.u64 = 0;
    __imp__sub_8228BAB0(frame.ctx, base);
    if (int(R16(base, menu + 58)) == row + 1) sel.row = row;
  }
  g_lobby_selectors_menu = menu;
}
// Zombies as a game mode: an extra entry in the lobby's Mode selector,
// between the last mode and the first (Gangsta Brawl). The game's own modes
// stay untouched underneath: while "Zombies" shows, the game's mode is the
// first one (the selector stays on index 0 and only its text is swapped), and
// the Zombies mod is told to run the match as a zombie game. The Mode row is
// recognised by its choices (one is "Gangsta Brawl") when the row adder adds
// it (options_menu.cpp calls LobbyRowAdded).
uint32_t g_mode_item = 0, g_mode_menu = 0;
int g_mode_row = -1;
bool g_zombie_mode = false;
uint32_t g_mode_text_saved = 0;  // Gangsta Brawl's own text pointer while "Zombies" shows
uint32_t g_mode_text_index = 0;
uint32_t g_zombies_label = 0;
bool g_mode_was_gb = false;
bool ItemHasChoice(uint8_t* base, uint32_t item, const char* want) {
  const uint32_t n = R32(base, item + 784);
  if (n == 0 || n > 64) return false;
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t t = R32(base, item + 16 + i * 4);
    if (t < 0x40000000u) continue;
    size_t k = 0;
    for (; want[k]; ++k)
      if (R16(base, t + uint32_t(k) * 2) != uint16_t(uint8_t(want[k]))) break;
    if (!want[k] && R16(base, t + uint32_t(k) * 2) == 0) return true;
  }
  return false;
}
bool TextIs(uint8_t* base, uint32_t t, const char* want) {
  if (t < 0x40000000u) return false;
  size_t k = 0;
  for (; want[k]; ++k)
    if (R16(base, t + uint32_t(k) * 2) != uint16_t(uint8_t(want[k]))) return false;
  return R16(base, t + uint32_t(k) * 2) == 0;
}
// The text the Mode row shows now (the game keeps one choice: the current mode).
bool ModeShows(uint8_t* base, const char* want) {
  const uint32_t n = R32(base, g_mode_item + 784), i = R32(base, g_mode_item + 788);
  if (n == 0 || n > 64 || i >= n) return false;
  const uint32_t t = (g_mode_text_saved && i == g_mode_text_index) ? g_mode_text_saved : R32(base, g_mode_item + 16 + i * 4);
  return TextIs(base, t, want);
}
// Shows "Zombies" in place of the current mode's text (on) or puts the
// game's text back (off); tells the Zombies mod.
void ShowZombieMode(uint8_t* base, bool on) {
  if (on && RankedLobby(base)) on = false;  // Zombies is never a ranked mode
  if (!g_mode_item) return;
  const uint32_t i = R32(base, g_mode_item + 788);
  if (on) {
    if (!g_zombies_label) {
      g_zombies_label = REX_KERNEL_STATE()->memory()->SystemHeapAlloc(32);
      if (!g_zombies_label) return;
      WriteText(base, g_zombies_label, "Zombies", 32);
    }
    if (i < 64 && R32(base, g_mode_item + 16 + i * 4) != g_zombies_label) {
      g_mode_text_saved = R32(base, g_mode_item + 16 + i * 4);
      g_mode_text_index = i;
      W32(base, g_mode_item + 16 + i * 4, g_zombies_label);
    }
  } else if (g_mode_text_saved && g_mode_text_index < 64 &&
             R32(base, g_mode_item + 16 + g_mode_text_index * 4) == g_zombies_label) {
    W32(base, g_mode_item + 16 + g_mode_text_index * 4, g_mode_text_saved);
    g_mode_text_saved = 0;
  }
  if (g_zombie_mode != on) {
    g_zombie_mode = on;
    REXLOG_INFO("Multiplayer lobby: mode {}", on ? "Zombies" : "back to the game's modes");
    ZombiesSetMatch(on);
  }
}

void sr::LobbyRowAdded(uint8_t* base, uint32_t label, uint32_t item, int row) {
  // The game's own selectors live in its image (0x82000000-0x84160000).
  if (item < 0x82000000u || item > 0x84160000u - 800 || R32(base, item) != kSelectorVtable || !ZombiesLoaded()) return;
  const bool by_label = TextIs(base, label, "Mode");
  if (!by_label && !ItemHasChoice(base, item, "Gangsta Brawl")) return;
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  {
    static uint32_t last_item = 0, last_menu = 0;
    static int last_row = -2;
    const uint32_t menu_now = R32(base, kMenuPointer);
    if (item != last_item || row != last_row || menu_now != last_menu) {
      last_item = item; last_row = row; last_menu = menu_now;
      std::string choices;
      const uint32_t n = R32(base, item + 784);
      for (uint32_t k = 0; k < n && k < 12; ++k) {
        const uint32_t t = R32(base, item + 16 + k * 4);
        std::string one;
        for (uint32_t c = 0; t >= 0x40000000u && c < 30; ++c) {
          const uint16_t ch = R16(base, t + c * 2);
          if (!ch) break;
          one += char(ch < 128 ? ch : '?');
        }
        choices += (k ? ", " : "") + one;
      }
      REXLOG_INFO("Multiplayer lobby: Mode row {} ({}), item {:08X}, lobby kind {}, {} choice(s), current {}: {}", row,
                  by_label ? "label Mode" : "Gangsta Brawl choice", item, R32(base, kLobbyKind), n,
                  R32(base, item + 788), choices);
    }
  }
  // Ranked: no Zombies entry in the Mode list (and none left over from another lobby).
  if (RankedLobby(base)) {
    if (g_zombie_mode) {
      g_mode_item = item;
      ShowZombieMode(base, false);
    }
    g_mode_item = 0;
    g_mode_row = -1;
    g_mode_text_saved = 0;
    return;
  }
  g_mode_item = item;
  g_mode_row = row;
  g_mode_menu = R32(base, kMenuPointer);
  // The list was built again. If the selector still carries our text, it
  // wasn't refilled; otherwise the game wrote its own text again.
  const uint32_t i = R32(base, item + 788);
  const bool ours = i < 64 && g_zombies_label && R32(base, item + 16 + i * 4) == g_zombies_label;
  if (!ours) g_mode_text_saved = 0;
  if (g_zombie_mode) {
    if (ours || ModeShows(base, "Gangsta Brawl")) ShowZombieMode(base, true);  // still Zombies
    else ShowZombieMode(base, false);  // the mode was changed elsewhere (party leader / host)
  } else if (ours) {
    ShowZombieMode(base, false);
  }
  g_mode_was_gb = ModeShows(base, "Gangsta Brawl");
}

extern "C" void __imp__sub_823963D0(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_82292CC8(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_82292D68(PPCContext& ctx, uint8_t* base);
// Hands the game one press of a menu input (39 left, 40 right): the press
// test sub_8216ED48(code) reports bits of a mask (two bit numbers per code in
// the table at 0x829CC818, 16 bytes each, -1 = none) that are set in both
// +84 (pressed) and +88 (not used yet) of the input state 0x83710690.
void InjectPress(uint8_t* base, uint32_t code) {
  const uint32_t entry = 0x829CC818u + code * 16;
  uint32_t mask = 0;
  for (uint32_t k = 0; k < 2; ++k) {
    const int32_t bit = int32_t(R32(base, entry + k * 4));
    if (bit >= 0 && bit < 32) mask |= 1u << bit;
  }
  W32(base, 0x83710690u + 84, R32(base, 0x83710690u + 84) | mask);
  W32(base, 0x83710690u + 88, R32(base, 0x83710690u + 88) | mask);
}

// The row at `index` of a list menu (+0 first row; rows are a ring, +4 next)
// and the item the row adder gave it (row +16, sub_8228BAB0). The lobby's
// sub pages (Match Settings and the others, run by this same update with the
// page number at lobby +16) reuse the list, so the cursor being on row 1 or
// on the last row doesn't mean it is on the Mode or Players to Start row:
// left / right on the settings page's second row used to switch Zombies on
// and off (and the game's mode / map with it) instead of changing that
// setting - the "custom settings menu stuck" bug.
uint32_t RowItemAt(uint8_t* base, uint32_t menu, int index) {
  if (!menu || index < 0) return 0;
  const int rows = int(R16(base, menu + 58));
  if (index >= rows || rows > 256) return 0;
  uint32_t row = R32(base, menu + 0);
  for (int i = 0; row && i < index; ++i) row = R32(base, row + 4);
  return row ? R32(base, row + 16) : 0;
}

std::atomic<int64_t> g_lobby_update_ms{0}, g_mm_update_ms{0};
int64_t SteadyMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

PPC_FUNC(sub_823963D0) {
  bool call_game = true;
  bool mode_right = false, mode_to_first = false;
  {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    const uint32_t menu = R32(base, kMenuPointer);
    // Peek at left / right without using them up: the press tests
    // (sub_8216ED48 -> sub_8216E128) clear what they report in the input
    // state at 0x83710690, so it's put back afterwards and the game's own
    // rows (Type, Mode) still get the press.
    constexpr uint32_t kInputState = 0x83710690u;
    uint8_t saved_input[128];
    std::memcpy(saved_input, Host(base, kInputState), sizeof(saved_input));
    g_lobby_update_ms = SteadyMs();
    const int cursor = menu ? int(R16(base, menu + 62)) : -1;
    const uint32_t cursor_item = RowItemAt(base, menu, cursor);
    const bool on_mode_row = g_mode_item && menu == g_mode_menu && cursor == g_mode_row && cursor_item == g_mode_item;
    bool ours = on_mode_row;
    if (menu && menu == g_lobby_selectors_menu)
      for (auto& sel : g_lobby_selectors) ours = ours || (sel.row >= 0 && cursor == sel.row && cursor_item == sel.item);
    const bool input = ours && !g_dialog_open.load() && GuestBool(ctx, base, __imp__sub_82288750, 0);
    const bool left = input && GuestBool(ctx, base, __imp__sub_8216ED48, 39);
    const bool right = input && !left && GuestBool(ctx, base, __imp__sub_8216ED48, 40);
    std::memcpy(Host(base, kInputState), saved_input, sizeof(saved_input));
    // Mode row: the game cycles its modes itself (the selector holds the
    // current mode only). Zombies sits right before Gangsta Brawl: left from
    // Gangsta Brawl shows Zombies, right from Zombies shows Gangsta Brawl
    // again (the game doesn't move for either), left from Zombies goes on to
    // the game's last mode, and right from the last mode (the game wraps to
    // Gangsta Brawl) lands on Zombies.
    if (on_mode_row && g_mode_row >= 0 && g_mode_row < int(R16(base, menu + 58)) &&
        R32(base, g_mode_item) == kSelectorVtable && (left || right)) {
      REXLOG_INFO("Multiplayer lobby: {} on the Mode row (zombies {}, shows Gangsta Brawl {})", left ? "left" : "right",
                  g_zombie_mode, ModeShows(base, "Gangsta Brawl"));
      if (g_zombie_mode && right) { ShowZombieMode(base, false); g_mode_was_gb = true; call_game = false; }
      else if (!g_zombie_mode && left && ModeShows(base, "Gangsta Brawl")) { ShowZombieMode(base, true); g_mode_was_gb = true; call_game = false; }
      else if (g_zombie_mode && left) ShowZombieMode(base, false);
      else if (right && !g_zombie_mode && R32(base, g_mode_item + 784) > 1 &&
               R32(base, g_mode_item + 788) + 1 == R32(base, g_mode_item + 784)) {
        // Right on the last mode (the selector doesn't wrap): Zombies. The
        // game's mode must become Gangsta Brawl (the first) underneath: the
        // selector is put on the second mode and the game is handed one
        // left press, so it applies the first mode its own way.
        mode_to_first = true;
      }
      else if (right) { mode_right = true; g_mode_was_gb = ModeShows(base, "Gangsta Brawl"); }
    }
    // Our own selector rows (Players to Start).
    if (menu && menu == g_lobby_selectors_menu) {
      for (auto& sel : g_lobby_selectors) {
        if (sel.row < 0 || int(R16(base, menu + 58)) <= sel.row) continue;
        if (RowItemAt(base, menu, sel.row) != sel.item) continue;  // a sub page uses the list now
        if (cursor == sel.row && (left || right)) {
          rex::CallFrame frame(ctx);
          frame.ctx.r3.u64 = sel.item;
          frame.ctx.r4.u64 = 1;
          if (left) __imp__sub_82292CC8(frame.ctx, base);
          else __imp__sub_82292D68(frame.ctx, base);
        }
        // Apply whatever the selector shows now (keys or mouse on the arrows).
        const int chosen = int(R32(base, sel.item + 788));
        if (chosen >= 0 && chosen < int(sel.choices.size()) && chosen != sel.get()) sel.set(chosen);
      }
      ApplyMinPlayers(base);
    }
  }
  if (!call_game) return;
  if (mode_to_first) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    W32(base, g_mode_item + 788, 1);
    InjectPress(base, 39);  // left
    __imp__sub_823963D0(ctx, base);
    if (ModeShows(base, "Gangsta Brawl")) ShowZombieMode(base, true);
    REXLOG_INFO("Multiplayer lobby: right from the last mode -> Zombies (game mode now {})",
                ModeShows(base, "Gangsta Brawl") ? "Gangsta Brawl" : "not Gangsta Brawl");
    return;
  }
  __imp__sub_823963D0(ctx, base);
  if (mode_right) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (!g_zombie_mode && !g_mode_was_gb && ModeShows(base, "Gangsta Brawl")) ShowZombieMode(base, true);
  }
}

// Zombies texts instead of Gangsta Brawl's while the lobby has Zombies: the
// string lookup (sub_8251EBC8(key, default) -> UTF-16 text) for the loading
// screen's line (MP_GB_LOAD) and the in-game welcome (MULTI_GB_WELCOME), and
// the loading screen's title (sub_82374250 r8, called from sub_82375AA0).
uint32_t ZombieText(uint8_t* base, int which) {
  static uint32_t texts[3] = {};
  static const char* const kTexts[3] = {
      "Zombies",
      "Survive round after round of zombies. Every hit on a zombie earns points - the timer shows when the next round starts.",
      "Zombies: Survive the rounds - every hit on a zombie earns points!"};
  if (!texts[which]) {
    const uint32_t size = uint32_t(std::strlen(kTexts[which]) + 1) * 2;
    texts[which] = REX_KERNEL_STATE()->memory()->SystemHeapAlloc(size);
    if (texts[which]) WriteText(base, texts[which], kTexts[which], size * 2);
  }
  return texts[which];
}
bool KeyIs(uint8_t* base, uint32_t key, const char* want) {
  if (key < 0x40000000u) return false;
  const uint8_t* p = Host(base, key);
  return std::strncmp(reinterpret_cast<const char*>(p), want, std::strlen(want) + 1) == 0;
}
extern "C" void __imp__sub_8251EBC8(PPCContext& ctx, uint8_t* base);
// A text of our own in guest memory, rewritten when it changes (one slot per use).
uint32_t DynamicText(uint8_t* base, int slot, const std::string& text) {
  static uint32_t buf[4] = {};
  static std::string shown[4];
  constexpr uint32_t kChars = 64;
  if (slot < 0 || slot >= 4) return 0;
  if (!buf[slot]) buf[slot] = REX_KERNEL_STATE()->memory()->SystemHeapAlloc(kChars * 2);
  if (!buf[slot]) return 0;
  if (shown[slot] != text) {
    WriteText(base, buf[slot], text.substr(0, kChars - 1).c_str(), kChars * 2);
    shown[slot] = text;
  }
  return buf[slot];
}
PPC_FUNC(sub_8251EBC8) {
  const uint32_t key = ctx.r3.u32;
  if (g_zombie_mode && (KeyIs(base, key, "MP_GB_LOAD") || KeyIs(base, key, "MULTI_GB_WELCOME"))) {
    if (const uint32_t t = ZombieText(base, KeyIs(base, key, "MP_GB_LOAD") ? 1 : 2)) {
      ctx.r3.u64 = t;
      return;
    }
  }
  // Zombies: the loading screen doesn't wait for more players (Players to
  // Start), and the scoreboard says which round it is (the Zombies mod's
  // ZombiesRound: -1 no zombie game here, 0 before round 1).
  const int round = [] { auto f = ZombiesFunction<int (*)()>("ZombiesRound"); return f ? f() : -1; }();
  if (g_zombie_mode || round >= 0) {
    const char* text = nullptr;
    std::string dyn;
    int slot = -1;
    if (KeyIs(base, key, "MULTI_MATCH_GAME_COUNT")) { dyn = "Loading The Ultor Dome"; slot = 0; }
    else if (KeyIs(base, key, "MULTI_MATCH_EXIT")) { dyn = "Get ready to survive."; slot = 1; }
    else if (round >= 0 && KeyIs(base, key, "MP_SCOREBOARD_CAPS")) {
      dyn = round > 0 ? "ZOMBIES - ROUND " + std::to_string(round) : "ZOMBIES - GET READY";
      slot = 2;
    } else if (round >= 0 && KeyIs(base, key, "MP_SCORE_CAPS")) { dyn = "POINTS"; slot = 3; }
    (void)text;
    if (slot >= 0)
      if (const uint32_t t = DynamicText(base, slot, dyn)) {
        ctx.r3.u64 = t;
        return;
      }
  }
  __imp__sub_8251EBC8(ctx, base);
}
// The lobby's info panel (sub_82390F98: Type / Mode / Level / Status) copies
// the mode's name from the table at 0x827D5DD8 with sub_82648930(dest, text)
// (return address 0x82391274): "Zombies" instead while it is chosen.
extern "C" void __imp__sub_82648930(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_82648930) {
  if (g_zombie_mode && uint32_t(ctx.lr) == 0x82391274u)
    if (const uint32_t t = ZombieText(base, 0)) ctx.r4.u64 = t;
  __imp__sub_82648930(ctx, base);
}
extern "C" void __imp__sub_82374250(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_82374250) {
  if (g_zombie_mode && uint32_t(ctx.lr) == 0x82375CE8u)
    if (const uint32_t t = ZombieText(base, 0)) ctx.r8.u64 = t;
  __imp__sub_82374250(ctx, base);
}

bool sr::CoopMainMenuPre(PPCContext& ctx, uint8_t* base) {
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  g_main_object = ctx.r3.u32;
  g_main_cursor_before = 0xFFFF;
  if (g_open_load) {
    // Open the load screen like LOAD GAME (see kbm.cpp, World Studio).
    g_open_load = false;
    PPC_STORE_U32(0x827AF780u, 0);
    PPC_STORE_U32(0x8300FD3Cu, 4);
    PPC_STORE_U32(0x8300FD40u, 4);
    PPC_STORE_U32(0x8370DDB0u, 44);
    REXLOG_INFO("Co-op menu: join code set, opening the load screen");
    ctx.r3.u64 = 1;
    return true;
  }
  const uint32_t menu = R32(base, kMenuPointer);
  if (g_main_row < 0 || !g_main_object || !menu || g_main_row >= int(R16(base, menu + 58))) return false;
  const uint16_t cursor = R16(base, menu + 62);
  // The game's A handler reads its own row number (it skips 0xFFFF, ours).
  W16(base, g_main_object + 8, GameRow(cursor));
  g_main_cursor_before = cursor;
  if (cursor == uint16_t(g_main_row) && !g_dialog_open.load() && (CoopState() & 3) == 0 &&
      MainConfirmPressed(ctx, base)) {
    REXLOG_INFO("Co-op menu: join from the main menu");
    ShowDialog(ctx, base, DialogFor::kMainMenu);
  }
  return false;
}

void sr::CoopMainMenuPost(PPCContext& ctx, uint8_t* base) {
  (void)ctx;
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  const uint32_t menu = R32(base, kMenuPointer);
  if (g_main_cursor_before == 0xFFFF || !g_main_object || !menu) return;
  // After moving, the game stores the list cursor as its row number.
  const uint16_t cursor = R16(base, menu + 62);
  if (cursor != g_main_cursor_before) W16(base, g_main_object + 8, GameRow(cursor));
}

void DialogReleasePoll();

bool sr::CoopResumePulse() {
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  if (!g_resume_action || !g_resume_clock) return false;
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - g_resume_since).count();
  return ms >= 100 && ms < 250;
}

// A matchmade game hosted by this player starts its countdown only when the
// player count changes (sub_82362108 on join / leave sets the start time
// [0x8282B708] = clock [0x827AA6E0] + 10 s, 30 s with 4 players); a host
// alone with "Players to Start: 1" waited forever on "Searching for
// additional players".
// Since 2026-10-17 this only acts while the matchmaking screen itself runs
// (its update sub_823647A8 ran in the last second) and only when the game has
// no start time at all. Before, it went by bytes 0x8370EB4F +1606 / +1608,
// which stay set after leaving matchmaking, and it also cut the game's own
// countdown to 5 s whenever that was further away: a match then started
// while other players were still joining, and they ended up searching on
// their own. A host alone now waits 20 s, so searching players can still
// find this game first.
constexpr uint32_t kSoloStartWaitMs = 20000;
constexpr uint32_t kMatchWaitMs = 60000, kMatchCountdownMs = 10000;
void SoloStartPoll(uint8_t* base) {
  constexpr uint32_t kMm = 0x8370EB4Fu, kStartAt = 0x8282B708u, kClock = 0x827AA6E0u;
  ApplyMinPlayers(base);  // keeps ranked at 2+ even if the lobby kind changed after the menus were built
  const bool screen = SteadyMs() - g_mm_update_ms.load() < 1000;
  const uint8_t mm_host = Host(base, kMm + 1606)[0];
  const uint8_t searching = Host(base, kMm + 1608)[0], hosting_mm = Host(base, kMm + 1613)[0];
  const uint32_t count = Host(base, kMm)[0];
  const uint32_t need = R32(base, kMinPlayersVar);
  const int32_t start_at = int32_t(R32(base, kStartAt));
  const uint32_t clock = R32(base, kClock);
  const int64_t left = start_at >= 0 ? int64_t(start_at) - int64_t(clock) : -1;
  if (!screen) {
    g_mm_need_override.store(0);  // left matchmaking: "Players to Start" counts again
    return;
  }
  {
    static auto next = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    if (now >= next) {
      next = now + std::chrono::seconds(5);
      REXLOG_INFO("Matchmaking state: host loop {} searching {} mm-host {} players {} needed {} start at {} (in {} ms)",
                  mm_host, searching, hosting_mm, count, need, start_at, left);
    }
  }
  if (!(mm_host || (searching && hosting_mm)) || !MatchmadeLobby(base)) {
    g_mm_need_override.store(0);
    return;
  }
  // Hosting alone: move into another host waiting for the same match
  // (xgi_live.inc SrMatchmakingMerge decides; one of two lone hosts moves).
  if (count <= 1) {
    static auto next_merge = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    if (now >= next_merge) {
      next_merge = now + std::chrono::seconds(2);
      using MergeFn = int (*)();
      static const MergeFn merge = [] {
        HMODULE m = GetModuleHandleW(L"rexruntime.dll");
        return m ? reinterpret_cast<MergeFn>(GetProcAddress(m, "SrMatchmakingMerge")) : nullptr;
      }();
      if (merge && merge()) return;
    }
  }
  // 2+ players for 60 s: start whatever "Players to Start" says (2026-10-28:
  // hosts on 4 / 7 kept 2-6 players on "Finding matches" forever). The need
  // drops to 2 (the game's own join / leave handling then agrees) and, if the
  // game has no start time, the normal 10 s countdown begins, so late joiners
  // still get in.
  {
    static auto wait_since = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    if (count >= 2 && need > 2) {
      if (wait_since == std::chrono::steady_clock::time_point{}) wait_since = now;
      if (now - wait_since >= std::chrono::milliseconds(kMatchWaitMs)) {
        g_mm_need_override.store(2);
        ApplyMinPlayers(base);
        if (int32_t(R32(base, kStartAt)) < 0) W32(base, kStartAt, (clock + kMatchCountdownMs) % 0x6B49D200u);
        REXLOG_INFO("Matchmaking: {} players waited {} s (Players to Start {}): match starts in {} s", count,
                    kMatchWaitMs / 1000, need, kMatchCountdownMs / 1000);
        wait_since = {};
        return;
      }
    } else if (count < 2) {
      wait_since = {};
    }
  }
  // Only a host alone with "Players to Start: 1": with more players the
  // game's own join / leave handling sets (and keeps) the countdown.
  const uint32_t players = std::max<uint32_t>(count, 1u);
  if (players != 1 || need != 1 || RankedLobby(base)) return;
  if (start_at >= 0) return;  // the game has a start time: never shorten it
  W32(base, kStartAt, (clock + kSoloStartWaitMs) % 0x6B49D200u);
  REXLOG_INFO("Matchmaking: alone with Players to Start 1: match starts in {} s unless someone joins",
              kSoloStartWaitMs / 1000);
}

// The matchmaking screen's update (search, then the host loop sub_823637E0).
extern "C" void __imp__sub_823647A8(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_823647A8) {
  g_mm_update_ms = SteadyMs();
  __imp__sub_823647A8(ctx, base);
}

bool sr::MatchmakingScreenActive() { return SteadyMs() - g_mm_update_ms.load() < 1500; }
bool sr::LobbyScreenActive() { return SteadyMs() - g_lobby_update_ms.load() < 1500; }

// Map packs online: in a public lobby (Player Match / Ranked, lobby kind 1 /
// 2) only disc maps can be picked, so a public match never lands on a map
// another player doesn't have. Level tables (built once, flag 0x8370F22F):
// per mode slot 0..6 a count ([0x83074B1C] + 4 * slot) and 60-byte records
// ([0x83074B20] + 4 * slot); +29 Disabled byte, +32 downloadable-content index
// (-1 = on the disc), +36 id. Map pack levels are switched off while in a
// public lobby and back on outside it (Private Party, System Link, custom).
std::map<uint32_t, uint8_t> g_custom_levels_off;  // record -> its Disabled byte before
uint32_t g_custom_levels_table = 0;
bool CustomLevel(uint8_t* base, uint32_t rec) {
  const int32_t dlc = int32_t(R32(base, rec + 32));
  const uint16_t id = R16(base, rec + 36);
  return dlc != -1 || id >= 1000;  // disc levels: no content index, ids below 1000
}
void PublicLevelFilter(uint8_t* base) {
  const uint32_t kind = R32(base, 0x827AD564u);
  const bool is_public = kind == 1 || kind == 2;
  const bool built = Host(base, 0x8370F22Fu)[0] != 0;
  const uint32_t counts = R32(base, 0x83074B1Cu), records = R32(base, 0x83074B20u);
  if (!built || !counts || !records) return;
  if (records != g_custom_levels_table) {  // the table was built again: old records are gone
    g_custom_levels_off.clear();
    g_custom_levels_table = records;
  }
  if (!is_public) {
    if (g_custom_levels_off.empty()) return;
    for (auto& [rec, was] : g_custom_levels_off)
      if (CustomLevel(base, rec)) Host(base, rec + 29)[0] = was;
    REXLOG_INFO("Map packs: {} level(s) usable again (not a public lobby)", g_custom_levels_off.size());
    g_custom_levels_off.clear();
    return;
  }
  size_t added = 0;
  for (uint32_t slot = 0; slot < 7; ++slot) {
    const uint32_t n = R32(base, counts + slot * 4), recs = R32(base, records + slot * 4);
    if (!recs || n > 128) continue;
    for (uint32_t i = 0; i < n; ++i) {
      const uint32_t rec = recs + i * 60;
      if (!CustomLevel(base, rec)) continue;
      if (!g_custom_levels_off.count(rec)) {
        g_custom_levels_off[rec] = Host(base, rec + 29)[0];
        ++added;
      }
      Host(base, rec + 29)[0] = 1;
    }
  }
  if (added) REXLOG_INFO("Map packs: {} level(s) off in this public lobby (disc maps only)", added);
}

void sr::CoopMenuPoll(uint8_t* base) {
  PublicLevelFilter(base);
  sr::PlayersActivityPoll(base);  // what others see this player doing (PLAYERS tab)
  SoloStartPoll(base);
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  DialogReleasePoll();
  if (g_resume_action && !g_dialog_open.load()) {
    const auto now = std::chrono::steady_clock::now();
    if (!g_resume_clock) {
      g_resume_clock = true;
      g_resume_since = now;
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_resume_since).count();
    const bool menu_open = R32(base, kActiveScreen) != 0;
    if ((!menu_open && ms >= 400) || ms >= 4000) {
      REXLOG_INFO("Co-op menu: {} ({} ms after leaving the menu, menu {})",
                  g_resume_action == 1 ? "hosting" : "joining", ms, menu_open ? "still open" : "closed");
      if (g_resume_action == 1) CoopHost();
      else CoopJoin(g_resume_code);
      g_resume_action = 0;
    }
  }
  if (!g_join_pending) return;
  const auto now = std::chrono::steady_clock::now();
  if (now - g_join_since > std::chrono::minutes(5)) {
    g_join_pending = false;
    REXLOG_INFO("Co-op menu: no save loaded within 5 minutes; join cancelled");
    return;
  }
  // In the game (player exists, no menu, no cutscene) for 3 s in a row.
  const bool playing = R32(base, kPlayer) != 0 && R32(base, kActiveScreen) == 0 &&
                       !Host(base, 0x8370D991u)[0] && !Host(base, 0x8370D990u)[0];
  if (!playing) {
    g_join_ready = {};
    return;
  }
  if (g_join_ready.time_since_epoch().count() == 0) g_join_ready = now;
  if (now - g_join_ready < std::chrono::seconds(3)) return;
  g_join_pending = false;
  REXLOG_INFO("Co-op menu: save loaded, joining");
  CoopJoin(g_join_code);
}

bool sr::CoopDialogOpen() { return g_dialog_open.load(); }

namespace {
void DialogDone(bool ok) {
  std::string code;
  for (char c : g_typed)
    if (c != ' ') code += c;
  if (g_dialog_for == DialogFor::kFriend) {
    if (ok && !code.empty()) rex::cvar::SetFlagByName("online_friend_add", code);
    REXLOG_INFO("Co-op menu: friend code box {}", ok && !code.empty() ? "sent" : "cancelled");
    g_dialog_for = DialogFor::kNone;
    return;
  }
  if (g_dialog_for == DialogFor::kName) {
    std::string name = g_typed;
    while (!name.empty() && name.back() == ' ') name.pop_back();
    while (!name.empty() && name.front() == ' ') name.erase(name.begin());
    if (ok && !name.empty() && !NameAllowed(name)) {
      REXLOG_WARN("Co-op menu: the name '{}' is reserved", name);
      g_name_refused_until = std::chrono::steady_clock::now() + std::chrono::seconds(15);
      g_name_changed = true;
    } else if (ok && !name.empty()) {
      SetPlayerName(name);
      g_name_changed = true;
    }
    REXLOG_INFO("Co-op menu: name box {}", ok && !name.empty() ? "saved" : "cancelled");
    g_dialog_for = DialogFor::kNone;
    return;
  }
  if (ok && !code.empty()) {
    if (g_dialog_for == DialogFor::kPause) {
      StartAfterResume(2, code);
    } else if (g_dialog_for == DialogFor::kMainMenu) {
      g_join_code = code;
      g_join_pending = true;
      g_join_since = std::chrono::steady_clock::now();
      g_join_ready = {};
      g_open_load = true;
    }
  }
  REXLOG_INFO("Co-op menu: code box {}", ok && !code.empty() ? "join" : "cancelled");
  g_dialog_for = DialogFor::kNone;
}
}  // namespace

// The message dialog's update: typing, Enter / Esc for our code box.
PPC_FUNC(sub_8229F288) {
  const uint32_t object = ctx.r3.u32;
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  if (!g_dialog || object != g_dialog) {
    __imp__sub_8229F288(ctx, base);
    return;
  }
  if (g_dialog_state == DialogState::kOpen) {
    const int r = PollTyping();
    if (g_typed_changed) {
      g_typed_changed = false;
      // Wrap the new text again, keeping the box shown as it is.
      uint32_t fade;
      std::memcpy(&fade, Host(base, object + 600), 4);
      const uint8_t shown = Host(base, object + 597)[0];
      WriteText(base, g_dialog_text, DialogBody(), kTextSize);
      rex::CallFrame frame(ctx);
      frame.ctx.r3.u64 = object;
      __imp__sub_8229EF58(frame.ctx, base);
      std::memcpy(Host(base, object + 600), &fade, 4);
      Host(base, object + 597)[0] = shown;
    }
    if (r != 0) {
      DialogDone(r > 0);
      Host(base, object + 597)[0] = 0;  // fade out
      g_dialog_state = DialogState::kClosing;
    }
  }
  __imp__sub_8229F288(ctx, base);
  if (g_dialog_state == DialogState::kClosing && (ctx.r3.u32 & 0xFF) == 0)
    g_dialog_state = DialogState::kWaitRelease;
}

void DialogReleasePoll() {
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  // Menus get the keyboard back once Enter / Esc are let go (or they would
  // take the same press).
  if (g_dialog_state == DialogState::kWaitRelease && !KeyDown(VK_RETURN) && !KeyDown(VK_ESCAPE)) {
    g_dialog_state = DialogState::kClosed;
    g_dialog_open.store(false);
  }
}
