// M3 EXECUTOR, game side (AGENT_NATIVE23B2 2026-10-27; design: claude_ipc/native23/M3_EXECUTOR_HANDOFF.md "Interface (M3.0)",
// REPLY 17:40 "writer runs + NOP-span"). Included at the end of native_renderer_bridge.cpp (same TU: G32 / W32 / GpuAddr / kDev*).
//
// For an owned candidate call of 825D2478 (DrawIndexed) the ENGINE WRITER STILL RUNS (exact by construction: dirty bits,
// VS ucode patch 825DB738/825DC108, kicks and every other side effect stay the engine's own). Afterwards the PM4 dwords the
// call wrote are copied into a record v4 (v3 header + V4 extension + CALL_WORDS) and handed to the SDK synchronously:
//   native_m3_shadow : a 3-dword NOP {MAGIC, 0} is reserved at the write pointer BEFORE the writer runs (MARKER_BEFORE);
//                      after the writer the record goes out VERIFY_ONLY and the returned id is patched into that NOP.
//                      The CP still executes the real packets (the SDK compares them with the record).
//   native_m3_replace: keys the SDK reports OWNABLE (RexNativeRendererGetDrawMasksV2 flags bit 1, colour mask 0) go out OWNED (others as
//                      shadow records, see above); only if the SDK accepts (1 + id) the call's first 3 dwords become a
//                      NOP SPANNING THE WHOLE CALL {MAGIC, id, <original dwords 3..n-1 untouched>}: the CP skips the call in
//                      one packet and the SDK applies the record's call_words (writes + DRAW) at that point of the stream.
//                      Refused / malformed / not ready -> nothing is changed (the real packets execute) = no hole possible.
// Both modes need the SDK capability export RexNativeRendererM3CapsV1() (bit 0 shadow v4 intake, bit 1 NOP-span replace);
// an older rexgpu-xenos.dll without it -> M3 stays off. native_m3.off wins. Default OFF. Bridge verification (native_bridge_verify)
// and M3 are exclusive (verification wins).
// Owned candidates v1 (pre-pass family): caller 8223E864, no pixel shader (device +12432 == 0), direct layout (+10808 bit 1
// clear), no kick (825D3218) during the call, call <= 4 KB, opcodes type-0 / type-1 / type-2 / type-3 NOP SET_CONSTANT(2)
// SET_SHADER_CONSTANTS LOAD_ALU_CONSTANT IM_LOAD(_IMMEDIATE) + exactly one DRAW_INDX / DRAW_INDX_2 as the LAST packet.
namespace {
constexpr uint32_t kM3Magic = 0x4E524433u;          // REX_NATIVE_MARKER_MAGIC
constexpr uint32_t kM3ExtMagic = 0x34584D33u;       // REX_NATIVE_DRAW_V4_EXT_MAGIC 'M3X4'
constexpr uint32_t kM3Version4 = 4u;
constexpr uint32_t kM3FlagOwned = 1024u, kM3FlagRegValues = 2048u, kM3FlagMarkerBefore = 4096u, kM3FlagCallWords = 8192u;
constexpr uint32_t kM3MaxCallWords = 1024u;
constexpr uint32_t kM3Caller = 0x8223E864u;
struct alignas(8) M3V4Ext {  // SDK RexNativeDrawRecordV4Ext (AGENT_NATIVE23A2 proposal 17:25)
  uint32_t magic;
  uint32_t ext_size;  // 336 + 4 * fetch_value_words (+ 4 + 4 * call_word_count with CALL_WORDS)
  uint32_t state_values[64];
  uint32_t fetch_unit_mask[3];
  uint32_t fetch_value_words;
  uint32_t draw_packet_words;
  uint32_t draw_packet[8];
  uint32_t reserved[5];
};
static_assert(sizeof(M3V4Ext) == 336);
static_assert(offsetof(M3V4Ext, fetch_unit_mask) == 264);
static_assert(offsetof(M3V4Ext, draw_packet_words) == 280);
static_assert(offsetof(M3V4Ext, draw_packet) == 284);
static_assert(offsetof(M3V4Ext, reserved) == 316);

using M3SubmitFn = int (*)(const void* record, uint32_t size, uint32_t* out_record_id);
using M3CapsFn = uint32_t (*)();
struct M3MasksV2 {  // SDK RexNativeDrawMasksV2 (40 B)
  uint64_t vs_groups, ps_groups;
  uint32_t fetch_units[3];
  uint32_t generation;
  uint32_t flags;  // bit 0 SHADOW wanted, bit 1 OWNABLE (entry proven)
  uint32_t reserved;
};
static_assert(sizeof(M3MasksV2) == 40);
using M3MasksFn = int (*)(uint32_t vs_guest, uint32_t ps_guest, uint32_t decl, M3MasksV2* out);
struct M3Api {
  M3SubmitFn submit = nullptr;
  M3MasksFn masks = nullptr;
  uint32_t caps = 0;
};
const M3Api& M3GetApi() {
  static const M3Api api = [] {
    M3Api a;
    HMODULE gpu = GetModuleHandleW(L"rexgpu-xenos.dll");
    if (gpu) {
      const M3CapsFn caps = reinterpret_cast<M3CapsFn>(GetProcAddress(gpu, "RexNativeRendererM3CapsV1"));
      a.submit = reinterpret_cast<M3SubmitFn>(GetProcAddress(gpu, "RexNativeRendererSubmitOwnedDrawRecordV3"));
      a.masks = reinterpret_cast<M3MasksFn>(GetProcAddress(gpu, "RexNativeRendererGetDrawMasksV2"));
      a.caps = caps && a.submit ? caps() : 0u;
    }
    REXLOG_INFO("NATIVE M3 (game side): SDK caps {:X}{}", a.caps, a.caps ? "" : " (no M3 v4 intake in rexgpu-xenos.dll -> M3 off)");
    return a;
  }();
  return api;
}

enum M3Skip : int { kM3Tiled = 0, kM3Ps, kM3Space, kM3Kick, kM3Size, kM3Walk, kM3Opcode, kM3Shape, kM3SkipCount };
struct M3Stats {
  uint64_t cand = 0, skip[kM3SkipCount] = {}, submitted = 0, accepted = 0, refused = 0, bad = 0, words = 0, spans = 0;
  uint64_t owned_tried = 0, shadow_in_replace = 0;  // replace mode: OWNABLE key -> owned, else shadow (proving)
  uint32_t last_bad_opcode = 0;
  uint32_t bad_op[128] = {};  // rejected type-3 opcodes (M4 planning)
};
thread_local M3Stats t_m3;
thread_local uint32_t t_m3_seq = 0;

inline bool M3Allowed(uint32_t op) {
  switch (op) {
    case 0x10:  // NOP
    case 0x2D:  // SET_CONSTANT
    case 0x55:  // SET_CONSTANT2
    case 0x56:  // SET_SHADER_CONSTANTS
    case 0x2F:  // LOAD_ALU_CONSTANT (the CP reads its source when it executes = same time as the original)
    case 0x27:  // IM_LOAD
    case 0x2B:  // IM_LOAD_IMMEDIATE
    case 0x22:  // DRAW_INDX
    case 0x36:  // DRAW_INDX_2
      return true;
    default:
      return false;
  }
}

void M3MaybeLog() {
  M3Stats& s = t_m3;
  if ((s.cand & 0xFFFFFu) != 0) return;  // ~every 1M candidates per thread
  REXLOG_INFO("NATIVE M3 (this thread, mode {}): candidates {} | skipped tiled {} ps {} space {} kick {} size {} walk {} opcode {} "
              "(last {:02X}) shape {} | submitted {} accepted {} refused {} malformed {} | avg call words {:.1f} | NOP spans {} | replace: "
              "owned tries {} shadow (key not OWNABLE) {}",
              M3Mode() == 2 ? "replace" : "shadow", s.cand, s.skip[kM3Tiled], s.skip[kM3Ps], s.skip[kM3Space], s.skip[kM3Kick],
              s.skip[kM3Size], s.skip[kM3Walk], s.skip[kM3Opcode], s.last_bad_opcode, s.skip[kM3Shape], s.submitted, s.accepted,
              s.refused, s.bad, s.submitted ? double(s.words) / double(s.submitted) : 0.0, s.spans, s.owned_tried,
              s.shadow_in_replace);
  std::string ops;
  for (uint32_t i = 0; i < 128; ++i)
    if (s.bad_op[i]) {
      char b[24];
      std::snprintf(b, sizeof(b), " %02X:%u", i, s.bad_op[i]);
      ops += b;
    }
  if (!ops.empty()) REXLOG_INFO("NATIVE M3 rejected opcodes (this thread):{}", ops);
}
}  // namespace

