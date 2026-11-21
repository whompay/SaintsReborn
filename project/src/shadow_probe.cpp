// Native version + probe of the character/vehicle shadow-volume builder sub_822447B0 (the
// hottest function of the game's job worker threads: 11-26 % of each worker's
// samples). Only active with a file named "shadow_probe" next to the exe:
// logs every 10 s "SHADOW PROBE: calls/s, us per call, CPU ms/s, by caller
// and by object type (vtable)".
//
// Callers: sub_82234CA8 and sub_82237560 (both clear a 64 KB stack buffer,
// let the object fill it through vtable +48, then call this with the buffer,
// object +8 (position) and object +20 (rotation)).

#include <algorithm>
#include <cmath>
#include <atomic>
#include <cstring>
#include <string>
#include <vector>
#include <cstdio>
#include <mutex>
#include <unordered_map>

#include "saintsrow_config.h"
#include "saintsrow_init.h"

#include <rex/logging.h>
#include <rex/ppc/function.h>
#include <rex/ppc/intrinsics.h>

#ifdef _WIN32
#include <windows.h>
#endif

extern "C" void __imp__sub_822447B0(PPCContext& ctx, uint8_t* base);

namespace {

bool Enabled() {
  static const bool on = [] {
    FILE* f = std::fopen("shadow_probe", "rb");
    if (f) std::fclose(f);
    return f != nullptr;
  }();
  return on;
}

int64_t Now() {
#ifdef _WIN32
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return t.QuadPart;
#else
  return 0;
#endif
}
int64_t Freq() {
#ifdef _WIN32
  LARGE_INTEGER f;
  QueryPerformanceFrequency(&f);
  return f.QuadPart;
#else
  return 1;
#endif
}

struct Stat {
  uint64_t calls = 0;
  int64_t ticks = 0;
};
std::mutex g_mutex;
std::unordered_map<uint32_t, Stat> g_by_caller, g_by_vtable;
Stat g_total;
int64_t g_window_start = 0;

uint32_t Load32(uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, base + a, 4);
  return __builtin_bswap32(v);
}

std::string Top(std::unordered_map<uint32_t, Stat>& m, double freq) {
  std::vector<std::pair<uint32_t, Stat>> v(m.begin(), m.end());
  std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second.ticks > b.second.ticks; });
  std::string s;
  char buf[96];
  for (size_t i = 0; i < v.size() && i < 8; ++i) {
    std::snprintf(buf, sizeof buf, " [%08X %llu calls %.1f us]", v[i].first,
                  static_cast<unsigned long long>(v[i].second.calls),
                  v[i].second.calls ? v[i].second.ticks * 1e6 / freq / v[i].second.calls : 0.0);
    s += buf;
  }
  return s;
}

}  // namespace

#include "native_shadow.inc"
#include "native_math.inc"
#include "native_vfetch.inc"
#include "native_flush.inc"
#include "native_clip.inc"
#include "native_tex.inc"
#include "native_cull.inc"
#include "native_skin.inc"
#include "native_geom.inc"
#include "native_nav.inc"  // fused nav-mesh query 824A1318 (AGENT_NATIVE20B, default ON, native_nav_off)
#include "native_leaf.inc"  // hot main-thread leaves 821287B0 8263A140 82637C90 8262E958 82715AC0 (AGENT_NATIVE21B, opt-in native_leaf)
#include "native_rleaf.inc"  // render-chain leaves 825DB228 825C6018 8251C1E8 826446F8 82629CD0 (AGENT_NATIVE22B, opt-in native_rleaf)
#include "native_leaf2.inc"  // main/worker leaves batch 2, 9 functions (AGENT_NATIVE22B, opt-in native_leaf2)
#include "native_leaf3.inc"  // 82637080 82636980 atomic leaves, context-exact (AGENT_NATIVE23B2, opt-in native_leaf3)

