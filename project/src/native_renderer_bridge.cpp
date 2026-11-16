#include "native_renderer_bridge.h"

#include <rex/logging.h>

#include <Windows.h>

#include <atomic>
#include <bit>
#include <immintrin.h>
#include <malloc.h>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace native_renderer_bridge {

std::atomic<bool> g_gate_enabled{false};
std::atomic<bool> g_kick_lock{true};

namespace {

using SubmitFn = int (*)(const void* record, uint32_t size);
using BoundaryFn = int (*)(uint64_t completed_frame_id);
using SubmitOwnedFn = int (*)(const void* record, uint32_t size, uint32_t* out_record_id);
using MasksFn = int (*)(uint32_t vs_guest, uint32_t ps_guest, uint32_t decl_guest, uint64_t* out_masks,
                        uint32_t* out_generation);
using BatchFn = int (*)(const void* blob, uint32_t bytes, uint32_t count);
using MetaBatchFn = int (*)(const void* batch, uint32_t batch_size);

struct Api {
  SubmitFn submit = nullptr;
  BoundaryFn boundary = nullptr;
  SubmitFn submit_v3 = nullptr;
  MasksFn masks = nullptr;
  SubmitOwnedFn submit_owned = nullptr;
  BatchFn submit_v3_batch = nullptr;  // EXEC8 x8c RexNativeRendererSubmitDrawRecordsV3Batch (optional)
  MetaBatchFn submit_meta = nullptr;  // FRAME9 RexNativeRendererSubmitDrawMetadataV1Batch (optional)
};

struct MeshContext {
  uint32_t pass = 0xFFFFFFFFu;
  uint32_t mesh = 0;
  uint32_t instance = 0;
};

thread_local MeshContext g_mesh;
thread_local uint32_t g_instance;
thread_local uint32_t t_writer_depth = 0;
thread_local uint64_t t_seq_frame = 0;
thread_local uint32_t t_seq = 0;
std::once_flag g_resolve_once;
Api g_api;
std::atomic<uint64_t> g_frame_id{1};
std::atomic<uint64_t> g_gate_check_at{0};
// Per log window (600 frames).
std::atomic<uint64_t> g_submitted{0}, g_accepted{0}, g_rejected{0}, g_errors{0};
std::atomic<uint64_t> g_writer_calls{0}, g_no_draw{0}, g_backscan{0}, g_not_found{0}, g_bad_device{0},
    g_parse_abort{0};
// Stream mirror (frame4, M3 constants): what the device's own command stream wrote into the constant
// registers since the last loss of track. Per log window.
std::atomic<uint64_t> g_gap_scanned{0}, g_gap_lost{0}, g_gap_dwords{0}, g_t0_const_words{0}, g_t0_fetch_words{0},
    g_set_const{0}, g_load_alu{0}, g_load_alu_words{0}, g_set_shader_consts{0}, g_ib_calls{0}, g_ib_skipped{0},
    g_mirror_resets{0}, g_rec_vs_stream{0}, g_rec_ps_stream{0}, g_rec_fetch_stream{0}, g_rec_with_stream{0},
    g_gap_draws{0}, g_kick_scans{0};
// Kick walks (frame5): path 9 = draw packets of a submitted segment that no hooked writer recorded. Per log window.
std::atomic<uint64_t> g_kick_dwords{0}, g_kick_bad_walk{0}, g_kick_too_big{0}, g_kick_no_device{0},
    g_kick_dev_r3{0}, g_kick_dev_thread{0}, g_kick_dev_differs{0}, g_kick_in_writer{0}, g_kick_writer_hits{0},
    g_kick_mirror_resets{0}, g_kick_ends_cleared{0}, g_writer_kicked{0},
    g_writer_dropped_kicked{0}, g_kick_pred_skipped{0}, g_kick_pred_run{0}, g_kick_overlap{0},
    g_kick_nested_bad{0}, g_kick_ends_expired{0}, g_tile_selects{0}, g_nop_headers{0}, g_merged{0}, g_kick_ib_skipped{0},
    g_merged_state_kick{0}, g_merged_shader_kick{0}, g_held_fallback{0};
// frame6: constant groups / fetch units at the kick merge (writer mirror knew all words, kick mirror knew all
// words, composed per word, left as written), held FIFO queueing / expiry / nested flushes.
std::atomic<uint64_t> g_mg_writer_full{0}, g_mg_kick_full{0}, g_mg_composed{0}, g_mg_kick_partial_left{0},
    g_held_queued{0}, g_held_expired_take{0}, g_held_nested_flush{0}, g_merged_vs_kick{0}, g_merged_ps_kick{0};
// Verification generation: bumped whenever verification is switched on, so held records and per-thread mirrors
// from an earlier enabled period are never reused.
std::atomic<uint32_t> g_gate_gen{1};
// Kick merge policy (frame6c): the kick mirror's CP-order value wins wherever it knows a register / a whole constant
// group; dist file native_bridge_writer_wins (read when verification is switched on) restores the frame5d policy
// (writer-mirror values win). Writer/kick disagreements where both know a value are counted either way.
std::atomic<bool> g_writer_wins{false};
// NATIVE7 s1: kick mirror policy. Default (verification on): ONE process-wide kick mirror, walked under g_submit_mu, which
// the callers of OnKick / OnTileSelect (stubs.cpp 825D3218 / 825DF0B8) hold across the walk AND SubmitCommandStream, so
// the mirror sees the segments in exactly the CP's order whichever thread kicks. dist native_bridge_thread_kick = old
// per-thread mirrors (A/B).
std::atomic<bool> g_thread_kick{false};
std::recursive_mutex g_submit_mu;
std::atomic<uint32_t> g_last_kick_tid{0};
std::atomic<uint64_t> g_kick_thread_switch{0}, g_kick_shared{0};
// v3 records (dist file native_bridge_v3, default off).
std::atomic<bool> g_v3_on{false};
// FRAME8: lean capture (dist native_frame_lean), lean without v3 values (native_frame_lean_novalues), cost profile off
// (native_frame_noprof); all read when the bridge is switched on.
std::atomic<bool> g_lean{false}, g_lean_values{true}, g_prof_on{true};
// native_frame_lean_nohash: lean records without the 224 constant/fetch hashes (production shape: hashes only serve
// verification; with native_bridge_verify on they would show as constant mismatches).
std::atomic<bool> g_lean_nohash{false};
// AGENT_NATIVE29B: capture v4-full (dist native_capture_full, read when the bridge is switched on; native_capture_full.inc).
std::atomic<bool> g_capture_full{false};
std::atomic<bool> g_capture_own{false};  // AGENT_NATIVE29B step 2 owned runs (dist native_capture_own)
std::atomic<bool> g_capture_own_tiled{false};
std::atomic<bool> g_capture_own_pred{false};  // MAIN31: dist native_capture_own_pred (own predicated draws, predicated run marker)  // MAIN31: dist native_capture_own_tiled (own draws in the deferred-tiling layout)
// FRAME9 (dist native_frame_meta): writer metadata batches per submission (see the header). Without native_frame_lean
// = production shape (no kick walk, no records, no submit lock); with lean the kick walk also checks every batch
// record against the record it builds (meta check counters).
std::atomic<bool> g_meta{false}, g_meta_only{false};
// FRAME9: lean kick records submitted as v3 batches (dist native_frame_nobatch = one call per record, A/B).
std::atomic<bool> g_batch_on{true};
std::atomic<uint64_t> g_v3_unpublished{0}, g_v3_values{0}, g_v3_no_values{0}, g_v3_value_words{0}, g_v3_shadow_groups{0},
    g_v3_accepted{0}, g_v3_rejected{0}, g_v3_off{0}, g_load_alu_virtual{0};
// M3b verify markers (dist file native_frame_exec_marker, default off, read at enable; needs native_bridge_v3): after a
// mesh pass-8 825D2478 call from 8223F3C4 with one DRAW_INDX, a type-3 NOP marker {magic, id} is appended after the
// call's packets (it refers back to that draw). The kick walk submits the merged v3 record of the draw before a marker as
// an SDK marker record (VERIFY_ONLY: the original draw still runs) and writes the returned id into the marker before the
// segment is handed to the GPU.
std::atomic<bool> g_marker_on{false};
std::atomic<uint64_t> g_mk_inserted{0}, g_mk_skip_kick{0}, g_mk_skip_layout{0}, g_mk_skip_shape{0}, g_mk_skip_space{0},
    g_mk_orphan{0}, g_mk_seen{0}, g_mk_accepted{0}, g_mk_refused{0}, g_mk_bad{0}, g_mk_replay{0}, g_mk_novalues{0};
constexpr uint32_t kMarkerMagic = 0x4E524433u;
std::atomic<uint64_t> g_dis_rt{0}, g_dis_state{0}, g_dis_const{0}, g_dis_state_regs[kNativeStateRegCount] = {};
std::atomic<uint64_t> g_p9_tag[16] = {};
// CP bin state (SET_BIN_MASK / SET_BIN_SELECT) in submission order: kick walks and the tile-select words
// forwarded by 825DF0B8. Initial values as the SDK command processor's.
std::atomic<uint64_t> g_bin_mask{0xFFFFFFFFull}, g_bin_select{0xFFFFFFFFull};
std::atomic<uint64_t> g_held_capped{0};  // writer records not held because the held map is over its cap

// Device shadow offsets (825D2478 / framecap, 2026-10-23).
constexpr uint32_t kDevRingWrite = 40;     // command buffer write pointer (last written dword)
constexpr uint32_t kDevFetch = 1024;       // 32 fetch slots x 6 dwords -> SHADER_CONSTANT_FETCH_00_0
constexpr uint32_t kDevVsConst = 1792;     // 256 float4 -> SHADER_CONSTANT_000_X
constexpr uint32_t kDevPsConst = 5888;     // 256 float4 -> SHADER_CONSTANT_256_X
constexpr uint32_t kDevSurface = 10240, kDevColorMask = 10332, kDevDepthControl = 10420,
                   kDevModeControl = 10452;
constexpr uint32_t kDevVertexDecl = 11552, kDevIndexBuffer = 12180, kDevPixelShader = 12432,
                   kDevVertexShader = 12436;

inline const uint8_t* GuestPtr(const uint8_t* base, uint32_t a) {
  return base + a + (a >= 0xE0000000u ? 0x1000u : 0u);
}
inline uint32_t G32(const uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, GuestPtr(base, a), 4);
  return _byteswap_ulong(v);
}
// Guest big-endian store (M3b markers only).
inline void W32(const uint8_t* base, uint32_t a, uint32_t value) {
  const uint32_t v = _byteswap_ulong(value);
  std::memcpy(const_cast<uint8_t*>(GuestPtr(base, a)), &v, 4);
}

// Same function as RexNativeBridgeHashWords in the SDK's native_frame_bridge.h.
inline uint32_t HashWords(const uint32_t* words, uint32_t count) {
  uint64_t h = 0xCBF29CE484222325ull;
  for (uint32_t i = 0; i + 1 < count; i += 2) {
    h ^= uint64_t(words[i]) | (uint64_t(words[i + 1]) << 32);
    h *= 0x100000001B3ull;
    h ^= h >> 29;
  }
  return uint32_t(h ^ (h >> 32));
}
// Hash n big-endian guest words starting at a (device shadow).
inline uint32_t HashGuest(const uint8_t* base, uint32_t a, uint32_t n) {
  uint32_t w[16];
  const uint8_t* p = GuestPtr(base, a);
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t v;
    std::memcpy(&v, p + i * 4, 4);
    w[i] = _byteswap_ulong(v);
  }
  return HashWords(w, n);
}

void ClearHeld();

// NATIVE7 s3: cost profile of the bridge (only while it is on): QPC ticks per section, logged every 600 frames.
std::atomic<uint64_t> g_t_runs{0}, g_t_runsdk{0};  // MAIN31: CapApplyRuns total / its SDK batch calls
std::atomic<uint64_t> g_t_ctor{0}, g_t_dtor{0}, g_t_kick{0}, g_t_apply{0}, g_t_sdk{0}, g_t_lock{0}, g_t_tile{0},
    g_n_ctor{0}, g_n_dtor{0}, g_n_kick{0}, g_n_sdk{0}, g_n_lock{0};
inline uint64_t Ticks() {
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return uint64_t(t.QuadPart);
}
struct TickScope {
  std::atomic<uint64_t>& acc;
  std::atomic<uint64_t>& cnt;
  uint64_t t0;
  TickScope(std::atomic<uint64_t>& a, std::atomic<uint64_t>& n)
      : acc(a), cnt(n), t0(g_prof_on.load(std::memory_order_relaxed) ? Ticks() : 0) {}
  ~TickScope() {
    if (!t0) return;
    acc.fetch_add(Ticks() - t0, std::memory_order_relaxed);
    cnt.fetch_add(1, std::memory_order_relaxed);
  }
};

