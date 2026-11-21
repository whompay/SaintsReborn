// AGENT_NATIVE29B (session 29): CAPTURE V4-FULL = self-sufficient draw records from the lean kick walk.
// Included by native_renderer_bridge.cpp inside its anonymous namespace, directly before LeanEmit::Draw (same TU: LeanMirror,
// LeanMaskCache, G32, GpuAddr, kNativeStateRegs, Api / GetApi).
//
// The lean kick walk (dist native_frame_lean) mirrors the device's command stream in CP order before SubmitCommandStream and
// therefore knows, at every draw packet, the full register state the CP will have there: the 64 kNativeStateRegs, all float
// constants and all fetch constants. Until now it only sent hashes (+ v3 const values). With dist 'native_capture_full'
// (read when the bridge is switched on; needs native_bridge_verify or native_frame_exec_dry, plus native_frame_lean and
// native_bridge_v3) every lean-walk draw ALSO:
//   * is counted in a per-pass census (log 'NATIVE CAPTURE CENSUS' every 10 s): pass key = RB_SURFACE_INFO pitch/msaa,
//     colour/depth base, colour mask != 0, PS present; per pass draws/frame, self-sufficient %, why not, top writer callers
//     (+ mesh pass id) and top (VS, PS) guest pairs;
//   * goes out as a record VERSION 4 (instead of v3) when rexgpu-xenos.dll exports RexNativeRendererSubmitDrawRecordsV4Batch:
//       v3 header (version 4, size = total) + values[value_words] (used VS/PS groups, as v3)
//       + RexNativeDrawRecordV4Ext {magic 'M3X4', ext_size = 336 + 4 * fetch_value_words, state_values[64] (REG_VALUES if all 64
//         known), fetch_unit_mask[3], fetch_value_words = 2 * units, draw_packet_words/draw_packet[8] = the DRAW packet}
//       + fetch_values[fetch_value_words] (2 host-order words per included unit, ascending). No CALL_WORDS.
//     flags: VERIFY_ONLY | REG_VALUES (2048) | SELF_SUFFICIENT (16384, proposed) | CONST_VALUES as v3.
//     Fetch units included: the used units published by RexNativeRendererGetDrawMasksV2 (fetch_units) when non-zero, else every
//     unit whose two words the mirror knows (fallback, up to 96 units).
//   SELF_SUFFICIENT = all 64 state registers known + every used const group fully known by the mirror (no device-shadow word)
//     + the used fetch mask published and every used unit fully known.
// Off (no native_capture_full): one relaxed load per lean draw. Nothing here changes what the CP executes.
// g_capture_full: declared in native_renderer_bridge.cpp (read in VerificationEnabled).
constexpr uint32_t kV4Version = 4u;
constexpr uint32_t kV4ExtMagic = 0x34584D33u;  // REX_NATIVE_DRAW_V4_EXT_MAGIC
constexpr uint32_t kFlagRegValues = 2048u;     // REX_NATIVE_DRAW_FLAG_REG_VALUES
constexpr uint32_t kFlagSelfSufficient = 16384u;  // proposed REX_NATIVE_DRAW_FLAG_SELF_SUFFICIENT (29B 2026-11-16)
struct alignas(8) CapV4Ext {  // == RexNativeDrawRecordV4Ext (native_m3_exec.inc), 336 bytes
  uint32_t magic;
  uint32_t ext_size;
  uint32_t state_values[64];
  uint32_t fetch_unit_mask[3];
  uint32_t fetch_value_words;
  uint32_t draw_packet_words;
  uint32_t draw_packet[8];
  uint32_t reserved[5];
};
static_assert(sizeof(CapV4Ext) == 336);
static_assert(offsetof(CapV4Ext, fetch_unit_mask) == 264);
static_assert(offsetof(CapV4Ext, draw_packet_words) == 280);
static_assert(offsetof(CapV4Ext, draw_packet) == 284);
static_assert(offsetof(CapV4Ext, reserved) == 316);
struct CapMasksV2 {  // == RexNativeDrawMasksV2 (40 B)
  uint64_t vs_groups, ps_groups;
  uint32_t fetch_units[3];
  uint32_t generation;
  uint32_t flags;
  uint32_t reserved;
};
static_assert(sizeof(CapMasksV2) == 40);
using CapBatchFn = int (*)(const void* blob, uint32_t bytes, uint32_t count);
using CapMasksV2Fn = int (*)(uint32_t vs_guest, uint32_t ps_guest, uint32_t decl, void* out);
struct CapApi {
  CapBatchFn submit_v4_batch = nullptr;  // RexNativeRendererSubmitDrawRecordsV4Batch (29A, proposed)
  CapMasksV2Fn masks_v2 = nullptr;       // RexNativeRendererGetDrawMasksV2
};
const CapApi& GetCapApi() {
  static const CapApi a = [] {
    CapApi r;
    if (HMODULE gpu = GetModuleHandleW(L"rexgpu-xenos.dll")) {
      r.submit_v4_batch = reinterpret_cast<CapBatchFn>(GetProcAddress(gpu, "RexNativeRendererSubmitDrawRecordsV4Batch"));
      r.masks_v2 = reinterpret_cast<CapMasksV2Fn>(GetProcAddress(gpu, "RexNativeRendererGetDrawMasksV2"));
    }
    REXLOG_INFO("NATIVE CAPTURE V4-FULL (29B): SDK v4 batch intake {}, draw masks v2 {}",
                r.submit_v4_batch ? "found" : "MISSING (census only, records stay v3)", r.masks_v2 ? "found" : "missing");
    return r;
  }();
  return a;
}