PPC_FUNC(sub_822447B0) {
  if (!Enabled()) {
    const int mode = native_shadow::Mode();
    if (mode == 1) {
      if (native_shadow::Run(ctx, base, true)) return;
    } else if (mode == 2) {
      static thread_local uint32_t tick = 0;
      if ((++tick & 31) == 0) { native_shadow::Verify(ctx, base); return; }
      if (native_shadow::Run(ctx, base, true)) return;
    }
    __imp__sub_822447B0(ctx, base);
    return;
  }
  const uint32_t caller = uint32_t(ctx.lr);
  const uint32_t obj = ctx.r4.u32 - 8;
  const uint32_t vtable = obj >= 0x10000 ? Load32(base, obj) : 0;
  const int64_t t0 = Now();
  __imp__sub_822447B0(ctx, base);
  const int64_t dt = Now() - t0;
  std::lock_guard<std::mutex> lock(g_mutex);
  g_total.calls++;
  g_total.ticks += dt;
  auto& c = g_by_caller[caller];
  c.calls++;
  c.ticks += dt;
  auto& v = g_by_vtable[vtable];
  v.calls++;
  v.ticks += dt;
  if (!g_window_start) g_window_start = t0;
  static const double freq = double(Freq());
  const double secs = (t0 - g_window_start) / freq;
  if (secs >= 10.0) {
    REXLOG_INFO("SHADOW PROBE 10 s: {:.0f} calls/s, {:.1f} us per call, {:.1f} ms/s CPU | by caller:{} | by vtable:{}",
                g_total.calls / secs, g_total.calls ? g_total.ticks * 1e6 / freq / g_total.calls : 0.0,
                g_total.ticks * 1e3 / freq / secs, Top(g_by_caller, freq), Top(g_by_vtable, freq));
    g_total = {};
    g_by_caller.clear();
    g_by_vtable.clear();
    g_window_start = t0;
  }
}

// ALLOC PROBE (file "alloc_probe"): the game's VirtualAlloc wrapper 8271B570(r3 address, r4 size,
// r5 type, r6 protect) and VirtualFree 82717360(r3 address, r4 size, r5 type): calls and bytes per
// second by caller, every 10 s ("ALLOC PROBE"). Committing memory the game had decommitted makes the
// runtime zero it.
extern "C" void __imp__sub_8271B570(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_82717360(PPCContext& ctx, uint8_t* base);
namespace alloc_probe {
bool On() {
  static const bool on = [] {
    FILE* f = std::fopen("alloc_probe", "rb");
    if (f) std::fclose(f);
    return f != nullptr;
  }();
  return on;
}
struct S { uint64_t calls = 0, bytes = 0, ticks = 0; };
std::mutex m;
std::unordered_map<uint64_t, S> by;  // (kind << 63) | (type << 32) | caller
int64_t start = 0;
void Add(bool fr, uint32_t caller, uint32_t type, uint32_t size, int64_t dt) {
  std::lock_guard<std::mutex> lock(m);
  auto& s = by[(uint64_t(fr) << 63) | (uint64_t(type) << 32) | caller];
  ++s.calls; s.bytes += size; s.ticks += dt;
  const int64_t now = Now();
  if (!start) start = now;
  static const double freq = double(Freq());
  const double secs = (now - start) / freq;
  if (secs >= 10.0) {
    std::vector<std::pair<uint64_t, S>> v(by.begin(), by.end());
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second.ticks > b.second.ticks; });
    std::string t;
    for (size_t i = 0; i < v.size() && i < 10; ++i) {
      char buf[160];
      std::snprintf(buf, sizeof buf, " [%s %08X type %08X: %.0f/s, %.1f MB/s, %.1f ms/s]",
                    (v[i].first >> 63) ? "free" : "alloc", uint32_t(v[i].first),
                    uint32_t(v[i].first >> 32) & 0x7FFFFFFF, v[i].second.calls / secs,
                    v[i].second.bytes / secs / 1048576.0, v[i].second.ticks * 1e3 / freq / secs);
      t += buf;
    }
    REXLOG_INFO("ALLOC PROBE 10 s:{}", t);
    by.clear();
    start = now;
  }
}
}  // namespace alloc_probe

