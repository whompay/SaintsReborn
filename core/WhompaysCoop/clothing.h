// Co-op 3.0 clothing pool (mod.ini clothing_pool = 1): the parts of
// coop.cpp's clothing memory that need no game code - the arenas the
// component buffers are cut from, which tier (full outfit or base layer) a
// player is dressed in, which of an outfit's items go on, and which player's
// outfit changes next. The same file runs in the mod and in
// tests/clothing_test.cpp.
//
// In single player the game keeps one ring of clothing components per
// category (54, table 0x8310CF78) and hands any free one to whoever asks
// (8248D5F0 looks at the owner only with the multiplayer byte set). A worn
// item needs a free component in its category; one that finds none stays
// listed without a piece ("No place to put streaming component model"), and
// nothing in the game asks again. So components are added per category for
// what the characters here wear, never more than physical memory allows, and
// an item that has no component is left off rather than put on. Everyone's
// base layer comes before anyone's hat (GrowRings), and a player left short
// is dressed again only once memory changed (MayRedress). When memory has run
// out with a player here still lacking part of its base layer, another
// player's extras (hats, glasses ...) are taken off, the farthest first, and
// their memory goes to that base layer (PickTake); they come back once memory
// allows, the nearest first. Past own_body_max players the 16 MB body block
// isn't made (DecideBodyBlock).
#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