// ---- census (all of it runs inside OnKickLean under g_submit_mu: plain counters) ----
enum CapWhy : uint32_t { kCwState = 0, kCwConstUnpub, kCwConstShadow, kCwFetchUnpub, kCwFetchPartial, kCwGap, kCwCount };
static const char* const kCapWhyName[kCwCount] = {"state-partial", "const-unpub", "const-shadow", "fetch-unpub",
                                                  "fetch-partial", "gap(no writer rec)"};
struct CapTop {  // tiny top-k counter (replace the smallest)
  static constexpr uint32_t kN = 8;
  uint64_t key[kN] = {};
  uint32_t cnt[kN] = {};
  void Add(uint64_t k) {
    uint32_t mi = 0;
    for (uint32_t i = 0; i < kN; ++i) {
      if (cnt[i] && key[i] == k) {
        ++cnt[i];
        return;
      }
      if (cnt[i] < cnt[mi]) mi = i;
    }
    key[mi] = k;
    cnt[mi] = 1;
  }
};
struct CapPass {
  uint64_t key = 0;
  bool used = false;
  uint64_t draws = 0, ss = 0, sent_v4 = 0, why[kCwCount] = {};
  CapTop callers, pairs;
};
struct CapCensus {
  static constexpr uint32_t kPasses = 32;
  CapPass p[kPasses];
  uint64_t other = 0, frame0 = 0, next_log = 0, v4_bytes = 0, v4_sent = 0, fetch_units = 0, fetch_fallback = 0;
  uint32_t last = 0;
  CapPass* Find(uint64_t key) {
    if (p[last].used && p[last].key == key) return &p[last];
    for (uint32_t i = 0; i < kPasses; ++i) {
      if (p[i].used && p[i].key == key) return &p[last = i];
      if (!p[i].used) {
        p[i] = CapPass{};
        p[i].used = true;
        p[i].key = key;
        return &p[last = i];
      }
    }
    return nullptr;
  }
};
CapCensus* g_cap = nullptr;

void CapRunLog(double frames);  // step 2 stats, below

void CapLog(CapCensus& c, uint64_t now_ms) {
  const uint64_t f1 = g_frame_id.load(std::memory_order_relaxed);
  const double frames = f1 > c.frame0 ? double(f1 - c.frame0) : 1.0;
  CapPass* order[CapCensus::kPasses];
  uint32_t n = 0;
  uint64_t total = 0, total_ss = 0;
  for (CapPass& x : c.p) {
    if (x.used && x.draws) {
      order[n++] = &x;
      total += x.draws;
      total_ss += x.ss;
    }
  }
  std::sort(order, order + n, [](const CapPass* a, const CapPass* b) { return a->draws > b->draws; });
  REXLOG_INFO("NATIVE CAPTURE CENSUS (29B, {:.0f} frames): {:.1f} draws/frame, self-sufficient {:.2f} %, other-pass draws {:.1f}/frame, "
              "v4 sent {:.1f}/frame ({:.1f} KB/frame), fetch units/record {:.1f} (all-known fallback {:.1f} %)",
              frames, total / frames, total ? 100.0 * total_ss / total : 0.0, c.other / frames, c.v4_sent / frames,
              c.v4_bytes / frames / 1024.0, c.v4_sent ? double(c.fetch_units) / c.v4_sent : 0.0,
              c.v4_sent ? 100.0 * c.fetch_fallback / c.v4_sent : 0.0);
  for (uint32_t i = 0; i < n && i < 12; ++i) {
    CapPass& x = *order[i];
    const uint64_t k = x.key;
    std::string why, callers, pairs;
    for (uint32_t w = 0; w < kCwCount; ++w) {
      if (x.why[w]) why += fmt::format(" {} {:.1f}", kCapWhyName[w], x.why[w] / frames);
    }
    // callers: key = caller << 32 | path << 24 | (pass_id & 0xFFFFFF)
    for (uint32_t j = 0; j < CapTop::kN; ++j) {
      if (x.callers.cnt[j])
        callers += fmt::format(" {:08X}/p{}/m{:X}:{:.1f}", uint32_t(x.callers.key[j] >> 32),
                               uint32_t(x.callers.key[j] >> 24) & 0xFFu, uint32_t(x.callers.key[j]) & 0xFFFFFFu,
                               x.callers.cnt[j] / frames);
      if (x.pairs.cnt[j])
        pairs += fmt::format(" {:08X}/{:08X}:{:.1f}", uint32_t(x.pairs.key[j] >> 32), uint32_t(x.pairs.key[j]),
                             x.pairs.cnt[j] / frames);
    }
    REXLOG_INFO("NATIVE CAPTURE CENSUS pass pitch {} msaa {} cbase {:X} dbase {:X} {}{}: {:.1f} draws/frame, self-sufficient {:.2f} %, "
                "v4 {:.1f} | not because:{} | callers/path/mesh-pass:{} | top VS/PS (guest, first sampled):{}",
                k & 0x3FFFu, (k >> 14) & 3u, (k >> 16) & 0xFFFu, (k >> 28) & 0xFFFu, ((k >> 40) & 1) ? "colour" : "depth-only",
                ((k >> 41) & 1) ? " +PS" : " no-PS", x.draws / frames, x.draws ? 100.0 * x.ss / x.draws : 0.0, x.sent_v4 / frames,
                why.empty() ? " -" : why, callers, pairs);
  }
  if (g_capture_own.load(std::memory_order_relaxed)) CapRunLog(frames);
  for (CapPass& x : c.p) x = CapPass{};
  c.other = c.v4_bytes = c.v4_sent = c.fetch_units = c.fetch_fallback = 0;
  c.last = 0;
  c.frame0 = f1;
  c.next_log = now_ms + 10000;
}

