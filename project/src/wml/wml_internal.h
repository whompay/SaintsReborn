// Whompay's Mod Loader - shared internals of the game-side loader.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "mod_list.h"

struct PPCContext;
struct lua_State;

namespace wml {

using HostFunction = void (*)(PPCContext& ctx, uint8_t* base);

// Guest memory base (address 0), set by Start.
extern uint8_t* g_guest_base;

// Host pointer of a guest address (the 0xE0000000+ range sits 4 KB further up).
inline uint8_t* GuestPointer(uint32_t address) {
  return g_guest_base + address + (address >= 0xE0000000u ? 0x1000u : 0u);
}

uint8_t ReadU8(uint32_t address);
uint16_t ReadU16(uint32_t address);
uint32_t ReadU32(uint32_t address);
float ReadF32(uint32_t address);
void WriteU8(uint32_t address, uint8_t value);
void WriteU16(uint32_t address, uint16_t value);
void WriteU32(uint32_t address, uint32_t value);
void WriteF32(uint32_t address, float value);

void Log(const std::string& mod, const std::string& message);

// The recompiled function for a guest address, or nullptr.
HostFunction FindFunction(uint32_t guest_address);

// Detours the guest function (chaining onto earlier hooks). Returns 0 on success.
int InstallHook(uint32_t guest_address, HostFunction hook, HostFunction* original);

bool KeyDown(int virtual_key);
bool KeyPressed(int virtual_key);
void TakeKey(int virtual_key, bool taken);
void ForceKey(int virtual_key, bool down);
void TurnCamera(double radians);
// Picture look (see wml.h set_look); count 0 = original.
void SetLook(const float* data, int count);
// The last look a mod set with set_look (14 values); false = none.
bool LastLook(float* out14);

// Register access by index (r0-r31, f0-f31).
uint64_t GetR(PPCContext& ctx, int index);
void SetR(PPCContext& ctx, int index, uint64_t value);
double GetF(PPCContext& ctx, int index);
void SetF(PPCContext& ctx, int index, double value);

// Runs the patch.lua of each mod, builds patched packfiles under
// cache_dir/files and returns that folder (empty if nothing was patched).
std::filesystem::path RunPatchScripts(const std::vector<ModInfo>& mods,
                                      const std::filesystem::path& game_dir,
                                      const std::filesystem::path& cache_dir);

// Files inside packfiles that patch scripts replaced (this start, or the
// cached run they reuse): which mod, which packfile (relative to the game
// folder) and which file in it.
struct PatchWrite {
  std::string mod_id;
  std::string pack;
  std::string name;
};
const std::vector<PatchWrite>& PatchWrites();

// Fair play (fair_play.cpp): checks in the background which enabled mods only
// change how the game looks (textures, fonts, the time-of-day lighting
// without less fog or brighter nights) or are approved code mods. Call after
// RunPatchScripts.
void StartFairCheck(const std::vector<ModInfo>& mods, const std::filesystem::path& game_dir,
                    const std::filesystem::path& cache_dir);

// Pushes the value of wml.setting(name, default) (arguments 1 and 2) and
// returns 1. Shared by patch scripts and script mods.
int PushSetting(lua_State* L, const std::vector<std::pair<std::string, std::string>>& settings);

// Lua script mods (lua_mods.cpp).
void StartLuaMod(const ModInfo& mod);
void LuaOnFrame();

}  // namespace wml
