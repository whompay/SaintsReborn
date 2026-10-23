// Saintify - hitting an NPC up close converts them to the Saints ("Playas").
//
// Detection: an NPC whose health drops while within melee range (~4.5 m) of
// the on-foot player is considered hit by the player. Positions are at
// object+20 (vec3f, from the FirstPerson mod's notes); health at +1912.
// Close-range gunfire also converts - the melee-weapon check needs the
// player's current-weapon field, not mapped yet.
//
// Conversion is a direct write of the team id at object+232 - the same store
// the game's own set_team script function performs (thunk 0x824DB708). The
// team id is read from the player (the player is a Playas).

#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <unordered_set>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "wml.h"

namespace {

const WmlApi* api;
const WmlMod* self;

constexpr uint32_t kPlayerPtr = 0x8309ABEC;
constexpr uint32_t kObjectTable = 0x830866C8;  // object = +12 + index*16
constexpr uint32_t kMpFlag = 0x8370E9F6;

constexpr int kObjHandle = 68;    // object+68: handle
constexpr int kObjType = 72;      // object+72: type (1 = human, 2/3 = props/corpses, 5 = vehicle)
constexpr int kObjTeam = 232;     // object+232: team id (set_team thunk store)
constexpr int kObjHealth = 1912;  // object+1912: health f32
constexpr int kObjPos = 20;       // object+20: position vec3f (FirstPerson notes)
constexpr int kObjCombatFlags = 3692;  // object+3692: bit 0x08 = combat disabled

constexpr float kMeleeRange = 4.5f;

std::unordered_map<uint32_t, float> g_health;  // object -> last seen health
int g_frame = 0;
int g_converted = 0;
uint32_t g_next_flee_mode = 4;  // calibrated: 4 = "never cower or flee"

// When enabled, NPCs converted by the player can convert others the same way
// (close-range hits). Only player-converted NPCs spread it, not every Saint.
std::unordered_set<uint32_t> g_converted_npcs;  // object addresses
bool g_spread_enabled = false;                  // menu option, default off

// Behavior node source for converts (menu option 6 cycles):
// 0 = copy from a nearby gang/police NPC, 1 = combatant node (default),
// 2/3 = the other known nodes (0x82C06D7C / 0x82C06E0C, for experiments).
int g_node_mode = 1;
constexpr uint32_t kKnownNodes[] = {0, 0x82C06D70, 0x82C06D7C, 0x82C06E0C};
const char* kNodeModeNames[] = {"nearby gang NPC", "combatant (0x82C06D70)",
                                "node 0x82C06D7C", "node 0x82C06E0C"};

// Heat on convert (menu option 8, default off): the conversion is reported to
// the game's own crime queue, so notoriety rises exactly as it does when the
// player kills someone - points, level ups, NPC alerts, chase music and the
// wanted meter on the HUD all come from the game's own code.
//
// The crime pipeline (from the recompiled source):
//   sub_821D3148(crime_class, team, counts_without_witness, damage_info)
//     queues a 16-byte crime event {+0 class, +4 team, +8 report delay,
//     +12 u8 counts-without-witness, +13 u8 point multiplier}.
//   sub_821D2AD0 drains the queue per frame and calls the reporter
//     sub_82481B28(player, &event), which maps team -> faction
//     (sub_821D2EE8), adds the points (sub_821D2840 -> sub_821D2178), alerts
//     the faction's NPCs (sub_821D1A20), updates the music and posts the HUD
//     notoriety event that moves the wanted meter.
//   The class/team pairs the game itself reports for a kill come from
//     sub_82482098's switch on the victim's team: civilian -> class 4 /
//     team -1, police -> class 20 / team -1, rival gang -> class 11 /
//     team = the victim's team. Team -1 means "no faction of its own", which
//     sub_821D2BE0 turns into the police.
//
// Notoriety state lives ON THE PLAYER OBJECT, *(0x8309ABEC): the crime drain,
// the reporter and notoriety_get (0x824D4290) all read that one global, which
// in PPC form is -21524(0x830A0000) - easy to mis-read as 0x82C9ABEC, and
// writing heat there only scribbles over unrelated memory (that was the bug
// behind "the numbers rise but no stars ever show").
// Faction i's entry is at player + 3868 + i*24: +0 = wanted level (int 0-5),
// +4/+8 = min/max level clamps (+8 is story-progression gated), +12 =
// progress points (f32; points-per-level table at 0x82B2BAB4), +16 = decay
// timestamp. Per-crime points come from the crime table at 0x82B2BAC8:
// [class*5 + faction] * 20 bytes {+0 points, +12 level cap, +16 u8
// witness-required}; sub_821D2840 refuses the points when the faction's level
// is already above that cap.
// Faction names: 5-entry pointer table at 0x827D6628 (0=los_carnales,
// 1=vice_kings, 2=rollers, 3=players, 4=police).
bool g_heat_enabled = false;
int g_police_faction = -1;       // resolved from the faction name table
bool g_factions_resolved = false;
// Crime class used for converts: -1 = the class the game itself uses for that
// victim; menu key 0 cycles through the kill classes for experiments.
constexpr int kCrimeClassChoices[] = {-1, 4, 11, 20};
int g_crime_class_idx = 0;
constexpr uint32_t kNotorietyOwnerPtr = kPlayerPtr;  // notoriety lives on the player object
constexpr uint32_t kQueueCrimeFn = 0x821D3148;      // sub_821D3148(class, team, flag, dmg_info)
constexpr uint32_t kHeatPointsTable = 0x82B2BAB4;   // 5 ints: points per level
constexpr uint32_t kCrimeTable = 0x82B2BAC8;        // [class*5+faction]*20
constexpr uint32_t kCrimeNameTable = 0x8286E5C8;    // [class] -> cstr
constexpr int kCrimeClassDumpCount = 24;            // classes listed by the notoriety dump
constexpr uint32_t kFactionNameTable = 0x827D6628;
constexpr uint32_t kTeamNameTable = 0x820387D8;     // indexed by team id
// Master visibility flag for the notoriety HUD render: sub_822E1508 skips the
// wanted-meter draw (sub_822ECDB0) while this byte is 0. The game manages it
// itself; the dump prints it so a missing meter can be told apart from a
// missing crime.
constexpr uint32_t kNotorietyHudVisibleFlag = 0x82FFB592;

// Teams of victims converted since the last game update, reported as crimes
// by the update hook (api->call needs a hook context, which the damage hook
// path has but the frame-loop fallback does not - the queue covers both).
uint32_t g_pending_heat[32];
int g_pending_heat_count = 0;

std::string ReadImageString(uint32_t addr) {
  if (addr < 0x82000000 || addr >= 0x84160000) return {};
  const char* p = reinterpret_cast<const char*>(
      static_cast<uint8_t*>(api->guest_pointer(addr)));
  std::string out;
  for (int i = 0; i < 64; ++i) {
    const char c = p[i];
    if (!c) break;
    if (c < 0x20 || c > 0x7E) return {};
    out += c;
  }
  return out;
}

std::string ToLower(std::string s) {
  for (char& c : s) c = char(std::tolower(uint8_t(c)));
  return s;
}

void ResolveFactions() {
  if (g_factions_resolved) return;
  g_factions_resolved = true;
  for (int i = 0; i < 5; ++i) {
    const std::string name = ToLower(ReadImageString(api->read_u32(kFactionNameTable + i * 4)));
    if (name.find("police") != std::string::npos) g_police_faction = i;
    api->log(self, ("  faction " + std::to_string(i) + " = \"" + name + "\"").c_str());
  }
}

// Team -> faction mapping, straight from sub_821D2EE8's jump table: team 0
// (Playas) -> 3, 1 -> 0, 2 -> 1, 3 -> 2, team 5 (Police) -> 4, and everything
// else (team 4 = Neutral Gang, civilians, unmapped teams) -> -1, no faction.
// (The teams.xtbl order in older notes is NOT the runtime team id order.)
int TeamToFaction(uint32_t team) {
  switch (team) {
    case 0: return 3;
    case 1: return 0;
    case 2: return 1;
    case 3: return 2;
    case 5: return 4;
    default: return -1;
  }
}

void AddConvertHeat(uint32_t victim_team) {
  if (g_pending_heat_count < 32) {
    g_pending_heat[g_pending_heat_count++] = victim_team;
  }
}

std::string CrimeClassName(int cls) {
  return ReadImageString(api->read_u32(kCrimeNameTable + cls * 4));
}

// The crime the game reports when the player kills this victim, from
// sub_82482098's switch on the victim's team. Rival gang members are reported
// against their own team; civilians and cops have no faction of their own, so
// they go in as team -1 and sub_821D2BE0 hands them to the police. Team 0
// (fellow Saints) and team 4 (the neutral gang) are no crime at all. Teams the
// game does not map are treated as civilians. Returns false when the victim's
// team carries no crime.
bool CrimeForVictimTeam(uint32_t team, int* crime_class, int32_t* report_team) {
  if (team == 0 || team == 4) return false;
  if (team >= 1 && team <= 3) {
    *crime_class = 11;
    *report_team = int32_t(team);
  } else if (team == 5) {
    *crime_class = 20;
    *report_team = -1;
  } else {
    *crime_class = 4;
    *report_team = -1;
  }
  const int forced = kCrimeClassChoices[g_crime_class_idx];
  if (forced >= 0) *crime_class = forced;
  return true;
}

// Formats current faction heat wanted-meter style, e.g.
// "  POL [***--] 25%  ROL [*----] 28%". Returns the number of chars written
// (0 = no heat or not in gameplay).
int FormatHeatLine(char* out, size_t left) {
  const uint32_t wrapper = api->read_u32(kNotorietyOwnerPtr);
  if (wrapper <= 0x1000) return 0;
  static const char* kShort[5] = {"LC", "VK", "ROL", "PLY", "POL"};
  int written = 0;
  for (int f = 0; f < 5; ++f) {
    if (f == 3) continue;  // the player faction never has heat
    const uint32_t entry = wrapper + 3868 + f * 24;
    const uint32_t level = api->read_u32(entry + 0);
    const float progress = api->read_f32(entry + 12);
    if (level == 0 && progress <= 0.0f) continue;
    if (level >= 5) {
      written += snprintf(out + written, left - written, "  %s [*****] MAX", kShort[f]);
    } else {
      char stars[6];
      for (uint32_t s = 0; s < 5; ++s) stars[s] = s < level ? '*' : '-';
      stars[5] = '\0';
      const uint32_t next = api->read_u32(kHeatPointsTable + level * 4);
      written += snprintf(out + written, left - written, "  %s [%s] %.0f%%", kShort[f], stars,
                          next ? progress * 100.0f / float(next) : 0.0f);
    }
  }
  return written;
}

// F3 toggles the mod's menu (like Whompay's trainer); number keys act.
bool g_enabled = true;
bool g_menu_open = false;
std::string g_action_note;

void RefreshMenuText() {
  if (!g_menu_open) return;
  if (api->size < sizeof(WmlApi) || !api->overlay_text) return;
  char text[512];
  snprintf(text, sizeof(text),
           "Saintify (F3 close)\n"
           "1) Enabled: %s\n"
           "2) Dump nearby objects (log)\n"
           "3) AI field scan (log)\n"
           "4) Snapshot nearest NPC (file)\n"
           "5) Behavior descriptors (file)\n"
           "6) Converts can convert others: %s\n"
           "7) Behavior node: %s\n"
           "8) Heat on convert: %s\n"
           "9) Dump notoriety (log)\n"
           "0) Heat crime class: %s\n"
           "converted: %d%s%s",
           g_enabled ? "ON" : "OFF", g_spread_enabled ? "ON" : "OFF",
           kNodeModeNames[g_node_mode], g_heat_enabled ? "ON" : "OFF",
           kCrimeClassChoices[g_crime_class_idx] < 0
               ? "same as a kill"
               : std::to_string(kCrimeClassChoices[g_crime_class_idx]).c_str(),
           g_converted,
           g_action_note.empty() ? "" : "\n", g_action_note.c_str());
  char segs[128];
  if (FormatHeatLine(segs, sizeof(segs)) > 0) {
    const size_t used = strlen(text);
    snprintf(text + used, sizeof(text) - used, "\nheat:%s", segs);
  }
  api->overlay_text(text);
}

void ToggleMenu() {
  g_menu_open = !g_menu_open;
  g_action_note.clear();
  if (g_menu_open) {
    RefreshMenuText();
  } else if (api->size >= sizeof(WmlApi) && api->overlay_text) {
    api->overlay_text("");
  }
}

// Notoriety dump: the player object's entries at player + 3868 + i*24 (+0 =
// wanted level, +8 = the story-gated level cap, +12 = progress points toward
// the next level), plus the crime table at 0x82B2BAC8 so a crime class can be
// looked up: [class*5 + faction]*20 bytes {+0 points, +12 level cap, +16 u8
// witness-required}.
void DumpNotoriety() {
  char line[256];
  api->log(self, "--- notoriety dump ---");
  ResolveFactions();
  const uint32_t owner = api->read_u32(kNotorietyOwnerPtr);
  if (owner <= 0x1000) {
    snprintf(line, sizeof(line), "  notoriety owner (player) = %#x (not in gameplay)", owner);
    api->log(self, line);
  } else {
    snprintf(line, sizeof(line),
             "  notoriety owner (player) = %#x  health(+1912)=%.1f flags(+2996)=%#x "
             "hud handle(+4036)=%#x",
             owner, api->read_f32(owner + 1912), api->read_u32(owner + 2996),
             api->read_u32(owner + 4036));
    api->log(self, line);
    for (int i = 0; i < 5; ++i) {
      const uint32_t entry = owner + 3868 + i * 24;
      snprintf(line, sizeof(line), "    faction[%d] level %d progress %.1f (min %d max %d)", i,
               int(api->read_u32(entry + 0)), api->read_f32(entry + 12),
               int(api->read_u32(entry + 4)), int(api->read_u32(entry + 8)));
      api->log(self, line);
    }
  }
  api->log(self, "--- crime classes (id: name) ---");
  for (int cls = 0; cls < kCrimeClassDumpCount; ++cls) {
    const std::string name = CrimeClassName(cls);
    if (name.empty()) continue;
    snprintf(line, sizeof(line), "  class %d = \"%s\"", cls, name.c_str());
    api->log(self, line);
  }
  api->log(self, "--- teams (runtime id order) ---");
  for (int team = 0; team < 8; ++team) {
    const std::string name = ReadImageString(api->read_u32(kTeamNameTable + team * 4));
    snprintf(line, sizeof(line), "  team %d = \"%s\" -> faction %d", team, name.c_str(),
             TeamToFaction(uint32_t(team)));
    api->log(self, line);
  }
  api->log(self, "--- crime table (class -> faction: points / cap / witness-flag) ---");
  for (int cls = 0; cls < 10; ++cls) {
    for (int f = 0; f < 5; ++f) {
      const uint32_t rec = kCrimeTable + (cls * 5 + f) * 20;
      snprintf(line, sizeof(line), "  class %d faction %d: points %d cap %d flags 0x%02X", cls,
               f, int(api->read_u32(rec + 0)), int(api->read_u32(rec + 12)),
               api->read_u8(rec + 16));
      api->log(self, line);
    }
  }
  api->log(self, "  points per level:");
  for (int i = 0; i < 5; ++i) {
    snprintf(line, sizeof(line), "    level %d -> %d points", i,
             int(api->read_u32(kHeatPointsTable + i * 4)));
    api->log(self, line);
  }
  snprintf(line, sizeof(line), "  notoriety HUD visibility flag (0x82FFB592) = %u",
           api->read_u8(kNotorietyHudVisibleFlag));
  api->log(self, line);
  api->log(self, "--- end notoriety dump ---");
  g_action_note = "notoriety dumped to wml.log";
  RefreshMenuText();
}

// Hook on sub_82209E30 (the recurring game-loop update, same target Whompay's
// trainer uses). Drains the pending-heat queue: each converted victim's
// original team becomes a queued crime (sub_821D3148), exactly the call the
// game makes when the player kills that kind of NPC. The game's own drain
// (sub_821D2AD0) picks the event up on a later frame and runs the full
// reporter, so the points, the NPC alerts, the music and the HUD wanted meter
// all come from the game itself. The call has to happen in a hook context
// because api->call needs one.
WmlGuestFunction g_orig_update_fn = nullptr;

void GameUpdateHook(WmlContext* ctx, uint8_t* base) {
  g_orig_update_fn(ctx, base);
  if (g_pending_heat_count == 0) return;
  if (api->read_u8(kMpFlag) != 0) {  // the reporter ignores crimes in multiplayer
    g_pending_heat_count = 0;
    return;
  }
  if (api->read_u32(kNotorietyOwnerPtr) <= 0x1000) {  // not in gameplay
    g_pending_heat_count = 0;
    return;
  }
  uint64_t saved_regs[7];
  for (int i = 1; i <= 6; ++i) saved_regs[i] = api->get_r(ctx, i);
  while (g_pending_heat_count > 0) {
    const uint32_t team = g_pending_heat[--g_pending_heat_count];
    int cls = 0;
    int32_t report_team = -1;
    if (!CrimeForVictimTeam(team, &cls, &report_team)) continue;
    api->set_r(ctx, 3, uint64_t(uint32_t(cls)));
    api->set_r(ctx, 4, uint64_t(int64_t(report_team)));
    api->set_r(ctx, 5, 1);  // counts even when nobody witnessed it
    api->set_r(ctx, 6, 0);  // no damage-info struct (the game passes one for kills)
    if (api->call(ctx, kQueueCrimeFn) != 0) {
      api->log(self, "  heat: crime queue call failed");
      break;
    }
    char line[224];
    snprintf(line, sizeof(line), "  heat: queued crime class %d (\"%s\") vs team %d (victim team %u)",
             cls, CrimeClassName(cls).c_str(), report_team, team);
    api->log(self, line);
  }
  for (int i = 1; i <= 6; ++i) api->set_r(ctx, i, saved_regs[i]);
}

// Hook on sub_824470D0: the character damage function (r3 = victim object,
// r4 = attacker object; identified by hook-testing every function that
// subtracts from health at +1912). When the attacker is the player object,
// the victim is marked briefly and converted when its health actually drops.
WmlGuestFunction g_orig_damage_fn = nullptr;
std::unordered_map<uint32_t, ULONGLONG> g_hit_by_player;  // victim -> tick
bool g_attribution_proven = false;  // set once the hook sees attacker == player


void ConvertNpc(WmlContext* ctx, uint8_t* base, uint32_t obj, uint32_t team, uint32_t player);
void ConvertLoop(uint32_t player, uint32_t saints_team, bool on_foot);
bool PlayerOnFoot(uint32_t player);
float DistanceToPlayer(uint32_t obj, uint32_t player);
float DistanceBetween(uint32_t a, uint32_t b);
uint32_t FindGangBehaviorNode(uint32_t player);

void DamageHook(WmlContext* ctx, uint8_t* base) {
  const uint32_t victim = static_cast<uint32_t>(api->get_r(ctx, 3));
  const uint32_t attacker = static_cast<uint32_t>(api->get_r(ctx, 4));
  const uint32_t player = api->read_u32(kPlayerPtr);
  g_orig_damage_fn(ctx, base);
  if (!attacker || !victim || victim == player) return;
  if (!g_enabled) return;
  if (api->read_u8(kMpFlag) != 0) return;
  if (api->read_u32(victim + kObjType) != 1) return;       // humans only
  if (api->read_f32(victim + kObjHealth) <= 0.0f) return;  // dead
  if (!player) return;
  const uint32_t saints_team = api->read_u32(player + kObjTeam);
  if (api->read_u32(victim + kObjTeam) == saints_team) return;  // already a Saint

  if (attacker == player) {
    if (!PlayerOnFoot(player)) return;
    if (DistanceToPlayer(victim, player) > kMeleeRange) return;
    if (!g_attribution_proven) {
      g_attribution_proven = true;
      api->log(self, "player attribution confirmed (attacker == player); enforcing it");
    }
    ConvertNpc(ctx, base, victim, saints_team, player);
    return;
  }

  // Spread: converted NPCs convert others with close-range hits (menu option,
  // default off). Only player-converted NPCs spread it, not every Saint.
  if (!g_spread_enabled) return;
  if (!g_converted_npcs.count(attacker)) return;
  if (api->read_u32(attacker + kObjTeam) != saints_team) return;  // sanity
  if (api->read_u32(attacker + kObjType) != 1) return;            // still a human
  if (DistanceBetween(victim, attacker) > kMeleeRange) return;
  ConvertNpc(ctx, base, victim, saints_team, player);
}

void ToggleEnabled() {
  g_enabled = !g_enabled;
  RefreshMenuText();
}

bool PlayerOnFoot(uint32_t player) {
  if (api->read_u32(player + 3456) != 0) return false;
  const uint32_t vehicle_handle = api->read_u32(player + 2496);
  if (vehicle_handle && (api->read_u8(player + 2569) & 0x10)) return false;
  const uint32_t state = api->read_u32(player + 508);
  if (state == 9 || state == 12) return false;
  if (!vehicle_handle) return true;
  const uint32_t index = vehicle_handle & 0xFFFF;
  if (index >= 4096) return true;
  const uint32_t object = api->read_u32(kObjectTable + 12 + index * 16);
  if (!object) return true;
  return !(api->read_u32(object + kObjHandle) == vehicle_handle &&
           api->read_u32(object + kObjType) == 5);
}

float DistanceToPlayer(uint32_t obj, uint32_t player) {
  const float dx = api->read_f32(obj + kObjPos) - api->read_f32(player + kObjPos);
  const float dy = api->read_f32(obj + kObjPos + 4) - api->read_f32(player + kObjPos + 4);
  const float dz = api->read_f32(obj + kObjPos + 8) - api->read_f32(player + kObjPos + 8);
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

float DistanceBetween(uint32_t a, uint32_t b) {
  const float dx = api->read_f32(a + kObjPos) - api->read_f32(b + kObjPos);
  const float dy = api->read_f32(a + kObjPos + 4) - api->read_f32(b + kObjPos + 4);
  const float dz = api->read_f32(a + kObjPos + 8) - api->read_f32(b + kObjPos + 8);
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// Finds the behavior node of a nearby gang/police NPC (team 1-5: Vice Lords,
// Los Carnales, Rollerz, Kings, Police) to copy onto a converted NPC, so
// converts get a male gang combat style rather than a fixed (possibly female)
// node. Returns 0 if none is nearby.
uint32_t FindGangBehaviorNode(uint32_t player) {
  for (uint32_t index = 0; index < 4096; ++index) {
    const uint32_t obj = api->read_u32(kObjectTable + 12 + index * 16);
    if (!obj || obj == player) continue;
    if (api->read_u32(obj + kObjType) != 1) continue;
    const uint32_t team = api->read_u32(obj + kObjTeam);
    if (team == 0 || team >= 6) continue;  // skip Saints and civilians
    if (DistanceToPlayer(obj, player) > 60.0f) continue;
    const uint32_t ai = api->read_u32(obj + 568);
    if (!ai) continue;
    const uint32_t node = api->read_u32(ai + 3748);
    if (node) return node;
  }
  return 0;
}

void DumpNearbyObjects() {
  const uint32_t player = api->read_u32(kPlayerPtr);
  if (!player) return;
  // The 5 cower/flee mode names from the image (0x821F9588 table).
  api->log(self, "--- cower/flee mode names ---");
  for (int i = 0; i < 5; ++i) {
    const uint32_t str_ptr = api->read_u32(0x821F9588 + i * 4);
    const char* s = str_ptr ? reinterpret_cast<const char*>(
                         static_cast<uint8_t*>(api->guest_pointer(str_ptr)))
                            : nullptr;
    char line[128];
    snprintf(line, sizeof(line), "  mode %d = \"%s\"", i, s ? s : "(unreadable)");
    api->log(self, line);
  }
  api->log(self, "--- nearby object dump ---");
  for (uint32_t index = 0; index < 4096; ++index) {
    const uint32_t obj = api->read_u32(kObjectTable + 12 + index * 16);
    if (!obj || obj == player) continue;
    const float dist = DistanceToPlayer(obj, player);
    if (dist > 12.0f) continue;
    char line[320];
    const uint32_t ai = api->read_u32(obj + 568);  // AI persona sub-object?
    snprintf(line, sizeof(line),
             "obj 0x%08X idx %u type %u handle %08X team@232 %d health %.1f "
             "ai@568 %08X flee@ai+3704 %u dist %.1f",
             obj, index, api->read_u32(obj + kObjType), api->read_u32(obj + kObjHandle),
             int32_t(api->read_u32(obj + kObjTeam)), api->read_f32(obj + kObjHealth), ai,
             ai ? api->read_u32(ai + 3704) : 0xFFFFFFFFu, dist);
    api->log(self, line);
  }
  api->log(self, "--- end dump ---");
}

// F9: snapshot the nearest NPC's entity and AI persona memory to a file, so
// two snapshots (e.g. before and after recruit+dismiss) can be diffed offline
// to find exactly which fields the game changes.
int g_snapshot_num = 0;

void SnapshotNearestNpc() {
  const uint32_t player = api->read_u32(kPlayerPtr);
  if (!player) return;
  uint32_t best = 0;
  float best_dist = 1e9f;
  for (uint32_t index = 0; index < 4096; ++index) {
    const uint32_t obj = api->read_u32(kObjectTable + 12 + index * 16);
    if (!obj || obj == player) continue;
    if (api->read_u32(obj + kObjType) != 1) continue;
    const float d = DistanceToPlayer(obj, player);
    if (d < best_dist) {
      best_dist = d;
      best = obj;
    }
  }
  if (!best) {
    api->log(self, "snapshot: no NPC nearby");
    return;
  }
  char path[520];
  snprintf(path, sizeof(path), "%s\\..\\npc_snap_%d.txt", self->folder, g_snapshot_num++);
  FILE* f = fopen(path, "w");
  if (!f) return;
  const uint32_t ai = api->read_u32(best + 568);
  fprintf(f, "obj 0x%08X ai 0x%08X\n", best, ai);
  for (int off = 0; off <= 4200; off += 4) {
    const uint32_t v = api->read_u32(best + off);
    if (v) fprintf(f, "obj +%-5d 0x%08X\n", off, v);
  }
  if (ai) {
    for (int off = 0; off <= 4200; off += 4) {
      const uint32_t v = api->read_u32(ai + off);
      if (v) fprintf(f, "ai  +%-5d 0x%08X\n", off, v);
    }
  }
  fclose(f);
  char line[160];
  snprintf(line, sizeof(line), "snapshot %d written for obj 0x%08X (dist %.1f)",
           g_snapshot_num - 1, best, best_dist);
  api->log(self, line);
}

// F12: dump the AI behavior descriptor table region (around the entries seen
// in the recruit/dismiss diff) plus the current descriptors of the nearest
// NPCs, to a file for offline analysis.
void DumpBehaviorDescriptors() {
  const uint32_t player = api->read_u32(kPlayerPtr);
  if (!player) return;
  char path[520];
  snprintf(path, sizeof(path), "%s\\..\\ai_desc_dump.txt", self->folder);
  FILE* f = fopen(path, "w");
  if (!f) return;

  // Region covering the descriptor entries seen so far.
  fprintf(f, "== table region 0x829E3800..0x829E4600 ==\n");
  for (uint32_t a = 0x829E3800; a < 0x829E4600; a += 4) {
    const uint32_t v = api->read_u32(a);
    if (v) fprintf(f, "0x%08X: 0x%08X\n", a, v);
  }
  // Nearest NPCs: their descriptor pointers and descriptor contents.
  fprintf(f, "== nearby NPC descriptors ==\n");
  for (uint32_t index = 0; index < 4096; ++index) {
    const uint32_t obj = api->read_u32(kObjectTable + 12 + index * 16);
    if (!obj || obj == player) continue;
    if (api->read_u32(obj + kObjType) != 1) continue;
    if (DistanceToPlayer(obj, player) > 15.0f) continue;
    const uint32_t ai = api->read_u32(obj + 568);
    if (!ai) continue;
    const uint32_t d1 = api->read_u32(ai + 3616);
    const uint32_t d2 = api->read_u32(ai + 3748);
    fprintf(f, "npc 0x%08X team %d: ai+3616 = 0x%08X, ai+3748 = 0x%08X\n", obj,
            int32_t(api->read_u32(obj + kObjTeam)), d1, d2);
    for (uint32_t d : {d1, d2}) {
      if (!d) continue;
      fprintf(f, "  descriptor 0x%08X:", d);
      for (int off = 0; off <= 60; off += 4) {
        fprintf(f, " %+d=0x%08X", off, api->read_u32(d + off));
      }
      fprintf(f, "\n");
    }
  }
  fclose(f);
  api->log(self, "AI descriptor dump written to mods\\ai_desc_dump.txt");
}

// F8: for each nearby human NPC, dump pointer-looking fields in the AI region
// of the object (+3000..+4200). Fields that are equal within a behavior group
// (civilians vs gang members) but differ between groups are personality
// pointer candidates.
void DumpAiFields() {
  const uint32_t player = api->read_u32(kPlayerPtr);
  if (!player) return;
  api->log(self, "--- AI field scan (nearby humans) ---");
  for (uint32_t index = 0; index < 4096; ++index) {
    const uint32_t obj = api->read_u32(kObjectTable + 12 + index * 16);
    if (!obj || obj == player) continue;
    if (api->read_u32(obj + kObjType) != 1) continue;
    if (DistanceToPlayer(obj, player) > 12.0f) continue;
    char line[320];
    snprintf(line, sizeof(line), "obj 0x%08X team %d health %.1f:", obj,
             int32_t(api->read_u32(obj + kObjTeam)), api->read_f32(obj + kObjHealth));
    api->log(self, line);
    for (int off = 3000; off <= 4200; off += 4) {
      const uint32_t v = api->read_u32(obj + off);
      if (v < 0x82000000 || v >= 0x84160000) continue;  // pointers into image/data only
      snprintf(line, sizeof(line), "    +%d: 0x%08X", off, v);
      api->log(self, line);
    }
    // The archetype/character definition at +3552: dump every nonzero word so
    // the personality field can be spotted by comparing archetypes.
    const uint32_t arch = api->read_u32(obj + 3552);
    if (arch >= 0x82000000 && arch < 0x84160000) {
      snprintf(line, sizeof(line), "    archetype 0x%08X:", arch);
      api->log(self, line);
      for (int off = 0; off <= 1020; off += 4) {
        const uint32_t v = api->read_u32(arch + off);
        if (v != 0) {
          snprintf(line, sizeof(line), "      +%d: 0x%08X", off, v);
          api->log(self, line);
        }
      }
    }
  }
  api->log(self, "--- end AI field scan ---");
}

void OnFrame(void*) {
  // F3 opens/closes the menu; number keys run the actions while it's open.
  if (api->key_pressed(VK_F3)) {
    ToggleMenu();
  }
  if (g_menu_open) {
    if (api->key_pressed('1')) {
      ToggleEnabled();
    } else if (api->key_pressed('2')) {
      DumpNearbyObjects();
      g_action_note = "objects dumped to wml.log";
    } else if (api->key_pressed('3')) {
      DumpAiFields();
      g_action_note = "AI fields dumped to wml.log";
    } else if (api->key_pressed('4')) {
      SnapshotNearestNpc();
      g_action_note = "snapshot written (npc_snap_N.txt)";
    } else if (api->key_pressed('5')) {
      DumpBehaviorDescriptors();
      g_action_note = "descriptors written (ai_desc_dump.txt)";
    } else if (api->key_pressed('6')) {
      g_spread_enabled = !g_spread_enabled;
    } else if (api->key_pressed('7')) {
      g_node_mode = (g_node_mode + 1) % 4;
    } else if (api->key_pressed('8')) {
      g_heat_enabled = !g_heat_enabled;
    } else if (api->key_pressed('9')) {
      DumpNotoriety();
    } else if (api->key_pressed('0')) {
      g_crime_class_idx =
          (g_crime_class_idx + 1) % int(sizeof(kCrimeClassChoices) / sizeof(kCrimeClassChoices[0]));
    }
    RefreshMenuText();
  }
  if (!g_enabled) return;
  if (++g_frame % 3 != 0) return;
  if (api->read_u8(kMpFlag) != 0) return;  // no converting in multiplayer
  const uint32_t player = api->read_u32(kPlayerPtr);
  if (!player) return;
  const uint32_t saints_team = api->read_u32(player + kObjTeam);
  const bool on_foot = PlayerOnFoot(player);
  ConvertLoop(player, saints_team, on_foot);
}
// Applies the full conversion to an NPC object. When called from the damage
// hook (ctx != nullptr) the NPC's AI state is also reset to idle via
// sub_8257C400(obj, 19) - the core of npc_go_idle - so an in-flight flee
// action is cancelled and the brain re-evaluates with the new settings.
void ConvertNpc(WmlContext* ctx, uint8_t* base, uint32_t obj, uint32_t team, uint32_t player) {
  const uint32_t old_team = api->read_u32(obj + kObjTeam);
  api->write_u32(obj + kObjTeam, team);
  if (g_heat_enabled) {
    AddConvertHeat(old_team);
  }
  const uint32_t player_handle = api->read_u32(player + kObjHandle);
  // combat_enable: clear the "combat disabled" bit (combat_disable sets
  // 0x08 at obj+3692, combat_enable clears it).
  api->write_u8(obj + kObjCombatFlags, api->read_u8(obj + kObjCombatFlags) & ~0x08u);
  // Cower/flee override: the AI persona (entity+568) keeps the mode at
  // +3704; 0 = personality default. Mode 4 = "never cower or flee"
  // (calibrated in game).
  const uint32_t ai = api->read_u32(obj + 568);
  if (ai) {
    api->write_u32(ai + 3704, g_next_flee_mode);
    // Behavior node swap: ai+3748 points into the AI behavior list (nodes
    // with function pointers at 0x82BE16xx). Source is menu-selectable:
    // copy from a nearby gang/police NPC, or a fixed node (default: the
    // combatant node from the recruit/dismiss diff).
    const uint32_t behavior = api->read_u32(ai + 3748);
    uint32_t gang_node = 0;
    if (g_node_mode == 0) {
      gang_node = FindGangBehaviorNode(player);
    }
    const uint32_t target =
        gang_node ? gang_node : kKnownNodes[g_node_mode == 0 ? 1 : g_node_mode];
    if (behavior != target) {
      api->write_u32(ai + 3748, target);
      char note[128];
      snprintf(note, sizeof(note), "  behavior node 0x%08X -> 0x%08X (%s)", behavior, target,
               kNodeModeNames[g_node_mode]);
      api->log(self, note);
    }
    // Party-leader handle on the persona: is_in_party (0x824D05E0) compares
    // this against the player's handle; party members don't panic at
    // gunfire. Written by the recruit cycle, kept after dismiss.
    api->write_u32(ai + 4128, player_handle);
  } else {
    char note[128];
    snprintf(note, sizeof(note), "  note: obj 0x%08X has no AI persona (+568 null)", obj);
    api->log(self, note);
  }
  // Recruit/dismiss leaves an ex-civilian combat-ready; the snapshot diff
  // showed these entity fields change during that cycle. Replicate them.
  const uint32_t flags = api->read_u32(obj + 216);
  api->write_u32(obj + 216, flags & ~0x04000000u);
  api->write_u32(obj + 512, 1);
  api->write_u32(obj + 3568, 2);
  api->write_u32(obj + 3972, 1);
  // Leader links the recruit cycle wrote (is_in_party reads a leader handle
  // via the inner object; gunshot panic is suppressed for the leader's
  // party). Dismissed NPCs keep these without following.
  api->write_u32(obj + 1128, player_handle);
  api->write_u32(obj + 1176, player_handle);
  api->write_u32(obj + 3976, player_handle);
  // Remaining recruit/dismiss changes from the snapshot diff: cleared
  // target/goal handles and one mode field.
  api->write_u32(obj + 292, 0xFFFFFFFFu);
  api->write_u32(obj + 772, 0xFFFFFFFFu);
  api->write_u32(obj + 776, 0xFFFFFFFFu);
  api->write_u32(obj + 2404, 0xFFFFFFFFu);
  api->write_u32(obj + 4200, 0x10);
  ++g_converted;
  g_converted_npcs.insert(obj);
  char line[224];
  snprintf(line, sizeof(line), "SAINTIFIED object 0x%08X (team %u -> %u, total %d)%s", obj,
           old_team, team, g_converted, ctx ? " +ai reset" : "");
  api->log(self, line);
  if (ctx) {
    // AI state switch to 19 (the core of npc_go_idle, sub_8257C400), with the
    // game's own guards: npc_go_idle only switches when the NPC is not in a
    // vehicle state (508 != 9) and its sub-state (+2556) is 1 or 3. Forcing it
    // outside those states gave converts a wrong locomotion set ("girl run").
    const uint32_t state = api->read_u32(obj + 508);
    const uint32_t sub_state = api->read_u32(obj + 2556);
    if (state != 9 && (sub_state == 1 || sub_state == 3)) {
      const uint64_t saved_r3 = api->get_r(ctx, 3);
      const uint64_t saved_r4 = api->get_r(ctx, 4);
      api->set_r(ctx, 3, obj);
      api->set_r(ctx, 4, 19);
      api->call(ctx, 0x8257C400);
      api->set_r(ctx, 3, saved_r3);
      api->set_r(ctx, 4, saved_r4);
    }
  }
}

void ConvertLoop(uint32_t player, uint32_t saints_team, bool on_foot) {
  for (uint32_t index = 0; index < 4096; ++index) {
    const uint32_t obj = api->read_u32(kObjectTable + 12 + index * 16);
    if (!obj || obj == player) continue;
    // Only humans (type 1; that includes the player, excluded above).
    if (api->read_u32(obj + kObjType) != 1) continue;
    const float health = api->read_f32(obj + kObjHealth);
    if (health <= 0.0f) continue;  // dead

    auto [it, inserted] = g_health.emplace(obj, health);
    const float before = it->second;
    it->second = health;
    if (inserted || !on_foot || health >= before - 0.5f) continue;
    if (DistanceToPlayer(obj, player) > kMeleeRange) continue;
    // Attribution: once the damage hook has proven that its attacker arg is
    // the player object, only convert NPCs the player actually damaged
    // (within the last 1.5 s). Until then, fall back to proximity-only.
    if (g_attribution_proven) {
      auto hit = g_hit_by_player.find(obj);
      if (hit == g_hit_by_player.end() ||
          GetTickCount64() - hit->second > 1500) {
        continue;
      }
      g_hit_by_player.erase(hit);
    }

    const uint32_t team = api->read_u32(obj + kObjTeam);
    if (team == saints_team) continue;  // already a Saint
    ConvertNpc(nullptr, nullptr, obj, saints_team, player);
  }
}

}  // namespace

extern "C" WML_EXPORT int wml_mod_init(const WmlApi* loader_api, const WmlMod* mod) {
  api = loader_api;
  self = mod;
  if (api->version < WML_API_VERSION) return 1;
  api->on_frame(OnFrame, nullptr);
  if (api->hook(0x824470D0, DamageHook, &g_orig_damage_fn) != 0) {
    api->log(self, "WARNING: damage hook failed; player attribution disabled");
  }
  if (api->hook(0x82209E30, GameUpdateHook, &g_orig_update_fn) != 0) {
    api->log(self, "WARNING: update hook failed; heat on convert disabled");
  }
  api->log(self, "Saintify v8 armed: hit an NPC up close while on foot to convert them. F3 toggles.");
  return 0;
}