int M3Mode() {
  static const int m = [] {
    int v = 0;
    if (GetFileAttributesW(L"native_m3.off") == INVALID_FILE_ATTRIBUTES) {
      if (GetFileAttributesW(L"native_m3_shadow") != INVALID_FILE_ATTRIBUTES) v = 1;
      if (GetFileAttributesW(L"native_m3_replace") != INVALID_FILE_ATTRIBUTES) v = 2;
    }
    if (v) {
      const uint32_t need = v == 2 ? 2u : 1u;
      if ((M3GetApi().caps & need) == 0 || (v == 2 && !M3GetApi().masks)) v = 0;
      REXLOG_INFO("NATIVE M3 (game side, AGENT_NATIVE23B2): {}", v == 2 ? "REPLACE (owned pre-pass calls -> NOP span)"
                                                              : v == 1 ? "SHADOW (marker before + verify record)"
                                                                       : "requested but the SDK lacks the capability -> off");
    }
    return v;
  }();
  return m;
}

// Owned callers: default {8223E864} (pre-pass, PS 0 required). dist\native_m3_callers (text, hex return addresses separated by
// whitespace / commas, up to 16) REPLACES the list and drops the PS-0 requirement (M4 shadow measurements, e.g. 8223F3C4 main pass).
struct M3Callers {
  uint32_t lr[16] = {kM3Caller};
  uint32_t n = 1;
  bool need_no_ps = true;
};
const M3Callers& M3GetCallers() {
  static const M3Callers c = [] {
    M3Callers r;
    if (FILE* f = std::fopen("native_m3_callers", "rb")) {
      char buf[512] = {};
      const size_t got = std::fread(buf, 1, sizeof(buf) - 1, f);
      std::fclose(f);
      buf[got] = 0;
      uint32_t n = 0;
      for (char* p = buf; *p && n < 16;) {
        char* e = nullptr;
        const unsigned long v = std::strtoul(p, &e, 16);
        if (e == p) { ++p; continue; }
        if (v) r.lr[n++] = uint32_t(v);
        p = e;
      }
      if (n) {
        r.n = n;
        r.need_no_ps = false;
      }
      REXLOG_INFO("NATIVE M3: native_m3_callers -> {} callers (first {:08X}), PS-0 requirement off", r.n, r.lr[0]);
    } else if (GetFileAttributesW(L"native_m3_main") != INVALID_FILE_ATTRIBUTES) {
      // NATIVE24B (M4 shadow measurement): pre-pass caller + main pass caller 8223F3C4, PS-0 requirement off.
      r.lr[1] = 0x8223F3C4u;
      r.n = 2;
      r.need_no_ps = false;
      REXLOG_INFO("NATIVE M3: native_m3_main -> callers 8223E864 + 8223F3C4, PS-0 requirement off");
    }
    return r;
  }();
  return c;
}

