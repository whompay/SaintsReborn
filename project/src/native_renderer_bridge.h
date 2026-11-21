#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace native_renderer_bridge {

// Stable, pointer-free ABI shared with rexgpu-xenos.dll (SDK include/rex/graphics/native_frame_bridge.h,
// RexNativeDrawRecordV2). All addresses are 32-bit guest addresses. Verification only: submitting a
// record never authorizes the project or the GPU backend to suppress the guest draw.
//
// v2 (AGENT_FRAME 2026-10-23): one record per actual draw packet (DRAW_INDX / DRAW_INDX_2) written by
// any hooked engine draw writer, keyed by the guest address just past that packet in the device's
// command buffer (the command processor sees the same address for the packet it executes). v1 only
// recorded 825D2478 draws inside mesh pass 8 and was matched by state, which left the other draws of
// the main colour target (HUD / sprites, mesh passes 0 / 3 / 10, 82249C3C, 8251Bxxx sky) without a
// record and gave ambiguous matches for repeated identical draws.
struct alignas(8) DrawRecordV2 {
  uint32_t size;
  uint32_t version;
  uint64_t frame_id;
  uint32_t sequence;
  uint32_t pass_id;
  uint32_t flags;
  uint32_t thread_id;
  uint32_t path_id;
  uint32_t caller;
  uint32_t packet_end;
  uint32_t draw_initiator;
  uint32_t dma_base;
  uint32_t primitive_type;
  uint32_t index_count;
  uint32_t start_index;
  int32_t base_vertex;
  uint32_t mesh_guest;
  uint32_t instance_guest;
  uint32_t device_guest;
  uint32_t vertex_shader_guest;
  uint32_t pixel_shader_guest;
  uint32_t index_buffer_guest;
  uint32_t vertex_decl_guest;
  uint32_t surface_info;
  uint32_t color_mask;
  uint32_t depth_control;
  uint32_t mode_control;
  uint32_t reserved0;
  uint32_t reserved1;
  uint32_t vs_const_hash[64];  // VS float constants, per group of 4 (shadow +1792)
  uint32_t ps_const_hash[64];  // PS float constants, per group of 4 (shadow +5888)
  uint32_t fetch_hash[96];     // fetch constants per 2-word unit (shadow +1024)
  uint32_t reserved2[2];
};

static_assert(sizeof(DrawRecordV2) == 1024);
static_assert(offsetof(DrawRecordV2, packet_end) == 40);
static_assert(offsetof(DrawRecordV2, vs_const_hash) == 120);
static_assert(offsetof(DrawRecordV2, fetch_hash) == 632);

constexpr uint32_t kDrawRecordVersion = 2;

// Record v3 ("v3b", SDK RexNativeDrawRecordV3Header): the v2 record + the used float-constant values. Sent only while
// the dist file native_bridge_v3 exists (read when verification is switched on) and only for (VS, PS, decl) keys the
// SDK publishes (RexNativeRendererGetConstantMasks). values[value_words] follow the header: VS groups ascending, then
// PS groups ascending, 16 host-order words per group.
struct alignas(8) DrawRecordV3Header {
  DrawRecordV2 v2;  // v2.version = 3, v2.size = 1056 + 4 * value_words
  uint64_t vs_group_mask;
  uint64_t ps_group_mask;
  uint32_t value_words;
  uint32_t mask_generation;
  uint32_t draw_header;  // type-3 header of the DRAW packet (bit 0 = predicate)
  uint32_t reserved3;
};
static_assert(sizeof(DrawRecordV3Header) == 1056);
static_assert(offsetof(DrawRecordV3Header, vs_group_mask) == 1024);
static_assert(offsetof(DrawRecordV3Header, ps_group_mask) == 1032);
static_assert(offsetof(DrawRecordV3Header, value_words) == 1040);
static_assert(offsetof(DrawRecordV3Header, mask_generation) == 1044);
static_assert(offsetof(DrawRecordV3Header, draw_header) == 1048);
constexpr uint32_t kDrawRecordVersion3 = 3;
constexpr uint32_t kDrawRecordV3HeaderSize = 1056;
constexpr uint32_t kDrawRecordV3MaxWords = 2048;
constexpr uint32_t kFlagConstValues = 1u << 9;
constexpr uint32_t kFlagVerificationOnly = 1u << 0;
constexpr uint32_t kFlagHasPacket = 1u << 1;
constexpr uint32_t kFlagPm4Geometry = 1u << 2;
constexpr uint32_t kFlagIndexed = 1u << 3;
constexpr uint32_t kFlagState = 1u << 4;
constexpr uint32_t kFlagBackscan = 1u << 5;
constexpr uint32_t kFlagEngineBase = 1u << 6;
constexpr uint32_t kFlagStreamConsts = 1u << 7;  // some constant hashes come from the stream mirror
constexpr uint32_t kFlagStreamState = 1u << 8;   // reserved1 = render-state hash, reserved2 = known mask

