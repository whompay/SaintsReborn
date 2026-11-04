// Rendering fixes for the PC port (always on; each has an off switch file
// next to saintsrow.exe for testing).
//
// 1. Single-pass rendering. The game's render configs (9 x 156 bytes at
//    0x827D6D38: +0 MSAA 0/1/2 = 1x/2x/4x, +12 tile count, +20 tile rects
//    x1,y1,x2,y2) include 2x/4x MSAA modes split into 2-4 screen strips
//    ("predicated tiling"), because on the Xbox 360 a 4x MSAA 720p frame does
//    not fit in the GPU's 10 MB of EDRAM. The shop menus use the 4-strip
//    mode: the whole scene is drawn 4 times, and the replayed command buffers
//    are also what broke the asynchronous GPU queue (rainbow garbage, damaged
//    textures, freezes). Before the game lays out its configs (82183008), all
//    of them become 1x with one full-screen tile. The internal 2x resolution
//    already smooths edges. File "msaa_tiling" = original behaviour.
//
// 2. Rooftop visibility. The game only draws the parts of the city its
//    precomputed visibility data (PVS) lists for the camera's grid cell. The
//    grid (0x832AC0C8: +0 cell count, +4 cells (48 bytes each), +8/+12/+16
//    cells in x/y/z, +20 origin, +44 cell size) is flat: 10 x 10 m cells, one
//    layer ~45 m tall, computed from street level. On rooftops you see much
//    more than from the street below, so whole areas vanish (black / sky) and
//    pop back a few steps further, in the next cell. With the camera more
//    than pvs_height meters above the grid's floor (default 25, i.e. about
//    11 m above the street; with street_points.txt: 11 m above the nearest
//    street point, so raised streets and hills keep it) the lookup (825C1B18) reports no cell, and the
//    game then draws everything in range, as it does outside the PVS grid.
//    Street-level rendering is unchanged. File "pvs_original" = original behaviour.

#include "saintsrow_config.h"
#include "saintsrow_init.h"
#include "options_menu.h"
#include "wml/mod_loader.h"

#include <rex/logging.h>
#include <rex/ppc/function.h>

#include <cstdint>
#include <intrin.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <array>
#include <mutex>
#include <atomic>
#include <chrono>
#include <map>
#include <rex/system/kernel_state.h>
#include <rex/system/xthread.h>
extern "C" __declspec(dllimport) unsigned long long __stdcall GetTickCount64(void);
#include <unordered_map>
#include <vector>
#include <string>

extern "C" __declspec(dllimport) unsigned long __stdcall GetCurrentThreadId(void);
extern "C" uint32_t sr_render_list_cap();
extern "C" uint32_t sr_render_list_base(int n);
extern "C" __declspec(dllimport) short __stdcall GetAsyncKeyState(int);

namespace {

inline uint32_t Rd32(uint8_t* base, uint32_t addr) {
    return __builtin_bswap32(*reinterpret_cast<volatile uint32_t*>(base + addr));
}
inline float RdF(uint8_t* base, uint32_t addr) {
    const uint32_t v = Rd32(base, addr);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}
inline void Wr32(uint8_t* base, uint32_t addr, uint32_t value) {
    *reinterpret_cast<volatile uint32_t*>(base + addr) = __builtin_bswap32(value);
}
bool FileExists(const char* name) {
    if (FILE* f = std::fopen(name, "rb")) {
        std::fclose(f);
        return true;
    }
    return false;
}
// 2026-09-30: the air-view diagnostics of 2026-09-29 (per-object AIRFLICKER reasons, FRUSTUM CHECK double test)
// cost CPU while flying; only with dist\air_diagnostics now. IO trace only with dist\io_trace.
bool AirDiag() { static const bool on = FileExists("air_diagnostics"); return on; }
// 2026-10-08: the air view (2. rooftop visibility, 3. far clip regions and
// everything keyed on far_clip::g_air_view) is for flying with the
// Superpowers mod. Without it the switch also fired on rooftops, bridges and
// overpasses and raised parts of the multiplayer maps, drawing far more than
// the original game (missing road textures, glitches, frame drops). Now it is
// only on while the Superpowers mod is enabled; file "air_view_always" turns
// it on without the mod (the files pvs_original / far_clip_original still
// turn it off with the mod).
bool AirViewEnabled() {
    static bool on = false, forced = FileExists("air_view_always"), logged = false;
    static auto next = std::chrono::steady_clock::time_point{};
    if (on || forced) return true;
    const auto now = std::chrono::steady_clock::now();
    if (now < next) return false;
    next = now + std::chrono::seconds(2);
    for (const auto& id : wml::EnabledModIds())
        if (_stricmp(id.c_str(), "Superpowers") == 0) on = true;
    if (!logged || on) {
        logged = true;
        REXLOG_INFO("Air view (rooftop PVS / far clip): {}", on ? "on (Superpowers mod enabled)" : "off (no Superpowers mod)");
    }
    return on;
}

constexpr uint32_t kRenderConfigs = 0x827D6D38;

// Street points (x y z per line, '#' comments), bucketed in 32 m squares.
constexpr float kStreetLimit = 11.0f;  // camera this far above the street = rooftop
struct StreetPoints {
    bool loaded = false;
    std::unordered_map<uint64_t, std::vector<std::array<float, 3>>> buckets;
};
inline uint64_t BucketKey(int32_t bx, int32_t bz) { return (uint64_t(uint32_t(bx)) << 32) | uint32_t(bz); }
const StreetPoints& Streets() {
    static const StreetPoints points = [] {
        StreetPoints p;
        FILE* f = std::fopen("street_points.txt", "rb");
        if (!f) {
            REXLOG_INFO("PVS: street_points.txt missing, rooftop rule uses the grid floor");
            return p;
        }
        char line[256];
        size_t count = 0;
        while (std::fgets(line, sizeof(line), f)) {
            float x, y, z;
            if (line[0] == '#' || std::sscanf(line, "%f %f %f", &x, &y, &z) != 3) continue;
            p.buckets[BucketKey(int32_t(std::floor(x / 32.0f)), int32_t(std::floor(z / 32.0f)))].push_back({x, y, z});
            ++count;
        }
        std::fclose(f);
        p.loaded = count > 0;
        REXLOG_INFO("PVS: {} street points loaded", count);
        return p;
    }();
    return points;
}
// The street under the camera: among the street points within 25 m that are
// not above the camera (so an overpass above does not count), the height
// level (3 m bands) most of them share, else the nearest point within 60 m.
// The points are the game's navpoints/spawns; about 1 in 9 sits on a roof or
// stairs. With "the highest point below the camera" a roof spawn became the
// street when standing on that roof: air view off, street PVS on, the roof
// and courtyard black for seconds. Streets have many path nodes, roofs few.
bool NearestStreetHeight(float x, float y, float z, float* height) {
    const StreetPoints& p = Streets();
    if (!p.loaded) return false;
    const int32_t bx = int32_t(std::floor(x / 32.0f)), bz = int32_t(std::floor(z / 32.0f));
    float nearest = 60.0f * 60.0f, nearest_y = 0.0f, below_y = -1e9f;
    bool found = false, found_below = false;
    std::map<int32_t, std::pair<int, float>> bands;  // 3 m band -> points, highest y
    for (int32_t dx = -2; dx <= 2; ++dx) {
        for (int32_t dz = -2; dz <= 2; ++dz) {
            auto it = p.buckets.find(BucketKey(bx + dx, bz + dz));
            if (it == p.buckets.end()) continue;
            for (const auto& q : it->second) {
                const float d = (q[0] - x) * (q[0] - x) + (q[2] - z) * (q[2] - z);
                if (d < nearest) {
                    nearest = d;
                    nearest_y = q[1];
                    found = true;
                }
                if (d < 25.0f * 25.0f && q[1] <= y + 1.0f) {
                    auto& b = bands[int32_t(std::floor(q[1] / 3.0f))];
                    b.second = b.first ? std::max(b.second, q[1]) : q[1];
                    ++b.first;
                    found_below = true;
                }
            }
        }
    }
    if (found_below) {
        // Most points wins; the lower band wins a tie (map is ordered low -> high).
        int best = 0;
        for (const auto& [band, b] : bands) {
            (void)band;
            if (b.first > best) { best = b.first; below_y = b.second; }
        }
        *height = below_y;
    } else if (found) *height = nearest_y;
    return found || found_below;
}
// Street height that doesn't drop out: the last street point found is kept
// (for up to 400 m of travel) when there's no street point within 60 m of the
// camera (over water, parks, rooftops far from roads). Before, a failed lookup
// fell back to the PVS grid floor or "not high" - while flying high the air
// view switched off and on (whole buildings popped in and out).
bool StickyStreetHeight(float x, float y, float z, float* height) {
    static float last_x = 0.0f, last_z = 0.0f, last_h = 0.0f;
    static bool have_last = false;
    float h;
    if (NearestStreetHeight(x, y, z, &h)) {
        last_x = x; last_z = z; last_h = h; have_last = true;
        *height = h;
        return true;
    }
    if (have_last && (x - last_x) * (x - last_x) + (z - last_z) * (z - last_z) < 400.0f * 400.0f) {
        *height = last_h;
        return true;
    }
    return false;
}
// Air view on / off with a minimum time in each state (no flapping when the
// camera bobs around the limit).
bool Debounced(bool want, bool& state, std::chrono::steady_clock::time_point& since, int min_ms) {
    const auto now = std::chrono::steady_clock::now();
    // Switching ON (camera went high: draw everything) happens at once; only
    // switching back OFF waits min_ms since the last change. With the wait
    // both ways, a super jump shortly after landing kept the street rules
    // (PVS cell + far clip regions) for up to 3 s: most of the city vanished.
    if (want != state && (want || now - since >= std::chrono::milliseconds(min_ms))) {
        state = want;
        since = now;
    }
    return state;
}
// Camera climbing fast (super jump, flying up): switch to the air view early,
// 5 m above the street instead of 11, so the street-level culling doesn't
// hide the city during the climb.
float ClimbRate(float y) {
    static float last_y = 0.0f;
    static auto last_t = std::chrono::steady_clock::time_point{};
    static float rate = 0.0f;
    const auto now = std::chrono::steady_clock::now();
    const float dt = std::chrono::duration<float>(now - last_t).count();
    if (dt >= 0.1f) {
        rate = dt < 1.0f ? (y - last_y) / dt : 0.0f;
        last_y = y;
        last_t = now;
    }
    return rate;
}
bool ClimbingFast(float y) { return ClimbRate(y) > 8.0f; }
// Recent visibility events (PVS set changes, air view on/off), dumped by the
// F9 mark: what changed in the 3 s before the moment the player marked.
struct Event { std::chrono::steady_clock::time_point t; std::string text; };
std::mutex g_events_mutex;
std::vector<Event> g_events;
void AddEvent(std::string text) {
    std::lock_guard<std::mutex> lock(g_events_mutex);
    const auto now = std::chrono::steady_clock::now();
    if (g_events.size() > 400) g_events.erase(g_events.begin(), g_events.begin() + 200);
    g_events.push_back({now, std::move(text)});
}
std::string RecentEvents(int ms) {
    std::lock_guard<std::mutex> lock(g_events_mutex);
    const auto now = std::chrono::steady_clock::now();
    std::string out;
    for (const auto& e : g_events) {
        const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(now - e.t).count();
        if (age <= ms) out += fmt::format(" [-{} ms {}]", age, e.text);
    }
    return out;
}
constexpr uint32_t kRenderConfigSize = 156;
constexpr uint32_t kRenderConfigCount = 9;

}  // namespace

extern "C" void __imp__sub_82183008(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_82183008) {
    static const bool keep_tiling = FileExists("msaa_tiling");
    if (!keep_tiling) {
        // Config 0 is the normal 1x single-tile one; copy its full-screen rect.
        uint32_t x2 = Rd32(base, kRenderConfigs + 28);
        uint32_t y2 = Rd32(base, kRenderConfigs + 32);
        // Another aspect ratio: the frame is narrower / wider than 1280, and
        // config 0 (already 1x, one tile) gets the new width too.
        const int aspect_width = sr::AspectFrameWidth();
        const bool widen = aspect_width > 0 && x2 == 1280;
        if (widen) {
            x2 = uint32_t(aspect_width);
            if (y2 == 720) y2 = uint32_t(sr::FrameHeight());
        }
        REXLOG_INFO("Render configs: full-screen tile {}x{}", x2, y2);
        for (uint32_t i = 0; i < kRenderConfigCount; ++i) {
            const uint32_t e = kRenderConfigs + i * kRenderConfigSize;
            if (Rd32(base, e) == 0 && Rd32(base, e + 12) == 1) {
                if (widen && Rd32(base, e + 28) == 1280) Wr32(base, e + 28, x2);
                if (widen && Rd32(base, e + 32) == 720) Wr32(base, e + 32, y2);
                continue;
            }
            Wr32(base, e, 0);       // no MSAA
            Wr32(base, e + 12, 1);  // one tile
            Wr32(base, e + 20, 0);
            Wr32(base, e + 24, 0);
            Wr32(base, e + 28, x2);
            Wr32(base, e + 32, y2);
        }
    }
    __imp__sub_82183008(ctx, base);
}

// 9. Aspect ratio (Display > Aspect Ratio, aspect.txt, read at start-up;
//    options_menu.cpp). The game's render set-up (82184260) hard-codes a
//    1280 x 720 frame and passes {+0 width, +4 height, +16 aspect} to
//    8263D8D8, which sizes the frame buffers. Other ratios keep the 720 lines
//    and change the width (4:3 = 960, 21:9 = 1680) and the aspect; the window
//    shows the frame at that shape (black bars around it). The field of view
//    follows: 8210A860 widens the camera's 4:3 FOV (0x827D9778 +188) for 16:9,
//    2 atan(tan(fov / 2) * 4/3); here the factor is ratio / (4/3). Not when the
//    game uses its 4:3 FOV as it is (byte 0x8370D991).
extern "C" void __imp__sub_8263D8D8(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_8263D8D8) {
    const uint32_t s = ctx.r3.u32;
    const uint32_t w = Rd32(base, s), h = Rd32(base, s + 4), a_bits = Rd32(base, s + 16);
    float a;
    std::memcpy(&a, &a_bits, 4);
    const int width = sr::AspectFrameWidth();
    if (width > 0 && h == 720) {
        const float ratio = sr::AspectRatioValue();
        uint32_t bits;
        std::memcpy(&bits, &ratio, 4);
        const int height = sr::FrameHeight();
        Wr32(base, s, uint32_t(width));
        Wr32(base, s + 4, uint32_t(height));
        Wr32(base, s + 16, bits);
        REXLOG_INFO("Aspect ratio: frame {}x{} (aspect {:.3f}) -> {}x{} (aspect {:.3f})", w, h, a, width, height, ratio);
    } else {
        REXLOG_INFO("Aspect ratio: frame {}x{} (aspect {:.3f}), unchanged", w, h, a);
    }
    __imp__sub_8263D8D8(ctx, base);
}

extern "C" void __imp__sub_8210A860(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_8210A860) {
    __imp__sub_8210A860(ctx, base);
    if (sr::AspectFrameWidth() == 0 || base[0x8370D991] != 0) return;
    const float ratio = sr::AspectRatioValue();
    const float fov = RdF(base, 0x827D9778 + 188);
    const float to_rad = RdF(base, 0x8208973C) * RdF(base, 0x8202073C);   // the game's own constants
    const float from_rad = RdF(base, 0x8203B42C) * RdF(base, 0x820896B8);
    const float wide = float(std::atan(std::tan(double(fov * to_rad)) * double(ratio * 0.75f))) * from_rad;
    ctx.f1.f64 = double(wide);
    static int logs = 0;
    if (logs < 3) {
        ++logs;
        REXLOG_INFO("Aspect ratio: FOV {:.2f} -> {:.2f} for {:.3f}", fov, wide, ratio);
    }
}

constexpr uint32_t kPvsGrid = 0x832AC0C8;      // pointer to the current chunk's PVS grid
constexpr uint32_t kPvsCellIndex = 0x832AC0E4;  // index of the camera's cell (written here)
constexpr uint32_t kPvsLockedCell = 0x827ABF34;  // debug "PVS cell is locked" index (-1 = off)