// NATIVE24B caller census (dist native_m3_census, with an M3 mode): every 825D2478 call by return address -> calls, colour-mask-0,
// no-PS, 4x pre-pass surface (pitch 640, 4x MSAA). Log 'NATIVE M3 CENSUS per second' (top 16) every 10 s (divide by DRAWSTATS fps).
namespace m3census {
constexpr uint32_t kSlots = 256;
struct Slot {
  std::atomic<uint32_t> lr{0};
  std::atomic<uint32_t> calls{0}, cmask0{0}, nops{0}, prepass{0};
};
Slot g_slots[kSlots];
std::atomic<uint32_t> g_other{0};
std::atomic<int64_t> g_next{0};
bool On() { static const bool on = GetFileAttributesW(L"native_m3_census") != INVALID_FILE_ATTRIBUTES; return on; }
void Count(uint8_t* base, uint32_t dev, uint32_t lr) {
  uint32_t h = (lr * 2654435761u) >> 24;
  Slot* s = nullptr;
  for (uint32_t i = 0; i < 16; ++i, h = (h + 1) & (kSlots - 1)) {
    uint32_t cur = g_slots[h].lr.load(std::memory_order_relaxed);
    if (cur == lr) { s = &g_slots[h]; break; }
    if (cur == 0 && g_slots[h].lr.compare_exchange_strong(cur, lr)) { s = &g_slots[h]; break; }
    if (cur == lr) { s = &g_slots[h]; break; }
  }
  if (!s) { g_other.fetch_add(1, std::memory_order_relaxed); return; }
  s->calls.fetch_add(1, std::memory_order_relaxed);
  if (G32(base, dev + kDevColorMask) == 0) s->cmask0.fetch_add(1, std::memory_order_relaxed);
  if (G32(base, dev + kDevPixelShader) == 0) s->nops.fetch_add(1, std::memory_order_relaxed);
  const uint32_t si = G32(base, dev + kDevSurface);
  if ((si & 0x3FFFu) == 640u && ((si >> 16) & 3u) == 2u) s->prepass.fetch_add(1, std::memory_order_relaxed);
  const int64_t now = int64_t(GetTickCount64());
  int64_t n = g_next.load(std::memory_order_relaxed);
  if (now < n || !g_next.compare_exchange_strong(n, now + 10000)) return;
  if (!n) {  // first window: reset only
    for (Slot& x : g_slots) { x.calls.store(0); x.cmask0.store(0); x.nops.store(0); x.prepass.store(0); }
    g_other.store(0);
    return;
  }
  const double fr = double(now - (n - 10000)) / 1000.0;  // seconds in this window (per-second rates; divide by fps)
  struct Row { uint32_t lr, calls, cmask0, nops, prepass; };
  std::vector<Row> rows;
  uint64_t total = 0;
  for (Slot& x : g_slots) {
    const uint32_t l = x.lr.load(std::memory_order_relaxed);
    Row r{l, x.calls.exchange(0), x.cmask0.exchange(0), x.nops.exchange(0), x.prepass.exchange(0)};
    total += r.calls;
    if (l && r.calls) rows.push_back(r);
  }
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.calls > b.calls; });
  std::string t;
  for (size_t i = 0; i < rows.size() && i < 16; ++i) {
    char b[128];
    std::snprintf(b, sizeof(b), " | %08X %.0f (cm0 %.0f nops %.0f pre %.0f)", rows[i].lr, rows[i].calls / fr, rows[i].cmask0 / fr,
                  rows[i].nops / fr, rows[i].prepass / fr);
    t += b;
  }
  static int lines = 0;
  if (lines++ < 200)
    REXLOG_INFO("NATIVE M3 CENSUS per second ({:.1f} s): 825D2478 calls {:.1f}, callers {}, untracked {}{}", fr, total / fr,
                rows.size(), g_other.exchange(0), t);
}
}  // namespace m3census