// ARENA COMMITS WITHOUT KERNEL ZEROING (2026-10-02). The game's job arenas (82633108 reserve, grown by
// 82633340 in 64 KB commits, shrunk by 826335C0, released by 82633240) are created and released ~1000x/s and
// recommit ~300 MB/s; the kernel zero-fills every commit (Memory::Zero) - ~5 % of all busy CPU at the air spot,
// on the job workers and the main thread. Arena memory below the high-water mark is reused with stale contents
// anyway, and arenas that need zeros (flag byte +56) memset it themselves, so the commit at 826334A8 gets
// X_MEM_NOZERO when dist\arena_nozero exists (opt-in); dist\arena_poison = NOZERO and fill with 0xCD (test that nothing
// relies on zeroed pages).
namespace arena_fast {
int Mode() {
  static const int m = [] {
    // Opt-in since 2026-10-02 20:30: one GPU device-removed (TDR) happened in an "on" bench run (nz_on3) - not
    // proven related, but no gain on 28 threads either. File "arena_nozero" = on.
    int r = 0;
    if (FILE* f = std::fopen("arena_nozero", "rb")) { std::fclose(f); r = 1; }
    else if (FILE* f2 = std::fopen("arena_poison", "rb")) { std::fclose(f2); r = 2; }
    REXLOG_INFO("Arena commits: {}", r == 0 ? "kernel zeroing (original)" : r == 1 ? "no kernel zeroing" :
                "no kernel zeroing, POISON fill 0xCD (test mode)");
    return r;
  }();
  return m;
}
}  // namespace arena_fast

PPC_FUNC(sub_8271B570) {
  if (uint32_t(ctx.lr) == 0x826334A8u && (ctx.r5.u32 & 0x1000u) && arena_fast::Mode() != 0) {
    ctx.r5.u64 = ctx.r5.u32 | 0x00800000u;  // X_MEM_NOZERO
    if (arena_fast::Mode() == 2) {
      const uint32_t size = ctx.r4.u32;
      __imp__sub_8271B570(ctx, base);
      if (ctx.r3.u32) std::memset(base + ctx.r3.u32, 0xCD, size);
      return;
    }
  }
  if (!alloc_probe::On()) { __imp__sub_8271B570(ctx, base); return; }
  const uint32_t caller = uint32_t(ctx.lr), size = ctx.r4.u32, type = ctx.r5.u32;
  const int64_t t0 = Now();
  __imp__sub_8271B570(ctx, base);
  alloc_probe::Add(false, caller, type, size, Now() - t0);
}
PPC_FUNC(sub_82717360) {
  if (!alloc_probe::On()) { __imp__sub_82717360(ctx, base); return; }
  const uint32_t caller = uint32_t(ctx.lr), size = ctx.r4.u32, type = ctx.r5.u32;
  const int64_t t0 = Now();
  __imp__sub_82717360(ctx, base);
  alloc_probe::Add(true, caller, type, size, Now() - t0);
}