// Called at the game frame boundary. Touch the filesystem at most once a second; the draw path only
// reads the resulting atomic gate.
bool VerificationEnabled() {
  const uint64_t now = GetTickCount64();
  uint64_t check_at = g_gate_check_at.load(std::memory_order_acquire);
  if (now < check_at) {
    return g_gate_enabled.load(std::memory_order_acquire);
  }
  const DWORD attributes = GetFileAttributesW(L"native_bridge_verify");
  // NATIVE7 s2: dist native_frame_exec_dry = the record path without verification (SDK dry mode; v3 on).
  const DWORD dry_attr = GetFileAttributesW(L"native_frame_exec_dry");
  const bool dry = dry_attr != INVALID_FILE_ATTRIBUTES && (dry_attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
  const DWORD meta_attr = GetFileAttributesW(L"native_frame_meta");
  const bool meta = meta_attr != INVALID_FILE_ATTRIBUTES && (meta_attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
  const bool enabled =
      (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) || dry || meta;
  if (g_gate_check_at.compare_exchange_strong(check_at, now + 1000, std::memory_order_acq_rel)) {
    // A new generation is published before the gate opens, so nothing recorded under the open gate can carry an
    // older generation; the bin state starts over like the CP's.
    const bool was_enabled = g_gate_enabled.load(std::memory_order_acquire);
    if (enabled && !was_enabled) {
      g_gate_gen.fetch_add(1, std::memory_order_acq_rel);
      {
        const DWORD ww = GetFileAttributesW(L"native_bridge_writer_wins");
        g_writer_wins.store(ww != INVALID_FILE_ATTRIBUTES && (ww & FILE_ATTRIBUTE_DIRECTORY) == 0,
                            std::memory_order_relaxed);
        const DWORD tk = GetFileAttributesW(L"native_bridge_thread_kick");
        g_thread_kick.store(tk != INVALID_FILE_ATTRIBUTES && (tk & FILE_ATTRIBUTE_DIRECTORY) == 0,
                            std::memory_order_relaxed);
        const DWORD v3 = GetFileAttributesW(L"native_bridge_v3");
        g_v3_on.store((v3 != INVALID_FILE_ATTRIBUTES && (v3 & FILE_ATTRIBUTE_DIRECTORY) == 0) || dry,
                      std::memory_order_relaxed);
        auto has = [](const wchar_t* f) {
          const DWORD a = GetFileAttributesW(f);
          return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
        };
        g_lean.store(has(L"native_frame_lean"), std::memory_order_relaxed);
        g_lean_values.store(!has(L"native_frame_lean_novalues"), std::memory_order_relaxed);
        g_prof_on.store(!has(L"native_frame_noprof"), std::memory_order_relaxed);
        g_lean_nohash.store(has(L"native_frame_lean_nohash"), std::memory_order_relaxed);
        g_batch_on.store(!has(L"native_frame_nobatch"), std::memory_order_relaxed);
        g_capture_full.store(has(L"native_capture_full"), std::memory_order_relaxed);  // AGENT_NATIVE29B
        g_capture_own.store(has(L"native_capture_full") && has(L"native_capture_own"), std::memory_order_relaxed);
        g_capture_own_tiled.store(has(L"native_capture_own_tiled"), std::memory_order_relaxed);  // MAIN31
        g_capture_own_pred.store(has(L"native_capture_own_pred"), std::memory_order_relaxed);  // MAIN31
        g_meta.store(meta, std::memory_order_relaxed);
        g_meta_only.store(meta && !g_lean.load(std::memory_order_relaxed), std::memory_order_relaxed);
        g_kick_lock.store(!g_meta_only.load(std::memory_order_relaxed), std::memory_order_relaxed);
        const DWORD mk = GetFileAttributesW(L"native_frame_exec_marker");
        g_marker_on.store(mk != INVALID_FILE_ATTRIBUTES && (mk & FILE_ATTRIBUTE_DIRECTORY) == 0 &&
                              g_v3_on.load(std::memory_order_relaxed),
                          std::memory_order_relaxed);
      }
      g_bin_mask.store(0xFFFFFFFFull, std::memory_order_relaxed);
      g_bin_select.store(0xFFFFFFFFull, std::memory_order_relaxed);
    }
    g_gate_enabled.store(enabled, std::memory_order_release);
    if (enabled && !was_enabled) {
      REXLOG_INFO("NATIVE FRAME BRIDGE: verification enabled (v2 draw records){}{}{}{}",
                  g_meta_only.load(std::memory_order_relaxed)
                      ? ", DRAW-META ONLY (FRAME9: writer metadata batches, no kick walk / records / submit lock)"
                      : g_meta.load(std::memory_order_relaxed) ? ", draw-meta batches + kick-walk check (FRAME9)" : "",
                  g_lean.load(std::memory_order_relaxed) ? ", LEAN capture (FRAME8)" : "",
                  g_lean.load(std::memory_order_relaxed) && !g_lean_values.load(std::memory_order_relaxed) ? " without v3 values" : "",
                  g_prof_on.load(std::memory_order_relaxed) ? "" : ", cost profile off");
    } else if (!enabled && was_enabled) {
      ClearHeld();
      REXLOG_INFO("NATIVE FRAME BRIDGE: verification disabled");
    }
    return enabled;
  }
  return g_gate_enabled.load(std::memory_order_acquire);
}

const Api& GetApi() {
  std::call_once(g_resolve_once, [] {
    HMODULE gpu = GetModuleHandleW(L"rexgpu-xenos.dll");
    if (gpu) {
      g_api.submit = reinterpret_cast<SubmitFn>(GetProcAddress(gpu, "RexNativeRendererSubmitDrawRecordV2"));
      g_api.boundary = reinterpret_cast<BoundaryFn>(GetProcAddress(gpu, "RexNativeRendererFrameBoundaryV1"));
      g_api.submit_v3 = reinterpret_cast<SubmitFn>(GetProcAddress(gpu, "RexNativeRendererSubmitDrawRecordV3"));
      g_api.masks = reinterpret_cast<MasksFn>(GetProcAddress(gpu, "RexNativeRendererGetConstantMasks"));
      g_api.submit_owned =
          reinterpret_cast<SubmitOwnedFn>(GetProcAddress(gpu, "RexNativeRendererSubmitOwnedDrawRecordV3"));
      g_api.submit_v3_batch =
          reinterpret_cast<BatchFn>(GetProcAddress(gpu, "RexNativeRendererSubmitDrawRecordsV3Batch"));
      g_api.submit_meta =
          reinterpret_cast<MetaBatchFn>(GetProcAddress(gpu, "RexNativeRendererSubmitDrawMetadataV1Batch"));
    }
    if (g_api.submit) {
      REXLOG_INFO("NATIVE FRAME BRIDGE: draw-record v2 intake found{}{}{}{}",
                  g_api.boundary ? " with frame boundaries" : " (no frame-boundary export)",
                  g_api.submit_v3 && g_api.masks ? ", v3 intake + constant masks found" : ", no v3 intake",
                  g_api.submit_v3_batch ? ", v3 batch intake" : ", no v3 batch intake",
                  g_api.submit_meta ? ", draw-metadata batch intake" : ", no draw-metadata batch intake");
    } else {
      REXLOG_INFO("NATIVE FRAME BRIDGE: SDK v2 intake unavailable (older rexgpu-xenos.dll); emulated rendering unchanged");
    }
  });
  return g_api;
}

bool PlausibleDevice(const uint8_t* base, uint32_t dev) {
  // Recording devices are heap objects (2026-10-23 captures: 40001E00, 40009480).
  if (dev < 0x40000000u || dev >= 0x80000000u || (dev & 3)) return false;
  const uint32_t ring = G32(base, dev + kDevRingWrite);
  return ring >= 0x80000000u && !(ring & 3);
}

// Constant registers as last written by this thread's device command stream (type-0 / type-1 writes,
// SET_CONSTANT, SET_CONSTANT2, LOAD_ALU_CONSTANT, nested INDIRECT_BUFFERs). Verification only: built by
// reading the packets the engine already wrote, never written back.
struct StreamMirror {
  uint32_t device = 0;
  uint32_t last_after = 0;  // device +40 after the previous writer call (0 = track lost)
  uint32_t alu[2048];       // registers 0x4000-0x47FF
  uint32_t fetch[192];      // registers 0x4800-0x48BF
  uint32_t ctx[0x1000];     // registers 0x2000-0x2FFF (render state)
  uint32_t boolloop[40];    // registers 0x4900-0x4927
  uint64_t alu_valid[32];
  uint64_t fetch_valid[3];
  uint64_t ctx_valid[64];
  uint64_t boolloop_valid;
  // Last IM_LOAD(_IMMEDIATE) per stage: ucode guest address (0 = immediate, the SDK skips the check).
  uint32_t shader_addr[2];
  bool shader_known[2];
  uint64_t bin_mask = 0xFFFFFFFFull, bin_select = 0xFFFFFFFFull;
  void Reset() {
    shader_known[0] = shader_known[1] = false;
    std::memset(alu_valid, 0, sizeof(alu_valid));
    std::memset(fetch_valid, 0, sizeof(fetch_valid));
    std::memset(ctx_valid, 0, sizeof(ctx_valid));
    boolloop_valid = 0;
  }
  // Value of a context / bool / loop register if the stream wrote it since the last reset.
  bool Reg(uint32_t reg, uint32_t& value) const {
    if (reg >= 0x2000u && reg < 0x3000u) {
      const uint32_t i = reg - 0x2000u;
      if (!((ctx_valid[i >> 6] >> (i & 63)) & 1)) return false;
      value = ctx[i];
      return true;
    }
    if (reg >= 0x4900u && reg < 0x4928u) {
      const uint32_t i = reg - 0x4900u;
      if (!((boolloop_valid >> i) & 1)) return false;
      value = boolloop[i];
      return true;
    }
    return false;
  }
  void Write(uint32_t reg, uint32_t value) {
    if (reg >= 0x4000u && reg < 0x4800u) {
      const uint32_t i = reg - 0x4000u;
      alu[i] = value;
      alu_valid[i >> 6] |= 1ull << (i & 63);
    } else if (reg >= 0x4800u && reg < 0x48C0u) {
      const uint32_t i = reg - 0x4800u;
      fetch[i] = value;
      fetch_valid[i >> 6] |= 1ull << (i & 63);
    } else if (reg >= 0x2000u && reg < 0x3000u) {
      const uint32_t i = reg - 0x2000u;
      ctx[i] = value;
      ctx_valid[i >> 6] |= 1ull << (i & 63);
    } else if (reg >= 0x4900u && reg < 0x4928u) {
      const uint32_t i = reg - 0x4900u;
      boolloop[i] = value;
      boolloop_valid |= 1ull << i;
    }
  }
  bool AluValid(uint32_t i) const { return (alu_valid[i >> 6] >> (i & 63)) & 1; }
  bool FetchValid(uint32_t i) const { return (fetch_valid[i >> 6] >> (i & 63)) & 1; }
};
thread_local StreamMirror* t_mirror = nullptr;
thread_local uint32_t t_mirror_gen = 0;

StreamMirror& Mirror(uint32_t dev) {
  if (!t_mirror) {
    t_mirror = new StreamMirror();  // one per recording thread, lives for the process (verification only)
    t_mirror->Reset();
  }
  const uint32_t gen = g_gate_gen.load(std::memory_order_acquire);
  if (t_mirror_gen != gen) {
    t_mirror_gen = gen;
    t_mirror->device = 0;  // forces the reset below
  }
  if (t_mirror->device != dev) {
    t_mirror->device = dev;
    t_mirror->last_after = 0;
    t_mirror->Reset();
    g_mirror_resets.fetch_add(1, std::memory_order_relaxed);
  }
  return *t_mirror;
}

// Physical (GPU) address -> the guest view the CPU uses for physical memory.
inline uint32_t PhysView(uint32_t gpu) { return 0xA0000000u | (gpu & 0x1FFFFFFFu); }
// Engine-side CPU address -> GPU physical (identical to NativeBridgeGpu in the SDK's verify code:
// 0xE0000000-range pages carry +0x1000).
inline uint32_t GpuAddr(uint32_t a) { return (a & 0x1FFFFFFFu) + (a >= 0xE0000000u ? 0x1000u : 0u); }

// What the writer's own stream mirror knew when it made a record (the rest came from the device shadow):
// the kick merge keeps these and fills only the remainder from the kick mirror.
struct WriterPartial {
  uint8_t kind;    // 0 VS constant group, 1 PS constant group, 2 fetch unit
  uint8_t index;   // group (0-63) / unit (0-95)
  uint16_t known;  // words the writer's mirror knew
  uint32_t w[16];  // writer-time words: mirror value where known, device shadow otherwise
};
struct WriterExtra {
  uint32_t state[kNativeStateRegCount];
  uint64_t state_known = 0;
  uint64_t vs_full = 0, ps_full = 0;   // constant groups whose 16 words all came from the writer's mirror
  uint64_t fetch_full[2] = {};         // fetch units (96) whose 2 words both came from the writer's mirror
  std::vector<WriterPartial> partial;  // groups / units the writer's mirror knew only partly, and (v3) every group of
                                       // the writer-time published constant masks, with their words
  uint32_t rt_known = 0;               // 1 surface 2 color mask 4 depth 8 mode
};

thread_local bool t_kick_walk = false;  // the current ApplyPackets runs for a kick walk (no mirror counters)

// v3 values captured at a draw's execution in the kick walk (parallel to KickState::out).
struct V3Info {
  uint8_t kind = 0;  // 0 send v2, 1 v3 without values, 2 v3 with values
  uint64_t vs_mask = 0, ps_mask = 0;
  uint32_t generation = 0, header = 0, offset = 0, words = 0;
};

// Per recording thread: the segments this thread kicked (825D3218) and the packet ends its writers recorded
// that no executed kick has consumed yet.
struct KickState {
  StreamMirror mirror;  // state as the kicked segments set it, continuous across kicks
  std::vector<DrawRecordV2> hits;      // held writer records taken by the current walk (as the writer made them)
  std::vector<DrawRecordV2> out;       // path-9 records of the current walk (submitted when it succeeds)
  std::vector<V3Info> v3;              // per out record
  std::vector<uint32_t> v3_words;      // value words of the current walk
  std::vector<uint32_t> inflight_p9;   // path-9 ends emitted by kicks inside the current outer writer
  uint32_t epoch = 0;                  // kicks on this thread
  uint32_t gen = 0;                    // verification generation the mirror state belongs to
  std::vector<std::pair<uint32_t, uint32_t>> nested;  // GPU ranges of the nested IBs the current walk reached
  struct Range {
    uint32_t from, to, epoch;  // GPU [from, to) of a successfully walked kick
  } recent[16] = {};
};
thread_local KickState* t_kick = nullptr;
// Process-wide kick mirror (see g_thread_kick); only touched by OnKick under g_submit_mu.
StreamMirror* g_shared_kick_mirror = nullptr;
uint32_t g_shared_kick_gen = 0;

// Writer records held until their packet's first execution in a kicked segment (any thread: command
// buffers recorded on worker threads run through INDIRECT_BUFFERs of the render thread's segments). Keyed by
// GPU packet end; merged with the kick mirror's CP-order state and submitted by the kick walk. A later
// execution of the same packet gets path 9.
struct Held {
  DrawRecordV2 r;
  WriterExtra x;
  uint32_t gen;  // verification generation of the writer call
};
// One FIFO per packet end (a packet address can be recorded again before its earlier recording executed):
// executions consume the oldest record, like the SDK's pending queue per address.
struct HeldShard {
  std::mutex mu;
  std::unordered_map<uint32_t, std::deque<Held>> map;
};
HeldShard g_held[16];
inline HeldShard& Shard(uint32_t end) { return g_held[(end >> 2) & 15]; }
std::atomic<uint64_t> g_held_left{0};  // records held after the last expiry pass
constexpr uint64_t kHeldCap = 1u << 20;   // diagnostic cap (normal: a few thousand)
void Hold(uint32_t end, const DrawRecordV2& r, WriterExtra&& x, uint32_t gen) {
  if (g_held_left.load(std::memory_order_relaxed) > kHeldCap) {
    g_held_capped.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  HeldShard& h = Shard(end);
  std::lock_guard<std::mutex> lock(h.mu);
  std::deque<Held>& q = h.map[end];
  if (!q.empty()) g_held_queued.fetch_add(1, std::memory_order_relaxed);
  q.emplace_back();
  q.back().r = r;
  q.back().x = std::move(x);
  q.back().gen = gen;
}
bool Take(uint32_t end, Held& out) {
  HeldShard& h = Shard(end);
  const uint64_t now = g_frame_id.load(std::memory_order_relaxed);
  std::lock_guard<std::mutex> lock(h.mu);
  auto it = h.map.find(end);
  if (it == h.map.end()) return false;
  std::deque<Held>& q = it->second;
  // Records older than the expiry age never executed (or from an earlier verification period): drop them instead
  // of handing them to this execution.
  const uint32_t gen = g_gate_gen.load(std::memory_order_relaxed);
  while (!q.empty() && (q.front().r.frame_id + 8 < now || q.front().gen != gen)) {
    q.pop_front();
    g_held_expired_take.fetch_add(1, std::memory_order_relaxed);
  }
  if (q.empty()) {
    h.map.erase(it);
    return false;
  }
  out = std::move(q.front());
  q.pop_front();
  if (q.empty()) h.map.erase(it);
  return true;
}
void ClearHeld() {
  for (HeldShard& h : g_held) {
    std::lock_guard<std::mutex> lock(h.mu);
    h.map.clear();
  }
}

KickState& Kick() {
  if (!t_kick) {
    t_kick = new KickState();  // one per thread, lives for the process (verification only)
    t_kick->mirror.Reset();
    t_kick->out.reserve(256);
  }
  const uint32_t gen = g_gate_gen.load(std::memory_order_acquire);
  if (t_kick->gen != gen) {
    // Verification was switched off and on again: nothing this thread saw before is current.
    KickState& k = *t_kick;
    k.gen = gen;
    k.mirror.Reset();
    k.mirror.device = 0;
    k.inflight_p9.clear();
    k.hits.clear();
    k.out.clear();
    for (KickState::Range& r : k.recent) r = {};
  }
  return *t_kick;
}

// Draw packets of a kicked segment (any IB depth the CP executes): a path-9 record for each executed draw
// whose packet end no hooked writer recorded.
struct KickEmit {
  const uint8_t* base;
  uint32_t dev;
  KickState* k;
  uint32_t tag;  // 0x90000000 | 2 kick inside a writer | 4 range overlaps a recent kick
  bool failed;
  size_t last_draw = SIZE_MAX;  // index in k->out of the last draw walked (an M3b marker refers back to it)
  void Draw(const StreamMirror& m, uint32_t at, uint32_t n, uint32_t header, int depth);
  void Marker(uint32_t at);
};

// Header-only walk of [from, to): true when the packets tile the range exactly (nothing is applied).
// A zero dword is a one-dword NOP, as in the SDK command processor.
bool PacketsTile(const uint8_t* base, uint32_t from, uint32_t to) {
  uint32_t p = from;
  while (p < to) {
    const uint32_t h = G32(base, p);
    const uint32_t type = h >> 30;
    const uint32_t body = h == 0 ? 0u : type == 1 ? 2u : type == 2 ? 0u : ((h >> 16) & 0x3FFFu) + 1;
    p += (body + 1) * 4;
  }
  return p == to;
}

// Applies the register writes of the packets in [from, to) (guest addresses) to the mirror. Returns false
// when the walk does not end exactly at `to` (then nothing after the failure point is trusted). With emit
// (kick walks only, after PacketsTile validated the range), each executed draw packet is handed to it with
// the mirror state at that packet, and predicated packets follow the CP's bin test.
bool ApplyPackets(const uint8_t* base, StreamMirror& m, uint32_t from, uint32_t to, int depth,
                  KickEmit* emit = nullptr) {
  uint32_t p = from;
  static constexpr uint32_t kTypeBase[5] = {0x4000u, 0x4800u, 0x4900u, 0x4908u, 0x2000u};
  while (p < to) {
    const uint32_t h = G32(base, p);
    const uint32_t type = h >> 30;
    uint32_t body = 0;
    if (h == 0) {
      if (!t_kick_walk) g_nop_headers.fetch_add(1, std::memory_order_relaxed);
    } else if (type == 0) {
      body = ((h >> 16) & 0x3FFFu) + 1;
      if (p + (body + 1) * 4 > to) return false;
      const uint32_t reg0 = h & 0x7FFFu;
      const bool one = (h >> 15) & 1;
      for (uint32_t i = 0; i < body; ++i) m.Write(one ? reg0 : reg0 + i, G32(base, p + 4 + i * 4));
      if (!t_kick_walk && reg0 >= 0x4000u && reg0 < 0x4800u) g_t0_const_words.fetch_add(body, std::memory_order_relaxed);
      if (!t_kick_walk && reg0 >= 0x4800u && reg0 < 0x48C0u) g_t0_fetch_words.fetch_add(body, std::memory_order_relaxed);
    } else if (type == 1) {
      body = 2;
      if (p + 12 > to) return false;
      m.Write(h & 0x7FFu, G32(base, p + 4));
      m.Write((h >> 11) & 0x7FFu, G32(base, p + 8));
    } else if (type == 3) {
      body = ((h >> 16) & 0x3FFFu) + 1;
      if (p + (body + 1) * 4 > to) return false;
      const uint32_t opcode = (h >> 8) & 0x7Fu;
      const bool is_draw = opcode == 0x22 || opcode == 0x36;
      if (emit && (h & 1)) {
        // Predicated packet: the CP skips it when (bin select & bin mask) == 0.
        if (!(m.bin_select & m.bin_mask)) {
          if (is_draw) g_kick_pred_skipped.fetch_add(1, std::memory_order_relaxed);
          p += (body + 1) * 4;
          continue;
        }
        if (is_draw) g_kick_pred_run.fetch_add(1, std::memory_order_relaxed);
      }
      if (emit && is_draw) {
        emit->Draw(m, p, body, h, depth);
      } else if (emit && opcode == 0x10 && body == 2 && G32(base, p + 4) == kMarkerMagic) {
        emit->Marker(p);
      } else if ((opcode == 0x50 || opcode == 0x51) && body >= 2) {  // SET_BIN_MASK / SET_BIN_SELECT: hi, lo
        const uint64_t v = (uint64_t(G32(base, p + 4)) << 32) | G32(base, p + 8);
        (opcode == 0x50 ? m.bin_mask : m.bin_select) = v;
      } else if (opcode >= 0x60 && opcode <= 0x63 && body >= 1) {  // SET_BIN_MASK/SELECT_LO/HI
        const uint64_t v = G32(base, p + 4);
        uint64_t& t = opcode < 0x62 ? m.bin_mask : m.bin_select;
        t = (opcode & 1) ? ((t & 0xFFFFFFFFull) | (v << 32)) : ((t & 0xFFFFFFFF00000000ull) | v);
      } else if (opcode == 0x27 && body >= 1) {  // IM_LOAD: addr | stage
        const uint32_t at = G32(base, p + 4);
        if ((at & 3) < 2) {
          m.shader_addr[at & 3] = at & ~3u;
          m.shader_known[at & 3] = true;
        }
      } else if (opcode == 0x2B && body >= 1) {  // IM_LOAD_IMMEDIATE: stage, ucode inline
        const uint32_t t = G32(base, p + 4) & 3;
        if (t < 2) {
          m.shader_addr[t] = 0;
          m.shader_known[t] = true;
        }
      } else if (opcode == 0x2D && body >= 1) {  // SET_CONSTANT
        const uint32_t ot = G32(base, p + 4);
        const uint32_t t = (ot >> 16) & 0xFFu;
        if (t < 5) {
          const uint32_t reg0 = kTypeBase[t] + (ot & 0x7FFu);
          for (uint32_t i = 1; i < body; ++i) m.Write(reg0 + i - 1, G32(base, p + 4 + i * 4));
        }
        if (!t_kick_walk) g_set_const.fetch_add(1, std::memory_order_relaxed);
      } else if (opcode == 0x55 && body >= 1) {  // SET_CONSTANT2
        const uint32_t reg0 = G32(base, p + 4) & 0xFFFFu;
        for (uint32_t i = 1; i < body; ++i) m.Write(reg0 + i - 1, G32(base, p + 4 + i * 4));
        if (!t_kick_walk) g_set_const.fetch_add(1, std::memory_order_relaxed);
      } else if (opcode == 0x2F && body >= 3) {  // LOAD_ALU_CONSTANT
        const uint32_t addr = G32(base, p + 4) & 0x3FFFFFFFu;
        const uint32_t ot = G32(base, p + 8);
        const uint32_t n = G32(base, p + 12) & 0xFFFu;
        const uint32_t t = (ot >> 16) & 0xFFu;
        if (t < 5 && addr) {
          const uint32_t reg0 = kTypeBase[t] + (ot & 0x7FFu);
          // Source as the SDK selects it (SelectGuestDataSource): the physical view unless its first words are all
          // zero and the virtual view's are not.
          uint32_t src = PhysView(addr);
          const uint32_t probe = n < 64u ? n : 64u;
          bool phys_any = false;
          for (uint32_t i = 0; i < probe && !phys_any; ++i) phys_any = G32(base, src + i * 4) != 0;
          if (!phys_any && probe) {
            bool virt_any = false;
            for (uint32_t i = 0; i < probe && !virt_any; ++i) virt_any = G32(base, addr + i * 4) != 0;
            if (virt_any) {
              src = addr;
              g_load_alu_virtual.fetch_add(1, std::memory_order_relaxed);
            }
          }
          for (uint32_t i = 0; i < n; ++i) m.Write(reg0 + i, G32(base, src + i * 4));
        }
        if (!t_kick_walk) {
          g_load_alu.fetch_add(1, std::memory_order_relaxed);
          g_load_alu_words.fetch_add(n, std::memory_order_relaxed);
        }
      } else if (opcode == 0x56) {  // SET_SHADER_CONSTANTS: register range at index (as the SDK: like SET_CONSTANT2)
        if (body >= 1) {
          const uint32_t reg0 = G32(base, p + 4) & 0xFFFFu;
          for (uint32_t i = 1; i < body; ++i) m.Write(reg0 + i - 1, G32(base, p + 4 + i * 4));
        }
        if (!t_kick_walk) g_set_shader_consts.fetch_add(1, std::memory_order_relaxed);
      } else if ((opcode == 0x3F || opcode == 0x37) && body >= 2) {  // INDIRECT_BUFFER(_PFD)
        const uint32_t ib = G32(base, p + 4);
        const uint32_t len = G32(base, p + 8) & 0xFFFFFu;
        // The SDK executes nested buffers up to depth 4.
        if (depth < 4 && ib && len && len <= 0xFFFFFu) {
          const uint32_t a = PhysView(ib);
          if (emit) emit->k->nested.emplace_back(ib & 0x1FFFFFFFu, (ib & 0x1FFFFFFFu) + len * 4);
          if (emit && !PacketsTile(base, a, a + len * 4)) {
            emit->failed = true;
            m.Reset();
          } else if (!ApplyPackets(base, m, a, a + len * 4, depth + 1, emit)) {
            if (emit) emit->failed = true;
            m.Reset();
          }
          if (!t_kick_walk) g_ib_calls.fetch_add(1, std::memory_order_relaxed);
        } else {
          (t_kick_walk ? g_kick_ib_skipped : g_ib_skipped).fetch_add(1, std::memory_order_relaxed);
        }
      }
    }
    p += (body + 1) * 4;
  }
  return p == to;
}

// Packets the engine wrote on this device between the previous writer call and this one (state setup,
// constants written straight into the command buffer).
void MirrorGap(const uint8_t* base, uint32_t dev, uint32_t ring_before) {
  StreamMirror& m = Mirror(dev);
  const uint32_t last = m.last_after;
  if (last && ring_before >= last && ring_before - last <= 0x100000u) {
    g_gap_scanned.fetch_add(1, std::memory_order_relaxed);
    g_gap_dwords.fetch_add((ring_before - last) / 4, std::memory_order_relaxed);
    if (!ApplyPackets(base, m, last + 4, ring_before + 4, 0)) {
      m.Reset();
      g_gap_lost.fetch_add(1, std::memory_order_relaxed);
    }
  } else {
    // New command buffer segment (or first call): packets before this call in the new segment are unknown.
    m.Reset();
    g_gap_lost.fetch_add(1, std::memory_order_relaxed);
  }
}

void FillState(const uint8_t* base, uint32_t dev, const StreamMirror* mirror, DrawRecordV2& r,
               WriterExtra* x = nullptr) {
  if (x) *x = WriterExtra{};
  const uint32_t vs = G32(base, dev + kDevVertexShader), ps = G32(base, dev + kDevPixelShader);
  const uint32_t ib = G32(base, dev + kDevIndexBuffer);
  r.vertex_shader_guest = vs ? G32(base, vs + 48) : 0;
  r.pixel_shader_guest = ps ? G32(base, ps + 24) : 0;
  r.index_buffer_guest = ib ? G32(base, ib + 24) : 0;
  r.vertex_decl_guest = G32(base, dev + kDevVertexDecl);
  r.surface_info = G32(base, dev + kDevSurface);
  r.color_mask = G32(base, dev + kDevColorMask);
  r.depth_control = G32(base, dev + kDevDepthControl);
  r.mode_control = G32(base, dev + kDevModeControl);
  const StreamMirror* ms = (mirror && mirror->device == dev) ? mirror : nullptr;
  uint64_t v3_mask[2] = {0, 0};
  if (x && g_v3_on.load(std::memory_order_relaxed)) {
    const Api& api = GetApi();
    uint32_t gen = 0;
    if (api.masks && api.masks(r.vertex_shader_guest, r.pixel_shader_guest, r.vertex_decl_guest, v3_mask, &gen) != 1) {
      v3_mask[0] = v3_mask[1] = 0;
    }
  }
  if (ms) {
    // Render-target state as the stream set it at this draw (multi-draw writers change it in between).
    const uint32_t rt_known = (ms->Reg(0x2000u, r.surface_info) ? 1u : 0u) | (ms->Reg(0x2104u, r.color_mask) ? 2u : 0u) |
                              (ms->Reg(0x2200u, r.depth_control) ? 4u : 0u) | (ms->Reg(0x2208u, r.mode_control) ? 8u : 0u);
    if (x) x->rt_known = rt_known;
    // M3 render state: hash of the kNativeStateRegs the stream has written (others count as 0 on both
    // sides), with the known-register mask in reserved2.
    uint32_t w[kNativeStateRegCount];
    uint64_t known = 0;
    for (uint32_t i = 0; i < kNativeStateRegCount; ++i) {
      w[i] = 0;
      if (ms->Reg(kNativeStateRegs[i], w[i])) known |= 1ull << i;
    }
    r.reserved1 = HashWords(w, kNativeStateRegCount);
    r.reserved2[0] = uint32_t(known);
    r.reserved2[1] = uint32_t(known >> 32);
    if (known) r.flags |= kFlagStreamState;
    if (x) {
      std::memcpy(x->state, w, sizeof(w));
      x->state_known = known;
    }
  }
  // Constants: the value the device's command stream last wrote (stream mirror) where known, else the
  // device shadow.
  const StreamMirror* m = ms;
  uint32_t vs_stream = 0, ps_stream = 0, fetch_stream = 0;
  // Per word: the writer mirror's value where it knows it (mask kept in x for the kick merge), else the shadow.
  auto group = [&](uint32_t shadow_at, uint32_t alu_first, uint32_t n, uint32_t& from_stream, uint8_t kind,
                   uint8_t index, uint64_t* full_bits) {
    uint32_t w[16];
    uint32_t known = 0;
    const uint8_t* p = GuestPtr(base, shadow_at);
    for (uint32_t i = 0; i < n; ++i) {
      if (m && (alu_first < 0x10000u ? m->AluValid(alu_first + i) : m->FetchValid(alu_first - 0x10000u + i))) {
        w[i] = alu_first < 0x10000u ? m->alu[alu_first + i] : m->fetch[alu_first - 0x10000u + i];
        known |= 1u << i;
      } else {
        uint32_t v;
        std::memcpy(&v, p + i * 4, 4);
        w[i] = _byteswap_ulong(v);
      }
    }
    const bool masked = kind < 2 && ((v3_mask[kind] >> index) & 1);
    if (known) ++from_stream;
    if (full_bits && (known || masked)) {
      {
        if (known == (1u << n) - 1u) full_bits[index >> 6] |= 1ull << (index & 63);
        if (known != (1u << n) - 1u || masked) {
          WriterPartial wp;
          wp.kind = kind;
          wp.index = index;
          wp.known = uint16_t(known);
          std::memcpy(wp.w, w, n * 4);
          x->partial.push_back(wp);
        }
      }
    }
    return HashWords(w, n);
  };
  for (uint32_t g = 0; g < 64; ++g) {
    r.vs_const_hash[g] = group(dev + kDevVsConst + g * 64, g * 16, 16, vs_stream, 0, uint8_t(g), x ? &x->vs_full : nullptr);
    r.ps_const_hash[g] =
        group(dev + kDevPsConst + g * 64, 1024 + g * 16, 16, ps_stream, 1, uint8_t(g), x ? &x->ps_full : nullptr);
  }
  for (uint32_t u = 0; u < 96; ++u) {
    r.fetch_hash[u] =
        group(dev + kDevFetch + u * 8, 0x10000u + u * 2, 2, fetch_stream, 2, uint8_t(u), x ? x->fetch_full : nullptr);
  }
  r.reserved0 = vs_stream | (ps_stream << 8) | (fetch_stream << 16);  // groups taken from the stream mirror
  if (vs_stream | ps_stream | fetch_stream) {
    r.flags |= kFlagStreamConsts;
    g_rec_with_stream.fetch_add(1, std::memory_order_relaxed);
    g_rec_vs_stream.fetch_add(vs_stream, std::memory_order_relaxed);
    g_rec_ps_stream.fetch_add(ps_stream, std::memory_order_relaxed);
    g_rec_fetch_stream.fetch_add(fetch_stream, std::memory_order_relaxed);
  }
  r.flags |= kFlagState;
}

// A draw packet the engine wrote outside the hooked writers (found in the stream between writer calls or at
// a kick): one record with the stream state at that packet. Geometry from the packet, shaders from the
// stream's last IM_LOAD, no index-buffer object (the packet's DMA base is the CP's own value).
void BuildGapDraw(const uint8_t* base, const StreamMirror& m, uint32_t dev, uint32_t at, uint32_t n,
                  uint32_t header, uint32_t tag, DrawRecordV2& r) {
  const uint32_t opcode = (header >> 8) & 0x7Fu;
  r = DrawRecordV2{};
  r.size = sizeof(r);
  r.version = kDrawRecordVersion;
  r.frame_id = g_frame_id.load(std::memory_order_acquire);
  if (t_seq_frame != r.frame_id) {
    t_seq_frame = r.frame_id;
    t_seq = 0;
  }
  r.sequence = t_seq++;
  r.pass_id = 0xFFFFFFFFu;
  r.thread_id = GetCurrentThreadId();
  r.path_id = kPathGap;
  r.caller = tag;  // path 9: 0x90000000 | 1 nested IB | 2 kick inside a writer | 4 overlapping kick | 8 predicated
  r.device_guest = dev;
  FillState(base, dev, &m, r);
  r.vertex_shader_guest = m.shader_known[0] ? m.shader_addr[0] : 0;
  r.pixel_shader_guest = m.shader_known[1] ? m.shader_addr[1] : 0;
  r.index_buffer_guest = 0;
  const uint32_t init_at = at + (opcode == 0x22 ? 8 : 4);
  const uint32_t initiator = n >= (opcode == 0x22 ? 2u : 1u) ? G32(base, init_at) : 0;
  const uint32_t source = (initiator >> 6) & 3;
  r.flags = (r.flags & (kFlagState | kFlagStreamConsts | kFlagStreamState)) | kFlagVerificationOnly |
            kFlagHasPacket | kFlagPm4Geometry | (source == 0 ? kFlagIndexed : 0u);
  r.packet_end = at + (n + 1) * 4;
  r.draw_initiator = initiator;
  r.dma_base = (opcode == 0x22 && source == 0 && n >= 4) ? G32(base, at + 12) : 0;
  r.primitive_type = initiator & 0x3F;
  r.index_count = initiator >> 16;
}

// A writer record at its execution: what the writer's own stream mirror knew stays (as in the writer-only
// records); registers and constant groups it took from the device shadow are replaced by the kick mirror's
// (CP order, continuous across segments) where the kick mirror knows them. Writers that emit several draw
// packets take the shaders of this packet from the kick mirror's IM_LOADs.
void MergeKickState(const uint8_t* base, uint32_t dev, const StreamMirror& m, const WriterExtra& x,
                    DrawRecordV2& r) {
  (void)base;
  g_merged.fetch_add(1, std::memory_order_relaxed);
  if (m.device != dev) return;
  const bool writer_wins = g_writer_wins.load(std::memory_order_relaxed);
  bool rt_dis = false;
  auto rt = [&](uint32_t bit, uint32_t reg, uint32_t& field) {
    uint32_t v;
    if (!m.Reg(reg, v)) return;
    const bool writer_knew = (x.rt_known & bit) != 0;
    if (writer_knew && v != field) rt_dis = true;
    if (!writer_knew || !writer_wins) field = v;
  };
  rt(1, 0x2000u, r.surface_info);
  rt(2, 0x2104u, r.color_mask);
  rt(4, 0x2200u, r.depth_control);
  rt(8, 0x2208u, r.mode_control);
  if (rt_dis) g_dis_rt.fetch_add(1, std::memory_order_relaxed);
  uint32_t w[kNativeStateRegCount];
  uint64_t known = x.state_known;
  bool added = false, state_dis = false;
  for (uint32_t i = 0; i < kNativeStateRegCount; ++i) {
    const bool writer_knew = (x.state_known >> i) & 1;
    w[i] = writer_knew ? x.state[i] : 0u;
    uint32_t v;
    if (m.Reg(kNativeStateRegs[i], v)) {
      if (writer_knew && v != w[i]) {
        state_dis = true;
        g_dis_state_regs[i].fetch_add(1, std::memory_order_relaxed);
      }
      if (!writer_knew || !writer_wins) {
        if (v != w[i] || !writer_knew) added = true;
        w[i] = v;
      }
      known |= 1ull << i;
    }
  }
  if (state_dis) g_dis_state.fetch_add(1, std::memory_order_relaxed);
  if (added) {
    r.reserved1 = HashWords(w, kNativeStateRegCount);
    r.reserved2[0] = uint32_t(known);
    r.reserved2[1] = uint32_t(known >> 32);
    r.flags |= kFlagStreamState;
    g_merged_state_kick.fetch_add(1, std::memory_order_relaxed);
  }
  // Constant groups / fetch units. Kick policy: the kick mirror knows all words -> its hash; else a group all of whose
  // words came from the writer's mirror stays; else per word kick mirror, else the writer-time word (mirror or shadow).
  // Writer policy: writer-full first, then kick-full, then per word writer mirror before kick mirror. Groups the
  // writer's mirror knew nothing of and the kick mirror only partly stay as written.
  uint16_t partial_at[3][96];
  std::memset(partial_at, 0xFF, sizeof(partial_at));
  for (size_t i = 0; i < x.partial.size(); ++i) partial_at[x.partial[i].kind][x.partial[i].index] = uint16_t(i);
  uint64_t n_writer_full = 0, n_kick_full = 0, n_composed = 0, n_left = 0, n_dis = 0;
  auto merge = [&](uint32_t kind, uint32_t index, uint32_t first, uint32_t n, bool writer_full, uint32_t& hash) {
    const bool fetch = kind == 2;
    const uint32_t* kv = fetch ? &m.fetch[first] : &m.alu[first];
    uint32_t kick_known = 0;
    for (uint32_t i = 0; i < n; ++i) {
      if (fetch ? m.FetchValid(first + i) : m.AluValid(first + i)) kick_known |= 1u << i;
    }
    const bool kick_full = kick_known == (1u << n) - 1u;
    if (writer_full && kick_full && HashWords(kv, n) != hash) ++n_dis;
    if (writer_full && (writer_wins || !kick_full)) {
      ++n_writer_full;
      return;
    }
    if (!kick_known) return;
    if (kick_full) {
      hash = HashWords(kv, n);
      ++n_kick_full;
      return;
    }
    const uint16_t pi = partial_at[kind][index];
    if (pi == 0xFFFFu) {
      ++n_left;
      return;
    }
    const WriterPartial& p = x.partial[pi];
    uint32_t w[16];
    for (uint32_t i = 0; i < n; ++i) {
      const bool wk = (p.known >> i) & 1, kk = (kick_known >> i) & 1;
      w[i] = (kk && (!wk || !writer_wins)) ? kv[i] : p.w[i];
    }
    hash = HashWords(w, n);
    ++n_composed;
  };
  for (uint32_t g = 0; g < 64; ++g) {
    merge(0, g, g * 16, 16, (x.vs_full >> g) & 1, r.vs_const_hash[g]);
    merge(1, g, 1024 + g * 16, 16, (x.ps_full >> g) & 1, r.ps_const_hash[g]);
  }
  for (uint32_t u = 0; u < 96; ++u) merge(2, u, u * 2, 2, (x.fetch_full[u >> 6] >> (u & 63)) & 1, r.fetch_hash[u]);
  if (n_dis) g_dis_const.fetch_add(1, std::memory_order_relaxed);
  g_mg_writer_full.fetch_add(n_writer_full, std::memory_order_relaxed);
  g_mg_kick_full.fetch_add(n_kick_full, std::memory_order_relaxed);
  g_mg_composed.fetch_add(n_composed, std::memory_order_relaxed);
  g_mg_kick_partial_left.fetch_add(n_left, std::memory_order_relaxed);
  // Multi-packet writers: each stage's shader from this packet's IM_LOAD when the kick mirror has a guest address
  // for it (IM_LOAD_IMMEDIATE / not seen yet: the writer's device-object shader stays).
  if (r.flags & kFlagPm4Geometry) {
    bool any = false;
    if (m.shader_known[0] && m.shader_addr[0]) {
      r.vertex_shader_guest = m.shader_addr[0];
      g_merged_vs_kick.fetch_add(1, std::memory_order_relaxed);
      any = true;
    }
    if (m.shader_known[1] && m.shader_addr[1]) {
      r.pixel_shader_guest = m.shader_addr[1];
      g_merged_ps_kick.fetch_add(1, std::memory_order_relaxed);
      any = true;
    }
    if (any) g_merged_shader_kick.fetch_add(1, std::memory_order_relaxed);
  }
}

// v3 values of one record at its execution, consistent with the hashes it carries: merged writer records follow the
// merge policy (MergeKickState), path-9 records take the kick mirror's words, else the device shadow's (as FillState
// hashed them at this kick). A published group without such words -> the record goes without values.
void CaptureV3(const uint8_t* base, uint32_t dev, const StreamMirror& m, const WriterExtra* x, const DrawRecordV2& r,
               uint32_t header, KickState* k) {
  k->v3.emplace_back();
  V3Info& v = k->v3.back();
  if (!g_v3_on.load(std::memory_order_relaxed)) {
    g_v3_off.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const Api& api = GetApi();
  uint64_t mk[2] = {0, 0};
  uint32_t gen = 0;
  if (!api.submit_v3 || !api.masks ||
      api.masks(r.vertex_shader_guest, r.pixel_shader_guest, r.vertex_decl_guest, mk, &gen) != 1) {
    g_v3_unpublished.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  v.kind = 1;
  v.generation = gen;
  v.header = header;
  if (m.device != dev) {
    g_v3_no_values.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const bool writer_wins = g_writer_wins.load(std::memory_order_relaxed);
  const size_t off = k->v3_words.size();
  uint64_t shadow_groups = 0;
  auto words = [&](uint32_t kind, uint32_t g) -> bool {
    const uint32_t first = kind == 0 ? g * 16 : 1024 + g * 16;
    uint32_t kick_known = 0;
    for (uint32_t i = 0; i < 16; ++i) {
      if (m.AluValid(first + i)) kick_known |= 1u << i;
    }
    const bool kick_full = kick_known == 0xFFFFu;
    uint32_t w[16];
    if (!x) {
      const uint32_t shadow = dev + (kind == 0 ? kDevVsConst : kDevPsConst) + g * 64;
      for (uint32_t i = 0; i < 16; ++i) w[i] = ((kick_known >> i) & 1) ? m.alu[first + i] : G32(base, shadow + i * 4);
      if (!kick_full) ++shadow_groups;
    } else {
      const bool writer_full = ((kind == 0 ? x->vs_full : x->ps_full) >> g) & 1;
      const WriterPartial* e = nullptr;
      for (const WriterPartial& p : x->partial) {
        if (p.kind == kind && p.index == g) {
          e = &p;
          break;
        }
      }
      if (kick_full && (!writer_full || !writer_wins)) {
        std::memcpy(w, &m.alu[first], sizeof(w));
      } else if (!e) {
        return false;
      } else {
        bool shadow = false;
        for (uint32_t i = 0; i < 16; ++i) {
          const bool wk = (e->known >> i) & 1, kk = (kick_known >> i) & 1;
          w[i] = (kk && (!wk || !writer_wins) && !writer_full) ? m.alu[first + i] : e->w[i];
          if (!wk && !kk) shadow = true;
        }
        if (shadow) ++shadow_groups;
      }
    }
    k->v3_words.insert(k->v3_words.end(), w, w + 16);
    return true;
  };
  bool ok = true;
  for (uint32_t kind = 0; kind < 2 && ok; ++kind) {
    for (uint64_t bits = mk[kind]; bits && ok; bits &= bits - 1) ok = words(kind, uint32_t(std::countr_zero(bits)));
  }
  const size_t n = k->v3_words.size() - off;
  if (!ok || n > kDrawRecordV3MaxWords) {
    k->v3_words.resize(off);
    g_v3_no_values.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  v.kind = 2;
  v.vs_mask = mk[0];
  v.ps_mask = mk[1];
  v.offset = uint32_t(off);
  v.words = uint32_t(n);
  g_v3_values.fetch_add(1, std::memory_order_relaxed);
  g_v3_value_words.fetch_add(n, std::memory_order_relaxed);
  g_v3_shadow_groups.fetch_add(shadow_groups, std::memory_order_relaxed);
}

void SubmitMarker(const uint8_t* base, uint32_t marker_at, const DrawRecordV2& r, const V3Info& v,
                  const uint32_t* words);

void KickEmit::Draw(const StreamMirror& m, uint32_t at, uint32_t n, uint32_t header, int depth) {
  const uint32_t end = GpuAddr(at + (n + 1) * 4);
  Held held;
  if (Take(end, held)) {
    k->hits.push_back(held.r);
    g_kick_writer_hits.fetch_add(1, std::memory_order_relaxed);
    MergeKickState(base, dev, m, held.x, held.r);
    k->out.push_back(held.r);
    CaptureV3(base, dev, m, &held.x, held.r, header, k);
    last_draw = k->out.size() - 1;
    return;
  }
  k->out.emplace_back();
  BuildGapDraw(base, m, dev, at, n, header, tag | (depth > 0 ? 1u : 0u) | ((header & 1) ? 8u : 0u), k->out.back());
  CaptureV3(base, dev, m, nullptr, k->out.back(), header, k);
  last_draw = k->out.size() - 1;
}

void KickEmit::Marker(uint32_t at) {
  if (last_draw == SIZE_MAX || last_draw >= k->out.size()) {
    g_mk_orphan.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  SubmitMarker(base, at, k->out[last_draw], k->v3[last_draw], k->v3_words.data());
  last_draw = SIZE_MAX;
}


// ---------------------------------------------------------------------------------------------------------------
// FRAME8 lean capture (dist file native_frame_lean, read when the bridge is switched on; default OFF; needs the bridge
// on = native_frame_exec_dry or native_bridge_verify). Same records as the verify path (v2 fields + exact hashes +
// 64-reg state + v3 values) but built cheaply:
//  - writers: no writer stream mirror (MirrorGap / ApplyPackets), no FillState hashes, no WriterExtra; one small POD
//    per draw packet in a direct-mapped held table (no unordered_map / deque / 1 KB copies);
//  - kick walk: ONE pass (no PacketsTile pre-walk) over a lean shared mirror that keeps dirty bits per constant group /
//    fetch unit / state block and cached hashes -> per draw only the groups written since the previous draw are hashed;
//  - masks query cached per (VS, PS, decl) within a kick; records are built straight into one byte buffer per kick.
// The kick mirror already won every group under the default policy (n8: writer-full 0, kick-full 100 %), so the
// records are the same as the verify path's after the mirror's warm-up.
struct LeanHeld {
  uint32_t end = 0;  // GPU packet end (0 = empty)
  uint32_t gen = 0;
  uint64_t frame_id = 0;
  uint32_t sequence, pass_id, thread_id, path_id, caller, mesh, instance, dev, flags, packet_end, initiator, dma_base,
      prim, count, start;
  int32_t base_vertex;
  uint32_t vs, ps, ib, decl, surface, cmask, depth, mode;
};
constexpr uint32_t kLeanSlots = 1u << 17;  // direct-mapped by (end >> 2): spans 512 KB of command buffer
constexpr uint32_t kLeanLocks = 64;
LeanHeld* g_lean_held = nullptr;  // allocated on first use (bridge on only), lives for the process
std::mutex g_lean_locks[kLeanLocks];
std::once_flag g_lean_once;
std::atomic<uint64_t> g_ln_held{0}, g_ln_evicted{0}, g_ln_taken{0}, g_ln_stale{0}, g_ln_gap{0}, g_ln_records{0},
    g_ln_values{0}, g_ln_value_words{0}, g_ln_unpub{0}, g_ln_novalues{0}, g_ln_bad_walk{0}, g_ln_dropped{0},
    g_ln_dirty_groups{0}, g_ln_partial_groups{0}, g_ln_mask_queries{0}, g_ln_dwords{0}, g_ln_accepted{0},
    g_ln_rejected{0}, g_ln_kicks{0}, g_ln_state_partial{0}, g_ln_batches{0};
inline uint32_t LeanSlot(uint32_t end) { return (end >> 2) & (kLeanSlots - 1); }
void LeanHold(const LeanHeld& h) {
  std::call_once(g_lean_once, [] { g_lean_held = new LeanHeld[kLeanSlots](); });
  const uint32_t s = LeanSlot(h.end);
  std::lock_guard<std::mutex> lock(g_lean_locks[s & (kLeanLocks - 1)]);
  LeanHeld& e = g_lean_held[s];
  if (e.end && e.gen == h.gen && e.frame_id + 8 >= h.frame_id) g_ln_evicted.fetch_add(1, std::memory_order_relaxed);
  e = h;
  g_ln_held.fetch_add(1, std::memory_order_relaxed);
}
bool LeanTake(uint32_t end, LeanHeld& out) {
  std::call_once(g_lean_once, [] { g_lean_held = new LeanHeld[kLeanSlots](); });
  const uint32_t s = LeanSlot(end);
  const uint64_t now = g_frame_id.load(std::memory_order_relaxed);
  const uint32_t gen = g_gate_gen.load(std::memory_order_relaxed);
  std::lock_guard<std::mutex> lock(g_lean_locks[s & (kLeanLocks - 1)]);
  LeanHeld& e = g_lean_held[s];
  if (e.end != end) return false;
  if (e.gen != gen || e.frame_id + 8 < now) {
    e.end = 0;
    g_ln_stale.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  out = e;
  e.end = 0;
  return true;
}

// ---------------------------------------------------------------------------------------------------------------
// FRAME9 draw metadata (dist native_frame_meta; layout + protocol in the header). Writers stage records per recording
// device; the kick of that device's segment sends the records inside the segment as one batch right before its
// SubmitCommandStream. Records of segments that are never kicked as a root stream (nested buffers) expire.
struct MetaStaged {
  DrawMetaRecordV1 r;
  uint32_t gen;
  uint32_t frame;
};
struct MetaStage {
  std::atomic<uint32_t> dev{0};
  std::mutex mu;
  std::vector<MetaStaged> v;
  bool overflow = false;
  uint32_t pruned_frame = 0;  // writer side: records older than kMetaMaxAgeFrames dropped once per frame
};
constexpr uint32_t kMetaStages = 16;
constexpr size_t kMetaStageCap = 16384;
// Records of buffers that are never kicked as a root stream (nested IBs recorded by other devices) are dropped after
// this many frames (the root kick of a writer's segment comes within the frame).
constexpr uint32_t kMetaMaxAgeFrames = 2;
MetaStage g_mstage[kMetaStages];
std::atomic<uint64_t> g_meta_epoch{0};
std::atomic<uint64_t> g_mt_staged{0}, g_mt_sent{0}, g_mt_batches{0}, g_mt_empty_batches{0}, g_mt_tile_batches{0},
    g_mt_incomplete{0}, g_mt_overflow{0}, g_mt_expired{0}, g_mt_kept{0}, g_mt_no_stage{0}, g_mt_sdk_ok{0},
    g_mt_sdk_bad{0}, g_mt_chk_match{0}, g_mt_chk_miss0{0}, g_mt_chk_miss_nested{0}, g_mt_chk_diff{0},
    g_mt_chk_p9_hit{0}, g_mt_chk_p9_miss{0}, g_mt_chk_reused{0}, g_mt_chk_diff_field[8] = {}, g_t_meta{0}, g_n_meta{0},
    g_mt_pruned{0};
// Drops expired records (other generation / older than kMetaMaxAgeFrames); caller holds st.mu.
uint64_t MetaPrune(MetaStage& st, uint32_t gen, uint32_t now) {
  size_t w = 0;
  for (size_t i = 0; i < st.v.size(); ++i) {
    const MetaStaged& e = st.v[i];
    if (e.gen != gen || uint32_t(now - e.frame) > kMetaMaxAgeFrames) continue;
    if (w != i) st.v[w] = e;
    ++w;
  }
  const uint64_t n = st.v.size() - w;
  st.v.resize(w);
  return n;
}
MetaStage* StageFor(uint32_t dev) {
  thread_local uint32_t t_dev = 0;
  thread_local MetaStage* t_stage = nullptr;
  if (t_stage && t_dev == dev) return t_stage;
  for (uint32_t i = 0; i < kMetaStages; ++i) {
    uint32_t d = g_mstage[i].dev.load(std::memory_order_acquire);
    if (d == 0 && g_mstage[i].dev.compare_exchange_strong(d, dev, std::memory_order_acq_rel)) d = dev;
    if (d == dev) {
      t_dev = dev;
      t_stage = &g_mstage[i];
      return t_stage;
    }
  }
  return nullptr;
}
// Builds the batch for the root stream [base, base + 4 * dwords) of device dev into out (header + records).
void MetaBuild(uint32_t dev, uint32_t base_phys, uint32_t dwords, uint32_t flags, std::vector<uint8_t>& out) {
  out.resize(sizeof(DrawMetaBatchV1));
  DrawMetaBatchV1 h{};
  h.version = kDrawMetaVersion1;
  h.record_size = sizeof(DrawMetaRecordV1);
  h.kick_epoch = g_meta_epoch.fetch_add(1, std::memory_order_relaxed) + 1;
  h.stream_base = base_phys;
  h.stream_dwords = dwords;
  h.producer_thread = GetCurrentThreadId();
  h.flags = flags;
  MetaStage* st = dev ? StageFor(dev) : nullptr;
  if (!(flags & kDrawMetaBatchTileControl)) {
    if (!st) {
      h.flags |= kDrawMetaBatchIncomplete;
      g_mt_no_stage.fetch_add(1, std::memory_order_relaxed);
    } else {
      const uint32_t gen = g_gate_gen.load(std::memory_order_relaxed);
      const uint32_t now = uint32_t(g_frame_id.load(std::memory_order_relaxed));
      const uint32_t lo = base_phys, hi = base_phys + dwords * 4u;
      uint64_t expired = 0, kept = 0;
      std::lock_guard<std::mutex> lock(st->mu);
      if (st->overflow) {
        h.flags |= kDrawMetaBatchIncomplete;
        st->overflow = false;
      }
      size_t w = 0;
      for (size_t i = 0; i < st->v.size(); ++i) {
        MetaStaged& e = st->v[i];
        if (e.gen != gen || uint32_t(now - e.frame) > kMetaMaxAgeFrames) {
          ++expired;
          continue;
        }
        if (e.r.packet_end > lo && e.r.packet_end <= hi) {
          e.r.flags |= kDrawMetaFlagDirectRoot;
          const size_t o = out.size();
          out.resize(o + sizeof(DrawMetaRecordV1));
          std::memcpy(out.data() + o, &e.r, sizeof(DrawMetaRecordV1));
          continue;
        }
        if (w != i) st->v[w] = e;
        ++w;
        ++kept;
      }
      st->v.resize(w);
      if (expired) g_mt_expired.fetch_add(expired, std::memory_order_relaxed);
      if (kept) g_mt_kept.fetch_add(kept, std::memory_order_relaxed);
    }
  }
  h.record_count = uint32_t((out.size() - sizeof(DrawMetaBatchV1)) / sizeof(DrawMetaRecordV1));
  h.size = uint32_t(out.size());
  std::memcpy(out.data(), &h, sizeof(h));
  g_mt_batches.fetch_add(1, std::memory_order_relaxed);
  if (flags & kDrawMetaBatchTileControl) g_mt_tile_batches.fetch_add(1, std::memory_order_relaxed);
  if (!h.record_count) g_mt_empty_batches.fetch_add(1, std::memory_order_relaxed);
  if (h.flags & kDrawMetaBatchIncomplete) g_mt_incomplete.fetch_add(1, std::memory_order_relaxed);
  if (h.record_count) g_mt_sent.fetch_add(h.record_count, std::memory_order_relaxed);
}
void MetaSend(const std::vector<uint8_t>& batch) {
  const Api& api = GetApi();
  if (!api.submit_meta) return;
  const int r = api.submit_meta(batch.data(), uint32_t(batch.size()));
  (r >= 0 ? g_mt_sdk_ok : g_mt_sdk_bad).fetch_add(1, std::memory_order_relaxed);
}

}  // namespace

InstanceScope::InstanceScope(uint32_t instance_guest) : previous_instance_(0), on_(Enabled()) {
  if (!on_) return;
  previous_instance_ = g_instance;
  g_instance = instance_guest;
}

InstanceScope::~InstanceScope() {
  if (on_) g_instance = previous_instance_;
}

MeshScope::MeshScope(uint32_t pass_id, uint32_t mesh_guest)
    : previous_pass_(0), previous_mesh_(0), previous_instance_(0), on_(Enabled()) {
  if (!on_) return;
  previous_pass_ = g_mesh.pass;
  previous_mesh_ = g_mesh.mesh;
  previous_instance_ = g_mesh.instance;
  g_mesh = {pass_id, mesh_guest, g_instance};
}

MeshScope::~MeshScope() {
  if (on_) g_mesh = {previous_pass_, previous_mesh_, previous_instance_};
}

WriterCall::WriterCall(uint8_t* base, uint32_t path, const WriterArgs& args)
    : base_(base), path_(path), args_(args), ring_before_(0), outer_(t_writer_depth++ == 0) {
  if (!outer_) return;
  TickScope ts(g_t_ctor, g_n_ctor);
  if (!PlausibleDevice(base, args.device)) {
    outer_ = false;
    g_bad_device.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  ring_before_ = G32(base, args.device + kDevRingWrite);
  KickState& kick = Kick();
  kick_epoch_ = kick.epoch;
  gen_ = kick.gen;
  kick.inflight_p9.clear();
  if (!g_lean.load(std::memory_order_relaxed) && !g_meta_only.load(std::memory_order_relaxed)) {
    MirrorGap(base, args.device, ring_before_);
  }
}

namespace {
// M3b verify marker after a finished 825D2478 call (see g_marker_on): NOP {magic, 0} appended at the write pointer,
// right after the call's packets (nothing the engine wrote moves: in the deferred-tiling layout, device +10808 bit 1,
// the DRAW is followed by SET_BIN_MASK_LO / EVENT_WRITE_EXT / EVENT_WRITE, which D3D may patch later). The marker
// refers to the call's (single) DRAW packet before it; its predicate bit is the DRAW's. Only without a kick during
// the call and when the 3 dwords fit below the limit (+44).
void AppendMarker(uint8_t* base, uint32_t dev, uint32_t draw_header) {
  const uint32_t wp = G32(base, dev + kDevRingWrite);
  const uint32_t limit = G32(base, dev + 44);
  if (!(wp + 16 < limit)) {
    g_mk_skip_space.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (*GuestPtr(base, dev + 10808) & 2) g_mk_skip_layout.fetch_add(1, std::memory_order_relaxed);
  W32(base, wp + 4, (3u << 30) | (1u << 16) | (0x10u << 8) | (draw_header & 1u));  // NOP, 2 body dwords
  W32(base, wp + 8, kMarkerMagic);
  W32(base, wp + 12, 0);
  W32(base, dev + kDevRingWrite, wp + 12);
  g_mk_inserted.fetch_add(1, std::memory_order_relaxed);
}
}  // namespace

WriterCall::~WriterCall() {
  --t_writer_depth;
  if (!outer_) return;
  TickScope ts(g_t_dtor, g_n_dtor);
  const Api& api = GetApi();
  if (!api.submit) return;
  {
    thread_local uint32_t t_calls = 0;  // FRAME9: shared counter touched once per 64 calls per thread
    if (++t_calls == 64) {
      g_writer_calls.fetch_add(64, std::memory_order_relaxed);
      t_calls = 0;
    }
  }
  const uint8_t* base = base_;
  const uint32_t dev = args_.device;
  const uint32_t after = G32(base, dev + kDevRingWrite);  // last dword written

  // Draw packets written by this call: (header address, dword count after the header).
  struct Found {
    uint32_t at, n, header;
  } found[8];
  uint32_t n_found = 0;
  uint32_t flags = 0;
  bool clean = false;
  KickState& kick = Kick();
  // A kick inside the call: the call's packets before it were handed to the GPU (and got path-9 records
  // from the kick walk); the rest is in a new segment, so the forward walk would cross segments.
  const bool kicked = kick.epoch != kick_epoch_;
  if (kicked) g_writer_kicked.fetch_add(1, std::memory_order_relaxed);
  if (!kicked && after >= ring_before_ && after - ring_before_ <= 0x40000u) {
    uint32_t p = ring_before_ + 4;
    while (p <= after && p > ring_before_) {
      const uint32_t h = G32(base, p);
      const uint32_t type = h >> 30;
      uint32_t body;
      if (h == 0) {
        body = 0;  // one-dword NOP (SDK command processor)
      } else if (type == 3) {
        body = ((h >> 16) & 0x3FFFu) + 1;
        const uint32_t opcode = (h >> 8) & 0x7Fu;
        if ((opcode == 0x22 || opcode == 0x36) && n_found < 8) found[n_found++] = {p, body, h};
      } else if (type == 0) {
        body = ((h >> 16) & 0x3FFFu) + 1;
      } else if (type == 1) {
        body = 2;
      } else {
        body = 0;
      }
      p += (body + 1) * 4;
    }
    clean = p == after + 4;
    if (!clean) {
      // Did not land exactly on the write pointer (the call moved to another command buffer
      // segment inside the scanned range): trust nothing from the forward walk.
      g_parse_abort.fetch_add(1, std::memory_order_relaxed);
      n_found = 0;
    }
  }
  if (!clean) {
    // The call switched to a new command buffer segment: find the last packet ending at the write
    // pointer (825D2478 / 825D1680 end with their draw packet).
    for (uint32_t k = 1; k <= 64 && !n_found; ++k) {
      const uint32_t a = after - 4 * k;
      const uint32_t h = G32(base, a);
      const uint32_t opcode = (h >> 8) & 0x7Fu;
      if ((h >> 30) == 3 && (opcode == 0x22 || opcode == 0x36) && ((h >> 16) & 0x3FFFu) + 1 == k) {
        found[n_found++] = {a, k, h};
        flags |= kFlagBackscan;
      }
    }
    (n_found ? g_backscan : g_not_found).fetch_add(1, std::memory_order_relaxed);
  }
  if (g_meta.load(std::memory_order_relaxed) && n_found) {
    // FRAME9 draw metadata: one staged record per draw packet (production: nothing else happens below).
    MetaStage* st = StageFor(dev);
    if (!st) {
      g_mt_no_stage.fetch_add(1, std::memory_order_relaxed);
    } else {
      MetaStaged e;
      std::memset(&e, 0, sizeof(e));
      e.gen = gen_;
      e.frame = uint32_t(g_frame_id.load(std::memory_order_acquire));
      thread_local uint32_t t_meta_frame = 0, t_meta_seq = 0;  // own sequence: lean records keep frame8d's
      if (t_meta_frame != e.frame) {
        t_meta_frame = e.frame;
        t_meta_seq = 0;
      }
      e.r.pass_id = g_mesh.pass;
      e.r.mesh_guest = g_mesh.pass != 0xFFFFFFFFu ? g_mesh.mesh : 0;
      e.r.instance_guest = g_mesh.pass != 0xFFFFFFFFu ? g_mesh.instance : g_instance;
      e.r.caller = args_.lr;
      e.r.path_id = uint16_t(path_);
      uint32_t staged = 0;
      std::lock_guard<std::mutex> lock(st->mu);
      if (st->pruned_frame != e.frame) {
        st->pruned_frame = e.frame;
        const uint64_t n = MetaPrune(*st, gen_, e.frame);
        if (n) g_mt_pruned.fetch_add(n, std::memory_order_relaxed);
      }
      for (uint32_t i = 0; i < n_found; ++i) {
        const Found& f = found[i];
        const uint32_t end_gpu = GpuAddr(f.at + (f.n + 1) * 4);
        if (kicked && !kick.inflight_p9.empty() &&
            std::find(kick.inflight_p9.begin(), kick.inflight_p9.end(), end_gpu) != kick.inflight_p9.end()) {
          continue;  // the kick walk already made it a path-9 record (lean check mode only)
        }
        if (st->v.size() >= kMetaStageCap) {
          st->overflow = true;
          g_mt_overflow.fetch_add(1, std::memory_order_relaxed);
          break;
        }
        e.r.packet_end = end_gpu;
        e.r.writer_sequence = t_meta_seq++;
        e.r.flags = n_found > 1 ? kDrawMetaFlagMultiDrawWriter : 0;
        e.r.base_vertex = 0;
        e.r.start_index = 0;
        e.r.engine_count = 0;
        if (path_ == kPathDrawIndexed && n_found == 1) {
          e.r.base_vertex = int32_t(args_.r5);
          e.r.start_index = args_.r6;
          e.r.engine_count = args_.r7;
          e.r.flags |= kDrawMetaFlagEngineArgsValid;
        } else if (path_ == kPathDrawUP && n_found == 1) {
          e.r.engine_count = args_.r5;
          e.r.flags |= kDrawMetaFlagEngineArgsValid;
        }
        st->v.push_back(e);
        ++staged;
      }
      thread_local uint32_t t_mt_staged = 0;  // shared counter touched once per 64 records per thread
      t_mt_staged += staged;
      if (t_mt_staged >= 64) {
        g_mt_staged.fetch_add(t_mt_staged, std::memory_order_relaxed);
        t_mt_staged = 0;
      }
    }
    if (g_meta_only.load(std::memory_order_relaxed)) {
      kick.inflight_p9.clear();
      return;
    }
  } else if (g_meta_only.load(std::memory_order_relaxed)) {
    if (!n_found) g_no_draw.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (g_lean.load(std::memory_order_relaxed)) {
    // FRAME8 lean: no writer mirror, no FillState; one small held entry per draw packet (merged at its kick).
    if (!n_found) {
      g_no_draw.fetch_add(1, std::memory_order_relaxed);
      kick.inflight_p9.clear();
      return;
    }
    LeanHeld h;
    h.gen = gen_;
    h.frame_id = g_frame_id.load(std::memory_order_acquire);
    if (t_seq_frame != h.frame_id) {
      t_seq_frame = h.frame_id;
      t_seq = 0;
    }
    h.pass_id = g_mesh.pass;
    h.thread_id = GetCurrentThreadId();
    h.path_id = path_;
    h.caller = args_.lr;
    h.mesh = g_mesh.pass != 0xFFFFFFFFu ? g_mesh.mesh : 0;
    h.instance = g_mesh.pass != 0xFFFFFFFFu ? g_mesh.instance : g_instance;
    h.dev = dev;
    {
      const uint32_t vs = G32(base, dev + kDevVertexShader), ps = G32(base, dev + kDevPixelShader);
      const uint32_t ib = G32(base, dev + kDevIndexBuffer);
      h.vs = vs ? G32(base, vs + 48) : 0;
      h.ps = ps ? G32(base, ps + 24) : 0;
      h.ib = ib ? G32(base, ib + 24) : 0;
      h.decl = G32(base, dev + kDevVertexDecl);
      h.surface = G32(base, dev + kDevSurface);
      h.cmask = G32(base, dev + kDevColorMask);
      h.depth = G32(base, dev + kDevDepthControl);
      h.mode = G32(base, dev + kDevModeControl);
    }
    for (uint32_t i = 0; i < n_found; ++i) {
      const Found& f = found[i];
      const uint32_t end_gpu = GpuAddr(f.at + (f.n + 1) * 4);
      if (kicked && std::find(kick.inflight_p9.begin(), kick.inflight_p9.end(), end_gpu) != kick.inflight_p9.end()) {
        g_writer_dropped_kicked.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      const uint32_t opcode = (f.header >> 8) & 0x7Fu;
      const uint32_t init_at = f.at + (opcode == 0x22 ? 8 : 4);
      const uint32_t initiator = f.n >= (opcode == 0x22 ? 2u : 1u) ? G32(base, init_at) : 0;
      const uint32_t source = (initiator >> 6) & 3;
      h.end = end_gpu;
      h.sequence = t_seq++;
      h.flags = kFlagVerificationOnly | kFlagHasPacket | flags | (source == 0 ? kFlagIndexed : 0u);
      h.packet_end = f.at + (f.n + 1) * 4;
      h.initiator = initiator;
      h.dma_base = (opcode == 0x22 && source == 0 && f.n >= 4) ? G32(base, f.at + 12) : 0;
      h.start = 0;
      h.base_vertex = 0;
      if (path_ == kPathDrawIndexed && n_found == 1) {
        h.prim = args_.r4;
        h.base_vertex = int32_t(args_.r5);
        h.start = args_.r6;
        h.count = args_.r7;
        h.flags |= kFlagEngineBase;
      } else if (path_ == kPathDrawUP && n_found == 1) {
        h.prim = args_.r4;
        h.count = args_.r5;
      } else {
        h.prim = initiator & 0x3F;
        h.count = initiator >> 16;
        h.flags |= kFlagPm4Geometry;
      }
      LeanHold(h);
    }
    kick.inflight_p9.clear();
    return;
  }
  // The call's own packets (constant flushes, fetch constants, state, the draws) go into the stream
  // mirror up to each draw packet; when they could not be walked the mirror no longer knows the state.
  StreamMirror& m = Mirror(dev);
  bool track = clean;
  if (!track) m.Reset();
  uint32_t cursor = ring_before_ + 4;
  auto finish_mirror = [&] {
    if (track && !ApplyPackets(base, m, cursor, after + 4, 0)) m.Reset();
    m.last_after = after;
  };
  if (!n_found) {
    finish_mirror();
    g_no_draw.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  DrawRecordV2 r{};
  r.size = sizeof(r);
  r.version = kDrawRecordVersion;
  r.frame_id = g_frame_id.load(std::memory_order_acquire);
  if (t_seq_frame != r.frame_id) {
    t_seq_frame = r.frame_id;
    t_seq = 0;
  }
  r.pass_id = g_mesh.pass;
  r.thread_id = GetCurrentThreadId();
  r.path_id = path_;
  r.caller = args_.lr;
  r.mesh_guest = g_mesh.pass != 0xFFFFFFFFu ? g_mesh.mesh : 0;
  r.instance_guest = g_mesh.pass != 0xFFFFFFFFu ? g_mesh.instance : g_instance;
  r.device_guest = dev;
  for (uint32_t i = 0; i < n_found; ++i) {
    const Found& f = found[i];
    const uint32_t end_gpu = GpuAddr(f.at + (f.n + 1) * 4);
    if (kicked && std::find(kick.inflight_p9.begin(), kick.inflight_p9.end(), end_gpu) != kick.inflight_p9.end()) {
      g_writer_dropped_kicked.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    if (track) {
      if (ApplyPackets(base, m, cursor, f.at, 0)) {
        cursor = f.at;
      } else {
        m.Reset();
        track = false;
      }
    }
    r.flags = 0;
    // Device shadow after the call (= what the call flushed for its last draw), overlaid with the
    // stream mirror's values at this draw packet.
    WriterExtra extra;
    FillState(base, dev, t_mirror, r, &extra);
    const uint32_t opcode = (f.header >> 8) & 0x7Fu;
    // DRAW_INDX: viz query token, initiator, [DMA base, DMA size]; DRAW_INDX_2: initiator, [indices].
    const uint32_t init_at = f.at + (opcode == 0x22 ? 8 : 4);
    const uint32_t initiator = f.n >= (opcode == 0x22 ? 2u : 1u) ? G32(base, init_at) : 0;
    const uint32_t source = (initiator >> 6) & 3;
    r.sequence = t_seq++;
    r.flags = (r.flags & (kFlagState | kFlagStreamConsts | kFlagStreamState)) | kFlagVerificationOnly |
              kFlagHasPacket | flags;
    r.packet_end = f.at + (f.n + 1) * 4;
    r.draw_initiator = initiator;
    r.dma_base = (opcode == 0x22 && source == 0 && f.n >= 4) ? G32(base, f.at + 12) : 0;
    if (source == 0) r.flags |= kFlagIndexed;
    r.start_index = 0;
    r.base_vertex = 0;
    if (path_ == kPathDrawIndexed && n_found == 1) {
      // Engine arguments (independent of the packet): r4 primitive, r5 base vertex, r6 start, r7 count.
      r.primitive_type = args_.r4;
      r.base_vertex = int32_t(args_.r5);
      r.start_index = args_.r6;
      r.index_count = args_.r7;
      r.flags |= kFlagEngineBase;
    } else if (path_ == kPathDrawUP && n_found == 1) {
      // DrawVerticesUP: r4 primitive, r5 vertex count (r6 stride, r7 data).
      r.primitive_type = args_.r4;
      r.index_count = args_.r5;
    } else {
      r.primitive_type = initiator & 0x3F;
      r.index_count = initiator >> 16;
      r.flags |= kFlagPm4Geometry;
    }
    // Held until the packet executes in a kicked segment (OnKick merges the CP-order state and submits).
    Hold(end_gpu, r, std::move(extra), gen_);
  }
  kick.inflight_p9.clear();
  finish_mirror();
  if (path_ == kPathDrawIndexed && g_marker_on.load(std::memory_order_relaxed) && g_mesh.pass == 8 &&
      args_.lr == 0x8223F3C4u) {
    if (kicked) {
      g_mk_skip_kick.fetch_add(1, std::memory_order_relaxed);
    } else if (!clean || n_found != 1 || (found[0].header >> 8 & 0x7Fu) != 0x22) {
      g_mk_skip_shape.fetch_add(1, std::memory_order_relaxed);
    } else {
      AppendMarker(base_, dev, found[0].header);
    }
  }
}

namespace {
int SubmitRecord(const DrawRecordV2& r) {
  const Api& api = GetApi();
  if (!api.submit) return 0;
  TickScope ts(g_t_sdk, g_n_sdk);
  const int result = api.submit(&r, sizeof(r));
  g_submitted.fetch_add(1, std::memory_order_relaxed);
  (result > 0 ? g_accepted : result == 0 ? g_rejected : g_errors).fetch_add(1, std::memory_order_relaxed);
  return result;
}
// One record as v3 (kind 1: without values, 2: with the captured values).
uint32_t BuildV3(std::vector<uint8_t>& buf, const DrawRecordV2& r, const V3Info& v, const uint32_t* words) {
  const uint32_t n = v.kind == 2 ? v.words : 0u;
  const uint32_t size = kDrawRecordV3HeaderSize + 4u * n;
  buf.resize(size);
  DrawRecordV3Header hd{};
  hd.v2 = r;
  hd.v2.version = kDrawRecordVersion3;
  hd.v2.size = size;
  hd.v2.flags |= kFlagVerificationOnly;
  if (v.kind == 2) {
    hd.v2.flags |= kFlagConstValues;
    hd.vs_group_mask = v.vs_mask;
    hd.ps_group_mask = v.ps_mask;
    hd.value_words = n;
  }
  hd.mask_generation = v.generation;
  hd.draw_header = v.header;
  std::memcpy(buf.data(), &hd, sizeof(hd));
  if (n) std::memcpy(buf.data() + sizeof(hd), words + v.offset, 4u * n);
  return size;
}
void SubmitV3(const DrawRecordV2& r, const V3Info& v, const uint32_t* words) {
  const Api& api = GetApi();
  thread_local std::vector<uint8_t> buf;
  TickScope ts(g_t_sdk, g_n_sdk);
  const uint32_t size = BuildV3(buf, r, v, words);
  const int result = api.submit_v3(buf.data(), size);
  g_submitted.fetch_add(1, std::memory_order_relaxed);
  (result > 0 ? g_accepted : result == 0 ? g_rejected : g_errors).fetch_add(1, std::memory_order_relaxed);
  (result > 0 ? g_v3_accepted : g_v3_rejected).fetch_add(1, std::memory_order_relaxed);
}
// The merged record of the draw right after an M3b marker: submitted as a VERIFY_ONLY marker record; the returned id
// goes into the marker (once: tiled replays / later executions of the same buffer reuse it).
void SubmitMarker(const uint8_t* base, uint32_t marker_at, const DrawRecordV2& r, const V3Info& v,
                  const uint32_t* words) {
  g_mk_seen.fetch_add(1, std::memory_order_relaxed);
  if (G32(base, marker_at + 8) != 0) {
    g_mk_replay.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const Api& api = GetApi();
  if (!api.submit_owned || v.kind != 2) {
    g_mk_novalues.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  thread_local std::vector<uint8_t> buf;
  const uint32_t size = BuildV3(buf, r, v, words);
  uint32_t id = 0;
  const int result = api.submit_owned(buf.data(), size, &id);
  if (result > 0 && id) {
    W32(base, marker_at + 8, id);
    g_mk_accepted.fetch_add(1, std::memory_order_relaxed);
  } else {
    (result < 0 ? g_mk_bad : g_mk_refused).fetch_add(1, std::memory_order_relaxed);
  }
}
// A kicked segment that could not be walked: its held writer records go out with their writer-time state.
// Records are moved out under the shard lock and submitted after it is released.
void SubmitHeldInRange(uint32_t from_gpu, uint32_t to_gpu) {
  std::vector<DrawRecordV2> out;
  for (HeldShard& h : g_held) {
    {
      std::lock_guard<std::mutex> lock(h.mu);
      for (auto it = h.map.begin(); it != h.map.end();) {
        if (it->first > from_gpu && it->first <= to_gpu) {
          for (const Held& e : it->second) out.push_back(e.r);
          it = h.map.erase(it);
        } else {
          ++it;
        }
      }
    }
    for (const DrawRecordV2& r : out) {
      SubmitRecord(r);
      g_held_fallback.fetch_add(1, std::memory_order_relaxed);
    }
    out.clear();
  }
}
// Expiry of held records whose packet no kick executed within 8 frames (FrameBoundary, every 4 frames).
// Returns how many records are still held.
uint64_t ExpireHeld() {
  const uint64_t now = g_frame_id.load(std::memory_order_relaxed);
  uint64_t left = 0;
  for (HeldShard& h : g_held) {
    std::lock_guard<std::mutex> lock(h.mu);
    for (auto it = h.map.begin(); it != h.map.end();) {
      std::deque<Held>& q = it->second;
      while (!q.empty() && q.front().r.frame_id + 8 < now) {
        q.pop_front();
        g_kick_ends_expired.fetch_add(1, std::memory_order_relaxed);
      }
      if (q.empty()) {
        it = h.map.erase(it);
      } else {
        left += q.size();
        ++it;
      }
    }
  }
  return left;
}
}  // namespace


namespace {
// FRAME8 lean kick mirror (see LeanHeld): registers in CP order + dirty bits + cached hashes.
struct LeanStateMap {
  uint8_t ctx[0x1000];
  uint8_t bl[16];
  int rt[4];  // state indices of 0x2000 (surface), 0x2104 (color mask), 0x2200 (depth), 0x2208 (mode)
  LeanStateMap() {
    std::memset(ctx, 0xFF, sizeof(ctx));
    std::memset(bl, 0xFF, sizeof(bl));
    for (uint32_t i = 0; i < kNativeStateRegCount; ++i) {
      const uint32_t r = kNativeStateRegs[i];
      if (r >= 0x2000u && r < 0x3000u) ctx[r - 0x2000u] = uint8_t(i);
      if (r >= 0x4900u && r < 0x4910u) bl[r - 0x4900u] = uint8_t(i);
    }
    rt[0] = ctx[0x000];
    rt[1] = ctx[0x104];
    rt[2] = ctx[0x200];
    rt[3] = ctx[0x208];
  }
};
const LeanStateMap g_lsm;

struct LeanMirror {
  uint32_t device = 0;
  uint32_t alu[2048];
  uint32_t fetch[192];
  uint32_t state[kNativeStateRegCount];
  uint64_t alu_valid[32];
  uint64_t fetch_valid[3];
  uint64_t state_known = 0;
  uint32_t shader_addr[2] = {0, 0};
  bool shader_known[2] = {false, false};
  uint64_t bin_mask = 0xFFFFFFFFull, bin_select = 0xFFFFFFFFull;
  uint64_t cdirty[2], fdirty[2];  // constant groups 0-127 (VS 0-63, PS 64-127) / fetch units 0-95 written since Refresh
  bool sdirty = true;
  uint64_t cfull[2], ffull[2];  // groups / units whose words are all known (cached hash valid)
  uint32_t chash[128], fhash[96], shash = 0;
  void Reset() {
    std::memset(alu_valid, 0, sizeof(alu_valid));
    std::memset(fetch_valid, 0, sizeof(fetch_valid));
    state_known = 0;
    shader_known[0] = shader_known[1] = false;
    cdirty[0] = cdirty[1] = ~0ull;
    fdirty[0] = ~0ull;
    fdirty[1] = 0xFFFFFFFFull;
    sdirty = true;
    cfull[0] = cfull[1] = ffull[0] = ffull[1] = 0;
  }
  void WriteAlu(uint32_t i, uint32_t v) {
    alu[i] = v;
    alu_valid[i >> 6] |= 1ull << (i & 63);
    cdirty[i >> 10] |= 1ull << ((i >> 4) & 63);
  }
  void WriteFetch(uint32_t i, uint32_t v) {
    fetch[i] = v;
    fetch_valid[i >> 6] |= 1ull << (i & 63);
    const uint32_t u = i >> 1;
    fdirty[u >> 6] |= 1ull << (u & 63);
  }
  void Write(uint32_t reg, uint32_t v) {
    if (reg - 0x4000u < 0x800u) {
      WriteAlu(reg - 0x4000u, v);
    } else if (reg - 0x4800u < 0xC0u) {
      WriteFetch(reg - 0x4800u, v);
    } else if (reg - 0x2000u < 0x1000u) {
      const uint8_t s = g_lsm.ctx[reg - 0x2000u];
      if (s != 0xFF) {
        state[s] = v;
        state_known |= 1ull << s;
        sdirty = true;
      }
    } else if (reg - 0x4900u < 0x10u) {
      const uint8_t s = g_lsm.bl[reg - 0x4900u];
      if (s != 0xFF) {
        state[s] = v;
        state_known |= 1ull << s;
        sdirty = true;
      }
    }
  }
  // Set bits [first, first + count) of a bit array.
  static void SetBits(uint64_t* w, uint32_t first, uint32_t count) {
    while (count) {
      const uint32_t b = first & 63, k = std::min<uint32_t>(64u - b, count);
      w[first >> 6] |= (k == 64 ? ~0ull : ((1ull << k) - 1)) << b;
      first += k;
      count -= k;
    }
  }
  // n consecutive big-endian guest words at `at` into registers reg0.. (constant / fetch runs: one copy loop, the
  // valid and dirty masks set once per covered range; same result as n Write calls).
  void WriteRun(const uint8_t* base, uint32_t reg0, uint32_t at, uint32_t n) {
    if (!n) return;
    if (reg0 >= 0x4000u && reg0 + n <= 0x4800u) {
      const uint32_t i0 = reg0 - 0x4000u;
      const uint8_t* p = GuestPtr(base, at);
      uint32_t* d = &alu[i0];
      for (uint32_t i = 0; i < n; ++i) {
        uint32_t v;
        std::memcpy(&v, p + i * 4, 4);
        d[i] = _byteswap_ulong(v);
      }
      SetBits(alu_valid, i0, n);
      SetBits(cdirty, i0 >> 4, ((i0 + n - 1) >> 4) - (i0 >> 4) + 1);
      return;
    }
    if (reg0 >= 0x4800u && reg0 + n <= 0x48C0u) {
      const uint32_t i0 = reg0 - 0x4800u;
      const uint8_t* p = GuestPtr(base, at);
      uint32_t* d = &fetch[i0];
      for (uint32_t i = 0; i < n; ++i) {
        uint32_t v;
        std::memcpy(&v, p + i * 4, 4);
        d[i] = _byteswap_ulong(v);
      }
      SetBits(fetch_valid, i0, n);
      SetBits(fdirty, i0 >> 1, ((i0 + n - 1) >> 1) - (i0 >> 1) + 1);
      return;
    }
    for (uint32_t i = 0; i < n; ++i) Write(reg0 + i, G32(base, at + i * 4));
  }
  bool StateReg(int idx, uint32_t& v) const {
    if (idx < 0 || idx >= int(kNativeStateRegCount) || !((state_known >> idx) & 1)) return false;
    v = state[idx];
    return true;
  }
  // Re-hash what was written since the previous draw. Returns the number of groups / units re-hashed.
  uint32_t Refresh(bool hash) {
    uint32_t n = 0;
    for (uint32_t h = 0; h < 2; ++h) {
      for (uint64_t b = cdirty[h]; b; b &= b - 1) {
        const uint32_t g = h * 64 + uint32_t(std::countr_zero(b));
        const uint32_t first = g * 16;
        const uint32_t bits = uint32_t(alu_valid[first >> 6] >> (first & 63)) & 0xFFFFu;
        if (bits == 0xFFFFu) {
          if (hash) chash[g] = HashWords(&alu[first], 16);
          cfull[h] |= 1ull << (g & 63);
        } else {
          cfull[h] &= ~(1ull << (g & 63));
        }
        ++n;
      }
      cdirty[h] = 0;
      for (uint64_t b = fdirty[h]; b; b &= b - 1) {
        const uint32_t u = h * 64 + uint32_t(std::countr_zero(b));
        if (u >= 96) continue;
        const uint32_t first = u * 2;
        const uint32_t bits = uint32_t(fetch_valid[first >> 6] >> (first & 63)) & 3u;
        if (bits == 3u) {
          if (hash) fhash[u] = HashWords(&fetch[first], 2);
          ffull[h] |= 1ull << (u & 63);
        } else {
          ffull[h] &= ~(1ull << (u & 63));
        }
        ++n;
      }
      fdirty[h] = 0;
    }
    if (sdirty) {
      uint32_t w[kNativeStateRegCount];
      for (uint32_t i = 0; i < kNativeStateRegCount; ++i) w[i] = ((state_known >> i) & 1) ? state[i] : 0u;
      shash = HashWords(w, kNativeStateRegCount);
      sdirty = false;
    }
    return n;
  }
  bool CFull(uint32_t g) const { return (cfull[g >> 6] >> (g & 63)) & 1; }
  bool FFull(uint32_t u) const { return (ffull[u >> 6] >> (u & 63)) & 1; }
};
LeanMirror* g_lean_mirror = nullptr;  // only touched by OnKickLean under g_submit_mu
uint32_t g_lean_mirror_gen = 0;

// Mask cache of the lean walk: one entry, valid within one kick (stamp).
struct LeanMaskEntry {
  uint32_t vs = 0, ps = 0, decl = 0, stamp = 0;
  bool published = false;
  uint64_t mk[2] = {0, 0};
  uint32_t gen = 0;
};
struct LeanMaskCache {
  LeanMaskEntry e[1];
  uint32_t stamp = 0;
  void NewKick() {
    if (++stamp == 0) {
      for (LeanMaskEntry& x : e) x.stamp = 0;
      stamp = 1;
    }
  }
};
// FRAME9: kick blob without zero-filling what is overwritten anyway (std::vector::resize value-initializes).
struct LeanBlob {
  uint8_t* p = nullptr;
  size_t n = 0, cap = 0;
  uint8_t* Grow(size_t k) {
    if (n + k > cap) {
      size_t c = cap ? cap * 2 : (size_t(1) << 20);
      while (c < n + k) c *= 2;
      p = static_cast<uint8_t*>(std::realloc(p, c));
      cap = c;
    }
    uint8_t* r = p + n;
    n += k;
    return r;
  }
  size_t size() const { return n; }
  const uint8_t* data() const { return p; }
  void clear() { n = 0; }
};

struct LeanEmit {
  const uint8_t* base;
  uint32_t dev;
  uint32_t tag;
  bool failed = false;
  bool values = true;
  bool hashless = false;
  LeanBlob* blob = nullptr;
  std::vector<uint32_t>* p9_ends = nullptr;
  LeanMaskCache* mc = nullptr;
  uint32_t records = 0;
  struct CapRun* run = nullptr;  // AGENT_NATIVE29B step 2 (owned runs), nullptr = off
  bool own_ok = false;           // last Draw: ownable v4 record at [own_off, own_off + own_size) of the blob
  uint32_t own_off = 0, own_size = 0;
  void Draw(LeanMirror& m, uint32_t at, uint32_t n, uint32_t header, int depth);
};

// AGENT_NATIVE29B step 2 (owned runs, native_capture_full.inc): per depth-0 packet / draw hooks of the run builder.
struct CapRun;
void CapRunPacket(CapRun* run, uint32_t p, uint32_t h);
void CapRunBreakSkipped(CapRun* run);  // MAIN31
void CapRunDraw(CapRun* run, uint32_t p, uint32_t body, bool ownable, uint32_t blob_off, uint32_t blob_size);

// ApplyPackets for the lean mirror: one pass, same packet semantics (predication by the CP bin test, IM_LOAD,
// SET_CONSTANT(2), SET_SHADER_CONSTANTS, LOAD_ALU_CONSTANT with the SDK's source selection, nested IBs to depth 4).
bool LeanApply(const uint8_t* base, LeanMirror& m, uint32_t from, uint32_t to, int depth, LeanEmit& emit) {
  uint32_t p = from;
  static constexpr uint32_t kTypeBase[5] = {0x4000u, 0x4800u, 0x4900u, 0x4908u, 0x2000u};
  while (p < to) {
    const uint32_t h = G32(base, p);
    const uint32_t type = h >> 30;
    uint32_t body = 0;
    if (depth == 0 && emit.run) CapRunPacket(emit.run, p, h);  // AGENT_NATIVE29B step 2 (draws: after emit.Draw)
    if (h == 0) {
    } else if (type == 0) {
      body = ((h >> 16) & 0x3FFFu) + 1;
      if (p + (body + 1) * 4 > to) return false;
      const uint32_t reg0 = h & 0x7FFFu;
      if ((h >> 15) & 1) {
        for (uint32_t i = 0; i < body; ++i) m.Write(reg0, G32(base, p + 4 + i * 4));
      } else {
        m.WriteRun(base, reg0, p + 4, body);
      }
    } else if (type == 1) {
      body = 2;
      if (p + 12 > to) return false;
      m.Write(h & 0x7FFu, G32(base, p + 4));
      m.Write((h >> 11) & 0x7FFu, G32(base, p + 8));
    } else if (type == 3) {
      body = ((h >> 16) & 0x3FFFu) + 1;
      if (p + (body + 1) * 4 > to) return false;
      const uint32_t opcode = (h >> 8) & 0x7Fu;
      const bool is_draw = opcode == 0x22 || opcode == 0x36;
      if ((h & 1) && !(m.bin_select & m.bin_mask)) {  // predicated packet the CP skips
        if (depth == 0 && emit.run) CapRunBreakSkipped(emit.run);  // MAIN31: never swallow a skipped packet into a run
        p += (body + 1) * 4;
        continue;
      }
      if (is_draw) {
        emit.own_ok = false;
        emit.Draw(m, p, body, h, depth);
        if (depth == 0 && emit.run) CapRunDraw(emit.run, p, body, emit.own_ok, emit.own_off, emit.own_size);
      } else if ((opcode == 0x50 || opcode == 0x51) && body >= 2) {
        const uint64_t v = (uint64_t(G32(base, p + 4)) << 32) | G32(base, p + 8);
        (opcode == 0x50 ? m.bin_mask : m.bin_select) = v;
      } else if (opcode >= 0x60 && opcode <= 0x63 && body >= 1) {
        const uint64_t v = G32(base, p + 4);
        uint64_t& t = opcode < 0x62 ? m.bin_mask : m.bin_select;
        t = (opcode & 1) ? ((t & 0xFFFFFFFFull) | (v << 32)) : ((t & 0xFFFFFFFF00000000ull) | v);
      } else if (opcode == 0x27 && body >= 1) {
        const uint32_t a = G32(base, p + 4);
        if ((a & 3) < 2) {
          m.shader_addr[a & 3] = a & ~3u;
          m.shader_known[a & 3] = true;
        }
      } else if (opcode == 0x2B && body >= 1) {
        const uint32_t t = G32(base, p + 4) & 3;
        if (t < 2) {
          m.shader_addr[t] = 0;
          m.shader_known[t] = true;
        }
      } else if (opcode == 0x2D && body >= 1) {
        const uint32_t ot = G32(base, p + 4);
        const uint32_t t = (ot >> 16) & 0xFFu;
        if (t < 5 && body > 1) m.WriteRun(base, kTypeBase[t] + (ot & 0x7FFu), p + 8, body - 1);
      } else if ((opcode == 0x55 || opcode == 0x56) && body >= 1) {
        const uint32_t reg0 = G32(base, p + 4) & 0xFFFFu;
        if (body > 1) m.WriteRun(base, reg0, p + 8, body - 1);
      } else if (opcode == 0x2F && body >= 3) {
        const uint32_t addr = G32(base, p + 4) & 0x3FFFFFFFu;
        const uint32_t ot = G32(base, p + 8);
        const uint32_t n = G32(base, p + 12) & 0xFFFu;
        const uint32_t t = (ot >> 16) & 0xFFu;
        if (t < 5 && addr) {
          uint32_t src = PhysView(addr);
          const uint32_t probe = n < 64u ? n : 64u;
          bool phys_any = false;
          for (uint32_t i = 0; i < probe && !phys_any; ++i) phys_any = G32(base, src + i * 4) != 0;
          if (!phys_any && probe) {
            bool virt_any = false;
            for (uint32_t i = 0; i < probe && !virt_any; ++i) virt_any = G32(base, addr + i * 4) != 0;
            if (virt_any) src = addr;
          }
          m.WriteRun(base, kTypeBase[t] + (ot & 0x7FFu), src, n);
        }
      } else if ((opcode == 0x3F || opcode == 0x37) && body >= 2) {
        const uint32_t ib = G32(base, p + 4);
        const uint32_t len = G32(base, p + 8) & 0xFFFFFu;
        if (depth < 4 && ib && len) {
          const uint32_t a = PhysView(ib);
          if (!LeanApply(base, m, a, a + len * 4, depth + 1, emit)) {
            emit.failed = true;
            m.Reset();
          }
        }
      }
    }
    p += (body + 1) * 4;
  }
  return p == to;
}

// FRAME9 meta check (native_frame_meta + native_frame_lean): the batch this kick sends (built before the walk) vs the
// writer record the lean walk takes for each executed draw. Field bits: 0 pass 1 path 2 caller 3 mesh 4 instance
// 5 engine base/start/count.
struct MetaIndex {
  static constexpr uint32_t kSize = 4096;  // open addressing, > 2x the largest batch seen (~600)
  uint32_t key[kSize];
  uint32_t idx[kSize];
  uint32_t stamp[kSize];
  uint32_t cur = 0;
  bool overflow = false;
  const DrawMetaRecordV1* recs = nullptr;
  uint32_t n = 0;
  void Build(const std::vector<uint8_t>& batch) {
    if (++cur == 0) {
      std::memset(stamp, 0, sizeof(stamp));
      cur = 1;
    }
    recs = reinterpret_cast<const DrawMetaRecordV1*>(batch.data() + sizeof(DrawMetaBatchV1));
    n = uint32_t((batch.size() - sizeof(DrawMetaBatchV1)) / sizeof(DrawMetaRecordV1));
    overflow = n > kSize / 2;
    for (uint32_t i = 0; i < n && !overflow; ++i) {
      uint32_t h = (recs[i].packet_end * 0x9E3779B1u) >> 20;
      while (stamp[h] == cur && key[h] != recs[i].packet_end) h = (h + 1) & (kSize - 1);
      key[h] = recs[i].packet_end;
      idx[h] = i;
      stamp[h] = cur;
    }
  }
  const DrawMetaRecordV1* Find(uint32_t end) const {
    if (overflow) {
      for (uint32_t i = 0; i < n; ++i) {
        if (recs[i].packet_end == end) return &recs[i];
      }
      return nullptr;
    }
    uint32_t h = (end * 0x9E3779B1u) >> 20;
    while (stamp[h] == cur) {
      if (key[h] == end) return &recs[idx[h]];
      h = (h + 1) & (kSize - 1);
    }
    return nullptr;
  }
};
MetaIndex* g_meta_index = nullptr;  // only touched by OnKickLean under g_submit_mu
bool g_meta_index_valid = false;
void MetaCheck(uint32_t end, const LeanHeld* h, int depth) {
  const DrawMetaRecordV1* r = g_meta_index_valid ? g_meta_index->Find(end) : nullptr;
  if (!h) {
    (r ? g_mt_chk_p9_hit : g_mt_chk_p9_miss).fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (!r) {
    (depth > 0 ? g_mt_chk_miss_nested : g_mt_chk_miss0).fetch_add(1, std::memory_order_relaxed);
    return;
  }
  uint32_t diff = (r->pass_id != h->pass_id ? 1u : 0u) | (r->path_id != h->path_id ? 2u : 0u) |
                  (r->caller != h->caller ? 4u : 0u) | (r->mesh_guest != h->mesh ? 8u : 0u) |
                  (r->instance_guest != h->instance ? 16u : 0u);
  if (r->flags & kDrawMetaFlagEngineArgsValid) {
    const bool up = r->path_id == kPathDrawUP;
    if (r->engine_count != h->count || (!up && (r->base_vertex != h->base_vertex || r->start_index != h->start))) diff |= 32u;
  }
  if (!diff) {
    g_mt_chk_match.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  g_mt_chk_diff.fetch_add(1, std::memory_order_relaxed);
  for (uint32_t b = 0; b < 6; ++b) {
    if ((diff >> b) & 1) g_mt_chk_diff_field[b].fetch_add(1, std::memory_order_relaxed);
  }
}

#include "native_capture_full.inc"  // AGENT_NATIVE29B: capture v4-full + per-pass census (opt-in native_capture_full)

void LeanEmit::Draw(LeanMirror& m, uint32_t at, uint32_t n, uint32_t header, int depth) {
  const bool nohash = hashless;
  g_ln_dirty_groups.fetch_add(m.Refresh(!nohash), std::memory_order_relaxed);
  const uint32_t end = GpuAddr(at + (n + 1) * 4);
  DrawRecordV3Header hd;
  std::memset(&hd, 0, sizeof(hd));
  DrawRecordV2& r = hd.v2;
  r.size = sizeof(DrawRecordV2);
  r.version = kDrawRecordVersion;
  LeanHeld h;
  const bool held = LeanTake(end, h);
  const uint32_t opcode = (header >> 8) & 0x7Fu;
  if (g_meta.load(std::memory_order_relaxed)) MetaCheck(end, held ? &h : nullptr, depth);
  if (held) {
    g_ln_taken.fetch_add(1, std::memory_order_relaxed);
    r.frame_id = h.frame_id;
    r.sequence = h.sequence;
    r.pass_id = h.pass_id;
    r.thread_id = h.thread_id;
    r.path_id = h.path_id;
    r.caller = h.caller;
    r.mesh_guest = h.mesh;
    r.instance_guest = h.instance;
    r.device_guest = h.dev;
    r.flags = h.flags;
    r.packet_end = h.packet_end;
    r.draw_initiator = h.initiator;
    r.dma_base = h.dma_base;
    r.primitive_type = h.prim;
    r.index_count = h.count;
    r.start_index = h.start;
    r.base_vertex = h.base_vertex;
    r.vertex_shader_guest = h.vs;
    r.pixel_shader_guest = h.ps;
    r.index_buffer_guest = h.ib;
    r.vertex_decl_guest = h.decl;
    r.surface_info = h.surface;
    r.color_mask = h.cmask;
    r.depth_control = h.depth;
    r.mode_control = h.mode;
    if (r.flags & kFlagPm4Geometry) {
      // Multi-packet writers: this packet's shaders from the kick mirror's IM_LOADs (as MergeKickState).
      if (m.shader_known[0] && m.shader_addr[0]) r.vertex_shader_guest = m.shader_addr[0];
      if (m.shader_known[1] && m.shader_addr[1]) r.pixel_shader_guest = m.shader_addr[1];
    }
  } else {
    g_ln_gap.fetch_add(1, std::memory_order_relaxed);
    r.frame_id = g_frame_id.load(std::memory_order_acquire);
    if (t_seq_frame != r.frame_id) {
      t_seq_frame = r.frame_id;
      t_seq = 0;
    }
    r.sequence = t_seq++;
    r.pass_id = 0xFFFFFFFFu;
    r.thread_id = GetCurrentThreadId();
    r.path_id = kPathGap;
    r.caller = tag | (depth > 0 ? 1u : 0u) | ((header & 1) ? 8u : 0u);
    r.device_guest = dev;
    r.vertex_shader_guest = m.shader_known[0] ? m.shader_addr[0] : 0;
    r.pixel_shader_guest = m.shader_known[1] ? m.shader_addr[1] : 0;
    r.index_buffer_guest = 0;
    r.vertex_decl_guest = G32(base, dev + kDevVertexDecl);
    r.surface_info = G32(base, dev + kDevSurface);
    r.color_mask = G32(base, dev + kDevColorMask);
    r.depth_control = G32(base, dev + kDevDepthControl);
    r.mode_control = G32(base, dev + kDevModeControl);
    const uint32_t init_at = at + (opcode == 0x22 ? 8 : 4);
    const uint32_t initiator = n >= (opcode == 0x22 ? 2u : 1u) ? G32(base, init_at) : 0;
    const uint32_t source = (initiator >> 6) & 3;
    r.flags = kFlagVerificationOnly | kFlagHasPacket | kFlagPm4Geometry | (source == 0 ? kFlagIndexed : 0u);
    r.packet_end = at + (n + 1) * 4;
    r.draw_initiator = initiator;
    r.dma_base = (opcode == 0x22 && source == 0 && n >= 4) ? G32(base, at + 12) : 0;
    r.primitive_type = initiator & 0x3F;
    r.index_count = initiator >> 16;
  }
  // CP-order render-target fields and 64-register state (the kick mirror wins where it knows a register).
  m.StateReg(g_lsm.rt[0], r.surface_info);
  m.StateReg(g_lsm.rt[1], r.color_mask);
  m.StateReg(g_lsm.rt[2], r.depth_control);
  m.StateReg(g_lsm.rt[3], r.mode_control);
  if (m.state_known != ~0ull) g_ln_state_partial.fetch_add(1, std::memory_order_relaxed);
  if (m.state_known) {
    r.reserved1 = m.shash;
    r.reserved2[0] = uint32_t(m.state_known);
    r.reserved2[1] = uint32_t(m.state_known >> 32);
    r.flags |= kFlagStreamState;
  }
  // Constant hashes: cached for fully known groups, else per word mirror / device shadow at this kick.
  uint32_t partial = 0;
  auto group_words = [&](uint32_t g, uint32_t* w) {
    const uint32_t first = g * 16;
    const uint32_t shadow = dev + (g < 64 ? kDevVsConst + g * 64 : kDevPsConst + (g - 64) * 64);
    for (uint32_t i = 0; i < 16; ++i) {
      w[i] = ((m.alu_valid[(first + i) >> 6] >> ((first + i) & 63)) & 1) ? m.alu[first + i] : G32(base, shadow + i * 4);
    }
  };
  for (uint32_t g = 0; g < 128 && !nohash; ++g) {
    uint32_t hsh;
    if (m.CFull(g)) {
      hsh = m.chash[g];
    } else {
      uint32_t w[16];
      group_words(g, w);
      hsh = HashWords(w, 16);
      ++partial;
    }
    (g < 64 ? r.vs_const_hash[g] : r.ps_const_hash[g - 64]) = hsh;
  }
  for (uint32_t u = 0; u < 96 && !nohash; ++u) {
    if (m.FFull(u)) {
      r.fetch_hash[u] = m.fhash[u];
    } else {
      uint32_t w[2];
      for (uint32_t i = 0; i < 2; ++i) {
        const uint32_t j = u * 2 + i;
        w[i] = ((m.fetch_valid[j >> 6] >> (j & 63)) & 1) ? m.fetch[j] : G32(base, dev + kDevFetch + j * 4);
      }
      r.fetch_hash[u] = HashWords(w, 2);
      ++partial;
    }
  }
  if (partial) g_ln_partial_groups.fetch_add(partial, std::memory_order_relaxed);
  const uint32_t nvs = uint32_t(std::popcount(m.cfull[0])), nps = uint32_t(std::popcount(m.cfull[1])),
                 nf = uint32_t(std::popcount(m.ffull[0]) + std::popcount(m.ffull[1]));
  r.reserved0 = nvs | (nps << 8) | (nf << 16);
  if (r.reserved0) r.flags |= kFlagStreamConsts;
  if (!nohash) r.flags |= kFlagState;
  ++records;
  if (r.path_id == kPathGap) p9_ends->push_back(end);
  // v3: masks of the final (VS, PS, decl), cached within this kick.
  // One entry, re-queried whenever the key differs from the previous draw's (frame8d semantics: the SDK publishes
  // masks asynchronously, a wider cache could reuse an older generation within the kick).
  LeanMaskEntry& c = mc->e[0];
  if (c.stamp != mc->stamp || c.vs != r.vertex_shader_guest || c.ps != r.pixel_shader_guest ||
      c.decl != r.vertex_decl_guest) {
    c.stamp = mc->stamp;
    c.vs = r.vertex_shader_guest;
    c.ps = r.pixel_shader_guest;
    c.decl = r.vertex_decl_guest;
    const Api& api = GetApi();
    c.published = api.masks && api.submit_v3 && api.masks(c.vs, c.ps, c.decl, c.mk, &c.gen) == 1;
    g_ln_mask_queries.fetch_add(1, std::memory_order_relaxed);
  }
  // AGENT_NATIVE29B: census + self-sufficient v4 record (true = the v4 record replaces this draw's v3 / v2 record).
  if (g_capture_full.load(std::memory_order_relaxed) && g_v3_on.load(std::memory_order_relaxed)) {
    const size_t off0 = blob->size();
    bool ownable = false;
    if (CaptureFullDraw(base, dev, m, blob, mc, c, hd, at, n, header, held, values, depth, &ownable)) {
      own_ok = ownable && run != nullptr;
      own_off = uint32_t(off0);
      own_size = uint32_t(blob->size() - off0);
      return;
    }
  }
  if (!g_v3_on.load(std::memory_order_relaxed) || !c.published) {
    if (g_v3_on.load(std::memory_order_relaxed)) g_ln_unpub.fetch_add(1, std::memory_order_relaxed);
    std::memcpy(blob->Grow(sizeof(DrawRecordV2)), &r, sizeof(DrawRecordV2));
    return;
  }
  uint32_t nwords = 0;
  if (values) nwords = 16u * uint32_t(std::popcount(c.mk[0]) + std::popcount(c.mk[1]));
  if (nwords > kDrawRecordV3MaxWords) {
    nwords = 0;
    g_ln_novalues.fetch_add(1, std::memory_order_relaxed);
  }
  r.version = kDrawRecordVersion3;
  r.size = kDrawRecordV3HeaderSize + 4u * nwords;
  r.flags |= kFlagVerificationOnly;
  hd.mask_generation = c.gen;
  hd.draw_header = header;
  if (nwords) {
    r.flags |= kFlagConstValues;
    hd.vs_group_mask = c.mk[0];
    hd.ps_group_mask = c.mk[1];
    hd.value_words = nwords;
  }
  uint8_t* dst = blob->Grow(r.size);
  std::memcpy(dst, &hd, sizeof(hd));
  if (nwords) {
    uint32_t* out = reinterpret_cast<uint32_t*>(dst + sizeof(hd));
    for (uint32_t kind = 0; kind < 2; ++kind) {
      for (uint64_t bits = c.mk[kind]; bits; bits &= bits - 1) {
        const uint32_t g = kind * 64 + uint32_t(std::countr_zero(bits));
        if (m.CFull(g)) {
          std::memcpy(out, &m.alu[g * 16], 64);
        } else {
          group_words(g, out);
        }
        out += 16;
      }
    }
    g_ln_values.fetch_add(1, std::memory_order_relaxed);
    g_ln_value_words.fetch_add(nwords, std::memory_order_relaxed);
  }
}

// OnKick in lean mode (the caller holds g_submit_mu): one walk of the segment with the lean shared mirror; records of
// a fully walked segment are submitted in walk order; a failed walk drops them (the CP falls back to no record).
void OnKickLean(uint8_t* base, uint32_t r3, uint32_t gpu_addr, uint32_t dwords) {
  KickState& k = Kick();
  ++k.epoch;
  g_ln_kicks.fetch_add(1, std::memory_order_relaxed);
  if (!g_lean_mirror) {
    g_lean_mirror = new LeanMirror();  // lives for the process (bridge on only)
    g_lean_mirror->Reset();
  }
  const uint32_t gen = g_gate_gen.load(std::memory_order_acquire);
  LeanMirror& m = *g_lean_mirror;
  if (g_lean_mirror_gen != gen) {
    g_lean_mirror_gen = gen;
    m.Reset();
    m.device = 0;
  }
  const bool in_writer = t_writer_depth != 0;
  const uint32_t dev = PlausibleDevice(base, r3) ? r3 : 0u;
  const uint32_t from = PhysView(gpu_addr);
  const uint32_t to = from + dwords * 4u;
  if (!dwords || dwords >= 0x400000u || to < from || !dev) {
    m.Reset();
    g_ln_bad_walk.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (m.device != dev) {
    m.device = dev;
    m.Reset();
  }
  g_ln_dwords.fetch_add(dwords, std::memory_order_relaxed);
  m.bin_mask = g_bin_mask.load(std::memory_order_relaxed);
  m.bin_select = g_bin_select.load(std::memory_order_relaxed);
  thread_local LeanBlob blob;
  thread_local std::vector<uint32_t> p9;
  thread_local LeanMaskCache mc;
  blob.clear();
  p9.clear();
  mc.NewKick();
  LeanEmit emit{base, dev, 0x90000000u | (in_writer ? 2u : 0u)};
  emit.values = g_lean_values.load(std::memory_order_relaxed);
  emit.hashless = g_lean_nohash.load(std::memory_order_relaxed);
  emit.blob = &blob;
  emit.p9_ends = &p9;
  emit.mc = &mc;
  thread_local CapRun own_run;  // AGENT_NATIVE29B step 2
  if (g_capture_own.load(std::memory_order_relaxed) && GetOwnedRunFn()) {
    own_run.Clear();
    own_run.base = base;
    emit.run = &own_run;
  }
  const bool prof = g_prof_on.load(std::memory_order_relaxed);
  const uint64_t t0 = prof ? Ticks() : 0;
  const bool ok = LeanApply(base, m, from, to, 0, emit) && !emit.failed;
  if (emit.run) {
    if (ok) {
      own_run.Break(kCbSegmentEnd);
      const uint64_t tr0 = prof ? Ticks() : 0;
      CapApplyRuns(base, own_run, blob);  // rewrites owned regions before the segment reaches the CP
      if (prof) g_t_runs.fetch_add(Ticks() - tr0, std::memory_order_relaxed);  // MAIN31
    } else {
      own_run.Clear();
    }
  }
  if (prof) g_t_apply.fetch_add(Ticks() - t0, std::memory_order_relaxed);
  g_bin_mask.store(m.bin_mask, std::memory_order_relaxed);
  g_bin_select.store(m.bin_select, std::memory_order_relaxed);
  if (!ok) {
    m.Reset();
    g_ln_bad_walk.fetch_add(1, std::memory_order_relaxed);
    g_ln_dropped.fetch_add(emit.records, std::memory_order_relaxed);
    return;
  }
  if (in_writer) k.inflight_p9.insert(k.inflight_p9.end(), p9.begin(), p9.end());
  const Api& api = GetApi();
  if (!api.submit) return;
  const uint64_t t1 = prof ? Ticks() : 0;
  uint64_t acc = 0, rej = 0, n = 0;
  // AGENT_NATIVE29B: with capture v4-full and the SDK v4 batch intake, every v3 / v4 record goes through the v4 batch (it takes
  // both versions); v4 records are only ever built when that export exists.
  const BatchFn batch_fn = (g_capture_full.load(std::memory_order_relaxed) && GetCapApi().submit_v4_batch)
                               ? BatchFn(GetCapApi().submit_v4_batch)
                               : api.submit_v3_batch;
  const bool batch = batch_fn && (g_batch_on.load(std::memory_order_relaxed) || batch_fn != api.submit_v3_batch);
  size_t run_off = 0;
  uint32_t run_n = 0;
  auto flush_run = [&](size_t end_off) {
    if (!run_n) return;
    const int res = batch_fn(blob.data() + run_off, uint32_t(end_off - run_off), run_n);
    if (res >= 0) {
      acc += uint32_t(res);
      rej += run_n - std::min<uint32_t>(run_n, uint32_t(res));
    } else {
      rej += run_n;
    }
    g_ln_batches.fetch_add(1, std::memory_order_relaxed);
    run_n = 0;
  };
  for (size_t off = 0; off < blob.size();) {
    const DrawRecordV2* r = reinterpret_cast<const DrawRecordV2*>(blob.data() + off);
    const bool v3 = r->version == kDrawRecordVersion3 || r->version == 4u;  // 4 = AGENT_NATIVE29B capture v4-full
    const uint32_t size = v3 ? r->size : uint32_t(sizeof(DrawRecordV2));
    if (r->flags & kFlagLocalSkip) {  // AGENT_NATIVE29B: went out OWNED in a run (no CP draw left to verify)
      if (batch) flush_run(off);
      off += size;
      continue;
    }
    if (batch && v3) {
      if (!run_n) run_off = off;
      ++run_n;
    } else {
      if (batch) flush_run(off);
      const int res = v3 ? api.submit_v3(r, size) : api.submit(r, size);
      (res > 0 ? acc : rej) += 1;
    }
    ++n;
    off += size;
  }
  if (batch) flush_run(blob.size());
  if (prof) g_t_sdk.fetch_add(Ticks() - t1, std::memory_order_relaxed);
  g_n_sdk.fetch_add(n, std::memory_order_relaxed);
  g_ln_records.fetch_add(n, std::memory_order_relaxed);
  g_ln_accepted.fetch_add(acc, std::memory_order_relaxed);
  g_ln_rejected.fetch_add(rej, std::memory_order_relaxed);
}
}  // namespace

// FRAME8: lock hold time (kick walk + SubmitCommandStream under the lock), outermost level only.
std::atomic<uint64_t> g_t_hold{0};
thread_local uint32_t t_lock_depth = 0;
thread_local uint64_t t_lock_t0 = 0;
void LockSubmit() {
  {
    TickScope ts(g_t_lock, g_n_lock);
    g_submit_mu.lock();
  }
  if (t_lock_depth++ == 0) t_lock_t0 = Ticks();
}
void UnlockSubmit() {
  if (--t_lock_depth == 0) g_t_hold.fetch_add(Ticks() - t_lock_t0, std::memory_order_relaxed);
  g_submit_mu.unlock();
}

namespace {
void OnKickWalk(uint8_t* base, uint32_t r3, uint32_t gpu_addr, uint32_t dwords);
}  // namespace

void OnKick(uint8_t* base, uint32_t r3, uint32_t gpu_addr, uint32_t dwords) {
  if (!Enabled()) return;
  if (!g_meta.load(std::memory_order_relaxed)) {
    OnKickWalk(base, r3, gpu_addr, dwords);
    return;
  }
  // FRAME9 draw metadata: the batch of this root stream goes out last, right before the caller's SubmitCommandStream.
  thread_local std::vector<uint8_t> batch;
  const uint32_t dev = PlausibleDevice(base, r3) ? r3 : 0u;
  if (g_meta_only.load(std::memory_order_relaxed)) {
    ++Kick().epoch;  // writers spanning this kick drop their forward walk
    TickScope ts(g_t_meta, g_n_meta);
    MetaBuild(dev, gpu_addr & 0x1FFFFFFFu, dwords, 0, batch);
    if (dwords && dwords < 0x400000u) MetaSend(batch);  // only when the caller really submits the stream
    return;
  }
  {
    TickScope ts(g_t_meta, g_n_meta);
    MetaBuild(dev, gpu_addr & 0x1FFFFFFFu, dwords, 0, batch);
  }
  if (g_lean.load(std::memory_order_relaxed)) {
    if (!g_meta_index) g_meta_index = new MetaIndex();  // lives for the process (bridge on only)
    g_meta_index->Build(batch);
    g_meta_index_valid = true;
  }
  OnKickWalk(base, r3, gpu_addr, dwords);
  g_meta_index_valid = false;
  if (dwords && dwords < 0x400000u) MetaSend(batch);  // only when the caller really submits the stream
}

void OnTileSubmit() {
  if (!g_meta.load(std::memory_order_relaxed)) return;
  thread_local std::vector<uint8_t> batch;
  MetaBuild(0, 0, 0, kDrawMetaBatchTileControl, batch);
  MetaSend(batch);
}

namespace {
void OnKickWalk(uint8_t* base, uint32_t r3, uint32_t gpu_addr, uint32_t dwords) {
  // The segment [gpu_addr, +dwords) is about to be handed to the GPU. Walk exactly that range (and the
  // buffers it calls), as the CP will execute it, with this thread's kick mirror; every executed draw packet
  // whose end no hooked writer recorded gets a path-9 record. Records are submitted only when the whole walk
  // tiled exactly.
  TickScope ts_kick(g_t_kick, g_n_kick);
  if (g_lean.load(std::memory_order_relaxed)) {
    OnKickLean(base, r3, gpu_addr, dwords);
    return;
  }
  KickState& k = Kick();
  g_kick_scans.fetch_add(1, std::memory_order_relaxed);
  {
    const uint32_t tid = GetCurrentThreadId();
    if (g_last_kick_tid.exchange(tid, std::memory_order_relaxed) != tid)
      g_kick_thread_switch.fetch_add(1, std::memory_order_relaxed);
  }
  StreamMirror* kmp = &k.mirror;
  if (!g_thread_kick.load(std::memory_order_relaxed)) {
    if (!g_shared_kick_mirror) {
      g_shared_kick_mirror = new StreamMirror();  // lives for the process (verification only)
      g_shared_kick_mirror->Reset();
      g_shared_kick_mirror->device = 0;
    }
    const uint32_t gen = g_gate_gen.load(std::memory_order_acquire);
    if (g_shared_kick_gen != gen) {
      g_shared_kick_gen = gen;
      g_shared_kick_mirror->Reset();
      g_shared_kick_mirror->device = 0;
    }
    kmp = g_shared_kick_mirror;
    g_kick_shared.fetch_add(1, std::memory_order_relaxed);
  }
  StreamMirror& km = *kmp;
  const bool in_writer = t_writer_depth != 0;
  if (in_writer) g_kick_in_writer.fetch_add(1, std::memory_order_relaxed);
  const uint32_t thread_dev = t_mirror ? t_mirror->device : 0u;
  uint32_t dev = 0;
  if (PlausibleDevice(base, r3)) {
    dev = r3;
    g_kick_dev_r3.fetch_add(1, std::memory_order_relaxed);
    if (thread_dev && thread_dev != r3) g_kick_dev_differs.fetch_add(1, std::memory_order_relaxed);
  } else if (thread_dev && PlausibleDevice(base, thread_dev)) {
    dev = thread_dev;
    g_kick_dev_thread.fetch_add(1, std::memory_order_relaxed);
  }
  const uint32_t from = PhysView(gpu_addr);
  const uint32_t to = from + dwords * 4u;
  const uint32_t from_gpu = gpu_addr & 0x1FFFFFFFu, to_gpu = from_gpu + dwords * 4u;
  ++k.epoch;
  bool overlap = false;
  for (const KickState::Range& r : k.recent) {
    if (r.epoch && from_gpu < r.to && to_gpu > r.from) overlap = true;
  }
  if (overlap) g_kick_overlap.fetch_add(1, std::memory_order_relaxed);
  if (!dwords || dwords >= 0x400000u || to < from || to_gpu > 0x20000000u) {
    g_kick_too_big.fetch_add(1, std::memory_order_relaxed);
    km.Reset();
  } else if (!dev) {
    g_kick_no_device.fetch_add(1, std::memory_order_relaxed);
    km.Reset();
    SubmitHeldInRange(from_gpu, to_gpu);
  } else {
    if (km.device != dev) {
      km.device = dev;
      km.Reset();
      g_kick_mirror_resets.fetch_add(1, std::memory_order_relaxed);
    }
    g_kick_dwords.fetch_add(dwords, std::memory_order_relaxed);
    km.bin_mask = g_bin_mask.load(std::memory_order_relaxed);
    km.bin_select = g_bin_select.load(std::memory_order_relaxed);
    if (!PacketsTile(base, from, to)) {
      km.Reset();
      g_kick_bad_walk.fetch_add(1, std::memory_order_relaxed);
      SubmitHeldInRange(from_gpu, to_gpu);
    } else {
      k.out.clear();
      k.hits.clear();
      k.nested.clear();
      k.v3.clear();
      k.v3_words.clear();
      KickEmit emit{base, dev, &k, 0x90000000u | (in_writer ? 2u : 0u) | (overlap ? 4u : 0u), false};
      t_kick_walk = true;
      const uint64_t t_apply0 = Ticks();
      const bool ok = ApplyPackets(base, km, from, to, 0, &emit) && !emit.failed;
      g_t_apply.fetch_add(Ticks() - t_apply0, std::memory_order_relaxed);
      t_kick_walk = false;
      g_bin_mask.store(km.bin_mask, std::memory_order_relaxed);
      g_bin_select.store(km.bin_select, std::memory_order_relaxed);
      if (!ok) {
        km.Reset();
        g_kick_nested_bad.fetch_add(1, std::memory_order_relaxed);
        // The writer records this walk took go out as the writers made them.
        for (const DrawRecordV2& r : k.hits) {
          SubmitRecord(r);
          g_held_fallback.fetch_add(1, std::memory_order_relaxed);
        }
        SubmitHeldInRange(from_gpu, to_gpu);
        // Nested buffers of this segment the CP may run (at least their valid prefix) lie outside its range.
        for (const auto& nr : k.nested) {
          SubmitHeldInRange(nr.first, nr.second);
          g_held_nested_flush.fetch_add(1, std::memory_order_relaxed);
        }
      } else {
        const Api& api = GetApi();
        for (size_t i = 0; i < k.out.size(); ++i) {
          if (!api.submit) break;
          const DrawRecordV2& r = k.out[i];
          if (i < k.v3.size() && k.v3[i].kind != 0 && api.submit_v3) {
            SubmitV3(r, k.v3[i], k.v3_words.data());
          } else {
            SubmitRecord(r);
          }
          if (r.path_id == kPathGap) {
            g_gap_draws.fetch_add(1, std::memory_order_relaxed);
            g_p9_tag[r.caller & 15].fetch_add(1, std::memory_order_relaxed);
            if (in_writer) k.inflight_p9.push_back(GpuAddr(r.packet_end));
          }
        }
        k.recent[k.epoch & 15] = {from_gpu, to_gpu, k.epoch};
      }
    }
  }
}
}  // namespace

void OnTileSelect(const uint32_t* words_be, uint32_t n) {
  // SET_BIN_* packets the tiling executor wrote straight into the primary ring (forwarded to the CP by
  // 825DF0B8 in submission order): keep the CP-order bin state the kick walks test predicated packets with.
  if (!Enabled()) return;
  g_tile_selects.fetch_add(1, std::memory_order_relaxed);
  uint64_t mask = g_bin_mask.load(std::memory_order_relaxed), select = g_bin_select.load(std::memory_order_relaxed);
  uint32_t i = 0;
  while (i < n) {
    const uint32_t h = _byteswap_ulong(words_be[i]);
    const uint32_t type = h >> 30;
    const uint32_t body = h == 0 ? 0u : type == 1 ? 2u : type == 2 ? 0u : ((h >> 16) & 0x3FFFu) + 1;
    if (type == 3 && h != 0 && i + body < n) {
      const uint32_t opcode = (h >> 8) & 0x7Fu;
      const bool run = !(h & 1) || (select & mask);
      auto w = [&](uint32_t j) { return uint64_t(_byteswap_ulong(words_be[i + j])); };
      if (run && (opcode == 0x50 || opcode == 0x51) && body >= 2) {
        (opcode == 0x50 ? mask : select) = (w(1) << 32) | w(2);
      } else if (run && opcode >= 0x60 && opcode <= 0x63 && body >= 1) {
        uint64_t& t = opcode < 0x62 ? mask : select;
        t = (opcode & 1) ? ((t & 0xFFFFFFFFull) | (w(1) << 32)) : ((t & 0xFFFFFFFF00000000ull) | w(1));
      }
    }
    i += body + 1;
  }
  g_bin_mask.store(mask, std::memory_order_relaxed);
  g_bin_select.store(select, std::memory_order_relaxed);
}

void FrameBoundary() {
  const bool was_enabled = g_gate_enabled.load(std::memory_order_acquire);
  const bool enabled = VerificationEnabled();
  if (!was_enabled && !enabled) {
    return;
  }
  const uint64_t completed = g_frame_id.fetch_add(1, std::memory_order_acq_rel);
  // Enabling is observed here, after this frame's draws. Start with the next frame rather than
  // reporting a misleading partial first frame to the SDK.
  if (!was_enabled) {
    return;
  }
  const Api& api = GetApi();
  if (api.boundary) {
    api.boundary(completed);
  }
  if ((completed & 3) == 0) g_held_left.store(ExpireHeld(), std::memory_order_relaxed);

  static uint32_t log_countdown = 1;
  if (--log_countdown == 0) {
    log_countdown = 600;
    {
      static const double us_per_tick = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return 1e6 / double(f.QuadPart);
      }();
      auto us = [](std::atomic<uint64_t>& a) { return double(a.exchange(0)) * us_per_tick; };
      const double ctor = us(g_t_ctor), dtor = us(g_t_dtor), kick = us(g_t_kick), apply = us(g_t_apply),
                   sdk = us(g_t_sdk), lock = us(g_t_lock), hold = us(g_t_hold);
      const uint64_t nc = g_n_ctor.exchange(0), nd = g_n_dtor.exchange(0), nk = g_n_kick.exchange(0),
                     ns = g_n_sdk.exchange(0), nl = g_n_lock.exchange(0);
      REXLOG_INFO(
          "NATIVE FRAME BRIDGE cost (600 frames, ms total / us per call): writer ctor {:.0f} / {:.2f} ({} calls) | writer "
          "dtor (decode+FillState+hold/submit) {:.0f} / {:.2f} | kick walk {:.0f} / {:.1f} ({} kicks; ApplyPackets {:.0f}) | "
          "SDK submit calls (incl. v3 build) {:.0f} / {:.2f} ({} records) | submit-lock wait {:.0f} / {:.2f} ({})",
          ctor / 1000, nc ? ctor / nc : 0.0, nc, dtor / 1000, nd ? dtor / nd : 0.0, kick / 1000, nk ? kick / nk : 0.0,
          nk, apply / 1000, sdk / 1000, ns ? sdk / ns : 0.0, ns, lock / 1000, nl ? lock / nl : 0.0, nl);
      {  // MAIN31
        const double runs_us = us(g_t_runs), runsdk_us = us(g_t_runsdk);
        REXLOG_INFO("NATIVE FRAME BRIDGE own-runs cost (600 frames, ms total): CapApplyRuns {:.0f} (of it SDK batch calls {:.0f}) "
                    "| walk/run building = ApplyPackets - CapApplyRuns", runs_us / 1000, runsdk_us / 1000);
      }
      REXLOG_INFO(
          "NATIVE FRAME BRIDGE lean (FRAME8): {} | kicks {} dwords {} bad walk {} records dropped {} | writer entries held {} "
          "evicted {} taken {} stale {} | path 9 {} | records {} accepted {} rejected {} v2 (unpublished) {} with values {} "
          "({} words) over budget {} | groups re-hashed {} partial {} | mask queries {} | records with < 64 known state regs {}",
          g_lean.load(std::memory_order_relaxed)
              ? (g_lean_nohash.load(std::memory_order_relaxed)
                     ? (g_lean_values.load(std::memory_order_relaxed) ? "on, no hashes" : "on, no hashes, no values")
                     : (g_lean_values.load(std::memory_order_relaxed) ? "on" : "on, no values"))
              : "off",
          g_ln_kicks.exchange(0), g_ln_dwords.exchange(0), g_ln_bad_walk.exchange(0), g_ln_dropped.exchange(0),
          g_ln_held.exchange(0), g_ln_evicted.exchange(0), g_ln_taken.exchange(0), g_ln_stale.exchange(0),
          g_ln_gap.exchange(0), g_ln_records.exchange(0), g_ln_accepted.exchange(0), g_ln_rejected.exchange(0),
          g_ln_unpub.exchange(0), g_ln_values.exchange(0), g_ln_value_words.exchange(0), g_ln_novalues.exchange(0),
          g_ln_dirty_groups.exchange(0), g_ln_partial_groups.exchange(0), g_ln_mask_queries.exchange(0), g_ln_state_partial.exchange(0));
      REXLOG_INFO("NATIVE FRAME BRIDGE cost 2 (FRAME8): submit-lock held (kick walk + SubmitCommandStream) {:.0f} ms / {:.2f} us",
                  hold / 1000, nl ? hold / nl : 0.0);
      std::string fields;
      static const char* kMetaField[6] = {"pass", "path", "caller", "mesh", "instance", "engine-args"};
      for (uint32_t b = 0; b < 6; ++b) {
        const uint64_t v = g_mt_chk_diff_field[b].exchange(0);
        if (v) fields += std::string(" ") + kMetaField[b] + ":" + std::to_string(v);
      }
      const double mt = us(g_t_meta);
      const uint64_t nm = g_n_meta.exchange(0);
      REXLOG_INFO(
          "NATIVE FRAME BRIDGE draw-meta (FRAME9): {} | staged ~{} sent {} expired at kick {} pruned by writers {} left staged after kicks (sum) {} overflow {} "
          "no stage {} | batches {} (empty {} tile control {} incomplete {}) build {:.0f} ms / {:.2f} us per kick | SDK "
          "intake {} ok {} bad {} | lean batches {} | check vs kick walk: writer records match {} missing at depth 0 {} "
          "nested {} differ {}{} | path-9 draws found in the batch {} not found {}",
          g_meta_only.load(std::memory_order_relaxed) ? "ONLY (no kick walk)"
                                                      : g_meta.load(std::memory_order_relaxed) ? "on + lean check" : "off",
          g_mt_staged.exchange(0), g_mt_sent.exchange(0), g_mt_expired.exchange(0), g_mt_pruned.exchange(0),
          g_mt_kept.exchange(0),
          g_mt_overflow.exchange(0), g_mt_no_stage.exchange(0), g_mt_batches.exchange(0), g_mt_empty_batches.exchange(0),
          g_mt_tile_batches.exchange(0), g_mt_incomplete.exchange(0), mt / 1000, nm ? mt / nm : 0.0,
          GetApi().submit_meta ? "found" : "absent", g_mt_sdk_ok.exchange(0), g_mt_sdk_bad.exchange(0),
          g_ln_batches.exchange(0), g_mt_chk_match.exchange(0), g_mt_chk_miss0.exchange(0),
          g_mt_chk_miss_nested.exchange(0), g_mt_chk_diff.exchange(0), fields, g_mt_chk_p9_hit.exchange(0),
          g_mt_chk_p9_miss.exchange(0));
    }
    REXLOG_INFO(
        "NATIVE FRAME BRIDGE: through frame {} writer calls {} records {} accepted {} rejected {} errors {} | "
        "calls without a draw packet {} (segment switch found by backscan {}, not found {}, parse aborted {}) "
        "bad device {} (verification only)",
        completed, g_writer_calls.exchange(0), g_submitted.exchange(0), g_accepted.exchange(0),
        g_rejected.exchange(0), g_errors.exchange(0), g_no_draw.exchange(0), g_backscan.exchange(0),
        g_not_found.exchange(0), g_parse_abort.exchange(0), g_bad_device.exchange(0));
    REXLOG_INFO(
        "NATIVE FRAME BRIDGE stream mirror: gaps scanned {} ({} dwords) lost {} resets {} | type-0 const words {} "
        "fetch words {} | SET_CONSTANT(2) {} LOAD_ALU_CONSTANT {} ({} words) SET_SHADER_CONSTANTS {} | IBs walked {} "
        "skipped {} | records using the stream {} (vs groups {} ps groups {} fetch units {}) | draws outside "
        "the writers {} (kick scans {})",
        g_gap_scanned.exchange(0), g_gap_dwords.exchange(0), g_gap_lost.exchange(0), g_mirror_resets.exchange(0),
        g_t0_const_words.exchange(0), g_t0_fetch_words.exchange(0), g_set_const.exchange(0), g_load_alu.exchange(0),
        g_load_alu_words.exchange(0), g_set_shader_consts.exchange(0), g_ib_calls.exchange(0), g_ib_skipped.exchange(0),
        g_rec_with_stream.exchange(0), g_rec_vs_stream.exchange(0), g_rec_ps_stream.exchange(0),
        g_rec_fetch_stream.exchange(0), g_gap_draws.exchange(0), g_kick_scans.exchange(0));
    REXLOG_INFO(
        "NATIVE FRAME BRIDGE kick walks: {} dwords, bad walk {} too big {} no device {} | device from r3 {} "
        "from thread {} r3 differs from thread {} | kicks inside a writer {} (writer calls spanning a kick {}, "
        "their draws left to the kick walk {}) | draws skipped as writer-recorded {} | kick mirror resets {} "
        "| writer ends cleared {} (verification only)",
        g_kick_dwords.exchange(0), g_kick_bad_walk.exchange(0), g_kick_too_big.exchange(0),
        g_kick_no_device.exchange(0), g_kick_dev_r3.exchange(0), g_kick_dev_thread.exchange(0),
        g_kick_dev_differs.exchange(0), g_kick_in_writer.exchange(0), g_writer_kicked.exchange(0),
        g_writer_dropped_kicked.exchange(0), g_kick_writer_hits.exchange(0), g_kick_mirror_resets.exchange(0),
        g_kick_ends_cleared.exchange(0));
    std::string tags;
    for (uint32_t t = 0; t < 16; ++t) {
      const uint64_t v = g_p9_tag[t].exchange(0);
      if (v) tags += " " + std::to_string(t) + ":" + std::to_string(v);
    }
    REXLOG_INFO(
        "NATIVE FRAME BRIDGE kick walks 2: kick mirror {} (shared walks {}, kicks on another thread than the previous kick {}) | overlapping kicks {} | predicated draws skipped {} run {} | tile "
        "selects {} | nested walk failed {} | writer ends expired {} | zero NOP headers (writer walks) {} | "
        "path-9 tags (1 nested 2 in writer 4 overlap 8 predicated):{} | writer records merged at execution {} "
        "(state from kick mirror {} shaders from kick mirror {}) submitted unwalked {} | IBs not walked {} (verification only)",
        g_thread_kick.load(std::memory_order_relaxed) ? "per thread" : "shared", g_kick_shared.exchange(0),
        g_kick_thread_switch.exchange(0),
        g_kick_overlap.exchange(0), g_kick_pred_skipped.exchange(0), g_kick_pred_run.exchange(0),
        g_tile_selects.exchange(0), g_kick_nested_bad.exchange(0), g_kick_ends_expired.exchange(0),
        g_nop_headers.exchange(0), tags, g_merged.exchange(0), g_merged_state_kick.exchange(0),
        g_merged_shader_kick.exchange(0), g_held_fallback.exchange(0), g_kick_ib_skipped.exchange(0));
    REXLOG_INFO(
        "NATIVE FRAME BRIDGE kick merge: const groups/fetch units writer-full {} kick-full {} composed per word {} "
        "kick-partial left as written {} | shaders from kick vs {} ps {} | held queued behind an earlier record {} "
        "expired at take {} nested ranges flushed {} held now {} not held (cap) {} (verification only)",
        g_mg_writer_full.exchange(0), g_mg_kick_full.exchange(0), g_mg_composed.exchange(0),
        g_mg_kick_partial_left.exchange(0), g_merged_vs_kick.exchange(0), g_merged_ps_kick.exchange(0),
        g_held_queued.exchange(0), g_held_expired_take.exchange(0), g_held_nested_flush.exchange(0),
        g_held_left.load(std::memory_order_relaxed), g_held_capped.exchange(0));
    std::string dis_regs;
    for (uint32_t i = 0; i < kNativeStateRegCount; ++i) {
      const uint64_t v = g_dis_state_regs[i].exchange(0);
      if (v) {
        char b[32];
        std::snprintf(b, sizeof(b), " %04X:%llu", kNativeStateRegs[i], static_cast<unsigned long long>(v));
        dis_regs += b;
      }
    }
    REXLOG_INFO(
        "NATIVE FRAME BRIDGE writer/kick disagreements (records where both mirrors know a value and differ): rt {} "
        "state {} const groups/fetch units {} | policy {} | state regs:{} (verification only)",
        g_dis_rt.exchange(0), g_dis_state.exchange(0), g_dis_const.exchange(0),
        g_writer_wins.load(std::memory_order_relaxed) ? "writer wins" : "kick wins", dis_regs);
    REXLOG_INFO(
        "NATIVE FRAME BRIDGE v3: {} | records with values {} ({} words, groups with shadow words {}) without values {} "
        "key not published {} switch off {} | v3 accepted {} not accepted {} | LOAD_ALU source virtual view {} "
        "(verification only)",
        g_v3_on.load(std::memory_order_relaxed) ? "on" : "off", g_v3_values.exchange(0), g_v3_value_words.exchange(0),
        g_v3_shadow_groups.exchange(0), g_v3_no_values.exchange(0), g_v3_unpublished.exchange(0), g_v3_off.exchange(0),
        g_v3_accepted.exchange(0), g_v3_rejected.exchange(0), g_load_alu_virtual.exchange(0));
    REXLOG_INFO(
        "NATIVE FRAME BRIDGE M3b markers: {} | appended {} (in tiling layout {}) skipped: kick in call {} shape {} space {} | "
        "walked without a draw before {} walked after a draw {} accepted (id written) {} refused {} malformed {} replays {} no values {} (verification only)",
        g_marker_on.load(std::memory_order_relaxed) ? "on" : "off", g_mk_inserted.exchange(0), g_mk_skip_layout.exchange(0),
        g_mk_skip_kick.exchange(0), g_mk_skip_shape.exchange(0), g_mk_skip_space.exchange(0), g_mk_orphan.exchange(0),
        g_mk_seen.exchange(0),
        g_mk_accepted.exchange(0), g_mk_refused.exchange(0), g_mk_bad.exchange(0), g_mk_replay.exchange(0),
        g_mk_novalues.exchange(0));
  }
}


#include "native_m3.inc"  // M3 executor, game side (AGENT_NATIVE23B2, opt-in native_m3_shadow / native_m3_replace)
}  // namespace native_renderer_bridge