bool M3Begin(uint8_t* base, uint32_t dev, uint32_t lr, M3Call& c) {
  c.active = false;
  const int mode = M3Mode();
  if (!mode) return false;
  if (m3census::On()) m3census::Count(base, dev, lr);
  const M3Callers& cl = M3GetCallers();
  bool hit = false;
  for (uint32_t i = 0; i < cl.n; ++i) hit |= cl.lr[i] == lr;
  if (!hit) return false;
  M3Stats& s = t_m3;
  ++s.cand;
  M3MaybeLog();
  if (*GuestPtr(base, dev + 10808) & 2) { ++s.skip[kM3Tiled]; return false; }
  if (cl.need_no_ps && G32(base, dev + kDevPixelShader) != 0) { ++s.skip[kM3Ps]; return false; }
  c.dev = dev;
  c.wp0 = G32(base, dev + kDevRingWrite);
  c.marker_at = 0;
  c.owned = false;
  if (mode == 2) {
    // Own only keys the SDK has proven (OWNABLE) and only depth-only draws (colour mask 0); everything else goes out as a shadow
    // record so the SDK keeps proving / re-proving keys (after a revoke).
    const uint32_t vs = G32(base, dev + kDevVertexShader), ps = G32(base, dev + kDevPixelShader);
    M3MasksV2 m{};
    if (G32(base, dev + kDevColorMask) == 0 &&
        M3GetApi().masks(vs ? G32(base, vs + 48) : 0, ps ? G32(base, ps + 24) : 0, G32(base, dev + kDevVertexDecl), &m) == 1 &&
        (m.flags & 2u))
      c.owned = true;
    if (c.owned) ++s.owned_tried; else ++s.shadow_in_replace;
  }
  if (!c.owned) {
    // Reserve the shadow marker (id 0 = plain NOP until the record is accepted).
    if (!(c.wp0 + 16 < G32(base, dev + 44))) { ++s.skip[kM3Space]; return false; }
    c.marker_at = c.wp0 + 4;
    W32(base, c.marker_at, (3u << 30) | (1u << 16) | (0x10u << 8));
    W32(base, c.marker_at + 4, kM3Magic);
    W32(base, c.marker_at + 8, 0);
    W32(base, dev + kDevRingWrite, c.wp0 + 12);
    c.wp0 += 12;
  }
  c.kicks0 = t_m3_kicks;
  c.active = true;
  return true;
}

