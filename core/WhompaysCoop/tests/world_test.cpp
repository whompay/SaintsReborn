// Stand-alone test of the co-op 2.2 world stream core (world.h): records,
// quantisers, the owner's registry, the per-member scheduler and the set
// digest, on a simulated clock with no game and no network. Build (from the
// repo root, with the toolchain that Setup installs):
//   clang++ -std=c++20 -O1 core/WhompaysCoop/tests/world_test.cpp -o build/world_test.exe
// Exit code 0 = all checks passed. An argument picks another random seed.
#include "../world.h"
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <random>
#include <set>
#include <sstream>

using namespace coopworld;

namespace {

int failures = 0;
void Check(bool ok, const std::string& what) {
  std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
  if (!ok) ++failures;
}
std::string Fmt(const char* f, double a, double b = 0, double c = 0) { char b2[256]; std::snprintf(b2, sizeof b2, f, a, b, c); return b2; }

// An owner with members and a registry on a 50 ms tick; checks the per-tick
// invariants (budget, creates before updates) as it goes.
struct World {
  std::mt19937 rng;
  Registry reg;
  std::array<Member, kMaxSlots> members{};
  Scheduler sched;
  float ref[3] = {0, 0, 0};
  uint64_t now = 1000;
  int ticks = 0;
  const uint8_t owner = 0;
  // Per member: ids created (tick) and per id the plans (ticks), for rates and ordering.
  std::map<Key, int> created[kMaxSlots];
  std::map<Key, std::vector<int>> planned[kMaxSlots];
  std::vector<std::array<MemberPlan, kMaxSlots>> history;
  bool keep_history = false;
  uint32_t over_budget = 0, update_before_create = 0, max_bytes = 0;

  explicit World(unsigned seed) : rng(seed) {}
  float U(float a, float b) { return std::uniform_real_distribution<float>(a, b)(rng); }
  Entry& AddCar(uint32_t id, const float pos[3], uint8_t kind = kKindTraffic, uint8_t flags = 0) {
    Entry& e = reg.Add(MakeNetId(owner, id, 0), owner, id, kind, 2, now); std::memcpy(e.pos, pos, sizeof e.pos); e.flags = flags; e.model = uint16_t(id % 40); return e;
  }
  Entry& AddPed(uint32_t id, const float pos[3], uint32_t car = 0, uint8_t seat = 0, uint8_t kind = kKindAmbient) {
    Entry& e = reg.Add(MakeNetId(owner, id, 0), owner, id, kind, 1, now); std::memcpy(e.pos, pos, sizeof e.pos); e.car = car; e.seat = seat; e.model = uint16_t(1 + id % 12);
    if (car) e.flags |= kEntryRest;
    return e;
  }
  void Join(int slot, const float eye[3], const float fwd[3], uint32_t budget) {
    Member& m = members[slot]; m.present = m.in_area = true; std::memcpy(m.eye, eye, sizeof m.eye); std::memcpy(m.fwd, fwd, sizeof m.fwd); m.budget = budget;
  }
  std::array<MemberPlan, kMaxSlots> Tick() {
    now += kTickMs; ++ticks;
    auto plans = sched.Plan(now, reg, members, ref);
    for (int s = 0; s < kMaxSlots; ++s) {
      const MemberPlan& p = plans[s];
      if (!members[s].present || !members[s].in_area) continue;
      if (p.bytes > p.budget) ++over_budget;
      max_bytes = std::max(max_bytes, p.bytes);
      for (auto& c : p.creates) { created[s][c.key] = ticks; planned[s][c.key].push_back(ticks); }
      for (auto& u : p.updates) {
        auto it = created[s].find(u.key);
        if (it == created[s].end() || it->second >= ticks) ++update_before_create;
        planned[s][u.key].push_back(ticks);
      }
      for (auto& r : p.removes) created[s].erase(r.key);
    }
    if (keep_history) history.push_back(plans);
    return plans;
  }
  void Run(int n) { for (int i = 0; i < n; ++i) Tick(); }
  // Plans per second of one id for one member within [from, to] ticks.
  double Hz(int s, Key k, int from, int to) const {
    auto it = planned[s].find(k);
    if (it == planned[s].end()) return 0;
    int n = 0; for (int t : it->second) n += t >= from && t <= to;
    return n / ((to - from + 1) * kTickMs / 1000.0);
  }
  std::string Serialise() const {
    std::ostringstream o;
    for (auto& plans : history) for (int s = 0; s < kMaxSlots; ++s) { auto& p = plans[s]; o << s << ':' << p.bytes << ';'; for (auto& c : p.creates) o << 'c' << c.key << ','; for (auto& u : p.updates) o << 'u' << u.key << ','; for (auto& r : p.removes) o << 'r' << r.key << ','; o << '|'; }
    return o.str();
  }
};

// Rotation matrix rows from an axis and an angle (Rodrigues), independent of UnqRot's formula.
void AxisAngle(const float k[3], float th, float rows[9]) {
  const float c = std::cos(th), s = std::sin(th), t = 1 - c;
  rows[0] = c + k[0] * k[0] * t;        rows[1] = k[0] * k[1] * t - k[2] * s; rows[2] = k[0] * k[2] * t + k[1] * s;
  rows[3] = k[1] * k[0] * t + k[2] * s; rows[4] = c + k[1] * k[1] * t;        rows[5] = k[1] * k[2] * t - k[0] * s;
  rows[6] = k[2] * k[0] * t - k[1] * s; rows[7] = k[2] * k[1] * t + k[0] * s; rows[8] = c + k[2] * k[2] * t;
}

// The scheduler's layout of tests 11 / 13: n_members eyes and n objects
// placed by `place`, cars (one in six parked) and people (some seated).
void Populate(World& w, int n_objects, const std::function<void(float*)>& place) {
  std::vector<uint32_t> cars;
  for (int i = 0; i < n_objects; ++i) {
    float pos[3]; place(pos);
    const uint32_t id = 0x10000u + uint32_t(i) * 7;
    if (i % 10 < 3) { w.AddCar(id, pos, i % 6 == 0 ? kKindParked : kKindTraffic, i % 6 == 0 ? kEntryRest : 0); cars.push_back(id); }
    else if (i % 10 == 3 && !cars.empty()) { const uint32_t car = cars[w.rng() % cars.size()]; w.AddPed(id, w.reg.Find(car)->pos, car, uint8_t(i % 4), kKindCrew); }
    else w.AddPed(id, pos);
  }
}

}  // namespace

