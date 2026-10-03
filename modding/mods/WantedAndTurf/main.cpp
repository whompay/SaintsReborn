// Wanted & Turf - a mod menu (F6) to set, lock or clear the wanted level of
// each faction, change the playa's team allegiance, and hand the turf the
// player is standing in to any gang.
//
// Wanted level: the notoriety entries live on the player object,
// *(0x8309ABEC) + 3868 + faction*24 (+0 level 0-5, +4 min clamp, +8 max clamp,
// +12 progress f32, +16 decay timestamp). The game's own setter is
// sub_821D2F50(entries_base, faction, f1 = level as a float) - what the
// notoriety_set script function calls. It writes the level and the leftover
// progress, re-arms the decay timer, alerts the faction's NPCs when the level
// changed and updates the chase music, so the HUD meter follows.
//
// Locking uses the game's own clamps: min and max both set to the locked level
// means the points pipeline cannot raise it (sub_821D2840 refuses points once
// the level is above the crime's cap, sub_821D2178 clamps to the max) and decay
// cannot lower it. A lock at 0 stars is therefore a "never wanted" switch. The
// original clamps are remembered and restored when the lock is released, so a
// save's story-gated caps are not changed permanently.
//
// Turf: the hood table is `*(uint32_t*)0x8370D098` with `*(int*)0x8370D094`
// entries of 256 bytes each - name inline at +0, owner team id at +72, zone
// volume at +108. sub_821201A8(&position, 0) returns the index of the hood a
// position is inside (-1 for none), which is what
// get_current_hood_by_position / get_current_hood_owner_by_position use. The
// ambient spawn code reads the owner out of that table every time it picks a
// spawn (e.g. sub_824256F8), so writing +72 changes which gang shows up - the
// same store the game's own "give this hood to the Saints" path makes.

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <string>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "wml.h"