// Rooftop visibility union. A PVS cell (48 bytes) holds a 256-bit mask of the
// streamed chunks that can be seen from it (+0..+31) and two bit streams of
// the objects that can be seen (+32 and +40, each {byte size, data pointer}).
// Stream (read by 825C6018, bits low first): 8-bit chunk id (255 = end), then
// 5-bit runs of objects, alternately hidden / shown, starting hidden; a run
// may be 0 (switches without objects); a 0 run after a 0 run ends the chunk
// and the next 8 bits are the next chunk id. Read by 82128848 (chunks),
// 825C4030 / 825C43D8 / 825C4770 (objects).
// On a rooftop the game's own cell is too small (it was computed at street
// level), and "no cell" draws everything. Instead the camera gets a made-up
// cell: the union of the cells within pvs_union_radius cells (file, default
// 4; 0 = draw everything as before). Rebuilt when the cell or radius changes.
namespace pvs_union {
struct BitReader {
    uint8_t* base;
    uint32_t data;
    uint32_t size;  // bytes (cell +32 / +40)
    uint32_t pos = 0;
    bool bad = false;
    uint32_t Read(int n) {
        uint32_t v = 0;
        for (int i = 0; i < n; ++i, ++pos) {
            if ((pos >> 3) >= size) {
                bad = true;
                return 255;
            }
            v |= uint32_t((base[data + (pos >> 3)] >> (pos & 7)) & 1) << i;
        }
        return v;
    }
};
struct BitWriter {
    std::vector<uint8_t> bytes;
    uint32_t pos = 0;
    void Write(uint32_t v, int n) {
        for (int i = 0; i < n; ++i, ++pos) {
            if ((pos >> 3) >= bytes.size()) bytes.push_back(0);
            if ((v >> i) & 1) bytes[pos >> 3] |= uint8_t(1u << (pos & 7));
        }
    }
};
using Visible = std::map<uint32_t, std::vector<uint8_t>>;  // chunk id -> shown flags
void Decode(uint8_t* base, uint32_t data, uint32_t size, Visible& out) {
    if (!data || !size || size > 1024 * 1024) return;
    BitReader r{base, data, size};
    uint32_t chunk = r.Read(8);
    if (chunk == 255) return;
    uint32_t prev = 255, index = 0;
    bool shown = false;
    for (int guard = 0; guard < 200000; ++guard) {
        uint32_t run = r.Read(5);
        if (r.bad) return;
        if (run == 0 && prev == 0) {
            chunk = r.Read(8);
            shown = false;
            index = 0;
            if (chunk == 255) return;
            run = r.Read(5);
            if (r.bad) return;
        }
        if (shown && run) {
            auto& v = out[chunk];
            if (v.size() < index + run) v.resize(index + run, 0);
            for (uint32_t i = index; i < index + run; ++i) v[i] = 1;
        }
        index += run;
        prev = run;
        shown = !shown;
    }
}
void WriteRun(BitWriter& w, uint32_t run) {
    // Longer than 31: 31, then (0 = empty opposite run, more) until done.
    uint32_t first = run < 31 ? run : 31;
    w.Write(first, 5);
    run -= first;
    while (run) {
        const uint32_t part = run < 31 ? run : 31;
        w.Write(0, 5);
        w.Write(part, 5);
        run -= part;
    }
}
std::vector<uint8_t> Encode(const Visible& in) {
    BitWriter w;
    for (const auto& [chunk, flags] : in) {
        size_t last = flags.size();
        while (last && !flags[last - 1]) --last;
        if (!last) continue;
        w.Write(chunk, 8);
        size_t i = 0;
        while (i < last) {
            size_t hidden = 0;
            while (i < last && !flags[i]) ++hidden, ++i;
            WriteRun(w, uint32_t(hidden));  // may be 0 at the chunk start
            size_t shown = 0;
            while (i < last && flags[i]) ++shown, ++i;
            WriteRun(w, uint32_t(shown));  // always > 0
        }
        w.Write(0, 5);
        w.Write(0, 5);
    }
    w.Write(255, 8);
    w.bytes.push_back(0);
    w.bytes.push_back(0);
    return w.bytes;
}
constexpr uint32_t kMemSize = 512 * 1024;
uint32_t mem = 0;
int Radius() {
    static int radius = 4;
    static auto next = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    if (now >= next) {
        next = now + std::chrono::milliseconds(500);
        // Default 0: above the street PVS is fully off. The union (4) made far
        // buildings flicker as the set changed while moving.
        int v = 0;
        if (FILE* f = std::fopen("pvs_union_radius.txt", "rb")) {
            if (std::fscanf(f, "%d", &v) != 1) v = 0;
            std::fclose(f);
        }
        radius = v < 0 ? 0 : v > 20 ? 20 : v;
    }
    return radius;
}
uint8_t acc_mask[32] = {};
Visible acc_a, acc_b;
uint32_t acc_grid = 0;
bool acc_dirty = true;
void Reset() {
    std::memset(acc_mask, 0, sizeof(acc_mask));
    acc_a.clear();
    acc_b.clear();
    acc_grid = 0;
    acc_dirty = true;
}
// Returns the made-up cell's guest address, or 0 (draw everything).
uint32_t Build(uint8_t* base, uint32_t grid, int32_t ix, int32_t iy, int32_t iz, int32_t nx, int32_t ny,
               int32_t nz, int32_t count) {
    const int radius = Radius();
    if (radius <= 0) return 0;
    static uint32_t built_grid = 0;
    static int32_t built_x = -1, built_y = -1, built_z = -1, built_radius = -1;
    static bool built_ok = false;
    if (!acc_dirty && grid == built_grid && ix == built_x && iy == built_y && iz == built_z &&
        radius == built_radius)
        return built_ok ? mem : 0;
    if (radius != built_radius) Reset();
    acc_dirty = false;
    built_grid = grid; built_x = ix; built_y = iy; built_z = iz; built_radius = radius;
    built_ok = false;
    if (!mem) {
        mem = REX_KERNEL_STATE()->memory()->SystemHeapAlloc(kMemSize);
        if (!mem) return 0;
    }
    const uint32_t cells = Rd32(base, grid + 4);
    // Accumulated while on the roof (objects are only added, never removed:
    // a set that changes with every step made far buildings flicker).
    if (grid != acc_grid) Reset();
    acc_grid = grid;
    uint8_t (&mask)[32] = acc_mask;
    Visible& a = acc_a;
    Visible& b = acc_b;
    int used = 0;
    for (int32_t z = iz - radius; z <= iz + radius; ++z) {
        for (int32_t x = ix - radius; x <= ix + radius; ++x) {
            if (x < 0 || z < 0 || x >= nx || z >= nz) continue;
            const int32_t index = (z * ny + iy) * nx + x;
            if (index < 0 || index >= count) continue;
            const uint32_t cell = cells + uint32_t(index) * 48u;
            for (int k = 0; k < 32; ++k) mask[k] |= base[cell + k];
            Decode(base, Rd32(base, cell + 36), Rd32(base, cell + 32), a);
            Decode(base, Rd32(base, cell + 44), Rd32(base, cell + 40), b);
            ++used;
        }
    }
    if (!used) return 0;
    {
        // Self-check (first builds): the camera cell's streams decoded,
        // encoded and decoded again must match.
        static int checks = 0;
        const int32_t center = (iz * ny + iy) * nx + ix;
        if (checks < 5 && center < count) {
            ++checks;
            const uint32_t cell = cells + uint32_t(center) * 48u;
            bool same = true;
            for (int k = 0; k < 2; ++k) {
                Visible one, two;
                Decode(base, Rd32(base, cell + 36 + 8 * k), Rd32(base, cell + 32 + 8 * k), one);
                std::vector<uint8_t> enc = Encode(one);
                enc.insert(enc.begin(), 16, 0);
                Decode(enc.data(), 16, uint32_t(enc.size() - 16), two);
                for (auto& [c, v] : one) {
                    auto t = v;
                    while (!t.empty() && !t.back()) t.pop_back();
                    auto u = two[c];
                    while (!u.empty() && !u.back()) u.pop_back();
                    if (t != u) same = false;
                }
            }
            REXLOG_INFO("PVS union: stream round trip {}", same ? "ok" : "MISMATCH");
        }
    }
    const std::vector<uint8_t> sa = Encode(a), sb = Encode(b);
    const uint32_t stream_a = mem + 64, stream_b = (stream_a + uint32_t(sa.size()) + 15) & ~15u;
    if (stream_b + sb.size() > mem + kMemSize) return 0;
    std::memcpy(base + stream_a, sa.data(), sa.size());
    std::memcpy(base + stream_b, sb.data(), sb.size());
    std::memcpy(base + mem, mask, 32);
    Wr32(base, mem + 32, uint32_t(sa.size()));
    Wr32(base, mem + 36, stream_a);
    Wr32(base, mem + 40, uint32_t(sb.size()));
    Wr32(base, mem + 44, stream_b);
    built_ok = true;
    static int lines = 0;
    if (lines++ < 40) {
        size_t objects_a = 0, objects_b = 0;
        for (auto& [c, v] : a) for (uint8_t f : v) objects_a += f;
        for (auto& [c, v] : b) for (uint8_t f : v) objects_b += f;
        int chunks = 0;
        for (uint8_t m : mask) chunks += __builtin_popcount(m);
        REXLOG_INFO("PVS union: {} cells (radius {}) -> {} chunks, objects {} + {} ({} + {} bytes)", used, radius,
                    chunks, objects_a, objects_b, sa.size(), sb.size());
    }
    return mem;
}
uint32_t two_mem = 0;
// Street level: the union of the cells the camera and the player were in during
// the last 1.2 s. The PVS was computed for places people stand; a camera
// pushed behind an overpass or a wall lands in a cell that sees almost nothing
// (black ground), and a camera bobbing across a cell edge switched between two
// sets every few frames (groups of buildings flickering). Returns the made-up
// cell's address, or 0.
// How long a cell stays in the set: file pvs_sticky_ms.txt (default 0 = only
// the current camera + player cells; the 1.2 s version did not stop the
// flicker, which turned out to be chunk swaps).
int StickyMs() {
    static int v = -1;
    if (v < 0) {
        v = 0;
        if (FILE* f = std::fopen("pvs_sticky_ms.txt", "rb")) {
            if (std::fscanf(f, "%d", &v) != 1) v = 0;
            std::fclose(f);
        }
        if (v < 0) v = 0;
        if (v > 5000) v = 5000;
    }
    return v;
}
uint32_t TwoCells(uint8_t* base, uint32_t grid, int32_t a, int32_t b) {
    struct Recent { int32_t index; std::chrono::steady_clock::time_point seen; };
    static std::vector<Recent> recent;
    static uint32_t recent_grid = 0;
    static std::vector<int32_t> built;
    static bool built_ok = false;
    const auto now = std::chrono::steady_clock::now();
    if (grid != recent_grid) { recent.clear(); built.clear(); recent_grid = grid; }
    for (int32_t index : {a, b}) {
        bool found = false;
        for (auto& r : recent) if (r.index == index) { r.seen = now; found = true; }
        if (!found) recent.push_back({index, now});
    }
    recent.erase(std::remove_if(recent.begin(), recent.end(),
                                [&](const Recent& r) { return now - r.seen > std::chrono::milliseconds(StickyMs()); }),
                 recent.end());
    while (recent.size() > 8) recent.erase(recent.begin());
    std::vector<int32_t> want;
    for (const auto& r : recent) want.push_back(r.index);
    std::sort(want.begin(), want.end());
    if (want == built) return built_ok ? two_mem : 0;
    built = want;
    built_ok = false;
    if (!two_mem) {
        two_mem = REX_KERNEL_STATE()->memory()->SystemHeapAlloc(kMemSize);
        if (!two_mem) return 0;
    }
    const uint32_t cells = Rd32(base, grid + 4);
    uint8_t mask[32] = {};
    Visible va, vb;
    for (int32_t index : want) {
        const uint32_t cell = cells + uint32_t(index) * 48u;
        for (int k = 0; k < 32; ++k) mask[k] |= base[cell + k];
        Decode(base, Rd32(base, cell + 36), Rd32(base, cell + 32), va);
        Decode(base, Rd32(base, cell + 44), Rd32(base, cell + 40), vb);
    }
    const std::vector<uint8_t> sa = Encode(va), sb = Encode(vb);
    const uint32_t stream_a = two_mem + 64, stream_b = (stream_a + uint32_t(sa.size()) + 15) & ~15u;
    if (stream_b + sb.size() > two_mem + kMemSize) return 0;
    std::memcpy(base + stream_a, sa.data(), sa.size());
    std::memcpy(base + stream_b, sb.data(), sb.size());
    std::memcpy(base + two_mem, mask, 32);
    Wr32(base, two_mem + 32, uint32_t(sa.size()));
    Wr32(base, two_mem + 36, stream_a);
    Wr32(base, two_mem + 40, uint32_t(sb.size()));
    Wr32(base, two_mem + 44, stream_b);
    built_ok = true;
    AddEvent(fmt::format("PVS street set: {} cells", want.size()));
    return two_mem;
}
}  // namespace pvs_union

// Small far objects. The PVS object draw (825C29E8, or the batched 825C2F38)
// gets (chunk slot r3, object r4); the object record is chunk
// (0x829A97F8 + slot*4) +216 -> records of 80 bytes: +0 box min xyz, +16 box
// max xyz. An object whose size / distance to the camera is below
// detail_cull.txt (0 = off) is not drawn. Camera position 0x827D9778 +44.
namespace detail_cull {
float Threshold() {
    static float value = 0.0f;
    static auto next = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    if (now >= next) {
        next = now + std::chrono::milliseconds(500);
        float v = 0.0f;
        if (FILE* f = std::fopen("detail_cull.txt", "rb")) {
            if (std::fscanf(f, "%f", &v) != 1) v = 0.0f;
            std::fclose(f);
        }
        value = v < 0.0f ? 0.0f : v;
    }
    return value;
}
uint64_t drawn = 0, culled = 0;
bool Skip(uint8_t* base, uint32_t slot, uint32_t object) {
    const float k = Threshold();
    if (k <= 0.0f || slot > 255) return false;
    const uint32_t chunk = Rd32(base, 0x829A97F8 + slot * 4);
    if (!chunk) return false;
    const uint32_t records = Rd32(base, chunk + 216);
    if (!records) return false;
    const uint32_t r = records + object * 80;
    const float x0 = RdF(base, r + 0), y0 = RdF(base, r + 4), z0 = RdF(base, r + 8);
    const float x1 = RdF(base, r + 16), y1 = RdF(base, r + 20), z1 = RdF(base, r + 24);
    const float sx = x1 - x0, sy = y1 - y0, sz = z1 - z0;
    const float size2 = sx * sx + sy * sy + sz * sz;
    const float cx = RdF(base, 0x827D9778 + 44), cy = RdF(base, 0x827D9778 + 48), cz = RdF(base, 0x827D9778 + 52);
    const float dx = (x0 + x1) * 0.5f - cx, dy = (y0 + y1) * 0.5f - cy, dz = (z0 + z1) * 0.5f - cz;
    const float dist2 = dx * dx + dy * dy + dz * dz;
    const bool skip = std::isfinite(size2) && std::isfinite(dist2) && size2 < dist2 * k * k;
    ++(skip ? culled : drawn);
    static auto next = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    const auto now = std::chrono::steady_clock::now();
    if (now >= next) {
        next = now + std::chrono::seconds(5);
        static int lines = 0;
        if (lines++ < 200)
            REXLOG_INFO("Detail cull {:.3f}: {} objects drawn, {} skipped (5 s)", k, drawn, culled);
        drawn = culled = 0;
    }
    return skip;
}
}  // namespace detail_cull

// 4. No flicker at the fade distance. Every city object is drawn only while
//    the camera is nearer than its fade distance (82129550: object record
//    +28 = squared fade distance, compared with the camera distance x 0.77
//    from 821287B0). From the air lots of buildings sit right at that
//    distance, and the camera's small movements (flying, bobbing, shake) move
//    them back and forth across it: they flickered in and out. An object that
//    was drawn a moment ago now counts as within range up to fade_hysteresis
//    (file fade_hysteresis.txt, default 1.15 = 15 % further; 1 = off) of its
//    distance, so it only goes once the camera is clearly past it. The record
//    keeps its own value (remembered per record, written back as it was).
//    Object record: chunk (0x829A97F8 + slot*4) +216 -> 80 bytes per object.
namespace far_clip {
extern bool g_air_view;
}
namespace fade_hysteresis {
// Air view ground boost: flat, wide objects (streets, roofs, water, park
// ground: box under 6 m tall, over 15 m across) get their fade distance
// multiplied by ground_fade_boost.txt (default 4, 1 = off) while the camera is
// high; from up there they faded out while their area's low-detail copy was
// already hidden (black ground). Counted for the AIRFLICKER log.
float GroundBoost() {
    static float value = 4.0f;
    static auto next = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    if (now >= next) {
        next = now + std::chrono::milliseconds(1000);
        float v = 4.0f;
        if (FILE* f = std::fopen("ground_fade_boost.txt", "rb")) {
            if (std::fscanf(f, "%f", &v) != 1) v = 4.0f;
            std::fclose(f);
        }
        value = (v >= 1.0f && v <= 20.0f) ? v : 4.0f;
    }
    return value;
}
// Air view: every object of a low-detail "al" chunk (the far city) gets its
// fade distance multiplied by al_fade_boost.txt (default 3, 1 = off). From
// up high the far ground and trees showed but its buildings had faded out
// (their distances are tuned for street level). Log "AL FADE" every 10 s.
float AlBoost() {
    static float value = 3.0f;
    static auto next = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    if (now >= next) {
        next = now + std::chrono::milliseconds(1000);
        float v = 3.0f;
        if (FILE* f = std::fopen("al_fade_boost.txt", "rb")) {
            if (std::fscanf(f, "%f", &v) != 1) v = 3.0f;
            std::fclose(f);
        }
        value = (v >= 1.0f && v <= 20.0f) ? v : 3.0f;
    }
    return value;
}
std::atomic<uint32_t> g_al_tests{0}, g_al_drawn{0}, g_al_fade_sum{0};
std::atomic<uint64_t> g_ground_forced{0};
// Why the fade test (82129550) rejected an object within 80 m of the camera in air view (AIRFLICKER log):
// 0 render hidden flag, 1 object flags 0x40388020, 2 needs toggle 0x8370DD00, 3 needs cell record byte,
// 4 LOD-set gate (0x83710560), 5 gate byte[6] + 0x00800000, 6 decal path (0x200), 7 beyond fade distance,
// 8 already counted this frame (dedupe), 9 other (fade in/out, allocation)
std::atomic<uint32_t> g_reject[10] = {};
std::atomic<uint32_t> g_near_tests{0}, g_near_drawn{0};
// 17. Air-view detail budget: the main render list holds 2500 objects (section
// 16); from the air more pass, and whatever doesn't fit is dropped - a
// different set each frame (far flicker). Objects whose size / distance is
// below g_budget_k are skipped before they are queued; k adapts per frame:
// up when the list was full, down when it had room (< 2100). Big and near
// objects are never skipped. File "air_budget_off" = off.
std::atomic<float> g_budget_k{0.0f};
std::atomic<uint32_t> g_budget_skipped{0};
// Fade-in restarts: 82129550 stores the frame counter (halfword 0x8370D0E0) in
// the object's +52 and restarts its fade-in from alpha 0 (shared render data
// +84) when the old stamp isn't counter - 1. Counted for every test: same frame
// / previous frame / gap; "fixed" = a recently drawn object whose stamp was
// set to counter - 1 before the test (file "fade_restart_original" = off).
std::atomic<uint32_t> g_st_same{0}, g_st_next{0}, g_st_gap{0}, g_st_fixed{0}, g_counter_steps{0};
// reason 1 split: [al chunk?][bit index of 0x40388020's 6 bits]
std::atomic<uint32_t> g_flagbit[2][6] = {};
constexpr uint32_t kFlagBits[6] = {0x40000000, 0x00200000, 0x00100000, 0x00080000, 0x00008000, 0x00000020};
std::atomic<uint32_t> g_flat_tests{0}, g_flat_drawn{0};
float Factor() {
    static float value = 1.15f;
    static auto next = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    if (now >= next) {
        next = now + std::chrono::milliseconds(1000);
        float v = 1.15f;
        if (FILE* f = std::fopen("fade_hysteresis.txt", "rb")) {
            if (std::fscanf(f, "%f", &v) != 1) v = 1.15f;
            std::fclose(f);
        }
        value = (v >= 1.0f && v <= 3.0f) ? v : 1.15f;
    }
    return value;
}
struct Entry {
    std::atomic<uint32_t> rec{0};
    std::atomic<uint32_t> fade_bits{0};    // the record's own squared fade distance
    std::atomic<uint32_t> drawn_ms{0};     // last time it was drawn (steady ms, never 0 once drawn)
    std::atomic<uint32_t> tested_ms{0};    // last time it was tested at all (F9 object dump)
    std::atomic<uint8_t> last{2};          // last fade test result (flicker diagnostic; 2 = none yet)
    std::atomic<uint8_t> last_c[4] = {2, 2, 2, 2};  // per caller
    std::atomic<uint32_t> change_ms[4] = {0, 0, 0, 0};
};
// Flicker diagnostic (air_flicker below): fade tests and in/out changes, per
// caller (LR); "rapid" = the same object changed again within 100 ms.
std::atomic<uint32_t> g_fade_tests{0}, g_fade_flips{0};
std::atomic<uint32_t> g_caller_lr[4] = {0, 0, 0, 0};
std::atomic<uint32_t> g_c_tests[4] = {0, 0, 0, 0}, g_c_flips[4] = {0, 0, 0, 0}, g_c_rapid[4] = {0, 0, 0, 0};
int CallerIndex(uint32_t lr) {
    for (int i = 0; i < 4; ++i) {
        uint32_t cur = g_caller_lr[i].load(std::memory_order_relaxed);
        if (cur == lr) return i;
        if (cur == 0 && g_caller_lr[i].compare_exchange_strong(cur, lr)) return i;
        if (cur == lr) return i;
    }
    return -1;
}
constexpr uint32_t kSlots = 1u << 16;
Entry table[kSlots];
inline uint32_t Hash(uint32_t r) { return (r * 2654435761u) >> 16; }
inline uint32_t NowMs() {
    // 2026-10-02: GetTickCount64 (shared-page read) instead of QPC - called per fade test (~1 % of busy CPU).
    return uint32_t(GetTickCount64()) | 1u;
}
}  // namespace fade_hysteresis