int main(int argc, char** argv) {
  const unsigned seed = argc > 1 ? unsigned(std::strtoul(argv[1], nullptr, 10)) : 12345u;
  std::mt19937 rng(seed);
  auto U = [&](float a, float b) { return std::uniform_real_distribution<float>(a, b)(rng); };
  std::printf("world_test seed %u: CarRec %zu PedRec %zu SeatedRec %zu FireExt %zu UpdateHeader %zu CreateRec %zu RemoveRec %zu AdmitRec %zu DigestRec %zu OwnerMapRec %zu\n",
              seed, sizeof(CarRec), sizeof(PedRec), sizeof(SeatedRec), sizeof(FireExt), sizeof(UpdateHeader), sizeof(CreateRec), sizeof(RemoveRec), sizeof(AdmitRec), sizeof(DigestRec), sizeof(OwnerMapRec));
  Check(sizeof(CarRec) == 30 && sizeof(FireExt) == 7 && sizeof(PedRec) == 20 && sizeof(SeatedRec) == 12, "Package C sizes: CarRec 30 (the drive byte), FireExt 7, PedRec / SeatedRec unchanged");

  // 1. Quantiser round trips and bounds.
  {
    float worst_pos = 0, worst_rot = 0, worst_orth = 0, worst_vel = 0, worst_ang = 0, worst_yaw = 0, worst_pv = 0;
    int saturated = 0, in_range_false = 0;
    for (int i = 0; i < 20000; ++i) {
      const float ref[3] = {U(-3000, 3000), U(-100, 100), U(-3000, 3000)};
      const float pos[3] = {ref[0] + U(-700, 700), ref[1] + U(-700, 700), ref[2] + U(-700, 700)};
      int16_t q[3]; float back[3];
      const bool in = std::fabs(pos[0] - ref[0]) <= kPosMax - 0.011f && std::fabs(pos[1] - ref[1]) <= kPosMax - 0.011f && std::fabs(pos[2] - ref[2]) <= kPosMax - 0.011f;
      if (QPos(ref, pos, q)) { UnqPos(ref, q, back); for (int k = 0; k < 3; ++k) worst_pos = std::max(worst_pos, std::fabs(back[k] - pos[k])); }
      else { ++saturated; if (in) ++in_range_false; }
      float k[3] = {U(-1, 1), U(-1, 1), U(-1, 1)};
      const float kn = std::sqrt(k[0] * k[0] + k[1] * k[1] + k[2] * k[2]) + 1e-6f; for (float& v : k) v /= kn;
      float rows[9], rows2[9]; int16_t r[3];
      AxisAngle(k, i % 7 == 0 ? kPi - U(0, 0.01f) : U(-kPi, kPi), rows);
      QRot(rows, r); UnqRot(r, rows2);
      for (int j = 0; j < 9; ++j) worst_rot = std::max(worst_rot, std::fabs(rows[j] - rows2[j]));
      for (int a = 0; a < 3; ++a) for (int b = 0; b < 3; ++b) { float d = 0; for (int j = 0; j < 3; ++j) d += rows2[3 * a + j] * rows2[3 * b + j]; worst_orth = std::max(worst_orth, std::fabs(d - (a == b ? 1 : 0))); }
      const float v = U(-250, 250); worst_vel = std::max(worst_vel, std::fabs(UnqVel(QVel(v)) - v));
      const float w = U(-7.9f, 7.9f); worst_ang = std::max(worst_ang, std::fabs(UnqAng(QAng(w)) - w));
      const float y = U(0, 2 * kPi); float dy = std::fabs(UnqYaw(QYaw(y)) - y); dy = std::min(dy, 2 * kPi - dy); worst_yaw = std::max(worst_yaw, dy);
      const float pv = U(-7.9f, 7.9f); worst_pv = std::max(worst_pv, std::fabs(UnqPedVel(QPedVel(pv)) - pv));
    }
    Check(worst_pos <= kPosUnit / 2 + 1e-3f && in_range_false == 0 && saturated > 0, Fmt("positions: round trip within 1 cm (worst %.4f m), offsets past 655 m saturate (%g of 20000)", worst_pos, saturated));
    Check(worst_rot < 1e-3f && worst_orth < 2e-3f, Fmt("rotations: smallest-three round trip within 1e-3 per element (worst %.2e), orthonormal (%.2e)", worst_rot, worst_orth));
    Check(worst_vel <= kVelUnit / 2 + 1e-4f && worst_ang <= kAngUnit / 2 + 1e-4f, Fmt("velocities within 1/256 m/s (%.5f), angular within 1/32 rad/s (%.4f)", worst_vel, worst_ang));
    Check(worst_yaw <= kPi / 256 + 1e-4f && worst_pv <= kPedVelUnit / 2 + 1e-4f, Fmt("yaw within 0.7 degrees (%.3f deg), people velocity within 1/32 m/s (%.4f)", worst_yaw * 180 / kPi, worst_pv));
    Check(QVel(1000) == 32767 && QVel(-1000) == -32767 && QAng(100) == 127 && QPedVel(-100) == -127 && QHp(50, 100) == 128 && QHp(-5, 100) == 0 && QHp(200, 100) == 255, "velocities and hp clamp");
    const float fwd[3] = {1, 0, 0};
    Check(std::fabs(YawOf(fwd) - kPi / 2) < 1e-5f, "YawOf: +x forward is a quarter turn (atan2(x, z), y up)");
  }

  // 2. Message round trips: create, admit, update packing, digest parts.
  {
    CreateMsg c; c.head.id = 0x12345678; c.head.time = 777; c.head.ref[0] = 1500.5f; c.head.ref[2] = -320.25f; c.head.kind = kKindTraffic; c.head.authority = 0; c.head.model = 42; c.head.team = 3;
    c.paint.assign(300, 0x5A); c.crew = {1, 2, 3};
    CarRec car{}; car.id = c.head.id; car.model = 42; car.hp = 200; car.damage = 0x21;
    c.first.assign(reinterpret_cast<uint8_t*>(&car), reinterpret_cast<uint8_t*>(&car) + sizeof car);
    uint8_t buf[kMaxMessage];
    const size_t n = WriteCreate(c, buf, sizeof buf);
    CreateMsg back;
    const bool car_ok = n == sizeof(CreateRec) + 300 + 12 + sizeof(CarRec) && ReadCreate(buf, n, back) && back.head.id == c.head.id && back.head.ref[0] == 1500.5f && back.head.ref[2] == -320.25f && back.paint == c.paint && back.crew == c.crew && back.first == c.first && HasMagic(buf, "WWC1");
    CreateMsg p; p.head.id = 9; p.head.kind = kKindCrew; p.head.car = 0x12345678; p.head.seat = 2; p.name = "PD_cop_a"; p.look.assign(127, 7);
    SeatedRec sr{}; sr.id = 9; sr.car = 0x12345678; sr.seat = 2; sr.flags = kPedSeated;
    p.first.assign(reinterpret_cast<uint8_t*>(&sr), reinterpret_cast<uint8_t*>(&sr) + sizeof sr);
    const size_t n2 = WriteCreate(p, buf, sizeof buf);
    CreateMsg back2;
    const bool ped_ok = n2 && ReadCreate(buf, n2, back2) && back2.name == "PD_cop_a" && back2.look == p.look && back2.head.car == p.head.car && back2.first.size() == sizeof(SeatedRec);
    CreateMsg bad = p; bad.name.assign(47, 'x');
    const bool bounds = WriteCreate(bad, buf, sizeof buf) == 0 && !ReadCreate(buf, n2 - 1, back2);
    Check(car_ok && ped_ok && bounds, "WWC1: car with paint and crew, seated person with name and look, round trip; oversized name refused, a cut message refused");

    // 2.2.1: the paint tail is 8254DAF8's blob; 8254DCC0 walks it by its own counts.
    std::vector<uint8_t> look = {0x0A, 0x55, 0xB0, 0x0B, 1, 0x33, 1, 2, 0, 155};   // a 1-byte variant block, 1 part: slot 2, part 0, 155 colours
    for (int i = 0; i < 155; ++i) { look.push_back(uint8_t(i % 12)); look.push_back(uint8_t(i)); }
    std::vector<uint16_t> look_parts;
    const bool full_ok = look.size() == kMaxPaint && PaintWalk(look.data(), look.size(), &look_parts) == nullptr && look_parts == std::vector<uint16_t>{7};
    CreateMsg pc = c; pc.paint = look;
    const size_t pn = WriteCreate(pc, buf, sizeof buf);
    CreateMsg pback;
    const bool tail_ok = pn == sizeof(CreateRec) + kMaxPaint + 12 + sizeof(CarRec) && ReadCreate(buf, pn, pback) && pback.paint == look && !PaintWalk(pback.paint.data(), pback.paint.size());
    const bool tail_cut = !ReadCreate(buf, sizeof(CreateRec) + 200, pback);   // (cut inside the paint tail)
    CreateMsg over = pc; over.paint.push_back(0);
    const bool too_long = WriteCreate(over, buf, sizeof buf) == 0 && PaintWalk(over.paint.data(), over.paint.size()) != nullptr;
    const std::vector<uint8_t> cut(look.begin(), look.end() - 1);
    std::vector<uint8_t> padded = {0x0A, 0x55, 0xB0, 0x0B, 0, 1, 4, 1, 1, 2, 7, 0};   // slot 4 part 1, 1 colour, then a stray byte
    std::vector<uint8_t> bad_magic = look; bad_magic[3] = 0x0C;
    std::vector<uint8_t> big_variant = {0x0A, 0x55, 0xB0, 0x0B, 65}; big_variant.resize(5 + 65 + 1, 0);
    std::vector<uint8_t> no_parts = {0x0A, 0x55, 0xB0, 0x0B, 0, 0};
    const bool refused = PaintWalk(cut.data(), cut.size()) && PaintWalk(padded.data(), padded.size()) && !PaintWalk(padded.data(), padded.size() - 1) &&
                         PaintWalk(bad_magic.data(), bad_magic.size()) && PaintWalk(big_variant.data(), big_variant.size()) && !PaintWalk(no_parts.data(), no_parts.size()) &&
                         PaintWalk(no_parts.data(), 5) && PaintWalk(look.data(), 9);
    Check(full_ok && tail_ok && tail_cut && too_long && refused, "WWC1 paint tail: a full 320 B look round trip and well-formed; 321 B not written; a cut tail, a cut look, a stray byte, another magic, a variant block over 64 refused");

    AdmitMsg a; a.head.cars_held = 16; a.head.cars_cap = 16; a.head.people_held = 40; a.head.people_cap = 56; a.head.chunks = 0x8000000000000005ull; a.head.dropped_chunk = 2; a.head.near_cars = 7; a.head.near_people = 19;
    a.models = {5, 1, 9}; for (uint32_t i = 0; i < kMaxDeclined; ++i) a.declined.push_back(0x20000 + i);
    const size_t na = WriteAdmit(a, buf, sizeof buf);
    AdmitMsg ab;
    Check(na == sizeof(AdmitRec) + 6 + 4 * kMaxDeclined && ReadAdmit(buf, na, ab) && ab.models == a.models && ab.declined == a.declined && ab.head.chunks == a.head.chunks && ab.head.cars_cap == 16 && ab.head.near_cars == 7 && ab.head.near_people == 19 && !ReadAdmit(buf, na - 1, ab), "WWA1: counts, near counts, 3 models, 32 declined ids round trip; a cut message refused");

    {   // (co-op 3, protocol 10) the owners and NetIds in WWC1 / WWR1 / WWA1
      CreateMsg o = c; o.head.owner = 3; o.head.origin = 5; o.head.nonce = 0xBEEF; o.head.net = 0x0ABC0123; o.head.own_epoch = 4097;
      const size_t no = WriteCreate(o, buf, sizeof buf);
      CreateMsg ob;
      const bool create = no == n && ReadCreate(buf, no, ob) && ob.head.owner == 3 && ob.head.origin == 5 && ob.head.nonce == 0xBEEF && ob.head.net == 0x0ABC0123 && ob.head.own_epoch == 4097 && ob.head.id == c.head.id && ob.first == c.first;
      RemoveRec r{}; SetMagic(r.magic, "WWR1"); r.id = 77; r.fade_ms = 400; r.reason = kRemoveAreaLeft; r.owner = 11; r.own_epoch = 65535;
      uint8_t rb[sizeof(RemoveRec)]; std::memcpy(rb, &r, sizeof r); RemoveRec rr; std::memcpy(&rr, rb, sizeof rr);
      const bool remove = rr.owner == 11 && rr.own_epoch == 65535 && rr.id == 77 && rr.reason == kRemoveAreaLeft && offsetof(RemoveRec, owner) == 15;
      AdmitMsg t = a; t.head.owner = 9;
      const size_t nt = WriteAdmit(t, buf, sizeof buf);
      AdmitMsg tb;
      const bool admit = nt == na && ReadAdmit(buf, nt, tb) && tb.head.owner == 9 && tb.declined == a.declined;
      Check(create && remove && admit, "WWC1 owner, origin, nonce, origin handle and ownership epoch, WWR1 owner and epoch, WWA1 target owner: round trip");
      const NetId id = MakeNetId(11, 0xFFFFFFFEu, 0xA5A5);
      const uint32_t tag = NetTag(id, 0x1234);
      const bool pack = NetOrigin(id) == 11 && NetHandle(id) == 0xFFFFFFFEu && NetNonce(id) == 0xA5A5 && TagOrigin(tag) == 11 && TagNonce(tag) == 0xA5A5 && TagEpoch(tag) == 0x234 &&
                        NetShortTag(id, 0x1234) == uint16_t(tag >> 16) && TagOrigin(TagFromShort(NetShortTag(id, 7))) == 11 && TagEpoch(TagFromShort(NetShortTag(id, 7))) == 7 &&
                        MakeNetId(0, 5, 0) < MakeNetId(0, 6, 0) && MakeNetId(0, 6, 0) < MakeNetId(1, 0, 0) && MakeNetId(0, 5, 0xFFFF) < MakeNetId(0, 6, 0);
      const bool older = TagOlder(NetTag(id, 4), 5) && !TagOlder(NetTag(id, 5), 5) && !TagOlder(NetTag(id, 6), 5) && TagOlder(NetTag(id, 0xFFF), 0x1000) && !TagOlder(NetTag(id, 0), 0xFFF);
      Check(pack && older, "NetId: origin, handle and nonce pack and order by the origin's handle; full and short tags; an older epoch wraps at 12 bits");
      // The event words as coop.cpp packs them: damage bytes | the short tag (kEventPedDamage a[7]), kind | the
      // attacker's origin << 4 | the short tag (kEventPedReact a[1]), 3-bit argument kinds | an origin per argument
      // from bit 12 (kEventPedCall a[6]), and "no person" (origin 15).
      const uint32_t bytes = 0xBEEF | ShortBits(NetTag(id, 0x765)), react = 4u | 9u << 4 | ShortBits(tag);
      const bool words = (bytes & 0xFFFF) == 0xBEEF && TagOrigin(TagOfShortWord(bytes)) == 11 && TagEpoch(TagOfShortWord(bytes)) == 0x765 && TagNonce(TagOfShortWord(bytes)) == 0 &&
                         TagOfShortWord(bytes) == TagFromShort(NetShortTag(id, 0x765)) && (react & 15) == 4 && TagOrigin(OriginTag(uint8_t(react >> 4))) == 9 &&
                         TagOfShortWord(react) == TagFromShort(NetShortTag(id, 0x1234)) && OriginTag(9) == (9u << 28) && TagNonce(OriginTag(9)) == 0 && TagEpoch(OriginTag(9)) == 0;
      uint32_t kinds = 0; const uint8_t origins[4] = {0, 5, 11, 14};
      for (int i = 0; i < 4; ++i) kinds |= uint32_t(i == 1 ? 4 : 2) << (3 * i) | uint32_t(origins[i]) << (12 + 4 * i);
      bool args = kinds < (1u << 28);
      for (int i = 0; i < 4; ++i) args = args && ((kinds >> (3 * i)) & 7) == uint32_t(i == 1 ? 4 : 2) && TagOrigin(OriginTag(uint8_t(kinds >> (12 + 4 * i)))) == origins[i];
      const bool none = OriginTag(15) == kNoPersonTag && TagOrigin(kNoPersonTag) == 15 && ShortBits(kNoPersonTag) == kNoPersonTag && TagOrigin(TagOfShortWord(0x1234 | ShortBits(kNoPersonTag))) == 15;
      Check(words && args && none, "event tag packing: ShortBits / TagOfShortWord round trip a short tag beside 16 bits, OriginTag names an argument's origin alone, kNoPersonTag is origin 15");
      Registry g;
      g.Add(MakeNetId(2, 40, 0x77), 2, 40, kKindTraffic, 2, 1).epoch = 3;
      g.Add(MakeNetId(2, 41, 0x77), 2, 900, kKindAmbient, 0, 1);   // (held here by another handle: an adopted one, phase 3)
      const bool again = g.Add(MakeNetId(2, 40, 0x77), 2, 40, kKindTraffic, 2, 2).activations == 2 && g.Size() == 2;
      const bool finds = g.Find(900) && g.Find(900)->net == MakeNetId(2, 41, 0x77) && !g.Find(41) && g.FindNet(MakeNetId(2, 40, 0x77)) == g.Find(40) && g.FindOrigin(2, 41) == g.Find(900) &&
                         !g.FindOrigin(2, 42) && !g.FindOrigin(3, 40) && !g.FindNet(MakeNetId(2, 40, 0x78));
      g.Find(40)->offer[1].created = true;
      const bool removed = g.Remove(40, 300) && !g.Find(40) && !g.Remove(40) && g.Size() == 1;
      const auto rem = g.TakeRemovals();
      const bool removal = rem.size() == 1 && rem[0].key == MakeKey(2, 40) && rem[0].epoch == 3 && rem[0].fade_ms == 300 && rem[0].created[1];
      Check(again && finds && removed && removal, "registry by NetId: found by this game's handle, by NetId and by origin + handle; a remove names the record (owner, handle) and its epoch");
      // An owner other than slot 0 plans its records by (owner, its handle) and declines come back by its handle.
      g.Add(MakeNetId(2, 50, 0x77), 2, 50, kKindTraffic, 2, 1);
      g.Add(MakeNetId(2, 51, 0x77), 2, 51, kKindCrew, 1, 1).car = 50;
      std::array<Member, kMaxSlots> ms{}; ms[4].present = ms[4].in_area = true; ms[4].budget = 2000;
      Scheduler sc; const float zero[3] = {0, 0, 0};
      auto pl = sc.Plan(100, g, ms, zero);
      std::set<Key> made; for (auto& p : pl[4].creates) made.insert(p.key);
      AdmitMsg dm; dm.head.owner = 2; dm.declined = {900};
      sc.OnAdmit(150, g, ms[4], 4, 2, dm);
      const bool declined = g.Find(900)->offer[4].declined_until > 150 && !g.Find(900)->offer[4].created;
      Check(made == std::set<Key>{MakeKey(2, 50), MakeKey(2, 51), MakeKey(2, 900)} && declined, "an owner in slot 2: creates named (2, its handle), the crew after its car by handle; a decline finds the entry by handle");
    }

    {   // WWT1 (2.4, the sync checker)
      TruthMsg t; t.head.owner = 0; t.head.epoch = 7; t.head.seq = 65535; t.head.time = 123456; t.head.center[0] = -1200.5f; t.head.center[2] = 88; t.head.radius = 60; t.head.flags = kTruthComplete;
      for (uint32_t i = 0; i < kTruthRecs; ++i) { TruthRec r{}; r.id = 0x10000 + i; r.car = i % 3 ? 0 : 0x500; r.model = uint16_t(0x5D0 + i); r.pos[0] = int16_t(-3000 + 100 * int(i)); r.vel[1] = QTruthVel(-9.5f); r.kind = uint8_t(i % 2 ? kKindAmbient : kKindTraffic); r.yaw = uint8_t(i * 10); r.stance = uint8_t(i); r.weapon = uint8_t(kWeaponRaised | 3); r.flags = kPedCrouch; r.host = uint8_t(1 | kTruthChunkOff); r.steady = uint8_t(200 + i); r.seat = uint8_t(i % 4); t.recs.push_back(r); }
      for (uint32_t i = 0; i < kTruthItems; ++i) t.items.push_back({0x900 + i, uint16_t(i), {1, 2, int16_t(-3)}});
      for (uint32_t i = 0; i < kTruthProps; ++i) t.props.push_back({0xABC0 + i, uint8_t(i), 1, {4, 5, 6}});
      const size_t nt = WriteTruth(t, buf, sizeof buf);
      TruthMsg tb;
      const bool full = nt == 822 && ReadTruth(buf, nt, tb) && tb.head.n_recs == kTruthRecs && tb.head.seq == 65535 && tb.head.time == 123456 && tb.head.center[0] == -1200.5f && tb.head.flags == kTruthComplete &&
                        std::memcmp(tb.recs.data(), t.recs.data(), sizeof(TruthRec) * kTruthRecs) == 0 && std::memcmp(tb.items.data(), t.items.data(), sizeof(TruthItem) * kTruthItems) == 0 &&
                        std::memcmp(tb.props.data(), t.props.data(), sizeof(TruthProp) * kTruthProps) == 0 && UnqTruthVel(tb.recs[3].vel[1]) == -9.5f;
      const bool refused = !ReadTruth(buf, nt - 1, tb) && !ReadTruth(buf, 10, tb);
      std::vector<uint8_t> longer(buf, buf + nt); longer.push_back(0);
      const bool overlong = !ReadTruth(longer.data(), longer.size(), tb);
      TruthMsg big = t; big.recs.push_back(t.recs[0]);
      const bool too_many = WriteTruth(big, buf, sizeof buf) == 0 && WriteTruth(t, buf, nt - 1) == 0;
      TruthMsg empty; empty.head.seq = 4;
      const size_t ne = WriteTruth(empty, buf, sizeof buf);
      const bool none = ne == sizeof(TruthHeader) && ReadTruth(buf, ne, tb) && tb.recs.empty() && tb.items.empty() && tb.props.empty() && tb.head.seq == 4;
      buf[0] = 'X';
      Check(full && refused && overlong && too_many && none && !ReadTruth(buf, ne, tb), "WWT1: 24 records + 8 items + 6 props (822 B, one packet) and an empty sample round trip; cut, overlong, too many, another magic refused");
      unsigned per[6]{}; bool classes = StanceClass(0xFF) == 0;
      for (unsigned s = 0; s < 227; ++s) { const uint8_t c = StanceClass(uint8_t(s)); classes &= c >= 1 && c <= 5; ++per[c]; }
      classes &= StanceClass(0) == 1 && StanceClass(4) == 2 && StanceClass(5) == 3 && StanceClass(45) == 4 && StanceClass(97) == 2 && StanceClass(126) == 4 && StanceClass(141) == 1 && StanceClass(150) == 5;
      Check(classes && per[1] == 12 && per[2] == 3 && per[3] == 8 && per[4] == 56, Fmt("StanceClass: every stance 0-226 has a class (still %g, crouch %g, crouch moving %g, ...); 0xFF none", per[1], per[2], per[3]));
    }

    std::vector<CarRec> cars(30); std::vector<PedRec> peds(40); std::vector<SeatedRec> seated(15);
    for (size_t i = 0; i < cars.size(); ++i) { cars[i].id = uint32_t(100 + i); cars[i].hp = uint8_t(i); }
    for (size_t i = 0; i < peds.size(); ++i) { peds[i].id = uint32_t(200 + i); peds[i].flags = uint8_t(i % 2 ? kPedCrouch : 0); peds[i].action = uint16_t(i); }
    for (size_t i = 0; i < seated.size(); ++i) { seated[i].id = uint32_t(300 + i); seated[i].car = 100; seated[i].seat = uint8_t(i % 4); seated[i].flags = kPedSeated | kPedSign; }
    UpdateHeader h{}; h.owner = 0; h.epoch = 3; h.time = 4321; h.ref[0] = 100; h.ref[2] = -50;
    auto msgs = PackUpdates(h, cars, peds, seated);
    std::vector<CarRec> gc; std::vector<PedRec> gp; std::vector<SeatedRec> gs;
    bool sizes = true, parse = true;
    for (auto& m : msgs) { sizes &= m.size() <= size_t(kMaxMessage); UpdateMsg u; parse &= ReadUpdate(m.data(), m.size(), u) && u.head.time == 4321 && u.head.epoch == 3; gc.insert(gc.end(), u.cars.begin(), u.cars.end()); gp.insert(gp.end(), u.peds.begin(), u.peds.end()); gs.insert(gs.end(), u.seated.begin(), u.seated.end()); }
    const size_t total = 30 * 30 + 40 * 20 + 15 * 12;
    const bool same = gc.size() == 30 && gp.size() == 40 && gs.size() == 15 && std::memcmp(gc.data(), cars.data(), 30 * sizeof(CarRec)) == 0 && std::memcmp(gp.data(), peds.data(), 40 * sizeof(PedRec)) == 0 && std::memcmp(gs.data(), seated.data(), 15 * sizeof(SeatedRec)) == 0;
    Check(sizes && parse && same && msgs.size() == 2, Fmt("WWS1: 30 cars + 40 people + 15 seated (%g B) pack into %g messages <= kMaxMessage and read back identical", double(total), double(msgs.size())));
    UpdateMsg u; auto m0 = msgs[0]; m0.pop_back();
    Check(!ReadUpdate(m0.data(), m0.size(), u) && !ReadUpdate(buf, 10, u), "WWS1: a cut or short message is refused");

    // Package C: 30 on foot (10 with a FireExt) + 15 seated (5 with one) + 20 cars with the drive byte.
    {
      std::vector<CarRec> c2(20); std::vector<PedRec> p2(30); std::vector<SeatedRec> s2(15); std::vector<FireExt> pe(30), se(15);
      for (size_t i = 0; i < c2.size(); ++i) { c2[i].id = uint32_t(400 + i); c2[i].damage = uint8_t(i & 1 ? kCarBrake : kCarReverse); c2[i].drive = uint8_t(QSteer(-0.5f + 0.05f * float(i)) | (i % 3 ? kDriveEngine : kDriveHorn)); }
      for (size_t i = 0; i < p2.size(); ++i) {
        p2[i].id = uint32_t(500 + i); p2[i].weapon = uint8_t(1 + i % 50) | (i % 2 ? kWeaponRaised : 0);
        if (i % 3 == 0) { p2[i].weapon |= kWeaponFire; pe[i].fire = uint8_t((i & kFireCount) | kFireAim | (i % 2 ? kFireHeld : 0)); pe[i].aim[0] = int16_t(i * 100); pe[i].aim[1] = -7; pe[i].aim[2] = int16_t(-int(i) * 33); }
      }
      for (size_t i = 0; i < s2.size(); ++i) {
        s2[i].id = uint32_t(600 + i); s2[i].car = 400; s2[i].flags = kPedSeated; s2[i].weapon = uint8_t(3);
        if (i % 3 == 0) { s2[i].weapon |= kWeaponFire; se[i].fire = uint8_t(i | kFireBlank | kFireDriveBy); se[i].aim[2] = 1234; }
      }
      auto mm = PackUpdates(h, c2, p2, s2, pe, se);
      std::vector<CarRec> rc; std::vector<PedRec> rp; std::vector<SeatedRec> rs; std::vector<FireExt> rpe, rse;
      bool sz = true, ok = true; size_t bytes = 0;
      for (auto& m : mm) { sz &= m.size() <= size_t(kMaxMessage); bytes += m.size() - sizeof(UpdateHeader); UpdateMsg r; ok &= ReadUpdate(m.data(), m.size(), r) && r.peds_ext.size() == r.peds.size() && r.seated_ext.size() == r.seated.size();
        rc.insert(rc.end(), r.cars.begin(), r.cars.end()); rp.insert(rp.end(), r.peds.begin(), r.peds.end()); rs.insert(rs.end(), r.seated.begin(), r.seated.end()); rpe.insert(rpe.end(), r.peds_ext.begin(), r.peds_ext.end()); rse.insert(rse.end(), r.seated_ext.begin(), r.seated_ext.end()); }
      bool same2 = rc.size() == 20 && rp.size() == 30 && rs.size() == 15 && std::memcmp(rc.data(), c2.data(), 20 * sizeof(CarRec)) == 0 && std::memcmp(rp.data(), p2.data(), 30 * sizeof(PedRec)) == 0 && std::memcmp(rs.data(), s2.data(), 15 * sizeof(SeatedRec)) == 0;
      for (size_t i = 0; i < 30 && same2; ++i) same2 = (p2[i].weapon & kWeaponFire) ? std::memcmp(&rpe[i], &pe[i], sizeof(FireExt)) == 0 : rpe[i].fire == 0;
      for (size_t i = 0; i < 15 && same2; ++i) same2 = (s2[i].weapon & kWeaponFire) ? std::memcmp(&rse[i], &se[i], sizeof(FireExt)) == 0 : rse[i].fire == 0;
      Check(sz && ok && same2 && bytes == 20 * 30 + 30 * 20 + 10 * 7 + 15 * 12 + 5 * 7,
            Fmt("WWS1 with fire parts: 30 on foot (10 firing) + 15 seated (5 firing) + 20 cars (%g B) in %g message(s) read back byte-identical, fire parts too", double(bytes), double(mm.size())));
      // A message cut inside a FireExt is refused (and one whose last record says a FireExt follows but has none).
      std::vector<PedRec> one(1); one[0].id = 9; one[0].weapon = 5 | kWeaponFire; std::vector<FireExt> oe(1); oe[0].fire = 3;
      auto m1 = PackUpdates(h, {}, one, {}, oe, {})[0];
      UpdateMsg r1; const bool whole = ReadUpdate(m1.data(), m1.size(), r1) && r1.peds_ext.size() == 1 && r1.peds_ext[0].fire == 3;
      bool cut = true; for (int k = 1; k <= 7; ++k) cut &= !ReadUpdate(m1.data(), m1.size() - size_t(k), r1);
      Check(whole && cut && m1.size() == sizeof(UpdateHeader) + 27, "WWS1: a record with its FireExt reads back (27 B); the message cut anywhere inside the FireExt is refused");
      int worst = 0; for (int i = -40; i <= 40; ++i) { const float s = float(i) / 40.0f; worst = std::max(worst, int(std::lround(std::fabs(UnqSteer(QSteer(s)) - s) * 1000))); }
      Check(worst <= 17 && UnqSteer(QSteer(2.0f)) == 1.0f && UnqSteer(QSteer(-2.0f)) == -1.0f, Fmt("steering in 6 bits: within 1/62 (worst %.3f), clamped to -1..1", worst / 1000.0));
    }

    std::vector<uint32_t> ids; for (int i = 0; i < 190; ++i) ids.push_back(uint32_t(1000 + i * 3));
    auto parts = WriteDigest(0, 5555, ids);
    DigestParts dp; bool complete = false; int completes = 0;
    for (size_t i = parts.size(); i-- > 0;) { complete = dp.Add(parts[i].data(), parts[i].size()); completes += complete; }
    Check(parts.size() == 3 && completes == 1 && complete && dp.Ids() == ids && dp.hash == DigestHash(ids) && dp.time == 5555, "WWD1: 190 ids go as 3 parts, complete once all are in (any order), hash matches");
    std::vector<uint32_t> mine = ids; mine.erase(mine.begin() + 5); mine.erase(mine.begin() + 100); mine.push_back(7); mine.push_back(999999); std::sort(mine.begin(), mine.end());
    std::vector<uint32_t> missing, extra;
    DigestDiff(ids, mine, missing, extra);
    Check(missing.size() == 2 && missing[0] == ids[5] && missing[1] == ids[101] && extra.size() == 2 && extra[0] == 7 && extra[1] == 999999, "digest diff: two planted missing and two planted extra ids found");
    DigestDiff(ids, ids, missing, extra);
    Check(missing.empty() && extra.empty() && DigestHash(ids) != DigestHash(mine), "digest diff of equal sets is empty; different sets hash differently");
    auto empty = WriteDigest(0, 1, {});
    Check(empty.size() == 1 && dp.Add(empty[0].data(), empty[0].size()) && dp.Ids().empty(), "an empty digest is one part and complete");
  }

  // 3. Budget formula.
  Check(TickBudget(1000, 1) == 1200 && TickBudget(1000, 11) == 568 && TickBudget(1000, 12) == 520 && TickBudget(100, 11) == 300 && TickBudget(100000, 2) == 1200, "TickBudget: 1000 kbps gives 1200 B/tick at 1 member, 568 at 11; clamped to 6-24 KB/s");

  // 4. Scheduler: 12 members in a 400 m square, 200 objects, 400 ticks, at 1200 B/tick
  //    (one member's clamp) the floors 12 / 5 / 1.5 Hz; at 570 B/tick (11 members)
  //    proportional slowdown, no band starved; the budget never exceeded; no
  //    update before its create; identical runs plan identically.
  auto square = [&](World& w, uint32_t budget, int n_objects) {
    for (int s = 0; s < kMaxSlots; ++s) {
      const float eye[3] = {w.U(0, 400), 1.5f, w.U(0, 400)};
      const float a = w.U(0, 2 * kPi); const float fwd[3] = {std::sin(a), 0, std::cos(a)};
      w.Join(s, eye, fwd, budget);
    }
    Populate(w, n_objects, [&](float* p) { p[0] = w.U(0, 400); p[1] = w.U(0, 3); p[2] = w.U(0, 400); });
  };
  struct Rates { double min[3] = {1e9, 1e9, 1e9}, sum[3] = {}, rest_min = 1e9; int n[3] = {}, n_rest = 0; };
  auto rates = [&](World& w, int from, int to) {
    Rates r;
    for (int s = 0; s < kMaxSlots; ++s) {
      if (!w.members[s].present) continue;
      for (auto& [k, e] : w.reg.Entries()) {
        const Entry& o = e;
        const float* pos = o.Seated() ? w.reg.Find(o.car)->pos : o.pos;
        const int band = BandOf(Dist(pos, w.members[s].eye));
        const double hz = w.Hz(s, MakeKey(o.owner, o.local), from, to);
        if (o.flags & kEntryRest) { r.rest_min = std::min(r.rest_min, hz); ++r.n_rest; continue; }
        r.min[band] = std::min(r.min[band], hz); r.sum[band] += hz; ++r.n[band];
      }
    }
    return r;
  };
  std::string plan_a, plan_b;
  for (int run = 0; run < 2; ++run) {
    World w(seed);
    w.keep_history = true;
    square(w, 1200, 200);
    w.Run(400);
    if (run == 1) { plan_b = w.Serialise(); break; }
    plan_a = w.Serialise();
    int catch_up = 0;
    for (int s = 0; s < kMaxSlots; ++s) for (auto& [k, t] : w.created[s]) catch_up = std::max(catch_up, t);
    bool all_created = true;
    for (int s = 0; s < kMaxSlots; ++s) all_created &= w.created[s].size() == w.reg.Size();
    Check(all_created && catch_up <= 60, Fmt("12 members x 200 objects at 1200 B/tick: every object created on every member within %g ticks (4 a tick)", catch_up));
    const Rates r = rates(w, 100, 400);
    std::printf("      near %.1f Hz (min %.1f, %d), mid %.1f Hz (min %.1f, %d), far %.2f Hz (min %.2f, %d), resting min %.2f Hz (%d); max %u B of 1200\n",
                r.sum[0] / std::max(1, r.n[0]), r.min[0], r.n[0], r.sum[1] / std::max(1, r.n[1]), r.min[1], r.n[1], r.sum[2] / std::max(1, r.n[2]), r.min[2], r.n[2], r.rest_min, r.n_rest, w.max_bytes);
    Check(r.min[0] >= 12 && r.min[1] >= 5 && r.min[2] >= 1.5, "rate floors at 1200 B/tick: every object >= 12 Hz within 50 m, >= 5 Hz within 150 m, >= 1.5 Hz beyond");
    Check(r.rest_min >= 1.5, "resting objects (seated, parked) >= 1.5 Hz");
    Check(w.over_budget == 0, Fmt("the budget is never exceeded (max %g B of 1200 in a tick)", w.max_bytes));
    Check(w.update_before_create == 0, "no update is planned before (or in the same tick as) the object's create");
    uint32_t over = 0; for (int s = 0; s < kMaxSlots; ++s) over += w.members[s].over_create_budget;
    std::printf("      creates held back by the budget: %u\n", over);
    // The member counters the owner's 10 s line prints: moving objects per band, the resting ones apart.
    double lo_moving[3] = {1e9, 1e9, 1e9}, lo_rest = 1e9, hi_rest = 0;
    for (int s = 0; s < kMaxSlots; ++s) {
      const Member& m = w.members[s];
      uint32_t ro = 0, rp = 0;
      for (int b = 0; b < 3; ++b) {
        const uint32_t n = m.band[b].objects - m.band[b].rest_objects;
        if (n) lo_moving[b] = std::min(lo_moving[b], 20.0 * (m.band[b].planned - m.band[b].rest_planned) / n);
        ro += m.band[b].rest_objects; rp += m.band[b].rest_planned;
      }
      if (ro) { lo_rest = std::min(lo_rest, 20.0 * rp / ro); hi_rest = std::max(hi_rest, 20.0 * rp / ro); }
    }
    std::printf("      member counters: moving objects at least %.1f / %.1f / %.2f Hz, resting %.2f .. %.2f Hz\n", lo_moving[0], lo_moving[1], lo_moving[2], lo_rest, hi_rest);
    Check(lo_moving[0] >= 15 && lo_moving[1] >= 7.5 && lo_moving[2] >= 1.5 && lo_rest >= 1.5 && hi_rest <= 2.5,   // (from tick 1: the catch-up included)
          "the members' band counters (the owner line) give the moving objects their band rates and the resting ones 2 Hz apart");
  }
  Check(plan_a == plan_b && plan_a.size() > 10000, Fmt("the plan is deterministic: two runs on equal inputs give identical plans (%g bytes of plan)", double(plan_a.size())));
  {
    World w(seed);
    square(w, 570, 200);
    w.Run(400);
    const Rates r = rates(w, 100, 400);
    const double near = r.sum[0] / std::max(1, r.n[0]), mid = r.sum[1] / std::max(1, r.n[1]), far = r.sum[2] / std::max(1, r.n[2]);
    std::printf("      at 570 B/tick: near %.1f Hz (min %.1f), mid %.1f Hz (min %.1f), far %.2f Hz (min %.2f), resting min %.2f; ratios of target %.2f / %.2f / %.2f; max %u B\n",
                near, r.min[0], mid, r.min[1], far, r.min[2], r.rest_min, near / 20, mid / 10, far / 2, w.max_bytes);
    const double ratios[3] = {near / 20, mid / 10, far / 2};
    const double lo = std::min({ratios[0], ratios[1], ratios[2]}), hi = std::max({ratios[0], ratios[1], ratios[2]});
    Check(w.over_budget == 0, Fmt("570 B/tick: the budget is never exceeded (max %g B)", w.max_bytes));
    Check(lo > 0.25 && hi / lo < 1.8, Fmt("570 B/tick: every band slows in proportion (achieved / target %.2f .. %.2f), none starved", lo, hi));
    Check(r.min[0] >= 6 && r.min[1] >= 3 && r.min[2] >= 0.6 && r.rest_min >= 0.6, "570 B/tick: no object below 6 / 3 / 0.6 Hz in its band");
    Check(w.update_before_create == 0, "570 B/tick: no update before its create");
  }

  // 5. No starvation: 400 people at one distance, room for 20 a tick: each is
  //    planned within every 20 ticks once the creates are through.
  {
    World w(seed);
    const float eye[3] = {0, 1.5f, 0}, fwd[3] = {0, 0, 1};
    w.Join(1, eye, fwd, 20 * 20 + 25);
    for (int i = 0; i < 400; ++i) { const float a = float(i) * 2 * kPi / 400; const float pos[3] = {100 * std::sin(a), 1, 100 * std::cos(a)}; w.AddPed(1000 + i, pos); }
    w.Run(400);   // creates: 2-3 a tick within 425 B
    int worst_gap = 0;
    for (auto& [k, ticks] : w.planned[1]) { int last = -1; for (int t : ticks) { if (t > 200) { if (last > 0) worst_gap = std::max(worst_gap, t - last); last = t; } } if (last < 0) worst_gap = 1000; }
    Check(w.created[1].size() == 400 && worst_gap <= 20, Fmt("400 objects at one distance with room for 20 a tick: each planned within %g ticks (no starvation)", worst_gap));
    Check(w.over_budget == 0, "no starvation run: budget kept");
  }

  // 6. Declines: declined ids get nothing for 3 s, then only when the member reports room, nearest first.
  {
    World w(seed);
    const float eye[3] = {0, 1.5f, 0}, fwd[3] = {0, 0, 1};
    w.Join(2, eye, fwd, 1200);
    for (int i = 0; i < 30; ++i) { const float pos[3] = {float(5 + i * 4), 0, 0}; w.AddCar(500 + i, pos); }
    w.Run(20);
    AdmitMsg a; a.head.cars_held = 20; a.head.cars_cap = 20; a.head.people_cap = 56;
    for (int i = 20; i < 30; ++i) a.declined.push_back(500 + i);   // the 10 farthest
    w.sched.OnAdmit(w.now, w.reg, w.members[2], 2, 0, a);
    int offered_while_declined = 0, created_full = 0;
    const uint64_t declined_at = w.now;
    for (int t = 0; t < 80; ++t) {   // 4 s, still full
      auto plans = w.Tick();
      for (auto& u : plans[2].updates) if (IdOf(u.key) >= 520) ++offered_while_declined;
      for (auto& c : plans[2].creates) if (IdOf(c.key) >= 520) ++created_full;
    }
    Check(offered_while_declined == 0 && created_full == 0, "declined ids get no update and no new create for 4 s while the member is full");
    a.head.cars_held = 18; a.declined.clear();   // room for two
    w.sched.OnAdmit(w.now, w.reg, w.members[2], 2, 0, a);
    std::vector<uint32_t> reoffered;
    for (int t = 0; t < 2; ++t) { auto plans = w.Tick(); for (auto& c : plans[2].creates) reoffered.push_back(IdOf(c.key)); }
    Check(reoffered.size() >= 2 && reoffered[0] == 520 && reoffered[1] == 521 && w.now - declined_at > kDeclineMs, Fmt("with room reported after 3 s the declined ids are re-offered, nearest first (%g in two ticks, first 520 then 521)", double(reoffered.size())));
    Check(w.members[2].reoffers == reoffered.size() && w.members[2].declines == 10, "the member's counters: 10 declines, the re-offers counted");
    // Declined early, room reported early: still nothing before 3 s.
    World w2(seed);
    w2.Join(3, eye, fwd, 1200);
    for (int i = 0; i < 5; ++i) { const float pos[3] = {float(5 + i * 4), 0, 0}; w2.AddCar(600 + i, pos); }
    w2.Run(5);
    AdmitMsg d; d.head.cars_held = 4; d.head.cars_cap = 20; d.declined = {604};
    w2.sched.OnAdmit(w2.now, w2.reg, w2.members[3], 3, 0, d);
    int early = 0, late = 0;
    for (int t = 0; t < 59; ++t) { auto plans = w2.Tick(); for (auto& c : plans[3].creates) if (IdOf(c.key) == 604) ++early; }
    for (int t = 0; t < 3; ++t) { auto plans = w2.Tick(); for (auto& c : plans[3].creates) if (IdOf(c.key) == 604) ++late; }
    Check(early == 0 && late == 1, "a declined id with room is still not re-offered before 3 s, and is right after");
  }

  // 6b. Re-offers (review A3-3): one free slot reported -> one re-offer until
  //     the next WWA1 (the planned creates count as held); a repeat decline
  //     doubles the hold; a person whose model the member can't load (11
  //     models there, not its) isn't re-offered until it can.
  {
    World w(seed);
    const float eye[3] = {0, 1.5f, 0}, fwd[3] = {0, 0, 1};
    w.Join(2, eye, fwd, 1200);
    for (int i = 0; i < 30; ++i) { const float pos[3] = {float(5 + i * 4), 0, 0}; w.AddCar(800 + i, pos); }
    w.Run(20);
    AdmitMsg a; a.head.cars_held = 20; a.head.cars_cap = 20; a.head.people_cap = 56;
    for (int i = 20; i < 30; ++i) a.declined.push_back(800 + i);
    w.sched.OnAdmit(w.now, w.reg, w.members[2], 2, 0, a);
    w.Run(70);   // 3.5 s, full
    a.head.cars_held = 19; a.declined.clear();   // one free slot
    w.sched.OnAdmit(w.now, w.reg, w.members[2], 2, 0, a);
    std::vector<uint32_t> re;
    for (int t = 0; t < 20; ++t) { auto plans = w.Tick(); for (auto& c : plans[2].creates) re.push_back(IdOf(c.key)); }   // 1 s without a WWA1
    Check(re.size() == 1 && re[0] == 820, Fmt("one free slot reported: %g re-offer in the second before the next WWA1 (the nearest, 820)", double(re.size())));
    // The member declines 820 again: held 6 s this time.
    a.head.cars_held = 20; a.declined = {820};
    w.sched.OnAdmit(w.now, w.reg, w.members[2], 2, 0, a);
    a.head.cars_held = 15; a.head.cars_cap = 40; a.declined.clear();   // room for everything from now on
    w.sched.OnAdmit(w.now, w.reg, w.members[2], 2, 0, a);
    int at4 = 0, at7 = 0;
    for (int t = 0; t < 110; ++t) { auto plans = w.Tick(); for (auto& c : plans[2].creates) if (IdOf(c.key) == 820) ++at4; }   // 5.5 s
    for (int t = 0; t < 20; ++t) { auto plans = w.Tick(); for (auto& c : plans[2].creates) if (IdOf(c.key) == 820) ++at7; }    // to 6.5 s
    Check(at4 == 0 && at7 == 1, Fmt("declined a second time: not re-offered in 5.5 s with room (%g), re-offered once the 6 s hold is over (%g)", at4, at7));
    // A person declined while the member's 11 models don't include its model.
    World w3(seed);
    w3.Join(4, eye, fwd, 1200);
    const float ppos[3] = {8, 0, 0};
    Entry& pe = w3.AddPed(0x900, ppos); pe.model = 77;
    w3.Run(5);
    AdmitMsg m; m.head.cars_cap = 26; m.head.people_held = 10; m.head.people_cap = 56;
    for (uint16_t i = 1; i <= 11; ++i) m.models.push_back(i);
    m.declined = {0x900};
    w3.sched.OnAdmit(w3.now, w3.reg, w3.members[4], 4, 0, m);
    int model_reoffers = 0;
    for (int t = 0; t < 200; ++t) { auto plans = w3.Tick(); model_reoffers += int(plans[4].creates.size()); }   // 10 s
    m.declined.clear(); m.models.push_back(77);
    w3.sched.OnAdmit(w3.now, w3.reg, w3.members[4], 4, 0, m);
    int with_model = 0;
    for (int t = 0; t < 4; ++t) { auto plans = w3.Tick(); with_model += int(plans[4].creates.size()); }
    Check(model_reoffers == 0 && with_model == 1, Fmt("a person declined for its model: %g re-offers in 10 s while 11 others are loaded there, re-offered once its model is (%g)", double(model_reoffers), double(with_model)));
  }

  // 7. Removes: gone -> reason 0 with the fade to members holding it, nothing
  //    to a member that declined it; leaving the area -> reason 3 for everything
  //    held and nothing for declined ids; a car driven by a player gets no
  //    updates (its crew does); a removed id is dropped from the digest.
  {
    World w(seed);
    const float eye[3] = {0, 1.5f, 0}, fwd[3] = {0, 0, 1};
    for (int s = 1; s <= 3; ++s) w.Join(s, eye, fwd, 1200);
    for (int i = 0; i < 10; ++i) { const float pos[3] = {float(5 + i * 4), 0, 0}; w.AddCar(700 + i, pos); }
    const float ppos[3] = {9, 0, 0};
    w.AddPed(800, ppos, 701, 0, kKindCrew);
    w.Run(10);
    AdmitMsg a; a.head.cars_held = 5; a.head.cars_cap = 5; a.declined = {705, 706};
    w.sched.OnAdmit(w.now, w.reg, w.members[2], 2, 0, a);
    w.Tick();
    w.reg.Remove(705, 400);
    w.reg.Remove(701, 1000);
    auto plans = w.Tick();
    auto has = [](const std::vector<PlannedRemove>& v, uint32_t id, uint8_t reason, uint16_t fade) { for (auto& r : v) if (IdOf(r.key) == id && r.reason == reason && r.fade_ms == fade) return true; return false; };
    Check(has(plans[1].removes, 705, kRemoveGone, 400) && has(plans[3].removes, 705, kRemoveGone, 400) && !has(plans[2].removes, 705, kRemoveGone, 400) && plans[1].removes.size() == 2 && plans[2].removes.size() == 1,
          "a removed id goes as reason 0 with its fade to the members holding it, not to the one that declined it");
    Check(has(plans[2].removes, 701, kRemoveGone, 1000), "the member that declined another id still gets the remove of the car it held");
    // The crew of the removed car stays (its own destroy hook removes it) and streams on foot now.
    w.reg.Find(703)->authority = 2;   // and a player drives car 703: no record for it
    int car_updates = 0, other_updates = 0, crew_on_foot = 0, crew_seated = 0;
    for (int t = 0; t < 20; ++t) { auto p = w.Tick(); for (auto& u : p[1].updates) { if (IdOf(u.key) == 703) ++car_updates; else ++other_updates; if (IdOf(u.key) == 800) (u.bytes == sizeof(PedRec) ? crew_on_foot : crew_seated)++; } }
    Check(w.reg.Find(800) && w.reg.Find(800)->offer[1].created && crew_on_foot > 0 && crew_seated == 0, "the crew of a removed car stays in the registry and streams as a person on foot");
    Check(car_updates == 0 && other_updates > 0, "a car with a player's authority gets no WWS1 record; the others do");
    // The owner's own player gets into car 704: the authority stays the owner's slot, the flag says a pose carries it.
    w.reg.Find(704)->flags |= kEntryPlayerCar;
    int own_driven = 0;
    for (int t = 0; t < 20; ++t) { auto p = w.Tick(); for (auto& u : p[1].updates) own_driven += IdOf(u.key) == 704; }
    w.reg.Find(704)->flags &= uint8_t(~kEntryPlayerCar);
    int own_back = 0;
    for (int t = 0; t < 20; ++t) { auto p = w.Tick(); for (auto& u : p[1].updates) own_back += IdOf(u.key) == 704; }
    Check(own_driven == 0 && own_back > 0, "a car the owner's own player drives (kEntryPlayerCar) gets no record, and gets them again once it is back");
    // Member 3 leaves the area: 8 cars (10 - 705 - 701) and the crew member.
    w.members[3].in_area = false;
    plans = w.Tick();
    bool all3 = plans[3].removes.size() == 9;
    for (auto& r : plans[3].removes) all3 &= r.reason == kRemoveAreaLeft;
    Check(all3 && plans[1].removes.empty(), Fmt("a member that leaves the area gets reason 3 removes for everything it held (%g), the others nothing", double(plans[3].removes.size())));
    w.members[3].in_area = true;
    plans = w.Tick();
    Check(plans[3].creates.size() == 4 && plans[3].updates.empty(), "back in the area it is caught up again from the start, 4 creates a tick");
    // Digest after 10 s lists what member 1 was offered and not what was removed.
    bool due = false; std::vector<uint32_t> digest;
    for (int t = 0; t < 210 && !due; ++t) { auto p = w.Tick(); if (p[1].digest_due) { due = true; digest = p[1].digest; } }
    Check(due && std::is_sorted(digest.begin(), digest.end()) && !std::binary_search(digest.begin(), digest.end(), 705u) && std::binary_search(digest.begin(), digest.end(), 703u) && digest.size() == 9,
          Fmt("a digest comes within 10 s, sorted, without the removed id and with the player-driven car (%g ids)", double(digest.size())));
    // Member 2 declined 706: its digest lists it (declined ids are in the offered set) so its diff counts it as declined, not missing.
    due = false;
    for (int t = 0; t < 210 && !due; ++t) { auto p = w.Tick(); if (p[2].digest_due) { due = true; digest = p[2].digest; } }
    Check(due && std::binary_search(digest.begin(), digest.end(), 706u), "the digest to a member includes the ids it declined (the proof line counts them apart)");
    // An entry that stops streaming (its chunk or model unloaded on the owner, review A2-2): for 2 s nothing
    // (no records, no remove, still in the digest) and its records resume; for 6 s one reason 0 remove where it
    // was created, then one create when it streams again.
    auto inactive_for = [&](uint32_t id, int ticks, int& removes, int& records, int& creates, bool& in_digest) {
      removes = records = creates = 0; in_digest = false;
      w.reg.Find(id)->flags |= kEntryInactive;
      for (int t = 0; t < ticks; ++t) { auto p = w.Tick(); for (auto& r : p[1].removes) removes += IdOf(r.key) == id; for (auto& u : p[1].updates) records += IdOf(u.key) == id; if (p[1].digest_due) in_digest = std::binary_search(p[1].digest.begin(), p[1].digest.end(), id); }
      w.reg.Find(id)->flags &= uint8_t(~kEntryInactive);
      int after = 0;
      for (int t = 0; t < 20; ++t) { auto p = w.Tick(); for (auto& c : p[1].creates) creates += IdOf(c.key) == id; for (auto& u : p[1].updates) after += IdOf(u.key) == id; }
      records = records ? -records : after;   // (records while inactive count negative)
    };
    int rm = 0, rec = 0, cr = 0; bool dig = false;
    w.members[1].last_digest = w.now - kDigestMs + 20 * kTickMs;   // (a digest falls inside the 2 s)
    inactive_for(702, 40, rm, rec, cr, dig);
    Check(rm == 0 && cr == 0 && rec > 0 && dig, Fmt("an entry inactive for 2 s: no remove, still in the digest, no new create, its records resume (%g)", rec));
    inactive_for(702, 120, rm, rec, cr, dig);
    Check(rm == 1 && cr == 1 && rec >= 0, "an entry inactive for 6 s: one reason 0 remove where it was created, no records meanwhile, one create when it streams again");
  }

  // 7b. (3.0, MJQ's VK02 report) A person created on foot sits in a car the
  //     member can't have (left out: a host mission car): removed there in that
  //     tick (reason 0) and nothing more while it sits; once the car streams,
  //     the car is created and then its crew. In a car the member holds that is
  //     inactive for a moment the crew stays; after kInactiveMs the car goes,
  //     and its crew with it (or the tick after).
  {
    World w(seed);
    const float eye[3] = {0, 1.5f, 0}, fwd[3] = {0, 0, 1};
    w.Join(1, eye, fwd, 1200);
    const float cpos[3] = {6, 0, 4}, ppos[3] = {4, 0, 4};
    w.AddCar(900, cpos, kKindScriptCar, kEntryExcluded);
    w.AddPed(901, ppos, 0, 0, kKindMissionPerson);
    w.Run(10);
    const bool on_foot = w.reg.Find(901)->offer[1].created && !w.reg.Find(900)->offer[1].created;
    Entry& aisha = *w.reg.Find(901);
    aisha.car = 900; aisha.seat = 1; aisha.flags |= kEntryRest;
    auto count = [](const MemberPlan& p, uint32_t id, int& creates, int& updates, int& removes) {
      for (auto& c : p.creates) creates += IdOf(c.key) == id;
      for (auto& u : p.updates) updates += IdOf(u.key) == id;
      for (auto& r : p.removes) removes += IdOf(r.key) == id && r.reason == kRemoveGone;
    };
    int c0 = 0, u0 = 0, r0 = 0;
    count(w.Tick()[1], 901, c0, u0, r0);
    int c1 = 0, u1 = 0, r1 = 0;
    for (int t = 0; t < 40; ++t) count(w.Tick()[1], 901, c1, u1, r1);
    Check(on_foot && r0 == 1 && c0 == 0 && c1 + u1 + r1 == 0 && !aisha.offer[1].created && w.members[1].removes_crew == 1,
          Fmt("a person created on foot that sits in a car left out of the stream is removed there at once (%g), then nothing while it sits (%g)", double(r0), double(c1 + u1 + r1)));
    w.reg.Find(900)->flags &= uint8_t(~kEntryExcluded); w.reg.Find(900)->flags |= kEntryPlayerCar;   // (a player sits in it: it streams now, F1)
    int car_at = -1, crew_at = -1;
    for (int t = 0; t < 20; ++t) { auto p = w.Tick(); for (auto& c : p[1].creates) { if (IdOf(c.key) == 900 && car_at < 0) car_at = t; if (IdOf(c.key) == 901 && crew_at < 0) crew_at = t; } }
    Check(car_at >= 0 && crew_at >= car_at, Fmt("once its car streams, the car is created (tick %g) and its crew with it or after (tick %g)", double(car_at), double(crew_at)));
    w.reg.Find(900)->flags |= kEntryInactive;   // (its chunk or model unloaded on the owner for a moment)
    int c2 = 0, u2 = 0, r2 = 0;
    for (int t = 0; t < 40; ++t) count(w.Tick()[1], 901, c2, u2, r2);   // 2 s
    const bool kept = r2 == 0 && aisha.offer[1].created;
    int car_gone = -1, crew_gone = -1;
    for (int t = 0; t < 80 && crew_gone < 0; ++t) { auto p = w.Tick(); for (auto& r : p[1].removes) { if (IdOf(r.key) == 900 && car_gone < 0) car_gone = t; if (IdOf(r.key) == 901 && crew_gone < 0) crew_gone = t; } }
    Check(kept && car_gone >= 0 && crew_gone >= car_gone && crew_gone <= car_gone + 1,
          Fmt("in a car the member holds that is inactive for 2 s the crew stays; after kInactiveMs the car is removed (tick %g) and its crew with it or the tick after (%g)", double(car_gone), double(crew_gone)));
  }

  // 7d. (review_mjq_fixes M3) A person of this owner sitting in a car this
  //     registry doesn't have (a pose's car copy, another owner's car in shared
  //     mode): not removed for it; it streams on foot as before.
  {
    World w(seed);
    const float eye[3] = {0, 1.5f, 0}, fwd[3] = {0, 0, 1};
    w.Join(1, eye, fwd, 1200);
    const float ppos[3] = {4, 0, 4};
    w.AddPed(911, ppos, 0, 0, kKindMissionPerson);
    w.Run(10);
    Entry& rider = *w.reg.Find(911);
    const bool made = rider.offer[1].created;
    rider.car = 950; rider.seat = 1;   // (950: not in this registry)
    int creates = 0, updates = 0, removes = 0;
    for (int t = 0; t < 40; ++t) {
      auto p = w.Tick();
      for (auto& c : p[1].creates) creates += IdOf(c.key) == 911;
      for (auto& u : p[1].updates) updates += IdOf(u.key) == 911;
      for (auto& r : p[1].removes) removes += IdOf(r.key) == 911;
    }
    Check(made && removes == 0 && creates == 0 && updates > 0 && rider.offer[1].created && w.members[1].removes_crew == 0,
          Fmt("a person in a car this owner doesn't know (a car copy, another owner's) isn't removed: it streams on foot (%g updates in 2 s)", double(updates)));
  }

  // 7c. (3.0, player_mission_cars) The member's own twin of a host mission car:
  //     the nearest of the same model; for a car a player sat in only within
  //     25 m of where the host's car was first seen or is now.
  {
    TwinCand c[4];
    c[0].model = 19; c[0].pos[0] = 300;              // same model, far (another mission car of the model)
    c[1].model = 7;  c[1].pos[0] = 1;                // other model, at the spot
    c[2].model = 19; c[2].pos[0] = 2; c[2].pos[2] = 1;   // the twin at the nav point
    c[3].model = 19; c[3].pos[0] = 40;
    const float spot[3] = {0, 0, 0}, driven[3] = {60, 0, 0}, far_off[3] = {500, 0, 500};
    float d = 0;
    const int at_spot = PickTwin(c, 4, 19, spot, spot, 25.0f, &d);
    const int driven_off = PickTwin(c, 4, 19, driven, spot, 25.0f);   // (the host drove 60 m before the list came: the twin by the first place)
    const int none = PickTwin(c, 4, 19, far_off, far_off, 25.0f);
    const int any = PickTwin(c, 4, 19, far_off, far_off, 0.0f);       // (2.4's lists: no radius, the nearest)
    const int no_model = PickTwin(c, 4, 33, spot, spot, 25.0f);
    Check(at_spot == 2 && d > 2.2f && d < 2.3f && driven_off == 2 && none == -1 && any == 0 && no_model == -1,
          "the twin: nearest of the model; for a player's car within 25 m of the first or current place, nearest to either (else none); 2.4's lists take the nearest anywhere");
  }

  // 8. Crowd: 12 members within 30 m of 55 objects at 570 B/tick: the near band
  //    >= 8 Hz for every object; nothing below 1.5 Hz.
  {
    World w(seed);
    for (int s = 0; s < kMaxSlots; ++s) { const float eye[3] = {w.U(-10, 10), 1.5f, w.U(-10, 10)}; const float a = w.U(0, 2 * kPi); const float fwd[3] = {std::sin(a), 0, std::cos(a)}; w.Join(s, eye, fwd, 570); }
    for (int i = 0; i < 20; ++i) { const float pos[3] = {w.U(-20, 20), 0, w.U(-20, 20)}; w.AddCar(2000 + i, pos); }
    for (int i = 0; i < 35; ++i) { const float pos[3] = {w.U(-20, 20), 0, w.U(-20, 20)}; w.AddPed(3000 + i, pos); }
    w.Run(400);
    const Rates r = rates(w, 100, 400);
    std::printf("      crowd: near %.1f Hz (min %.1f over %d member-objects); max %u B of 570\n", r.sum[0] / std::max(1, r.n[0]), r.min[0], r.n[0], w.max_bytes);
    Check(r.n[0] == 12 * 55 && r.min[0] >= 8 && r.n[1] == 0, "crowd of 12 at 570 B/tick: every member's near band >= 8 Hz for every one of the 55 objects");
    Check(r.min[0] >= 1.5 && w.over_budget == 0, "crowd: nothing below 1.5 Hz, budget kept");
  }

  // 8b. Package C: people firing carry 7 B more per record (Entry.extra): 20
  //     firing people in a member's near band at 570 B/tick are charged 27 B a
  //     record and the budget holds; the crowd test with 15 of its 55 objects
  //     firing keeps the near band >= 7 Hz (the floor 8 becomes 7 for it).
  {
    World w(seed);
    const float eye[3] = {0, 1.5f, 0}, fwd[3] = {0, 0, 1};
    w.Join(1, eye, fwd, 570);
    for (int i = 0; i < 20; ++i) { const float pos[3] = {w.U(-30, 30), 0, w.U(-30, 30)}; w.AddPed(7000 + i, pos).extra = sizeof(FireExt); }
    for (int i = 0; i < 10; ++i) { const float pos[3] = {w.U(-30, 30), 0, w.U(-30, 30)}; w.AddCar(7100 + i, pos); }
    bool charged = true; int firing_updates = 0;
    for (int t = 0; t < 200; ++t) { auto p8 = w.Tick(); for (auto& u : p8[1].updates) if (!IsCar(u.kind)) { charged &= u.bytes == sizeof(PedRec) + sizeof(FireExt) && !u.seated; ++firing_updates; } }
    Check(charged && firing_updates > 0 && w.over_budget == 0, Fmt("20 firing people near a member at 570 B/tick: each record charged 27 B (%g records), the budget never exceeded", double(firing_updates)));
    World c(seed);
    for (int s = 0; s < kMaxSlots; ++s) { const float e2[3] = {c.U(-10, 10), 1.5f, c.U(-10, 10)}; const float a = c.U(0, 2 * kPi); const float f2[3] = {std::sin(a), 0, std::cos(a)}; c.Join(s, e2, f2, 570); }
    for (int i = 0; i < 20; ++i) { const float pos[3] = {c.U(-20, 20), 0, c.U(-20, 20)}; c.AddCar(2000 + i, pos); }
    for (int i = 0; i < 35; ++i) { const float pos[3] = {c.U(-20, 20), 0, c.U(-20, 20)}; Entry& e = c.AddPed(3000 + i, pos); if (i < 15) e.extra = sizeof(FireExt); }
    c.Run(400);
    const Rates r = rates(c, 100, 400);
    std::printf("      crowd with 15 firing: near %.1f Hz (min %.1f over %d member-objects); max %u B of 570\n", r.sum[0] / std::max(1, r.n[0]), r.min[0], r.n[0], c.max_bytes);
    Check(r.n[0] == 12 * 55 && r.min[0] >= 7 && c.over_budget == 0, "crowd of 12 at 570 B/tick with 15 of 55 objects firing: near band >= 7 Hz for every object, budget kept");
  }

  // 9. Long run: 60 s at 20 Hz, 12 members and 300 objects all moving, at
  //    570 B/tick: budget kept every tick; update age (time since the last
  //    plan, sampled each tick) p50 <= 120 ms and p95 <= 250 ms in the near
  //    band; objects come and go (churn) without an update before a create.
  {
    World w(seed);
    square(w, 570, 300);
    std::vector<uint32_t> ids; for (auto& [k, e] : w.reg.Entries()) ids.push_back(e.local);
    std::vector<uint32_t> ages_near, ages_mid;
    uint32_t next_id = 0x90000;
    for (int t = 0; t < 1200; ++t) {
      // Everyone drifts; every second a few objects go and a few come.
      for (auto& [k, e] : w.reg.Entries()) if (!e.Seated()) { e.pos[0] += w.U(-0.5f, 0.5f); e.pos[2] += w.U(-0.5f, 0.5f); }
      for (int s = 0; s < kMaxSlots; ++s) { w.members[s].eye[0] += w.U(-0.3f, 0.3f); w.members[s].eye[2] += w.U(-0.3f, 0.3f); }
      if (t % 20 == 19) {
        for (int i = 0; i < 3 && !ids.empty(); ++i) { const size_t j = w.rng() % ids.size(); if (w.reg.Find(ids[j]) && !w.reg.Find(ids[j])->Seated()) { w.reg.Remove(ids[j]); ids[j] = ids.back(); ids.pop_back(); } }
        for (int i = 0; i < 3; ++i) { const float pos[3] = {w.U(0, 400), 1, w.U(0, 400)}; w.AddPed(next_id, pos); ids.push_back(next_id++); }
      }
      w.Tick();
      if (t >= 200)
        for (int s = 0; s < kMaxSlots; ++s)
          for (auto& [k, e] : w.reg.Entries()) {
            const Offer& o = e.offer[s];
            if (!o.created || (e.flags & kEntryRest) || e.Seated()) continue;
            const int band = BandOf(Dist(e.pos, w.members[s].eye));
            const uint32_t age = uint32_t(w.now - o.last_sent);
            if (band == 0) ages_near.push_back(age); else if (band == 1) ages_mid.push_back(age);
          }
    }
    auto pct = [](std::vector<uint32_t>& v, double p) { if (v.empty()) return 0u; std::sort(v.begin(), v.end()); return v[std::min(v.size() - 1, size_t(p * v.size()))]; };
    const uint32_t n50 = pct(ages_near, 0.5), n95 = pct(ages_near, 0.95), m50 = pct(ages_mid, 0.5), m95 = pct(ages_mid, 0.95);
    std::printf("      60 s: near age p50 %u ms p95 %u ms (%zu samples), mid p50 %u p95 %u; max %u B of 570; registry %zu\n", n50, n95, ages_near.size(), m50, m95, w.max_bytes, w.reg.Size());
    Check(w.over_budget == 0, "60 s run: the budget is never exceeded");
    Check(n50 <= 120 && n95 <= 250, "60 s run: near-band age p50 <= 120 ms, p95 <= 250 ms (the plan-side part of the update age)");
    Check(w.update_before_create == 0, "60 s run with churn: no update before a create");
    uint32_t added, removed, re; w.reg.TakeCounts(added, removed, re);
    Check(added == 300 + 3 * 60 && removed > 100 && re == 0, Fmt("registry counts: %g added, %g removed, %g re-activated", added, removed, re));
    Entry& again = w.reg.Add(MakeNetId(0, ids[0], 0), 0, ids[0], kKindAmbient, 1, w.now);
    w.reg.TakeCounts(added, removed, re);
    Check(again.activations == 2 && re == 1 && added == 0, "adding a known id again is idempotent: one activation more, no new entry");
  }

  // 10. (3.0, world_mode = shared) Per-object interest: three owners (slots
  //     0-2, each with its own registry around its own spot, 200 m apart) and
  //     one viewer (slot 3) walking past all of them. Each owner offers only
  //     what is within 100 m (people) / 150 m (cars) of the viewer's eye and
  //     removes it (kRemoveInterest) past that + 20 m; a crew comes and goes
  //     with its car; what is in range is caught up; the hysteresis holds an
  //     object on the edge.
  {
    struct Owner { Registry reg; Scheduler sched; std::array<Member, kMaxSlots> members{}; float ref[3]{}; std::set<Key> held; };
    std::array<Owner, 3> owners;
    std::mt19937 orng(seed + 10);
    auto OU = [&](float a, float b) { return std::uniform_real_distribution<float>(a, b)(orng); };
    const int viewer = 3;
    uint32_t far_held = 0, crew_alone = 0, wrong_reason = 0, removes = 0, creates = 0, updates_out = 0, owner_mismatch = 0;
    for (int o = 0; o < 3; ++o) {
      Owner& w = owners[o];
      w.ref[0] = 200.0f * o;
      std::vector<uint32_t> cars;
      for (int i = 0; i < 60; ++i) {
        const float pos[3] = {w.ref[0] + OU(-160, 160), 0, OU(-160, 160)};
        const uint32_t id = 0x100u + uint32_t(i);   // (the same handles on every owner: the keys tell them apart)
        const NetId net = MakeNetId(uint8_t(o), id, uint16_t(7 + o));
        if (i % 4 == 0) { Entry& e = w.reg.Add(net, uint8_t(o), id, kKindTraffic, 2, 1000); std::memcpy(e.pos, pos, sizeof pos); cars.push_back(id); }
        else if (i % 4 == 1 && !cars.empty()) { const uint32_t car = cars.back(); Entry& e = w.reg.Add(net, uint8_t(o), id, kKindCrew, 1, 1000); std::memcpy(e.pos, w.reg.Find(car)->pos, sizeof e.pos); e.car = car; e.flags |= kEntryRest; }
        else { Entry& e = w.reg.Add(net, uint8_t(o), id, kKindAmbient, 1, 1000); std::memcpy(e.pos, pos, sizeof pos); e.model = uint16_t(1 + i % 12); }
      }
      Member& m = w.members[viewer];
      m.present = m.in_area = true; m.budget = 570; m.interest_people = kInterestPeople; m.interest_cars = kInterestCars;
    }
    uint64_t now = 1000;
    float eye[3] = {-300, 1.5f, 0};
    const float fwd[3] = {1, 0, 0};
    auto tick = [&]() {
      now += kTickMs;
      for (int o = 0; o < 3; ++o) {
        Owner& w = owners[o];
        Member& m = w.members[viewer];
        std::memcpy(m.eye, eye, sizeof eye); std::memcpy(m.fwd, fwd, sizeof fwd);
        auto plans = w.sched.Plan(now, w.reg, w.members, w.ref);
        const MemberPlan& p = plans[viewer];
        for (auto& c : p.creates) { w.held.insert(c.key); ++creates; if (OwnerOf(c.key) != o) ++owner_mismatch; }
        for (auto& r : p.removes) { w.held.erase(r.key); ++removes; if (r.reason != kRemoveInterest) ++wrong_reason; }
        for (auto& u : p.updates) if (!w.held.count(u.key)) ++updates_out;
        for (Key k : w.held) {
          const Entry* e = w.reg.Find(IdOf(k));
          if (!e) continue;
          const Entry* car = e->car ? w.reg.Find(e->car) : nullptr;
          const float r = IsCar(car ? car->kind : e->kind) ? kInterestCars : kInterestPeople;
          if (Dist(car ? car->pos : e->pos, eye) > r + kInterestHyst + 0.01f) ++far_held;
          if (car && !w.held.count(MakeKey(uint8_t(o), car->local))) ++crew_alone;
        }
      }
    };
    // Walk from x = -300 to 900 at 4 m/s, standing 5 s at each owner's spot.
    uint32_t missing_at_dwell = 0;
    for (int stop = 0; stop < 3; ++stop) {
      while (eye[0] < 200.0f * stop) { eye[0] += 0.2f; tick(); }
      for (int t = 0; t < 100; ++t) tick();
      for (int o = 0; o < 3; ++o) for (auto& [k, e] : owners[o].reg.Entries()) {
        const Entry* car = e.car ? owners[o].reg.Find(e.car) : nullptr;
        const float r = IsCar(car ? car->kind : e.kind) ? kInterestCars : kInterestPeople;
        if (Dist(car ? car->pos : e.pos, eye) < r - 1.0f && !owners[o].held.count(MakeKey(uint8_t(o), e.local))) ++missing_at_dwell;
      }
    }
    while (eye[0] < 900.0f) { eye[0] += 0.2f; tick(); }
    for (int t = 0; t < 40; ++t) tick();
    size_t left = 0; for (auto& w : owners) left += w.held.size();
    std::printf("      3 owners, 1 viewer walking past: %u creates, %u removes, held past r + 20 m %u, crew without its car %u, missing in range after 5 s %u, left at the end %zu\n",
                creates, removes, far_held, crew_alone, missing_at_dwell, left);
    Check(creates > 0 && removes > 0 && wrong_reason == 0 && owner_mismatch == 0, "shared: per-object creates and removes on every owner, every remove kRemoveInterest, keys in the owner's namespace");
    Check(far_held == 0 && updates_out == 0, "shared: no copy held past its radius + 20 m, no update for an object not created there");
    Check(crew_alone == 0, "shared: a crew member is never held without its car (they come and go together)");
    Check(missing_at_dwell == 0, "shared: everything within its radius of the viewer is created within 5 s of standing there, on every owner");
    Check(left == 0, "shared: 500 m past the last owner's spot nothing is held any more");
    // Hysteresis: a person on the edge (95-115 m) stays; past 120 m it goes; it comes back only within 100 m.
    Owner h;
    { Entry& e = h.reg.Add(MakeNetId(1, 9, 1), 1, 9, kKindAmbient, 1, 1000); e.pos[0] = 95; }
    { Member& m = h.members[viewer]; m.present = m.in_area = true; m.budget = 570; m.interest_people = kInterestPeople; m.interest_cars = kInterestCars; }
    uint32_t hc = 0, hr = 0;
    auto htick = [&](float x) { h.reg.Find(9)->pos[0] = x; now += kTickMs; auto p = h.sched.Plan(now, h.reg, h.members, h.ref); hc += uint32_t(p[viewer].creates.size()); hr += uint32_t(p[viewer].removes.size()); };
    for (int t = 0; t < 200; ++t) htick(95 + 20.0f * float(t % 20) / 19.0f);   // (95-115 m: created at once, then held)
    const bool held_edge = hc == 1 && hr == 0;
    htick(121); const bool gone = hr == 1;
    htick(110); htick(105); const bool not_back = hc == 1;
    htick(99); const bool back = hc == 2;
    Check(held_edge && gone && not_back && back, "shared: hysteresis 20 m: held from 95 to 115 m, removed past 120 m, offered again only within 100 m");
    // Area mode (no radius): the same far object is offered and never removed for interest.
    Owner a;
    { Entry& e = a.reg.Add(MakeNetId(0, 9, 1), 0, 9, kKindAmbient, 1, 1000); e.pos[0] = 400; }
    { Member& m = a.members[viewer]; m.present = m.in_area = true; m.budget = 570; }
    uint32_t ac = 0, ar = 0;
    for (int t = 0; t < 20; ++t) { now += kTickMs; auto p = a.sched.Plan(now, a.reg, a.members, a.ref); ac += uint32_t(p[viewer].creates.size()); ar += uint32_t(p[viewer].removes.size()); }
    Check(ac == 1 && ar == 0, "area mode (interest 0): an object 400 m away is offered as in 2.4, no interest removes");
  }

  // 11. (3.0 phase 2b) The spawn veto: 2-6 players at random places; every
  // game makes spots within its own reach and asks SpawnWinner with the other
  // players as it knows them. Exactly one of the games that could make a spot
  // says it spawns there; players within the margin of each other give every
  // spot to the lower slot; a player past its reach vetoes nothing.
  {
    std::mt19937 vr(seed + 11);
    std::uniform_real_distribution<float> place(-150.0f, 150.0f), unit(-1.0f, 1.0f);
    const float reach = 90.0f;
    unsigned spots = 0, none = 0, twice = 0, by_nearest_off = 0;
    for (int round = 0; round < 400; ++round) {
      const int n = 2 + round % 5;
      SpawnView all[kMaxSlots];
      for (int i = 0; i < n; ++i) { all[i].slot = uint8_t((round * 7 + i * 5) % kMaxSlots); all[i].pos[0] = place(vr); all[i].pos[1] = 0; all[i].pos[2] = place(vr); }
      bool distinct = true;
      for (int i = 0; i < n; ++i) for (int j = 0; j < i; ++j) distinct = distinct && all[i].slot != all[j].slot;
      if (!distinct) continue;
      for (int maker = 0; maker < n; ++maker) for (int k = 0; k < 10; ++k) {
        const float spot[3] = {all[maker].pos[0] + reach * unit(vr), 0, all[maker].pos[2] + reach * unit(vr)};
        if (Dist2(spot, all[maker].pos) > reach) continue;
        ++spots;
        int yes = 0; float d_yes = 0, d_min = 1e9f;
        for (int g = 0; g < n; ++g) {
          const float d = Dist2(spot, all[g].pos);
          if (d > reach) continue;   // (that game's spawner never makes this spot)
          d_min = std::min(d_min, d);
          SpawnView others[kMaxSlots]; int m = 0;
          for (int o = 0; o < n; ++o) if (o != g) others[m++] = all[o];
          if (SpawnWinner(spot, all[g].slot, all[g].pos, others, m, reach) == all[g].slot) { ++yes; d_yes = d; }
        }
        if (yes == 0) ++none; else if (yes > 1) ++twice;
        if (yes == 1 && d_yes > d_min + kVetoMargin + 1e-3f) ++by_nearest_off;
      }
    }
    std::printf("      veto: %u spots, none %u, two or more %u, winner past the nearest + margin %u\n", spots, none, twice, by_nearest_off);
    Check(spots > 1000 && none == 0 && twice == 0, "veto: exactly one game spawns at every spot (2-6 players, each with its own view of the others)");
    Check(by_nearest_off == 0, "veto: the spawning game is the nearest player's or one within the 10 m margin of it");
    // Two players 6 m apart (one car, standing together): the lower slot spawns everywhere, the other nowhere.
    const SpawnView a{3, {0, 0, 0}}, b{7, {6, 0, 0}};
    unsigned a_wins = 0, b_wins = 0;
    for (int k = 0; k < 200; ++k) {
      const float spot[3] = {60.0f * unit(vr), 0, 60.0f * unit(vr)};   // (within both players' reach)
      a_wins += SpawnWinner(spot, a.slot, a.pos, &b, 1, reach) == a.slot;
      b_wins += SpawnWinner(spot, b.slot, b.pos, &a, 1, reach) == b.slot;
    }
    Check(a_wins == 200 && b_wins == 0, "veto: two players within the margin of each other: the lower slot spawns at every spot");
    // 100 m apart: each spawns its own side; 4 m past the midpoint is still the lower slot's (the margin).
    const SpawnView c{2, {0, 0, 0}}, d{1, {100, 0, 0}};
    const float near_c[3] = {10, 0, 0}, near_d[3] = {90, 0, 0}, mid_c[3] = {46, 0, 0}, past_band[3] = {38, 0, 0};
    Check(SpawnWinner(near_c, c.slot, c.pos, &d, 1, reach) == 2 && SpawnWinner(near_d, d.slot, d.pos, &c, 1, reach) == 1 &&
          SpawnWinner(mid_c, c.slot, c.pos, &d, 1, reach) == 1 && SpawnWinner(past_band, c.slot, c.pos, &d, 1, reach) == 2,
          "veto: 100 m apart each spawns its own side, the tie band (within 10 m) goes to the lower slot");
    // A player past its reach of the spot vetoes nothing (it can't fill it).
    const SpawnView e{0, {120, 0, 0}};
    const float spot_e[3] = {25, 0, 0};
    Check(SpawnWinner(spot_e, 5, c.pos, &e, 1, reach) == 5, "veto: another player beyond its reach (95 m > 90 m) of the spot leaves it to this game");
  }

  // 12. (3.0 phase 2c, review_phase2a M3) A viewer that holds an owner's
  // objects jumps 1 km (a respawn, a taxi) and reports another chunk mask:
  // everything it held is removed (kRemoveInterest) in the next tick, nothing
  // is left frozen there; the same entries out of its chunks in area mode
  // (no radius) are skipped as before, with no removes.
  {
    Registry reg; Scheduler sched; std::array<Member, kMaxSlots> members{}; const float ref[3] = {0, 0, 0};
    std::mt19937 jr(seed + 12);
    std::uniform_real_distribution<float> near(-60.0f, 60.0f);
    for (uint32_t i = 0; i < 40; ++i) {
      Entry& e = reg.Add(MakeNetId(0, 0x200 + i, 3), 0, 0x200 + i, i % 3 ? kKindAmbient : kKindTraffic, i % 3 ? 1 : 2, 1000);
      e.pos[0] = near(jr); e.pos[2] = near(jr); e.chunk = int8_t(i % 4); e.model = uint16_t(1 + i % 9);
    }
    Member& m = members[2];
    m.present = m.in_area = true; m.budget = 2400; m.interest_people = kInterestPeople; m.interest_cars = kInterestCars; m.chunks = 0xF;
    uint64_t now = 1000; std::set<Key> held; uint32_t wrong = 0;
    auto tick = [&]() { now += kTickMs; auto p = sched.Plan(now, reg, members, ref); for (auto& c : p[2].creates) held.insert(c.key); for (auto& r : p[2].removes) { held.erase(r.key); if (r.reason != kRemoveInterest) ++wrong; } return p; };
    for (int t = 0; t < 60; ++t) tick();
    const size_t before = held.size();
    m.eye[0] = 1000; m.chunks = 0xF0;   // (1 km away, other chunks)
    tick();
    std::printf("      jump: held %zu before, %zu one tick after\n", before, held.size());
    Check(before == 40 && held.empty() && wrong == 0, "shared: a viewer that jumps 1 km with a new chunk mask has every copy of the owner's removed in one tick (kRemoveInterest)");
    m.eye[0] = 0; m.chunks = 0x3;   // (back, half the chunks: only those are offered, and kept)
    for (int t = 0; t < 60; ++t) tick();
    unsigned off_chunk = 0; for (Key k : held) if (const Entry* e = reg.Find(IdOf(k)); e && e->chunk >= 2) ++off_chunk;
    Check(!held.empty() && off_chunk == 0, "shared: back with chunks 0-1 only: what is in them is held, nothing of chunks 2-3");
    Registry ra; Scheduler sa; std::array<Member, kMaxSlots> ma{};
    for (uint32_t i = 0; i < 10; ++i) { Entry& e = ra.Add(MakeNetId(0, 0x300 + i, 3), 0, 0x300 + i, kKindAmbient, 1, 1000); e.pos[0] = float(i); e.chunk = 1; }
    ma[2].present = ma[2].in_area = true; ma[2].budget = 2400; ma[2].chunks = 0x2;
    uint32_t ac = 0, ar = 0;
    for (int t = 0; t < 20; ++t) { now += kTickMs; auto p = sa.Plan(now, ra, ma, ref); ac += uint32_t(p[2].creates.size()); }
    ma[2].chunks = 0x1;
    for (int t = 0; t < 20; ++t) { now += kTickMs; auto p = sa.Plan(now, ra, ma, ref); ar += uint32_t(p[2].removes.size()); }
    Check(ac == 10 && ar == 0 && ma[2].skipped_chunk > 0, "area mode (interest 0): an entry out of the member's chunks is skipped as before, no removes");
  }

  // 13. (3.0 phase 2c) Budgets per viewer: the viewer's share of what it takes
  // in, by the copies it holds of each owner with a floor; an owner's budget by
  // its busy viewers, the relay's share, the viewer's share; the viewer's
  // backpressure; WWA1's 2c fields round trip.
  {
    const uint32_t w3[3] = {20, 10, 0};
    uint32_t s3[3]; ShareOut(w3, 3, kViewerBps, kViewerFloorBps, s3);
    const uint32_t w0[4] = {0, 0, 0, 0};
    uint32_t s4[4]; ShareOut(w0, 4, 100, 30, s4);   // (the floor doesn't fit: even shares)
    const uint32_t w1[1] = {5};
    uint32_t s1[1]; ShareOut(w1, 1, kViewerBps, kViewerFloorBps, s1);
    std::printf("      shares: 20/10/0 copies of %u B/s: %u / %u / %u; 4 x 0 of 100 (floor 30): %u %u %u %u\n", kViewerBps, s3[0], s3[1], s3[2], s4[0], s4[1], s4[2], s4[3]);
    Check(s3[0] + s3[1] + s3[2] == kViewerBps && s3[2] == kViewerFloorBps && s3[0] > s3[1] && s3[1] > s3[2] && s3[0] - kViewerFloorBps == 2 * (s3[1] - kViewerFloorBps),
          "ShareOut: the floor for each, the rest by copies held (20 / 10 / 0), summing to the total");
    Check(s4[0] + s4[1] + s4[2] + s4[3] == 100 && s4[0] == 25 && s4[3] == 25 && s1[0] == kViewerBps, "ShareOut: a floor that doesn't fit gives even shares; one owner gets everything");
    std::mt19937 sr(seed + 13);
    unsigned bad_sum = 0;
    for (int round = 0; round < 500; ++round) {
      const int n = 1 + int(sr() % 11);
      uint32_t w[11], o[11], total = 1000 + sr() % 60000, floor = sr() % 8000;
      for (int i = 0; i < n; ++i) w[i] = sr() % 50;
      ShareOut(w, n, total, floor, o);
      uint64_t sum = 0; bool floors = true;
      for (int i = 0; i < n; ++i) { sum += o[i]; floors = floors && (uint64_t(floor) * n > total || o[i] >= floor); }
      if (sum != total || !floors) ++bad_sum;
    }
    Check(bad_sum == 0, "ShareOut: 500 random rounds sum to the total and keep the floor when it fits");
    const uint32_t alone = SharedTickBudget(1000, 1, 0, 0), busy11 = SharedTickBudget(1000, 11, 0, 0), busy2 = SharedTickBudget(1000, 2, 0, 0);
    const uint32_t relayed = SharedTickBudget(1000, 2, RelayStreams(1), 0), relayed10 = SharedTickBudget(1000, 11, RelayStreams(10), 0), capped = SharedTickBudget(1000, 1, 0, 9000);
    std::printf("      budgets (B/tick): one viewer %u, 2 %u, 11 %u; relayed 1 of 2 %u, 10 of 11 %u; viewer's share 9 KB/s %u\n", alone, busy2, busy11, relayed, relayed10, capped);
    Check(alone == TickBudget(1000, 1) && busy11 == TickBudget(1000, 11) && busy2 == TickBudget(1000, 2), "SharedTickBudget: no relay, no share: TickBudget by the busy viewers");
    const uint32_t shared11 = SharedTickBudget(1000, 11, 0, kViewerBps / 11), tiny = SharedTickBudget(1000, 1, 0, 100);
    Check(relayed <= busy2 && relayed10 == kSharedMinBps * kTickMs / 1000 && capped == 9000 * kTickMs / 1000, "SharedTickBudget: the relay's share and the viewer's share cap it, never below 1.5 KB/s");
    Check(shared11 == kViewerBps / 11 * kTickMs / 1000 && shared11 < kBudgetMinBps * kTickMs / 1000 && tiny == kSharedMinBps * kTickMs / 1000, "SharedTickBudget: the caps apply after the 6 KB/s floor (review_phase2c M6)");
    Check(!ViewerBehind(3000, 0) && !ViewerBehind(10000, 8000) && ViewerBehind(10000, 7000) && ViewerBehind(40000, 0), "ViewerBehind: over a quarter of 4 KB+ lost");
    AdmitMsg a; a.head.budget_bps = 18000; a.head.received = 123456; a.head.interest_people_pct = 70; a.head.interest_cars_pct = 50; a.head.owner = 4;
    uint8_t abuf[kMaxMessage];
    const size_t na = WriteAdmit(a, abuf, sizeof abuf);
    AdmitMsg ab;
    Check(na == sizeof(AdmitRec) && ReadAdmit(abuf, na, ab) && ab.head.budget_bps == 18000 && ab.head.received == 123456 && ab.head.interest_people_pct == 70 && ab.head.interest_cars_pct == 50,
          "WWA1 (2c): the share, the bytes received, the interest percentages round trip");
    Registry reg; Scheduler sched; Member m; m.present = m.in_area = true;
    sched.OnAdmit(1000, reg, m, 1, 4, ab);
    AdmitMsg az; sched.OnAdmit(1000, reg, m, 1, 4, ab);
    Member mz; sched.OnAdmit(1000, reg, mz, 1, 4, az);
    Check(m.viewer_bps == 18000 && m.interest_people_pct == 70 && m.interest_cars_pct == 50 && mz.interest_people_pct == 100 && mz.interest_cars_pct == 100 && mz.viewer_bps == 0,
          "OnAdmit (2c): the member's share and percentages taken; 0 (area mode, an older WWA1) is the full radius and no share");
  }

  // 14. (3.0 phase 2c) The police response owner: wanted players within 150 m
  // of each other (chained) share one, the highest level, then the lower slot;
  // every player of a group computes the same owner and the same group; a
  // player alone owns its own; one not wanted is in no group.
  {
    std::mt19937 pr(seed + 14);
    std::uniform_real_distribution<float> place(-300.0f, 300.0f);
    unsigned rounds = 0, disagree = 0, not_highest = 0, mask_wrong = 0, alone_wrong = 0, unwanted_wrong = 0, groups = 0;
    for (int round = 0; round < 500; ++round) {
      const int n = 2 + round % 7;
      WantedView v[kMaxSlots];
      for (int i = 0; i < n; ++i) { v[i].slot = uint8_t((round * 5 + i * 7) % kMaxSlots); v[i].pos[0] = place(pr); v[i].pos[2] = place(pr); v[i].level = uint8_t(pr() % 6); }
      bool distinct = true;
      for (int i = 0; i < n; ++i) for (int j = 0; j < i; ++j) distinct = distinct && v[i].slot != v[j].slot;
      if (!distinct) continue;
      ++rounds;
      uint8_t owner[kMaxSlots]; uint32_t mask[kMaxSlots];
      for (int i = 0; i < n; ++i) mask[i] = ResponseGroup(v, n, i, &owner[i]);
      for (int i = 0; i < n; ++i) {
        if (!v[i].level) { if (mask[i] || owner[i] != 0xFF) ++unwanted_wrong; continue; }
        bool near_other = false;
        for (int j = 0; j < n; ++j) if (j != i && v[j].level && Dist2(v[i].pos, v[j].pos) <= kResponseGroupM) near_other = true;
        if (!near_other && (owner[i] != v[i].slot || mask[i] != (1u << v[i].slot))) ++alone_wrong;
        if (owner[i] == v[i].slot) ++groups;
        for (int j = 0; j < n; ++j) if (j != i && (mask[i] >> v[j].slot & 1)) {
          if (owner[j] != owner[i] || mask[j] != mask[i]) ++disagree;
          if (v[j].level > v[i].level && owner[i] == v[i].slot) ++not_highest;
          if (!v[j].level) ++mask_wrong;
        }
      }
    }
    std::printf("      response: %u rounds, %u groups; disagree %u, owner not the highest %u, unwanted in a group %u, alone not its own %u\n", rounds, groups, disagree, not_highest, mask_wrong + unwanted_wrong, alone_wrong);
    Check(rounds > 300 && disagree == 0 && not_highest == 0, "response: every wanted player of a group computes the same owner and group; the owner has the group's highest level");
    Check(mask_wrong == 0 && unwanted_wrong == 0 && alone_wrong == 0, "response: a player not wanted is in no group; a wanted player alone owns its own response");
    WantedView chain[3] = {{4, {0, 0, 0}, 2}, {1, {140, 0, 0}, 2}, {9, {280, 0, 0}, 3}};
    uint8_t o0 = 0, o2 = 0;
    const uint32_t m0 = ResponseGroup(chain, 3, 0, &o0), m2 = ResponseGroup(chain, 3, 2, &o2);
    WantedView tie[2] = {{6, {0, 0, 0}, 4}, {3, {50, 0, 0}, 4}};
    uint8_t t0 = 0; ResponseGroup(tie, 2, 0, &t0);
    Check(m0 == m2 && m0 == ((1u << 4) | (1u << 1) | (1u << 9)) && o0 == 9 && o2 == 9 && t0 == 3, "response: a chain 140 m + 140 m is one group owned by its highest level (slot 9); a tie goes to the lower slot");
  }

  // 15. (3.0 phase 3, review_phase2b M2 / H1 / M6 / L5) The veto with spots
  // made past a game's reach (its camera's ring times the zoom: up to 1.2 x
  // reach): every game that could make a spot asks with the same players
  // (each only within reach, itself too), so exactly one spawns at every spot
  // that some game has within reach. An inactive game leaves its spots to an
  // active player within reach. The hood rule is greedy by slot: a chain of
  // players 140 m apart runs it on slots 0 and 2, not 1.
  {
    std::mt19937 vr(seed + 15);
    std::uniform_real_distribution<float> place(-150.0f, 150.0f), unit(-1.0f, 1.0f);
    const float reach = 90.0f;
    unsigned spots = 0, none = 0, twice = 0, nobody_near = 0;
    for (int round = 0; round < 400; ++round) {
      const int n = 2 + round % 5;
      SpawnView all[kMaxSlots];
      for (int i = 0; i < n; ++i) { all[i].slot = uint8_t((round * 7 + i * 5) % kMaxSlots); all[i].pos[0] = place(vr); all[i].pos[1] = 0; all[i].pos[2] = place(vr); }
      bool distinct = true;
      for (int i = 0; i < n; ++i) for (int j = 0; j < i; ++j) distinct = distinct && all[i].slot != all[j].slot;
      if (!distinct) continue;
      for (int maker = 0; maker < n; ++maker) for (int k = 0; k < 10; ++k) {
        const float spot[3] = {all[maker].pos[0] + 1.2f * reach * unit(vr), 0, all[maker].pos[2] + 1.2f * reach * unit(vr)};
        if (Dist2(spot, all[maker].pos) > 1.2f * reach) continue;
        bool any_in = false;
        for (int g = 0; g < n; ++g) any_in = any_in || Dist2(spot, all[g].pos) <= reach;
        if (!any_in) { ++nobody_near; continue; }   // (only games past reach could make it: each its own, as without a veto)
        ++spots;
        int yes = 0;
        for (int g = 0; g < n; ++g) {
          if (Dist2(spot, all[g].pos) > 1.2f * reach) continue;   // (that game's spawner never makes this spot)
          SpawnView others[kMaxSlots]; int m = 0;
          for (int o = 0; o < n; ++o) if (o != g) others[m++] = all[o];
          yes += SpawnWinner(spot, all[g].slot, all[g].pos, others, m, reach) == all[g].slot;
        }
        if (yes == 0) ++none; else if (yes > 1) ++twice;
      }
    }
    std::printf("      veto past reach: %u spots, none %u, two or more %u (nobody within reach %u)\n", spots, none, twice, nobody_near);
    Check(spots > 1000 && none == 0 && twice == 0, "veto (M2): spots made up to 1.2 x reach: exactly one game spawns at each one some game has within reach");
    // The review's example: reach 100, A (slot 0) 103 m from the spot, B (slot 1) 99 m: only B.
    const SpawnView ra{0, {103, 0, 0}}, rb{1, {-99, 0, 0}};
    const float spot0[3] = {0, 0, 0};
    Check(SpawnWinner(spot0, ra.slot, ra.pos, &rb, 1, 100.0f) == 1 && SpawnWinner(spot0, rb.slot, rb.pos, &ra, 1, 100.0f) == 1, "veto (M2): A past its reach (103 m), B within (99 m): both say B");
    // H1: an inactive game (dead, paused) leaves a spot within an active player's reach to it, keeps one past it.
    const SpawnView act{6, {50, 0, 0}};
    const float near_act[3] = {20, 0, 0}, far_act[3] = {-70, 0, 0}, here[3] = {0, 0, 0};
    Check(SpawnWinner(near_act, 2, here, &act, 1, reach, false) == 6 && SpawnWinner(far_act, 2, here, &act, 1, reach, false) == 2 && SpawnWinner(near_act, 2, here, &act, 1, reach, true) == 2,
          "veto (H1): an inactive game gives a spot within an active player's reach to it (whatever the slots), keeps one past it");
    // M6: greedy by slot; every game computes the same runners; no two runners within the radius; every player has one within it.
    unsigned hood_rounds = 0, hood_disagree = 0, hood_close = 0, hood_uncovered = 0;
    for (int round = 0; round < 400; ++round) {
      const int n = 2 + round % 7;
      SpawnView all[kMaxSlots];
      for (int i = 0; i < n; ++i) { all[i].slot = uint8_t((round * 5 + i * 7) % kMaxSlots); all[i].pos[0] = 2 * place(vr); all[i].pos[2] = 2 * place(vr); }
      bool distinct = true;
      for (int i = 0; i < n; ++i) for (int j = 0; j < i; ++j) distinct = distinct && all[i].slot != all[j].slot;
      if (!distinct) continue;
      ++hood_rounds;
      bool runs[kMaxSlots];
      for (int i = 0; i < n; ++i) runs[i] = HoodRuns(all, n, i, true, 150.0f);
      for (int i = 0; i < n; ++i) {   // (each game lists the players in its own order: the same answers)
        SpawnView rot[kMaxSlots];
        for (int k = 0; k < n; ++k) rot[k] = all[(i + k) % n];
        for (int k = 0; k < n; ++k) if (HoodRuns(rot, n, k, true, 150.0f) != runs[(i + k) % n]) ++hood_disagree;
      }
      for (int i = 0; i < n; ++i) {
        bool covered = runs[i];
        for (int j = 0; j < n; ++j) if (j != i && runs[j] && Dist2(all[i].pos, all[j].pos) <= 150.0f) { covered = true; if (runs[i]) ++hood_close; }
        if (!covered) ++hood_uncovered;
      }
    }
    std::printf("      hood: %u rounds, disagree %u, runners within 150 m of each other %u, players with no runner near %u\n", hood_rounds, hood_disagree, hood_close, hood_uncovered);
    Check(hood_rounds > 300 && hood_disagree == 0 && hood_close == 0 && hood_uncovered == 0, "hood (M6): greedy by slot: the same runners on every game, none within 150 m of another, every player has one within 150 m");
    const SpawnView chain3[3] = {{0, {0, 0, 0}}, {1, {140, 0, 0}}, {2, {280, 0, 0}}};
    Check(HoodRuns(chain3, 3, 0, true, 150.0f) && !HoodRuns(chain3, 3, 1, true, 150.0f) && HoodRuns(chain3, 3, 2, true, 150.0f), "hood (M6): a chain 0 - 1 - 2, 140 m apart: slots 0 and 2 run it, 1 doesn't");
    // L5: an inactive game runs it only with no running player within the radius.
    const SpawnView pair[2] = {{3, {0, 0, 0}}, {1, {100, 0, 0}}};
    const SpawnView lone[2] = {{3, {0, 0, 0}}, {1, {400, 0, 0}}};
    Check(!HoodRuns(pair, 2, 0, false, 150.0f) && HoodRuns(lone, 2, 0, false, 150.0f) && !HoodRuns(pair, 2, 0, true, 150.0f) && HoodRuns(pair, 2, 1, true, 150.0f),
          "hood (L5): an inactive game skips it near an active one (whatever its slot), runs it alone");
  }

  // 16. (3.0 phase 3) The hand-over: WWH1 round trips; HandOut / HandIn /
  // HandWins / ClaimDelayMs driven by a small network of 3 games (the owner A
  // slot 0, the candidate B slot 1, a viewer C slot 2) with delays: the normal
  // order, an Accept too late, B gone after its Accept, B unable to adopt (its
  // Return), an OwnerChanged slower than kHandReleaseMs (both own it for a
  // while, then the newer epoch wins), the owner gone (claims by slot), and
  // random runs. At the end exactly one game owns the object and every other
  // holds its copy under that owner and epoch. Then HandPick's hysteresis.
  {
    HandRec r{}; r.type = kHandRelease; r.reason = kHandNoNode; r.kind = kKindTraffic; r.flags = kHandUrgent; SetHandNet(r, MakeNetId(3, 0x12345, 0xBEEF)); r.epoch = 7; r.id = 0x2222;
    r.created = 0x0805; r.pos[0] = 12.5f; r.vel[2] = -3; r.hp = 50; r.hp_max = 100; r.n_crew = 2; r.crew[0] = 0x3333; r.crew_hp[1] = 25;
    uint8_t buf[256]; HandRec back{};
    const size_t n = WriteHand(r, buf, sizeof(buf));
    const bool rt = n == sizeof(HandRec) && ReadHand(buf, n, back) && HasMagic(back.magic, "WWH1") && HandNet(back) == MakeNetId(3, 0x12345, 0xBEEF) && back.epoch == 7 && back.created == 0x0805 &&
                    back.pos[0] == 12.5f && back.vel[2] == -3 && back.n_crew == 2 && back.crew[0] == 0x3333 && back.crew_hp[1] == 25;
    HandRec bad = r; bad.n_crew = kHandMaxCrew + 1;
    buf[4] = 9;   // (an unknown type)
    const bool refused = !ReadHand(buf, n, back) && !WriteHand(bad, buf, sizeof(buf)) && !ReadHand(buf, n - 1, back);
    Check(rt && refused && sizeof(HandRec) == 110, "WWH1: round trip (NetId, epoch, state, crew); a bad type, crew count or length refused");
    Check(EpochNewer(1, 0) && EpochNewer(2, 0xFFFF) && !EpochNewer(5, 5) && HandWins(4, 3, 3, 0) && HandWins(4, 1, 4, 2) && !HandWins(4, 2, 4, 1) && ClaimDelayMs(0) == 2000 && ClaimDelayMs(11) == 4750,
          "epochs wrap; the newer epoch wins, then the lower slot; claims wait 2 s + 250 ms x slot");
    { // (review_phase3 M3 / M5) a Cancel goes on the wire; any newer OwnerChanged ends a release (no take-back).
      HandRec x = r; x.type = kHandCancel; x.n_crew = 0; HandRec xb{};
      const size_t xn = WriteHand(x, buf, sizeof(buf));
      HandOut o; o.Offer(1, 7, 0); o.OnAccept(1, 7, 10);
      HandOut stale = o;
      const bool third = o.OnOwner(2, 9) == kHandDone && o.step == kHandIdle && o.Tick(5000) == kHandNothing;
      const bool older = stale.OnOwner(2, 7) == kHandNothing && stale.Tick(5000) == kHandTakeBack;
      Check(xn == sizeof(HandRec) && ReadHand(buf, xn, xb) && xb.type == kHandCancel && third && older,
            "WWH1 Cancel round trip; an OwnerChanged from any game at e + 1 or later ends a release (no take-back), an older one doesn't");
    }

    struct Game { bool owns = false, present = true, can_adopt = true, has_copy = true; uint16_t epoch = 0; uint8_t owner = 0; HandOut out; bool have_in = false; HandIn in; uint64_t claim_at = 0; };
    struct Msg { uint64_t at; uint8_t from, to; HandRec r; };
    struct Net {
      Game g[3]; std::vector<Msg> q; uint64_t now = 0; uint32_t delay_min = 30, delay_max = 150; std::mt19937 rng{1};
      uint64_t both_ms = 0; unsigned yields = 0, take_backs = 0, returns = 0, claims = 0;
      uint32_t Delay() { return delay_min + uint32_t(rng() % (delay_max - delay_min + 1)); }
      void Send(uint8_t from, uint8_t to, uint8_t type, uint16_t epoch, uint8_t reason = 0) {
        HandRec m{}; m.type = type; m.epoch = epoch; m.reason = reason; SetHandNet(m, MakeNetId(0, 9, 1));
        for (int t = 0; t < 3; ++t) if ((to == 0xFF && t != from) || t == to) q.push_back({now + Delay(), from, uint8_t(t), m});
      }
      void Own(uint8_t s, uint16_t e) { g[s].owns = true; g[s].epoch = e; g[s].owner = s; g[s].claim_at = 0; Send(s, 0xFF, kHandOwner, e); }
      void Take(uint8_t s, const Msg& m) {
        Game& x = g[s];
        if (!x.present || !g[m.from].present) return;   // (a game that left: what it still had in flight is dropped with it)
        const HandRec& r = m.r;
        switch (r.type) {
          case kHandOffer:
            if (!x.has_copy) { Send(s, m.from, kHandDecline, r.epoch, kHandNoCopy); break; }
            x.have_in = true; x.in.from = m.from; x.in.epoch = r.epoch; x.in.at = now; Send(s, m.from, kHandAccept, r.epoch); break;
          case kHandAccept:
            if (x.out.OnAccept(m.from, r.epoch, now) == kHandSendRelease) { x.owns = false; x.epoch = x.out.epoch; x.owner = m.from; Send(s, m.from, kHandRelease, x.out.epoch); }
            break;
          case kHandDecline: x.out.OnDecline(m.from, r.epoch); break;
          case kHandRelease:
            if (x.have_in && x.in.Matches(m.from, r.epoch, now) && x.can_adopt) { x.have_in = false; Own(s, r.epoch); }
            else { x.have_in = false; Send(s, m.from, kHandReturn, uint16_t(r.epoch + 1), kHandStale); ++returns; }
            break;
          case kHandReturn: { const uint16_t e = uint16_t(x.out.epoch + 1); if (x.out.OnReturn(m.from, r.epoch) == kHandTakeBack) { Own(s, e); ++take_backs; } break; }
          case kHandOwner:
            x.out.OnOwner(m.from, r.epoch);
            if (x.owns) { if (!HandWins(x.epoch, s, r.epoch, m.from)) { x.owns = false; x.epoch = r.epoch; x.owner = m.from; ++yields; } }
            else if (x.owner == m.from || HandWins(r.epoch, m.from, x.epoch, x.owner)) { x.epoch = r.epoch; x.owner = m.from; x.claim_at = 0; }
            break;
        }
      }
      void Step(uint64_t dt) {
        for (uint64_t t = 0; t < dt; t += 10) {
          now += 10;
          std::stable_sort(q.begin(), q.end(), [](const Msg& a, const Msg& b) { return a.at < b.at; });
          while (!q.empty() && q.front().at <= now) { const Msg m = q.front(); q.erase(q.begin()); Take(m.to, m); }
          for (uint8_t s = 0; s < 3; ++s) {
            Game& x = g[s];
            if (!x.present) continue;
            const uint16_t e = x.out.epoch;
            if (x.out.Tick(now) == kHandTakeBack) { Own(s, uint16_t(e + 1)); ++take_backs; }
            if (x.claim_at && now >= x.claim_at) { ++claims; Own(s, uint16_t(x.epoch + 1)); }
          }
          int owners = 0; for (const Game& x : g) owners += x.present && x.owns;
          if (owners > 1) both_ms += 10;
        }
      }
      void Offer(uint8_t from, uint8_t to) { Game& x = g[from]; x.out.Offer(to, x.epoch, now); Send(from, to, kHandOffer, x.epoch); }
      bool Settled(uint8_t* who) const {
        int owners = 0; uint8_t o = 0xFF; uint16_t e = 0;
        for (uint8_t s = 0; s < 3; ++s) if (g[s].present && g[s].owns) { ++owners; o = s; e = g[s].epoch; }
        if (who) *who = o;
        if (owners != 1) return false;
        for (uint8_t s = 0; s < 3; ++s) if (g[s].present && !g[s].owns && (g[s].owner != o || g[s].epoch != e)) return false;
        return true;
      }
    };
    auto fresh = [&](unsigned seed_k) { Net w; w.rng.seed(seed + seed_k); w.g[0].owns = true; for (Game& x : w.g) { x.owner = 0; x.epoch = 3; } return w; };
    uint8_t who = 0xFF;
    // The normal order: B owns at e + 1, nobody twice at once.
    Net a = fresh(1); a.Offer(0, 1); a.Step(3000);
    Check(a.Settled(&who) && who == 1 && a.g[1].epoch == 4 && a.both_ms == 0 && !a.g[0].owns, "hand-over: Offer, Accept, Release, OwnerChanged: B owns it at e + 1, A and C hold B's copy, never two owners");
    // The Accept too late (a 1.2 s link): A keeps it; the late Release never comes, B's Accept expires.
    Net b = fresh(2); b.delay_min = b.delay_max = 1200; b.Offer(0, 1); b.Step(6000);
    Check(b.Settled(&who) && who == 0 && b.g[0].epoch == 3 && b.both_ms == 0, "hand-over: no Accept within 1 s: the owner keeps it at its epoch");
    // B gone after its Accept (the Release lost): A takes it back at e + 2.
    Net c = fresh(3); c.delay_min = c.delay_max = 40; c.Offer(0, 1);
    for (int i = 0; i < 100 && c.g[0].out.step != kHandReleased; ++i) c.Step(10);
    c.g[1].present = false; c.Step(4000);
    Check(c.g[0].owns && c.g[0].epoch == 5 && c.take_backs == 1 && c.g[2].owner == 0 && c.g[2].epoch == 5, "hand-over: the Release lost (B gone): A takes it back at e + 2 after 2 s, C follows");
    // B can't adopt at the Release: its Return, A takes it back at e + 2.
    Net d = fresh(4); d.g[1].can_adopt = false; d.Offer(0, 1); d.Step(3000);
    Check(d.Settled(&who) && who == 0 && d.g[0].epoch == 5 && d.returns == 1 && d.take_backs == 1 && d.both_ms == 0, "hand-over: B can't adopt at the Release: Return (e + 2), A owns it again");
    // A Decline: nothing changes.
    Net e = fresh(5); e.g[1].has_copy = false; e.Offer(0, 1); e.Step(2000);
    Check(e.Settled(&who) && who == 0 && e.g[0].epoch == 3 && e.g[0].out.step == kHandIdle, "hand-over: Decline: the owner keeps it, its offer over");
    // The OwnerChanged slower than 2 s: A takes it back at e + 2 too; B (e + 1) yields on A's.
    Net f = fresh(6); f.delay_min = f.delay_max = 100; f.Offer(0, 1);
    for (int i = 0; i < 100 && f.g[0].out.step != kHandReleased; ++i) f.Step(10);
    f.delay_min = f.delay_max = 2500; f.Step(10000);   // (the Release still goes in 100 ms; the OwnerChanged takes 2.5 s)
    Check(f.Settled(&who) && who == 0 && f.g[0].epoch == 5 && f.yields >= 1 && f.both_ms > 0 && f.both_ms < 4000, "hand-over: an OwnerChanged slower than 2 s: two owners for a while, the newer epoch (A's e + 2) wins, B yields");
    // The owner gone: B (slot 1) and C (slot 2) claim after 2.25 / 2.5 s; C hears B's first.
    Net h = fresh(7); h.g[0].present = false; h.g[1].claim_at = h.now + ClaimDelayMs(1); h.g[2].claim_at = h.now + ClaimDelayMs(2); h.Step(6000);
    Check(h.Settled(&who) && who == 1 && h.g[1].epoch == 4 && h.claims == 1, "claims: the owner gone: slot 1 claims at 2.25 s, slot 2 hears it before its own (2.5 s)");
    // A slow link (400 ms): both claim at the same epoch; the lower slot keeps it, the other yields.
    Net k = fresh(8); k.delay_min = k.delay_max = 400; k.g[0].present = false; k.g[1].claim_at = k.now + ClaimDelayMs(1); k.g[2].claim_at = k.now + ClaimDelayMs(2); k.Step(6000);
    Check(k.Settled(&who) && who == 1 && k.claims == 2 && k.yields == 1, "claims: both claim at e + 1 (a 400 ms link): the lower slot keeps it, the other yields");
    // Random runs: delays 30-600 ms, offers back and forth, a game leaving at random: always settles on one owner.
    unsigned runs = 0, unsettled = 0, doubled = 0;
    for (int run = 0; run < 300; ++run) {
      Net w = fresh(100 + unsigned(run)); w.delay_max = 30 + uint32_t(w.rng() % 600);
      for (int step = 0; step < 6; ++step) {
        uint8_t o = 0xFF; for (uint8_t s = 0; s < 3; ++s) if (w.g[s].present && w.g[s].owns) o = s;
        if (o != 0xFF && w.g[o].out.step == kHandIdle) { uint8_t to = uint8_t((o + 1 + w.rng() % 2) % 3); if (w.g[to].present) w.Offer(o, to); }
        if (w.rng() % 10 == 0) { const uint8_t s = uint8_t(w.rng() % 3); if (w.g[s].owns) { w.g[s].present = false; for (uint8_t t = 0; t < 3; ++t) if (t != s && w.g[t].present && !w.g[t].owns) w.g[t].claim_at = w.now + ClaimDelayMs(t); } }
        w.Step(100 + w.rng() % 1500);
      }
      w.Step(10000);
      ++runs; if (!w.Settled(nullptr)) { ++unsettled; std::printf("      unsettled run %d:", run); for (const Game& x : w.g) std::printf(" [%s%s e%u o%u claim %llu]", x.present ? "" : "gone ", x.owns ? "owns" : "copy", unsigned(x.epoch), unsigned(x.owner), (unsigned long long)x.claim_at); std::printf("\n"); }
      if (w.both_ms > 3000) ++doubled;
    }
    std::printf("      hand-over: %u random runs, unsettled %u, two owners over 3 s %u\n", runs, unsettled, doubled);
    Check(unsettled == 0 && doubled == 0, "hand-over: random delays, offers and an owner leaving: one owner at the end, every copy under its epoch; two owners never over 3 s");

    // The registry's part: the old owner detaches its entry (no remove to its holders); the new owner adopts it
    // with the NetId and the new epoch, its holders' offers created: its next ticks stream them updates, no create.
    {
      Registry ra, rb; Scheduler sb; std::array<Member, kMaxSlots> mb{}; const float ref[3] = {0, 0, 0};
      const NetId net = MakeNetId(0, 0x41, 7);
      { Entry& e = ra.Add(net, 0, 0x41, kKindAmbient, 1, 1000); e.offer[1].created = e.offer[2].created = true; }
      Entry out{};
      const bool detached = ra.Detach(0x41, &out) && !ra.Find(0x41) && ra.TakeRemovals().empty() && out.net == net && !ra.Detach(0x41);
      Entry& b = rb.Adopt(net, 1, 0x99, kKindAmbient, 0, 4, uint16_t((1u << 0) | (1u << 2)), 5000);
      b.pos[0] = 5;
      for (int s : {0, 2}) { Member& m = mb[s]; m.present = m.in_area = true; m.budget = 570; m.interest_people = kInterestPeople; m.interest_cars = kInterestCars; m.was_in_area = true; m.last_digest = 5000; }
      unsigned creates = 0, updates = 0;
      uint64_t t = 5000;
      for (int k = 0; k < 10; ++k) { t += kTickMs; auto p = sb.Plan(t, rb, mb, ref); for (int s : {0, 2}) { creates += unsigned(p[s].creates.size()); updates += unsigned(p[s].updates.size()); } }
      Check(detached && rb.FindNet(net) == &b && b.epoch == 4 && b.owner == 1 && rb.Find(0x99) == &b && creates == 0 && updates >= 10,
            "hand-over: Detach leaves no remove; Adopt keeps the NetId at the new epoch and streams to the holders with no create");
    }
    // HandPick: 25 m nearer for 2 s, then at most every 5 s; urgent past 80 % of the ring at once.
    HandHyst hy; bool urgent = false; uint64_t t = 1000;
    HandView v{1, 30.0f, true};
    const bool not_nearer = HandPick(hy, 50.0f, &v, 1, kHandPeople, t, &urgent) == 0xFF;   // (only 20 m nearer)
    unsigned picked_at = 0;
    for (int i = 0; i < 40 && !picked_at; ++i) { t += 100; if (HandPick(hy, 60.0f, &v, 1, kHandPeople, t, &urgent) == 1) picked_at = unsigned(t - 1000); }
    hy.last = t;
    const bool spaced = HandPick(hy, 60.0f, &v, 1, kHandPeople, t + 4000, &urgent) == 0xFF && HandPick(hy, 60.0f, &v, 1, kHandPeople, t + 5100, &urgent) == 1;
    HandHyst hu; hu.last = t;
    const bool urgent_now = HandPick(hu, 73.0f, &v, 1, kHandPeople, t + 10, &urgent) == 1 && urgent;
    HandView far_v{1, 80.0f, true}, off_v{1, 10.0f, false};
    HandHyst hf;
    const bool keep_radius = HandPick(hf, 89.0f, &far_v, 1, kHandPeople, t, &urgent) == 0xFF && HandPick(hf, 89.0f, &off_v, 1, kHandPeople, t, &urgent) == 0xFF;
    Check(not_nearer && picked_at >= 2000 && picked_at <= 2200 && spaced, "HandPick: 25 m nearer for 2 s, then not again within 5 s");
    Check(urgent_now && keep_radius, "HandPick: urgent past 80 % of the ring (no hold, no spacing); never past the candidate's keep radius or to a viewer without the copy");
  }

  // The fades fix: the owner's camera-aware hold and the member's sight of a removed copy.
  {
    const float eye[3] = {0, 0, 0}, fwd[3] = {0, 0, 1};
    const float ahead[3] = {5, 0, 80}, behind[3] = {0, 0, -80}, close_behind[3] = {0, 0, -20}, past[3] = {0, 0, 125}, side[3] = {90, 0, 10};
    bool close_by = true;
    const bool seen = SeenFrom(eye, fwd, ahead, kInterestPeople, &close_by) && !close_by && !SeenFrom(eye, fwd, behind, kInterestPeople) &&
                      SeenFrom(eye, fwd, close_behind, kInterestPeople, &close_by) && close_by && !SeenFrom(eye, fwd, past, kInterestPeople) &&
                      SeenFrom(eye, fwd, past, kInterestCars) && !SeenFrom(eye, fwd, side, kInterestPeople);
    Check(seen, "SeenFrom: within 30 m of the eye any way, else in front (60 degrees) within the interest + 20 m");
    SeenHold h; uint64_t t = 1000;
    const bool fresh = HoldAsk(h, t) == kHoldNew && HoldActive(h, t);
    uint8_t a = kHoldOn; unsigned on = 0;
    while (t < 1000 + kHoldKeepMs + 500 && a != kHoldEnded) { t += 16; a = HoldAsk(h, t); on += a == kHoldOn; }
    const bool ended = a == kHoldEnded && t > 1000 + kHoldKeepMs && t <= 1000 + kHoldKeepMs + 16 && !HoldActive(h, t) && HoldAsk(h, t + 16) == kHoldOver;
    const uint64_t end = t;
    const bool cools = HoldAsk(h, t + 16 + kHoldGapMs + 1) == kHoldOver && !HoldActive(h, t + 16 + kHoldGapMs + 1) && !HoldStarts(h, t + 16 + 2 * kHoldGapMs + 2);
    const bool gap_restarts = HoldStarts(h, end + kHoldCoolMs) && HoldAsk(h, end + kHoldCoolMs) == kHoldNew && HoldActive(h, end + kHoldCoolMs);
    SeenHold g; HoldAsk(g, 5000);
    const bool lapses = HoldActive(g, 5000 + kHoldGapMs) && !HoldActive(g, 5000 + kHoldGapMs + 1);
    // The housekeeping's re-arm (L1): held 5 s, its asks pause 3 s, then a new ask: no new 20 s until 30 s after the last ask.
    SeenHold r; for (uint64_t u = 1000; u <= 6000; u += 500) HoldAsk(r, u);
    const bool no_rearm = HoldAsk(r, 9000) == kHoldOver && HoldAsk(r, 9500) == kHoldOver && HoldAsk(r, 6000 + kHoldCoolMs + 100) == kHoldNew;
    Check(fresh && on > 1000 && ended && cools && gap_restarts && lapses && no_rearm,
          "HoldAsk: a hold lasts 20 s from its start whatever the asks, then none until 30 s after its end (and the path left it 2 s)");
    const bool sight = SightOf(true, 59.0f, false) == kSightView && SightOf(false, 10.0f, false) == kSightBehind && SightOf(true, 100.0f, false) == kSightView &&
                       SightOf(true, 121.0f, false) == kSightFar && SightOf(true, 160.0f, true) == kSightView && SightOf(true, 171.0f, true) == kSightFar &&
                       SightOf(true, std::nanf(""), true) == kSightFar;
    Check(sight, "SightOf: faded only on the screen and within the interest + 20 m (people 120 m, cars 170 m), else at once");
    // (fades fix 2) The distance despawn's hold past 90 m: in front within 120 m only; the limits' holds within the caps.
    const float at[3] = {0, 0, 0}, look[3] = {0, 0, 1}, p110[3] = {0, 0, 110}, p_back[3] = {0, 0, -110}, p125[3] = {0, 0, 125};
    const bool cone = SeenFrom(at, look, p110, kInterestPeople) && !SeenFrom(at, look, p_back, kInterestPeople) && !SeenFrom(at, look, p125, kInterestPeople);
    Check(cone && kHoldAtLimit > 0 && kHoldAtLimit <= kHoldPeople && kCorpseLinger > 0 && kCorpseLingerMs >= kDefaultFadeMs,
          "fades fix 2: a person 90-120 m ahead of a member is held, not one behind or past 120 m; kHoldAtLimit within kHoldPeople");
    // (review_fades2 M1 / M2) Lingering bodies under the game's corpse manager's 5; the far holds leave the cull some of the 8.
    Check(kCorpseLinger < 5 && kHoldFar > 0 && kHoldFar < kHoldPeople,
          "review fixes: kCorpseLinger under the corpse manager's 5 bodies; kHoldFar leaves the off-screen cull holds");
    // (review_overnight M3) No hold in a cutscene or before a mission start's quiet time is over.
    Check(HoldsPaused(5000, true, 0) && HoldsPaused(5000, false, 10000) && !HoldsPaused(10000, false, 10000) && !HoldsPaused(5000, false, 0),
          "overnight fixes: no hold during a cutscene or within a mission start's quiet time, held again after it");
  }

  // 3.0: a player's climb (kEventClimb) and the sound pause watchdog.
  {
    ClimbInfo c; c.flag = 0xA5; c.state = 4; c.sub = 3; c.new_sub = 5; c.caller = kClimbJump;
    const ClimbInfo u = UnpackClimbInfo(PackClimbInfo(c));
    Check(u.flag == 0xA5 && u.state == 4 && u.sub == 3 && u.new_sub == 5 && u.caller == kClimbJump && UnpackClimbInfo(PackClimbInfo(ClimbInfo{0xFF, 15, 255, 15, 15})).sub == 255,
          "climb: kEventClimb a[7] packs flag, state / sub before, new sub-state and caller and back");
    Check(ClimbCallerOf(0x8216AC90u) == kClimbGround && ClimbCallerOf(0x823F6BB0u) == kClimbJump && ClimbCallerOf(0x824634A4u) == kClimbFall && ClimbCallerOf(0x823B0864u) == kClimbOther,
          "climb: the caller from ClimbStart's return address (ground / jump grab / fall grab / other)");
    const float p[3] = {10, 2, 5}, near[3] = {10.3f, 2.2f, 5.2f}, far[3] = {10.6f, 2, 5}, f[3] = {0, 0, 1}, f30[3] = {0.5f, 0, 0.866f}, f50[3] = {0.766f, 0, 0.643f};
    const float big[3] = {0, 0, 511}, nanv[3] = {std::nanf(""), 0, 0};
    Check(ClimbLedgeMatches(near, p, f, f30) && ClimbLedgeMatches(p, p, big, f) && !ClimbLedgeMatches(far, p, f, f) && !ClimbLedgeMatches(p, p, f, f50) &&
          !ClimbLedgeMatches(nanv, p, f, f) && !ClimbLedgeMatches(p, p, nanv, f),
          "climb: the ledge here is theirs within 0.5 m and ~37 degrees of facing (any length of direction; NaN never)");
    const float right_handed[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}, mirrored[9] = {-1, 0, 0, 0, 1, 0, 0, 0, 1};
    float r1[9], r2[9];
    ClimbRows(1.0f, right_handed, r1); ClimbRows(1.0f, mirrored, r2);
    auto triple = [](const float* r) { return (r[1] * r[5] - r[2] * r[4]) * r[6] + (r[2] * r[3] - r[0] * r[5]) * r[7] + (r[0] * r[4] - r[1] * r[3]) * r[8]; };
    Check(std::abs(r1[6] - std::sin(1.0f)) < 1e-5f && std::abs(r1[8] - std::cos(1.0f)) < 1e-5f && triple(r1) > 0.99f && triple(r2) < -0.99f && r1[0] == -r2[0],
          "climb: rows from the yaw face it and keep the character's own handedness");
    Check(ClimbDropsState(5) && ClimbDropsState(6) && ClimbDropsState(1) && ClimbDropsState(0) && !ClimbDropsState(4) && !ClimbDropsState(3) && !ClimbDropsState(8) &&
          ClimbDropsAction(0x20B) && ClimbDropsAction(0x218) && ClimbDropsAction(0x219) && ClimbDropsAction(0x2A) && ClimbDropsAction(0x2E) &&
          !ClimbDropsAction(0x20A) && !ClimbDropsAction(0x21A) && !ClimbDropsAction(0x20) && !ClimbDropsAction(0x24),
          "climb: while replayed, the owner's 0 / 1 / 5 / 6 states and actions 0x20B-0x219, 0x2A, 0x2E are the character's own");
    Check(ClimbOwnerEnds(6, 0xE, false) && ClimbOwnerEnds(0, 0, false) && ClimbOwnerEnds(1, 0, true) && !ClimbOwnerEnds(1, 0, false) && !ClimbOwnerEnds(6, 0xC, true) && !ClimbOwnerEnds(5, 9, true),
          "climb: the owner's climb ends on 6/E, 0/x, or 1/x after its drop (not the 1/0 between climb and drop)");
    const uint64_t s = 100000;
    const bool watch = ClimbHoldStep(s + 50, s, 0, 1) == kClimbAborted && ClimbHoldStep(s + 50, s, 0, 5) == kClimbHold && ClimbHoldStep(s + 200, s, 0, 5) == kClimbHold;
    const bool ends = ClimbHoldStep(s + 900, s, s + 800, 1) == kClimbHandBack && ClimbHoldStep(s + 900, s, s + 800, 6) == kClimbHold &&
                      ClimbHoldStep(s + 1200, s, s + 800, 6) == kClimbHandBack && ClimbHoldStep(s + 2499, s, 0, 5) == kClimbHold && ClimbHoldStep(s + 2500, s, 0, 5) == kClimbHandBack &&
                      ClimbHoldStep(s + 2499, s, 0, 6) == kClimbHold;
    Check(watch && ends, "climb hold: aborted when the character leaves state 5 within 150 ms; handed back after the owner's end (out of 5/6, or 400 ms on), or at 2.5 s");
    // review_memfair M3: a climb that ends on top (owner 5/5 -> 5/9 -> 1/0, no 6/x): handed back once the
    // character's own state is out of 5 / 6, not at the 2.5 s cap; the owner's 5/9 arriving after that is the tail.
    {
      uint64_t owner_end = 0; bool dropped = false;
      const uint32_t owner[3][2] = {{5, 5}, {5, 9}, {1, 0}};
      for (const auto& o : owner) { if (!owner_end && ClimbOwnerEnds(o[0], o[1], dropped)) owner_end = s + 900; if (o[0] == 6) dropped = true; }
      const bool holds = ClimbHoldStep(s + 600, s, owner_end, 5) == kClimbHold;
      const bool back = owner_end == 0 && ClimbHoldStep(s + 1050, s, owner_end, 1) == kClimbHandBack;
      const uint64_t tail = s + kClimbCapMs;
      const bool tail_ok = ClimbTailStep(s + 1100, tail, 5, 9, false) == kTailDrop && ClimbTailStep(s + 1100, tail, 6, 0xC, false) == kTailDrop &&
                           ClimbTailStep(s + 1150, tail, 1, 0, false) == 0 && ClimbTailStep(s + 1200, tail, 6, 0xE, true) == (kTailDrop | kTailOver) &&
                           ClimbTailStep(s + 1200, tail, 0, 0, false) == kTailOver && ClimbTailStep(tail, tail, 5, 9, false) == kTailOver &&
                           ClimbTailStep(s + 1100, 0, 5, 9, false) == 0;
      Check(holds && back && tail_ok, "climb hold: a climb ending on top (5/x -> 1/0, no drop) hands back when the character is out of 5 / 6; the owner's later 5/x / 6/x are its tail");
    }
    SoundWatch w;
    const bool quiet = !SoundStuck(w, 1000, 1, 0) && !SoundStuck(w, 5999, 1, 0) && SoundStuck(w, 6000, 1, 0);
    SoundWatch w2; SoundStuck(w2, 1000, 1, 0);
    const bool reason_resets = !SoundStuck(w2, 4000, 1, kSoundCutscene) && !SoundStuck(w2, 8000, 1, 0) && !SoundStuck(w2, 12999, 1, 0) && SoundStuck(w2, 13000, 1, 0);
    SoundWatch w3; SoundStuck(w3, 0, 2, 0);
    const bool zero_resets = !SoundStuck(w3, 3000, 0, 0) && !SoundStuck(w3, 6000, 1, 0) && !SoundStuck(w3, 10999, 1, 0) && SoundStuck(w3, 11000, 1, 0) && !SoundStuck(w3, 20000, 1, kSoundOwed);
    Check(quiet && reason_resets && zero_resets, "sound watchdog: stuck only after 5 s in a row above 0 with none of the game's reasons; a reason or 0 starts over");
    uint64_t since = 0;
    const bool waits = CutsceneEndWaits(kCutsceneBeginStep, false, 5000, since) && CutsceneEndWaits(kCutsceneBeginStep, true, 5500, since) &&
                       !CutsceneEndWaits(kCutsceneBeginStep, true, 6000, since) && since == 0;
    uint64_t since2 = 0;
    const bool goes = !CutsceneEndWaits(0, true, 5000, since2) && !CutsceneEndWaits(0x821F9700u, true, 5000, since2) && CutsceneEndWaits(0, false, 0, since2) &&
                      !CutsceneEndWaits(0, true, 10, since2) && since2 == 0;
    Check(waits && goes, "cutscene end: waits while the start's deferred begin is pending (or not running yet), at most 1 s");
  }

  // ---- Protocol 12: the compact pose (WPP2), WAP1 and the relay's tiers ----
  {
    auto rows_of = [](float yaw, float sign, float r[9]) {   // (coop_bot's RowsFromYaw: sign -1 is the mirrored convention)
      const float fx = std::sin(yaw), fz = std::cos(yaw);
      r[0] = sign * fz; r[1] = 0; r[2] = -sign * fx; r[3] = 0; r[4] = 1; r[5] = 0; r[6] = fx; r[7] = 0; r[8] = fz;
    };
    auto max_diff = [](const float* a, const float* b, int n) { float m = 0; for (int i = 0; i < n; ++i) m = std::max(m, std::fabs(a[i] - b[i])); return m; };
    PoseData p;
    p.sequence = 1234; p.time = 987654; p.pos[0] = -2120.37f; p.pos[1] = 4.51f; p.pos[2] = 3371.9f;
    rows_of(0.7f, -1, p.rows);
    p.eye[0] = p.pos[0] - 2.1f; p.eye[1] = p.pos[1] + 1.6f; p.eye[2] = p.pos[2] - 2.9f;
    // A pitched camera (right, up, forward), mirrored as the game's can be.
    const float pitch = 0.3f, yaw = 2.1f;
    const float fwd[3] = {std::cos(pitch) * std::sin(yaw), -std::sin(pitch), std::cos(pitch) * std::cos(yaw)};
    const float right[3] = {std::cos(yaw), 0, -std::sin(yaw)};
    const float up[3] = {fwd[1] * right[2] - fwd[2] * right[1], fwd[2] * right[0] - fwd[0] * right[2], fwd[0] * right[1] - fwd[1] * right[0]};
    for (int i = 0; i < 3; ++i) { p.cam[i] = -right[i]; p.cam[3 + i] = up[i]; p.cam[6 + i] = fwd[i]; }
    p.flags = 0x12345678u & ~0x10u; p.appearance = 7;
    uint8_t buf[kPoseInCar];
    const size_t n = EncodePose(p, buf);
    PoseData q;
    const bool dec = DecodePose(buf, n, q);
    const bool foot = n == 48 && dec && q.sequence == p.sequence && q.time == p.time && !std::memcmp(q.pos, p.pos, sizeof(p.pos)) && q.flags == p.flags && q.appearance == 7 && q.vehicle == -1;
    const bool rot = max_diff(q.rows, p.rows, 9) < 1e-3f && Det3(q.rows) < 0 && (buf[kPoseFormAt] & kFormRowsMirror);
    const bool cam = max_diff(q.cam, p.cam, 9) < 1e-3f && (buf[kPoseFormAt] & (kFormCam | kFormCamMirror)) == (kFormCam | kFormCamMirror) && max_diff(q.eye, p.eye, 3) < 0.011f;
    Check(foot && rot && cam, "WPP2 on foot: 48 B, position and flags exact, rows / camera within 1e-3 (mirrored ones too), eye within 1 cm");
    // No camera, no eye (0 = none, as before); an eye over 655 m away goes as none.
    PoseData r = p; std::memset(r.cam, 0, sizeof(r.cam)); std::memset(r.eye, 0, sizeof(r.eye)); rows_of(1.0f, 1, r.rows);
    PoseData rq; const bool none = DecodePose(buf, EncodePose(r, buf), rq) && !(buf[kPoseFormAt] & (kFormCam | kFormEye | kFormRowsMirror)) && !rq.cam[0] && !rq.cam[8] && !rq.eye[0] && !rq.eye[1] && Det3(rq.rows) > 0;
    r.eye[0] = r.pos[0] + 700; r.eye[1] = r.pos[1]; r.eye[2] = r.pos[2];
    const bool far_eye = DecodePose(buf, EncodePose(r, buf), rq) && !(buf[kPoseFormAt] & kFormEye) && !rq.eye[0];
    Check(none && far_eye, "WPP2: no camera / no eye stay none; an eye over 655 m from the player goes as none");
    // In a car: 75 B, the car's offset, rows, velocity and spin.
    PoseData c = p; c.vehicle = 117; c.seat = 1; c.vowner = 11; c.vid = 0x8A001234u;
    c.vpos[0] = c.pos[0] + 0.8f; c.vpos[1] = c.pos[1] - 0.4f; c.vpos[2] = c.pos[2] + 1.2f;
    rows_of(-2.5f, -1, c.vrows); c.vvel[0] = 31.25f; c.vvel[1] = -0.5f; c.vvel[2] = -12.0f; c.vang[0] = 0.25f; c.vang[1] = 1.5f; c.vang[2] = -9.0f;
    PoseData cq;
    const size_t cn = EncodePose(c, buf);
    const bool car = cn == 75 && DecodePose(buf, cn, cq) && cq.vehicle == 117 && cq.seat == 1 && cq.vowner == 11 && cq.vid == 0x8A001234u &&
                     max_diff(cq.vpos, c.vpos, 3) < 0.011f && max_diff(cq.vrows, c.vrows, 9) < 1e-3f && Det3(cq.vrows) < 0 &&
                     max_diff(cq.vvel, c.vvel, 3) < 0.005f && std::fabs(cq.vang[0] - 0.25f) < 0.04f && std::fabs(cq.vang[1] - 1.5f) < 0.04f && cq.vang[2] < -7.8f;
    // A car over 655 m from its passenger (never in the game): at the player.
    PoseData cf = c; cf.vpos[0] = c.pos[0] + 900;
    const bool car_far = DecodePose(buf, EncodePose(cf, buf), cq) && !std::memcmp(cq.vpos, cq.pos, sizeof(cq.pos));
    Check(car && car_far, "WPP2 in a car: 75 B, car within 1 cm / 1e-3 / 1/128 m/s, spin clamped to 7.9 rad/s; a car over 655 m away goes at the player");
    // Malformed: a size other than 48 / 75 or one that disagrees with kFormCar, another magic, a bad seat / owner, a NaN position.
    uint8_t bad[kPoseInCar + 1]{};
    EncodePose(c, bad);
    bool refused = !DecodePose(bad, 48, cq) && !DecodePose(bad, 76, cq) && !DecodePose(bad, 74, cq);
    EncodePose(p, bad); refused = refused && !DecodePose(bad, 75, cq);
    bad[3] = '1'; refused = refused && !DecodePose(bad, 48, cq);
    EncodePose(c, bad); bad[48 + 1] = 0xC9; refused = refused && !DecodePose(bad, 75, cq);   // (vowner 12)
    EncodePose(c, bad); bad[48 + 1] = 0x09; refused = refused && !DecodePose(bad, 75, cq);   // (seat 9)
    PoseData nan = p; nan.pos[1] = std::nanf(""); refused = refused && EncodePose(nan, bad) == 0;
    EncodePose(p, bad); const float inf = INFINITY; std::memcpy(bad + kPosePosAt + 4, &inf, 4); refused = refused && !DecodePose(bad, 48, cq);
    Check(refused, "WPP2: other sizes, a size that disagrees with kFormCar, another magic, seat >= 8, owner >= 12 and a NaN / inf position refused");
    // WAP1.
    uint8_t ab[256]; const uint8_t look[2] = {0x01, 0x02};
    const size_t an = WriteAppearance(3, "StyleTest_PC", look, 2, ab, sizeof(ab));
    uint8_t ver = 0; std::string model; std::vector<uint8_t> lk;
    const bool app = an == 21 && ReadAppearance(ab, an, ver, model, lk) && ver == 3 && model == "StyleTest_PC" && lk == std::vector<uint8_t>{1, 2};
    ab[4] = 0; const bool app_bad = !ReadAppearance(ab, an, ver, model, lk) && (ab[4] = 3, !ReadAppearance(ab, an - 1, ver, model, lk)) &&
                                    WriteAppearance(1, std::string(64, 'x'), look, 2, ab, sizeof(ab)) == 0 && WriteAppearance(1, "m", look, 2, ab, 9) == 0;
    Check(app && app_bad, "WAP1: 21 B for StyleTest_PC and a 2-byte look, round trip; version 0, a short message, a 64-char model and a full buffer refused");
    // review_bandwidth L1: an older WAP1 (up to 8 back, wrapping) is stale; anything with none held, further back (a restart) or newer is taken.
    const bool stale = AppearanceStale(4, 5) && AppearanceStale(255, 2) && AppearanceStale(1, 9) && !AppearanceStale(6, 5) && !AppearanceStale(5, 5) &&
                       !AppearanceStale(3, 0) && !AppearanceStale(1, 40) && !AppearanceStale(2, 255);
    Check(stale, "WAP1 order: 1-8 versions back (wrapping) dropped; newer, the same, none held, or further back (the sender started over) taken");
    // review_bandwidth M2: a scaled / skewed camera is sent orthonormalised; a degenerate one isn't (noted); a usable camera with no eye isn't sent (noted).
    PoseData sc = p;
    for (int i = 0; i < 3; ++i) { sc.cam[i] = 0.5f * p.cam[i]; sc.cam[3 + i] = 0.8f * p.cam[3 + i] + 0.1f * p.cam[6 + i]; sc.cam[6 + i] = 0.9f * p.cam[6 + i]; }
    PoseEncodeNote note; PoseData scq;
    const bool scaled = DecodePose(buf, EncodePose(sc, buf, &note), scq) && (buf[kPoseFormAt] & kFormCam) && !note.cam_refused && max_diff(scq.cam + 6, p.cam + 6, 3) < 2e-3f &&
                        max_diff(scq.cam, p.cam, 3) < 2e-3f && std::fabs(Det3(scq.cam) + 1) < 3e-3f;
    PoseData dg = p; for (int i = 0; i < 3; ++i) dg.cam[3 + i] = 0.05f * p.cam[3 + i];
    PoseEncodeNote dn; PoseData dq;
    const bool degenerate = DecodePose(buf, EncodePose(dg, buf, &dn), dq) && !(buf[kPoseFormAt] & kFormCam) && dn.cam_refused && !dq.cam[0] && !dq.cam[8];
    PoseData fe = p; fe.eye[0] = fe.pos[0] + 700;
    PoseEncodeNote fn; PoseData fq;
    const bool no_eye = DecodePose(buf, EncodePose(fe, buf, &fn), fq) && !(buf[kPoseFormAt] & (kFormCam | kFormEye)) && fn.cam_no_eye && !fn.cam_refused && !fq.cam[0] && !fq.cam[4] && !fq.eye[0];
    Check(scaled && degenerate && no_eye, "camera: a scaled / skewed one goes orthonormalised (forward kept, mirror kept); a row under 0.1 isn't sent (counted); an eye past 655 m sends no camera (counted)");
    // review_bandwidth L2 / L3 / L4: all-zero rows go as identity; a malformed rotation decodes as a rotation; seat >= 8 or owner >= 12 goes on foot.
    PoseData z = p; std::memset(z.rows, 0, sizeof(z.rows)); PoseData zq;
    const bool zero_rows = DecodePose(buf, EncodePose(z, buf), zq) && std::fabs(zq.rows[0] - 1) < 1e-3f && std::fabs(zq.rows[4] - 1) < 1e-3f && std::fabs(zq.rows[8] - 1) < 1e-3f;
    EncodePose(p, bad); const int16_t big3[3] = {int16_t(32764 | 3), 32767, 32767}; std::memcpy(bad + offsetof(PosePacket, rows), big3, sizeof(big3)); bad[kPoseFormAt] &= uint8_t(~kFormRowsMirror);
    PoseData mq; bool unit = DecodePose(bad, 48, mq) && std::fabs(Det3(mq.rows) - 1) < 1e-3f;
    for (float v : mq.rows) unit = unit && std::fabs(v) <= 1.001f;
    PoseData s9 = c; s9.seat = 9; PoseData o12 = c; o12.vowner = 12; PoseData sq;
    const bool remap = EncodePose(s9, buf) == 48 && DecodePose(buf, 48, sq) && sq.vehicle == -1 && EncodePose(o12, buf) == 48;
    Check(zero_rows && unit && remap, "rows: all zero go as identity (not a roll); three over-long components decode renormalised; a seat >= 8 / owner >= 12 goes on foot (no car tail)");
  }
  {
    // The tiers: 4 senders at 20 / 100 / 300 / 700 m from a target (slot 0), 30 a second for 10 s.
    RelayTiers t;
    auto pose = [](float x, float z, uint32_t time, uint32_t flags, bool car, float speed, uint8_t out[kPoseInCar]) {
      PoseData p; p.sequence = time / 33 + 1; p.time = time; p.pos[0] = x; p.pos[2] = z; p.rows[0] = p.rows[4] = p.rows[8] = 1; p.flags = flags;
      if (car) { p.vehicle = 3; p.vrows[0] = p.vrows[4] = p.vrows[8] = 1; std::memcpy(p.vpos, p.pos, sizeof(p.pos)); p.vvel[2] = speed; }
      return EncodePose(p, out);
    };
    uint8_t b[kPoseInCar];
    const float dist[4] = {20, 100, 300, 700};
    int got[4] = {};
    t.Note(0, b, pose(0, 0, 1000, 0, false, 0, b));
    for (uint32_t time = 1000; time < 11000; time += 33)
      for (int s = 0; s < 4; ++s) { const size_t len = pose(dist[s], 0, time + uint32_t(s), 0, false, 0, b); got[s] += t.Pass(1 + s, 0, b, len); }
    auto near_hz = [](int count, double hz) { return count >= hz * 10 * 0.9 && count <= hz * 10 * 1.1; };
    Check(near_hz(got[0], 30.3) && near_hz(got[1], 10) && near_hz(got[2], 5) && near_hz(got[3], 3),
          Fmt("tiers: senders at 20 / 100 / 300 / 700 m reach the target at 30 / 10 / 5 / 3 Hz +-10 %% (%.0f / %.0f / ", got[0] / 10.0, got[1] / 10.0) + Fmt("%.0f / %.0f Hz)", got[2] / 10.0, got[3] / 10.0));
    // Passed at once at every tier: a state bit flip, getting in a car, a 50 m jump; a fast car one tier nearer.
    bool flip = true, car_in = true, jump = true;
    for (int s = 0; s < 4; ++s) {
      const uint32_t at = 20000 + 1000 * uint32_t(s);
      t.Pass(1 + s, 0, b, pose(dist[s], 0, at, 0, false, 0, b));
      flip = flip && t.Pass(1 + s, 0, b, pose(dist[s], 0, at + 33, 1u << 7, false, 0, b));
      car_in = car_in && t.Pass(1 + s, 0, b, pose(dist[s], 0, at + 66, 1u << 7, true, 0, b));
      jump = jump && t.Pass(1 + s, 0, b, pose(dist[s] + 30, 40, at + 99, 1u << 7, true, 0, b));
    }
    // A flag outside the state bits (the gun in hand) is not forced.
    t.Pass(4, 0, b, pose(700, 0, 30000, 0, false, 0, b));
    const bool gun_waits = !t.Pass(4, 0, b, pose(700, 0, 30033, 5u << 8, false, 0, b));
    // A car at 25 m/s at 100 m: every pose (tier 0), at 300 m 10 a second.
    int fast100 = 0, fast300 = 0;
    for (uint32_t time = 40000; time < 41000; time += 33) {
      fast100 += t.Pass(2, 0, b, pose(100, 0, time, 0, true, 25, b));
      fast300 += t.Pass(3, 0, b, pose(300, 0, time, 0, true, 25, b));
    }
    Check(flip && car_in && jump && gun_waits && fast100 >= 29 && fast300 >= 9 && fast300 <= 11, "tiers: a state bit flip, a car entered and a 50 m jump pass at once at every tier (not the gun bits); a car over 20 m/s goes one tier nearer");
    // Hysteresis: at 65 m tier 0 stays, at 71 tier 1; back at 55 tier 1 stays, at 49 tier 0.
    const bool hyst = RelayTier(65, 0) == 0 && RelayTier(71, 0) == 1 && RelayTier(55, 1) == 1 && RelayTier(49, 1) == 0 && RelayTier(65, -1) == 1 && RelayTier(900, 0) == 3 && RelayTier(10, 3) == 0 &&
                      RelayTier(165, 2) == 2 && RelayTier(159, 2) == 1;
    // A target not seen yet gets everything; Forget starts a slot over.
    RelayTiers u; int all = 0;
    for (uint32_t time = 1000; time < 2000; time += 33) all += u.Pass(1, 5, b, pose(900, 0, time, 0, false, 0, b));
    u.Note(5, b, pose(0, 0, 2000, 0, false, 0, b));
    int thinned = 0; for (uint32_t time = 2000; time < 3000; time += 33) thinned += u.Pass(1, 5, b, pose(900, 0, time, 0, false, 0, b));
    u.Forget(5); int again = 0; for (uint32_t time = 3000; time < 4000; time += 33) again += u.Pass(1, 5, b, pose(900, 0, time, 0, false, 0, b));
    // Not a pose: always passed.
    const uint8_t other[8] = {'W', 'W', 'S', '1'};
    Check(hyst && all == 31 && thinned <= 4 && again == 31 && u.Pass(1, 5, other, sizeof(other)), "tiers: 10 m hysteresis both ways; everything to a target whose position isn't known (also after Forget); other messages pass");
    // review_bandwidth L5: in a car at 700 m, another seat, another car handle or another car type passes at once; the same car waits.
    {
      RelayTiers v; v.Note(0, b, pose(0, 0, 1000, 0, false, 0, b));
      auto car_pose = [](uint32_t time, uint32_t seat, uint32_t vid, int32_t vehicle, uint8_t out[kPoseInCar]) {
        PoseData p; p.sequence = time / 33 + 1; p.time = time; p.pos[0] = 700; p.rows[0] = p.rows[4] = p.rows[8] = 1; p.flags = 1u << 4;
        p.vehicle = vehicle; p.seat = seat; p.vid = vid; p.vrows[0] = p.vrows[4] = p.vrows[8] = 1; std::memcpy(p.vpos, p.pos, sizeof(p.pos));
        return EncodePose(p, out);
      };
      v.Pass(1, 0, b, car_pose(2000, 0, 77, 3, b));
      const bool same_waits = !v.Pass(1, 0, b, car_pose(2033, 0, 77, 3, b));
      const bool seat_goes = v.Pass(1, 0, b, car_pose(2066, 1, 77, 3, b));
      const bool vid_goes = v.Pass(1, 0, b, car_pose(2099, 1, 78, 3, b));
      const bool type_goes = v.Pass(1, 0, b, car_pose(2132, 1, 78, 4, b));
      const bool then_waits = !v.Pass(1, 0, b, car_pose(2165, 1, 78, 4, b));
      Check(same_waits && seat_goes && vid_goes && type_goes && then_waits, "tiers: a seat swap, another car handle or car type while in a car passes at once at 700 m; the same car waits its tier");
    }
    // review_memory_steps L7: a 48-byte pose whose form says car (a bad sender) has no tail: read as on foot, nothing past the message.
    {
      RelayTiers m; m.Note(0, b, pose(0, 0, 1000, 0, false, 0, b));
      m.Pass(1, 0, b, pose(700, 0, 2000, 0, false, 0, b));
      pose(700, 0, 2033, 0, true, 30, b);
      std::vector<uint8_t> short_car(b, b + kPoseFoot);   // (exactly 48 bytes on the heap)
      const bool is_car_form = (short_car[kPoseFormAt] & kFormCar) != 0;
      const bool waits = !m.Pass(1, 0, short_car.data(), short_car.size());
      Check(is_car_form && waits, "tiers: a 48-byte pose with the car form is relayed as on foot (no car tail read past the message)");
    }
    // review_bandwidth L6: the fast-car rule with hysteresis: over 22 m/s nearer, back only under 18 (a car hovering at 20 keeps its tier).
    {
      RelayTiers h; h.Note(0, b, pose(0, 0, 1000, 0, false, 0, b));
      auto count = [&](float speed, uint32_t from) { int n = 0; for (uint32_t time = from; time < from + 1000; time += 33) n += h.Pass(1, 0, b, pose(300, 0, time, 0, true, speed, b)); return n; };
      const int slow20 = count(20, 2000);   // (from slow: 20 isn't fast: 5 a second)
      const int fast25 = count(25, 4000);   // fast: tier 1, 10 a second
      const int still20 = count(20, 6000);  // still fast at 20
      const int slow17 = count(17, 8000);   // under 18: back to 5
      Check(slow20 <= 6 && fast25 >= 9 && still20 >= 9 && slow17 <= 6, Fmt("tiers: fast-car hysteresis at 300 m (20 m/s from slow %.0f, 25 %.0f, 20 after 25 %.0f, ", slow20, fast25, still20) + Fmt("17 %.0f a second)", slow17));
    }
  }

  std::printf("%s: %d failure(s)\n", failures ? "FAILED" : "ALL PASSED", failures);
  return failures ? 1 : 0;
}