// MAIN31: why a v4 draw was not ownable (0 ownable, 1 not self-sufficient, 2 no SDK bit 2, 3 nested, 4 predicated,
// 5 short DRAW, 6 VS from IM_LOAD_IMMEDIATE, 7 tiled layout without native_capture_own_tiled).
std::atomic<uint64_t> g_own_why[8] = {};
static const char* const kOwnWhyName[8] = {"ownable", "not-ss", "no-sdk-bit2", "nested", "predicated", "short-draw", "imm-vs", "tiled"};

// Single-entry cache of the v2 draw masks (fetch units), valid within one kick (stamp of the lean mask cache).
struct CapFetchMaskEntry {
  uint32_t vs = 0, ps = 0, decl = 0, stamp = 0;
  bool published = false;
  uint32_t units[3] = {0, 0, 0};
  uint32_t flags = 0;  // RexNativeDrawMasksV2 flags (bit 2 = RECORD_OWNABLE, proposed)
};

// Census + (with the SDK v4 intake) the v4 record. Returns true when a v4 record was appended to the blob (the caller then
// skips its own v3 / v2 record of this draw).
bool CaptureFullDraw(const uint8_t* base, uint32_t dev, LeanMirror& m, LeanBlob* blob, LeanMaskCache* mc,
                     const LeanMaskEntry& c, DrawRecordV3Header& hd, uint32_t at, uint32_t n, uint32_t header,
                     bool held, bool values, int depth, bool* ownable) {
  DrawRecordV2& r = hd.v2;
  *ownable = false;
  if (!g_cap) {
    g_cap = new CapCensus();  // lives for the process (bridge on only)
    g_cap->frame0 = g_frame_id.load(std::memory_order_relaxed);
    g_cap->next_log = GetTickCount64() + 10000;
  }
  CapCensus& cs = *g_cap;
  const uint64_t now = GetTickCount64();
  if (now >= cs.next_log) CapLog(cs, now);
  // Pass key from the CP-order state (fallback: the record's fields).
  uint32_t si = r.surface_info, ci = 0, di = 0;
  m.StateReg(0, si);  // kNativeStateRegs[0] = 0x2000 RB_SURFACE_INFO
  m.StateReg(1, ci);  // 0x2001 RB_COLOR_INFO
  m.StateReg(2, di);  // 0x2002 RB_DEPTH_INFO
  const uint64_t key = uint64_t(si & 0x3FFFu) | (uint64_t((si >> 16) & 3u) << 14) | (uint64_t(ci & 0xFFFu) << 16) |
                       (uint64_t(di & 0xFFFu) << 28) | (uint64_t(r.color_mask != 0) << 40) |
                       (uint64_t(r.pixel_shader_guest != 0) << 41);
  CapPass* ps = cs.Find(key);
  if (!ps) {
    ++cs.other;
  } else {
    ++ps->draws;
    ps->callers.Add((uint64_t(r.caller) << 32) | (uint64_t(r.path_id & 0xFFu) << 24) | (r.pass_id & 0xFFFFFFu));
    ps->pairs.Add((uint64_t(r.vertex_shader_guest) << 32) | r.pixel_shader_guest);
  }
  // Self-sufficiency.
  bool why[kCwCount] = {};
  why[kCwState] = m.state_known != ~0ull;
  why[kCwGap] = !held;
  uint32_t nwords = 0;
  bool const_mirror = true;
  if (!c.published) {
    why[kCwConstUnpub] = true;
  } else {
    for (uint32_t kind = 0; kind < 2; ++kind) {
      for (uint64_t bits = c.mk[kind]; bits; bits &= bits - 1) {
        if (!m.CFull(kind * 64 + uint32_t(std::countr_zero(bits)))) const_mirror = false;
      }
    }
    why[kCwConstShadow] = !const_mirror;
    if (values) nwords = 16u * uint32_t(std::popcount(c.mk[0]) + std::popcount(c.mk[1]));
    if (nwords > kDrawRecordV3MaxWords) nwords = 0;
  }
  const CapApi& capi = GetCapApi();
  thread_local CapFetchMaskEntry fe;
  if (capi.masks_v2 && (fe.stamp != mc->stamp || fe.vs != r.vertex_shader_guest || fe.ps != r.pixel_shader_guest ||
                        fe.decl != r.vertex_decl_guest)) {
    fe.stamp = mc->stamp;
    fe.vs = r.vertex_shader_guest;
    fe.ps = r.pixel_shader_guest;
    fe.decl = r.vertex_decl_guest;
    CapMasksV2 mv{};
    fe.published = capi.masks_v2(fe.vs, fe.ps, fe.decl, &mv) == 1 &&
                   (mv.fetch_units[0] | mv.fetch_units[1] | mv.fetch_units[2]) != 0;
    for (uint32_t i = 0; i < 3; ++i) fe.units[i] = fe.published ? mv.fetch_units[i] : 0u;
    fe.flags = mv.flags;
  }
  const bool fetch_pub = capi.masks_v2 && fe.published;
  uint32_t fmask[3] = {0, 0, 0};
  if (fetch_pub) {
    for (uint32_t i = 0; i < 3; ++i) fmask[i] = fe.units[i];
    for (uint32_t u = 0; u < 96; ++u) {
      if (((fmask[u >> 5] >> (u & 31)) & 1) && !m.FFull(u)) why[kCwFetchPartial] = true;
    }
  } else {
    why[kCwFetchUnpub] = true;
    for (uint32_t u = 0; u < 96; ++u) {
      if (m.FFull(u)) fmask[u >> 5] |= 1u << (u & 31);
    }
  }
  const bool ss = !why[kCwState] && !why[kCwConstUnpub] && !why[kCwConstShadow] && !why[kCwFetchUnpub] &&
                  !why[kCwFetchPartial];
  if (ps) {
    if (ss) ++ps->ss;
    for (uint32_t w = 0; w < kCwCount; ++w) ps->why[w] += why[w] ? 1u : 0u;
  }
  // v4 record only with the SDK intake, v3 intake semantics otherwise unchanged (the caller sends v3 / v2).
  // AGENT_NATIVE30B: also without published const masks (then no const values, not SELF_SUFFICIENT): the SDK still verifies
  // state_values / fetch values / DRAW header (c30b: the SDK built for n29a publishes no masks in verify mode -> 0 v4 sent).
  if (!capi.submit_v4_batch) return false;
  const uint32_t units = uint32_t(std::popcount(fmask[0]) + std::popcount(fmask[1]) + std::popcount(fmask[2]));
  const uint32_t fwords = units * 2u;
  const uint32_t size = kDrawRecordV3HeaderSize + 4u * nwords + uint32_t(sizeof(CapV4Ext)) + 4u * fwords;
  r.version = kV4Version;
  r.size = size;
  r.flags |= kFlagVerificationOnly;
  if (m.state_known == ~0ull) r.flags |= kFlagRegValues;
  if (ss) r.flags |= kFlagSelfSufficient;
  hd.mask_generation = c.gen;
  hd.draw_header = header;
  if (nwords) {
    r.flags |= kFlagConstValues;
    hd.vs_group_mask = c.mk[0];
    hd.ps_group_mask = c.mk[1];
    hd.value_words = nwords;
  } else {  // AGENT_NATIVE30B: SDK ParseV4 requires value_words == 16 * groups -> no groups without values
    r.flags &= ~kFlagConstValues;
    hd.vs_group_mask = 0;
    hd.ps_group_mask = 0;
    hd.value_words = 0;
  }
  uint8_t* dst = blob->Grow(size);
  std::memcpy(dst, &hd, sizeof(hd));
  uint32_t* out = reinterpret_cast<uint32_t*>(dst + sizeof(hd));
  for (uint32_t kind = 0; kind < 2 && nwords; ++kind) {
    for (uint64_t bits = c.mk[kind]; bits; bits &= bits - 1) {
      const uint32_t g = kind * 64 + uint32_t(std::countr_zero(bits));
      const uint32_t first = g * 16;
      const uint32_t shadow = dev + (g < 64 ? kDevVsConst + g * 64 : kDevPsConst + (g - 64) * 64);
      for (uint32_t i = 0; i < 16; ++i) {
        out[i] = ((m.alu_valid[(first + i) >> 6] >> ((first + i) & 63)) & 1) ? m.alu[first + i] : G32(base, shadow + i * 4);
      }
      out += 16;
    }
  }
  CapV4Ext x;
  std::memset(&x, 0, sizeof(x));
  x.magic = kV4ExtMagic;
  x.ext_size = uint32_t(sizeof(CapV4Ext)) + 4u * fwords;
  for (uint32_t i = 0; i < kNativeStateRegCount; ++i) x.state_values[i] = ((m.state_known >> i) & 1) ? m.state[i] : 0u;
  for (uint32_t i = 0; i < 3; ++i) x.fetch_unit_mask[i] = fmask[i];
  x.fetch_value_words = fwords;
  x.draw_packet_words = std::min<uint32_t>(n + 1u, 8u);
  for (uint32_t i = 0; i < x.draw_packet_words; ++i) x.draw_packet[i] = G32(base, at + 4u * i);
  std::memcpy(out, &x, sizeof(x));
  uint32_t* fv = reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(out) + sizeof(x));
  for (uint32_t u = 0; u < 96; ++u) {
    if (!((fmask[u >> 5] >> (u & 31)) & 1)) continue;
    for (uint32_t i = 0; i < 2; ++i) {
      const uint32_t j = u * 2 + i;
      *fv++ = ((m.fetch_valid[j >> 6] >> (j & 63)) & 1) ? m.fetch[j] : G32(base, dev + kDevFetch + j * 4);
    }
  }
  if (ps) ++ps->sent_v4;
  ++cs.v4_sent;
  cs.v4_bytes += size;
  cs.fetch_units += units;
  if (!fetch_pub) ++cs.fetch_fallback;
  // Step 2 eligibility (owned runs): self-sufficient, SDK says RECORD_OWNABLE for the key, top-level, not predicated, DRAW packet
  // >= 3 dwords (room for the run marker), direct (not tiled) device layout.
  // AGENT_NATIVE30B2: never own a draw whose VS came from IM_LOAD_IMMEDIATE (vertex_shader_guest 0 = D3D runtime clears,
  // 32/frame on the 1280 main pass, path 9 without writer record): the SDK executor maps the active VS by guest address.
  // MAIN31: the device is in the deferred-tiling layout (+10808 bit 1) for ~100 % of draws (every M3b marker ever appended was
  // 'in tiling layout') -> the old hard guard made every draw non-ownable (n30o: runs 0). dist native_capture_own_tiled lifts
  // it (region still ends at the DRAW, the D3D-patched SET_BIN_MASK / EVENT_WRITE packets after it stay outside the region).
  // First failing reason counted in g_own_why (logged with OWNED RUNS).
  {
    uint32_t w = 0;
    if (!ss) w = 1;
    else if ((fe.flags & 4u) == 0) w = 2;
    else if (depth != 0) w = 3;
    else if ((header & 1u) && !g_capture_own_pred.load(std::memory_order_relaxed)) w = 4;
    else if (n + 1u < 3u) w = 5;
    else if (r.vertex_shader_guest == 0) w = 6;
    else if ((*GuestPtr(base, dev + 10808) & 2) && !g_capture_own_tiled.load(std::memory_order_relaxed)) w = 7;
    if (g_capture_own.load(std::memory_order_relaxed)) g_own_why[w].fetch_add(1, std::memory_order_relaxed);
    *ownable = w == 0;
  }
  return true;
}