namespace {

const WmlApi* api;
const WmlMod* self;

constexpr uint32_t kPlayerPtr = 0x8309ABEC;   // player object (0 outside gameplay)
constexpr uint32_t kMpFlag = 0x8370E9F6;      // multiplayer session flag (byte)
constexpr int kObjPos = 20;                   // object+20: position vec3f
constexpr int kObjTeam = 232;                 // object+232: team id (set_team store)

constexpr int kEntriesBase = 3868;            // player + 3868 + faction*24
constexpr int kEntryStride = 24;
constexpr int kEntryLevel = 0;
constexpr int kEntryMin = 4;
constexpr int kEntryMax = 8;
constexpr int kEntryProgress = 12;
constexpr uint32_t kNotorietySetFn = 0x821D2F50;   // (entries, faction, f1 = level)
constexpr uint32_t kPointsPerLevel = 0x82B2BAB4;   // 5 ints: points to the next level
constexpr uint32_t kTeamNameTable = 0x820387D8;    // [team id] -> cstr

constexpr uint32_t kHoodCountAddr = 0x8370D094;
constexpr uint32_t kHoodTableAddr = 0x8370D098;
constexpr int kHoodStride = 256;
constexpr int kHoodOwner = 72;
constexpr uint32_t kHoodFromPosFn = 0x821201A8;  // (&pos, 0) -> hood index or -1

constexpr int kMaxStars = 5;
constexpr int kFactionCount = 5;  // 0 los_carnales, 1 vice_kings, 2 rollers, 3 players, 4 police

// The factions worth a menu entry, in the order the menu cycles them.
struct FactionOption {
  int faction;
  const char* label;
};
constexpr FactionOption kFactions[] = {
    {4, "Police"},
    {2, "West Side Rollerz"},
    {0, "Los Carnales"},
    {1, "Vice Kings"},
};
constexpr int kFactionOptionCount = int(sizeof(kFactions) / sizeof(kFactions[0]));

// Turf owners the menu can hand a hood to. These are runtime team ids (the
// same ids the team name table at 0x820387D8 is indexed by); -1 = unowned,
// which is what the game reports as "none".
struct OwnerOption {
  int team;
  const char* label;
};
constexpr OwnerOption kOwners[] = {
    {0, "Third Street Saints"},
    {1, "Los Carnales"},
    {2, "Vice Kings"},
    {3, "West Side Rollerz"},
    {-1, "Nobody"},
};
constexpr int kOwnerOptionCount = int(sizeof(kOwners) / sizeof(kOwners[0]));

// Playa allegiance options. Same runtime team ids as the turf owners / the
// team name table; writing player+232 is what the set_team script thunk does.
constexpr OwnerOption kAllegiances[] = {
    {6, "Civilian"},
    {5, "Police"},
    {0, "Third Street Saints"},
    {2, "Vice Kings"},
    {3, "West Side Rollerz"},
    {1, "Los Carnales"},
};
constexpr int kAllegianceOptionCount = int(sizeof(kAllegiances) / sizeof(kAllegiances[0]));

bool g_menu_open = false;
int g_faction_idx = 0;
int g_stars = 3;
int g_owner_idx = 0;
int g_allegiance_idx = 0;
std::string g_status;

// Lock state per faction id. The clamps are snapshotted the first time a
// faction is touched so releasing the lock restores the save's own values.
bool g_locked[kFactionCount] = {};
int g_locked_level[kFactionCount] = {};
bool g_clamps_saved[kFactionCount] = {};
uint32_t g_saved_min[kFactionCount] = {};
uint32_t g_saved_max[kFactionCount] = {};

// Hood under the player, refreshed from the game-update hook (the lookup is a
// guest call, which needs a hook context).
int g_hood_index = -1;
std::string g_hood_name;
int g_hood_owner = -1;

// Menu actions are queued here and run from the game-update hook.
struct Request {
  enum Kind { kApplyLevel, kClearAll, kSetLock, kSetTurf, kSetAllegiance, kDumpTables };
  Kind kind;
  int a;  // faction id / owner team / allegiance team
  int b;  // star level / lock on-off
};
Request g_requests[8];
int g_request_count = 0;

void Queue(Request::Kind kind, int a = 0, int b = 0) {
  if (g_request_count < int(sizeof(g_requests) / sizeof(g_requests[0]))) {
    g_requests[g_request_count++] = {kind, a, b};
  }
}

std::string ReadGuestString(uint32_t addr, int max_len) {
  if (addr < 0x82000000 || addr >= 0x84160000) return {};
  const char* p = static_cast<const char*>(api->guest_pointer(addr));
  std::string out;
  for (int i = 0; i < max_len; ++i) {
    const char c = p[i];
    if (!c) break;
    if (c < 0x20 || c > 0x7E) return {};
    out += c;
  }
  return out;
}

std::string TeamName(int team) {
  if (team < 0) return "Nobody";
  const std::string name = ReadGuestString(api->read_u32(kTeamNameTable + team * 4), 48);
  return name.empty() ? ("team " + std::to_string(team)) : name;
}

uint32_t EntryAddr(uint32_t player, int faction) {
  return player + kEntriesBase + faction * kEntryStride;
}

// Current level of a faction, or -1 when there is no player to read from.
int WantedLevel(int faction) {
  const uint32_t player = api->read_u32(kPlayerPtr);
  if (player <= 0x1000) return -1;
  return int(api->read_u32(EntryAddr(player, faction) + kEntryLevel));
}

// Progress toward the next star, 0-99. Only used for the menu readout.
int WantedPercent(int faction) {
  const uint32_t player = api->read_u32(kPlayerPtr);
  if (player <= 0x1000) return 0;
  const uint32_t entry = EntryAddr(player, faction);
  const uint32_t level = api->read_u32(entry + kEntryLevel);
  if (level >= kMaxStars) return 0;
  const uint32_t next = api->read_u32(kPointsPerLevel + level * 4);
  if (!next) return 0;
  const float pct = api->read_f32(entry + kEntryProgress) * 100.0f / float(next);
  return pct <= 0.0f ? 0 : (pct >= 99.0f ? 99 : int(pct));
}

void SaveClamps(uint32_t player, int faction) {
  if (g_clamps_saved[faction]) return;
  const uint32_t entry = EntryAddr(player, faction);
  g_saved_min[faction] = api->read_u32(entry + kEntryMin);
  g_saved_max[faction] = api->read_u32(entry + kEntryMax);
  g_clamps_saved[faction] = true;
}

// Calls the game's own notoriety setter, so NPC alerts, the chase music and
// the HUD meter all react the way they do for a scripted wanted level.
void CallSetLevel(WmlContext* ctx, uint32_t player, int faction, int level) {
  api->set_r(ctx, 3, player + kEntriesBase);
  api->set_r(ctx, 4, uint64_t(uint32_t(faction)));
  api->set_f(ctx, 1, double(level));
  if (api->call(ctx, kNotorietySetFn) != 0) {
    api->log(self, "notoriety setter call failed");
  }
}

void ApplyLevel(WmlContext* ctx, uint32_t player, int faction, int level) {
  SaveClamps(player, faction);
  const uint32_t entry = EntryAddr(player, faction);
  // The clamps have to allow the level or the game pulls it back: a save's max
  // is story-gated (often 1) and the min is normally 0.
  if (int(api->read_u32(entry + kEntryMax)) < level) api->write_u32(entry + kEntryMax, uint32_t(level));
  if (int(api->read_u32(entry + kEntryMin)) > level) api->write_u32(entry + kEntryMin, uint32_t(level));
  CallSetLevel(ctx, player, faction, level);
}

void RestoreClamps(uint32_t player, int faction) {
  if (!g_clamps_saved[faction]) return;
  const uint32_t entry = EntryAddr(player, faction);
  api->write_u32(entry + kEntryMin, g_saved_min[faction]);
  api->write_u32(entry + kEntryMax, g_saved_max[faction]);
  g_clamps_saved[faction] = false;
}

// A locked faction gets min == max == the locked level, which blocks both the
// points pipeline and the decay. The level is re-applied through the setter if
// anything managed to move it anyway.
void EnforceLocks(WmlContext* ctx, uint32_t player) {
  for (int f = 0; f < kFactionCount; ++f) {
    if (!g_locked[f]) continue;
    const uint32_t entry = EntryAddr(player, f);
    const uint32_t level = uint32_t(g_locked_level[f]);
    api->write_u32(entry + kEntryMin, level);
    api->write_u32(entry + kEntryMax, level);
    if (api->read_u32(entry + kEntryLevel) != level) {
      CallSetLevel(ctx, player, f, g_locked_level[f]);
    } else if (api->read_f32(entry + kEntryProgress) != 0.0f) {
      api->write_f32(entry + kEntryProgress, 0.0f);
    }
  }
}

void RefreshHood(WmlContext* ctx, uint32_t player) {
  api->set_r(ctx, 3, player + kObjPos);
  api->set_r(ctx, 4, 0);
  if (api->call(ctx, kHoodFromPosFn) != 0) return;
  const int index = int(uint32_t(api->get_r(ctx, 3)));
  const uint32_t table = api->read_u32(kHoodTableAddr);
  if (index < 0 || index >= int(api->read_u32(kHoodCountAddr)) || table <= 0x1000) {
    g_hood_index = -1;
    g_hood_name.clear();
    g_hood_owner = -1;
    return;
  }
  const uint32_t record = table + uint32_t(index) * kHoodStride;
  g_hood_index = index;
  g_hood_name = ReadGuestString(record, 63);
  g_hood_owner = int(api->read_u32(record + kHoodOwner));
}

void SetTurfOwner(int owner_team) {
  const uint32_t table = api->read_u32(kHoodTableAddr);
  if (g_hood_index < 0 || table <= 0x1000) {
    g_status = "not inside a turf right now";
    return;
  }
  const uint32_t record = table + uint32_t(g_hood_index) * kHoodStride;
  api->write_u32(record + kHoodOwner, uint32_t(owner_team));
  g_hood_owner = owner_team;
  char line[192];
  snprintf(line, sizeof(line), "turf \"%s\" (hood %d) now belongs to %s", g_hood_name.c_str(),
           g_hood_index, TeamName(owner_team).c_str());
  api->log(self, line);
  g_status = std::string("turf given to ") + TeamName(owner_team) + " (new spawns follow it)";
}

void SetPlayerAllegiance(uint32_t player, int team) {
  const int old_team = int(api->read_u32(player + kObjTeam));
  api->write_u32(player + kObjTeam, uint32_t(team));
  char line[160];
  snprintf(line, sizeof(line), "playa allegiance %s -> %s (team %d -> %d)", TeamName(old_team).c_str(),
           TeamName(team).c_str(), old_team, team);
  api->log(self, line);
  g_status = std::string("allegiance set to ") + TeamName(team);
}

int AllegianceIndexForTeam(int team) {
  for (int i = 0; i < kAllegianceOptionCount; ++i) {
    if (kAllegiances[i].team == team) return i;
  }
  return 0;
}

void DumpTables(uint32_t player) {
  char line[224];
  api->log(self, "--- teams (runtime id order) ---");
  for (int team = 0; team < 8; ++team) {
    snprintf(line, sizeof(line), "  team %d = \"%s\"", team, TeamName(team).c_str());
    api->log(self, line);
  }
  api->log(self, "--- notoriety (player object) ---");
  for (int f = 0; f < kFactionCount; ++f) {
    const uint32_t entry = EntryAddr(player, f);
    snprintf(line, sizeof(line), "  faction %d: level %d progress %.1f (min %d max %d)%s", f,
             int(api->read_u32(entry + kEntryLevel)), api->read_f32(entry + kEntryProgress),
             int(api->read_u32(entry + kEntryMin)), int(api->read_u32(entry + kEntryMax)),
             g_locked[f] ? " [locked]" : "");
    api->log(self, line);
  }
  const uint32_t table = api->read_u32(kHoodTableAddr);
  const uint32_t count = api->read_u32(kHoodCountAddr);
  snprintf(line, sizeof(line), "--- hoods (%u entries at %#x) ---", count, table);
  api->log(self, line);
  if (table > 0x1000) {
    for (uint32_t i = 0; i < count && i < 200; ++i) {
      const uint32_t record = table + i * kHoodStride;
      const int owner = int(api->read_u32(record + kHoodOwner));
      snprintf(line, sizeof(line), "  [%u] \"%s\" owner %d (%s)%s", i,
               ReadGuestString(record, 63).c_str(), owner, TeamName(owner).c_str(),
               int(i) == g_hood_index ? "  <- player is here" : "");
      api->log(self, line);
    }
  }
  api->log(self, "--- end dump ---");
  g_status = "tables dumped to wml.log";
}

WmlGuestFunction g_orig_update = nullptr;
int g_hood_refresh_countdown = 0;

// Hook on sub_82209E30, the recurring game-loop update (the same target the
// trainer and Saintify use). Everything that calls into the game runs here,
// because api->call needs a hook context.
void GameUpdateHook(WmlContext* ctx, uint8_t* base) {
  g_orig_update(ctx, base);
  const uint32_t player = api->read_u32(kPlayerPtr);
  if (player <= 0x1000 || api->read_u8(kMpFlag) != 0) {
    g_request_count = 0;
    g_hood_index = -1;
    return;
  }
  uint64_t saved_r[11];
  for (int i = 1; i <= 10; ++i) saved_r[i] = api->get_r(ctx, i);
  const double saved_f1 = api->get_f(ctx, 1);

  const bool turf_request = [] {
    for (int i = 0; i < g_request_count; ++i) {
      if (g_requests[i].kind == Request::kSetTurf) return true;
    }
    return false;
  }();
  if (turf_request || --g_hood_refresh_countdown <= 0) {
    g_hood_refresh_countdown = 15;
    RefreshHood(ctx, player);
  }

  for (int i = 0; i < g_request_count; ++i) {
    const Request& req = g_requests[i];
    switch (req.kind) {
      case Request::kApplyLevel:
        ApplyLevel(ctx, player, req.a, req.b);
        break;
      case Request::kClearAll:
        for (int f = 0; f < kFactionCount; ++f) {
          g_locked[f] = false;
          if (api->read_u32(EntryAddr(player, f) + kEntryLevel) != 0) {
            CallSetLevel(ctx, player, f, 0);
          }
          RestoreClamps(player, f);
        }
        break;
      case Request::kSetLock:
        if (req.b) {
          SaveClamps(player, req.a);
          g_locked[req.a] = true;
          g_locked_level[req.a] = req.b - 1;  // encoded as level + 1 so 0 stays "off"
          ApplyLevel(ctx, player, req.a, g_locked_level[req.a]);
        } else {
          g_locked[req.a] = false;
          RestoreClamps(player, req.a);
        }
        break;
      case Request::kSetTurf:
        SetTurfOwner(req.a);
        break;
      case Request::kSetAllegiance:
        SetPlayerAllegiance(player, req.a);
        break;
      case Request::kDumpTables:
        DumpTables(player);
        break;
    }
  }
  g_request_count = 0;
  EnforceLocks(ctx, player);

  for (int i = 1; i <= 10; ++i) api->set_r(ctx, i, saved_r[i]);
  api->set_f(ctx, 1, saved_f1);
}

void DrawMenu() {
  static bool drawn = false;
  if (!g_menu_open) {
    if (drawn) {
      api->overlay_text("");
      drawn = false;
    }
    return;
  }
  drawn = true;
  const FactionOption& opt = kFactions[g_faction_idx];
  const int level = WantedLevel(opt.faction);
  char wanted[96];
  if (level < 0) {
    snprintf(wanted, sizeof(wanted), "no gameplay");
  } else if (g_locked[opt.faction]) {
    snprintf(wanted, sizeof(wanted), "%d stars, LOCKED at %d", level, g_locked_level[opt.faction]);
  } else {
    snprintf(wanted, sizeof(wanted), "%d stars (%d%% to next)", level, WantedPercent(opt.faction));
  }

  const uint32_t player = api->read_u32(kPlayerPtr);
  const std::string current_team =
      player > 0x1000 ? TeamName(int(api->read_u32(player + kObjTeam))) : std::string("n/a");

  char text[768];
  snprintf(text, sizeof(text),
           "Wanted & Turf (F6 close)\n"
           "\n"
           "Faction: %s - %s\n"
           "1  Next faction\n"
           "2  Stars: %d   (2 = -1, 3 = +1)\n"
           "4  Apply %d stars to %s\n"
           "5  Lock stars: %s\n"
           "6  Clear all wanted levels\n"
           "\n"
           "Playa: %s\n"
           "0  Allegiance: %s\n"
           "P  Set playa allegiance\n"
           "\n"
           "Turf: %s - owner: %s\n"
           "7  New owner: %s\n"
           "8  Give this turf away\n"
           "9  Dump teams / notoriety / hoods to log%s%s",
           opt.label, wanted, g_stars, g_stars, opt.label,
           g_locked[opt.faction] ? "ON" : "OFF", current_team.c_str(),
           kAllegiances[g_allegiance_idx].label,
           g_hood_index < 0 ? "(none)" : g_hood_name.c_str(), TeamName(g_hood_owner).c_str(),
           kOwners[g_owner_idx].label, g_status.empty() ? "" : "\n\n",
           g_status.c_str());
  api->overlay_text(text);
}

void OnFrame(void*) {
  if (api->key_pressed(VK_F6)) {
    g_menu_open = !g_menu_open;
    g_status.clear();
    if (g_menu_open) {
      const uint32_t player = api->read_u32(kPlayerPtr);
      if (player > 0x1000) {
        g_allegiance_idx = AllegianceIndexForTeam(int(api->read_u32(player + kObjTeam)));
      }
    }
  }
  if (g_menu_open) {
    const FactionOption& opt = kFactions[g_faction_idx];
    if (api->key_pressed('1')) {
      g_faction_idx = (g_faction_idx + 1) % kFactionOptionCount;
      g_status.clear();
    } else if (api->key_pressed('2')) {
      if (g_stars > 0) --g_stars;
    } else if (api->key_pressed('3')) {
      if (g_stars < kMaxStars) ++g_stars;
    } else if (api->key_pressed('4')) {
      Queue(Request::kApplyLevel, opt.faction, g_stars);
      g_status = std::string("set ") + opt.label + " to " + std::to_string(g_stars) + " stars";
    } else if (api->key_pressed('5')) {
      if (g_locked[opt.faction]) {
        Queue(Request::kSetLock, opt.faction, 0);
        g_status = std::string("unlocked ") + opt.label;
      } else {
        Queue(Request::kSetLock, opt.faction, g_stars + 1);
        g_status = std::string("locked ") + opt.label + " at " + std::to_string(g_stars) +
                   (g_stars == 0 ? " stars (never wanted)" : " stars");
      }
    } else if (api->key_pressed('6')) {
      Queue(Request::kClearAll);
      g_status = "cleared every wanted level and released the locks";
    } else if (api->key_pressed('7')) {
      g_owner_idx = (g_owner_idx + 1) % kOwnerOptionCount;
      g_status.clear();
    } else if (api->key_pressed('8')) {
      Queue(Request::kSetTurf, kOwners[g_owner_idx].team);
    } else if (api->key_pressed('9')) {
      Queue(Request::kDumpTables);
    } else if (api->key_pressed('0')) {
      g_allegiance_idx = (g_allegiance_idx + 1) % kAllegianceOptionCount;
      g_status.clear();
    } else if (api->key_pressed('P')) {
      Queue(Request::kSetAllegiance, kAllegiances[g_allegiance_idx].team);
    }
  }
  DrawMenu();
}

}  // namespace

extern "C" WML_EXPORT int wml_mod_init(const WmlApi* loader_api, const WmlMod* mod) {
  api = loader_api;
  self = mod;
  if (api->version < WML_API_VERSION) return 1;
  if (api->size < sizeof(WmlApi) || !api->overlay_text) {
    api->log(self, "loader is too old: this mod needs the text overlay");
    return 1;
  }
  if (api->hook(0x82209E30, GameUpdateHook, &g_orig_update) != 0) {
    api->log(self, "WARNING: game-update hook failed; the menu cannot change anything");
  }
  api->on_frame(OnFrame, nullptr);
  api->log(self, "Wanted & Turf loaded. F6 opens the menu; keys 0-9 and P act while it is open.");
  return 0;
}