void M3End(uint8_t* base, const M3Call& c, const WriterArgs& a) {
  if (!c.active) return;
  M3Stats& s = t_m3;
  const uint32_t dev = c.dev;
  const uint32_t wp1 = G32(base, dev + kDevRingWrite);
  if (t_m3_kicks != c.kicks0) { ++s.skip[kM3Kick]; return; }
  if (wp1 <= c.wp0 || wp1 - c.wp0 > kM3MaxCallWords * 4u) { ++s.skip[kM3Size]; return; }
  const uint32_t from = c.wp0 + 4, to = wp1 + 4;  // device +40 = address of the last dword written
  uint32_t p = from, draw_at = 0, draw_n = 0, draws = 0;
  while (p < to) {
    const uint32_t h = G32(base, p);
    const uint32_t type = h >> 30;
    uint32_t n;
    if (type == 0) {
      n = ((h >> 16) & 0x3FFFu) + 2u;
    } else if (type == 1) {
      n = 3;
    } else if (type == 2) {
      n = 1;
    } else {
      const uint32_t op = (h >> 8) & 0x7Fu;
      n = ((h >> 16) & 0x3FFFu) + 2u;
      if (!M3Allowed(op)) {
        ++s.skip[kM3Opcode];
        s.last_bad_opcode = op;
        ++s.bad_op[op];
        return;
      }
      if (op == 0x22 || op == 0x36) {
        ++draws;
        draw_at = p;
        draw_n = n;
      }
    }
    p += n * 4u;
  }
  if (p != to) { ++s.skip[kM3Walk]; return; }
  if (draws != 1 || draw_at + draw_n * 4u != to || draw_n > 8) { ++s.skip[kM3Shape]; return; }
  const uint32_t nw = (to - from) / 4u;
  const M3Api& api = M3GetApi();

  // Record v4 = v3 header (identity + engine args, no hashes, no values) + V4 ext (DRAW words) + CALL_WORDS.
  thread_local std::vector<uint8_t> buf;
  const uint32_t size = kDrawRecordV3HeaderSize + uint32_t(sizeof(M3V4Ext)) + 4u + 4u * nw;
  buf.assign(size, 0);
  DrawRecordV3Header hd{};
  DrawRecordV2& r = hd.v2;
  r.size = size;
  r.version = kM3Version4;
  r.frame_id = g_frame_id.load(std::memory_order_relaxed);
  r.sequence = t_m3_seq++;
  r.pass_id = 0xFFFFFFFFu;
  r.thread_id = GetCurrentThreadId();
  r.path_id = 1;  // REX_NATIVE_PATH_DRAW_INDEXED
  r.caller = a.lr;
  r.packet_end = to;
  const uint32_t dh = G32(base, draw_at);
  const uint32_t op = (dh >> 8) & 0x7Fu;
  r.draw_initiator = G32(base, draw_at + (op == 0x22 ? 8u : 4u));
  const uint32_t source = (r.draw_initiator >> 6) & 3u;
  r.dma_base = (op == 0x22 && source == 0 && draw_n >= 5) ? G32(base, draw_at + 12) : 0;
  r.flags = (c.owned ? kM3FlagOwned : (kFlagVerificationOnly | kM3FlagMarkerBefore)) | kFlagHasPacket | kFlagEngineBase |
            kM3FlagCallWords | (source == 0 ? kFlagIndexed : 0u);
  r.primitive_type = a.r4;
  r.base_vertex = int32_t(a.r5);
  r.start_index = a.r6;
  r.index_count = a.r7;
  r.device_guest = dev;
  const uint32_t vs = G32(base, dev + kDevVertexShader), ib = G32(base, dev + kDevIndexBuffer);
  r.vertex_shader_guest = vs ? G32(base, vs + 48) : 0;
  r.pixel_shader_guest = 0;
  r.index_buffer_guest = ib ? G32(base, ib + 24) : 0;
  r.vertex_decl_guest = G32(base, dev + kDevVertexDecl);
  r.surface_info = G32(base, dev + kDevSurface);
  r.color_mask = G32(base, dev + kDevColorMask);
  r.depth_control = G32(base, dev + kDevDepthControl);
  r.mode_control = G32(base, dev + kDevModeControl);
  hd.draw_header = dh;
  std::memcpy(buf.data(), &hd, sizeof(hd));
  M3V4Ext ext{};
  ext.magic = kM3ExtMagic;
  ext.ext_size = uint32_t(sizeof(M3V4Ext)) + 4u + 4u * nw;
  ext.draw_packet_words = draw_n;
  for (uint32_t i = 0; i < draw_n; ++i) ext.draw_packet[i] = G32(base, draw_at + 4u * i);
  std::memcpy(buf.data() + kDrawRecordV3HeaderSize, &ext, sizeof(ext));
  uint8_t* w = buf.data() + kDrawRecordV3HeaderSize + sizeof(M3V4Ext);
  std::memcpy(w, &nw, 4);
  for (uint32_t i = 0; i < nw; ++i) {
    const uint32_t v = G32(base, from + 4u * i);
    std::memcpy(w + 4 + 4 * i, &v, 4);
  }
  uint32_t id = 0;
  const int result = api.submit(buf.data(), size, &id);
  ++s.submitted;
  s.words += nw;
  if (result < 0) { ++s.bad; return; }
  if (result == 0 || id == 0) { ++s.refused; return; }
  ++s.accepted;
  if (!c.owned) {
    W32(base, c.marker_at + 8, id);  // shadow: the reserved NOP now names the record
  } else {
    // replace: NOP spanning the call (count field = body dwords - 1 = nw - 2), predicate bit 0 (direct layout only).
    W32(base, from, (3u << 30) | ((nw - 2u) << 16) | (0x10u << 8));
    W32(base, from + 4, kM3Magic);
    W32(base, from + 8, id);
    ++s.spans;
  }
}