// =====================================================================================================================
// STEP 2 (AGENT_NATIVE29B, proposal agreed with 29A 2026-11-16 18:47): OWNED RUNS = the kick walk drops the PM4 of runs of
// record-owned draws and leaves one marker + one coalesced register restore in their place.
//   dist 'native_capture_own' (read at bridge enable; needs native_capture_full) + the SDK V4 batch export (ABI = 29A's
//   18:30 proposal, PERF_AGENTS.md session 29): the run's records go out BEFORE the kick in ONE RexNativeRendererSubmitDrawRecordsV4Batch
//   call, flags OWNED | SELF_SUFFICIENT | REG_VALUES (| CONST_VALUES), VERIFY_ONLY clear, v2.reserved0 = run_id (bit 31 set, never 0),
//   v2.sequence = index in the run. The segment is rewritten only when the call returned n (all accepted).
//   Without the export / flag: nothing here runs.
// A RUN (depth-0 packets of the segment being kicked, in stream order) = maximal contiguous sequence of
//   type-0 writes to covered registers (the 64 kNativeStateRegs, float / fetch constants, bool / loop), type-2 fillers,
//   plain NOPs, SET_CONSTANT (types 0-4, covered registers), SET_CONSTANT2 / SET_SHADER_CONSTANTS (covered), IM_LOAD(_IMMEDIATE),
//   and DRAW packets that are OWNABLE (self-sufficient v4 record sent, SDK masks-v2 flag bit 2 RECORD_OWNABLE, not predicated,
//   draw packet >= 3 dwords, device not in the tiled layout). Anything else ends the run (LOAD_ALU_CONSTANT too: its source is
//   read by the CP at execution time; dist native_capture_own_loadalu would include it with submit-time values - NOT implemented).
// REGION = [first packet of the run, end of its LAST owned DRAW). The region is rewritten in place (after the walk succeeded and the
//   SDK accepted all of the run's records) as:
//   NOP {REX_NATIVE_RUN_MARKER_MAGIC 'NRN4', run_id, n}   (the SDK executes the run's n records, in order, at this point)
//   + the region's last IM_LOAD / IM_LOAD_IMMEDIATE packet per stage (verbatim)
//   + type-0 packets with the FINAL value of every register the region wrote (ascending, contiguous ranges coalesced)
//   + NOP / type-2 padding to the region end.
//   => the register file and loaded shaders after the region equal the original stream's; the n DRAWs are not seen by the CP.
//   Fits by construction except in pathological cases (checked; a run that does not fit is left alone).
constexpr uint32_t kRunMarkerMagic = 0x4E524E34u;  // 'NRN4' (29A 18:30 proposal)
constexpr uint32_t kMasksFlagRecordOwnable = 4u;   // proposed REX_NATIVE_MASKS_FLAG_RECORD_OWNABLE (GetDrawMasksV2 flags bit 2)
constexpr uint32_t kFlagOwnedRec = 1024u;          // REX_NATIVE_DRAW_FLAG_OWNED
constexpr uint32_t kFlagLocalSkip = 0x80000000u;   // exe-local: record went out in an owned run, skip it in the verify batch
// g_capture_own: declared in native_renderer_bridge.cpp (dist native_capture_own, read in VerificationEnabled).
uint32_t g_run_id = 0;  // under g_submit_mu
CapBatchFn GetOwnedRunFn() { return GetCapApi().submit_v4_batch; }