// ---------------------------------------------------------------------------------------------------------------------
// SHADOW VOLUME DRAW GATE (whompay, 2026-10-27). sub_82234DE8(r3 object, r4 volume descriptor written by 82234CA8:
// +0 u16 index count, r5, r6) draws one character / vehicle stencil shadow volume through the mesh draw 8223E510 into the
// depth/stencil pre-pass (RB_DEPTHCONTROL 1C7187E3 z-pass / C07E07E3 z-fail). With index count 0 the original returns
// without drawing; a skipped volume takes exactly that path (no other side effect; callers 82233B64 / 82236ABC ignore r3).
// GPU: the volume class costs ~0.44 ms per frame at the AIR spot (fill of the 4x MSAA mask), independent of draw count.
// Switches (dist files, default absent = original):
//   shadow_vol_stats : log 'SHADOW VOL 10 s' (volumes/s, indices, distance histogram, object floats) - diagnostics only.
//   shadow_vol_skip  : skip every volume (upper bound of the volume GPU cost; character / vehicle shadows disappear).
//   shadow_vol_far   : skip volumes of objects further than shadow_vol_far.txt metres from the camera (default 60, re-read 2 s).
//   shadow_vol_far150: the same with a fixed 150 m (bench arm; flag files are created empty by the runner).
//   shadow_vol.off   : wins over everything.
extern "C" void __imp__sub_82234DE8(PPCContext& ctx, uint8_t* base);
namespace shadow_vol {
bool FileOn(const char* n) {
  FILE* f = std::fopen(n, "rb");
  if (f) std::fclose(f);
  return f != nullptr;
}
struct Cfg {
  bool stats = false, skip = false, far_cull = false, far150 = false;
  bool any() const { return stats || skip || far_cull; }
};
const Cfg& Get() {
  static const Cfg c = [] {
    Cfg r;
    if (!FileOn("shadow_vol.off")) {
      r.stats = FileOn("shadow_vol_stats");
      r.skip = FileOn("shadow_vol_skip");
      r.far150 = FileOn("shadow_vol_far150");
      r.far_cull = FileOn("shadow_vol_far") || r.far150;
    }
    if (r.any())
      REXLOG_INFO("SHADOW VOL gate (whompay): stats {} skip-all {} far-cull {}", r.stats, r.skip, r.far_cull);
    return r;
  }();
  return c;
}
float FarDist() {
  if (Get().far150) return 150.0f;
  static std::atomic<float> v{60.0f};
  static std::atomic<int64_t> next{0};
  const int64_t now = int64_t(GetTickCount64());
  int64_t n = next.load(std::memory_order_relaxed);
  if (now >= n && next.compare_exchange_strong(n, now + 2000)) {
    float d = 60.0f;
    if (FILE* f = std::fopen("shadow_vol_far.txt", "rb")) {
      if (std::fscanf(f, "%f", &d) != 1) d = 60.0f;
      std::fclose(f);
    }
    if (!(d >= 1.0f && d <= 100000.0f)) d = 60.0f;
    v.store(d, std::memory_order_relaxed);
  }
  return v.load(std::memory_order_relaxed);
}
inline float F(uint8_t* b, uint32_t a) {
  const uint32_t w = native_shadow::L32(b, a);
  float f;
  std::memcpy(&f, &w, 4);
  return f;
}
// Stats (10 s windows).
std::atomic<uint64_t> s_calls{0}, s_empty{0}, s_idx{0}, s_skipped{0}, s_skipped_idx{0};
std::atomic<uint64_t> s_hist[6] = {};  // < 25, < 50, < 100, < 200, < 400, >= 400 m (drawn + skipped, non-empty)
std::atomic<uint64_t> s_by_lr[2] = {};  // 82233B78 / 82236AD0 (other -> [1])
std::mutex s_mu;
float s_fmin[3] = {1e30f, 1e30f, 1e30f}, s_fmax[3] = {-1e30f, -1e30f, -1e30f};
std::atomic<int64_t> s_next{0};
void Report() {
  const int64_t now = int64_t(GetTickCount64());
  int64_t n = s_next.load(std::memory_order_relaxed);
  if (now < n || !s_next.compare_exchange_strong(n, now + 10000)) return;
  if (!n) return;  // first call: start the window
  const double sec = double(now - (n - 10000)) / 1000.0;
  float fmn[3], fmx[3];
  {
    std::lock_guard<std::mutex> l(s_mu);
    for (int i = 0; i < 3; ++i) { fmn[i] = s_fmin[i]; fmx[i] = s_fmax[i]; s_fmin[i] = 1e30f; s_fmax[i] = -1e30f; }
  }
  const uint64_t c = s_calls.exchange(0), e = s_empty.exchange(0), ix = s_idx.exchange(0), sk = s_skipped.exchange(0),
                 skix = s_skipped_idx.exchange(0);
  uint64_t h[6];
  for (int i = 0; i < 6; ++i) h[i] = s_hist[i].exchange(0);
  const uint64_t l0 = s_by_lr[0].exchange(0), l1 = s_by_lr[1].exchange(0);
  static int lines = 0;
  if (lines++ < 400)
    REXLOG_INFO("SHADOW VOL 10 s: {:.0f} calls/s ({:.0f} empty), {:.0f} volumes/s drawn+skipped, avg {:.0f} indices | skipped {:.0f}/s "
                "({:.0f} indices/s) far {:.0f} m | dist <25 {:.0f} <50 {:.0f} <100 {:.0f} <200 {:.0f} <400 {:.0f} >=400 {:.0f} /s | "
                "caller 82233B78 {:.0f} other {:.0f} /s | obj+56 [{:.3g} {:.3g}] +100 [{:.3g} {:.3g}] +104 [{:.3g} {:.3g}]",
                c / sec, e / sec, (c - e) / sec, (c - e) ? double(ix) / double(c - e) : 0.0, sk / sec, skix / sec,
                Get().far_cull ? FarDist() : 0.0f, h[0] / sec, h[1] / sec, h[2] / sec, h[3] / sec, h[4] / sec, h[5] / sec, l0 / sec,
                l1 / sec, fmn[0], fmx[0], fmn[1], fmx[1], fmn[2], fmx[2]);
}
}  // namespace shadow_vol