// M3 render state (same list and order as REX_NATIVE_STATE_REGS in the SDK's native_frame_bridge.h):
// RB_SURFACE_INFO..RB_COLOR3_INFO, PA_SC_SCREEN_SCISSOR_TL/BR, PA_SC_WINDOW_OFFSET/SCISSOR_TL/BR,
// VGT_MAX/MIN_VTX_INDX, VGT_INDX_OFFSET, VGT_MULTI_PRIM_IB_RESET_INDX, RB_COLOR_MASK, RB_BLEND_RGBA,
// RB_STENCILREFMASK_BF/STENCILREFMASK, RB_ALPHA_REF, PA_CL_VPORT_*, SQ_PROGRAM_CNTL, SQ_CONTEXT_MISC,
// RB_DEPTHCONTROL, RB_BLENDCONTROL0, RB_COLORCONTROL, PA_CL_CLIP_CNTL, PA_SU_SC_MODE_CNTL, PA_CL_VTE_CNTL,
// RB_MODECONTROL, RB_BLENDCONTROL1-3, PA_SU_POINT_SIZE, PA_SU_LINE_CNTL, PA_SC_AA_MASK, PA_SU_POLY_OFFSET_*,
// bool constants 0-255, loop constants 0-7.
inline constexpr uint32_t kNativeStateRegs[] = {
    0x2000, 0x2001, 0x2002, 0x2003, 0x2004, 0x2005, 0x200E, 0x200F, 0x2080, 0x2081, 0x2082,
    0x2100, 0x2101, 0x2102, 0x2103, 0x2104, 0x2105, 0x2106, 0x2107, 0x2108,
    0x210C, 0x210D, 0x210E, 0x210F, 0x2110, 0x2111, 0x2112, 0x2113, 0x2114,
    0x2180, 0x2181, 0x2200, 0x2201, 0x2202, 0x2204, 0x2205, 0x2206, 0x2208, 0x2209, 0x220A, 0x220B,
    0x2280, 0x2282, 0x2312, 0x2380, 0x2381, 0x2382, 0x2383,
    0x4900, 0x4901, 0x4902, 0x4903, 0x4904, 0x4905, 0x4906, 0x4907,
    0x4908, 0x4909, 0x490A, 0x490B, 0x490C, 0x490D, 0x490E, 0x490F};
inline constexpr uint32_t kNativeStateRegCount = sizeof(kNativeStateRegs) / sizeof(kNativeStateRegs[0]);
static_assert(kNativeStateRegCount == 64);

// Engine draw writers (path_id).
enum Path : uint32_t {
  kPathDrawIndexed = 1,  // 825D2478 DrawIndexedVertices
  kPathDrawUP = 2,       // 825D1680 DrawVerticesUP
  kPathDraw1B78 = 3,     // 825D1B78
  kPathDraw2070 = 4,     // 825D2070
  kPathDrawVertices = 5, // 825D15D0
  kPathDraw7620 = 6,     // 825D7620
  kPathResolve = 7,      // 825D8490
  kPathClear = 8,        // 825E49C0
  kPathGap = 9,          // draw packet written outside the hooked writers (stream scan)
};