enum CapBreak : uint32_t {
  kCbOpcode = 0, kCbPredicated, kCbReg, kCbLoadAlu, kCbMarker, kCbType1, kCbDraw, kCbSegmentEnd, kCbCount
};
static const char* const kCapBreakName[kCbCount] = {"opcode", "predicated", "uncovered-reg", "load-alu", "marker-nop",
                                                    "type1", "draw-not-ownable", "segment-end"};
struct CapRunStats {
  uint64_t runs = 0, owned = 0, region_words = 0, restore_words = 0, no_fit = 0, refused = 0, empty_breaks = 0;
  uint64_t brk[kCbCount] = {};
};
CapRunStats g_run_stats;  // under g_submit_mu

struct CapRun {
  struct Write { uint32_t reg, value; };  // reg 0x80000000 | words << 4 | stage = IM_LOAD packet (value = its address)
  struct Owned { uint32_t blob_off, blob_size, log_pos, end; };
  struct Pending { uint32_t start, end, first, n; std::vector<uint32_t> restore; uint32_t pred = 0; };
  uint32_t pred = 0;  // MAIN31: predicate bit of the current run's draws
  const uint8_t* base = nullptr;
  uint32_t start = 0;  // 0 = no candidate run
  std::vector<Write> log;
  std::vector<Owned> draws;          // owned draws of the current candidate run
  std::vector<Owned> all_owned;      // owned draws of closed (pending) runs, in order
  std::vector<Pending> pending;
  void Reset() {
    start = 0;
    log.clear();
    draws.clear();
  }
  void Clear() {
    Reset();
    all_owned.clear();
    pending.clear();
  }
  static bool Covered(uint32_t reg) {
    if (reg - 0x4000u < 0x800u || reg - 0x4800u < 0xC0u) return true;
    if (reg - 0x2000u < 0x1000u) return g_lsm.ctx[reg - 0x2000u] != 0xFF;
    if (reg - 0x4900u < 0x10u) return g_lsm.bl[reg - 0x4900u] != 0xFF;
    return false;
  }
  void Break(uint32_t why) {
    if (!draws.empty()) {
      ++g_run_stats.brk[why];
      Close();
    } else {
      ++g_run_stats.empty_breaks;
    }
    Reset();
  }
  void Begin(uint32_t p) {
    if (!start) start = p;
  }
  bool LogRun(uint32_t reg0, uint32_t at, uint32_t n, bool one_reg) {
    for (uint32_t i = 0; i < n; ++i) {
      const uint32_t reg = one_reg ? reg0 : reg0 + i;
      if (!Covered(reg)) return false;
    }
    for (uint32_t i = 0; i < n; ++i) log.push_back({one_reg ? reg0 : reg0 + i, G32(base, at + 4u * i)});
    return true;
  }
  // Every depth-0 packet before the walker applies it (draw packets: see Draw).
  void Packet(uint32_t p, uint32_t h) {
    static constexpr uint32_t kTypeBase[5] = {0x4000u, 0x4800u, 0x4900u, 0x4908u, 0x2000u};
    // MAIN31 (lazy start): a run starts AT its first owned DRAW. Packets before it stay in the stream for the CP (every owned
    // record carries its full state, so the SDK does not need them) -> no logging while no run is open (was ~0.85 ms/frame:
    // every register write of the frame was logged although only ~14 % of draws end up in runs).
    if (draws.empty()) return;
    const uint32_t type = h >> 30;
    if (h == 0 || type == 2) {  // walker no-op / filler
      Begin(p);
      return;
    }
    if (type == 1) return Break(kCbType1);
    if (type == 0) {
      const uint32_t n = ((h >> 16) & 0x3FFFu) + 1u;
      Begin(p);
      if (!LogRun(h & 0x7FFFu, p + 4, n, ((h >> 15) & 1) != 0)) return Break(kCbReg);
      return;
    }
    const uint32_t opcode = (h >> 8) & 0x7Fu;
    const uint32_t body = ((h >> 16) & 0x3FFFu) + 1u;
    const bool is_draw = opcode == 0x22 || opcode == 0x36;
    // MAIN31: with native_capture_own_pred a predicated DRAW may join a run (Draw() keeps one predicate bit per run).
    if ((h & 1) && !(is_draw && g_capture_own_pred.load(std::memory_order_relaxed))) return Break(kCbPredicated);
    if (is_draw) return;  // Draw() decides
    switch (opcode) {
      case 0x10: {  // NOP: markers of M3 / this code end the run
        const uint32_t b0 = body >= 1 ? G32(base, p + 4) : 0u;
        if (b0 == kRunMarkerMagic || b0 == 0x4E524433u || b0 == 0x4E525235u) return Break(kCbMarker);
        Begin(p);
        return;
      }
      case 0x2D: {  // SET_CONSTANT
        if (body < 1) return Break(kCbOpcode);
        const uint32_t ot = G32(base, p + 4), t = (ot >> 16) & 0xFFu;
        if (t >= 5) return Break(kCbOpcode);
        Begin(p);
        if (body > 1 && !LogRun(kTypeBase[t] + (ot & 0x7FFu), p + 8, body - 1, false)) return Break(kCbReg);
        return;
      }
      case 0x55:
      case 0x56: {  // SET_CONSTANT2 / SET_SHADER_CONSTANTS
        if (body < 1) return Break(kCbOpcode);
        Begin(p);
        if (body > 1 && !LogRun(G32(base, p + 4) & 0xFFFFu, p + 8, body - 1, false)) return Break(kCbReg);
        return;
      }
      case 0x27:
      case 0x2B: {  // IM_LOAD / IM_LOAD_IMMEDIATE: stage in body[0] bits 0-1
        if (body < 1 || body + 1 >= (1u << 24)) return Break(kCbOpcode);
        const uint32_t stage = G32(base, p + 4) & 3u;
        if (stage > 1) return Break(kCbOpcode);
        Begin(p);
        log.push_back({0x80000000u | ((body + 1u) << 4) | stage, p});
        return;
      }
      case 0x2F:
        return Break(kCbLoadAlu);
      default:
        return Break(kCbOpcode);
    }
  }
  void Draw(uint32_t p, uint32_t body, bool ownable, uint32_t blob_off, uint32_t blob_size) {
    if (!ownable) return Break(kCbDraw);
    const uint32_t pb = G32(base, p) & 1u;
    if (!draws.empty() && pb != pred) Break(kCbPredicated);  // MAIN31: one predicate bit per run
    if (draws.empty()) pred = pb;
    Begin(p);
    draws.push_back({blob_off, blob_size, uint32_t(log.size()), p + (body + 1u) * 4u});
  }
  // The candidate run ends: its region up to the last owned draw becomes a pending rewrite (if it fits).
  void Close() {
    if (draws.empty() || !start) return;
    const Owned& last = draws.back();
    const uint32_t region_words = (last.end - start) / 4u;
    // Final values (log up to the last owned draw).
    thread_local std::vector<uint32_t> regs;
    thread_local std::vector<uint32_t> vals;
    thread_local std::vector<uint32_t> seen;  // stamp per register index 0..0x7FFF
    thread_local uint32_t stamp = 0;
    if (seen.empty()) seen.assign(0x8000, 0);
    if (++stamp == 0) {
      std::fill(seen.begin(), seen.end(), 0u);
      stamp = 1;
    }
    thread_local std::vector<uint32_t> value_at;
    if (value_at.empty()) value_at.assign(0x8000, 0);
    regs.clear();
    uint32_t imload_at[2] = {0, 0}, imload_words[2] = {0, 0};
    for (uint32_t i = 0; i < last.log_pos; ++i) {
      const Write& w = log[i];
      if (w.reg & 0x80000000u) {
        const uint32_t stage = w.reg & 3u;
        imload_at[stage] = w.value;
        imload_words[stage] = (w.reg >> 4) & 0xFFFFFFu;
        continue;
      }
      if (seen[w.reg] != stamp) {
        seen[w.reg] = stamp;
        regs.push_back(w.reg);
      }
      value_at[w.reg] = w.value;
    }
    std::sort(regs.begin(), regs.end());
    Pending pr;
    pr.start = start;
    pr.end = last.end;
    pr.first = uint32_t(all_owned.size());
    pr.n = uint32_t(draws.size());
    pr.pred = pred;
    std::vector<uint32_t>& out = pr.restore;
    out.reserve(region_words);
    for (uint32_t s = 0; s < 2; ++s) {
      for (uint32_t i = 0; i < imload_words[s]; ++i) out.push_back(G32(base, imload_at[s] + 4u * i));
    }
    for (size_t i = 0; i < regs.size();) {
      size_t j = i + 1;
      while (j < regs.size() && regs[j] == regs[j - 1] + 1 && j - i < 0x4000) ++j;
      const uint32_t n = uint32_t(j - i);
      out.push_back(((n - 1u) << 16) | regs[i]);  // type-0, consecutive registers
      for (size_t k = i; k < j; ++k) out.push_back(value_at[regs[k]]);
      i = j;
    }
    const uint32_t marker_words = 4u;
    if (marker_words + uint32_t(out.size()) > region_words) {
      ++g_run_stats.no_fit;
      return;
    }
    all_owned.insert(all_owned.end(), draws.begin(), draws.end());
    pending.push_back(std::move(pr));
  }
};