PPC_FUNC(sub_82234DE8) {
  namespace sv = shadow_vol;
  const sv::Cfg& cfg = sv::Get();
  if (!cfg.any()) { __imp__sub_82234DE8(ctx, base); return; }
  const uint32_t obj = ctx.r3.u32, desc = ctx.r4.u32;
  const uint32_t count = desc ? native_shadow::L16(base, desc) : 0u;
  if (cfg.stats) {
    sv::s_calls.fetch_add(1, std::memory_order_relaxed);
    sv::Report();
  }
  if (count == 0) {
    if (cfg.stats) sv::s_empty.fetch_add(1, std::memory_order_relaxed);
    __imp__sub_82234DE8(ctx, base);
    return;
  }
  float dist = 0.0f;
  if (cfg.stats || cfg.far_cull) {
    const float dx = sv::F(base, obj + 8) - sv::F(base, 0x827D9778u + 44), dy = sv::F(base, obj + 12) - sv::F(base, 0x827D9778u + 48),
                dz = sv::F(base, obj + 16) - sv::F(base, 0x827D9778u + 52);
    dist = std::sqrt(dx * dx + dy * dy + dz * dz);
  }
  bool skip = cfg.skip;
  if (!skip && cfg.far_cull && std::isfinite(dist) && dist > sv::FarDist()) skip = true;
  if (cfg.stats) {
    sv::s_idx.fetch_add(count, std::memory_order_relaxed);
    const int b = !(dist < 400.0f) ? 5 : dist < 25.0f ? 0 : dist < 50.0f ? 1 : dist < 100.0f ? 2 : dist < 200.0f ? 3 : 4;
    sv::s_hist[b].fetch_add(1, std::memory_order_relaxed);
    sv::s_by_lr[uint32_t(ctx.lr) == 0x82233B78u ? 0 : 1].fetch_add(1, std::memory_order_relaxed);
    const float v[3] = {sv::F(base, obj + 56), sv::F(base, obj + 100), sv::F(base, obj + 104)};
    std::lock_guard<std::mutex> l(sv::s_mu);
    for (int i = 0; i < 3; ++i) {
      if (v[i] < sv::s_fmin[i]) sv::s_fmin[i] = v[i];
      if (v[i] > sv::s_fmax[i]) sv::s_fmax[i] = v[i];
    }
  }
  if (skip) {
    if (cfg.stats) {
      sv::s_skipped.fetch_add(1, std::memory_order_relaxed);
      sv::s_skipped_idx.fetch_add(count, std::memory_order_relaxed);
    }
    return;  // = the original's index-count-0 path (returns without drawing)
  }
  __imp__sub_82234DE8(ctx, base);
}