// FRAME9 production metadata (dist file native_frame_meta; same layout as the SDK
// include/rex/graphics/native_frame_bridge.h RexNativeDrawMetaBatchV1 / RexNativeDrawMetaRecordV1 once agreed).
// Writers stage one 48-byte record per draw packet they wrote (per recording device). 825D3218 hands the SDK ONE batch
// per SubmitCommandStream, on the submitting thread, immediately before that call: the records whose packet_end lies
// inside the submitted root stream (DIRECT_ROOT). 825DF0B8 sends an empty TILE_CONTROL batch before its stream. The SDK
// binds the batch to exactly that submission; key = (submission, packet_end); records are not consumed. No kick walk,
// no record submission, no submit lock (unless native_frame_lean also runs the verification walk).
constexpr uint32_t kDrawMetaVersion1 = 1;
constexpr uint32_t kDrawMetaFlagEngineArgsValid = 0x0001u;
constexpr uint32_t kDrawMetaFlagDirectRoot = 0x0002u;
constexpr uint32_t kDrawMetaFlagMultiDrawWriter = 0x0004u;
constexpr uint32_t kDrawMetaBatchIncomplete = 0x00000001u;
constexpr uint32_t kDrawMetaBatchTileControl = 0x00000002u;
struct alignas(8) DrawMetaBatchV1 {
  uint32_t size;             // 0  48 + record_count * 48
  uint32_t version;          // 4  kDrawMetaVersion1
  uint32_t record_size;      // 8  48
  uint32_t record_count;     // 12
  uint64_t kick_epoch;       // 16 process-wide, +1 per batch
  uint32_t stream_base;      // 24 physical root-stream base (0 for tile control)
  uint32_t stream_dwords;    // 28
  uint32_t producer_thread;  // 32 diagnostics
  uint32_t flags;            // 36 kDrawMetaBatch*
  uint64_t reserved0;        // 40 0
};
struct DrawMetaRecordV1 {
  uint32_t packet_end;       // 0  GPU physical address just past the DRAW packet (SDK NativeBridgeGpu form)
  uint32_t pass_id;          // 4  0xFFFFFFFF outside a mesh pass
  uint32_t mesh_guest;       // 8
  uint32_t instance_guest;   // 12
  uint32_t caller;           // 16 writer return address
  int32_t base_vertex;       // 20 engine value (ENGINE_ARGS_VALID)
  uint32_t start_index;      // 24 engine value (ENGINE_ARGS_VALID)
  uint32_t engine_count;     // 28 engine value (ENGINE_ARGS_VALID)
  uint16_t path_id;          // 32 Path
  uint16_t flags;            // 34 kDrawMetaFlag*
  uint32_t writer_sequence;  // 36 per-thread sequence in the frame (diagnostics)
  uint32_t reserved0;        // 40 0
  uint32_t reserved1;        // 44 0
};
static_assert(sizeof(DrawMetaBatchV1) == 48);
static_assert(sizeof(DrawMetaRecordV1) == 48);
static_assert(offsetof(DrawMetaBatchV1, kick_epoch) == 16);
static_assert(offsetof(DrawMetaRecordV1, path_id) == 32);

// True only while verification is on (dist file native_bridge_verify, default OFF, polled once a
// second at the frame boundary). The disabled draw path costs this one relaxed atomic load.
extern std::atomic<bool> g_gate_enabled;
inline bool Enabled() { return g_gate_enabled.load(std::memory_order_relaxed); }
// FRAME9: the kick / tile-select hooks need the submit lock (every mode except native_frame_meta without lean).
// Metadata-only mode needs no lock: the batch is bound to the SubmitCommandStream of the same thread.
extern std::atomic<bool> g_kick_lock;
inline bool KickLock() { return g_kick_lock.load(std::memory_order_relaxed); }