// After a successful walk: submit each pending run's records as OWNED, then rewrite its region. Returns the number of draws owned.
uint64_t CapApplyRuns(uint8_t* base, CapRun& run, LeanBlob& blob) {
  const CapBatchFn fn = GetOwnedRunFn();
  uint64_t owned = 0;
  thread_local std::vector<uint8_t> tmp;
  // MAIN31: ONE SDK batch call for all runs of the kick (was one call per run: ~258 calls/frame x 3.6 us). The SDK
  // splits the batch at sequence 0 and accepts all runs or none.
  thread_local std::vector<uint32_t> ids;
  ids.clear();
  tmp.clear();
  uint32_t total = 0;
  for (CapRun::Pending& pr : run.pending) {
    if (++g_run_id == 0x80000000u || g_run_id == 0) g_run_id = 1;
    const uint32_t run_id = 0x80000000u | g_run_id;
    ids.push_back(run_id);
    for (uint32_t i = 0; i < pr.n; ++i) {
      const CapRun::Owned& o = run.all_owned[pr.first + i];
      const size_t at = tmp.size();
      tmp.insert(tmp.end(), blob.data() + o.blob_off, blob.data() + o.blob_off + o.blob_size);
      DrawRecordV2 r;
      std::memcpy(&r, tmp.data() + at, sizeof(r));
      r.flags = (r.flags & ~kFlagVerificationOnly) | kFlagOwnedRec;
      r.reserved0 = run_id;
      r.sequence = i;
      std::memcpy(tmp.data() + at, &r, sizeof(r));
    }
    total += pr.n;
  }
  if (run.pending.empty()) {
    run.Clear();
    return 0;
  }
  const uint64_t ts0 = Ticks();
  const int res = fn ? fn(tmp.data(), uint32_t(tmp.size()), total) : 0;
  g_t_runsdk.fetch_add(Ticks() - ts0, std::memory_order_relaxed);  // MAIN31
  if (res != int(total)) {
    g_run_stats.refused += run.pending.size();
    run.Clear();
    return 0;
  }
  for (size_t ri = 0; ri < run.pending.size(); ++ri) {
    CapRun::Pending& pr = run.pending[ri];
    const uint32_t run_id = ids[ri];
    // Rewrite [start, end): marker, restore, padding.
    uint32_t a = pr.start;
    const uint32_t region_words = (pr.end - pr.start) / 4u;
    W32(base, a, (3u << 30) | (2u << 16) | (0x10u << 8) | pr.pred);  // NOP, body = magic, run_id, n (MAIN31: run's predicate bit)
    W32(base, a + 4, kRunMarkerMagic);
    W32(base, a + 8, run_id);
    W32(base, a + 12, pr.n);
    a += 16;
    for (uint32_t v : pr.restore) {
      W32(base, a, v);
      a += 4;
    }
    const uint32_t left = (pr.end - a) / 4u;
    if (left == 1) {
      W32(base, a, 0x80000000u);  // type-2 filler
    } else if (left >= 2) {
      W32(base, a, (3u << 30) | ((left - 2u) << 16) | (0x10u << 8));  // NOP over the rest (body left - 1 words)
    }
    for (uint32_t i = 0; i < pr.n; ++i) {
      const CapRun::Owned& o = run.all_owned[pr.first + i];
      DrawRecordV2 r;
      std::memcpy(&r, blob.data() + o.blob_off, sizeof(r));
      r.flags |= kFlagLocalSkip;
      std::memcpy(const_cast<uint8_t*>(blob.data()) + o.blob_off, &r, sizeof(r));
    }
    ++g_run_stats.runs;
    g_run_stats.owned += pr.n;
    g_run_stats.region_words += region_words;
    g_run_stats.restore_words += pr.restore.size() + 4u;
    owned += pr.n;
  }
  run.Clear();
  return owned;
}

