// Stand-alone test of the co-op 3.0 clothing pool's plan (clothing.h): the
// arenas, the tiers, which items go on and which player's outfit changes
// next, with no game. Build (from the repo root, with the toolchain that
// Setup installs):
//   clang++ -std=c++20 -O1 core/WhompaysCoop/tests/clothing_test.cpp -o build/clothing_test.exe
// Exit code 0 = all checks passed.
#include "../clothing.h"
#include <cstdio>
#include <string>

using namespace coopclothing;

namespace {

int failures = 0;
void Check(bool ok, const std::string& what) {
  std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
  if (!ok) ++failures;
}

}  // namespace

int main() {
  // Arenas: 4 KB aligned first fit; a 715 KB pants buffer and a 600 KB
  // overshirt don't share 1 MB, small ones fill the gaps; a give-back undoes
  // only the last take.
  {
    std::vector<Arena> a{{0xA2000000, 0, kArenaBytes}};
    const uint32_t pants = ArenaTake(a, BufferBytes(715 * 1024 + 7));
    const uint32_t shirt = ArenaTake(a, BufferBytes(600 * 1024));
    a.push_back({0xA1F00000, 0, kArenaBytes});
    const uint32_t shirt2 = ArenaTake(a, BufferBytes(600 * 1024));
    const uint32_t ring = ArenaTake(a, BufferBytes(16 * 1024 + 1));
    Check(pants == 0xA2000000 && shirt == 0 && shirt2 == 0xA1F00000 && ring == 0xA2000000 + BufferBytes(715 * 1024 + 7) &&
          (ring & (kBufferAlign - 1)) == 0, "arenas: first fit, 4 KB aligned, a buffer never split across two");
    const uint32_t used = a[0].used;
    const uint32_t extra = ArenaTake(a, 8192);
    ArenaGiveBack(a, extra, 8192);
    const bool undone = a[0].used == used;
    ArenaGiveBack(a, pants, BufferBytes(715 * 1024 + 7));   // (not the arena's last take: kept)
    Check(undone && a[0].used == used, "arenas: a give-back undoes the last take only");
    Check(ArenaBytesFor(4096) == kArenaBytes && ArenaBytesFor(0x123456) == 0x130000, "arenas: 1 MB, or the buffer's size in 64 KB pages when bigger");
    Check(ArenaAllowed(17, kArenaBytes, 0) && !ArenaAllowed(16, kArenaBytes, 0) && !ArenaAllowed(3, kArenaBytes, 0),
          "arenas: the first minute, none once free physical memory would drop below 16 MB");
    Check(ArenaAllowed(13, kArenaBytes, 0, 12) && !ArenaAllowed(13, kArenaBytes, 0, 11) && !ArenaAllowed(12, kArenaBytes, 0, 20) &&
          ArenaAllowed(16, kArenaBytes, 0, 15) && RoomMb(16, 15, 0) == 4 && RoomMb(16, kUnknownMb, 0) == 0 && RoomMb(30, 12, 0) == 1,
          "arenas: then none below 12 MB free, nor once the lowest of the last 5 min would drop below 11 MB (16 MB / lowest 15: 4 more)");
    LowWater low;
    uint64_t t = 1000;
    low.Note(t, 16);
    for (t = 3000; t <= 299000; t += 2000) low.Note(t, t == 41000 ? 13 : 15);
    const bool early = low.Low(290000) == kUnknownMb;
    low.Note(301000, 15);
    const uint32_t seen = low.Low(301000);
    for (t = 303000; t <= 700000; t += 2000) low.Note(t, 16);
    const uint32_t later = low.Low(700000);
    low.Note(720000, 16);   // (20 s without a reading: a load)
    const bool gap = low.Low(720000) == kUnknownMb && low.Low(781000) == 16;
    Check(early && seen == 13 && later == 16 && gap && LowWater{}.Low(500000) == kUnknownMb,
          "low water: not known the first 5 min nor a minute after a gap in readings, the lowest reading of the last 5 min after that");
    // review_clothing12 H3: low 13 (free 13 at a reading with no arenas), free
    // 16 now: two arenas, not four (the readings count the arenas as free).
    unsigned made_since = 0;
    for (uint64_t arenas = 0; arenas < (16u << 20) && ArenaAllowed(16 - uint32_t(arenas >> 20), kArenaBytes, arenas, 13); arenas += kArenaBytes) ++made_since;
    Check(made_since == 2 && RoomMb(16, 13, 0) == 2 && RoomMb(15, 13, kArenaBytes) == 1 && RoomMb(14, 13, 2 * kArenaBytes) == 0 &&
          LowNowMb(15, 3 * kArenaBytes) == 12 && ArenasMb(kArenaBytes + 1) == 2,
          "arenas: the lowest reading counts the arenas made since (a 13 MB low and 16 MB free: two more, not four)");
    std::vector<Arena> made(23, Arena{0, 0, kArenaBytes});
    const bool under_cap = ArenaAllowed(kUnknownMb, kArenaBytes, ArenasBytes(made));
    made.push_back({0, 0, kArenaBytes});
    Check(under_cap && !ArenaAllowed(kUnknownMb, kArenaBytes, ArenasBytes(made)) && !ArenaAllowed(kUnknownMb, 0x1900000, 0),
          "arenas: free memory not known: no more than 24 MB of arenas in all");
  }
  // A category whose ring can't be grown (Max_equipped 0, no or a huge
  // Slot_size): its item is planned as not known here, so it goes on and
  // memory isn't counted short.
  {
    Check(Growable(1, 0x1000) && !Growable(0, 0x1000) && !Growable(4, 0) && !Growable(4, 0x800001) && Growable(4, 0x800000),
          "growable: a Max_equipped and a Slot_size up to 8 MB");
    const uint32_t max_equipped[] = {1, 0}, slot_size[] = {0x2000, 0x2000};
    int cats[] = {9, 44};
    for (unsigned i = 0; i < 2; ++i) if (!Growable(max_equipped[i], slot_size[i])) cats[i] = -1;
    int usable[kCategories] = {};
    usable[9] = 1;
    const Fit f = FitOutfit(cats, 2, kFull, usable);
    Check(f.kept == 2 && f.unknown == 1 && !f.left_short && !f.lacking, "growable: an item in a ring that can't grow goes on, not left off for memory");
  }
  // Tiers: full while memory lasts; when short, full within 60 m (kept to 80 m), base layer beyond.
  Check(PickTier(500, kBase, false) == kFull && PickTier(50, kBase, true) == kFull && PickTier(70, kBase, true) == kBase &&
        PickTier(70, kFull, true) == kFull && PickTier(81, kFull, true) == kBase,
        "tiers: everyone in full while memory lasts; nearest first with 20 m hysteresis when short");
  // One outfit: pants (9), coat (7), shoes (11), a ring (25), a second ring
  // in the same category, an item not known here.
  {
    const int cats[] = {9, 7, 11, 25, 25, -1};
    uint16_t want[kCategories];
    Want(cats, 6, kFull, want);
    const bool full_want = want[9] == 1 && want[7] == 1 && want[25] == 2 && want[0] == 0;
    Want(cats, 6, kBase, want);
    Check(full_want && want[9] == 1 && want[11] == 1 && want[7] == 0 && want[25] == 0, "want: per category, what the tier keeps");
    Check(Usable(5, 3) == 2 && Usable(4, 3) == 1 && Usable(2, 3) == 0, "usable: every free component, none kept back as a spare");
    Check(BaseLayer(0) && BaseLayer(9) && BaseLayer(11) && BaseLayer(5) && !BaseLayer(53) && !BaseLayer(12) && !BaseLayer(24),
          "base layer: hair, shirts, underwear, pants, shoes, skin; not eyes, hats, glasses");
    int usable[kCategories] = {};
    usable[9] = usable[7] = usable[11] = 1; usable[25] = 1;
    Fit f = FitOutfit(cats, 6, kFull, usable);
    Check(f.kept == 5 && f.keep[4] == 0 && f.left_short == 1 && f.lacking == Bit(25) && f.unknown == 1 && f.extras == (Bit(7) | Bit(25)) && usable[25] == 0,
          "fit: never an item without a component; the second ring left off for memory");
    int usable_b[kCategories] = {};
    usable_b[9] = usable_b[7] = usable_b[11] = usable_b[25] = 4;
    Fit b = FitOutfit(cats, 6, kBase, usable_b);
    Check(b.kept == 3 && b.left_far == 3 && b.distant == (Bit(7) | Bit(25)) && !b.extras && b.on == (Bit(9) | Bit(11)), "fit: the base layer only for a far player");
    // Base layer first: one pants component, a pants item listed after a coat is still the one that goes on.
    const int order[] = {7, 9, 9};
    int u2[kCategories] = {};
    u2[7] = 0; u2[9] = 1;
    Fit o = FitOutfit(order, 3, kFull, u2);
    Check(o.keep[0] == 0 && o.keep[1] == 1 && o.keep[2] == 0 && o.lacking == (Bit(7) | Bit(9)), "fit: the base layer takes the components first");
    const uint8_t worn[] = {3, 0x10, 0, 0, 1, 0x20, 0, 0, 2, 0x30, 0, 0, 3};
    const std::vector<uint8_t> on = Filter(worn, sizeof(worn), {1, 0, 1});
    const std::vector<uint8_t> none = Filter(worn, sizeof(worn), {0, 0, 0});
    const std::vector<uint8_t> bad = Filter(worn, sizeof(worn) - 1, {1, 0, 1});
    Check(on == std::vector<uint8_t>{2, 0x10, 0, 0, 1, 0x30, 0, 0, 3} && none == std::vector<uint8_t>{0} && bad.size() == sizeof(worn) - 1,
          "filter: the count byte and the kept items' 4 bytes; a list it can't read goes on unchanged");
  }
  // (3.0, appearance_feedback_research 2) The sender's worn list goes out once
  // it stayed the same 1.5 s; the first one at once.
  {
    WornSettle s;
    const std::vector<uint8_t> a{1, 1, 0, 0, 1}, b{1, 2, 0, 0, 1}, c{1, 3, 0, 0, 1};
    const bool first = s.Feed(a, 1000) && s.settled == a && !s.DueAt();
    Check(first && !s.Feed({}, 1500) && s.settled == a, "worn settle: the first list goes at once; a list not read changes nothing");
    // Hover try-ons every 0.5 s: nothing goes out until they stop.
    bool held = !s.Feed(b, 2000) && s.DueAt() == 3500 && !s.Feed(c, 2500) && s.DueAt() == 4000 && !s.Feed(b, 3000) && !s.Feed(b, 4400);
    const bool settled = s.Feed(b, 4500) && s.settled == b && !s.DueAt();
    Check(held && settled, "worn settle: a list that keeps changing waits; it goes once it held 1.5 s");
    // Back to the sent list before it settled: nothing to send.
    const bool back = !s.Feed(a, 5000) && !s.Feed(b, 5600) && !s.DueAt() && !s.Feed(b, 9000) && s.settled == b;
    Check(back, "worn settle: a try-on undone before it settled sends nothing");
  }
  // (3.0) Dressing by difference: only what changed comes off and goes on.
  {
    // Worn: hair 0x10, shirt 0x20, pants 0x30, a hat 0x40 whose piece was taken over (not on).
    const std::vector<RecordNode> nodes{{0x10, true}, {0x20, true}, {0x30, true}, {0x40, false}};
    // New outfit: hair, another shirt 0x21, pants, the hat.
    const uint32_t next[] = {0x10, 0x21, 0x30, 0x40};
    const Diff d = DressDiff(next, 4, nodes);
    Check(d.ok && d.off == std::vector<uint32_t>{0x20, 0x40} && d.wear == std::vector<uint8_t>{0, 1, 0, 1} && d.worn == 2 && d.kept == 2,
          "dress diff: the old shirt comes off, the new one goes on; hair and pants stay; a piece taken over comes off and goes on again");
    // The plan left the hat off now (memory): it comes off, nothing new goes on.
    const uint32_t base[] = {0x10, 0x20, 0x30};
    const Diff b = DressDiff(base, 3, {{0x10, true}, {0x20, true}, {0x30, true}, {0x40, true}});
    Check(b.ok && b.off == std::vector<uint32_t>{0x40} && !b.worn && b.kept == 3, "dress diff: what the pool left off comes off; the rest stays");
    // An item not known here: dress it all.
    const uint32_t unknown[] = {0x10, 0};
    Check(!DressDiff(unknown, 2, nodes).ok, "dress diff: an item not known here dresses the whole outfit");
    // A second stale node of a piece that is on: nothing taken off (8248D198 would take the first).
    const uint32_t same[] = {0x10, 0x20};
    const Diff s = DressDiff(same, 2, {{0x10, true}, {0x10, false}, {0x20, true}});
    Check(s.ok && s.off.empty() && !s.worn, "dress diff: the same outfit changes nothing, a stale second node included");
    // The same change from nothing on: everything goes on (a first dressing is still dressed in full).
    const Diff e = DressDiff(next, 4, {});
    Check(e.ok && e.off.empty() && e.worn == 4, "dress diff: nothing on yet, all of it goes on");
    // After the wear: the new shirt has a node, the hat doesn't (the game didn't put it on alone).
    Check(NotWorn(next, d.wear, {{0x10, true}, {0x21, false}, {0x30, true}}) == 1 && NotWorn(next, d.wear, {{0x21, false}, {0x40, false}}) == 0,
          "dress diff: an item the game didn't put on alone is seen (that outfit is dressed in full once)");
  }
  // Which outfit changes next.
  {
    View v[3];
    v[0] = {true, 20, kFull, Bit(7), 0, Bit(12), 0};       // near, short of a headwear component
    v[1] = {true, 70, kFull, Bit(12) | Bit(7), 0, 0, 0};   // 70 m, wears a hat and a coat
    v[2] = {true, 120, kFull, Bit(24), 0, 0, 0};           // far, wears glasses
    Change c = PickChange(v, 3, true, 0, false, 1000);
    Check(c.index < 0, "change: nobody within 80 m loses what it wears for a nearer player, nor a far one what nobody lacks");
    v[2] = {true, 120, kFull, Bit(12) | Bit(24), 0, 0, 0};
    c = PickChange(v, 3, true, 0, false, 1000);
    Check(c.index == 2 && c.tier == kBase, "change: a far player beyond 80 m wearing what a nearer one lacks into the base layer");
    v[2].next_change = 5000;
    c = PickChange(v, 3, true, 0, false, 1000);
    Check(c.index < 0, "change: at most one per player per 10 s");
    v[2] = {true, 120, kBase, 0, Bit(24), 0, 11000};
    v[1] = {true, 70, kBase, 0, Bit(12) | Bit(7), 0, 11000};
    c = PickChange(v, 3, true, Bit(12) | Bit(7), false, 12000);
    Check(c.index == 0 && c.tier == kFull, "change: the nearest player short of something now free dressed again first");
    v[0] = {true, 20, kFull, Bit(7) | Bit(12), 0, 0, 22000};
    c = PickChange(v, 3, true, 0, false, 12000);
    Check(c.index < 0, "change: nothing free, nobody short: no change (no loop)");
    c = PickChange(v, 3, false, Bit(24), false, 12000);
    Check(c.index == 2 && c.tier == kFull, "change: memory no longer short: a far player back into its full outfit when it has room");
    View n{false, 10, kFull, 0, 0, Bit(9), 0};
    Check(PickChange(&n, 1, true, Bit(9), false, 1000).index < 0, "change: never one being dressed already");
  }
  // A far player in the base layer (hat and glasses left off): back into its
  // full outfit only when all of it fits, and after an upgrade that came up
  // short not again for 60 s, then 120 s ... (no base / full flip-flop).
  {
    View far{true, 120, kBase, 0, Bit(12) | Bit(24), 0, 0};
    const bool part = PickChange(&far, 1, false, Bit(12), false, 20000).index < 0;
    const Change all = PickChange(&far, 1, false, Bit(12) | Bit(24), false, 20000);
    const Change room = PickChange(&far, 1, false, 0, true, 20000);
    const bool short_no_room = PickChange(&far, 1, true, Bit(12), true, 20000).index < 0;
    Check(part && all.index == 0 && all.tier == kFull && room.index == 0 && room.tier == kFull && short_no_room,
          "upgrade: a far player only when all it left off is free (or memory isn't short and an arena may be made)");
    View near{true, 40, kBase, 0, Bit(12) | Bit(24), 0, 0};
    Check(PickChange(&near, 1, true, Bit(12), false, 20000).tier == kFull, "upgrade: a near player as soon as one of its items is free");
    uint64_t hold = NextUpgradeHold(0, true);
    const uint64_t first = hold;
    hold = NextUpgradeHold(hold, true);
    const uint64_t second = hold;
    for (int i = 0; i < 10; ++i) hold = NextUpgradeHold(hold, true);
    Check(first == kUpgradeHoldMs && second == 2 * kUpgradeHoldMs && hold == kUpgradeHoldMaxMs && NextUpgradeHold(hold, false) == 0,
          "upgrade hold: 60 s after one that came up short, doubled up to 8 min, none after one that fitted");
    far.upgrade_after = 20000 + first;
    const bool held = PickChange(&far, 1, false, Bit(12) | Bit(24), true, 20000).index < 0;
    Check(held && PickChange(&far, 1, false, Bit(12) | Bit(24), true, 20000 + first).index == 0, "upgrade hold: no way back before it");
    View retry{true, 120, kBase, 0, Bit(24), Bit(9), 0};
    retry.upgrade_after = 999999;
    const Change r = PickChange(&retry, 1, false, Bit(9) | Bit(24), true, 20000);
    Check(r.index == 0 && r.tier == kBase, "upgrade hold: a retry for what it lacked keeps it in the base layer");
  }

  // 12-player fixes (research tools/coop_test/TEST-3.0-bots12-findings.md).
  // Base layer first for everyone: X (pants, shoes, a hat) is dressed while Y
  // (here, undressed) still lacks pants and shoes; memory for 4 components.
  {
    auto grow = [](unsigned budget, bool rest_room, unsigned& added, Grow* g) {
      added = 0;
      return GrowRings(g, [&](int) { if (added == budget) return false; ++added; return true; },
                       [&](const Grow*, int) { return rest_room; });
    };
    auto rings = [](Grow* g) {
      for (int c = 0; c < kCategories; ++c) g[c] = Grow{};
      g[9] = {1, 1, 1, 0, 1};    // pants: the host's in use; X's base; Y lacks one
      g[11] = {1, 1, 1, 0, 1};   // shoes: the same
      g[12] = {0, 0, 0, 1, 0};   // hat: X's rest
    };
    Grow g[kCategories];
    unsigned added = 0;
    rings(g);
    const int four = grow(4, true, added, g);
    const bool y_kept = g[9].count == 3 && g[11].count == 3 && g[12].count == 0 && added == 4;
    int usable[kCategories] = {};
    for (int c = 0; c < kCategories; ++c) usable[c] = Usable(g[c].count, g[c].others);
    const int cats[] = {12, 9, 11};
    const Fit f = FitOutfit(cats, 3, kFull, usable);
    Check(four == kStageRest && y_kept && f.keep[0] == 0 && f.keep[1] == 1 && f.keep[2] == 1 && f.lacking == Bit(12) && usable[9] == 1 && usable[11] == 1,
          "grow: this outfit's base layer, then the others' still lacking, before its hat (left off; Y's pants and shoes stay free)");
    rings(g);
    const int three = grow(3, true, added, g);
    Check(three == kStageOthersBase && g[12].count == 0, "grow: nothing for the rest while someone's base layer has no memory");
    rings(g);
    const int no_room = grow(10, false, added, g);
    Check(no_room == kStageRest && added == 4 && g[12].count == 0, "grow: the rest only with room left for one more base layer");
    rings(g);
    Check(grow(10, true, added, g) == kStages && g[12].count == 1 && g[9].count == 4 && g[11].count == 4 && added == 7,
          "grow: plenty of memory: everything, and a spare pants / shoes (not a spare hat)");
    Check(RestFits(0, 2 << 20, 0) && RestFits(1 << 20, 2 << 20, 3 << 20) && !RestFits(1 << 20, 2 << 20, (5 << 20) / 2),
          "rest fits: its components and one more base layer in the room left (nothing needed: always)");
  }
  // The hair with the hat wear: dropped by the game when the hat was left off,
  // so the hat is part of that outfit's base layer from then on.
  {
    const uint64_t promoted = Promote(0, Bit(0), Bit(12) | Bit(53));
    Check(promoted == Bit(12) && Promote(0, Bit(9), Bit(12) | Bit(53)) == 0 && Promote(0, Bit(53), Bit(12)) == 0 &&
          Promote(0, 0, Bit(12)) == 0 && Promote(Bit(12), Bit(0), 0) == Bit(12) && Promote(0, Bit(0) | Bit(9), Bit(12)) == Bit(12),
          "promote: only the hat for a dropped hair (nothing for another dropped item, a dropped extra, or without a drop)");
    const int cats[] = {12, 0, 9};
    int usable[kCategories] = {};
    usable[12] = usable[0] = 1;
    const Fit f = FitOutfit(cats, 3, kFull, usable, Bit(12));
    uint16_t want[kCategories];
    Want(cats, 3, kBase, want, Bit(12));
    Check(f.keep[0] == 1 && f.keep[1] == 1 && !f.extras && f.lacking == Bit(9) && want[12] == 1, "promote: the hat goes on with the base layer, a far player keeps it");
  }
  // No dressing again without a memory change: a player left short is dressed
  // again only in a later epoch, never before its 10 s, never two a second.
  {
    Check(!MayRedress(20000, 11000, 0, true, 7, 7) && MayRedress(20000, 11000, 0, true, 7, 8) && MayRedress(20000, 11000, 0, false, 7, 7),
          "redress: one left short only once memory changed");
    Check(!MayRedress(10000, 11000, 0, true, 7, 8) && !MayRedress(20000, 11000, 20500, false, 7, 7) && MayRedress(20500, 11000, 20500, false, 7, 7),
          "redress: not before its next change (10 s), not within 1 s of anyone's");
    View v[3];
    v[0] = {true, 20, kFull, 0, 0, Bit(9), 0};
    v[1] = {true, 120, kBase, 0, Bit(12), 0, 0};
    v[0].changed = v[1].changed = false;
    const bool still = PickChange(v, 2, true, Bit(9) | Bit(12), false, 60000).index < 0 && PickChange(v, 2, false, Bit(9) | Bit(12), true, 60000).index < 0;
    v[0].changed = true;
    const bool retried = PickChange(v, 2, true, Bit(9) | Bit(12), false, 60000).index == 0;
    // (A near player in the base layer isn't held by the epoch: within 60 m it goes back into its full outfit.)
    v[0].changed = false;
    v[2] = {true, 40, kBase, 0, Bit(12), 0, 0};
    v[2].changed = false;
    const Change near = PickChange(v, 3, true, Bit(12), false, 60000);
    Check(still && retried && near.index == 2 && near.tier == kFull,
          "redress: the tick doesn't retry or upgrade a far player before memory changed (a near one in the base layer: at once)");
  }
  // review_clothing12 H2: memory frees but the rings are still full (no free
  // component): a player left short is dressed again when an arena may be
  // made, once; a near one in the base layer too; never without room or a
  // free component.
  {
    View v{true, 30, kFull, 0, 0, Bit(9) | Bit(12), 0};
    const Change room = PickChange(&v, 1, true, 0, true, 60000);
    const bool no_room = PickChange(&v, 1, true, 0, false, 60000).index < 0;
    v.changed = false;   // (its retry's arenas moved the epoch before the dressing was noted: came up short again)
    const bool once = PickChange(&v, 1, true, 0, true, 60000).index < 0;
    View near{true, 40, kBase, 0, Bit(12) | Bit(24), 0, 0};
    near.changed = false;
    const Change up = PickChange(&near, 1, true, 0, true, 60000);
    View far{true, 120, kBase, 0, Bit(24), Bit(9), 0};
    const Change far_retry = PickChange(&far, 1, true, 0, true, 60000);
    Check(room.index == 0 && room.tier == kFull && no_room && once && up.index == 0 && up.tier == kFull && far_retry.index == 0 && far_retry.tier == kBase,
          "retry: with room for an arena and no free component, once per memory change (a near base-layer player back into full; a far one stays in the base layer)");
  }

  // review_clothing12 M5: a promotion marks the player changed (its epoch one
  // behind) and its hat, left off for the distance, as lacking: re-planned
  // once a component is free or an arena may be made, not otherwise, and only
  // once.
  {
    const uint32_t epoch = 9;
    uint32_t dress_epoch = epoch;                    // (ClothingNoteFit set it to the epoch ...)
    uint64_t far_bits = Bit(12) | Bit(24), lacking = 0;
    const uint64_t added = Promote(0, Bit(0), far_bits);   // (the hat, for the dropped hair)
    lacking |= far_bits & added; far_bits &= ~added;
    dress_epoch = epoch - 1;                         // (... and one behind on a promotion)
    View v{true, 120, kBase, 0, far_bits, lacking, 0};
    v.changed = dress_epoch != epoch;
    const Change free_hat = PickChange(&v, 1, true, Bit(12), false, 60000);
    const Change room = PickChange(&v, 1, true, 0, true, 60000);
    const bool nothing = PickChange(&v, 1, true, Bit(24), false, 60000).index < 0;
    v.changed = false;
    Check(added == Bit(12) && lacking == Bit(12) && far_bits == Bit(24) && free_hat.index == 0 && free_hat.tier == kBase && room.index == 0 && nothing &&
              PickChange(&v, 1, true, Bit(12), true, 60000).index < 0,
          "promote: re-planned at once when its hat has a component or an arena may be made (in its base layer), not when nothing fits, then not again");
  }
  // review_clothing12 M6: a spare buffer of any category that fits; the
  // headroom grows with players whose outfit hasn't come; a hat never takes
  // the component another player's (promoted) base-layer hat needs.
  {
    const std::vector<Spare> spares{{0x1000000, BufferBytes(431 * 1024)}, {0x2000000, BufferBytes(747 * 1024)}, {0x3000000, BufferBytes(600 * 1024)}};
    Check(BestSpare(spares, BufferBytes(275 * 1024)) == 0 && BestSpare(spares, BufferBytes(715 * 1024)) == 1 && BestSpare(spares, BufferBytes(500 * 1024)) == 2 &&
              BestSpare(spares, BufferBytes(800 * 1024)) < 0 && BestSpare({}, 4096) < 0,
          "spare: the smallest buffer that fits, from any category (a skin buffer for pants, a headwear buffer for hair)");
    Check(HeadroomBytes(2 << 20, 0) == (2u << 20) && HeadroomBytes(2 << 20, 3) == (8u << 20) && !RestFits(1 << 20, HeadroomBytes(2 << 20, 2), 6 << 20) &&
              RestFits(1 << 20, HeadroomBytes(2 << 20, 0), 6 << 20),
          "headroom: one base layer for the next to join, one more per player here whose outfit hasn't come");
    // A (hat, pants) plans while B (beyond 60 m, no character here) still needs its promoted hat: one hat component, memory for none.
    Grow g[kCategories];
    for (int c = 0; c < kCategories; ++c) g[c] = Grow{};
    g[9] = {2, 1, 1, 0, 0};   // pants: the host's and a free one; A's base
    g[12] = {1, 0, 0, 1, 1};  // hat: one free; A's rest; B's base layer lacks one
    const int stages = GrowRings(g, [](int) { return false; }, [](const Grow*, int) { return true; });
    int usable[kCategories] = {};
    for (int c = 0; c < kCategories; ++c) usable[c] = BaseFor(c, 0) ? Usable(g[c].count, g[c].others) : UsableRest(g[c].count, g[c].others, g[c].reserve);
    const int cats[] = {12, 9};
    const Fit f = FitOutfit(cats, 2, kFull, usable);
    Check(stages == kStageRest && f.keep[0] == 0 && f.keep[1] == 1 && f.lacking == Bit(12) && UsableRest(3, 1, 1) == 1 && UsableRest(1, 1, 1) == 0,
          "reserve: another player's base-layer hat (promoted) is kept for it; this outfit's hat is left off");
  }
  // M1: memory low (below 8 MB free, or the lowest of 5 min less the arenas
  // now below 9 MB) until growth would be allowed again (12 MB free, projected
  // low 11 MB: review_clothing_release R2); then far players lose their extras
  // first, all their items only while still below the floor.
  {
    Check(MemoryLow(false, 7, kUnknownMb, 0) && !MemoryLow(false, 8, kUnknownMb, 0) && MemoryLow(false, 20, 14, 6 * kArenaBytes) &&
              !MemoryLow(false, 20, 14, 4 * kArenaBytes) && MemoryLow(true, 11, kUnknownMb, 0) && !MemoryLow(true, 12, kUnknownMb, 0) &&
              MemoryLow(true, kUnknownMb, kUnknownMb, 0) && !MemoryLow(false, kUnknownMb, kUnknownMb, 0),
          "memory low: below 8 MB free or a projected low below 9 MB, until 12 MB free");
    Check(!MemoryLow(true, 13, 13, 0) && !MemoryLow(true, 12, 15, 4 * kArenaBytes) && MemoryLow(true, 14, 14, 4 * kArenaBytes) &&
              MemoryLow(true, 20, 10, 0) && !MemoryLow(false, 10, 10, 0),
          "memory low: clears at 12-14 MB free once the projected low is 11 MB (the pool's own floor no longer keeps it low)");
    View v[3];
    v[0] = {true, 20, kFull, Bit(12), 0, 0, 0};
    v[1] = {true, 150, kFull, Bit(24), 0, 0, 999999};   // (its 10 s don't hold a shed back)
    v[2] = {true, 90, kBase, 0, Bit(12), 0, 0};
    const Change first = PickShed(v, 3, false);
    v[1].tier = kBase; v[1].extras = 0;
    const Change second = PickShed(v, 3, false);
    v[0].tier = kBase; v[0].extras = 0;
    const bool base_only = PickShed(v, 3, false).index < 0;
    const Change strip = PickShed(v, 3, true);
    v[1].wearing = false;
    const Change next = PickShed(v, 3, true);
    Check(first.index == 1 && first.tier == kBase && second.index == 0 && base_only && strip.index == 1 && strip.tier == kNone && next.index == 2,
          "shed: everyone's extras, farthest first, then (still below the floor) the farthest player's items");
    v[2].wearing = false;
    const bool near_kept = PickShed(v, 3, true).index < 0;   // (v[0] at 20 m: base layer, never stripped)
    v[0].distance = 61;
    const Change far_now = PickShed(v, 3, true);
    v[0].distance = 20;
    Check(near_kept && far_now.index == 0 && far_now.tier == kNone, "shed: nobody within 60 m is stripped of all items (review_clothing_release R3)");
    ShedWatch w;
    const bool no_arenas = MayShed(w, 1000, 0, 0);
    const bool start = MayShed(w, 2000, 3, 0), during = MayShed(w, 31999, 3, 0), stop = MayShed(w, 32000, 3, 0);
    const bool again = MayShed(w, 40000, 3, 1), after = MayShed(w, 69999, 2, 1), stop2 = MayShed(w, 70000, 2, 1);
    const bool emptied = MayShed(w, 71000, 0, 2), fresh = MayShed(w, 72000, 1, 2) && w.since == 72000;
    Check(!no_arenas && start && during && !stop && again && after && !stop2 && !emptied && fresh,
          "shed: not with no arenas, stops after 30 s that freed none, an arena freed starts the 30 s again (review_clothing_release R3)");
    const int cats[] = {9, 12, -1};
    int usable[kCategories] = {};
    usable[9] = usable[12] = 1;
    uint16_t want[kCategories];
    Want(cats, 3, kNone, want);
    const Fit none = FitOutfit(cats, 3, kNone, usable);
    Check(none.kept == 0 && none.left_far == 3 && none.distant == (Bit(9) | Bit(12)) && !none.lacking && want[9] == 0 && usable[9] == 1,
          "shed: nothing goes on (no component used), all counted as left off for the distance");
    v[1].tier = kNone; v[1].next_change = 0;
    const Change back = PickChange(v, 3, true, 0, false, 60000);
    Check(back.index == 1 && back.tier == kBase && PickChange(v, 3, false, 0, false, 60000).tier == kFull,
          "shed: once memory is back, back into its base layer (short) or full outfit, without waiting for a memory change");
    std::vector<Arena> a{{0xA0000000, 3 * 4096, kArenaBytes}, {0xA0100000, 4096, kArenaBytes}};
    const std::vector<uint32_t> live{0xA0001000, 0xA0200000};
    Check(!ArenaUnused(a[0], live) && ArenaUnused(a[1], live) && ArenaUnused(a[0], {}), "release: an arena goes back only when no component holds a buffer cut from it");
  }
  // (review_overnight M4) The pool's own changes: one every 5 s on top of the
  // 1 s gap; items taken from a player off this screen first, then the farthest.
  {
    Check(!PoolChangeDue(1500, 2000, 0) && !PoolChangeDue(3000, 2000, 6000) && PoolChangeDue(6000, 2000, 6000) && kPoolGapMs == 5000 && kPoolGapMs > kDressGapMs,
          "pool gap: a change the pool begins waits for both the 1 s gap and its own 5 s");
    View v[3];
    v[0] = {true, 20, kFull, Bit(7), 0, Bit(12), 0};
    v[1] = {true, 150, kFull, Bit(12), 0, 0, 0};   // the farthest, on this screen
    v[2] = {true, 110, kFull, Bit(12), 0, 0, 0};   // nearer, off it
    v[1].on_screen = true;
    const Change off = PickChange(v, 3, true, 0, false, 1000);
    v[2].on_screen = true;
    const Change both = PickChange(v, 3, true, 0, false, 1000);
    v[1].on_screen = false;
    const Change far_off = PickChange(v, 3, true, 0, false, 1000);
    Check(off.index == 2 && off.tier == kBase && both.index == 1 && far_off.index == 1,
          "change: into the base layer, one off this screen before a farther one on it; both on it: the farthest");
    View s[3];
    s[0] = {true, 30, kFull, Bit(9), 0, 0, 0};
    s[1] = {true, 140, kFull, Bit(9), 0, 0, 0};
    s[2] = {true, 90, kFull, Bit(9), 0, 0, 0};
    s[0].on_screen = s[1].on_screen = true;
    const Change shed = PickShed(s, 3, false);
    s[2].on_screen = true;
    const Change shed_all_on = PickShed(s, 3, false);
    s[1].tier = s[2].tier = kBase; s[1].on_screen = false;
    const Change strip = PickShed(s, 3, true);   // (s[0] the only one with extras: pass 0 takes it, whatever the screen)
    Check(shed.index == 2 && shed_all_on.index == 1 && strip.index == 0 && strip.tier == kBase,
          "shed: off this screen first, then the farthest; the base layer before anyone is stripped");
  }

  // memory_fair_notes 1: spares split and merged (never across arenas), the
  // tail handed back to its arena.
  {
    std::vector<Arena> a{{0xA0000000, 0, kArenaBytes}, {0xA0100000, 0, kArenaBytes}};
    const uint32_t pants = ArenaTake(a, 0x50000), hat = ArenaTake(a, 0x20000), glasses = ArenaTake(a, 0x10000), gloves = ArenaTake(a, 0x30000);
    const uint32_t other = ArenaTake(a, 0x90000);   // (doesn't fit the first: second arena)
    std::vector<Spare> s;
    SpareAdd(s, a, {hat, 0x20000});
    SpareAdd(s, a, {glasses, 0x10000});
    const bool merged = s.size() == 1 && s[0].buffer == hat && s[0].bytes == 0x30000;
    const bool no_fit = BestSpare(s, 0x50000) < 0;
    SpareAdd(s, a, {gloves, 0x30000});   // (ends at the arena's used end: the whole run goes back to its tail)
    const bool tail = s.empty() && a[0].used == hat - a[0].base && other == 0xA0100000;
    SpareAdd(s, a, {other, 0x90000});    // (second arena: its tail too)
    Check(merged && no_fit && tail && a[1].used == 0 && pants == 0xA0000000, "spares: a player's freed extras merge into one run; a run at an arena's end goes back to its tail");
    std::vector<Spare> t{{0xA0000000, 0x60000}, {0xA0200000, 0x10000}};
    const uint32_t got = SpareTake(t, 0x20000), small = SpareTake(t, 0x10000), none = SpareTake(t, 0x50000);
    Check(got == 0xA0000000 && t.size() == 1 && t[0].buffer == 0xA0020000 && t[0].bytes == 0x40000 && small == 0xA0200000 && none == 0,
          "spares: a take keeps the rest of a bigger spare (the smallest that fits first)");
    std::vector<Arena> b{{0xA0000000, 0x80000, 0x100000}, {0xA0100000, 0, 0x100000}};
    std::vector<Spare> u;
    SpareAdd(u, b, {0xA00F0000, 0x10000});   // (the last 64 KB of the first arena, beyond its used end: no tail)
    SpareAdd(u, b, {0xA0100000, 0x10000});   // (the first of the second, right after it in memory)
    Check(u.size() == 2, "spares: never merged across two arenas (they go back to the game one by one)");
    std::vector<Arena> full{{0xA0000000, kArenaBytes, kArenaBytes}};
    Check(!BufferRoom({}, full, 4096) && BufferRoom({{0xA0000000, 0x10000}}, full, 0x10000) && !BufferRoom({{0xA0000000, 0x10000}}, full, 0x20000) &&
          BufferRoom({}, a, 0x10000), "buffer room: a spare that holds it or an arena's free tail");
  }
  // memory_fair_notes 1: fair sharing. 12 players, memory at the floor: the
  // base layer for everyone here before anyone keeps a hat.
  {
    const uint64_t base = Bit(9) | Bit(11), hat = Bit(12), glasses = Bit(17);
    View v[4];
    v[0] = {true, 15, kFull, hat | glasses, 0, 0, 0};      // near, on screen
    v[1] = {true, 70, kFull, hat, 0, 0, 0};                // farther, on screen
    v[2] = {true, 40, kFull, glasses, 0, 0, 0};            // off screen
    v[3] = {true, 30, kFull, 0, 0, base | hat, 0};         // lacks pants, shoes and a hat
    v[0].on_screen = v[1].on_screen = true;
    v[3].base_lacking = base;
    const uint64_t lacking = BaseLackingAll(v, 4);
    Check(lacking == base && !MayTakeExtras(lacking, Bit(9), false, false) && !MayTakeExtras(lacking, 0, true, false) &&
          !MayTakeExtras(lacking, 0, false, true) && MayTakeExtras(lacking, hat, false, false) && !MayTakeExtras(0, 0, false, false),
          "fair: extras taken only when a base-layer item has no free component, no room for its buffer and nothing is on its way back");
    const Change first = PickTake(v, 4, hat | glasses, 1000);
    v[2].tier = kBase; v[2].extras = 0; v[2].next_change = 11000;
    const Change second = PickTake(v, 4, hat | glasses, 6000);
    v[1].tier = kBase; v[1].extras = 0;
    const Change last = PickTake(v, 4, hat | glasses, 11000);
    v[0].tier = kBase; v[0].extras = 0;
    Check(first.index == 2 && first.tier == kBase && second.index == 1 && last.index == 0 && PickTake(v, 4, hat | glasses, 20000).index < 0,
          "fair: off this screen first, then the farthest, the nearest last, into the base layer (never less); then nobody left to take from");
    View w[2];
    w[0] = {true, 90, kFull, hat, 0, 0, 0};
    w[1] = {true, 20, kFull, Bit(30), 0, 0, 0};
    Check(PickTake(w, 2, Bit(30), 1000).index == 1 && PickTake(w, 2, 0, 1000).index < 0,
          "fair: only extras on co-op's components count (taking off one of the game's own frees no memory)");
    w[0].next_change = 5000;
    Check(PickTake(w, 2, hat, 1000).index < 0, "fair: not within a player's 10 s");
    // While someone lacks base-layer items: only their retries, no way back into a full outfit, no rule 1.
    View x[3];
    x[0] = {true, 20, kBase, 0, hat, 0, 0};                // a donor, near, a hat component free
    x[1] = {true, 50, kFull, 0, 0, base, 0};               // lacks its base layer
    x[2] = {true, 150, kFull, hat, 0, 0, 0};               // far, wears a hat the near one lacks for the distance
    x[1].base_lacking = base;
    const Change none = PickChange(x, 3, true, hat, false, 1000, true);
    const Change retry = PickChange(x, 3, true, hat | Bit(9), false, 1000, true);
    const Change room = PickChange(x, 3, true, 0, true, 1000, true);
    x[1].changed = false;
    const Change unchanged = PickChange(x, 3, true, Bit(9), true, 1000, true);
    Check(none.index < 0 && retry.index == 1 && retry.tier == kFull && room.index == 1 && unchanged.index < 0,
          "fair: while a base layer is short only its retry (a free component or room, after a memory change); no hat back, no rule 1");
    x[1].base_lacking = 0; x[1].lacking = 0;
    const Change back = PickChange(x, 3, false, hat, false, 1000, false);
    Check(back.index == 0 && back.tier == kFull, "fair: once every base layer is on, the extras come back (the nearest first)");
    View y[2];
    y[0] = {true, 30, kBase, 0, hat, 0, 0};
    y[1] = {true, 10, kBase, 0, glasses, 0, 0};
    Check(PickChange(y, 2, false, 0, true, 1000).index == 1, "fair: nearest first back into the full outfit when memory allows");
    View z[1];
    z[0] = {true, 200, kNone, 0, base | hat, 0, 0};
    z[0].base_lacking = 0;
    const Change unshed = PickChange(z, 1, false, 0, false, 1000, true);
    Check(unshed.index == 0 && unshed.tier == kBase, "fair: a shed player comes back into its base layer only, while another's is short");
    // The loop guard, step by step: a take, the 5 s give-back, the spare that
    // fits, the retry; no second take while the first is on its way back.
    View p[2];
    p[0] = {true, 100, kFull, hat | glasses, 0, 0, 0};
    p[1] = {true, 20, kFull, 0, 0, Bit(9), 0};
    p[1].base_lacking = Bit(9);
    bool taken = false, again = false;
    for (uint64_t t = 1000; t <= 30000; t += 1000) {
      const bool freeing = taken && t < 12000;       // its components idle, given back at 11 s
      const bool spare = taken && t >= 12000;        // their merged buffer holds the pants
      const uint64_t lack = BaseLackingAll(p, 2);
      const Change r = PickChange(p, 2, true, 0, spare, t, lack != 0);
      if (r.index == 1) { p[1].base_lacking = p[1].lacking = 0; p[1].next_change = t + kSwapMs; continue; }
      if (MayTakeExtras(lack, 0, spare, freeing)) {
        const Change k = PickTake(p, 2, hat | glasses, t);
        if (k.index == 0) { again = again || taken; taken = true; p[0].tier = kBase; p[0].extras = 0; p[0].distant = hat | glasses; p[0].next_change = t + kSwapMs; }
      }
    }
    Check(taken && !again && p[1].base_lacking == 0 && p[0].tier == kBase, "fair: one take, its memory back, the pants on; nothing more taken (no loop)");
  }
  // memory_fair_notes 2: the body block past own_body_max players.
  {
    Check(DecideBodyBlock(false, false, 4, 6, 0, true) == kBodyWait && DecideBodyBlock(false, false, 4, 6, kBodySettleMs, true) == kBodyMake &&
          DecideBodyBlock(false, false, 7, 6, 0, true) == kBodyOff && DecideBodyBlock(false, true, 2, 6, 99999, false) == kBodyOff &&
          DecideBodyBlock(true, false, 12, 6, 0, true) == kBodyMake && DecideBodyBlock(false, false, 12, 0, 0, true) == kBodyMake &&
          DecideBodyBlock(false, false, 12, 12, 0, true) == kBodyMake && DecideBodyBlock(false, false, 2, 6, 0, false) == kBodyMake &&
          DecideBodyBlock(false, false, 7, 6, 0, false) == kBodyOff,
          "body block: waits 5 s for the players joining together (when it settles), not made past own_body_max (held for the session), kept once made, 0 = no limit");
    // review_memfair M5: no settle for a 2-player session, a joiner, or a session far under the cap.
    Check(!BodySettles(true, 2, 6) && !BodySettles(false, 5, 6) && !BodySettles(true, 3, 6) && BodySettles(true, 4, 6) &&
          BodySettles(true, 3, 4) && !BodySettles(true, 5, 11) && BodySettles(true, 6, 11),
          "body block: the settle only on the host, with 3+ players and over half of own_body_max");
  }
  // review_memfair M2 / L3: a component's idle time is continuous; a missed look starts it again.
  {
    IdleWatch w;
    IdleSeen(w, 1000);
    for (uint64_t t = 2000; t < 6000; t += 1000) IdleSeen(w, t);
    const bool early = IdleLongEnough(w, 5000);
    IdleSeen(w, 6000);
    const bool after5 = IdleLongEnough(w, 6000);
    IdleWatch s;   // seen idle, then its category left out for 10 s (worn and taken off meanwhile), then looked at again
    IdleSeen(s, 1000);
    IdleSeen(s, 11000);
    const bool stale = IdleLongEnough(s, 11000);
    for (uint64_t t = 12000; t <= 16000; t += 1000) IdleSeen(s, t);
    const bool fresh = IdleLongEnough(s, 16000);
    IdleWatch b;
    IdleSeen(b, 1000); IdleBusy(b); IdleSeen(b, 2000);
    Check(!early && after5 && !stale && fresh && !IdleLongEnough(b, 2000) && b.since == 2000,
          "give-back: idle 5 s at every look; a look missed for over 1.5 s or a busy one starts it again");
    IdleWatch f;
    IdleSeen(f, 1000);
    Check(IdleFreeing(f, 1000) && IdleFreeing(f, 3000) && !IdleFreeing(f, 1000 + kFreeingMaxMs) && !IdleFreeing(IdleWatch{}, 5000),
          "give-back: freeing for under 15 s only (one that can't go back never holds the takes)");
  }

  // ---- Item-sized buffers (memory_plan_12 section 1) ----
  {
    // Pants: Slot_size 698 K (714752); an item of mesh 300 K + peg 168 K: the peg (4 KB aligned) ends at 468 K.
    const uint32_t st = 0xA0000000u, slot = 714752;
    const bool s_ok = ItemBytes(st, slot, st, st + 307200, 172032, 0) == 479232 && ItemBytes(st, slot, st, st + 307200, 172032, st + 200000) == 479232 &&
                      ItemBytes(st, slot, st, st + 600000, 200000, 0) == 0 && ItemBytes(st, slot, st, st - 4096, 4096, 0) == 0 && ItemBytes(0, slot, 0, 4096, 4096, 0) == 0 &&
                      ItemBytes(st, slot, st, st + 307200, 172032, st + 500000) == 0 && ItemBytes(st, slot, st, st, slot, 0) == 0;
    Check(s_ok, "item size: S = peg end - staging; not known (0) past Slot_size, before the staging, with no staging, or with a morph after the peg");
    // (review M2) The mesh must lie in [staging, peg): at or after the peg, or before the staging, S isn't known.
    const bool mesh_ok = ItemBytes(st, slot, st + 4096, st + 307200, 172032, 0) == 479232 && ItemBytes(st, slot, st + 307200, st + 307200, 172032, 0) == 0 &&
                         ItemBytes(st, slot, st + 500000, st + 307200, 172032, 0) == 0 && ItemBytes(st, slot, st - 16, st + 307200, 172032, 0) == 0 &&
                         ItemBytes(st, slot, st, st + 307200, 172032, st - 16) == 0;
    Check(mesh_ok, "item size: a mesh at or after the peg (or before the staging) and a morph before the staging: not known (full size)");
    Check(ItemNeed(0, slot) == 0 && ItemNeed(479232, slot) == 479232 + 4096 && ItemNeed(slot - 100, slot) == slot && ItemNeed(slot, slot) == slot,
          "item size: phase B works with S + 4 KB (at most Slot_size) until phase A confirms S; not known stays 0");
    auto mask = [](const std::vector<uint8_t>& v) { uint64_t m = 0; for (size_t i = 0; i < v.size() && i < 64; ++i) if (v[i]) m |= uint64_t(1) << i; return m; };
    // A ring: [0] game's own free (Slot), [1] co-op 200 K free, [2] co-op 700 K busy, [3] co-op 600 K free idle.
    PickComp ring[4] = {{true, false, true, slot}, {true, true, true, 204800}, {false, true, false, 716800}, {true, true, true, 614400}};
    const uint32_t nb[2] = {204800, slot}; const uint8_t nc[2] = {1, 0};
    PickGuard g = PlanPick(ring, 4, nb, nc, 2, 479232, true);
    bool ok = mask(g.hide) == 2 && mask(g.clear) == 1 && !g.grow && g.trim == -1 && g.hidden() == 1 && g.cleared() == 1;   // (the game's own comes first: no trim)
    PickComp ring2[3] = {{true, true, true, 204800}, {true, true, true, 716800}, {true, true, false, 716800}};
    g = PlanPick(ring2, 3, nullptr, nullptr, 0, 479232, true);
    ok = ok && mask(g.hide) == 1 && g.trim == 1 && g.trim_bytes == 479232 && !g.grow;
    PickComp ring3[2] = {{true, true, true, 204800}, {false, true, true, 716800}};
    g = PlanPick(ring3, 2, nullptr, nullptr, 0, 479232, true);
    const bool grows = mask(g.hide) == 1 && g.grow && g.grow_bytes == 479232;
    const uint32_t nb3[1] = {716800}; const uint8_t nc3[1] = {1};
    g = PlanPick(ring3, 2, nb3, nc3, 1, 479232, true);   // (the record's own 700 K component takes it: a swap, no new one)
    const bool swaps = !g.grow && !mask(g.clear);
    PickComp busy_idle[1] = {{true, true, false, 716800}};
    g = PlanPick(busy_idle, 1, nullptr, nullptr, 0, 479232, true);
    const bool no_trim_busy = g.trim == -1 && !mask(g.hide);   // (refs or 0xD8: picked, not cut)
    PickComp exact[1] = {{true, true, true, 479232 + 4096 - 1}};
    g = PlanPick(exact, 1, nullptr, nullptr, 0, 479232, true);
    const bool no_trim_small = g.trim == -1;   // (less than 4 KB over: kept)
    g = PlanPick(ring, 4, nb, nc, 2, 0, true);
    const bool unknown = g.hide.empty() && g.clear.empty() && !g.grow && g.trim == -1;
    Check(ok && grows && swaps && no_trim_busy && no_trim_small && unknown,
          "item-sized pick: too-small co-op components hidden and their nodes cleared, the picked free co-op one cut to S, one made of S when nothing fits (not when the record's own holds it), S not known: nothing");
    // (review M1) The whole ring is guarded, past the old 64.
    std::vector<PickComp> longring(100, PickComp{true, true, true, 204800});
    longring[90] = PickComp{true, true, true, 716800};
    g = PlanPick(longring.data(), 100, nullptr, nullptr, 0, 479232, true);
    Check(g.hide.size() == 100 && g.hidden() == 99 && !g.hide[90] && g.hide[99] && g.trim == 90 && !g.grow, "item-sized pick: a ring of 100 is guarded whole (every too-small one hidden, the one that fits picked and cut)");
    // (review M1) Stopped after two refusals: every free component hidden, every node cleared (the picker fails cleanly).
    g = StopPick(ring, 4, 2);
    Check(mask(g.hide) == (1 | 2 | 8) && mask(g.clear) == 3 && !g.grow && g.trim == -1, "item-sized pick: a twice-refused item gets no component (free ones hidden, the record's nodes cleared)");
    CopyRefusals cr;
    bool rf = !cr.Stopped(0x1000) && cr.Add(0x1000) == 1 && !cr.Stopped(0x1000) && cr.Add(0x2000) == 1 && cr.Add(0x1000) == 2 && cr.Stopped(0x1000) && !cr.Stopped(0x2000) && !cr.Stopped(0);
    cr.Clear(); rf = rf && !cr.Stopped(0x1000);
    Check(rf, "item-sized copy: refusals counted per item, the item stopped after 2");
    Check(CopyGuard(479232, 479232, slot) == 1 && CopyGuard(479232, 204800, slot) == 2 && CopyGuard(0, 204800, slot) == 2 && CopyGuard(479232, 716800, slot) == 0 &&
          CopyGuard(479232, 0, slot) == 0, "item-sized copy: B bytes when S fits a smaller co-op buffer, refused (null component) when it doesn't or S isn't known, as is for full-size buffers and the game's own");
  }

  // ---- The body block of K slots (memory_plan_12 section 2) ----
  {
    BodyGroupState idle; idle.count = 11; idle.levels = 1; idle.free_states = 11; idle.queued_free = 11;
    bool ok = !*BodyGroupBusy(idle);
    BodyGroupState g = idle; g.count = 2; ok = ok && *BodyGroupBusy(g);                // (cut already, or not the game's 11)
    g = idle; g.lists = 0x1234; ok = ok && *BodyGroupBusy(g);                           // a slot in a list, or a request record active
    g = idle; g.free_states = 10; ok = ok && *BodyGroupBusy(g);                         // a slot loading / loaded
    g = idle; g.queued_requests = 1; ok = ok && *BodyGroupBusy(g);                      // a load queued
    g = idle; g.queued_free = 10; ok = ok && *BodyGroupBusy(g);                         // a free slot taken out of the queue
    g = idle; g.locked = true; ok = ok && *BodyGroupBusy(g);
    g = idle; g.levels = 0; ok = ok && *BodyGroupBusy(g);
    g = idle; g.levels = 2; ok = ok && *BodyGroupBusy(g);                               // (review L6: only level 0's queues are read)
    Check(ok, "body block: group 3 is cut only when idle (the game's 11 slots, all free and queued, no list, request or load, not locked)");
    Check(BodySlots(-1) == 0 && BodySlots(0) == 0 && BodySlots(2) == 2 && BodySlots(11) == 11 && BodySlots(40) == 11, "body block: own_body_slots clamped to 0-11");
  }
  {
    BodyRefs r;
    // K = 2: the first two indices take a body; a third gets the shared one; a held index keeps it.
    bool take = r.MayTake(3, 2);
    r.Take(3);
    take = take && r.MayTake(5, 2);
    r.Take(5);
    take = take && !r.MayTake(7, 2) && r.MayTake(3, 2) && r.MayTake(5, 2) && !r.MayTake(0, 2) && !r.MayTake(12, 2) && !r.MayTake(4, 0);
    Check(take && Bits(r.held) == 2, "body refs: at most K indices hold a body model (K = 2: the third gets the shared body, a held one keeps its own)");
    // Index 3 unused from t = 1000: released after 5 s, not before; 5 in use all along stays.
    const uint32_t in5 = 1u << 5;
    bool rel = r.Tick(in5, 1000) == 0 && r.Tick(in5, 3000) == 0 && r.Tick(in5, 5999) == 0;
    const uint32_t out = r.Tick(in5, 6000);
    rel = rel && out == (1u << 3) && r.held == in5 && r.MayTake(7, 2);
    // Used again in between: its idle time starts over.
    r.Take(7);
    rel = rel && r.Tick(in5, 7000) == 0 && r.Tick(in5 | 1u << 7, 9000) == 0 && r.Tick(in5, 10000) == 0 && r.Tick(in5, 14999) == 0 && r.Tick(in5, 15000) == (1u << 7);
    Check(rel, "body refs: an index no character uses for 5 s is given back (its slot serves the next player); one in use again starts over");
  }
  {
    // (review H1) The key-5 copy (pool index 4) and any active pool object wearing its own definition count; free ones don't.
    bool ok = BodiesInUse(0, 0, 0, 4) == (1u << 4) && BodiesInUse(1u << 2, 0, 0, -1) == (1u << 2);
    ok = ok && BodiesInUse(0, (1u << 6) | (1u << 7), 1u << 7, -1) == (1u << 6);       // (7 on the free list: not drawn)
    ok = ok && BodiesInUse(0, (1u << 6) | (1u << 7), 0, -1) == ((1u << 6) | (1u << 7));   // (the free list not read: both kept)
    ok = ok && BodiesInUse(1u, 1u, 0, 0) == 0 && BodiesInUse(0, 0, 0, 12) == 0;          // (index 0, the local player's, and out of range: never)
    Check(ok, "body refs: drawn = the other players' characters, the player copy, every active pool object wearing its own body definition");
    // The copy held at index 4: never given back while it stands.
    BodyRefs r;
    r.Take(4);
    bool kept = true;
    for (uint64_t t = 1000; t <= 30000; t += 1000) kept = kept && r.Tick(BodiesInUse(0, 1u << 4, 0, 4), t) == 0;
    Check(kept && r.held == (1u << 4), "body refs: the key-5 player copy's reference is kept while it stands (review H1)");
    // (review L3) Taken at t = 1000: not counted unused during its 8 s load wait, released 5 s after that.
    BodyRefs w;
    w.Take(3, 1000);
    bool wait = w.Tick(0, 2000) == 0 && w.Tick(0, 8999) == 0 && w.Tick(0, 9000) == 0 && w.Tick(0, 13999) == 0 && w.Tick(0, 14000) == (1u << 3);
    Check(wait && !w.held, "body refs: a reference waiting for its load isn't given back during the 8 s wait (no take / release churn)");
    // (review L4) Dropped without a release.
    BodyRefs d; d.Take(2); d.Take(5); d.Drop(2);
    Check(d.held == (1u << 5) && d.Tick(1u << 5, 1000) == 0 && d.MayTake(7, 2), "body refs: a reference a world reset took is forgotten (its slot counts free)");
  }

  std::printf("%s: %d failure(s)\n", failures ? "FAILED" : "ALL PASSED", failures);
  return failures ? 1 : 0;
}
