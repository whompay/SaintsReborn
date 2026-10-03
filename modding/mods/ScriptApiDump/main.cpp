// Script API Dump - developer tool mod for Whompay's Mod Loader.
//
// Maps the game's script API: Lua-visible function name -> thunk address.
// The (image string address, thunk) pairs were extracted offline from the
// registration table builder sub_824E1AD0 in the recompiled game code (see
// scriptapi_pairs.h, generated); at runtime the mod just reads the strings
// and writes the resolved mapping to mods\scriptapi_map.txt.
//
// Runs once ~15 s after the player object exists, and again on F6.

#include <cstdint>
#include <cstdio>
#include <cstring>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "wml.h"
#include "scriptapi_pairs.h"

namespace {

const WmlApi* api;
const WmlMod* self;
uint8_t* g_base;  // host pointer for guest address 0

bool IsReadable(const void* p) {
  MEMORY_BASIC_INFORMATION mbi;
  return VirtualQuery(p, &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT;
}

const char* ReadImageString(uint32_t addr) {
  if (addr < 0x82000000 || addr >= 0x84160000) return nullptr;
  const char* s = reinterpret_cast<const char*>(g_base + addr);
  if (!IsReadable(s)) return nullptr;
  for (int i = 0; i < 96; ++i) {
    if (!IsReadable(s + i)) return nullptr;
    if (s[i] == 0) return i >= 2 ? s : nullptr;
    if (s[i] < 0x20 || s[i] > 0x7E) return nullptr;
  }
  return nullptr;
}

void RunDump() {
  char path[520];
  snprintf(path, sizeof(path), "%s\\..\\scriptapi_map.txt", self->folder);
  FILE* f = fopen(path, "w");
  if (!f) {
    api->log(self, "could not open scriptapi_map.txt for writing");
    return;
  }
  int ok = 0, bad = 0;
  char line[256];
  for (const auto& pair : kScriptApiPairs) {
    const char* name = ReadImageString(pair.name_addr);
    if (name) {
      fprintf(f, "%-40s 0x%08X\n", name, pair.thunk);
      ++ok;
    } else {
      ++bad;
    }
  }
  fclose(f);
  snprintf(line, sizeof(line),
           "script API map written to mods\\scriptapi_map.txt: %d resolved, %d unreadable", ok,
           bad);
  api->log(self, line);
  if (ok == 0) {
    api->log(self, "WARNING: no strings resolved - the image layout differs from the dump");
  }
}

bool g_scanned = false;
ULONGLONG g_player_since = 0;

void OnFrame(void*) {
  if (api->key_pressed(VK_F6)) {
    RunDump();
    return;
  }
  if (g_scanned) return;
  if (api->read_u32(0x8309ABEC) != 0) {  // player object exists
    if (!g_player_since) g_player_since = GetTickCount64();
    if (GetTickCount64() - g_player_since > 15000) {
      g_scanned = true;
      RunDump();
    }
  }
}

}  // namespace

extern "C" WML_EXPORT int wml_mod_init(const WmlApi* loader_api, const WmlMod* mod) {
  api = loader_api;
  self = mod;
  if (api->version < WML_API_VERSION) return 1;
  g_base = static_cast<uint8_t*>(api->guest_pointer(0));
  if (!g_base) return 2;
  api->on_frame(OnFrame, nullptr);
  api->log(self, "Script API dump armed: runs ~15s into gameplay, or press F6.");
  return 0;
}