// ---------------------------------------------------------------------------------------------------------------------
// POPULATION CENSUS (AGENT_NATIVE26B, 2026-10-27). Opt-in dist file "pop_census" (absent = nothing runs; the per-frame
// hook in render_fixes.cpp 82194BD8 is one static bool test). Proves NPCs / traffic are loaded in a bench log and shows the
// CPU cost: a background thread logs every 10 s
//   'POP CENSUS 10 s: N fps, frame avg/max ms | objects live L: people P (<30 m a, <60 m b, <120 m c) vehicles V (...)
//    other O | types ... | density ped/veh f f'
//   'POP CPU 10 s per frame: <thread name> ms ...' (top 8 host threads of the process by CPU time per frame, GetThreadTimes).
// Object table = the one World Studio uses (0x830866C8, 4096 slots of 16 bytes, object at +12; object +68 handle whose low
// 16 bits = slot, +72 type: 1 person, 5 vehicle; +20 position). Camera position 0x827D9778 +44. Density = the game's
// ambient ped / vehicle density floats 0x827AD060 / 0x827AD064 (Benchmark mod 'density_half' variant). Read-only.
#include <chrono>
#include <thread>
#include <map>
#ifdef _WIN32
#include <tlhelp32.h>
#endif
namespace sr26_pop {
std::atomic<uint64_t> g_frames{0}, g_frame_ticks_sum{0}, g_frame_ticks_max{0};
int64_t g_last = 0;
bool On() {
  static const bool on = [] {
    FILE* f = std::fopen("pop_census", "rb");
    if (f) std::fclose(f);
    return f != nullptr;
  }();
  return on;
}
bool Readable(uint8_t* base, uint32_t a, size_t len) {
#ifdef _WIN32
  if (a < 0x10000u || uint64_t(a) + len > 0xFFFF0000ull) return false;
  const uintptr_t p = reinterpret_cast<uintptr_t>(native_shadow::G(base, a));
  MEMORY_BASIC_INFORMATION mi{};
  if (!VirtualQuery(reinterpret_cast<void*>(p), &mi, sizeof(mi))) return false;
  if (mi.State != MEM_COMMIT || (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
  return p + len <= reinterpret_cast<uintptr_t>(mi.BaseAddress) + mi.RegionSize;
#else
  return false;
#endif
}
float Fl(uint8_t* b, uint32_t a) {
  const uint32_t w = native_shadow::L32(b, a);
  float f;
  std::memcpy(&f, &w, 4);
  return f;
}
void Census(uint8_t* base, double sec, uint64_t frames, double avg_ms, double max_ms) {
  constexpr uint32_t kTable = 0x830866C8u, kSlots = 4096, kCam = 0x827D9778u;
  const float cx = Fl(base, kCam + 44), cy = Fl(base, kCam + 48), cz = Fl(base, kCam + 52);
  uint32_t live = 0, n[3] = {}, nr[3][3] = {};  // [person, vehicle, other][<30, <60, <120 m]
  std::map<uint32_t, uint32_t> types;
  for (uint32_t i = 0; i < kSlots; ++i) {
    const uint32_t obj = native_shadow::L32(base, kTable + 12 + i * 16);
    if (!obj || !Readable(base, obj, 80)) continue;
    const uint32_t handle = native_shadow::L32(base, obj + 68), type = native_shadow::L32(base, obj + 72);
    if (!handle || (handle & 0xFFFFu) != i || type == 0 || type > 64) continue;
    ++live;
    ++types[type];
    const int k = type == 1 ? 0 : type == 5 ? 1 : 2;
    ++n[k];
    const float dx = Fl(base, obj + 20) - cx, dy = Fl(base, obj + 24) - cy, dz = Fl(base, obj + 28) - cz;
    const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (std::isfinite(d)) {
      if (d < 30.0f) ++nr[k][0];
      if (d < 60.0f) ++nr[k][1];
      if (d < 120.0f) ++nr[k][2];
    }
  }
  std::string ts;
  for (auto& [t, c] : types) ts += fmt::format(" {}:{}", t, c);
  static int lines = 0;
  if (lines++ < 2000)
    REXLOG_INFO("POP CENSUS 10 s: {:.1f} fps, frame avg {:.2f} max {:.2f} ms | objects live {}: people {} (<30 m {} <60 m {} <120 m {}) "
                "vehicles {} ({} {} {}) other {} ({} {} {}) | types{} | density ped {:.2f} veh {:.2f} | cam {:.1f} {:.1f} {:.1f}",
                frames / sec, avg_ms, max_ms, live, n[0], nr[0][0], nr[0][1], nr[0][2], n[1], nr[1][0], nr[1][1],
                nr[1][2], n[2], nr[2][0], nr[2][1], nr[2][2], ts, Fl(base, 0x827AD060u), Fl(base, 0x827AD064u), cx, cy, cz);
}
#ifdef _WIN32
struct ThreadCpu { std::string name; uint64_t t = 0; };
std::map<DWORD, ThreadCpu> g_threads;
std::string ThreadName(HANDLE h) {
  PWSTR w = nullptr;
  std::string s;
  // via GetProcAddress like perf_monitor.cpp (no dependency on the SDK declaring it / kernel32.lib exporting it)
  using GetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
  static const auto fn = reinterpret_cast<GetThreadDescriptionFn>(
      reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription")));
  if (fn && SUCCEEDED(fn(h, &w)) && w) {
    char buf[128] = {};
    WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, sizeof(buf) - 1, nullptr, nullptr);
    s = buf;
    LocalFree(w);
  }
  return s;
}
void ThreadCensus(uint64_t frames) {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap == INVALID_HANDLE_VALUE) return;
  const DWORD pid = GetCurrentProcessId();
  THREADENTRY32 te{};
  te.dwSize = sizeof(te);
  std::vector<std::pair<double, std::string>> rows;
  std::map<DWORD, ThreadCpu> now;
  for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
    if (te.th32OwnerProcessID != pid) continue;
    HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
    if (!h) continue;
    FILETIME c, e, k, u;
    if (GetThreadTimes(h, &c, &e, &k, &u)) {
      ThreadCpu tc;
      tc.t = (uint64_t(k.dwHighDateTime) << 32 | k.dwLowDateTime) + (uint64_t(u.dwHighDateTime) << 32 | u.dwLowDateTime);
      auto it = g_threads.find(te.th32ThreadID);
      tc.name = it != g_threads.end() ? it->second.name : ThreadName(h);
      if (tc.name.empty()) tc.name = fmt::format("tid{:X}", te.th32ThreadID);
      if (it != g_threads.end() && frames && tc.t >= it->second.t)
        rows.emplace_back(double(tc.t - it->second.t) / 10000.0 / double(frames), tc.name + fmt::format("[{:X}]", te.th32ThreadID));
      now[te.th32ThreadID] = tc;
    }
    CloseHandle(h);
  }
  CloseHandle(snap);
  g_threads.swap(now);
  std::sort(rows.rbegin(), rows.rend());
  double total = 0;
  for (auto& r : rows) total += r.first;
  std::string line;
  for (size_t i = 0; i < rows.size() && i < 10; ++i) line += fmt::format(" | {} {:.2f}", rows[i].second, rows[i].first);
  static int lines = 0;
  if (!rows.empty() && lines++ < 2000) REXLOG_INFO("POP CPU 10 s per frame (ms): all threads {:.2f}{}", total, line);
}
#endif
void CensusThread(uint8_t* base) {
  uint64_t f0 = g_frames.load();
  auto t0 = std::chrono::steady_clock::now();
#ifdef _WIN32
  ThreadCensus(0);
#endif
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(10));
    const auto t1 = std::chrono::steady_clock::now();
    const uint64_t f1 = g_frames.load(), sum = g_frame_ticks_sum.exchange(0), mx = g_frame_ticks_max.exchange(0);
    const double sec = std::chrono::duration<double>(t1 - t0).count(), ms_per_tick = 1000.0 / double(Freq());
    const uint64_t fr = f1 - f0;
    Census(base, sec, fr, fr ? double(sum) * ms_per_tick / double(fr) : 0.0, double(mx) * ms_per_tick);
#ifdef _WIN32
    ThreadCensus(fr);
#endif
    f0 = f1;
    t0 = t1;
  }
}
}  // namespace sr26_pop

// Main thread, once per frame (render_fixes.cpp sub_82194BD8).
void sr_pop_census_frame(uint8_t* base) {
  if (!::sr26_pop::On()) return;
  const int64_t t = Now();
  if (::sr26_pop::g_last) {
    const uint64_t d = uint64_t(t - ::sr26_pop::g_last);
    ::sr26_pop::g_frame_ticks_sum.fetch_add(d, std::memory_order_relaxed);
    if (d > ::sr26_pop::g_frame_ticks_max.load(std::memory_order_relaxed)) ::sr26_pop::g_frame_ticks_max.store(d, std::memory_order_relaxed);
  } else {
    REXLOG_INFO("POP CENSUS on (AGENT_NATIVE26B): object table 0x830866C8, log every 10 s");
    std::thread([base] { ::sr26_pop::CensusThread(base); }).detach();
  }
  ::sr26_pop::g_last = t;
  ::sr26_pop::g_frames.fetch_add(1, std::memory_order_relaxed);
}