void CapRunLog(double frames) {
  CapRunStats& s = g_run_stats;
  std::string brk;
  for (uint32_t i = 0; i < kCbCount; ++i) {
    if (s.brk[i]) brk += fmt::format(" {} {:.1f}", kCapBreakName[i], s.brk[i] / frames);
  }
  REXLOG_INFO("NATIVE CAPTURE OWNED RUNS (29B, per frame): runs {:.1f}, owned draws {:.1f}, region words {:.0f} -> marker+restore {:.0f}, "
              "no-fit {:.2f}, SDK refused {:.2f} | runs ended by:{}",
              s.runs / frames, s.owned / frames, s.region_words / frames, s.restore_words / frames, s.no_fit / frames,
              s.refused / frames, brk.empty() ? " -" : brk);
  std::string why;
  for (uint32_t i = 0; i < 8; ++i) {
    const uint64_t v = g_own_why[i].exchange(0, std::memory_order_relaxed);
    if (v) why += fmt::format(" {} {:.1f}", kOwnWhyName[i], double(v) / frames);
  }
  REXLOG_INFO("NATIVE CAPTURE OWNABLE (MAIN31, per frame, first failing reason; tiled allowed {}):{}",
              g_capture_own_tiled.load(std::memory_order_relaxed) ? 1 : 0, why.empty() ? " -" : why);
  s = CapRunStats{};
}

void CapRunPacket(CapRun* run, uint32_t p, uint32_t h) { run->Packet(p, h); }
void CapRunBreakSkipped(CapRun* run) { run->Break(kCbPredicated); }  // MAIN31
void CapRunDraw(CapRun* run, uint32_t p, uint32_t body, bool ownable, uint32_t blob_off, uint32_t blob_size) {
  run->Draw(p, body, ownable, blob_off, blob_size);
}