namespace coopclothing {

constexpr int kCategories = 54;
constexpr unsigned kSpare = 1;                  // a free component per category in use, while memory lasts (a take-off and wear overlapping)
constexpr uint32_t kArenaBytes = 0x100000;      // 1 MB physical blocks (82716618), not one 5.9 MB block per player
constexpr uint32_t kArenaAlign = 0x10000;       // (64 KB pages, as 82184260 allocates)
constexpr uint32_t kBufferAlign = 4096;         // a component buffer's alignment (82489EF8 / 8248DAA0 ask their heaps for 4096)
// The floor (research tools/coop_test/TEST-3.0-bots12-findings.md 1): the game
// itself swings 5-6 MB below the free memory the arenas were made at (22 -> 17
// MB, then down to 11 MB with no co-op allocation, PC host) and was seen
// running at 11 MB. So: no arena that leaves less than kFloorMb free now, nor
// one that would take the lowest reading of the last kLowWindowMs below
// kLowFloorMb (each reading counts co-op's arenas as free, so the arenas made
// since count against it: review_clothing12 H3); until kLowHistoryMs of
// readings exist, or for a minute after a gap of kLowGapMs in them (a load,
// between sessions: the dips there weren't seen), kStartFloorMb (3.0's).
constexpr uint32_t kFloorMb = 12;
constexpr uint32_t kLowFloorMb = 11;
constexpr uint32_t kStartFloorMb = 16;
constexpr uint64_t kLowWindowMs = 300000;
constexpr uint64_t kLowHistoryMs = kLowWindowMs;
constexpr uint64_t kLowGapMs = 10000;
constexpr uint32_t kUnknownMb = ~0u;            // free memory not known (MmQueryStatistics refused)
constexpr uint64_t kUnknownCapBytes = 24u << 20;   // ... then the arenas stop at this in all (about four of 2.4's sets)
constexpr float kNearM = 60.0f;                 // dressed in full within this when memory is short ...
constexpr float kHysteresisM = 20.0f;           // ... and kept in full to kNearM + this
constexpr uint64_t kSwapMs = 10000;             // at most one outfit change per player in this time ...
constexpr uint64_t kDressGapMs = 1000;          // ... and one dressing (50-90 ms each) a second for everyone
constexpr uint64_t kPoolGapMs = 5000;           // ... but one the pool begins (PickChange, PickShed) every 5 s at most (review_overnight M4)
constexpr uint64_t kShortHoldMs = 60000;        // memory counts as short this long after a player was last left short
constexpr uint64_t kUpgradeHoldMs = 60000;      // a far player whose way back into its full outfit came up short waits this long for the next ...
constexpr uint64_t kUpgradeHoldMaxMs = 480000;  // ... doubled each time, up to this

// The base layer: what everyone here gets before anyone gets the rest, and
// what a far player is dressed in when memory is short (category names at
// 0x827ACBD8): hair head, undershirt, overshirt, underwear, pants, shoes and
// skin. (Eyes, hats, glasses ... are the rest.)
inline bool BaseLayer(int c) {
  switch (c) {
    case 0: case 5: case 6: case 8: case 9: case 11: case 44: return true;
    default: return false;
  }
}
inline uint64_t Bit(int c) { return c >= 0 && c < kCategories ? uint64_t(1) << c : 0; }
// A category of one outfit's base layer (promoted: what that outfit's base
// layer needs besides, Promote).
inline bool BaseFor(int c, uint64_t promoted) { return BaseLayer(c) || (promoted & Bit(c)) != 0; }
// An item of the base layer that the plan put on and the game didn't
// (dropped), while other items were left off (left_off): it needs one of them.
// The hair in its hat wear (CUST_WEAR_HAT, the bots' and any hat wearer's)
// needs the hat (category 12, the worn key): it went when the fedora was left
// off. That one counts as that outfit's base layer from then on. Any other
// drop (an item substitution, a category the lookup got wrong) promotes
// nothing: promoting all that was left off made the whole outfit base layer
// for good (review_clothing12 L4); the caller logs it.
constexpr int kHairHead = 0, kHeadwear = 12;
inline uint64_t Promote(uint64_t promoted, uint64_t dropped, uint64_t left_off) {
  if ((dropped & Bit(kHairHead)) && BaseFor(kHairHead, promoted) && (left_off & Bit(kHeadwear))) return promoted | Bit(kHeadwear);
  return promoted;
}
// Can a category's ring be grown: a Max_equipped (+8) and a Slot_size (+12)
// up to 8 MB (EnsureComponents skips the others too). An item in one that
// can't is planned like one not known here: it goes on, never "left off for
// memory".
inline bool Growable(uint32_t max_equipped, uint32_t slot_size) { return max_equipped && slot_size && slot_size <= 0x800000; }

// ---- Arenas: component buffers cut from 1 MB physical blocks ----
struct Arena { uint32_t base = 0, used = 0, size = 0; };

inline uint32_t AlignUp(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }
// The bytes of one component buffer (its category's Slot_size, 4 KB aligned).
inline uint32_t BufferBytes(uint32_t slot_size) { return AlignUp(slot_size, kBufferAlign); }
// The size of a new arena for a buffer of this many bytes.
inline uint32_t ArenaBytesFor(uint32_t bytes) { return std::max(kArenaBytes, AlignUp(bytes, kArenaAlign)); }
// First fit across the arenas (the first that still has room); 0 when none has.
inline uint32_t ArenaTake(std::vector<Arena>& arenas, uint32_t bytes) {
  for (Arena& a : arenas) {
    const uint32_t at = AlignUp(a.used, kBufferAlign);
    if (at <= a.size && a.size - at >= bytes) { a.used = at + bytes; return a.base + at; }
  }
  return 0;
}
// Undoes the last ArenaTake (the component object for it wasn't there).
inline void ArenaGiveBack(std::vector<Arena>& arenas, uint32_t address, uint32_t bytes) {
  for (Arena& a : arenas)
    if (address >= a.base && address - a.base + bytes == a.used) { a.used = address - a.base; return; }
}
// A component buffer free again (its component given back), for any category
// whose buffer fits in it: all are 4 KB aligned in the same physical memory,
// and a component only uses its category's Slot_size of it.
struct Spare { uint32_t buffer = 0, bytes = 0; };
// The smallest spare of at least this many bytes (-1: none).
inline int BestSpare(const std::vector<Spare>& spares, uint32_t bytes) {
  int best = -1;
  for (size_t i = 0; i < spares.size(); ++i)
    if (spares[i].bytes >= bytes && (best < 0 || spares[i].bytes < spares[size_t(best)].bytes)) best = int(i);
  return best;
}
// (memory_fair_notes 1) Takes this many bytes from the smallest spare that has
// them; the rest of it stays a spare (all 4 KB multiples, so still aligned).
// 0: none has. The whole spare went before: a 2 MB run of a player's freed
// extras would have gone into one pants buffer.
inline uint32_t SpareTake(std::vector<Spare>& spares, uint32_t bytes) {
  const int i = BestSpare(spares, bytes);
  if (i < 0) return 0;
  Spare& s = spares[size_t(i)];
  const uint32_t at = s.buffer;
  if (s.bytes == bytes) spares.erase(spares.begin() + i);
  else { s.buffer += bytes; s.bytes -= bytes; }
  return at;
}
// A buffer free again: merged with the spares right before and after it in the
// same arena (a player's hat, glasses and gloves were cut one after another, so
// together they can hold a pair of pants), and handed back to its arena's free
// tail when it ends there (ArenaTake and RestFits see it then). Never merged
// across two arenas: they go back to the game one by one (ArenaUnused), and the
// game carves them back to back.
inline void SpareAdd(std::vector<Spare>& spares, std::vector<Arena>& arenas, Spare s) {
  for (Arena& a : arenas) {
    if (s.buffer < a.base || s.buffer - a.base >= a.size) continue;
    for (bool merged = true; merged;) {
      merged = false;
      for (size_t i = 0; i < spares.size(); ++i) {
        const Spare o = spares[i];
        if (o.buffer < a.base || o.buffer - a.base >= a.size) continue;
        if (o.buffer + o.bytes == s.buffer) s = {o.buffer, o.bytes + s.bytes};
        else if (s.buffer + s.bytes == o.buffer) s.bytes += o.bytes;
        else continue;
        spares.erase(spares.begin() + long(i)); merged = true; break;
      }
    }
    if (s.buffer + s.bytes == a.base + a.used) { a.used = s.buffer - a.base; return; }
    break;
  }
  spares.push_back(s);
}
// Is there memory for one more buffer of this many bytes without a new arena:
// a spare that holds it, or an arena's free tail.
inline bool BufferRoom(const std::vector<Spare>& spares, const std::vector<Arena>& arenas, uint32_t bytes) {
  if (BestSpare(spares, bytes) >= 0) return true;
  for (const Arena& a : arenas) {
    const uint32_t at = AlignUp(a.used, kBufferAlign);
    if (at <= a.size && a.size - at >= bytes) return true;
  }
  return false;
}
// The bytes of all the arenas made.
inline uint64_t ArenasBytes(const std::vector<Arena>& arenas) {
  uint64_t n = 0;
  for (const Arena& a : arenas) n += a.size;
  return n;
}
// ... in MB (rounded up), as each free-memory reading is noted with them.
inline uint32_t ArenasMb(uint64_t arenas_bytes) { return uint32_t((arenas_bytes + 0xFFFFF) >> 20); }
// The lowest reading of the last 5 min as it would be with the arenas there
// now (LowWater notes free + the arenas then).
inline int LowNowMb(uint32_t low_mb, uint64_t arenas_bytes) { return int(low_mb) - int(ArenasMb(arenas_bytes)); }
// The MB new arenas may still take with this much free physical memory now
// and this lowest reading of the last 5 min (free + the arenas at each
// reading; kUnknownMb: not enough readings yet); arenas_bytes: those made so
// far (free memory not known: up to kUnknownCapBytes in all).
inline uint32_t RoomMb(uint32_t free_mb, uint32_t low_mb, uint64_t arenas_bytes) {
  if (free_mb == kUnknownMb) return arenas_bytes >= kUnknownCapBytes ? 0 : uint32_t((kUnknownCapBytes - arenas_bytes) >> 20);
  const int room = std::min(int(free_mb) - int(kFloorMb),
                            low_mb == kUnknownMb ? int(free_mb) - int(kStartFloorMb) : std::min(LowNowMb(low_mb, arenas_bytes), int(free_mb)) - int(kLowFloorMb));
  return room > 0 ? uint32_t(room) : 0;
}
// The release path (review_clothing12 M1): below kHardFloorMb free, or with
// the lowest reading of 5 min (less the arenas now) below kHardLowMb, memory
// is low: no growth at all, far players' items are shed and free co-op
// components and empty arenas given back, until growth itself would be
// allowed again: kFloorMb free and the projected low kLowFloorMb (4 MB / 2 MB
// above the trigger). It cleared only above 14 MB before, but the pool grows
// down to 12 MB, so after one dip free memory settled at 12-14 MB and memory
// stayed low (review_clothing_release R2).
constexpr uint32_t kHardFloorMb = 8;
constexpr uint32_t kHardLowMb = 9;
inline bool MemoryLow(bool was_low, uint32_t free_mb, uint32_t low_mb, uint64_t arenas_bytes) {
  if (free_mb == kUnknownMb) return was_low;
  const bool low_known = low_mb != kUnknownMb;
  if (free_mb < kHardFloorMb || (low_known && LowNowMb(low_mb, arenas_bytes) < int(kHardLowMb))) return true;
  return was_low && (free_mb < kFloorMb || (low_known && LowNowMb(low_mb, arenas_bytes) < int(kLowFloorMb)));
}
// An arena may go back (827166B0) once no component holds a buffer cut from
// it (live: the buffers of co-op's components still in their rings).
inline bool ArenaUnused(const Arena& a, const std::vector<uint32_t>& live) {
  for (uint32_t b : live)
    if (b >= a.base && b - a.base < a.size) return false;
  return true;
}
// May a new arena of this size be made?
inline bool ArenaAllowed(uint32_t free_mb, uint32_t arena_bytes, uint64_t arenas_bytes, uint32_t low_mb = kUnknownMb) {
  if (free_mb == kUnknownMb) return arenas_bytes + arena_bytes <= kUnknownCapBytes;
  return (arena_bytes + 0xFFFFF) / 0x100000 <= RoomMb(free_mb, low_mb, arenas_bytes);
}
// The lowest free-memory reading of the last kLowWindowMs (in 10 buckets),
// kUnknownMb before kLowHistoryMs of readings and for a minute after a gap of
// more than kLowGapMs between two.
struct LowWater {
  static constexpr int kBuckets = 10;
  uint64_t first = 0, last = 0, gap_until = 0;
  uint64_t start[kBuckets] = {};
  uint32_t low[kBuckets] = {};
  bool used[kBuckets] = {};
  void Note(uint64_t now, uint32_t mb) {
    if (mb == kUnknownMb) return;
    if (!first) first = now + 1;
    if (last && now - last > kLowGapMs) gap_until = now + 60000;
    last = now;
    const uint64_t span = kLowWindowMs / kBuckets, at = now / span * span;
    const int i = int(now / span % kBuckets);
    if (!used[i] || start[i] != at) { used[i] = true; start[i] = at; low[i] = mb; }
    else low[i] = std::min(low[i], mb);
  }
  uint32_t Low(uint64_t now) const {
    if (!first || now + 1 - first < kLowHistoryMs || now < gap_until) return kUnknownMb;
    uint32_t mb = kUnknownMb;
    for (int i = 0; i < kBuckets; ++i)
      if (used[i] && start[i] + kLowWindowMs > now) mb = std::min(mb, low[i]);
    return mb;
  }
};

// ---- Tiers: nearest first ----
// kNone: all its items left off (memory low, PickShed), until memory is back.
enum Tier : uint8_t { kFull = 0, kBase = 1, kNone = 2 };
// Everyone in full while memory lasts; when short, the full outfit within
// kNearM (kept to kNearM + kHysteresisM), the base layer further out.
inline Tier PickTier(float distance, Tier was, bool short_of_memory) {
  if (!short_of_memory || distance <= kNearM) return kFull;
  return was == kFull && distance <= kNearM + kHysteresisM ? kFull : kBase;
}

// ---- One outfit ----
// cats[i]: item i's category (-1: not known here; it goes on and the game
// shows where it went). The base layer goes first, then the rest, each in
// list order.
// Components the outfit asks for per category, with what the tier keeps.
inline void Want(const int* cats, unsigned n, Tier tier, uint16_t* want, uint64_t promoted = 0) {
  std::fill(want, want + kCategories, uint16_t(0));
  for (unsigned i = 0; i < n; ++i)
    if (cats[i] >= 0 && cats[i] < kCategories && (tier == kFull || (tier == kBase && BaseFor(cats[i], promoted)))) ++want[cats[i]];
}
// What one category can give this outfit: every free component (none is kept
// back: a spare left free while someone goes without pants is what left the
// second bot of the 12-bot test with half an outfit).
inline int Usable(unsigned count, unsigned in_use_others) { return std::max(0, int(count) - int(in_use_others)); }

// How far a category's ring is grown before this outfit goes on, in stages:
// 1. this outfit's base layer, 2. the base layers the other players here
// still lack (no component held for an item of theirs), 3. the rest of this
// outfit, 4. a spare in each category of its base layer (anyone's next pants;
// a spare hat is only good for another hat). 3 and 4 only with room left after
// them for one more base layer (RestFits). A stage is grown only once every
// ring the stages before it asked for was: when memory runs out, nobody's hat
// comes before someone else's pants.
struct Grow {
  unsigned count = 0;          // components in the ring
  unsigned others = 0;         // in use by the others here (not this record's)
  uint16_t base = 0, rest = 0; // this outfit's items in it (the tier's): its base layer / the rest
  uint16_t reserve = 0;        // the others' base-layer items in it that hold no component
};
enum Stage { kStageBase = 0, kStageOthersBase = 1, kStageRest = 2, kStageSpare = 3, kStages = 4 };
inline unsigned StageTarget(const Grow& g, int stage) {
  const unsigned own = g.others + g.base;
  switch (stage) {
    case kStageBase: return g.base ? own : 0;
    case kStageOthersBase: return g.reserve ? own + g.reserve : 0;
    case kStageRest: return g.rest ? own + g.reserve + g.rest : 0;
    default: return g.base ? own + g.reserve + kSpare : 0;
  }
}
// The rest of an outfit may have memory when, after the components it needs
// (rest_bytes; 0: all have a free one), there is still room for one more base
// layer (headroom_bytes: this outfit's): the next player to join still gets
// dressed.
inline bool RestFits(uint64_t rest_bytes, uint64_t headroom_bytes, uint64_t room_bytes) {
  return !rest_bytes || rest_bytes + headroom_bytes <= room_bytes;
}
// That headroom: this outfit's base layer (base_bytes) for the next player to
// join and for each player here whose base layer can't be reserved yet (no
// outfit received: unknown_players). Those whose outfit is known were grown
// in stage 2 already, so they aren't counted twice (review_clothing12 M6).
inline uint64_t HeadroomBytes(uint64_t base_bytes, unsigned unknown_players) { return base_bytes * (1u + unknown_players); }
// What a category outside this outfit's base layer can give it: the free
// components less the others' base-layer items still without one (reserve),
// so its hat never takes the hat another player's base layer needs (a
// promoted one). In its own base-layer categories it takes any (stage 1 comes
// first).
inline int UsableRest(unsigned count, unsigned in_use_others, unsigned reserve) {
  return std::max(0, int(count) - int(in_use_others) - int(reserve));
}
// add(c): one more component in category c (false: no memory for it).
// rest_ok(g, stage): RestFits for the components stage 3 or 4 adds, asked
// once the stages before it are grown. Returns the stages grown in full
// (kStages: all).
template <class Add, class RestOk>
int GrowRings(Grow* g, Add add, RestOk rest_ok) {
  for (int stage = 0; stage < kStages; ++stage) {
    if ((stage == kStageRest || stage == kStageSpare) && !rest_ok(static_cast<const Grow*>(g), stage)) return stage;
    bool all = true;
    for (int c = 0; c < kCategories; ++c) {
      const unsigned target = StageTarget(g[c], stage);
      while (g[c].count < target) {
        if (!add(c)) { all = false; break; }
        ++g[c].count;
      }
    }
    if (!all) return stage;
  }
  return kStages;
}

struct Fit {
  std::vector<uint8_t> keep;      // per item: 1 = goes on
  unsigned kept = 0, unknown = 0, left_far = 0, left_short = 0;
  uint64_t on = 0, extras = 0;    // category bits put on (all / outside the base layer)
  uint64_t distant = 0, lacking = 0;   // category bits left off for the distance / for memory
  int grown = kStages;            // the stages its plan grew in full (GrowRings)
};
// usable[c] is used up as items go on.
inline Fit FitOutfit(const int* cats, unsigned n, Tier tier, int* usable, uint64_t promoted = 0) {
  Fit f;
  f.keep.assign(n, 0);
  for (int pass = 0; pass < 2; ++pass)
    for (unsigned i = 0; i < n; ++i) {
      const int c = cats[i];
      const bool base = c < 0 || c >= kCategories || BaseFor(c, promoted);
      if ((pass == 0) != base) continue;
      if (tier == kNone) { ++f.left_far; f.distant |= Bit(c); continue; }
      if (c < 0 || c >= kCategories) { f.keep[i] = 1; ++f.kept; ++f.unknown; continue; }
      if (tier == kBase && !base) { ++f.left_far; f.distant |= Bit(c); continue; }
      if (usable[c] <= 0) { ++f.left_short; f.lacking |= Bit(c); continue; }
      --usable[c];
      f.keep[i] = 1; ++f.kept; f.on |= Bit(c);
      if (!base) f.extras |= Bit(c);
    }
  return f;
}
// The worn list (a count byte, then 4 bytes per item, as 8248DCA8 packs it)
// with only the kept items.
inline std::vector<uint8_t> Filter(const uint8_t* worn, unsigned length, const std::vector<uint8_t>& keep) {
  std::vector<uint8_t> out(1, 0);
  if (!length || length != 1u + 4u * worn[0] || keep.size() != worn[0]) return std::vector<uint8_t>(worn, worn + length);
  for (unsigned i = 0; i < keep.size(); ++i)
    if (keep[i]) { out.insert(out.end(), worn + 1 + 4 * i, worn + 5 + 4 * i); ++out[0]; }
  return out;
}

// ---- Outfit changes (3.0, appearance_feedback_research 2) ----
// The sender: a worn list that keeps changing (the SR Tweak Menu's store puts
// an item on as you hover over it, about every 0.5 s; a preset outfit is
// dozens of wears in one call) goes out once it has stayed the same for
// kWornSettleMs. The first one goes at once (a player who joins gets the
// settled one).
constexpr uint64_t kWornSettleMs = 1500;
struct WornSettle {
  std::vector<uint8_t> read;      // the list as last read
  std::vector<uint8_t> settled;   // the list to send
  uint64_t since = 0;             // when `read` last changed
  // The list read now (empty: not read). True when `settled` changed.
  bool Feed(const std::vector<uint8_t>& now_read, uint64_t now) {
    if (now_read.empty()) return false;
    if (now_read != read) { read = now_read; since = now; }
    if (read == settled || (!settled.empty() && now - since < kWornSettleMs)) return false;
    settled = read;
    return true;
  }
  // When to read again to see a change settle (0: none waiting).
  uint64_t DueAt() const { return !read.empty() && read != settled ? since + kWornSettleMs : 0; }
};
// The receiver: an outfit that changes on a character dressed by the pool
// comes off and goes on by difference, so the rest of it stays on (taking
// everything off showed the bare body for a few frames at every change).
// pieces[i]: the piece item i of the plan's list goes on as (0: not known
// here); nodes: the record's nodes, each its piece and whether its component
// is there and its own. What the plan left off comes off too (the pool
// decides what goes on; this only changes how). A node whose piece is gone
// or taken over comes off and is worn again. ok = false: an item not known
// here (dress it all). (Only `off` goes through 8248D198, then the wear: not
// 8248D340, which takes every node off: `while (*record) 8248D198(record,
// (*record)->piece)`.)
struct RecordNode { uint32_t piece = 0; bool on = false; };
struct Diff {
  bool ok = false;
  std::vector<uint32_t> off;      // the pieces to take off (8248D198)
  std::vector<uint8_t> wear;      // per item of the list: 1 = put on (Filter)
  unsigned worn = 0, kept = 0;    // items to put on, items left as they are
};
inline Diff DressDiff(const uint32_t* pieces, unsigned n, const std::vector<RecordNode>& nodes) {
  Diff d;
  d.wear.assign(n, 0);
  for (unsigned i = 0; i < n; ++i) if (!pieces[i]) return d;
  auto listed = [&](uint32_t piece) { for (unsigned i = 0; i < n; ++i) if (pieces[i] == piece) return true; return false; };
  auto on = [&](uint32_t piece) { for (const RecordNode& x : nodes) if (x.piece == piece && x.on) return true; return false; };
  // (A piece with a node that is on stays, even with a second stale node:
  // 8248D198 takes off the first node of that piece.)
  for (const RecordNode& x : nodes)
    if (x.piece && (!listed(x.piece) || !on(x.piece)) && std::find(d.off.begin(), d.off.end(), x.piece) == d.off.end()) d.off.push_back(x.piece);
  for (unsigned i = 0; i < n; ++i) {
    if (on(pieces[i])) ++d.kept; else { d.wear[i] = 1; ++d.worn; }
  }
  d.ok = true;
  return d;
}
// Right after the wear (8248DE28 links the nodes before their pieces load):
// the items it was given that no node holds. The game didn't put them on
// alone (an item that needs another one going on with it): that outfit is
// dressed again in full, once.
inline unsigned NotWorn(const uint32_t* pieces, const std::vector<uint8_t>& wear, const std::vector<RecordNode>& nodes) {
  unsigned missing = 0;
  for (size_t i = 0; i < wear.size(); ++i) {
    if (!wear[i]) continue;
    bool linked = false;
    for (const RecordNode& x : nodes) linked |= x.piece == pieces[i];
    missing += !linked;
  }
  return missing;
}

// ---- Dressing again ----
// Memory changes (an arena made, buffers back from the game's cleanup, a
// character removed, a dressing that put on less, room for an arena again)
// count up an epoch. A player is dressed again (pieces unloaded, the slot
// listing something else, a component free for what it lacks) not before its
// next_change (its last dressing + kSwapMs), not before the session's
// next_dress (the last one's start + kDressGapMs), and, when its last
// dressing left something off for memory, only in a later epoch: the same
// memory would give it the same items again (the 12-bot test dressed 11
// players once a second for nothing).
inline bool MayRedress(uint64_t now, uint64_t next_change, uint64_t next_dress, bool left_short, uint32_t dressed_epoch, uint32_t epoch) {
  return now >= next_change && now >= next_dress && (!left_short || dressed_epoch != epoch);
}

// ---- Which player's outfit changes next (one a second at most) ----
struct View {
  bool dressed = false;           // dressed and not being dressed again now
  float distance = 0;
  Tier tier = kFull;              // what it is dressed in
  uint64_t extras = 0, distant = 0, lacking = 0;   // category bits (Fit)
  uint64_t next_change = 0;       // its last change + kSwapMs
  uint64_t upgrade_after = 0;     // no way back into the full outfit before this (NextUpgradeHold)
  bool changed = true;            // memory changed since its last dressing (MayRedress)
  bool wearing = true;            // something of its outfit is on (PickShed)
  bool on_screen = false;         // its character on this game's screen (PickChange rule 1, PickShed: those off it first)
  uint64_t base_lacking = 0;      // lacking in its base layer (its promoted categories too): fair sharing (PickTake)
};
struct Change { int index = -1; Tier tier = kFull; };
// (review_overnight M4) A change the pool begins (a tier swap, a retry, a
// shed: each wear step 50-90 ms in one update) waits for the session's 1 s
// gap and for next_pool (the last one the pool began + kPoolGapMs): at 12
// players those kept the 1 s slot busy. A new record and a first dressing
// (a player who just joined) only wait for the 1 s gap.
inline bool PoolChangeDue(uint64_t now, uint64_t next_dress, uint64_t next_pool) { return now >= next_dress && now >= next_pool; }
// The player to take items from first: one off this screen before one on it (nobody sees its outfit change), then
// the farther.
inline bool TakeFirst(const View& x, bool best_on, float best_d) { return best_d < 0 || (x.on_screen != best_on ? !x.on_screen : x.distance > best_d); }
// The wait before a far player's next way back into its full outfit: none
// after one that fitted, kUpgradeHoldMs after one that came up short (it would
// go back into the base layer and up again), doubled each time after that.
inline uint64_t NextUpgradeHold(uint64_t hold, bool came_up_short) {
  return came_up_short ? std::min(kUpgradeHoldMaxMs, hold ? hold * 2 : kUpgradeHoldMs) : 0;
}
// free_cats: the categories with a free component.
// room: a new arena may be made (ArenaAllowed).
// 1. While memory is short, a far player (PickTier: beyond kNearM +
//    kHysteresisM) wearing something outside its base layer that a player
//    more than kHysteresisM nearer lacks: into the base layer (one off this
//    screen first, then the farthest: TakeFirst). Nobody nearer than that
//    loses what it wears for someone else.
// 2. A player left without something that now has a free component or for
//    which an arena may be made, or back within range of its full outfit:
//    dressed again (the nearest first), a retry only once memory changed since
//    its last dressing (its own arenas move the epoch before the dressing is
//    noted, so one that came up short again waits for the next change). A
//    near player in the base layer goes back into its full outfit as soon as
//    one of its items is free or an arena may be made. A far player (beyond
//    kNearM) goes back into its full outfit only once memory changed, when all
//    it left off has a free component, or memory isn't short and an arena may
//    be made, and not before its upgrade_after: part of it would leave it
//    short, and rule 1 would send it back into the base layer.
// base_first (fair sharing, memory_fair_notes 1): a player here lacks part of
// its base layer. Then nobody gets anything outside the base layer: no rule 1
// (it moves extras to a nearer player), no way back into a full outfit, only
// a retry of a player lacking base-layer items, once one of them has a free
// component or room (then: room for a base-layer buffer, a spare or an arena),
// and a shed player back into its base layer.
inline Change PickChange(const View* v, unsigned n, bool short_of_memory, uint64_t free_cats, bool room, uint64_t now, bool base_first = false) {
  Change best;
  float best_d = -1;
  bool best_on = false;
  if (base_first) {
    float near_d = 0;
    for (unsigned i = 0; i < n; ++i) {
      const View& x = v[i];
      if (!x.dressed || now < x.next_change) continue;
      const bool retry = x.changed && x.base_lacking && ((x.base_lacking & free_cats) != 0 || room);
      if ((retry || x.tier == kNone) && (best.index < 0 || x.distance < near_d)) { best = {int(i), x.tier == kNone ? kBase : x.tier}; near_d = x.distance; }
    }
    return best;
  }
  if (short_of_memory)
    for (unsigned i = 0; i < n; ++i) {
      const View& x = v[i];
      if (!x.dressed || now < x.next_change || x.tier != kFull || !x.extras || PickTier(x.distance, x.tier, true) != kBase) continue;
      for (unsigned a = 0; a < n; ++a)
        if (a != i && v[a].dressed && (v[a].lacking & x.extras) && x.distance > v[a].distance + kHysteresisM && TakeFirst(x, best_on, best_d)) {
          best = {int(i), kBase}; best_d = x.distance; best_on = x.on_screen;
        }
    }
  if (best.index >= 0) return best;
  float near_d = 0;
  for (unsigned i = 0; i < n; ++i) {
    const View& x = v[i];
    if (!x.dressed || now < x.next_change) continue;
    const Tier want = PickTier(x.distance, x.tier, short_of_memory);
    const bool all_free = x.distant && !(x.distant & ~free_cats);
    const bool upgrade = x.tier == kBase && want == kFull && x.distant &&
                         (x.distance <= kNearM ? (x.distant & free_cats) != 0 || room
                                               : x.changed && now >= x.upgrade_after && (all_free || (!short_of_memory && room)));
    const bool retry = x.changed && x.lacking && ((x.lacking & free_cats) != 0 || room);
    const bool unshed = x.tier == kNone;   // (memory is back: PickChange isn't asked while it is low)
    // (A retry of a far player in the base layer stays in it unless the upgrade may be made.)
    const Tier to = x.tier == kBase && !upgrade && x.distance > kNearM ? kBase : want;
    if ((upgrade || retry || unshed) && (best.index < 0 || x.distance < near_d)) { best = {int(i), to}; near_d = x.distance; }
  }
  return best;
}
// While memory is low (MemoryLow): the farthest player wearing something
// outside its base layer into it; with none left, and strip (still below
// kHardFloorMb), the farthest still wearing anything out of all its items
// (kNone), only one beyond kNearM (a player next to you is never undressed:
// review_clothing_release R3). Off this screen first each time (TakeFirst).
// Its 10 s don't hold this back; the pool's 5 s gap (PoolChangeDue) and one
// dressing at a time do (the caller), and MayShed. Each frees components,
// which ClothingGiveBack can then hand back.
inline Change PickShed(const View* v, unsigned n, bool strip) {
  Change best;
  float best_d = -1;
  bool best_on = false;
  for (int pass = 0; pass < 2 && best.index < 0; ++pass) {
    if (pass == 1 && !strip) break;
    for (unsigned i = 0; i < n; ++i) {
      const View& x = v[i];
      if (!x.dressed || !TakeFirst(x, best_on, best_d)) continue;
      if (pass == 0 ? x.tier == kFull && x.extras : x.tier != kNone && x.wearing && x.distance > kNearM) { best = {int(i), pass == 0 ? kBase : kNone}; best_d = x.distance; best_on = x.on_screen; }
    }
  }
  return best;
}
// Whether shedding may go on (review_clothing_release R3): only while co-op
// holds arenas (else the low is the game's own and shedding frees nothing),
// and not after kShedNoGainMs of shedding in which no arena went back (the
// arenas mix players and categories, so shedding may free none); an arena
// freed starts the time again. Reset the watch when memory low clears.
constexpr uint64_t kShedNoGainMs = 30000;
struct ShedWatch { uint64_t since = 0; unsigned freed = 0; };
inline bool MayShed(ShedWatch& w, uint64_t now, size_t arenas, unsigned arenas_freed) {
  if (!arenas) { w = {}; return false; }
  if (!w.since || arenas_freed != w.freed) { w.since = now; w.freed = arenas_freed; return true; }
  return now - w.since < kShedNoGainMs;
}

// ---- A co-op component's idle time (ClothingGiveBack) ----
// It goes back only after kGiveBackMs seen idle at every look in between
// (review_memfair M2): a look missed for over kIdleGapMs (its category left
// out of a partial give-back, a busy clothing queue) starts the time again,
// so a piece worn and taken off meanwhile never goes back on a stale start.
// Freeing (fair sharing takes nothing more meanwhile) counts one idle for
// under kFreeingMaxMs only (review_memfair L3: one that can't be unlinked
// never holds the takes for good; a busy queue that skipped the look still
// counts, the take waits for it).
constexpr uint64_t kGiveBackMs = 5000, kIdleGapMs = 1500, kFreeingMaxMs = 15000;
struct IdleWatch { uint64_t since = 0, seen = 0; };
inline void IdleSeen(IdleWatch& w, uint64_t now) {
  if (!w.since || now - w.seen > kIdleGapMs) w.since = now;
  w.seen = now;
}
inline void IdleBusy(IdleWatch& w) { w = {}; }
inline bool IdleCurrent(const IdleWatch& w, uint64_t now) { return w.since && now - w.seen <= kIdleGapMs; }
inline bool IdleLongEnough(const IdleWatch& w, uint64_t now) { return IdleCurrent(w, now) && now - w.since >= kGiveBackMs; }
inline bool IdleFreeing(const IdleWatch& w, uint64_t now) { return w.since && now - w.since < kFreeingMaxMs; }

// ---- Fair sharing: everyone's base layer before anyone's extras ----
// (memory_fair_notes 1; the second 12-bot test: 6 players dressed in full,
// hats and all, while the 6 who came later stood bare.) A component belongs to
// one category, so a hat taken off gives nobody pants; the memory under it
// does: ClothingGiveBack hands the free component back and its buffer becomes
// a spare (merged with its neighbours, SpareAdd), which any category whose
// buffer fits takes (SpareTake).
// The base-layer categories the players here lack.
inline uint64_t BaseLackingAll(const View* v, unsigned n) {
  uint64_t bits = 0;
  for (unsigned i = 0; i < n; ++i) if (v[i].dressed) bits |= v[i].base_lacking;
  return bits;
}
// Whether another player's extras are taken now: someone lacks base-layer items
// none of which has a free component (that one is a retry: PickChange), there
// is no room for one of their buffers (a spare, an arena's tail, a new arena:
// base_room), and nothing taken earlier is still on its way back (freeing:
// co-op components idle, waiting out ClothingGiveBack's 5 s). One take at a
// time, so no more is taken than the base layer needed.
inline bool MayTakeExtras(uint64_t base_lacking, uint64_t free_cats, bool base_room, bool freeing) {
  return base_lacking && !(base_lacking & free_cats) && !base_room && !freeing;
}
// The player whose extras go: wearing co-op components outside its base layer
// (coop_extras: the categories co-op added components to; taking off one of
// the game's own frees no memory), not changed in its last 10 s; off this
// screen before on it, then the farthest (TakeFirst): the nearest last.
// Into its base layer, never less (nobody's base layer goes for anything).
inline Change PickTake(const View* v, unsigned n, uint64_t coop_extras, uint64_t now) {
  Change best;
  float best_d = -1;
  bool best_on = false;
  for (unsigned i = 0; i < n; ++i) {
    const View& x = v[i];
    if (!x.dressed || x.tier != kFull || !(x.extras & coop_extras) || now < x.next_change || !TakeFirst(x, best_on, best_d)) continue;
    best = {int(i), kBase}; best_d = x.distance; best_on = x.on_screen;
  }
  return best;
}

// ---- The body block (own_body, memory_fair_notes 2) ----
// Other players' own body models need 11 stream slots of ~1.46 MB in one
// 16 MB block (group 3), made at the first character with its own body, and
// it can't go back while those bodies are drawn. Past own_body_max players
// (mod.ini; 0 = no limit) it isn't made: everyone shows this game's body
// shape, and the 16 MB stay free for clothing. The count is taken
// kBodySettleMs after the first character asked for its body in a session
// (the players of a session joining together count), then held for it -
// only when the session may go past own_body_max (BodySettles); otherwise it
// is decided at once (review_memfair M5: a 2-player session waited 5 s for
// its first character).
enum BodyBlock : uint8_t { kBodyWait = 0, kBodyMake = 1, kBodyOff = 2 };
constexpr uint64_t kBodySettleMs = 5000;
inline BodyBlock DecideBodyBlock(bool made, bool off, unsigned players, unsigned max_players, uint64_t asked_ms, bool settle) {
  if (made) return kBodyMake;   // (made: in use, kept)
  if (off) return kBodyOff;
  if (!max_players || max_players >= 12) return kBodyMake;
  if (players > max_players) return kBodyOff;
  return settle && asked_ms < kBodySettleMs ? kBodyWait : kBodyMake;
}
// Whether the decision waits for the players joining together: only on the
// session host (a joiner gets the whole player list at once), and only with
// more than one other player in already and over half of own_body_max (a
// burst of joins that may go past it). A small session decides at once.
inline bool BodySettles(bool host, unsigned players, unsigned max_players) {
  return host && players >= 3 && players * 2 > max_players;
}

// ---- Item-sized buffers (memory_plan_12 section 1) ----
// The loader stages a piece in a buffer of the category's Slot_size
// (request+80, 8248A100) - mesh, then the morph (only an explicit Morph_name),
// then the peg, 4096-aligned, last (825134F8) - and 8248AD88 copies all
// request+80 bytes into the component's buffer. The item's real size S is
// where the peg ends: peg pointer + peg size - staging (8248D5F0's r6 / r7,
// request+92). 0 = not known (a pointer outside the staging buffer, a mesh or
// a morph not before the peg): treat it as Slot_size. The mesh (8248D5F0's r4,
// request+164) must lie in [staging, peg): 8248AD88 parses it in place in the
// component, so an S that left it out would cut the buffer under it
// (review_memory_steps M2).
inline uint32_t ItemBytes(uint32_t staging, uint32_t slot, uint32_t mesh, uint32_t peg, uint32_t peg_size, uint32_t morph) {
  if (!staging || !slot || peg < staging || peg_size > slot) return 0;
  const uint64_t end = uint64_t(peg) + peg_size;
  if (end <= staging || end - staging > slot) return 0;
  if (mesh < staging || mesh >= peg) return 0;   // (the mesh at or after the peg: its size isn't known)
  if (morph && (morph < staging || morph >= peg)) return 0;   // (a morph past the peg: its size isn't known)
  return uint32_t(end - staging);
}
// Phase B's working size for an item of S: S plus a 4 KB margin (at most
// Slot_size) until phase A's S is confirmed against the packfile sizes
// (review_memory_steps M2: an S too small is the one way phase B corrupts
// memory; one too big only saves less). 0: not known (full size).
constexpr uint32_t kItemMarginBytes = 4096;
inline uint32_t ItemNeed(uint32_t s, uint32_t slot) {
  if (!s) return 0;
  const uint64_t n = uint64_t(s) + kItemMarginBytes;
  return n > slot ? slot : uint32_t(n);
}
// (review_memory_steps M1) A copy refused for an item more than this many
// times: the item isn't asked for again (the pick fails cleanly, "No place";
// the piece stays off and the base layer logic copes).
constexpr unsigned kCopyRefusalsMax = 2;
struct CopyRefusals {
  std::vector<std::pair<uint32_t, uint8_t>> items;   // (item, refusals)
  unsigned Count(uint32_t item) const { for (const auto& e : items) if (e.first == item) return e.second; return 0; }
  unsigned Add(uint32_t item) {
    for (auto& e : items) if (e.first == item) { if (e.second < 255) ++e.second; return e.second; }
    if (items.size() >= 1024) items.erase(items.begin());   // (bounded: the oldest forgotten)
    items.push_back({item, 1});
    return 1;
  }
  bool Stopped(uint32_t item) const { return item && Count(item) >= kCopyRefusalsMax; }
  void Clear() { items.clear(); }
};
// One component of the ring as the pick sees it.
struct PickComp {
  bool free = false;     // +132 & 0x50 clear: the picker may take it
  bool coop = false;     // one of co-op's (its buffer bytes known)
  bool idle = false;     // +132 & 0xD8 clear and +20 refs 0 (its buffer may be trimmed)
  uint32_t bytes = 0;    // its buffer (the game's own: Slot_size)
};
// What the pick hook does for an item of S bytes (s 0: nothing, full size).
struct PickGuard {
  std::vector<uint8_t> hide;    // per ring index: a free co-op component too small, +132 |= 0x10 for the call
  std::vector<uint8_t> clear;   // per record node: in the category on a co-op component too small, node+12 = 0 for the call
  bool grow = false;        // no free component and no node that holds S: add one of GrowBytes
  int trim = -1;            // the free co-op component the picker takes, its buffer over AlignUp(S): cut to it (the rest to the spares)
  uint32_t grow_bytes = 0, trim_bytes = 0;
  unsigned hidden() const { unsigned k = 0; for (uint8_t h : hide) k += h != 0; return k; }
  unsigned cleared() const { unsigned k = 0; for (uint8_t c : clear) k += c != 0; return k; }
};
// The longest ring or record walked (a guard against a broken link, not a
// limit on the guard: review_memory_steps M1, the whole ring is guarded).
constexpr unsigned kPickWalkMax = 4096;
inline PickGuard PlanPick(const PickComp* ring, unsigned n, const uint32_t* node_bytes, const uint8_t* node_coop, unsigned nodes, uint32_t s, bool trim) {
  PickGuard g;
  if (!s) return g;
  const uint32_t want = AlignUp(s, kBufferAlign);
  int first = -1;
  g.hide.assign(n, 0); g.clear.assign(nodes, 0);
  for (unsigned i = 0; i < n; ++i) {
    if (!ring[i].free) continue;
    if (ring[i].coop && ring[i].bytes < s) { g.hide[i] = 1; continue; }   // (the game's own: Slot_size, never too small)
    if (first < 0) first = int(i);
  }
  bool swap_ok = false;
  for (unsigned k = 0; k < nodes; ++k) {
    if (node_coop[k] && node_bytes[k] < s) g.clear[k] = 1;
    else swap_ok = true;
  }
  if (first < 0) {
    if (!swap_ok) { g.grow = true; g.grow_bytes = want; }
    return g;
  }
  if (trim && ring[first].coop && ring[first].idle && ring[first].bytes >= want + kBufferAlign) { g.trim = first; g.trim_bytes = want; }
  return g;
}
// An item not given a component again (CopyRefusals::Stopped): every free
// component hidden and every node of the record cleared, so the picker fails
// cleanly ("No place").
inline PickGuard StopPick(const PickComp* ring, unsigned n, unsigned nodes) {
  PickGuard g;
  g.hide.assign(n, 0);
  for (unsigned i = 0; i < n; ++i) g.hide[i] = ring[i].free ? 1 : 0;
  g.clear.assign(nodes, 1);
  return g;
}
// The copy (8248AD88) into a co-op component of b bytes, the request's
// Slot_size slot and the item's s: 0 = as is, 1 = copy b bytes (request+80 =
// b), 2 = refused (a null component: the game asks for the item again).
inline int CopyGuard(uint32_t s, uint32_t b, uint32_t slot) {
  if (!b || b >= slot) return 0;
  return s && s <= b ? 1 : 2;
}

// ---- The body block of K slots (own_body_slots, memory_plan_12 section 2) ----
// At the first join group 3 is given K load slots (its slot count +0 and
// quota[0] written, then the group's own reset 8250DC00 rebuilds its free-slot
// queue for K) and a block of K x slot size, not 11: (11 - K) x 1.46 MB stay
// free for clothing. Only while the group is idle: nothing loaded or asked
// for, every slot free. Which body model a load slot holds is the pass's
// choice, so any K of the 11 definitions (StyleTest_PC_MP<pool index + 1>) can
// be in: co-op holds at most K references; the next pool index gets the shared
// body at once; a reference no character used for kBodyRefIdleMs is released
// (8250CA48), and its slot serves the next one.
constexpr unsigned kBodyGroupSlots = 11;   // what the game built group 3 with (8250C3D0)
constexpr uint64_t kBodyRefIdleMs = 5000;
// The words of group 3 that say it is idle (read by coop.cpp).
struct BodyGroupState {
  unsigned count = 0, levels = 0;                      // +0, +4
  uint32_t lists = 0;                                  // +12 | +16 | +20 | +24 (slot lists) | +48 (active request records): 0 = empty
  unsigned free_states = 0;                            // slots < count in state 4 (free, +36)
  unsigned queued_requests = 0, queued_free = 0;       // level 0's queues at *(+56) / *(+60): their +0 counts
  bool locked = false;                                 // +200 & 0x20 (the game's pass)
};
// Empty when the slot count may be cut, else why not.
inline const char* BodyGroupBusy(const BodyGroupState& g) {
  if (g.count != kBodyGroupSlots) return "slot count isn't the game's 11";
  if (g.levels != 1) return "not one level (only level 0's queues are read)";   // (review L6: group 3 has 1, the f2534683 dump)
  if (g.locked) return "group locked (in the game's pass)";
  if (g.lists) return "a slot or request list isn't empty";
  if (g.free_states != g.count) return "a slot isn't free";
  if (g.queued_requests) return "a load is queued";
  if (g.queued_free != g.count) return "the free-slot queue doesn't hold every slot";
  return "";
}
// mod.ini own_body_slots -> the slots to make (0: own body off; at most 11).
inline unsigned BodySlots(int ini) { return unsigned(ini < 0 ? 0 : ini > int(kBodyGroupSlots) ? int(kBodyGroupSlots) : ini); }
inline unsigned Bits(uint32_t m) { unsigned n = 0; for (; m; m &= m - 1) ++n; return n; }
constexpr uint64_t kBodyLoadWaitMs = 8000;   // (UpdateAvatar waits this long for an own body model before the shared one)
// The pool indices (1-11) a character here is drawn with its own body model
// (review_memory_steps H1): the other players' characters (remotes), every
// pool object not on the pool's free list whose body definition (+228) is its
// own StyleTest_PC_MP<index + 1> (pool_own: the key-5 player copy, any later
// drawer), and the copy's index itself (copy_index, -1: none). A free list
// that couldn't be read (pool_free 0) counts every such object as drawn.
inline uint32_t BodiesInUse(uint32_t remotes, uint32_t pool_own, uint32_t pool_free, int copy_index) {
  uint32_t m = remotes | (pool_own & ~pool_free);
  if (copy_index >= 1 && copy_index <= 11) m |= 1u << copy_index;
  return m & 0xFFEu;
}
// The pool indices (1-11) co-op holds a body model reference on.
struct BodyRefs {
  uint32_t held = 0;
  uint64_t unused_since[12] = {};   // since when no character used it (0: in use, or not held)
  uint64_t wait_until[12] = {};     // (review L3) not counted unused before its load wait is over (Take's now + kBodyLoadWaitMs)
  // May index take (or keep) its own body with cap slots?
  bool MayTake(unsigned index, unsigned cap) const { return index >= 1 && index <= 11 && ((held >> index & 1) || Bits(held) < cap); }
  void Take(unsigned index, uint64_t now = 0) { if (index >= 1 && index <= 11) { held |= 1u << index; unused_since[index] = 0; wait_until[index] = now ? now + kBodyLoadWaitMs : 0; } }
  // Forgotten without a release (its reference is gone already: a world reset, review L4).
  void Drop(unsigned index) { if (index >= 1 && index <= 11) { held &= ~(1u << index); unused_since[index] = 0; wait_until[index] = 0; } }
  // in_use: the indices a character here is drawn with. Returns those to release now (and forgets them).
  uint32_t Tick(uint32_t in_use, uint64_t now) {
    uint32_t out = 0;
    for (unsigned i = 1; i <= 11; ++i) {
      if (!(held >> i & 1)) continue;
      if ((in_use >> i & 1) || now < wait_until[i]) { unused_since[i] = 0; continue; }
      if (!unused_since[i]) { unused_since[i] = now + 1; continue; }   // (+1: 0 is "in use")
      if (now + 1 - unused_since[i] >= kBodyRefIdleMs) { out |= 1u << i; Drop(i); }
    }
    return out;
  }
};

}  // namespace coopclothing