extern "C" void __imp__sub_82129550(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_82129550) {
    using namespace fade_hysteresis;
    const uint32_t caller_lr = uint32_t(ctx.lr);
    const float k = Factor();
    const uint32_t slot = ctx.r3.u32, object = ctx.r4.u32;
    uint32_t rec = 0, chunk = 0;
    if (k > 1.0f && slot < 256) {
        chunk = Rd32(base, 0x829A97F8 + slot * 4);
        const uint32_t records = chunk ? Rd32(base, chunk + 216) : 0;
        if (records) rec = records + object * 80;
    }
    if (!rec) {
        __imp__sub_82129550(ctx, base);
        return;
    }
    Entry& e = table[Hash(rec) & (kSlots - 1)];
    const uint32_t cur = Rd32(base, rec + 28);
    uint32_t own = cur;
    bool was = false;
    const uint32_t now = NowMs();
    if (e.rec.load(std::memory_order_relaxed) == rec) {
        const uint32_t stored = e.fade_bits.load(std::memory_order_relaxed);
        float sf, cf;
        std::memcpy(&sf, &stored, 4);
        std::memcpy(&cf, &cur, 4);
        // The record still holds its own value or our widened one: same object.
        if (cur == stored || std::fabs(cf - sf * k * k) <= sf * 1e-4f + 1e-3f) {
            own = stored;
            was = now - e.drawn_ms.load(std::memory_order_relaxed) < 250u;
        } else {
            e.fade_bits.store(cur, std::memory_order_relaxed);  // reloaded chunk / new object here
            e.drawn_ms.store(0, std::memory_order_relaxed);
        }
    } else {
        e.rec.store(rec, std::memory_order_relaxed);
        e.fade_bits.store(cur, std::memory_order_relaxed);
        e.drawn_ms.store(0, std::memory_order_relaxed);
    }
    float of;
    std::memcpy(&of, &own, 4);
    // Flat, wide object while the camera is high?
    float gb = 1.0f;
    bool flat = false;
    if (far_clip::g_air_view) {
        auto f = [&](uint32_t a) { return RdF(base, rec + a); };
        const float sx = f(16) - f(0), sy = f(20) - f(4), sz = f(24) - f(8);
        flat = std::isfinite(sx) && std::isfinite(sy) && std::isfinite(sz) && sy >= 0.0f && sy < 6.0f &&
               std::max(sx, sz) > 15.0f && sx < 5000.0f && sz < 5000.0f;
        if (flat) gb = GroundBoost();
    }
    const bool al = far_clip::g_air_view && chunk && (base[chunk + 20] & 0x80);
    if (al) gb = std::max(gb, AlBoost());
    const float kk = std::max(was ? k : 1.0f, gb);
    // Air view: big objects are judged by the distance to their MIDDLE; a
    // terrain piece 300 m across can have its middle far away while its edge
    // is right under the camera (black ground next to drawn buildings). Its
    // fade distance is lengthened by its box radius, so it counts from its
    // nearest edge instead. File "box_radius_off" = off.
    static const bool box_radius_off = FileExists("box_radius_off");
    float radius = 0.0f;
    if (far_clip::g_air_view && !box_radius_off) {
        auto f = [&](uint32_t a) { return RdF(base, rec + a); };
        const float sx = f(16) - f(0), sy = f(20) - f(4), sz = f(24) - f(8);
        const float r = 0.5f * std::sqrt(sx * sx + sy * sy + sz * sz);
        if (std::isfinite(r) && r > 20.0f && r < 3000.0f) radius = r;
    }
    if ((was || gb > 1.0f || radius > 0.0f) && std::isfinite(of) && of > 0.0f) {
        float wide = of * kk * kk;
        if (radius > 0.0f) {
            const float grown = std::sqrt(of) + radius;
            wide = std::max(wide, grown * grown);
        }
        uint32_t wb;
        std::memcpy(&wb, &wide, 4);
        Wr32(base, rec + 28, wb);
    }
    {
        static const bool fade_restart_original = FileExists("fade_restart_original");
        const uint32_t cur = (uint32_t(base[0x8370D0E0]) << 8) | base[0x8370D0E1];
        static std::atomic<uint32_t> last_cur{0};
        const uint32_t lc = last_cur.exchange(cur, std::memory_order_relaxed);
        if (lc != cur) g_counter_steps.fetch_add((cur - lc) & 0xFFFF, std::memory_order_relaxed);
        const uint32_t old = (uint32_t(base[rec + 52]) << 8) | base[rec + 53];
        const uint32_t prev = (cur - 1) & 0xFFFF;
        if (old == cur) g_st_same.fetch_add(1, std::memory_order_relaxed);
        else if (old == prev) g_st_next.fetch_add(1, std::memory_order_relaxed);
        else g_st_gap.fetch_add(1, std::memory_order_relaxed);
        if (!fade_restart_original && old != prev) {
            const uint32_t dms = e.drawn_ms.load(std::memory_order_relaxed);
            if (dms && now - dms < 300u) {
                base[rec + 52] = uint8_t(prev >> 8);
                base[rec + 53] = uint8_t(prev);
                g_st_fixed.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    {
        static const bool budget_off = FileExists("air_budget_off");
        const float k = g_budget_k.load(std::memory_order_relaxed);
        if (!budget_off && far_clip::g_air_view && k > 0.0f) {
            auto f = [&](uint32_t a) { return RdF(base, rec + a); };
            const float sx = f(16) - f(0), sy = f(20) - f(4), sz = f(24) - f(8);
            const float r = 0.5f * std::sqrt(sx * sx + sy * sy + sz * sz);
            const float mx = 0.5f * (f(0) + f(16)), my = 0.5f * (f(4) + f(20)), mz = 0.5f * (f(8) + f(24));
            const float cx = RdF(base, 0x827D9778 + 44), cy = RdF(base, 0x827D9778 + 48), cz = RdF(base, 0x827D9778 + 52);
            const float d = std::sqrt((mx - cx) * (mx - cx) + (my - cy) * (my - cy) + (mz - cz) * (mz - cz));
            // Sticky: an object drawn in the last 300 ms must be clearly below the cut (80 %) to go.
            const uint32_t dms = e.drawn_ms.load(std::memory_order_relaxed);
            const float kk_cut = (dms && now - dms < 300u) ? k * 0.8f : k;
            // Ground (flat: under 4 m tall, over 3 m across - garden patches, pavements) is never cut:
            // without it the spot shows black.
            const bool ground = sy < 4.0f && std::max(sx, sz) > 3.0f;
            if (!ground && std::isfinite(r) && d > 60.0f && r < 40.0f && r < kk_cut * d) {
                g_budget_skipped.fetch_add(1, std::memory_order_relaxed);
                Wr32(base, rec + 28, own);  // undo the widened fade distance written above
                ctx.r3.u64 = 0;
                return;
            }
        }
    }
    // Pre-state for the rejection reasons (same reads as the original's early checks).
    int reason = -1;
    float near_d = 1e9f;
    uint32_t counter_addr = 0;
    if (far_clip::g_air_view && AirDiag()) {
        auto f = [&](uint32_t a) { return RdF(base, rec + a); };
        const float mx = 0.5f * (f(0) + f(16)), my = 0.5f * (f(4) + f(20)), mz = 0.5f * (f(8) + f(24));
        const float cx = RdF(base, 0x827D9778 + 44), cy = RdF(base, 0x827D9778 + 48), cz = RdF(base, 0x827D9778 + 52);
        near_d = std::sqrt((mx - cx) * (mx - cx) + (my - cy) * (my - cy) + (mz - cz) * (mz - cz));
        if (near_d < 80.0f) {
            const uint32_t flags = Rd32(base, rec + 32);
            const uint32_t r68 = Rd32(base, rec + 68);
            const uint32_t rd = (r68 && (flags & 0x00010000)) ? r68 : Rd32(base, rec + 60);
            const uint32_t r108 = rd ? Rd32(base, rd + 108) : 0;
            const uint32_t r5b = ctx.r5.u32 & 0xFF, r6 = ctx.r6.u32;
            const uint8_t* g = base + 0x83710560;
            const uint32_t cellrec = chunk ? Rd32(base, chunk + 232) : 0;
            if ((r108 & 0x8000) && base[0x8370D991]) reason = 0;
            else if (flags & 0x40388020) reason = 1;
            else if ((r108 & 0x4000) && !base[0x8370DD00]) reason = 2;
            else if (!r5b && !r6 && (r108 & 0x1000) && cellrec && !base[cellrec + object * 24 + 20]) reason = 3;
            else if (!((g[0] && (flags & 0x08000000)) || (g[1] && (flags & 0x10000000)))) reason = 4;
            else if (g[6] && (flags & 0x00800000)) reason = 5;
            else if (flags & 0x200) reason = 6;
            if (rd) counter_addr = rd + (r6 == 1 ? 12 : r6 == 2 ? 16 : 8);
        }
    }
    __imp__sub_82129550(ctx, base);
    if (near_d < 80.0f) {
        g_near_tests.fetch_add(1, std::memory_order_relaxed);
        if (ctx.r3.u32 & 0xFF) g_near_drawn.fetch_add(1, std::memory_order_relaxed);
        else {
            if (reason < 0) {
                float fw;
                const uint32_t wv = Rd32(base, rec + 28);
                std::memcpy(&fw, &wv, 4);
                if (near_d * near_d > fw) reason = 7;
                else if (counter_addr && Rd32(base, counter_addr) > 1) reason = 8;
                else reason = 9;
            }
            g_reject[reason].fetch_add(1, std::memory_order_relaxed);
            if (reason == 1) {
                const uint32_t fl = Rd32(base, rec + 32);
                const int al_i = (chunk && (base[chunk + 20] & 0x80)) ? 1 : 0;
                for (int b = 0; b < 6; ++b)
                    if (fl & kFlagBits[b]) g_flagbit[al_i][b].fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    Wr32(base, rec + 28, own);
    // Ground under the player never faded out: a flat object (< 6 m tall, >
    // 15 m wide) whose box (x/z, 30 m margin) holds the camera or the player
    // passes the test. Big ground pieces are judged by the distance to their
    // middle, which can be far away while you stand on them (black ground
    // with trees on it). File "ground_force_off" = off.
    static const bool ground_force_off = FileExists("ground_force_off");
    if (!(ctx.r3.u32 & 0xFF) && !ground_force_off) {
        auto f = [&](uint32_t a) { return RdF(base, rec + a); };
        const float x0 = f(0), y0 = f(4), z0 = f(8), x1 = f(16), y1 = f(20), z1 = f(24);
        const float sx = x1 - x0, sy = y1 - y0, sz = z1 - z0;
        if (std::isfinite(sx) && std::isfinite(sz) && sy >= 0.0f && sy < 6.0f && std::max(sx, sz) > 15.0f &&
            sx < 5000.0f && sz < 5000.0f) {
            const float m = 30.0f;
            auto inside = [&](float px, float pz) {
                return px > x0 - m && px < x1 + m && pz > z0 - m && pz < z1 + m;
            };
            const float cx = RdF(base, 0x827D9778 + 44), cz = RdF(base, 0x827D9778 + 52);
            bool near = inside(cx, cz);
            if (!near) {
                const uint32_t player = Rd32(base, 0x8309ABEC);
                if (player > 0x10000 && player < 0xF0000000)
                    near = inside(RdF(base, player + 20), RdF(base, player + 28));
            }
            if (near) {
                ctx.r3.u64 = 1;
                g_ground_forced.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    const uint8_t result = (ctx.r3.u32 & 0xFF) ? 1 : 0;
    if (al) {
        g_al_tests.fetch_add(1, std::memory_order_relaxed);
        if (result) g_al_drawn.fetch_add(1, std::memory_order_relaxed);
        if (std::isfinite(of) && of > 0.0f) g_al_fade_sum.fetch_add(uint32_t(std::sqrt(of)), std::memory_order_relaxed);
        static std::atomic<int64_t> next_log{0};
        const int64_t t = now;
        int64_t n = next_log.load(std::memory_order_relaxed);
        const int64_t n0 = n;
        if (t - n >= 10000 && next_log.compare_exchange_strong(n, t)) {
            n = n0;
            const uint32_t tests = g_al_tests.exchange(0), drawn = g_al_drawn.exchange(0), sum = g_al_fade_sum.exchange(0);
            static int lines = 0;
            if (n && lines++ < 200)
                REXLOG_INFO("AL FADE (air view, x{:.1f}): {} far-city objects tested, {} drawn, their own fade distance "
                            "avg {:.0f} m", AlBoost(), tests, drawn, tests ? double(sum) / tests : 0.0);
        }
    }
    if (flat) {
        g_flat_tests.fetch_add(1, std::memory_order_relaxed);
        if (result) g_flat_drawn.fetch_add(1, std::memory_order_relaxed);
    }
    if (result) e.drawn_ms.store(now, std::memory_order_relaxed);
    e.tested_ms.store(now, std::memory_order_relaxed);
    g_fade_tests.fetch_add(1, std::memory_order_relaxed);
    if (e.last.exchange(result, std::memory_order_relaxed) == (result ^ 1))
        g_fade_flips.fetch_add(1, std::memory_order_relaxed);
    const int ci = CallerIndex(caller_lr);
    if (ci >= 0) {
        g_c_tests[ci].fetch_add(1, std::memory_order_relaxed);
        if (e.last_c[ci].exchange(result, std::memory_order_relaxed) == (result ^ 1)) {
            g_c_flips[ci].fetch_add(1, std::memory_order_relaxed);
            const uint32_t prev = e.change_ms[ci].exchange(now, std::memory_order_relaxed);
            if (prev && now - prev < 100u) g_c_rapid[ci].fetch_add(1, std::memory_order_relaxed);
        }
    }
}

extern "C" void __imp__sub_825C29E8(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_825C29E8) {
    if (detail_cull::Skip(base, ctx.r3.u32, ctx.r4.u32)) return;
    __imp__sub_825C29E8(ctx, base);
}
extern "C" void __imp__sub_825C2F38(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_825C2F38) {
    if (detail_cull::Skip(base, ctx.r3.u32, ctx.r4.u32)) return;
    __imp__sub_825C2F38(ctx, base);
}

extern "C" void __imp__sub_825C1B18(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_825C1B18) {
    static const bool original = FileExists("pvs_original");
    if (original || !AirViewEnabled()) {
        __imp__sub_825C1B18(ctx, base);
        return;
    }
    ctx.r3.u64 = 0;
    const uint32_t grid = Rd32(base, kPvsGrid);
    if (!grid) return;
    const int32_t count = int32_t(Rd32(base, grid + 0));
    const int32_t locked = int32_t(Rd32(base, kPvsLockedCell));
    int32_t index;
    if (locked < count && locked >= 0) {
        index = locked;
    } else {
        const uint32_t pos = ctx.r4.u32;
        const float fx = (RdF(base, pos + 0) - RdF(base, grid + 20)) / RdF(base, grid + 44);
        const float fy = (RdF(base, pos + 4) - RdF(base, grid + 24)) / RdF(base, grid + 48);
        const float fz = (RdF(base, pos + 8) - RdF(base, grid + 28)) / RdF(base, grid + 52);
        const int32_t nx = int32_t(Rd32(base, grid + 8));
        const int32_t ny = int32_t(Rd32(base, grid + 12));
        const int32_t nz = int32_t(Rd32(base, grid + 16));
        const int32_t ix = int32_t(std::floor(fx));
        const int32_t iz = int32_t(std::floor(fz));
        int32_t iy = int32_t(std::floor(fy));
        if (iy < 0) iy = 0;
        if (iy > ny - 1) iy = ny - 1;
        const int32_t camera_layer = iy;
        if (iy > 0) --iy;  // the layer below: see the note at the top
        if (ix < 0 || ix >= nx || iz < 0 || iz >= nz) return;
        // Camera well above the street: no cell (see the note at the top).
        static const float pvs_height = [] {
            float v = 25.0f;
            if (FILE* f = std::fopen("pvs_height.txt", "rb")) {
                if (std::fscanf(f, "%f", &v) != 1) v = 25.0f;
                std::fclose(f);
            }
            return v;
        }();
        const float above_floor = RdF(base, pos + 4) - RdF(base, grid + 24);
        // Street level from street_points.txt (the game's own navpoints and
        // spawns): the camera's height is compared with the nearest street
        // point, so hills and raised streets keep the visibility data. Only
        // without a street point within 60 m the grid floor rule is used.
        float street = 0.0f;
        const bool have_street = StickyStreetHeight(RdF(base, pos + 0), RdF(base, pos + 4), RdF(base, pos + 8), &street);
        const float above = have_street ? RdF(base, pos + 4) - street : above_floor;
        const float limit = have_street ? kStreetLimit : pvs_height;
        static bool was_high = false, high_state = false;
        static auto high_since = std::chrono::steady_clock::time_point{};
        const bool climbing = ClimbingFast(RdF(base, pos + 4));
        const bool high = Debounced(above > (high_state ? limit - 6.0f : (climbing ? 5.0f : limit)), high_state,
                                    high_since, 3000);
        if (high != was_high) {
            static int lines = 0;
            if (lines++ < 60) {
                REXLOG_INFO("PVS: camera {:.0f} m above the {} -> visibility data {}", above,
                            have_street ? "street" : "grid floor", high ? "off (draw everything in range)" : "on");
            }
            was_high = high;
        }
        if (!high) pvs_union::Reset();
        if (high) {
            // Rooftop: the union of the nearby cells (or draw everything).
            const uint32_t cell = pvs_union::Build(base, grid, ix, iy, iz, nx, ny, nz, count);
            if (cell) {
                const int32_t center = (iz * ny + iy) * nx + ix;
                if (center < count) Wr32(base, kPvsCellIndex, uint32_t(center));
                ctx.r3.u64 = cell;
            }
            return;
        }
        index = (iz * ny + iy) * nx + ix;
        static int32_t logged_layer = -1;
        static uint32_t logged_grid = 0;
        if (camera_layer != logged_layer || grid != logged_grid) {
            static int lines = 0;
            if (lines++ < 40) {
                REXLOG_INFO("PVS: camera in layer {} of {} (grid {}x{}x{}, cell size {:.0f}x{:.0f}x{:.0f}, "
                            "origin y {:.0f}), using layer {}",
                            camera_layer, ny, nx, ny, nz, RdF(base, grid + 44), RdF(base, grid + 48),
                            RdF(base, grid + 52), RdF(base, grid + 24), iy);
            }
            logged_layer = camera_layer;
            logged_grid = grid;
        }
        if (index >= count) return;
        // The player's cell too (see pvs_union::TwoCells). Player object
        // [0x8309ABEC], +72 == 1 = valid, +20 position. File "pvs_camera_only" = off.
        static const bool camera_only = FileExists("pvs_camera_only");
        const uint32_t player = Rd32(base, 0x8309ABEC);
        if (!camera_only && player > 0x10000 && player < 0xF0000000 && Rd32(base, player + 72) == 1) {
            const float px = RdF(base, player + 20), py = RdF(base, player + 24), pz = RdF(base, player + 28);
            const int32_t jx = int32_t(std::floor((px - RdF(base, grid + 20)) / RdF(base, grid + 44)));
            const int32_t jz = int32_t(std::floor((pz - RdF(base, grid + 28)) / RdF(base, grid + 52)));
            int32_t jy = int32_t(std::floor((py - RdF(base, grid + 24)) / RdF(base, grid + 48)));
            if (jy < 0) jy = 0;
            if (jy > ny - 1) jy = ny - 1;
            if (jy > 0) --jy;
            if (std::isfinite(px) && std::isfinite(pz) && jx >= 0 && jx < nx && jz >= 0 && jz < nz) {
                const int32_t pindex = (jz * ny + jy) * nx + jx;
                if (pindex < count) {
                    const uint32_t cell = pvs_union::TwoCells(base, grid, index, pindex);
                    if (cell) {
                        Wr32(base, kPvsCellIndex, uint32_t(index));
                        ctx.r3.u64 = cell;
                        return;
                    }
                }
            }
        }
    }
    Wr32(base, kPvsCellIndex, uint32_t(index));
    ctx.r3.u64 = Rd32(base, grid + 4) + uint32_t(index) * 48u;
}

// 3. Far clip regions from the air. Designers placed "far clip regions"
//    (City_far_clip_regions_enabled 0x8370D093): 82121F60(r3 camera position,
//    r4 camera) finds the region the camera is in (world +272 -> +172) and,
//    by the compass direction the camera looks, a far clip distance; it
//    stores "in region" at 0x8370D0AA and the distance (float split in two
//    halfwords) at 0x8370D0B0/B2. 82122300 caps every city object's fade
//    distance with it (82129550), movers (823F8358) and 82174710 read it too.
//    On the street those caps hide chunk seams nobody can see. From the air
//    the detailed chunk is cut off at the cap while its low-detail "al" copy
//    is already switched off (82128848 hides al as soon as the chunk is
//    loaded): black holes at mid distance. With the camera more than
//    far_clip_height meters above the street (file far_clip_height.txt,
//    default 11 = the rooftop PVS rule) the region is cleared after the
//    lookup. File "far_clip_original" = original behaviour.
//    (Tried first and removed: measuring fade distances from a lower camera
//    height doubled the draws and did not fill the holes.)
namespace far_clip {
float g_air_view_height = 0.0f;
bool g_air_view = false;
float Limit() {
    static float value = 11.0f;
    static auto next = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    if (now >= next) {
        next = now + std::chrono::milliseconds(1000);
        float v = 11.0f;
        if (FILE* f = std::fopen("far_clip_height.txt", "rb")) {
            if (std::fscanf(f, "%f", &v) != 1) v = 11.0f;
            std::fclose(f);
        }
        value = v;
    }
    return value;
}
bool High(uint8_t* base, float x, float y, float z) {
    static float cx = 1e9f, cy = 1e9f, cz = 1e9f;
    static bool high = false;
    if (std::fabs(cx - x) < 1.0f && std::fabs(cz - z) < 1.0f && std::fabs(cy - y) < 0.5f) return high;
    cx = x; cy = y; cz = z;
    float street = 0.0f;
    bool have = StickyStreetHeight(x, y, z, &street);
    if (!have) {
        const uint32_t grid = Rd32(base, kPvsGrid);
        if (grid) {
            street = RdF(base, grid + 24) + 14.0f;
            have = std::isfinite(street);
        }
    }
    if (!have) { street = 0.0f; have = true; }  // Stilwater's streets are around y 0
    const float limit = Limit();
    static bool state = false;
    static auto since = std::chrono::steady_clock::time_point{};
    const bool climbing = ClimbingFast(y);
    const bool now_high =
        Debounced(have && (y - street) > (state ? limit - 6.0f : (climbing ? 5.0f : limit)), state, since, 3000);
    g_air_view_height = y - street;
    if (now_high != high) {
        static int lines = 0;
        if (lines++ < 60)
            REXLOG_INFO("Far clip regions: camera {:.0f} m above the street -> {}", y - street,
                        now_high ? "off (air view)" : "on");
        AddEvent(now_high ? "air view on" : "air view off");
        high = now_high;
    }
    g_air_view = high;
    return high;
}
}  // namespace far_clip

// 5. Flicker diagnostic from the air (air view on): every second, how often
//    things changed that can make a building blink. 82128848 (every frame,
//    from several callers = passes) marks every al copy drawn in the table
//    0x829ABE10 (256 x 8 B, byte 0), then per loaded detailed chunk (list
//    0x829A97E8: +0 count, +16 pointers; chunk +8 id, +9 al id, +16 name) hides
//    either the chunk's own slot (flag +20 bit 0x20, or the PVS test) or its al
//    copy. Counted per caller against that caller's previous call: own slot
//    flips, al slot flips, chunks entering / leaving the list; plus fade test
//    in/out changes (82129550 above). Log "AIRFLICKER ..." (max 150 lines).
namespace air_flicker {
struct CallerState {
    uint32_t lr = 0;
    uint8_t own[256];
    uint8_t al[256];
    bool have = false;
};
CallerState callers[8];
uint32_t own_flips[256], al_flips[256];
uint32_t prev_list[64];
uint32_t prev_count = 0;
std::string added, removed;
uint32_t calls = 0;
std::string ChunkName(uint8_t* base, uint32_t c) {
    const uint32_t name_ptr = Rd32(base, c + 16);
    std::string s;
    if (name_ptr > 0x10000 && name_ptr < 0xF0000000) {
        for (int k = 0; k < 39; ++k) {
            const char ch = char(base[name_ptr + k]);
            if (!ch || ch < 32 || ch > 126) break;
            s += ch;
        }
    }
    return s;
}
}  // namespace air_flicker

// 6. Delayed swap from the air. A detailed chunk enters the list with flag +20
//    bit 0x20 (not ready: its own slot hidden, its al drawn); ~0.1-0.3 s later
//    0x18 -> 0x10 and the game swaps (own drawn, al hidden). From the air the
//    swapped-in chunk's surfaces were black for ~1 s (user clip 06:04: textures
//    not there yet). While the air view is on, the al copy stays drawn and the
//    chunk hidden for al_swap_delay_ms (file, default 1200, 0 = off) after the
//    chunk first becomes ready.
namespace swap_delay {
int DelayMs() {
    static int value = 1200;
    static auto next = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    if (now >= next) {
        next = now + std::chrono::milliseconds(1000);
        int v = 1200;
        if (FILE* f = std::fopen("al_swap_delay_ms.txt", "rb")) {
            if (std::fscanf(f, "%d", &v) != 1) v = 1200;
            std::fclose(f);
        }
        value = std::clamp(v, 0, 10000);
    }
    return value;
}
uint32_t chunk_ptr[256];
std::chrono::steady_clock::time_point ready_at[256];
bool ready_seen[256];
void Apply(uint8_t* base) {
    const int delay = DelayMs();
    const auto now = std::chrono::steady_clock::now();
    const uint32_t count = std::min<uint32_t>(Rd32(base, 0x829A97E8), 64);
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t c = Rd32(base, 0x829A97E8 + 16 + i * 4);
        if (!c) continue;
        const uint8_t id = base[c + 8], al = base[c + 9];
        if (chunk_ptr[id] != c) {
            chunk_ptr[id] = c;
            ready_seen[id] = false;
            // Chunks that were already in the list before the air view: no delay.
            if (!(base[c + 20] & 0x20)) {
                ready_seen[id] = true;
                ready_at[id] = now - std::chrono::seconds(60);
            }
        }
        if (al == 255 || delay <= 0) continue;
        // Ready = the "not ready" bit 0x20 is gone (0x08 is the one-frame
        // step before 0x10; the swap already happens there).
        const bool ready = !(base[c + 20] & 0x20);
        if (ready && !ready_seen[id]) {
            ready_seen[id] = true;
            ready_at[id] = now;
        }
        if (!ready_seen[id]) continue;
        if (now - ready_at[id] < std::chrono::milliseconds(delay) && base[0x829ABE10 + id * 8]) {
            base[0x829ABE10 + id * 8] = 0;  // detailed chunk: not yet
            base[0x829ABE10 + al * 8] = 1;  // its low-detail copy stays
        }
    }
}
}  // namespace swap_delay

extern "C" void __imp__sub_82128848(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_82128848) {
    using namespace air_flicker;
    const uint32_t lr = uint32_t(ctx.lr);
    __imp__sub_82128848(ctx, base);
    static std::mutex mu;
    std::lock_guard<std::mutex> lock(mu);
    {
        // Memory headroom for the city streaming (every 10 s, max 40 lines):
        // free guest physical memory and the "city page pool" (0x838B4D90:
        // +0x1C page size, +0x20 / +0x24 page counts, +0x28).
        static auto next_mem = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        static int mem_lines = 0;
        const auto tnow = std::chrono::steady_clock::now();
        if (mem_lines < 40 && tnow >= next_mem) {
            next_mem = tnow + std::chrono::seconds(10);
            ++mem_lines;
            auto* memory = REX_KERNEL_STATE()->memory();
            const rex::memory::BaseHeap* heaps[3] = {memory->LookupHeapByType(true, 0x1000),
                                                     memory->LookupHeapByType(true, 0x10000),
                                                     memory->LookupHeapByType(true, 0x1000000)};
            uint32_t unreserved = 0, reserved = 0, used = 0, reserved_bytes = 0;
            memory->GetHeapsPageStatsSummary(heaps, 3, unreserved, reserved, used, reserved_bytes);
            std::string pool;
            for (uint32_t off = 0; off < 0x40; off += 4) pool += fmt::format(" {:08X}", Rd32(base, 0x838B4D90 + off));
            REXLOG_INFO("MEMORY guest physical: used {} MB of 512, reserved {} MB | city page pool:{}",
                        uint64_t(used) * 4096 >> 20, uint64_t(reserved_bytes) >> 20, pool);
            // The detailed-chunk mempools picked by 8211D940 (max 2 detailed
            // chunks): 0x827DD37C, 0x827DD3C0, 0x827DD438, and 0x827DD48C.
            if (mem_lines <= 3) {
                for (uint32_t obj : {0x827DD37Cu, 0x827DD3C0u, 0x827DD438u, 0x827DD48Cu}) {
                    std::string d;
                    for (uint32_t off = 0; off < 0x44; off += 4) d += fmt::format(" {:08X}", Rd32(base, obj + off));
                    REXLOG_INFO("MEMPOOL {:08X}:{}", obj, d);
                }
            }
        }
    }
    {
        // F9 = mark the moment (black ground on screen / hit an invisible
        // wall): log the camera, the loaded chunks and what is drawn, so the
        // moment lines up with the streaming log.
        static bool f9_was = false;
        const bool f9 = (GetAsyncKeyState(0x78) & 0x8000) != 0;
        if (f9 && !f9_was) {
            static int marks = 0;
            ++marks;
            const uint32_t pos = 0x827D9778 + 44;
            const uint32_t n = std::min<uint32_t>(Rd32(base, 0x829A97E8), 64);
            std::string line;
            for (uint32_t i = 0; i < n; ++i) {
                const uint32_t c = Rd32(base, 0x829A97E8 + 16 + i * 4);
                if (!c) continue;
                const uint32_t id = base[c + 8], al = base[c + 9];
                line += fmt::format(" [{} fl {:02X} vis {} own_draw {} al_draw {}]", ChunkName(base, c), base[c + 20],
                                    base[c + 228], int(base[0x829ABE10 + id * 8]),
                                    al < 255 ? int(base[0x829ABE10 + al * 8]) : -1);
            }
            REXLOG_INFO("MARK {} (F9) events in the 3 s before:{} | ground pieces forced visible so far: {}", marks,
                        RecentEvents(3000), fade_hysteresis::g_ground_forced.load());
            REXLOG_INFO("MARK {} (F9): camera {:.1f} {:.1f} {:.1f}, air view {}, {} chunks:{}", marks, RdF(base, pos),
                        RdF(base, pos + 4), RdF(base, pos + 8), far_clip::g_air_view, n, line);
            // Big objects of the detailed chunks around the camera: why is one not drawn?
            const float cx = RdF(base, pos), cy = RdF(base, pos + 4), cz = RdF(base, pos + 8);
            const uint32_t now_ms = fade_hysteresis::NowMs();
            int lines_out = 0, near_total = 0, never = 0, not_drawn = 0;
            for (uint32_t i = 0; i < n; ++i) {
                const uint32_t c = Rd32(base, 0x829A97E8 + 16 + i * 4);
                if (!c || (base[c + 20] & 0x80)) continue;
                const uint32_t recs = Rd32(base, c + 216);
                if (recs < 0x40000000 || recs > 0xF0000000) continue;
                uint32_t count = 0;
                for (uint32_t k = 0; k < 20000; ++k) {
                    const uint32_t r = recs + k * 80;
                    auto f = [&](uint32_t a) { return RdF(base, r + a); };
                    const float x0 = f(0), y0 = f(4), z0 = f(8), x1 = f(16), y1 = f(20), z1 = f(24);
                    if (!(std::isfinite(x0) && std::isfinite(z1) && x1 >= x0 && y1 >= y0 && z1 >= z0 &&
                          std::fabs(x0) < 6000 && std::fabs(z1) < 6000 && std::fabs(y0) < 2000 && x1 - x0 < 3000))
                        break;
                    ++count;
                    const float sx = x1 - x0, sz = z1 - z0;
                    if (std::max(sx, sz) < 10.0f) continue;
                    const float m = 40.0f;
                    if (!(cx > x0 - m && cx < x1 + m && cz > z0 - m && cz < z1 + m)) continue;
                    ++near_total;
                    auto& e = fade_hysteresis::table[fade_hysteresis::Hash(r) & (fade_hysteresis::kSlots - 1)];
                    const bool mine = e.rec.load() == r;
                    const uint32_t tms = mine ? e.tested_ms.load() : 0, dms = mine ? e.drawn_ms.load() : 0;
                    const bool drawn_now = dms && now_ms - dms < 300;
                    never += !tms;
                    not_drawn += !drawn_now;
                    const uint32_t sub = Rd32(base, r + 12);
                    const float fade = std::sqrt(std::max(0.0f, f(28)));
                    const float mx = 0.5f * (x0 + x1), my = 0.5f * (y0 + y1), mz = 0.5f * (z0 + z1);
                    const float dmid = std::sqrt((mx - cx) * (mx - cx) + (my - cy) * (my - cy) + (mz - cz) * (mz - cz));
                    if (!drawn_now && lines_out++ < 60)
                        REXLOG_INFO("MARK {} OBJ {}#{} box {:.0f} {:.0f} {:.0f} .. {:.0f} {:.0f} {:.0f} | fade {:.0f} m, middle "
                                    "{:.0f} m away | flags {:08X} hw76 {:04X} sub {:08X} sub_vis {} | tested {} ms ago, "
                                    "drawn {} ms ago",
                                    marks, ChunkName(base, c), k, x0, y0, z0, x1, y1, z1, fade, dmid,
                                    Rd32(base, r + 32), uint32_t(base[r + 76] << 8 | base[r + 77]), sub,
                                    (sub > 0x40000000 && sub < 0xF0000000) ? int(base[sub + 228]) : -1,
                                    tms ? int64_t(now_ms - tms) : -1, dms ? int64_t(now_ms - dms) : -1);
                }
                REXLOG_INFO("MARK {} chunk {}: {} object records", marks, ChunkName(base, c), count);
            }
            REXLOG_INFO("MARK {} big objects around the camera: {}, not drawn now {}, never tested {}", marks,
                        near_total, not_drawn, never);
        }
        f9_was = f9;
    }
    if (far_clip::g_air_view) {
        swap_delay::Apply(base);
    } else {
        ;
    }
    {
        // 15. Stale street visibility from the air. The cell walk (82127390, at
        // 8212756C) skips an object whose render data has flag 0x1000 when its
        // "visible from the PVS cell" byte (chunk +232, 24 bytes per object,
        // +20; set by the street PVS pass 825C29E8) is 0 and the switch byte
        // 0x827D9211 (default 1; only reader) is on. In air view the PVS pass
        // doesn't run, so those bytes keep the last street cell's answer:
        // everything not visible from where the player took off stayed
        // undrawn (black ground, never even fade-tested). While the air view
        // is on the switch is 0; its own value comes back after landing.
        // File "pvs_object_gate_in_air" = original.
        static const bool keep = !FileExists("pvs_object_gate_air_experiment");  // 2026-10-02: experiment now opt-in
        static bool saved = false;
        static uint8_t original = 1;
        if (far_clip::g_air_view && !keep) {
            if (!saved) {
                original = base[0x827D9211];
                saved = true;
                REXLOG_INFO("PVS object gate: off in air view (was {})", int(original));
            }
            base[0x827D9211] = 0;
        } else if (saved) {
            base[0x827D9211] = original;
            saved = false;
        }
    }
    if (!far_clip::g_air_view) {
        // On the ground: remember chunks as they are (no delayed swaps pending).
        std::memset(swap_delay::chunk_ptr, 0, sizeof(swap_delay::chunk_ptr));
    }
    if (!far_clip::g_air_view) {
        for (auto& c : callers) c.have = false;
        prev_count = 0;
        return;
    }
    ++calls;
    CallerState* cs = nullptr;
    for (auto& c : callers) {
        if (c.lr == lr || c.lr == 0) {
            c.lr = lr;
            cs = &c;
            break;
        }
    }
    const uint32_t count = std::min<uint32_t>(Rd32(base, 0x829A97E8), 64);
    // Chunks entering / leaving the loaded list (once per frame is enough: first caller).
    if (cs == &callers[0]) {
        uint32_t list[64];
        for (uint32_t i = 0; i < count; ++i) list[i] = Rd32(base, 0x829A97E8 + 16 + i * 4);
        if (prev_count) {
            for (uint32_t i = 0; i < count; ++i) {
                bool found = false;
                for (uint32_t j = 0; j < prev_count && !found; ++j) found = prev_list[j] == list[i];
                if (!found && list[i] && added.size() < 300) added += " " + ChunkName(base, list[i]);
            }
            for (uint32_t j = 0; j < prev_count; ++j) {
                bool found = false;
                for (uint32_t i = 0; i < count && !found; ++i) found = prev_list[j] == list[i];
                if (!found && removed.size() < 300) removed += fmt::format(" {:08X}", prev_list[j]);
            }
        }
        std::memcpy(prev_list, list, count * 4);
        prev_count = count;
    }
    if (cs) {
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t c = Rd32(base, 0x829A97E8 + 16 + i * 4);
            if (!c) continue;
            const uint8_t id = base[c + 8], al = base[c + 9];
            const uint8_t own = base[0x829ABE10 + id * 8];
            const uint8_t ald = al < 255 ? base[0x829ABE10 + al * 8] : 0;
            if (cs->have) {
                if (cs->own[id] != own) {
                    ++own_flips[id];
                    AddEvent(fmt::format("{} detailed {}", ChunkName(base, c), own ? "shown" : "hidden"));
                }
                if (al < 255 && cs->al[al] != ald) {
                    ++al_flips[id];
                    AddEvent(fmt::format("{} low-detail {}", ChunkName(base, c), ald ? "shown" : "hidden"));
                }
            }
            cs->own[id] = own;
            if (al < 255) cs->al[al] = ald;
            // Streaming: log a chunk's flags (+20) / visible (+228) whenever they
            // change during its first 5 s in the list (to find "fully loaded").
            static uint32_t seen_ptr[256];
            static std::chrono::steady_clock::time_point seen_at[256];
            static uint16_t seen_state[256];
            const auto tnow = std::chrono::steady_clock::now();
            const uint16_t state = uint16_t(base[c + 20]) << 8 | base[c + 228];
            static int tl_lines = 0;
            if (seen_ptr[id] != c) {
                seen_ptr[id] = c;
                seen_at[id] = tnow;
                seen_state[id] = state;
                if (tl_lines++ < 300)
                    REXLOG_INFO("CHUNKLOAD {} id {} in: flags {:02X} vis {} own_draw {} al_draw {}", ChunkName(base, c), id,
                                base[c + 20], base[c + 228], own, al < 255 ? int(ald) : -1);
            } else if (state != seen_state[id] && tnow - seen_at[id] < std::chrono::seconds(5)) {
                seen_state[id] = state;
                if (tl_lines++ < 300)
                    REXLOG_INFO("CHUNKLOAD {} +{} ms: flags {:02X} vis {} own_draw {} al_draw {}", ChunkName(base, c),
                                std::chrono::duration_cast<std::chrono::milliseconds>(tnow - seen_at[id]).count(),
                                base[c + 20], base[c + 228], own, al < 255 ? int(ald) : -1);
            }
        }
        cs->have = true;
    }
    static auto next = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    static int lines = 0;
    const auto now = std::chrono::steady_clock::now();
    if (now < next) return;
    next = now + std::chrono::seconds(1);
    std::string flips;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t c = Rd32(base, 0x829A97E8 + 16 + i * 4);
        if (!c) continue;
        const uint8_t id = base[c + 8];
        if (own_flips[id] || al_flips[id])
            flips += fmt::format(" [{} own {} al {}]", ChunkName(base, c), own_flips[id], al_flips[id]);
    }
    const uint32_t tests = fade_hysteresis::g_fade_tests.exchange(0);
    const uint32_t fflips = fade_hysteresis::g_fade_flips.exchange(0);
    std::string per_caller;
    for (int i = 0; i < 4; ++i) {
        const uint32_t lr_i = fade_hysteresis::g_caller_lr[i].load();
        if (!lr_i) continue;
        per_caller += fmt::format(" [{:08X} tests {} changes {} rapid {}]", lr_i,
                                  fade_hysteresis::g_c_tests[i].exchange(0), fade_hysteresis::g_c_flips[i].exchange(0),
                                  fade_hysteresis::g_c_rapid[i].exchange(0));
    }
    {
        std::string rj;
        for (int i = 0; i < 10; ++i) rj += fmt::format(" {}", fade_hysteresis::g_reject[i].exchange(0));
        rj += " | flag bits 40000000/200000/100000/80000/8000/20 detailed:";
        for (int b = 0; b < 6; ++b) rj += fmt::format(" {}", fade_hysteresis::g_flagbit[0][b].exchange(0));
        rj += fmt::format(" | fade stamps: same frame {}, previous frame {}, GAP {}, restarts prevented {}, counter +{}/s",
                          fade_hysteresis::g_st_same.exchange(0), fade_hysteresis::g_st_next.exchange(0),
                          fade_hysteresis::g_st_gap.exchange(0), fade_hysteresis::g_st_fixed.exchange(0),
                          fade_hysteresis::g_counter_steps.exchange(0));
        rj += " al:";
        for (int b = 0; b < 6; ++b) rj += fmt::format(" {}", fade_hysteresis::g_flagbit[1][b].exchange(0));
        static int rl = 0;
        if (far_clip::g_air_view && rl++ < 400)
            REXLOG_INFO("AIRFLICKER near (<80 m) objects 1s: {} tests, {} drawn | rejected by reason 0-9:{}",
                        fade_hysteresis::g_near_tests.exchange(0), fade_hysteresis::g_near_drawn.exchange(0), rj);
    }
    if (lines < 150) REXLOG_INFO("AIRFLICKER fade by caller:{} | flat ground objects: {} submitted of {} tests (boost x{})",
                                 per_caller, fade_hysteresis::g_flat_drawn.exchange(0),
                                 fade_hysteresis::g_flat_tests.exchange(0), fade_hysteresis::GroundBoost());
    if (lines++ < 150) {
        uint32_t cam = 0;  // camera height above the street, from the far clip rule
        (void)cam;
        REXLOG_INFO("AIRFLICKER 1s: {:.0f} m up | {} calls ({} callers) | fade in/out changes {} of {} tests | "
                    "chunks in:{} out:{} | flips:{}",
                    far_clip::g_air_view_height, calls, [] {
                        int n = 0;
                        for (auto& c : callers) n += c.lr != 0;
                        return n;
                    }(), fflips, tests, added.empty() ? " -" : added, removed.empty() ? " -" : removed,
                    flips.empty() ? " none" : flips);
    }
    calls = 0;
    added.clear();
    removed.clear();
    std::memset(own_flips, 0, sizeof(own_flips));
    std::memset(al_flips, 0, sizeof(al_flips));
}

extern "C" void __imp__sub_82121F60(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_82121F60) {
    static const bool original = FileExists("far_clip_original");
    const uint32_t pos = ctx.r3.u32;
    __imp__sub_82121F60(ctx, base);
    if (original || !pos || !AirViewEnabled()) return;
    if (!far_clip::High(base, RdF(base, pos + 0), RdF(base, pos + 4), RdF(base, pos + 8))) return;
    {
        // Diagnostic for the remaining air holes: every 3 s while high, the
        // loaded chunk list (0x829A97E8: +0 count, +16 pointers; chunk +8 id,
        // +9 al id, +16 name, +20 flags, +228 visible) and the al draw table
        // (0x8299BE10, 8 bytes per al id, byte 0 = draw). Max 40 dumps.
        static auto next = std::chrono::steady_clock::time_point{};
        static int dumps = 0;
        const auto now = std::chrono::steady_clock::now();
        if (dumps < 40 && now >= next) {
            next = now + std::chrono::seconds(3);
            ++dumps;
            const uint32_t count = Rd32(base, 0x829A97E8);
            std::string line;
            for (uint32_t i = 0; i < count && i < 64; ++i) {
                const uint32_t c = Rd32(base, 0x829A97E8 + 16 + i * 4);
                if (!c) continue;
                const uint32_t name_ptr = Rd32(base, c + 16);
                char name[40] = {};
                if (name_ptr > 0x10000 && name_ptr < 0xF0000000) {
                    for (int k = 0; k < 39; ++k) {
                        const char ch = char(base[name_ptr + k]);
                        if (!ch || ch < 32 || ch > 126) break;
                        name[k] = ch;
                    }
                }
                const uint32_t id = base[c + 8], al = base[c + 9];
                line += fmt::format(" [{} id {} al {} fl {:02X} vis {} al_draw {}]", name, id, al, base[c + 20],
                                    base[c + 228], al < 255 ? int(base[0x829ABE10 + al * 8]) : -1);
            }
            REXLOG_INFO("CHUNKS (camera {:.0f} {:.0f} {:.0f}): {} loaded:{}", RdF(base, pos + 0), RdF(base, pos + 4),
                        RdF(base, pos + 8), count, line);
        }
    }
    if (base[0x8370D0AA]) {
        static int lines = 0;
        if (lines++ < 20) {
            const uint32_t bits = (uint32_t(base[0x8370D0B0]) << 24) | (uint32_t(base[0x8370D0B1]) << 16) |
                                  (uint32_t(base[0x8370D0B2]) << 8) | base[0x8370D0B3];
            float d;
            std::memcpy(&d, &bits, 4);
            REXLOG_INFO("Far clip regions: cap {:.0f} m cleared (camera high)", d);
        }
    }
    base[0x8370D0AA] = 0;
    // Distance 0 (as 82121F60 writes when outside every region).
    base[0x8370D0B0] = base[0x8370D0B1] = base[0x8370D0B2] = base[0x8370D0B3] = 0;
}

// 7. IO trace while flying high (diagnostic for late-loading surfaces).
//    Logs every file read (82638E40 = the game's file read: buf r3, len r4,
//    file object r5; 82716408 = Win32 ReadFile) with host thread, caller,
//    size, time spent, plus file names from CreateFile (82716220). Only while
//    the air view is on (and 5 s after), max 2500 lines. File "io_trace_off"
//    disables it.
namespace io_trace {
std::mutex m;
std::unordered_map<uint32_t, std::string> names;  // handle -> file name
std::atomic<int> lines{0};
std::chrono::steady_clock::time_point last_air{};
const auto t0 = std::chrono::steady_clock::now();
bool Active() {
    static const bool off = !FileExists("io_trace");  // opt-in since 2026-09-30
    if (off || lines.load() >= 2500) return false;
    const auto now = std::chrono::steady_clock::now();
    if (far_clip::g_air_view) { last_air = now; return true; }
    return last_air.time_since_epoch().count() != 0 && now - last_air < std::chrono::seconds(5);
}
double Ms(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(t - t0).count();
}
}  // namespace io_trace

extern "C" void __imp__sub_82716220(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_82716220) {
    const uint32_t name_ptr = ctx.r3.u32;
    std::string name;
    if (name_ptr > 0x10000 && name_ptr < 0xF0000000) {
        for (int k = 0; k < 120; ++k) {
            const char ch = char(base[name_ptr + k]);
            if (!ch) break;
            name += ch;
        }
    }
    __imp__sub_82716220(ctx, base);
    const uint32_t h = ctx.r3.u32;
    {
        std::lock_guard<std::mutex> lock(io_trace::m);
        io_trace::names[h] = name;
    }
    if (io_trace::Active()) {
        io_trace::lines++;
        REXLOG_INFO("IOTRACE open {:.1f} ms tid {} '{}' -> {:08X}", io_trace::Ms(std::chrono::steady_clock::now()),
                    GetCurrentThreadId(), name, h);
    }
}

extern "C" void __imp__sub_82716408(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_82716408) {
    const uint32_t h = ctx.r3.u32, len = ctx.r5.u32, ov = ctx.r7.u32;
    const uint32_t lr = uint32_t(ctx.lr);
    const bool act = false;  // per-128 KB reads: known (0.1 ms each), not logged anymore
    const auto t = std::chrono::steady_clock::now();
    __imp__sub_82716408(ctx, base);
    if (act) {
        const auto t2 = std::chrono::steady_clock::now();
        const uint32_t off = ov ? Rd32(base, ov + 8) : 0xFFFFFFFFu;
        std::string name;
        {
            std::lock_guard<std::mutex> lock(io_trace::m);
            auto it = io_trace::names.find(h);
            if (it != io_trace::names.end()) name = it->second;
        }
        io_trace::lines++;
        REXLOG_INFO("IOTRACE read {:.1f} ms tid {} from {:08X} '{}' off {:X} len {} took {:.2f} ms",
                    io_trace::Ms(t), GetCurrentThreadId(), lr, name, off, len,
                    std::chrono::duration<double, std::milli>(t2 - t).count());
    }
}

extern "C" void __imp__sub_82638E40(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_82638E40) {
    const uint32_t len = ctx.r4.u32, fobj = ctx.r5.u32;
    const uint32_t lr = uint32_t(ctx.lr);
    const bool act = io_trace::Active();
    const auto t = std::chrono::steady_clock::now();
    __imp__sub_82638E40(ctx, base);
    if (act && len >= 4096) {
        const auto t2 = std::chrono::steady_clock::now();
        io_trace::lines++;
        REXLOG_INFO("IOTRACE gameread {:.1f} ms tid {} from {:08X} file {:08X} len {} got {} took {:.2f} ms",
                    io_trace::Ms(t), GetCurrentThreadId(), lr, fobj, len, ctx.r3.u32,
                    std::chrono::duration<double, std::milli>(t2 - t).count());
    }
}

// 8. A third detailed-chunk memory pool. The game streams the detailed city
//    into two fixed pools, "chunk0" (0x827DD37C) and "chunk1" (0x827DD3C0),
//    56.6 MB each (inside one 197 MB block made by 82184260), and its pool
//    picker 8211D940 refuses a third detailed chunk (count of loaded chunks
//    without flags 0x80 / 0x40 >= 2 -> no memory). From the air that means
//    two detailed chunks and the low-detail city everywhere else. The guest
//    has ~110 MB free, so one more pool of the same size is made at start
//    ("chunk2") and handed out when the other two are busy. Pools are bump
//    allocators: +4 base, +8 name, +40 used, +52 alignment, +56 / +64 size.
//    Not in multiplayer (0x8370E9F6: the game uses its mp pool there).
//    File "chunk_pool_off" = original behaviour.
namespace chunk_pool {
uint32_t extra = 0;  // the third pool object, 0 = none
constexpr uint32_t kChunk0 = 0x827DD37C, kChunk1 = 0x827DD3C0;
}  // namespace chunk_pool

extern "C" void __imp__sub_82716618(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_82184260(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_82184260) {
    __imp__sub_82184260(ctx, base);
    if (FileExists("chunk_pool_off") || chunk_pool::extra) return;
    const uint64_t saved[7] = {ctx.r3.u64, ctx.r4.u64, ctx.r5.u64, ctx.r6.u64, ctx.r7.u64, ctx.r8.u64, ctx.r9.u64};
    const uint32_t size = Rd32(base, chunk_pool::kChunk1 + 56);
    uint32_t mem = 0;
    if (size >= 0x100000 && size < 0x10000000) {
        ctx.r3.u64 = size;
        ctx.r4.u64 = 0xFFFFFFFFull;  // any physical address
        ctx.r5.u64 = 0x10000;        // 64 KB alignment, as the game's block
        ctx.r6.u64 = 0x20000004;     // same protection flags as the game's block
        __imp__sub_82716618(ctx, base);
        mem = ctx.r3.u32;
    }
    uint32_t obj = mem ? REX_KERNEL_STATE()->memory()->SystemHeapAlloc(0x80) : 0;
    if (mem && obj) {
        std::memcpy(base + obj, base + chunk_pool::kChunk1, 0x44);  // fresh pool: vtable, alignment, sizes
        Wr32(base, obj + 4, mem);
        std::memset(base + obj + 8, 0, 32);
        std::memcpy(base + obj + 8, "chunk2", 7);
        Wr32(base, obj + 40, 0);
        Wr32(base, obj + 44, 0);
        Wr32(base, obj + 48, 0);
        chunk_pool::extra = obj;
    }
    REXLOG_INFO("Chunk pools: third detailed-chunk pool {} ({} MB at {:08X}, pool object {:08X})",
                chunk_pool::extra ? "ready" : "NOT made", size >> 20, mem, obj);
    ctx.r3.u64 = saved[0]; ctx.r4.u64 = saved[1]; ctx.r5.u64 = saved[2]; ctx.r6.u64 = saved[3];
    ctx.r7.u64 = saved[4]; ctx.r8.u64 = saved[5]; ctx.r9.u64 = saved[6];
}

// Pool picker. The game's picker returns chunk1 unconditionally when chunk0
// is busy, and nothing once two detailed chunks are counted; with a third
// pool that is no longer safe: whatever it returns must be an empty pool.
// While the prefetch (section 9) streams its chunk, only chunk2 is handed out.
namespace chunk_pool {
bool prefetch_call = false;
uint32_t reserved = 0;  // pool the prefetch has claimed (until its chunk is gone / adopted), 0 = none
}
extern "C" void __imp__sub_8211D940(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_8211D940) {
    // Three pools, chunk0 / chunk1 / chunk2, for at most two detailed chunks
    // the game knows about plus one hidden prefetched chunk (section 9).
    // - The prefetch gets any empty pool, which is then reserved for it.
    // - The game keeps its own answer "no memory" (it may only ever see two
    //   detailed chunks: its stream manager has room for exactly two).
    // - When the game's answer is a pool that is in use or reserved (it
    //   returns chunk1 without looking when chunk0 is busy), another empty
    //   pool is given instead.
    using namespace chunk_pool;
    const uint32_t size = ctx.r3.u32;
    if (!extra) {
        __imp__sub_8211D940(ctx, base);
        return;
    }
    auto usable = [&](uint32_t pool) {
        return pool != reserved && Rd32(base, pool + 40) == 0 && size <= Rd32(base, pool + 56);
    };
    if (prefetch_call) {
        ctx.r3.u64 = 0;
        if (reserved) return;
        for (uint32_t pool : {extra, kChunk1, kChunk0}) {
            if (usable(pool)) {
                ctx.r3.u64 = pool;
                reserved = pool;
                return;
            }
        }
        return;
    }
    __imp__sub_8211D940(ctx, base);
    const uint32_t got = ctx.r3.u32;
    if (!got || (got != kChunk0 && got != kChunk1)) return;
    if (usable(got)) return;
    ctx.r3.u64 = 0;
    for (uint32_t pool : {kChunk0, kChunk1, extra}) {
        if (usable(pool)) {
            ctx.r3.u64 = pool;
            static int lines = 0;
            if (lines++ < 60)
                REXLOG_INFO("Chunk pools: game got a busy pool {:08X}, giving {} instead", got,
                            pool == extra ? "chunk2" : pool == kChunk0 ? "chunk0" : "chunk1");
            return;
        }
    }
}

// 9. A third detailed chunk from the air (prefetch). The stream manager
//    (82510A58) asks the trigger query 8250FB68 which chunks are wanted
//    (trigger boxes, world [0x829A97E8+272]: +148 count, +152 boxes of 44
//    bytes: +8 min, +20 max, +32 name) and compares them with the loaded
//    detailed chunks - in a stack list with room for exactly 2 (a third one
//    is read past its end: streaming stalled, first try). So the extra chunk
//    is HIDDEN from the stream manager: during its update the chunk counts as
//    a low-detail one (flags +20 | 0x80) and the main count [0x829A97F0] is
//    one lower. It is streamed with the game's own call (82512C30(name, 0...),
//    like the chunk_stream_in command) into the third pool, near where the
//    camera will be in ~1.5 s. When the camera / player enter its trigger box
//    it is adopted: the other detailed chunk that is no longer wanted is
//    unloaded (8211D060(index), like chunk_stream_out) and the extra chunk
//    becomes an ordinary one. A stale extra chunk (far behind) is unloaded
//    for the next. Air view only, not in multiplayer, one request at a time.
//    File "chunk_prefetch_off" = off.
namespace chunk_prefetch {
constexpr uint32_t kList = 0x829A97E8;  // +0 count, +8 main count, +16 chunk pointers
bool MainChunkName(const std::string& n) {
    if (n.rfind("sr_chunk", 0) != 0 || n.size() < 10) return false;
    size_t i = 8;
    while (i < n.size() && n[i] >= '0' && n[i] <= '9') ++i;
    if (i == 8) return false;
    if (i < n.size() && n[i] >= 'a' && n[i] <= 'z') ++i;
    return i == n.size();
}
std::string GuestString(uint8_t* base, uint32_t p) {
    std::string s;
    if (p > 0x10000 && p < 0xF0000000)
        for (int k = 0; k < 40; ++k) {
            const char ch = char(base[p + k]);
            if (!ch || ch < 32 || ch > 126) break;
            s += ch;
        }
    return s;
}
// Distance (x/z) from a point to the nearest trigger box of that chunk name.
float BoxDistance(uint8_t* base, const std::string& name, float x, float y, float z, bool use_y) {
    const uint32_t world = Rd32(base, kList + 272);
    if (!world) return 1e9f;
    const int32_t boxes = int32_t(Rd32(base, world + 148));
    const uint32_t box0 = Rd32(base, world + 152);
    if (boxes <= 0 || boxes > 4096 || !box0) return 1e9f;
    float best = 1e9f;
    for (int32_t i = 0; i < boxes; ++i) {
        const uint32_t b = box0 + uint32_t(i) * 44;
        if (GuestString(base, Rd32(base, b + 32)) != name) continue;
        const float x0 = RdF(base, b + 8), y0 = RdF(base, b + 12), z0 = RdF(base, b + 16);
        const float x1 = RdF(base, b + 20), y1 = RdF(base, b + 24), z1 = RdF(base, b + 28);
        const float dx = x < x0 ? x0 - x : x > x1 ? x - x1 : 0.0f;
        const float dz = z < z0 ? z0 - z : z > z1 ? z - z1 : 0.0f;
        const float dy = !use_y ? 0.0f : y < y0 ? y0 - y : y > y1 ? y - y1 : 0.0f;
        best = std::min(best, std::sqrt(dx * dx + dy * dy + dz * dz));
    }
    return best;
}
uint32_t extra_chunk = 0;  // chunk object of the hidden extra chunk (0 = none)
std::chrono::steady_clock::time_point extra_since{};
std::string extra_name;
std::string pending;
std::chrono::steady_clock::time_point pending_since{};
int lines = 0;
int32_t IndexOf(uint8_t* base, uint32_t chunk) {
    const uint32_t n = std::min<uint32_t>(Rd32(base, kList), 64);
    for (uint32_t i = 0; i < n; ++i)
        if (Rd32(base, kList + 16 + i * 4) == chunk) return int32_t(i);
    return -1;
}
}  // namespace chunk_prefetch

extern "C" void __imp__sub_82512C30(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_8211D060(PPCContext& ctx, uint8_t* base);
// Every stream-in request of the game. A request that replaces a chunk keeps
// that chunk's LIST INDEX until its data arrives (a callback then unloads that
// index); the new chunk only shows up in the list later. Unloading anything
// ourselves in that window shifts the list and the game then unloads the wrong
// chunk (seen: our prefetched chunk vanished mid-load and could never be
// streamed again). So no unloads of ours for 4 s after any game request.
namespace chunk_prefetch {
bool own_call = false;
std::chrono::steady_clock::time_point last_game_request{};
}
PPC_FUNC(sub_82512C30) {
    const bool game = !chunk_prefetch::own_call;
    __imp__sub_82512C30(ctx, base);
    if (game && int32_t(ctx.r3.u32) >= 0) chunk_prefetch::last_game_request = std::chrono::steady_clock::now();
}
extern "C" void __imp__sub_82510A58(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_82510A58) {
    using namespace chunk_prefetch;
    static const bool off = FileExists("chunk_prefetch_off");
    const uint32_t pool2 = chunk_pool::extra;
    if (off || !pool2 || base[0x8370E9F6]) {
        // Streaming look-ahead (10.): while high up and fast, the game's
        // stream manager gets the point where the player will be in ~1.5 s as
        // its second position (r4), so it loads the next chunk before he gets
        // there - with its own logic and its own 2-chunk limit (nothing hidden,
        // nothing unloaded behind its back). File "stream_lookahead_off" = off.
        static const bool la_off = FileExists("stream_lookahead_off");
        static float lx = 0, ly = 0, lz = 0, vx = 0, vy = 0, vz = 0;
        static auto lt = std::chrono::steady_clock::time_point{};
        const uint32_t pos_b = ctx.r4.u32;
        const auto now = std::chrono::steady_clock::now();
        const float dt = std::chrono::duration<float>(now - lt).count();
        const float x = RdF(base, pos_b), y = RdF(base, pos_b + 4), z = RdF(base, pos_b + 8);
        if (dt >= 0.2f) {
            if (dt < 1.0f) { vx = (x - lx) / dt; vy = (y - ly) / dt; vz = (z - lz) / dt; } else { vx = vy = vz = 0; }
            lx = x; ly = y; lz = z; lt = now;
        }
        const float speed = std::sqrt(vx * vx + vz * vz);
        const bool ahead = !la_off && pos_b > 0x10000 && pos_b < 0xF0000000 && far_clip::g_air_view &&
                           far_clip::g_air_view_height > 20.0f && speed > 20.0f && std::isfinite(speed) &&
                           !base[0x8370E9F6];
        if (!ahead) {
            __imp__sub_82510A58(ctx, base);
            return;
        }
        float t = 1.5f;
        if (speed * t > 150.0f) t = 150.0f / speed;
        uint8_t saved[12];
        std::memcpy(saved, base + pos_b, 12);
        auto wf = [&](uint32_t a, float f) { uint32_t u; std::memcpy(&u, &f, 4); Wr32(base, a, u); };
        wf(pos_b, x + vx * t);
        wf(pos_b + 8, z + vz * t);
        __imp__sub_82510A58(ctx, base);
        std::memcpy(base + pos_b, saved, 12);
        static int lines = 0;
        static auto next_log = std::chrono::steady_clock::time_point{};
        if (lines < 40 && now >= next_log) {
            next_log = now + std::chrono::seconds(5);
            ++lines;
            REXLOG_INFO("Stream look-ahead: {:.0f} m/s, streaming for {:.0f} {:.0f} (now {:.0f} {:.0f})", speed,
                        x + vx * t, z + vz * t, x, z);
        }
        return;
    }
    const uint32_t cam = 0x827D9778 + 44;
    const float cx = RdF(base, cam), cy = RdF(base, cam + 4), cz = RdF(base, cam + 8);
    float px = cx, py = cy, pz = cz;
    const uint32_t player = Rd32(base, 0x8309ABEC);
    if (player > 0x10000 && player < 0xF0000000 && Rd32(base, player + 72) == 1) {
        px = RdF(base, player + 20); py = RdF(base, player + 24); pz = RdF(base, player + 28);
    }
    // Unloading moves the entries of the loaded list; a load in flight has
    // remembered a list index (the chunk it replaces, slot +420). So nothing
    // is unloaded while any chunk is still streaming in (flags 0x20).
    bool streaming = false;
    {
        const uint32_t n0 = std::min<uint32_t>(Rd32(base, kList), 64);
        for (uint32_t i = 0; i < n0; ++i) {
            const uint32_t c = Rd32(base, kList + 16 + i * 4);
            if (c && (base[c + 20] & 0x20)) streaming = true;
        }
    }
    if (std::chrono::steady_clock::now() - last_game_request < std::chrono::seconds(4)) streaming = true;
    auto run_unload = [&](int32_t index, const char* why, const std::string& name) {
        PPCContext saved = ctx;
        ctx.r3.u64 = uint32_t(index);
        __imp__sub_8211D060(ctx, base);
        ctx = saved;
        if (lines++ < 120) REXLOG_INFO("Chunk prefetch: unloaded {} ({})", name, why);
    };
    // Our requested chunk entered the list (maybe still streaming): from now
    // on it is the hidden extra chunk.
    if (!pending.empty() && !extra_chunk) {
        const uint32_t n0 = std::min<uint32_t>(Rd32(base, kList), 64);
        for (uint32_t i = 0; i < n0; ++i) {
            const uint32_t c = Rd32(base, kList + 16 + i * 4);
            if (c && air_flicker::ChunkName(base, c) == pending) {
                // Ours by name: while our request is pending the game can't
                // stream the same chunk (its own request would be refused as
                // "already streaming"). Chunk +12 is logged for reference.
                if (lines++ < 120 && Rd32(base, c + 12) != chunk_pool::reserved)
                    REXLOG_INFO("Chunk prefetch: note: {} chunk+12 {:08X}, reserved pool {:08X}", pending,
                                Rd32(base, c + 12), chunk_pool::reserved);
                extra_chunk = c;
                extra_name = pending;
                extra_since = std::chrono::steady_clock::now();
                pending.clear();
                if (lines++ < 120)
                    REXLOG_INFO("Chunk prefetch: {} arriving (main count {}, list {})", extra_name,
                                Rd32(base, kList + 8), n0);
            }
        }
    }
    // The extra chunk: still there? wanted now (adopt)?
    int32_t extra_index = extra_chunk ? IndexOf(base, extra_chunk) : -1;
    if (extra_chunk && extra_index < 0) {
        if (lines++ < 120) REXLOG_INFO("Chunk prefetch: {} is gone", extra_name);
        extra_chunk = 0;
    }
    // The third pool is free again once nothing of ours is in it.
    if (!extra_chunk && pending.empty()) chunk_pool::reserved = 0;
    if (extra_chunk && !streaming) {
        const bool wanted = BoxDistance(base, extra_name, cx, cy, cz, true) == 0.0f ||
                            BoxDistance(base, extra_name, px, py, pz, true) == 0.0f;
        if (wanted) {
            // Unload the other detailed chunk that is wanted least.
            const uint32_t n = std::min<uint32_t>(Rd32(base, kList), 64);
            int32_t drop = -1;
            float drop_d = 0.0f;
            std::string drop_name;
            for (uint32_t i = 0; i < n; ++i) {
                const uint32_t c = Rd32(base, kList + 16 + i * 4);
                if (!c || c == extra_chunk || (base[c + 20] & 0xE0)) continue;
                const std::string name = air_flicker::ChunkName(base, c);
                const float d = std::min(BoxDistance(base, name, cx, cy, cz, true),
                                         BoxDistance(base, name, px, py, pz, true));
                if (d > drop_d) { drop_d = d; drop = int32_t(i); drop_name = name; }
            }
            if (drop >= 0) {
                run_unload(drop, "the camera moved into the prefetched chunk", drop_name);
                if (lines++ < 120) REXLOG_INFO("Chunk prefetch: {} is now an ordinary chunk", extra_name);
                extra_chunk = 0;
                extra_index = -1;
                chunk_pool::reserved = 0;
            }
        }
    }
    // Hide the extra chunk from the stream manager during its update.
    uint8_t saved_flags = 0;
    const bool hide = extra_chunk && IndexOf(base, extra_chunk) >= 0;
    if (hide) {
        saved_flags = base[extra_chunk + 20];
        base[extra_chunk + 20] = uint8_t(saved_flags | 0x80);
        Wr32(base, kList + 8, Rd32(base, kList + 8) - 1);
    }
    __imp__sub_82510A58(ctx, base);
    if (hide) {
        base[extra_chunk + 20] = uint8_t((base[extra_chunk + 20] & ~0x80) | (saved_flags & 0x80));
        Wr32(base, kList + 8, Rd32(base, kList + 8) + 1);
    }
    if (!far_clip::g_air_view) return;
    // Camera velocity (sampled every 250 ms).
    static float lx = 0, lz = 0, vx = 0, vz = 0;
    static auto lt = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    const float dt = std::chrono::duration<float>(now - lt).count();
    if (dt >= 0.25f) {
        if (dt < 1.0f) { vx = (cx - lx) / dt; vz = (cz - lz) / dt; } else { vx = vz = 0; }
        lx = cx; lz = cz; lt = now;
    }
    // Nothing while any chunk is streaming in or a request is pending.
    const uint32_t n = std::min<uint32_t>(Rd32(base, kList), 64);
    std::vector<std::string> loaded;
    uint32_t detailed = 0;
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t c = Rd32(base, kList + 16 + i * 4);
        if (!c) continue;
        if (base[c + 20] & 0x20) return;
        loaded.push_back(air_flicker::ChunkName(base, c));
        if (!(base[c + 20] & 0xC0) && c != extra_chunk) ++detailed;
    }
    if (!pending.empty()) {
        const bool arrived = std::find(loaded.begin(), loaded.end(), pending) != loaded.end();
        if (arrived) {
            for (uint32_t i = 0; i < n; ++i) {
                const uint32_t c = Rd32(base, kList + 16 + i * 4);
                if (c && air_flicker::ChunkName(base, c) == pending) { extra_chunk = c; extra_name = pending; }
            }
            if (lines++ < 120)
                REXLOG_INFO("Chunk prefetch: {} loaded (main count {}, total {})", pending, Rd32(base, kList + 8), n);
            pending.clear();
        } else if (now - pending_since < std::chrono::seconds(30)) {
            return;
        } else {
            pending.clear();
            chunk_pool::reserved = 0;
        }
    }
    if (detailed < 2) return;  // the game still has a free pool of its own
    // Best candidate: the main chunk nearest to where the camera will be.
    const float fx = cx + vx * 1.5f, fz = cz + vz * 1.5f;
    const uint32_t world = Rd32(base, kList + 272);
    if (!world) return;
    const int32_t boxes = int32_t(Rd32(base, world + 148));
    const uint32_t box0 = Rd32(base, world + 152);
    if (boxes <= 0 || boxes > 4096 || !box0) return;
    float best = 200.0f;
    uint32_t best_name = 0;
    std::string best_str;
    for (int32_t i = 0; i < boxes; ++i) {
        const uint32_t b = box0 + uint32_t(i) * 44;
        const uint32_t name_ptr = Rd32(base, b + 32);
        const std::string name = GuestString(base, name_ptr);
        if (!MainChunkName(name)) continue;
        if (std::find(loaded.begin(), loaded.end(), name) != loaded.end()) continue;
        const float d = BoxDistance(base, name, fx, cy, fz, false);
        if (d < best) { best = d; best_name = name_ptr; best_str = name; }
    }
    if (!best_name) return;
    if (extra_chunk) {
        // Replace a stale extra chunk (the new candidate is nearer by 100 m+).
        // Kept at least 8 s, and only dropped once it is well behind (150 m
        // from the camera now): swapping chunks in and out every few seconds
        // made groups of buildings pop between low and full detail.
        const float d_extra = BoxDistance(base, extra_name, fx, cy, fz, false);
        const float d_extra_now = BoxDistance(base, extra_name, cx, cy, cz, false);
        if (now - extra_since < std::chrono::seconds(8) || d_extra_now < 150.0f || d_extra < best + 100.0f) return;
        if (streaming) return;
        const int32_t idx = IndexOf(base, extra_chunk);
        if (idx >= 0) run_unload(idx, "left behind", extra_name);
        extra_chunk = 0;
        return;  // stream the next one on a later update (the pool is freed first)
    }
    if (chunk_pool::reserved) return;
    PPCContext saved = ctx;
    ctx.r3.u64 = best_name;
    ctx.r4.u64 = 0; ctx.r5.u64 = 0; ctx.r6.u64 = 0; ctx.r7.u64 = 0; ctx.r8.u64 = 0; ctx.r9.u64 = 0;
    chunk_pool::prefetch_call = true;
    own_call = true;
    __imp__sub_82512C30(ctx, base);
    own_call = false;
    chunk_pool::prefetch_call = false;
    const int32_t result = int32_t(ctx.r3.u32);
    ctx = saved;
    pending = best_str;
    pending_since = now;
    if (lines++ < 120)
        REXLOG_INFO("Chunk prefetch: {} ({:.0f} m from where the camera will be, camera {:.0f} {:.0f} {:.0f}) -> {}",
                    best_str, best, cx, cy, cz, result);
}

// 11. Native renderer step 1: DEPTH PRE-PASS CAPTURE PROBE (read only).
// The game's D3D DrawIndexedVertices (825D2478: r3 device, r4 primitive,
// r5 base vertex, r6 start index, r7 index count) first flushes the device's
// dirty register shadow into PM4, then writes VGT_INDX_OFFSET + DRAW_INDX.
// Device shadow: +0..+32 dirty masks (+0 VS float consts, +8 PS float consts,
// +16 fetch consts / shaders, +24 / +32 render-state blocks), +1024 fetch
// constants (32 slots x 24 bytes; vertex fetch 95 = slot 31 dwords 4-5),
// +1792 float constants (c4..c7 at +1856), +10240 RB_SURFACE_INFO,
// +10332 RB_COLOR_MASK, +12180 index buffer object (+0 bit 31 = 32-bit,
// +24 address), +12432 pixel shader, +12436 vertex shader.
// Pre-pass = pitch 640, 4x MSAA, colour mask 0. File "prepass_probe" = log
// every 10 s what a native pre-pass would have to reproduce.
namespace prepass_probe {
struct Hist {
    std::array<std::pair<uint64_t, uint64_t>, 16> e{};  // key, count
    uint64_t other = 0;
    void Add(uint64_t k) {
        for (auto& x : e) {
            if (x.second && x.first == k) { ++x.second; return; }
            if (!x.second) { x = {k, 1}; return; }
        }
        ++other;
    }
    std::string Top(int n, bool hex64 = false) {
        auto v = e;
        std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
        std::string s;
        for (int i = 0; i < n && v[i].second; ++i)
            s += hex64 ? fmt::format(" {:016X}x{}", v[i].first, v[i].second)
                       : fmt::format(" {:08X}x{}", uint32_t(v[i].first), v[i].second);
        if (other) s += fmt::format(" other x{}", other);
        return s;
    }
    void Clear() { e = {}; other = 0; }
};
std::mutex mutex;
uint64_t draws = 0, pre = 0, pre_indices = 0, runs = 0, cur_run = 0, max_run = 0, with_ps = 0, ib32 = 0;
uint64_t dirty_state = 0, dirty_fetch = 0, dirty_vsconst = 0, dirty_psconst = 0, orig_ticks = 0, orig_ticks_other = 0;
uint64_t vs_changes = 0, ib_changes = 0, last_vs = 0, last_ib = 0;
Hist vs, callers, prims, vsconst_masks, state_masks, threads, breakers, breaker_callers, breaker_threads;
uint32_t pre_thread = 0;
std::chrono::steady_clock::time_point next{};
}  // namespace prepass_probe
namespace rt_timing {  // defined in section 20; used by the section-19 hooks too
bool Enabled();
int Register(uint32_t a, uint32_t p);
void Add(int i, uint64_t dt);
}  // namespace rt_timing
// 19. MESH CENSUS (file "mesh_census" next to the exe; off otherwise). Sizes an
//    engine-level native renderer. Path decoded 2026-09-30:
//      82191328 (main pass) -> for each main-list entry: entry+0 = render
//      instance (248 bytes, vtable 0x8205C120) -> vtable+44 = 822347C0(inst,
//      pass, ...) -> vtable+68 gives the mesh object -> 8223E8D8(params, ...)
//      which per submesh sets VB/IB/decl/material constants straight into
//      the D3D device shadow and calls DrawIndexed (825D2478).
//    Every 10 s logs per frame: instance renders by pass, mesh draws, unique
//    meshes (how many instances share a mesh = instancing potential), draws
//    inside/outside the mesh path, and inclusive time of each level.
namespace mesh_census {
bool On() { static const bool on = FileExists("mesh_census"); return on; }
thread_local int in_mesh = 0;
std::mutex mutex;
std::atomic<uint64_t> frames{0};
uint64_t inst_calls = 0, inst_ticks = 0, mesh_calls = 0, mesh_ticks = 0, submeshes = 0;
uint64_t draws = 0, draws_in_mesh = 0, draw_ticks = 0, draw_ticks_in_mesh = 0, patch_calls = 0, patch_ticks = 0;
std::unordered_map<uint32_t, uint32_t> meshes;       // mesh object -> mesh draws this window (main pass only)
std::map<uint32_t, uint64_t> inst_pass, mesh_pass;   // pass argument -> calls
std::map<uint32_t, uint64_t> threads;                // host thread -> mesh draws
uint64_t main_mesh_calls = 0, main_frames_meshes = 0;
std::chrono::steady_clock::time_point next{};
double TickUs() {
    static const double t = [] {
        const auto a = std::chrono::steady_clock::now();
        const uint64_t r = __rdtsc();
        while (std::chrono::steady_clock::now() - a < std::chrono::milliseconds(20)) {}
        return 20000.0 / double(__rdtsc() - r);
    }();
    return t;
}
void MaybeLog() {  // mutex held
    const auto now = std::chrono::steady_clock::now();
    if (now < next) return;
    const bool first = next.time_since_epoch().count() == 0;
    next = now + std::chrono::seconds(10);
    const uint64_t f = frames.exchange(0);
    if (!first && f) {
        const double us = TickUs(), fr = double(f);
        uint64_t shared = 0, max_share = 0;
        for (auto& [m, n] : meshes) { if (n > f) shared += n; max_share = std::max<uint64_t>(max_share, n); }
        std::string ip, mp, th;
        for (auto& [p, n] : inst_pass) ip += fmt::format(" p{}:{:.0f}", p, n / fr);
        for (auto& [p, n] : mesh_pass) mp += fmt::format(" p{}:{:.0f}", p, n / fr);
        for (auto& [t, n] : threads) th += fmt::format(" {:X}:{:.0f}", t, n / fr);
        REXLOG_INFO("MESH CENSUS {} frames: per frame instance renders {:.0f} ({:.2f} ms) by pass{} | mesh draws "
                    "{:.0f} ({:.2f} ms, {:.1f} submeshes each) by pass{} | DrawIndexed {:.0f} ({:.2f} ms), from the "
                    "mesh path {:.0f} ({:.2f} ms) | vertex-fetch patcher {:.0f} ({:.2f} ms) | threads{}",
                    f, inst_calls / fr, inst_ticks * us / 1000.0 / fr, ip, mesh_calls / fr,
                    mesh_ticks * us / 1000.0 / fr, mesh_calls ? double(submeshes) / mesh_calls : 0.0, mp,
                    draws / fr, draw_ticks * us / 1000.0 / fr, draws_in_mesh / fr,
                    draw_ticks_in_mesh * us / 1000.0 / fr, patch_calls / fr, patch_ticks * us / 1000.0 / fr, th);
        REXLOG_INFO("MESH CENSUS main pass: {:.0f} mesh draws/frame over {} unique meshes in 10 s; draws of meshes "
                    "used more than once per frame on average: {:.0f}% ; most-drawn mesh {:.1f}/frame",
                    main_mesh_calls / fr, meshes.size(), main_mesh_calls ? 100.0 * shared / main_mesh_calls : 0.0,
                    max_share / fr);
    }
    inst_calls = inst_ticks = mesh_calls = mesh_ticks = submeshes = 0;
    draws = draws_in_mesh = draw_ticks = draw_ticks_in_mesh = patch_calls = patch_ticks = main_mesh_calls = 0;
    meshes.clear(); inst_pass.clear(); mesh_pass.clear(); threads.clear();
}
}  // namespace mesh_census
extern "C" void __imp__sub_822347C0(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_822347C0) {
    if (!mesh_census::On()) {
        if (rt_timing::Enabled()) {
            static const int idx = rt_timing::Register(0x822347C0, 0x82191328);
            const uint64_t t0 = __rdtsc();
            __imp__sub_822347C0(ctx, base);
            rt_timing::Add(idx, __rdtsc() - t0);
            return;
        }
        __imp__sub_822347C0(ctx, base); return;
    }
    const uint32_t pass = ctx.r4.u32;
    const uint64_t t0 = __rdtsc();
    __imp__sub_822347C0(ctx, base);
    const uint64_t dt = __rdtsc() - t0;
    std::lock_guard lock(mesh_census::mutex);
    ++mesh_census::inst_calls; mesh_census::inst_ticks += dt; ++mesh_census::inst_pass[pass];
}
extern "C" void __imp__sub_8223E8D8(PPCContext& ctx, uint8_t* base);
#include "native_mesh.inc"
PPC_FUNC(sub_8223E8D8) {
    if (!mesh_census::On()) {
        if (rt_timing::Enabled()) {
            static const int idx = rt_timing::Register(0x8223E8D8, 0x822347C0);
            const uint64_t t0 = __rdtsc();
            native_mesh::Dispatch(ctx, base);
            rt_timing::Add(idx, __rdtsc() - t0);
            return;
        }
        native_mesh::Dispatch(ctx, base); return;
    }
    const uint32_t mesh = Rd32(base, ctx.r3.u32 + 24), pass = ctx.r6.u32;
    const uint32_t subs = mesh ? __builtin_bswap16(*reinterpret_cast<uint16_t*>(base + mesh + 2)) : 0;
    ++mesh_census::in_mesh;
    const uint64_t t0 = __rdtsc();
    native_mesh::Dispatch(ctx, base);
    const uint64_t dt = __rdtsc() - t0;
    --mesh_census::in_mesh;
    std::lock_guard lock(mesh_census::mutex);
    ++mesh_census::mesh_calls; mesh_census::mesh_ticks += dt; mesh_census::submeshes += subs;
    ++mesh_census::mesh_pass[pass]; ++mesh_census::threads[GetCurrentThreadId()];
    if (pass == 8) { ++mesh_census::main_mesh_calls; ++mesh_census::meshes[mesh]; }
    mesh_census::MaybeLog();
}
extern "C" void __imp__sub_825DB738(PPCContext& ctx, uint8_t* base);
void sr_vfetch_patch(PPCContext& ctx, uint8_t* base);  // shadow_probe.cpp (native_vfetch.inc): memo
PPC_FUNC(sub_825DB738) {
    if (!mesh_census::On()) { sr_vfetch_patch(ctx, base); return; }
    const uint64_t t0 = __rdtsc();
    sr_vfetch_patch(ctx, base);
    const uint64_t dt = __rdtsc() - t0;
    std::lock_guard lock(mesh_census::mutex);
    ++mesh_census::patch_calls; mesh_census::patch_ticks += dt;
}


// 20. RENDER THREAD BREAKDOWN (same switch as section 19: dist\mesh_census). Inclusive time per frame
//    of the children of the render thread proc 82188FB8, of the frame render 82193A20 and of the scene
//    render 82191EE8 / 821915E8, per host thread. Log "RT BREAKDOWN" every 10 s (>= 0.10 ms/frame).
namespace rt_timing {
constexpr int kSlots = 160, kThreads = 12;
// dist\colour_census = only these lock-free per-thread timers (no section-19 mutex, no timeline).
bool Enabled() { static const bool on = mesh_census::On() || FileExists("colour_census"); return on; }
std::atomic<uint64_t> ticks[kSlots][kThreads], calls[kSlots][kThreads];
std::atomic<uint32_t> tids[kThreads];
std::atomic<int> nthreads{0};
uint32_t addr[kSlots], parent[kSlots];
std::atomic<int> nslots{0};
std::atomic<uint64_t> frames{0};
std::chrono::steady_clock::time_point next{};
int Register(uint32_t a, uint32_t p) {
    const int i = nslots.fetch_add(1);
    if (i >= kSlots) return kSlots - 1;
    addr[i] = a; parent[i] = p;
    return i;
}
int ThreadSlot() {
    thread_local int slot = -1;
    if (slot < 0) {
        const int n = nthreads.fetch_add(1);
        slot = n < kThreads ? n : kThreads - 1;
        if (n < kThreads) tids[slot] = GetCurrentThreadId();
    }
    return slot;
}
void Add(int i, uint64_t dt) {
    const int t = ThreadSlot();
    ticks[i][t].fetch_add(dt, std::memory_order_relaxed);
    calls[i][t].fetch_add(1, std::memory_order_relaxed);
}
void MaybeLog() {
    static std::mutex m;
    std::lock_guard lock(m);
    const auto now = std::chrono::steady_clock::now();
    if (now < next) return;
    const bool first = next.time_since_epoch().count() == 0;
    next = now + std::chrono::seconds(10);
    const uint64_t f = frames.exchange(0);
    std::string s;
    const double us = mesh_census::TickUs();
    for (int t = 0; t < std::min<int>(nthreads.load(), kThreads); ++t) {
        std::string part;
        for (int i = 0; i < std::min<int>(nslots.load(), kSlots); ++i) {
            const uint64_t tk = ticks[i][t].exchange(0), c = calls[i][t].exchange(0);
            if (first || !f) continue;
            const double ms = tk * us / 1000.0 / f;
            if (ms >= 0.03) part += fmt::format(" {:08X}<{:08X} {:.3f}ms x{:.1f}", addr[i], parent[i], ms, double(c) / f);
        }
        if (!part.empty()) s += fmt::format(" || thread {:X}:{}", tids[t].load(), part);
    }
    if (!first && f) REXLOG_INFO("RT BREAKDOWN {} frames (inclusive ms per frame, child<parent){}", f, s);
}
}  // namespace rt_timing

// 21. TIMELINE (same switch: dist\mesh_census). Ring buffer of (thread, id, extra, start, end) events from the
//    section-20 timers, the job wait 826370E8, 82209E30 / 821888C0 / 82189418 on the main thread and every timed
//    thread wait (perf_monitor). Every 10 s the frame containing the slowest recent main-thread frame is dumped as
//    "TIMELINE" lines: per thread, events in time order relative to the start of that window (ms).
namespace timeline {
struct Ev { uint32_t id, extra, tid, pad; uint64_t t0, t1; };
constexpr uint32_t kN = 1 << 17;
Ev ring[kN];
std::atomic<uint32_t> head{0};
bool On() { return mesh_census::On(); }
inline void Add(uint32_t id, uint32_t extra, uint64_t t0, uint64_t t1) {
    const uint32_t i = head.fetch_add(1, std::memory_order_relaxed) & (kN - 1);
    ring[i] = Ev{id, extra, GetCurrentThreadId(), 0, t0, t1};
}
uint64_t last_frame_t = 0, worst_dt = 0, worst_t = 0;
std::chrono::steady_clock::time_point next{};
void Frame() {  // main thread, once per frame (82194BD8)
    const uint64_t now = __rdtsc();
    const auto n = std::chrono::steady_clock::now();
    if (last_frame_t && n + std::chrono::milliseconds(1500) >= next) {  // only the last 1.5 s before a dump
        const uint64_t dt = now - last_frame_t;
        if (dt > worst_dt && head.load() > 2000) { worst_dt = dt; worst_t = now; }
    }
    last_frame_t = now;
    if (n < next) return;
    const bool first = next.time_since_epoch().count() == 0;
    next = n + std::chrono::seconds(10);
    if (first || !worst_t) return;
    // Window: 3 frames' worth before the end of the worst frame.
    const uint64_t t_end = worst_t, t_begin = worst_t - std::min<uint64_t>(worst_t, worst_dt * 3);
    const double us = mesh_census::TickUs();
    std::vector<Ev> v;
    const uint32_t h = head.load();
    for (uint32_t k = 0; k < kN && k < h; ++k) {
        const Ev& e = ring[(h - 1 - k) & (kN - 1)];
        if (e.t1 < t_begin && e.t0 < t_begin) { if (e.t1 + worst_dt * 20 < t_begin) break; continue; }
        if (e.t0 > t_end) continue;
        v.push_back(e);
    }
    std::sort(v.begin(), v.end(), [](const Ev& a, const Ev& b) { return a.tid != b.tid ? a.tid < b.tid : a.t0 < b.t0; });
    REXLOG_INFO("TIMELINE worst frame {:.2f} ms; window {:.2f} ms; {} events (id(extra) start+duration ms)",
                worst_dt * us / 1000.0, (t_end - t_begin) * us / 1000.0, v.size());
    std::string line; uint32_t cur = 0; int n_in_line = 0;
    auto flush = [&] { if (!line.empty()) REXLOG_INFO("TIMELINE thread {:X}:{}", cur, line); line.clear(); n_in_line = 0; };
    for (const Ev& e : v) {
        if (e.tid != cur) { flush(); cur = e.tid; }
        const double a = (double(int64_t(e.t0 - t_begin))) * us / 1000.0, d = (e.t1 - e.t0) * us / 1000.0;
        if (d < 0.05 && e.id != 1) continue;
        line += e.extra ? fmt::format(" {:08X}({:X})@{:.2f}+{:.2f}", e.id, e.extra, a, d) : fmt::format(" {:08X}@{:.2f}+{:.2f}", e.id, a, d);
        if (++n_in_line >= 60) flush();
    }
    flush();
    worst_dt = 0; worst_t = 0;
}
}  // namespace timeline
extern "C" void sr_timeline_wait(uint32_t fn, uint32_t caller, uint64_t microseconds) {
    if (!timeline::On()) return;
    const uint64_t t1 = __rdtsc();
    const uint64_t d = uint64_t(double(microseconds) / mesh_census::TickUs());
    timeline::Add(fn, caller, t1 - std::min(t1, d), t1);
}
#include "render_overlap.inc"
#include "colour_split.inc"
#define SR_TL_TIMED(A)                                                                     \
    extern "C" void __imp__sub_##A(PPCContext& ctx, uint8_t* base);                      \
    PPC_FUNC(sub_##A) {                                                                    \
        if (!timeline::On()) { __imp__sub_##A(ctx, base); return; }                        \
        const uint32_t lr = uint32_t(ctx.lr);                                              \
        const uint64_t t0 = __rdtsc();                                                     \
        __imp__sub_##A(ctx, base);                                                         \
        timeline::Add(0x##A, lr, t0, __rdtsc());                                           \
    }
SR_TL_TIMED(826370E8)
SR_TL_TIMED(82209E30)
SR_TL_TIMED(82189418)
#undef SR_TL_TIMED
extern "C" void __imp__sub_821888C0(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_821888C0) {  // render kick (section 24: waits for the render thread when overlapping)
    render_overlap::KickWait();
    if (!timeline::On()) { __imp__sub_821888C0(ctx, base); return; }
    const uint32_t lr = uint32_t(ctx.lr);
    const uint64_t t0 = __rdtsc();
    __imp__sub_821888C0(ctx, base);
    timeline::Add(0x821888C0, lr, t0, __rdtsc());
}

// 22. RENDER-LIST PREP BATCH SIZE. 82190490 (from 821938B8) prepares every render-list entry (82190058) in jobs of
//    [0x827D6CB4] entries (game default 15) on the worker threads; 821938B8 then joins them (~1-1.7 ms of the
//    render thread's critical path, see research handoff 2026-09-30). File "list_batch.txt" = fixed batch size.
//    File "list_batch_ab" = cycle 15 / 40 / 100 / 8 every 10 s and log "LIST BATCH A/B" (frame time, 821938B8,
//    82193A20 per setting, cumulative).
namespace batch_ab {
constexpr uint32_t kAddr = 0x827D6CB4;
constexpr uint32_t kValues[] = {15, 40, 100, 8};
constexpr int kModes = 4;
bool On() { static const bool on = FileExists("list_batch_ab"); return on; }
int fixed = [] {
    int v = 0;
    if (FILE* f = std::fopen("list_batch.txt", "rb")) { if (std::fscanf(f, "%d", &v) != 1) v = 0; std::fclose(f); }
    return v;
}();
std::atomic<int> mode{0};
std::atomic<uint64_t> frames[kModes], frame_ticks[kModes], t938[kModes], t3a20[kModes];
uint64_t last_t = 0;
bool switched = true;
std::chrono::steady_clock::time_point next{};
void Frame(uint8_t* base) {  // main thread, 82194BD8
    if (fixed > 0 && fixed < 100000 && Rd32(base, kAddr) != uint32_t(fixed)) {
        Wr32(base, kAddr, uint32_t(fixed));
        REXLOG_INFO("List batch: render-list prep jobs of {} entries (list_batch.txt)", fixed);
    }
    if (!On()) return;
    const uint64_t now = __rdtsc();
    const int m = mode.load();
    if (last_t && !switched) { frames[m]++; frame_ticks[m] += now - last_t; }
    switched = false;
    last_t = now;
    const auto n = std::chrono::steady_clock::now();
    if (n < next) return;
    const bool first = next.time_since_epoch().count() == 0;
    next = n + std::chrono::seconds(10);
    if (!first) {
        const double us = mesh_census::TickUs();
        std::string s;
        for (int i = 0; i < kModes; ++i) {
            const double f = double(frames[i].load());
            if (f < 1) continue;
            s += fmt::format(" | batch {}: {:.0f} frames, frame {:.2f} ms, 821938B8 {:.2f} ms, 82193A20 {:.2f} ms", kValues[i], f,
                             frame_ticks[i] * us / 1000.0 / f, t938[i] * us / 1000.0 / f, t3a20[i] * us / 1000.0 / f);
        }
        REXLOG_INFO("LIST BATCH A/B (cumulative){}", s);
        mode.store((m + 1) % kModes);
        switched = true;
    }
    Wr32(base, kAddr, kValues[mode.load()]);
}
void Add(uint32_t fn, uint64_t dt) {
    const int m = mode.load();
    if (fn == 0x821938B8) t938[m] += dt; else if (fn == 0x82193A20) t3a20[m] += dt;
}
}  // namespace batch_ab
bool sr_native_copy68(PPCContext& ctx, uint8_t* base);  // native_flush.inc (shadow_probe.cpp)
bool sr_native_context(PPCContext& ctx, uint8_t* base);
// Plain wrappers: no diagnostics, A/B or special hooks this run -> the wrapped function directly (the checks
// below cost a few ns per call, ~60k calls per frame in heavy scenes).
inline bool sr_rt_plain() {
    static const bool plain = !rt_timing::Enabled() && !batch_ab::On() && !colour_split::On() &&
                              !render_overlap::Point() && native_mesh::Mode() != 2;
    return plain;
}
#define SR_RT_TIMED(A, P)                                                                  \
    extern "C" void __imp__sub_##A(PPCContext& ctx, uint8_t* base);                      \
    static void sr_timed_##A(PPCContext& ctx, uint8_t* base);                              \
    PPC_FUNC(sub_##A) {                                                                    \
        if (sr_rt_plain()) [[likely]] {                                                    \
            if (0x##A == 0x82129CA0 && sr_native_copy68(ctx, base)) return;                \
            if (0x##A == 0x826306A8 && sr_native_context(ctx, base)) return;               \
            __imp__sub_##A(ctx, base);                                                     \
            return;                                                                        \
        }                                                                                  \
        if (native_mesh::RecActive(ctx)) [[unlikely]] {                                    \
            native_mesh::RecBefore(ctx, base);                                             \
            sr_timed_##A(ctx, base);                                                       \
            native_mesh::RecAfter(ctx, base);                                              \
            return;                                                                        \
        }                                                                                  \
        if (sr_special_hooks()) [[unlikely]] {                                             \
            if (sr_special_before(ctx, base, 0x##A)) return;                               \
            sr_timed_##A(ctx, base);                                                       \
            sr_special_after(ctx, base, 0x##A);                                            \
            return;                                                                        \
        }                                                                                  \
        sr_timed_##A(ctx, base);                                                           \
    }                                                                                      \
    static void sr_timed_##A(PPCContext& ctx, uint8_t* base) {                             \
        static const int idx = rt_timing::Register(0x##A, 0x##P);                         \
        if (!rt_timing::Enabled()) {                                                        \
            if (0x##A == 0x82129CA0 && sr_native_copy68(ctx, base)) return;                \
            if ((0x##A == 0x821938B8 || 0x##A == 0x82193A20) && batch_ab::On()) {          \
                const uint64_t t0 = __rdtsc();                                             \
                __imp__sub_##A(ctx, base);                                                 \
                batch_ab::Add(0x##A, __rdtsc() - t0);                                      \
                return;                                                                    \
            }                                                                              \
            __imp__sub_##A(ctx, base); return;                                             \
        }                                                                                  \
        const uint64_t t0 = __rdtsc();                                                     \
        __imp__sub_##A(ctx, base);                                                         \
        const uint64_t t1 = __rdtsc();                                                     \
        rt_timing::Add(idx, t1 - t0);                                                      \
        if (0x##A != 0x8223DA00 && 0x##A != 0x82237770 && 0x##A != 0x8263D818 && 0x##A != 0x82715788) \
            timeline::Add(0x##A, 0, t0, t1);                                               \
    }
SR_RT_TIMED(82129060, 82188FB8)
SR_RT_TIMED(8217B638, 82188FB8)
SR_RT_TIMED(821884F8, 82188FB8)
SR_RT_TIMED(8218F380, 82188FB8)
SR_RT_TIMED(821903F0, 82188FB8)
SR_RT_TIMED(821937B8, 82188FB8)
SR_RT_TIMED(82193A20, 82188FB8)
SR_RT_TIMED(82196660, 82188FB8)
SR_RT_TIMED(826365E0, 82188FB8)
SR_RT_TIMED(82636688, 82188FB8)
SR_RT_TIMED(82637238, 82188FB8)
SR_RT_TIMED(82637738, 82188FB8)
SR_RT_TIMED(8263DD80, 82188FB8)
SR_RT_TIMED(82716C40, 82188FB8)
SR_RT_TIMED(82716D20, 82188FB8)
SR_RT_TIMED(8213D2B8, 82193A20)
SR_RT_TIMED(8214ABF0, 82193A20)
SR_RT_TIMED(8214BA10, 82193A20)
SR_RT_TIMED(82181AC0, 82193A20)
SR_RT_TIMED(82181BC8, 82193A20)
SR_RT_TIMED(8218CDE8, 82193A20)
SR_RT_TIMED(8218D948, 82193A20)
SR_RT_TIMED(82191EE8, 82193A20)
SR_RT_TIMED(82192490, 82193A20)
SR_RT_TIMED(82192850, 82193A20)
SR_RT_TIMED(821929C8, 82193A20)
SR_RT_TIMED(82192CE0, 82193A20)
SR_RT_TIMED(82193130, 82193A20)
SR_RT_TIMED(821938B8, 82193A20)
SR_RT_TIMED(8219A780, 82193A20)
SR_RT_TIMED(8219DCF0, 82193A20)
SR_RT_TIMED(8223B130, 82193A20)
SR_RT_TIMED(8223B1C0, 82193A20)
SR_RT_TIMED(8223B300, 82193A20)
SR_RT_TIMED(82515DC0, 82193A20)
SR_RT_TIMED(825662F8, 82193A20)
SR_RT_TIMED(8263D818, 82193A20)
SR_RT_TIMED(8263EE48, 82193A20)
SR_RT_TIMED(8263EF48, 82193A20)
SR_RT_TIMED(8263F048, 82193A20)
SR_RT_TIMED(82715788, 82193A20)
SR_RT_TIMED(8212ACB8, 82191EE8)
SR_RT_TIMED(821915E8, 82191EE8)
SR_RT_TIMED(826374B0, 82191EE8)
SR_RT_TIMED(8218DA88, 821915E8)
SR_RT_TIMED(8218DC50, 821915E8)
SR_RT_TIMED(82191328, 821915E8)
SR_RT_TIMED(821968F8, 821915E8)
SR_RT_TIMED(8223DA00, 822347C0)
SR_RT_TIMED(82237770, 00000000)
// Colour-pass breakdown (2026-09-30, build cb1): children of 82191328 / 822347C0 / 8223E8D8.
// 8219F258, 82631CF0, 82631BF0, 8212BC88 are also called from 8223E8D8/822347C0 (label = first parent).
SR_RT_TIMED(8219F258, 82191328)
SR_RT_TIMED(82631EC8, 82191328)
SR_RT_TIMED(82631CF0, 82191328)
SR_RT_TIMED(8219F2B0, 82191328)
SR_RT_TIMED(8263ECD0, 82191328)
SR_RT_TIMED(82631DC8, 82191328)
SR_RT_TIMED(82631BF0, 82191328)
SR_RT_TIMED(8223A298, 82191328)
SR_RT_TIMED(8219EF10, 82191328)
SR_RT_TIMED(8219E548, 82191328)
SR_RT_TIMED(8219E230, 82191328)
SR_RT_TIMED(82189658, 82191328)
SR_RT_TIMED(8212BC88, 82191328)
SR_RT_TIMED(8263ED98, 822347C0)
SR_RT_TIMED(82182F18, 822347C0)
SR_RT_TIMED(826406C8, 822347C0)
SR_RT_TIMED(8223F7E8, 822347C0)
SR_RT_TIMED(8223F6D8, 822347C0)
SR_RT_TIMED(82183DC0, 822347C0)
SR_RT_TIMED(82182FA0, 822347C0)
SR_RT_TIMED(825DAE10, 8223E8D8)
SR_RT_TIMED(825D0E90, 8223E8D8)
SR_RT_TIMED(825D0D80, 8223E8D8)
SR_RT_TIMED(82129CA0, 8223E8D8)
SR_RT_TIMED(826446F8, 8223E8D8)
SR_RT_TIMED(82631BB0, 8223E8D8)
SR_RT_TIMED(82631950, 8223E8D8)
SR_RT_TIMED(826306A8, 8223E8D8)
SR_RT_TIMED(822432F0, 8223E8D8)
SR_RT_TIMED(8223CC08, 8223E8D8)
SR_RT_TIMED(8223CA08, 8223E8D8)
SR_RT_TIMED(8223C828, 8223E8D8)
SR_RT_TIMED(8223C0A0, 8223E8D8)
SR_RT_TIMED(8223BF20, 8223E8D8)
SR_RT_TIMED(8223AE00, 8223E8D8)
SR_RT_TIMED(8223AD40, 8223E8D8)
SR_RT_TIMED(8223A720, 8223E8D8)
SR_RT_TIMED(8219F138, 8223E8D8)
SR_RT_TIMED(8219A128, 8223E8D8)
SR_RT_TIMED(82182610, 8223E8D8)
SR_RT_TIMED(821825C8, 8223E8D8)
SR_RT_TIMED(8214BE00, 8223E8D8)
#undef SR_RT_TIMED

extern "C" void __imp__sub_825D2478(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_825CDB08(PPCContext& ctx, uint8_t* base);
namespace glass_alpha {
// The glass effects blend their opacity and view-dependent reflection. The
// inherited cutout alpha test (reference 160/255) rejects Gat's 0.5-opacity
// lenses at some angles. Disable it only for a glass draw with a pixel shader,
// then restore it before another material can draw. Keep the state cache intact.
bool IsGlass(uint32_t hash) {
    return hash == 0xF04DA63F || hash == 0xFCAE7DF7 ||
           hash == 0xE1196D93 || hash == 0xEDFAB65B;
}
class DrawScope {
    PPCContext& ctx_;
    uint8_t* base_;
    uint32_t device_ = 0;
    void Set(bool enabled) {
        PPCContext scratch = ctx_;
        scratch.r3.u64 = device_;
        scratch.r4.u64 = enabled ? 1 : 0;
        __imp__sub_825CDB08(scratch, base_);
    }
public:
    DrawScope(PPCContext& ctx, uint8_t* base) : ctx_(ctx), base_(base) {
        const uint32_t device = ctx.r3.u32;
        if (!(Rd32(base, device + 10428) & 8) || !Rd32(base, device + 12200)) return;
        const uint32_t tls = Rd32(base, ctx.r13.u32);
        const uint32_t state = Rd32(base, tls + 7692);
        const uint32_t shader = Rd32(base, state + 4252);
        if (shader == 0xFFFFFFFFu) return;
        const uint32_t shaders = Rd32(base, state + 4280);
        if (!shaders || !IsGlass(Rd32(base, shaders + shader * 12))) return;
        device_ = device;
        Set(false);
    }
    ~DrawScope() { if (device_) Set(true); }
    DrawScope(const DrawScope&) = delete;
    DrawScope& operator=(const DrawScope&) = delete;
};
}  // namespace glass_alpha
static void sr_draw_body(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_825D2478) {
    if (native_mesh::RecActive(ctx)) [[unlikely]] {
        native_mesh::RecBefore(ctx, base);
        sr_draw_body(ctx, base);
        native_mesh::RecAfter(ctx, base);
        return;
    }
    glass_alpha::DrawScope glass(ctx, base);
    sr_draw_body(ctx, base);
}
static void sr_draw_body(PPCContext& ctx, uint8_t* base) {
    using namespace prepass_probe;
    static const bool on = FileExists("prepass_probe");
    if (!on) {
        if (!mesh_census::On()) {
            if (rt_timing::Enabled()) {
                static const int idx = rt_timing::Register(0x825D2478, 0x8223E8D8);
                const uint64_t t0 = __rdtsc();
                __imp__sub_825D2478(ctx, base);
                rt_timing::Add(idx, __rdtsc() - t0);
                return;
            }
            __imp__sub_825D2478(ctx, base); return;
        }
        const bool inside = mesh_census::in_mesh > 0;
        const uint64_t t0 = __rdtsc();
        __imp__sub_825D2478(ctx, base);
        const uint64_t dt = __rdtsc() - t0;
        std::lock_guard lock(mesh_census::mutex);
        ++mesh_census::draws; mesh_census::draw_ticks += dt;
        if (inside) { ++mesh_census::draws_in_mesh; mesh_census::draw_ticks_in_mesh += dt; }
        return;
    }
    const uint32_t dev = ctx.r3.u32;
    const uint32_t surf = Rd32(base, dev + 10240);
    const bool is_pre = (surf & 0x3FFF) == 640 && ((surf >> 16) & 3) == 2 && Rd32(base, dev + 10332) == 0;
    const uint64_t d0 = *reinterpret_cast<uint64_t*>(base + dev + 0);   // raw (big-endian) masks: only != 0 matters
    const uint64_t d1 = *reinterpret_cast<uint64_t*>(base + dev + 8);
    const uint64_t d2 = *reinterpret_cast<uint64_t*>(base + dev + 16);
    const uint64_t d3 = *reinterpret_cast<uint64_t*>(base + dev + 24);
    const uint64_t d4 = *reinterpret_cast<uint64_t*>(base + dev + 32);
    const uint32_t vsp = Rd32(base, dev + 12436), psp = Rd32(base, dev + 12432), ibp = Rd32(base, dev + 12180);
    const uint32_t prim = ctx.r4.u32, count = ctx.r7.u32, lr = uint32_t(ctx.lr);
    const uint64_t t0 = __rdtsc();
    __imp__sub_825D2478(ctx, base);
    const uint64_t dt = __rdtsc() - t0;
    std::lock_guard lock(mutex);
    ++draws;
    if (!is_pre) {
        orig_ticks_other += dt;
        if (cur_run) {
            ++runs; max_run = std::max(max_run, cur_run); cur_run = 0;
            // What ended the run: surface info / colour mask / depth info of the draw, its caller and thread.
            breakers.Add((uint64_t(surf) << 32) | Rd32(base, dev + 10332));
            breaker_callers.Add(lr);
            breaker_threads.Add((uint64_t(GetCurrentThreadId() == pre_thread) << 32) | Rd32(base, dev + 10248));
        }
    } else {
        ++pre; ++cur_run;
        pre_thread = GetCurrentThreadId();
        orig_ticks += dt;
        pre_indices += count;
        with_ps += psp != 0;
        if (ibp && (Rd32(base, ibp) & 0x80000000u)) ++ib32;
        if (d0) { ++dirty_vsconst; vsconst_masks.Add(__builtin_bswap64(d0)); }
        if (d1) ++dirty_psconst;
        if (d2) ++dirty_fetch;
        if (d3 || d4) { ++dirty_state; state_masks.Add(__builtin_bswap64(d3) ^ (__builtin_bswap64(d4) >> 1)); }
        if (vsp != last_vs) { ++vs_changes; last_vs = vsp; }
        if (ibp != last_ib) { ++ib_changes; last_ib = ibp; }
        vs.Add(vsp);
        callers.Add(lr);
        prims.Add(prim);
        threads.Add(GetCurrentThreadId());
    }
    const auto now = std::chrono::steady_clock::now();
    if (now < next) return;
    const bool first = next.time_since_epoch().count() == 0;
    next = now + std::chrono::seconds(10);
    if (first) return;
    static const double tick_us = [] {
        const auto a = std::chrono::steady_clock::now();
        const uint64_t r = __rdtsc();
        while (std::chrono::steady_clock::now() - a < std::chrono::milliseconds(20)) {}
        return 20000.0 / double(__rdtsc() - r);
    }();
    REXLOG_INFO("PREPASS PROBE 10 s: DrawIndexed {} calls, pre-pass {} ({:.0f}%), {} runs (avg {:.0f}, max {}), "
                "avg {:.0f} indices, with pixel shader {}, 32-bit IB {} | game draw call cost: pre-pass {:.2f} us, "
                "other {:.2f} us | per pre-pass draw dirty: VS consts {}, PS consts {}, fetch/shader {}, render state "
                "{} | VS changes {}, IB changes {}",
                draws, pre, draws ? 100.0 * pre / draws : 0.0, runs, runs ? double(pre) / runs : 0.0, max_run,
                pre ? double(pre_indices) / pre : 0.0, with_ps, ib32, pre ? orig_ticks * tick_us / pre : 0.0,
                draws > pre ? orig_ticks_other * tick_us / (draws - pre) : 0.0, dirty_vsconst, dirty_psconst,
                dirty_fetch, dirty_state, vs_changes, ib_changes);
    REXLOG_INFO("PREPASS PROBE shaders:{} | callers:{} | prims:{} | threads:{}", vs.Top(10), callers.Top(8),
                prims.Top(4), threads.Top(3));
    REXLOG_INFO("PREPASS PROBE run ended by (surface<<32|colour mask):{} | callers:{} | (same thread<<32|depth info):{}",
                breakers.Top(8, true), breaker_callers.Top(8), breakers.e[0].second ? breaker_threads.Top(6, true) : "");
    REXLOG_INFO("PREPASS PROBE dirty masks: VS consts{} | render state{}", vsconst_masks.Top(6, true),
                state_masks.Top(6, true));
    draws = pre = pre_indices = runs = cur_run = max_run = with_ps = ib32 = 0;
    dirty_state = dirty_fetch = dirty_vsconst = dirty_psconst = orig_ticks = orig_ticks_other = 0;
    vs_changes = ib_changes = 0;
    breakers.Clear(); breaker_callers.Clear(); breaker_threads.Clear(); vs.Clear(); callers.Clear(); prims.Clear(); vsconst_masks.Clear(); state_masks.Clear(); threads.Clear();
}

// 12. Bigger per-frame render instance pool. Every object that passes the
//    visibility/fade test (82129550 -> 82195D78) gets a 248-byte render
//    instance from "render_inst_pool" (0x827DDD34, same bump-allocator class
//    as the chunk pools: +4 base, +40 used, +56 / +64 size), 82194B78. The
//    pool is reset once per frame (82194BD8, from 821888C0) and holds only
//    1,000,000 bytes (~4000 objects). From the air (PVS off, far city) a frame
//    needs more: 82194B78 returns 0 and the object is silently not drawn -
//    the ones submitted last (often the ground around the player) go black,
//    and whole stretches of the far city are missing. At the first reset the
//    pool gets its own buffer of render_inst_pool_mb.txt MB (default 16; the
//    old buffer is left alone). File "render_pool_off" = original size.
//    Log "RENDER POOL" every 10 s: allocations, failures, peak use.
namespace chunk_order_lists {
std::atomic<uint32_t> max_count[4] = {}, full_frames[4] = {};
constexpr uint32_t kCount[4] = {0x8370D768, 0x8370D774, 0x8370D778, 0x8370D77C};
uint32_t kMax[4] = {2500, 1500, 512, 256};  // [0] updated from sr_render_list_cap()
}
namespace render_pool {
constexpr uint32_t kPool = 0x827DDD34;
std::atomic<uint64_t> allocs{0}, fails{0};
uint32_t peak = 0, frames = 0, full_frames = 0;
bool grown = false;
}
extern "C" void __imp__sub_82194B78(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_82194B78) {
    __imp__sub_82194B78(ctx, base);
    render_pool::allocs.fetch_add(1, std::memory_order_relaxed);
    if (!ctx.r3.u32) render_pool::fails.fetch_add(1, std::memory_order_relaxed);
}
extern "C" void __imp__sub_82194BD8(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_82194BD8) {
    using namespace render_pool;
    const uint32_t used_before = Rd32(base, kPool + 40);
    {
        // Section 17: adapt the air-view detail budget from the main list's fill this frame.
        const uint32_t main_fill_raw = Rd32(base, chunk_order_lists::kCount[0]);
        const uint32_t cap_now = sr_render_list_cap();
        chunk_order_lists::kMax[0] = cap_now;
        // Thresholds below are written for a 2500 list: scale the fill to that.
        const uint32_t main_fill = uint32_t(uint64_t(main_fill_raw) * 2500 / cap_now);
        float k = fade_hysteresis::g_budget_k.load(std::memory_order_relaxed);
        // Kept (slowly decaying) outside the air view too, so taking off again doesn't start from zero.
        if (!far_clip::g_air_view) k = k * 0.999f;
        else if (main_fill >= 2500) k = std::min(0.08f, std::max(k * 1.5f, k + 0.002f));  // overflowed: jump
        else if (main_fill >= 2200) k = std::min(0.08f, k * 1.05f + 0.0002f);
        else if (main_fill < 1800) k = k * 0.997f - 0.000005f;
        fade_hysteresis::g_budget_k.store(std::max(0.0f, k), std::memory_order_relaxed);
    }
    for (int l = 0; l < 4; ++l) {  // render list fill before the reset (section 16)
        const uint32_t c = Rd32(base, chunk_order_lists::kCount[l]);
        uint32_t m = chunk_order_lists::max_count[l].load(std::memory_order_relaxed);
        if (c > m) chunk_order_lists::max_count[l].store(c, std::memory_order_relaxed);
        if (c >= chunk_order_lists::kMax[l]) chunk_order_lists::full_frames[l].fetch_add(1, std::memory_order_relaxed);
    }
    const uint32_t size_before = Rd32(base, kPool + 56);
    __imp__sub_82194BD8(ctx, base);  // used = 0
    mesh_census::frames.fetch_add(1, std::memory_order_relaxed);
    native_mesh::AbTick();
    colour_split::Frame();
    if (rt_timing::Enabled()) { rt_timing::frames.fetch_add(1, std::memory_order_relaxed); rt_timing::MaybeLog(); }
    if (mesh_census::On()) timeline::Frame();
    batch_ab::Frame(base);
    const uint64_t saved_r3 = ctx.r3.u64;
    ++frames;
    peak = std::max(peak, used_before);
    if (size_before && used_before + 248 > size_before) ++full_frames;
    if (!grown) {
        grown = true;
        static const bool off = FileExists("render_pool_off");
        uint32_t mb = 16;
        if (FILE* f = std::fopen("render_inst_pool_mb.txt", "rb")) {
            if (std::fscanf(f, "%u", &mb) != 1) mb = 16;
            std::fclose(f);
        }
        mb = std::clamp(mb, 2u, 64u);
        const uint32_t old_size = Rd32(base, kPool + 56), old_base = Rd32(base, kPool + 4);
        const uint32_t new_size = mb << 20;
        uint32_t mem = 0;
        if (!off && old_base && old_size && old_size < new_size && Rd32(base, kPool + 40) == 0) {
            const uint64_t r[4] = {ctx.r4.u64, ctx.r5.u64, ctx.r6.u64, ctx.r7.u64};
            ctx.r3.u64 = new_size;
            ctx.r4.u64 = 0xFFFFFFFFull;
            ctx.r5.u64 = 0x1000;
            ctx.r6.u64 = 0x20000004;
            __imp__sub_82716618(ctx, base);
            mem = ctx.r3.u32;
            ctx.r4.u64 = r[0]; ctx.r5.u64 = r[1]; ctx.r6.u64 = r[2]; ctx.r7.u64 = r[3];
            if (mem) {
                std::memset(base + mem, 0, new_size);
                Wr32(base, kPool + 4, mem);
                if (Rd32(base, kPool + 64) == old_size) Wr32(base, kPool + 64, new_size);
                Wr32(base, kPool + 56, new_size);
            }
        }
        REXLOG_INFO("Render pool: render_inst_pool {} (was {} KB at {:08X}; now {} KB at {:08X}; +64 {:08X})",
                    off ? "left as is (render_pool_off)" : mem ? "enlarged" : "NOT enlarged", old_size >> 10,
                    old_base, Rd32(base, kPool + 56) >> 10, Rd32(base, kPool + 4), Rd32(base, kPool + 64));
    }
    ctx.r3.u64 = saved_r3;
    static auto next = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    const auto now = std::chrono::steady_clock::now();
    if (now >= next) {
        next = now + std::chrono::seconds(10);
        static int lines = 0;
        if (lines++ < 400)
            REXLOG_INFO("RENDER POOL 10 s: {} frames, {} instances ({:.0f}/frame), {} FAILED, {} frames full | peak use "
                        "{} KB of {} KB", frames, allocs.load(), frames ? double(allocs.load()) / frames : 0.0,
                        fails.load(), full_frames, peak >> 10, Rd32(base, kPool + 56) >> 10);
        if (lines < 400) {
            std::string ls;
            for (int l = 0; l < 4; ++l)
                ls += fmt::format(" [{} max: fullest {}, frames at max {}]", chunk_order_lists::kMax[l],
                                  chunk_order_lists::max_count[l].exchange(0), chunk_order_lists::full_frames[l].exchange(0));
            REXLOG_INFO("RENDER LISTS 10 s:{} | air budget: size/distance cut {:.4f}, skipped {}", ls,
                        fade_hysteresis::g_budget_k.load(), fade_hysteresis::g_budget_skipped.exchange(0));
        }
        allocs = 0; fails = 0; peak = 0; frames = 0; full_frames = 0;
    }
}

// 13. No occluder culling from the air. In air view the city is drawn by the
//    cell walk 82127390 (per cell: a list of objects -> fade test 82129550).
//    Before a cell (and via 82126C30 an object) is looked at, 82126B40(box,
//    occluder list: pointers, count at +128, r5 = prune) tests it against the
//    game's occluders (82125E30 per occluder) and skips it when hidden. The
//    occluders are made for street-level views: from above they hid ground
//    and objects 40-60 m in front of the camera (F9 object dump: fade 1000 m,
//    never tested for seconds) - black ground next to drawn buildings. In
//    air view the test still runs (list upkeep) but always answers "visible".
//    File "occluders_in_air" = original behaviour. Log "OCCLUDERS" every 10 s.
namespace occluders {
std::atomic<uint64_t> tests{0}, hidden{0}, overridden{0};
}
extern "C" void __imp__sub_82126B40(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_82126B40) {
    using namespace occluders;
    static const bool keep = !FileExists("occluders_air_experiment");  // 2026-10-02: experiment now opt-in
    __imp__sub_82126B40(ctx, base);
    tests.fetch_add(1, std::memory_order_relaxed);
    if (ctx.r3.u32 & 0xFF) {
        hidden.fetch_add(1, std::memory_order_relaxed);
        if (far_clip::g_air_view && !keep) {
            ctx.r3.u64 = 0;
            overridden.fetch_add(1, std::memory_order_relaxed);
        }
    }
    static std::atomic<int64_t> next{0};
    static thread_local uint32_t log_tick = 0;
    if ((++log_tick & 1023) != 0) return;
    const int64_t now = fade_hysteresis::NowMs();
    int64_t n = next.load(std::memory_order_relaxed);
    if (now - n >= 10000 && next.compare_exchange_strong(n, now) && n) {
        static int lines = 0;
        if (lines++ < 300)
            REXLOG_INFO("OCCLUDERS 10 s: {} tests, {} hidden by occluders, {} of them drawn anyway (air view)",
                        tests.exchange(0), hidden.exchange(0), overridden.exchange(0));
    }
}

// 14. No range sphere for the main view from the air. The city tree walk
//    (82129060 -> 82128510(-1) -> 82127F28) keeps a node only if 82126C30(box
//    min r3, box max r4, range r5, frustum r6, occluders r7) passes. For the
//    main view r5 = 0x838B4F10: +0 point, +12 radius (byte +32 = use it), +16
//    direction / +28 radius (byte +33 = cone test 8262BE78). From the air the
//    ground just in front of / below the camera fell outside it: those nodes'
//    objects were never even fade-tested (F9 dump: fade 1000 m, 40-60 m away,
//    "never tested") -> black ground. In air view the range test is skipped
//    for that record (frustum and each object's fade distance still apply).
//    File "range_sphere_in_air" = original. Log "RANGE SPHERE" every 5 s.
namespace range_sphere {
constexpr uint32_t kMain = 0x838B4F10;
std::atomic<uint64_t> calls{0}, skipped{0};
std::atomic<uint64_t> near_tests{0}, near_frustum_rejects{0}, near_occluder_rejects{0};
std::atomic<uint32_t> last_frustum{0};
}
extern "C" void __imp__sub_82126C30(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_82126C30) {
    using namespace range_sphere;
    static const bool keep = FileExists("range_sphere_in_air");
    calls.fetch_add(1, std::memory_order_relaxed);
    // Experiment (build nf1): main-view node test from the tree walk 82127F28
    // (r5 = 0: no light record, r7 = 0: no occluders) = frustum only. In air
    // view a frustum reject is counted and overridden. File
    // "frustum_in_air" = original.
    static const bool frustum_keep = !FileExists("frustum_air_experiment");  // 2026-10-02: experiment now opt-in
    const uint32_t lr = uint32_t(ctx.lr);
    // Diagnostic: per-object test from the cell walk (82127390, bl at 82127690)
    // for objects within 80 m of the camera in air view: how many does the
    // view frustum reject? And does the frustum's camera (frustum - 5092 +
    // 5044) match the real camera (0x827D9778 +44)?
    if (lr >= 0x82127660 && lr < 0x821276C0 && far_clip::g_air_view && ctx.r6.u32 && AirDiag()) {
        const uint32_t o = ctx.r3.u32;
        const float mx = 0.5f * (RdF(base, o) + RdF(base, o + 16)), my = 0.5f * (RdF(base, o + 4) + RdF(base, o + 20)),
                    mz = 0.5f * (RdF(base, o + 8) + RdF(base, o + 24));
        const float cx = RdF(base, 0x827D9778 + 44), cy = RdF(base, 0x827D9778 + 48), cz = RdF(base, 0x827D9778 + 52);
        const float d = std::sqrt((mx - cx) * (mx - cx) + (my - cy) * (my - cy) + (mz - cz) * (mz - cz));
        if (d < 80.0f) {
            const PPCContext saved = ctx;
            __imp__sub_82126C30(ctx, base);
            const bool pass = ctx.r3.u32 & 0xFF;
            // same object without occluders: frustum alone
            PPCContext c2 = saved;
            c2.r7.u64 = 0;
            __imp__sub_82126C30(c2, base);
            const bool frustum_pass = c2.r3.u32 & 0xFF;
            near_tests.fetch_add(1, std::memory_order_relaxed);
            if (!frustum_pass) near_frustum_rejects.fetch_add(1, std::memory_order_relaxed);
            else if (!pass) near_occluder_rejects.fetch_add(1, std::memory_order_relaxed);
            const uint32_t fr = saved.r6.u32;
            last_frustum = fr;
            return;
        }
    }
    const bool main_walk = lr >= 0x82127F28 && lr < 0x821281A8 && ctx.r5.u32 == 0 && ctx.r7.u32 == 0;
    if (main_walk && far_clip::g_air_view && !frustum_keep) {
        const PPCContext saved = ctx;
        __imp__sub_82126C30(ctx, base);
        if (!(ctx.r3.u32 & 0xFF)) {
            skipped.fetch_add(1, std::memory_order_relaxed);
            ctx = saved;
            ctx.r6.u64 = 0;  // no frustum
            __imp__sub_82126C30(ctx, base);
        }
    } else {
        (void)keep;
        __imp__sub_82126C30(ctx, base);
    }
    static std::atomic<int64_t> next{0};
    static thread_local uint32_t log_tick = 0;
    if ((++log_tick & 1023) != 0) return;
    const int64_t now = fade_hysteresis::NowMs();
    int64_t n = next.load(std::memory_order_relaxed);
    if (now - n >= 5000 && next.compare_exchange_strong(n, now) && n) {
        static int lines = 0;
        auto f = [&](uint32_t o) { return RdF(base, kMain + o); };
        const uint32_t fr = last_frustum.load();
        if (fr && lines < 300) {
            const float fx = RdF(base, fr - 5092 + 5044), fy = RdF(base, fr - 5092 + 5048), fz = RdF(base, fr - 5092 + 5052);
            const float cx = RdF(base, 0x827D9778 + 44), cy = RdF(base, 0x827D9778 + 48), cz = RdF(base, 0x827D9778 + 52);
            REXLOG_INFO("FRUSTUM CHECK 5 s: near (<80 m) object tests {}, rejected by the FRUSTUM {}, by occluders {} | "
                        "frustum planes {}, frustum camera {:.1f} {:.1f} {:.1f} vs camera {:.1f} {:.1f} {:.1f} (off by {:.1f} m) | "
                        "fov {:.1f}",
                        near_tests.exchange(0), near_frustum_rejects.exchange(0), near_occluder_rejects.exchange(0),
                        Rd32(base, fr), fx, fy, fz, cx, cy, cz,
                        std::sqrt((fx - cx) * (fx - cx) + (fy - cy) * (fy - cy) + (fz - cz) * (fz - cz)),
                        RdF(base, 0x827D9778 + 188));
        }
        if (lines++ < 300)
            REXLOG_INFO("RANGE SPHERE (air view {}): point {:.0f} {:.0f} {:.0f} radius {:.0f} use {} | dir {:.2f} {:.2f} {:.2f} "
                        "r2 {:.0f} use {} | +36 {:.1f} +40 {:.1f} +44 {:.1f} | camera {:.0f} {:.0f} {:.0f} | node tests {}, "
                        "frustum rejects overridden {}",
                        far_clip::g_air_view, f(0), f(4), f(8), f(12), int(base[kMain + 32]), f(16), f(20), f(24),
                        f(28), int(base[kMain + 33]), f(36), f(40), f(44), RdF(base, 0x827D9778 + 44),
                        RdF(base, 0x827D9778 + 48), RdF(base, 0x827D9778 + 52), calls.exchange(0), skipped.exchange(0));
    }
}

// 16. Nearest chunks first. Every object that passes the fade test is queued by
//    82195D78 into one of four FIXED lists (36-byte entries, reset per frame by
//    82194BD8): normal 0x838BFEC8 count 0x8370D768 max 2500, flag 0x100
//    0x838D5E58 / 0x8370D774 max 1500, type 1 0x838E3148 / 0x8370D778 max 512,
//    type 2 0x838E7948 / 0x8370D77C max 256. Past the max the object is
//    silently dropped. From the air (no PVS cell) a frame queues more than
//    2500 and the main-view walk 82128510(-1) visits chunks in list order -
//    low-detail "al" chunks first, the detailed chunks around the player last
//    - so the nearby ground and buildings were the ones dropped (black ground,
//    flickering buildings, worse with more far city). The main-view walk now
//    goes nearest chunk first (distance from the camera to the chunk tree's
//    root box, detailed before al on a tie), so an overflow only drops the
//    farthest objects. The lists live at fixed addresses read by ~25
//    functions, so they can't simply be made bigger.
//    File "chunk_order_original" = original order.
extern "C" void __imp__sub_82127F28(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_82128510(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_82128510) {
    static const bool original = FileExists("chunk_order_original");
    if (ctx.r3.s32 != -1 || original) {
        __imp__sub_82128510(ctx, base);
        return;
    }
    const PPCContext saved = ctx;
    const uint32_t n = std::min<uint32_t>(Rd32(base, 0x829A97E8), 64);
    const float cx = RdF(base, 0x827D9778 + 44), cz = RdF(base, 0x827D9778 + 52);
    std::array<std::pair<float, uint32_t>, 64> order;
    uint32_t used = 0;
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t chunk = Rd32(base, 0x829A97F8 + i * 4);
        if (!chunk) continue;
        if (!base[0x829ABE10 + base[chunk + 8] * 8]) continue;
        float d = 1e9f;
        const uint32_t root = Rd32(base, chunk + 236);
        if (root > 0x40000000 && root < 0xF0000000) {
            const float x0 = RdF(base, root + 0), z0 = RdF(base, root + 8);
            const float x1 = RdF(base, root + 16), z1 = RdF(base, root + 24);
            if (std::isfinite(x0) && std::isfinite(z1)) {
                const float dx = cx < x0 ? x0 - cx : cx > x1 ? cx - x1 : 0.0f;
                const float dz = cz < z0 ? z0 - cz : cz > z1 ? cz - z1 : 0.0f;
                d = std::sqrt(dx * dx + dz * dz);
            }
        }
        if (base[chunk + 20] & 0x80) d += 0.5f;  // detailed before al on a tie
        order[used++] = {d, i};
    }
    std::sort(order.begin(), order.begin() + used);
    const uint32_t blk = Rd32(base, Rd32(base, ctx.r13.u32) + 7692);
    for (uint32_t k = 0; k < used; ++k) {
        ctx = saved;
        ctx.r3.u64 = order[k].second;
        ctx.r4.u64 = blk + 5092;
        ctx.r5.u64 = 0x838B4F10;
        ctx.r6.u64 = Rd32(base, 0x8370D6FC);
        ctx.r7.s64 = -1;
        __imp__sub_82127F28(ctx, base);
    }
    ctx = saved;
}

// 18. Bigger main render list. The four render lists (section 16) are laid out
//    back to back: main 0x838BFEC8 (2500 x 36 bytes), then 1500 at 0x838D5E58,
//    512 at 0x838E3148, 256 at 0x838E7948 (ends 0x838E9D48). The three small
//    lists are moved to their own guest memory (their 21 address loads in the
//    generated code call sr_render_list_base, see _patches/render_lists_orig
//    for the untouched files), so the main list can use the whole area:
//    4768 entries instead of 2500 (the 4 cap checks call sr_render_list_cap).
//    File "render_list_original" = old addresses and 2500.
namespace render_list_move {
// REVERTED 2026-09-30 01:05: the generated code is back to the original (the
// moved lists broke the game: null calls from 825DA324 once the main list went
// past 2500 - something else is sized for 2500). Always the original layout.
bool Original() { return true; }
uint32_t Block() {
    static const uint32_t block = [] {
        const uint32_t size = 1500 * 36 + 512 * 36 + 256 * 36;
        const uint32_t b = REX_KERNEL_STATE()->memory()->SystemHeapAlloc(size + 64);
        REXLOG_INFO("Render lists: small lists moved to {:08X} ({} bytes), main list {} entries", b, size,
                    b ? 4768 : 2500);
        return b ? ((b + 15) & ~15u) : 0u;
    }();
    return block;
}
}  // namespace render_list_move
extern "C" uint32_t sr_render_list_base(int n) {
    using namespace render_list_move;
    static const uint32_t orig[4] = {0x838BFEC8, 0x838D5E58, 0x838E3148, 0x838E7948};
    if (n < 1 || n > 3) return orig[0];
    if (Original()) return orig[n];
    const uint32_t b = Block();
    if (!b) return orig[n];
    static const uint32_t off[4] = {0, 0, 1500 * 36, 1500 * 36 + 512 * 36};
    return b + off[n];
}
extern "C" uint32_t sr_render_list_cap() {
    using namespace render_list_move;
    if (Original() || !Block()) return 2500;
    return 4768;  // (0x838E9D48 - 0x838BFEC8) / 36
}

// 23. No hang when the character shadow buffer runs out. 822366B8 (vtable
//    0x8205C238 +48: a character's 300-byte render instance, from the
//    render-list prep jobs 82190268 -> 82237560) enters the instance's lock
//    (+268), then takes 12 x bones bytes from the "dupe anim shadows" mempool
//    (0x827DD130: +4 base, +40 used, +60 2 MB, reset after each colour pass
//    in 821929C8) through 82244748 -> 82633340. When that allocation fails it
//    returns 0 still holding the lock (loc_822366DC never leaves it), and the
//    next job that wants the instance that frame waits forever - then the
//    render thread for its job, then the game thread for "render done": the
//    whole game stops (a co-op host in a long firefight, 577 - 667 instances a
//    frame). Here the lock is left when the call returned 0 with this thread
//    one level deeper in it (+20 recursion, +24 owner; the early return of
//    822374c8 never takes it); the instance isn't drawn that frame, which is
//    what the game's own failure means. File "shadow_lock_fix_off" = original.
//    Log "SHADOW POOL" every 10 s when something happened: allocations,
//    failures, locks left, peak use.
//    822335E8 (vtable 0x8205C070 +48: the 392-byte instance of a character's
//    body and of each clothing piece of a player-type character, 82195330 /
//    82194FF8) has the same bug with its lock at +364 and its buffer at +360
//    (loc_8223360C returns 0 holding it): the same guard. Each of the first 20
//    failed allocations is logged at once with what was asked and what was
//    left, so a full pool, a page budget that ran out and a garbage request
//    can be told apart. (Co-op hosts of 4-5 players froze in 822335E8 on +364:
//    every player character draws one of these per clothing piece.)
namespace shadow_pool {
constexpr uint32_t kPool = 0x827DD130;
std::atomic<uint64_t> allocs{0}, fails{0}, leaks{0};
std::atomic<uint32_t> peak{0};
std::atomic<int64_t> next_log{0};  // steady_clock ms (the render workers call this: no lock)
void MaybeLog(uint8_t* base) {
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    int64_t due = next_log.load(std::memory_order_relaxed);
    if (due && (now < due || !next_log.compare_exchange_strong(due, now + 10000))) return;
    if (!due && !next_log.compare_exchange_strong(due, now + 10000)) return;   // (the first time: at once)
    const uint64_t f = fails.exchange(0), l = leaks.exchange(0), a = allocs.exchange(0);
    const uint32_t p = peak.exchange(0);
    static int lines = 0;
    if ((f || l) && lines++ < 400)
        REXLOG_INFO("SHADOW POOL 10 s: {} allocations, {} FAILED, {} locks left by 822366B8 / 822335E8 released | peak use {} KB of {} KB",
                    a, f, l, p >> 10, Rd32(base, kPool + 60) >> 10);
}
}  // namespace shadow_pool
extern "C" void __imp__sub_82633340(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_82633340) {  // a mempool's allocation: counted for the shadow pool only
    const uint32_t pool = ctx.r3.u32, size = ctx.r4.u32, lr = uint32_t(ctx.lr);
    __imp__sub_82633340(ctx, base);
    if (pool != shadow_pool::kPool) return;
    shadow_pool::allocs.fetch_add(1, std::memory_order_relaxed);
    if (!ctx.r3.u32) {
        shadow_pool::fails.fetch_add(1, std::memory_order_relaxed);
        static std::atomic<int> told{0};
        if (told.fetch_add(1, std::memory_order_relaxed) < 20)
            REXLOG_INFO("SHADOW POOL allocation FAILED: {} bytes asked from {:08X} | used {} KB, committed {} KB, max {} KB, flags {:04X} | page budget {} / {}",
                        size, lr, Rd32(base, pool + 40) >> 10, Rd32(base, pool + 64) >> 10, Rd32(base, pool + 60) >> 10,
                        uint32_t(Rd32(base, pool + 52) & 0xFFFF), Rd32(base, 0x838B4D40 + 36), Rd32(base, 0x838B4D40 + 32));
    }
    const uint32_t used = Rd32(base, pool + 40);
    uint32_t p = shadow_pool::peak.load(std::memory_order_relaxed);
    while (used > p && !shadow_pool::peak.compare_exchange_weak(p, used, std::memory_order_relaxed)) {}
}
extern "C" void __imp__RtlLeaveCriticalSection(PPCContext& __restrict ctx, uint8_t* base);
// The guard of both: the lock (+lock_off) left when the call returned 0 with no
// buffer (+buf_off) and this thread one level deeper in it.
template <void (*Orig)(PPCContext&, uint8_t*)>
void ShadowLockGuard(PPCContext& ctx, uint8_t* base, uint32_t lock_off, uint32_t buf_off) {
    static const bool off = FileExists("shadow_lock_fix_off");
    const uint32_t inst = ctx.r3.u32, cs = inst + lock_off;
    const auto* thread = rex::system::XThread::GetCurrentThread();
    const uint32_t me = thread ? thread->guest_object() : 0;
    const uint32_t owner_before = Rd32(base, cs + 24), depth_before = Rd32(base, cs + 20);
    Orig(ctx, base);
    if (off || ctx.r3.u32 || !me || Rd32(base, inst + buf_off) || Rd32(base, cs + 24) != me ||
        Rd32(base, cs + 20) != (owner_before == me ? depth_before + 1 : 1u)) {
        if (shadow_pool::fails.load(std::memory_order_relaxed)) shadow_pool::MaybeLog(base);
        return;
    }
    const uint64_t r3 = ctx.r3.u64;  // (0: not drawn this frame)
    ctx.r3.u64 = cs;
    __imp__RtlLeaveCriticalSection(ctx, base);
    ctx.r3.u64 = r3;
    shadow_pool::leaks.fetch_add(1, std::memory_order_relaxed);
    shadow_pool::MaybeLog(base);
}
extern "C" void __imp__sub_822366B8(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_822366B8) { ShadowLockGuard<__imp__sub_822366B8>(ctx, base, 268, 264); }
extern "C" void __imp__sub_822335E8(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_822335E8) { ShadowLockGuard<__imp__sub_822335E8>(ctx, base, 364, 360); }