// Scopes only touch their thread-local context while verification is on (off: one atomic load).
class InstanceScope {
 public:
  explicit InstanceScope(uint32_t instance_guest);
  ~InstanceScope();
  InstanceScope(const InstanceScope&) = delete;
  InstanceScope& operator=(const InstanceScope&) = delete;

 private:
  uint32_t previous_instance_;
  bool on_;
};

class MeshScope {
 public:
  MeshScope(uint32_t pass_id, uint32_t mesh_guest);
  ~MeshScope();
  MeshScope(const MeshScope&) = delete;
  MeshScope& operator=(const MeshScope&) = delete;

 private:
  uint32_t previous_pass_;
  uint32_t previous_mesh_;
  uint32_t previous_instance_;
  bool on_;
};


struct WriterArgs {
  uint32_t device, lr, r4, r5, r6, r7, r8, r9;
};

// Wraps one call of an engine draw writer while verification is on: remembers the device's command
// buffer write pointer (device +40) before the call and, when it goes out of scope after the call,
// decodes the packets the call wrote and submits one record per draw packet. Only the outermost
// writer on a thread records (writers that call other writers are not counted twice).
class WriterCall {
 public:
  WriterCall(uint8_t* base, uint32_t path, const WriterArgs& args);
  ~WriterCall();
  WriterCall(const WriterCall&) = delete;
  WriterCall& operator=(const WriterCall&) = delete;

 private:
  uint8_t* base_;
  uint32_t path_;
  WriterArgs args_;
  uint32_t ring_before_;
  uint32_t kick_epoch_ = 0;  // this thread's kick count when the call started
  uint32_t gen_ = 0;         // verification generation when the call started (its records carry it)
  bool outer_;
};

// 825D3218 (D3D queues a command buffer segment, r3 device, r4 -> {dwords, GPU address}): walks exactly
// that segment and records the draw packets in it that no hooked writer recorded (path 9), before the CP
// can execute them.
void OnKick(uint8_t* base, uint32_t r3, uint32_t gpu_addr, uint32_t dwords);

// NATIVE7: while Enabled(), the callers hold this (recursive) lock across OnKick / OnTileSelect AND the
// SubmitCommandStream that follows, so the bridge's kick walks happen in the CP's submission order.
void LockSubmit();
void UnlockSubmit();

// 825DF0B8 (tiling executor's tile select, forwarded to the CP): the n guest-order (big-endian) words it
// wrote into the primary ring. Keeps the CP's bin mask / select for the kick walks' predication test.
void OnTileSelect(const uint32_t* words_be, uint32_t n);

// FRAME9 (native_frame_meta): 825DF0B8 calls this while Enabled(), immediately before its SubmitCommandStream (empty
// TILE_CONTROL metadata batch, keeps the SDK's batch-to-submission association).
void OnTileSubmit();

// Called after 82194BD8 has joined/reset the completed game frame.
void FrameBoundary();

// M3 EXECUTOR, game side (AGENT_NATIVE23B2; native_m3.inc, dist native_m3_shadow / native_m3_replace, default OFF).
inline thread_local uint32_t t_m3_kicks = 0;  // 825D3218 calls on this thread (a kick inside a writer call disqualifies it)
int M3Mode();                                 // 0 off, 1 shadow, 2 replace (dist files read once, SDK capability checked)
inline int M3ModeCached() { static const int m = M3Mode(); return m; }
struct M3Call {
  uint32_t dev = 0, wp0 = 0, marker_at = 0, kicks0 = 0;
  bool active = false;
  bool owned = false;  // replace mode: OWNED record + NOP span (key OWNABLE), else shadow record + reserved marker
};
bool M3Begin(uint8_t* base, uint32_t dev, uint32_t lr, M3Call& c);  // before the writer; false = not a candidate
void M3End(uint8_t* base, const M3Call& c, const WriterArgs& a);    // after the writer (no-op unless M3Begin returned true)

}  // namespace native_renderer_bridge
