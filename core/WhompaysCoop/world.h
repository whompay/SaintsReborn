// Co-op v2 world stream (2.2): the records, the owner's registry and the
// per-member scheduler, modelled on the game's own multiplayer (report Q 2.1:
// remote_object_create / destroy reliable, update unreliable, one priority
// list per connection cut to a byte budget by 82353DB0 under 8235AE50).
//
// The owner (the host in 2.2) keeps a Registry of everything its game
// simulates, fed by the create / destroy hooks in coop.cpp. Every 50 ms per
// member, Scheduler::Plan turns it into creates (WWC1, reliable, at most 4 a
// tick), updates (WWS1, unreliable, within the member's byte budget) and
// removes (WWR1, reliable, only to members the id was created on), plus a
// set digest (WWD1) every 10 s. Members answer with WWA1 (what they hold,
// what they declined); the owner never removes for pool reasons.
//
// Co-op 3 (phase 1a, protocol 10): every object has a NetId (origin slot,
// origin handle, origin join nonce) for its whole life, apart from the handle
// a game holds it by (Entry.local) and from its owner (Entry.owner, with an
// ownership epoch). WWC1 carries all of them, WWR1 the owner and epoch, WWA1
// the owner it is for. Records (WWS1, WWR1, WWD1, WWT1, crews, seats) still
// name an object by the owner's handle in the owner's namespace (the header's
// or the record's owner): in 1a the host is the only owner and origin ==
// owner, handle == local.
// Phase 2a (world_mode = shared): every game owns its registry and every
// other game is a member of it; what a member is offered goes per object by
// its eye (InInterest: people 100 m, cars 150 m, + 20 m), with no areas and no
// WOM1.
//
// No game code here: time, positions and camera eyes are inputs; the same
// file runs in the mod and in tests/world_test.cpp. Plan is deterministic
// for equal inputs (map order, full tie-breaks).
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <map>
#include <string>
#include <vector>

namespace coopworld {

constexpr int kMaxSlots = 12;
constexpr int kMaxMessage = 1170 - 16 - 10;   // coopnet::kMaxMessage (one packet's payload)

// Create kinds (WWC1): cars below 16, people from 16.
enum : uint8_t {
  kKindTraffic = 1, kKindParked = 2, kKindResponseCar = 3, kKindSpecialCar = 4, kKindScriptCar = 5,
  kKindAmbient = 16, kKindCrew = 17, kKindResponseUnit = 18, kKindHood = 19, kKindSpecialPerson = 20, kKindMissionPerson = 21,
};
inline bool IsCar(uint8_t kind) { return kind < 16; }

// PedRec / SeatedRec flags (byte 11 of both records: a reader peeks it to tell them apart).
enum : uint8_t {
  kPedSign = 1, kPedMission = 2, kPedDead = 4, kPedCrouch = 8, kPedRagdoll = 16, kPedBurning = 32, kPedHidden = 64, kPedSeated = 128,
};
// PedRec / SeatedRec weapon byte (Package C): the gun in hand as weapon table
// index + 1 (0 none; an index past 62 goes as 0), the gun raised (+216
// 0x80000000, the 2.5 s hold 82449708 sets), and a FireExt after the record.
enum : uint8_t { kWeaponGun = 0x3F, kWeaponRaised = 0x40, kWeaponFire = 0x80 };
// FireExt.fire: the rounds the person fired (the owner's running count mod
// 16), trigger held (a burst gun that fired in the last 250 ms), the last
// round a blank (82470A50 r6), aim valid (the last round's first pellet's hit
// point), drive-by mode (seated, +516 == 3).
enum : uint8_t { kFireCount = 0x0F, kFireHeld = 0x10, kFireBlank = 0x20, kFireAim = 0x40, kFireDriveBy = 0x80 };
// CarRec.damage bits 6-7 (Package C): the brake lights (the 10-frame brake
// average v+53500 over 0.2, the tail lights' own test) and reversing ((int8)
// E+180, 82535C00). CarRec.drive: the steering E+412 + E+416 in 6 bits (two's
// complement, x 31), the horn (825241F0 ran in the last 150 ms) and the
// engine bit v+53976 0x08000000 (both lights need it).
enum : uint8_t { kCarBrake = 0x40, kCarReverse = 0x80 };
enum : uint8_t { kDriveSteer = 0x3F, kDriveHorn = 0x40, kDriveEngine = 0x80 };
// WWR1 reasons (1 "out of interest": 3.0, world_mode = shared, per object; 2 "trimmed" never did).
enum : uint8_t { kRemoveGone = 0, kRemoveInterest = 1, kRemoveAreaLeft = 3 };
// Registry entry flags.
enum : uint8_t {
  kEntryRest = 1,      // parked / resting car or a seated person: 2 Hz unless something changed
  kEntryInactive = 2,  // not streamed this frame (chunk unloaded, +120 0x04000000 clear); counted
  kEntryExcluded = 4,  // never streamed (train car, named story character); counted in the owner line
  kEntryPlayerCar = 8, // a car a player sits in or gets into (authority: that player, the owner's own included): its pose carries it, no record
};

constexpr uint8_t kSolo = 0xFF;             // OwnerMapRec.area: the slot runs its own world
constexpr uint8_t kAuthorityPlayer = 0x80;  // CreateRec.authority: a player's pose carries the car (that player's slot in the low bits; the owner's own player too)

#pragma pack(push, 1)
// WWS1: unreliable, unicast per member; then n_cars CarRec and n_people PedRec / SeatedRec.
struct UpdateHeader {
  char magic[4];      // WWS1
  uint8_t owner;      // owner slot (the records' namespace)
  uint16_t epoch;     // OwnerMap epoch
  uint32_t time;      // owner clock, ms
  float ref[3];       // positions are offsets from here (the owner's stream point 82485DB0)
  uint8_t n_cars, n_people;
};
struct CarRec {
  uint32_t id;        // the owner's handle
  uint16_t model;     // vehicle info index
  int16_t pos[3];     // 2 cm from ref (+-655 m)
  int16_t rot[3];     // smallest-three quaternion, 1/32767; rot[0] low 2 bits = the dropped component
  int16_t vel[3];     // 1/128 m/s
  int8_t ang[3];      // 1/16 rad/s
  uint8_t damage;     // CarDamage (bits 0 fire, 1 smoke, 2 wrecked, 5 heavy smoke) | CarSiren (3 lights, 4 siren) | kCarBrake | kCarReverse
  uint8_t hp;         // CarHealth 0-255
  uint8_t drive;      // steering (kDriveSteer) | kDriveHorn | kDriveEngine
};
struct PedRec {         // a person on foot
  uint32_t id;
  int16_t pos[3];     // 2 cm from ref
  uint8_t yaw;        // heading, 256 steps per turn (1.4 degrees)
  uint8_t flags;      // kPed* (kPedSeated clear)
  uint8_t stance;     // +576, the state playing on layer 0 (0-226: stand / crouch / walk ready, fire), 0xFF none (protocol 9; was +516 << 4 | +520, never read)
  uint8_t anim;       // +952
  uint16_t action;    // +756
  int8_t vel[2];      // x / z, 1/16 m/s
  uint8_t team;       // +232
  uint8_t weapon;     // kWeapon*: gun in hand index + 1 (0 none), raised, a FireExt follows
};
struct SeatedRec {      // a person in a car: one id with car + seat
  uint32_t id, car;
  uint8_t seat, team, weapon;
  uint8_t flags;      // kPed* with kPedSeated set (byte 11, as PedRec)
};
// After a PedRec / SeatedRec whose weapon has kWeaponFire: sent with every
// record of a person who fired in the last second (so the last count and the
// trigger's release always arrive); never in a WWC1's first record.
struct FireExt {
  uint8_t fire;       // kFire*
  int16_t aim[3];     // 2 cm from the header's ref (QPos)
};
// WWC1: reliable, unicast, one per object per member. Tail: name[name_len],
// look[look_len] (8238B0B8 block), paint[paint_len] (CarLookPacket content),
// u32 crew[n_crew], then the first full record (CarRec / PedRec / SeatedRec).
struct CreateRec {
  char magic[4];      // WWC1
  uint32_t id, time;
  float ref[3];       // the first record's positions are offsets from here (the owner's stream point at the create)
  uint8_t kind, authority;
  uint16_t model;     // vehicle info index or people model id
  uint8_t team;
  uint8_t name_len, look_len;   // <= kMaxName / kMaxLook
  uint16_t paint_len;           // <= kMaxPaint
  uint32_t car;       // seated people: the car id (0 on foot)
  uint8_t seat, n_crew;         // cars: the ids of the crew that follow
  // (3.0, protocol 10) id is the owner's handle (its records' name); the object's NetId and owner:
  uint8_t owner;      // the slot that simulates it and sent this (the records' namespace)
  uint8_t origin;     // NetId: the slot that made it
  uint16_t nonce;     // NetId: the origin's join nonce
  uint32_t net;       // NetId: the origin's handle (== id while origin == owner, all of 1a)
  uint16_t own_epoch; // ownership epoch (+1 a hand-over, phase 3; 0 in 1a): not the OwnerMap epoch
};
// WWR1: reliable, only to members the id was created on.
struct RemoveRec {
  char magic[4];      // WWR1
  uint32_t id, time;
  uint16_t fade_ms;   // fade then destroy (0: at once)
  uint8_t reason;     // kRemove*
  uint8_t owner;      // (3.0) id's namespace: the owner that sent it
  uint16_t own_epoch; // (3.0) the ownership epoch it was removed at
};
// WWA1: unreliable, member -> owner, once a second and on change. Tail: u16 models[n_models], u32 declined[n_declined].
struct AdmitRec {
  char magic[4];      // WWA1
  uint16_t cars_held, cars_cap, people_held, people_cap;
  uint64_t chunks;    // bit i: chunk list index i loaded here
  uint8_t dropped_model, dropped_chunk;   // 10 s counts
  uint8_t near_cars, near_people;         // copies shown within 50 m of the member's camera (the owner's World match line)
  uint8_t n_models;   // people models resident here
  uint8_t n_declined; // declined or evicted since the last report (<= kMaxDeclined)
  uint8_t owner;      // (3.0) the owner it is for (the message's target; its declined ids are that owner's handles)
  // (3.0 phase 2c, shared; 0 in area mode) The member's share for this owner
  // of what it takes in (bytes a second, ShareOut by the copies it holds of
  // each owner), the bytes of this owner's world messages it received since
  // its last WWA1 to it (the owner's backpressure), and how much of the
  // interest radii it has room for (percent; its pools run low: design 12/(13)).
  uint16_t budget_bps;
  uint32_t received;
  uint8_t interest_people_pct, interest_cars_pct;
};
// WWD1: reliable, every 10 s per member: the ids the member was offered, sorted. Tail: u32 ids[n].
struct DigestRec {
  char magic[4];      // WWD1
  uint8_t owner, part, parts, n;
  uint32_t time;      // owner clock
  uint32_t hash;      // DigestHash of the whole set
};
// WOM1: session host -> all (3.0: members take it from no one else; not sent in shared mode), reliable on change and once a second unreliable.
struct OwnerMapRec {
  char magic[4];      // WOM1
  uint16_t epoch;     // changes only when a slot's owner changes
  struct { uint8_t area, owner; } slots[kMaxSlots];   // area kSolo: its own world
};
// WWT1 (2.4, the sync checker): host -> one member, unreliable, at most every 3 s per member.
// Then n_recs TruthRec, n_items TruthItem, n_props TruthProp. Positions are offsets from center (the member's
// character as the host's pose has it), 2 cm (QPos). An older member ignores the type.
struct TruthHeader {
  char magic[4];      // WWT1
  uint8_t owner;      // the owner it samples (0 in 2.x and in 3.0's area mode; the member takes it only from that slot)
  uint8_t n_recs, n_items, n_props;
  uint16_t epoch;     // OwnerMap epoch (one from before the member's area join is dropped)
  uint16_t seq;       // per member, +1 a sample (the member counts lost ones)
  uint32_t time;      // owner clock, ms: when it was read
  float center[3];
  uint8_t radius;     // metres (60)
  uint8_t flags;      // kTruthComplete: every registry entry within radius is listed (the member may look for extras)
};
struct TruthRec {
  uint32_t id;        // the host's handle (the world stream's id)
  uint32_t car;       // seated: its car's id, else 0
  uint16_t model;     // car: vehicle table index; person: the people group's model id (as CreateRec.model)
  int16_t pos[3];     // 2 cm from center
  int8_t vel[2];      // x / z, 1/2 m/s (+-63)
  uint8_t kind;       // CreateRec kinds (IsCar: < 16)
  uint8_t yaw;        // QYaw
  uint8_t stance;     // person: +576 (0xFF none); car: 0xFF
  uint8_t weapon;     // person: kWeaponGun | kWeaponRaised as PedRec.weapon (never kWeaponFire); car: 0
  uint8_t flags;      // person: kPed* (dead, crouch, ragdoll, burning, hidden, seated); car: kTruthCarWrecked | kTruthCarFire
  uint8_t host;       // kTruth* below: what the host knows about this id and this member
  uint8_t steady;     // person: 100 ms units since its stance / crouch / gun / seat / dead / heading (>15 deg) last changed (255 = 25.5 s+); car 255
  uint8_t seat;
};
struct TruthItem { uint32_t handle; uint16_t index; int16_t pos[3]; };              // a loose item of the host's (own_items) near the member
struct TruthProp { uint32_t uid; uint8_t sub, loose; int16_t pos[3]; };           // a prop decided in the last 120 s (props = 1; not sent in 2.4)
#pragma pack(pop)
static_assert(sizeof(UpdateHeader) == 25 && sizeof(CarRec) == 30 && sizeof(PedRec) == 20 && sizeof(SeatedRec) == 12 && sizeof(FireExt) == 7, "wire format");
static_assert(sizeof(CreateRec) == 49 && sizeof(RemoveRec) == 18 && sizeof(AdmitRec) == 35 && sizeof(DigestRec) == 16 && sizeof(OwnerMapRec) == 30, "wire format");
static_assert(offsetof(PedRec, flags) == 11 && offsetof(SeatedRec, flags) == 11, "a reader peeks byte 11 for kPedSeated");
static_assert(offsetof(PedRec, weapon) == 19 && offsetof(SeatedRec, weapon) == 10, "a reader peeks the weapon byte for a FireExt");
static_assert(sizeof(TruthHeader) == 30 && sizeof(TruthRec) == 26 && sizeof(TruthItem) == 12 && sizeof(TruthProp) == 12, "wire format (WWT1)");

constexpr size_t kMaxName = 46, kMaxLook = 127, kMaxPaint = 320, kMaxDeclined = 32, kMaxModels = 32;
constexpr size_t kDigestMaxIds = 80;    // per WWD1 message (more go in parts)
// WWT1 (2.4): TruthHeader.flags, TruthRec.host and a car's TruthRec.flags; the sample's limits and timing.
enum : uint8_t { kTruthComplete = 1 };
enum : uint8_t {
  kTruthOffer = 3,            // bits 0-1: 0 not offered yet, 1 created on you, 2 you declined it (waiting), 3 not streamed
  kTruthWhyShift = 2,         // bits 2-3 when not streamed: 1 inactive / 2 no mission / 3 named story character (0: excluded)
  kTruthChunkOff = 16,        // its chunk isn't loaded on you (your WWA1): the scheduler skips it
  kTruthPlayerCar = 32,       // a car a player drives (kEntryPlayerCar): that player's pose carries it
};
enum : uint8_t { kTruthCarWrecked = 1, kTruthCarFire = 2 };
constexpr size_t kTruthRecs = 24, kTruthItems = 8, kTruthProps = 6;
constexpr float kTruthRadius = 60.0f, kTruthVelUnit = 0.5f;
constexpr uint32_t kTruthMs = 3000, kTruthGapMs = 250, kTruthHoldMs = 600;
static_assert(sizeof(TruthHeader) + kTruthRecs * sizeof(TruthRec) + kTruthItems * sizeof(TruthItem) + kTruthProps * sizeof(TruthProp) <= size_t(kMaxMessage), "a WWT1 is one packet");

inline bool HasMagic(const void* p, const char* m) { return std::memcmp(p, m, 4) == 0; }
inline void SetMagic(char* dst, const char* m) { std::memcpy(dst, m, 4); }

// Quantisation. Positions 2 cm, velocities 1/128 m/s, angular 1/16 rad/s,
// people 1/16 m/s and 1.4 degrees; a record whose offset saturates is skipped.
constexpr float kPosUnit = 0.02f, kPosMax = 32767 * kPosUnit;
constexpr float kVelUnit = 1.0f / 128, kAngUnit = 1.0f / 16, kPedVelUnit = 1.0f / 16;
constexpr float kPi = 3.14159265358979f;
inline int32_t Round(float v) { return int32_t(std::lround(v)); }
inline int16_t Clamp16(int32_t v) { return int16_t(std::max(-32767, std::min(32767, v))); }
inline int8_t Clamp8(int32_t v) { return int8_t(std::max(-127, std::min(127, v))); }
inline bool QPos(const float ref[3], const float pos[3], int16_t out[3]) {
  for (int i = 0; i < 3; ++i) {
    const int32_t v = Round((pos[i] - ref[i]) / kPosUnit);
    if (v < -32767 || v > 32767) return false;
    out[i] = int16_t(v);
  }
  return true;
}
inline void UnqPos(const float ref[3], const int16_t q[3], float out[3]) { for (int i = 0; i < 3; ++i) out[i] = ref[i] + q[i] * kPosUnit; }
inline int16_t QVel(float v) { return Clamp16(Round(v / kVelUnit)); }
inline float UnqVel(int16_t q) { return q * kVelUnit; }
inline int8_t QAng(float v) { return Clamp8(Round(v / kAngUnit)); }
inline float UnqAng(int8_t q) { return q * kAngUnit; }
inline int8_t QPedVel(float v) { return Clamp8(Round(v / kPedVelUnit)); }
inline float UnqPedVel(int8_t q) { return q * kPedVelUnit; }
inline uint8_t QYaw(float rad) { return uint8_t(Round(rad * (128 / kPi)) & 255); }
inline float UnqYaw(uint8_t q) { return q * (kPi / 128); }
inline uint8_t QSteer(float s) { return uint8_t(std::max(-31, std::min(31, Round(s * 31))) & kDriveSteer); }
inline float UnqSteer(uint8_t d) { const int v = d & kDriveSteer; return float(v & 0x20 ? v - 64 : v) / 31; }
inline float YawOf(const float fwd[3]) { return std::atan2(fwd[0], fwd[2]); }   // y up: heading as coop.cpp's atan2(fx, fz)
inline uint8_t QHp(float hp, float max) { return uint8_t(std::max(0, std::min(255, max > 0 ? Round(255 * hp / max) : 0))); }

// Orientation rows (+32 right, +44 up, +56 forward, as PosePacket.rows) ->
// quaternion (x, y, z, w), smallest three: the largest component is dropped,
// its index goes in the low two bits of rot[0] (that one keeps 1/8192).
inline void QRot(const float rows[9], int16_t out[3]) {
  const float* m = rows;   // m[3 * row + col]
  float q[4];
  const float t = m[0] + m[4] + m[8];
  if (t > 0) { const float s = std::sqrt(t + 1) * 2; q[3] = 0.25f * s; q[0] = (m[7] - m[5]) / s; q[1] = (m[2] - m[6]) / s; q[2] = (m[3] - m[1]) / s; }
  else if (m[0] > m[4] && m[0] > m[8]) { const float s = std::sqrt(1 + m[0] - m[4] - m[8]) * 2; q[3] = (m[7] - m[5]) / s; q[0] = 0.25f * s; q[1] = (m[1] + m[3]) / s; q[2] = (m[2] + m[6]) / s; }
  else if (m[4] > m[8]) { const float s = std::sqrt(1 + m[4] - m[0] - m[8]) * 2; q[3] = (m[2] - m[6]) / s; q[0] = (m[1] + m[3]) / s; q[1] = 0.25f * s; q[2] = (m[5] + m[7]) / s; }
  else { const float s = std::sqrt(1 + m[8] - m[0] - m[4]) * 2; q[3] = (m[3] - m[1]) / s; q[0] = (m[2] + m[6]) / s; q[1] = (m[5] + m[7]) / s; q[2] = 0.25f * s; }
  int big = 0;
  for (int i = 1; i < 4; ++i) if (std::fabs(q[i]) > std::fabs(q[big])) big = i;
  if (q[big] < 0) for (float& v : q) v = -v;
  float a[3]; int n = 0;
  for (int i = 0; i < 4; ++i) if (i != big) a[n++] = std::max(-1.0f, std::min(1.0f, q[i]));
  out[0] = int16_t((std::max(-8191, std::min(8191, Round(a[0] * 8191.75f))) * 4) | big);
  out[1] = Clamp16(Round(a[1] * 32767));
  out[2] = Clamp16(Round(a[2] * 32767));
}
inline void UnqRot(const int16_t in[3], float rows[9]) {
  const int big = in[0] & 3;
  const float a[3] = {int16_t(in[0] & ~3) / 32767.0f, in[1] / 32767.0f, in[2] / 32767.0f};
  float q[4]; int n = 0;
  for (int i = 0; i < 4; ++i) if (i != big) q[i] = a[n++];
  q[big] = std::sqrt(std::max(0.0f, 1 - a[0] * a[0] - a[1] * a[1] - a[2] * a[2]));
  // (A malformed packet's three can sum past 1: renormalised, so the rows stay a rotation.)
  const float len = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
  if (len > 1.0001f) for (float& v : q) v /= len;
  const float x = q[0], y = q[1], z = q[2], w = q[3];
  rows[0] = 1 - 2 * (y * y + z * z); rows[1] = 2 * (x * y - z * w);     rows[2] = 2 * (x * z + y * w);
  rows[3] = 2 * (x * y + z * w);     rows[4] = 1 - 2 * (x * x + z * z); rows[5] = 2 * (y * z - x * w);
  rows[6] = 2 * (x * z - y * w);     rows[7] = 2 * (y * z + x * w);     rows[8] = 1 - 2 * (x * x + y * y);
}

// Variable messages: write into a buffer (returns the length, 0 when it would
// not fit), read back (false when malformed). Tails follow the fixed head.
struct CreateMsg {
  CreateRec head{};
  std::string name;
  std::vector<uint8_t> look, paint;
  std::vector<uint32_t> crew;
  std::vector<uint8_t> first;   // the first full record (CarRec / PedRec / SeatedRec bytes)
};
inline size_t WriteCreate(const CreateMsg& c, uint8_t* out, size_t cap) {
  if (c.name.size() > kMaxName || c.look.size() > kMaxLook || c.paint.size() > kMaxPaint || c.crew.size() > 255) return 0;
  CreateRec h = c.head;
  SetMagic(h.magic, "WWC1");
  h.name_len = uint8_t(c.name.size()); h.look_len = uint8_t(c.look.size()); h.paint_len = uint16_t(c.paint.size()); h.n_crew = uint8_t(c.crew.size());
  const size_t n = sizeof(h) + c.name.size() + c.look.size() + c.paint.size() + 4 * c.crew.size() + c.first.size();
  if (n > cap) return 0;
  uint8_t* p = out;
  std::memcpy(p, &h, sizeof(h)); p += sizeof(h);
  std::memcpy(p, c.name.data(), c.name.size()); p += c.name.size();
  std::memcpy(p, c.look.data(), c.look.size()); p += c.look.size();
  std::memcpy(p, c.paint.data(), c.paint.size()); p += c.paint.size();
  for (uint32_t id : c.crew) { std::memcpy(p, &id, 4); p += 4; }
  std::memcpy(p, c.first.data(), c.first.size());
  return n;
}
inline bool ReadCreate(const uint8_t* d, size_t n, CreateMsg& c) {
  if (n < sizeof(CreateRec) || !HasMagic(d, "WWC1")) return false;
  std::memcpy(&c.head, d, sizeof(c.head));
  const CreateRec& h = c.head;
  const size_t fixed = sizeof(h) + h.name_len + h.look_len + h.paint_len + 4 * size_t(h.n_crew);
  if (h.name_len > kMaxName || h.look_len > kMaxLook || h.paint_len > kMaxPaint || fixed > n) return false;
  const uint8_t* p = d + sizeof(h);
  c.name.assign(reinterpret_cast<const char*>(p), h.name_len); p += h.name_len;
  c.look.assign(p, p + h.look_len); p += h.look_len;
  c.paint.assign(p, p + h.paint_len); p += h.paint_len;
  c.crew.resize(h.n_crew);
  for (uint32_t& id : c.crew) { std::memcpy(&id, p, 4); p += 4; }
  c.first.assign(p, d + n);
  const size_t want = IsCar(h.kind) ? sizeof(CarRec) : h.car ? sizeof(SeatedRec) : sizeof(PedRec);
  return c.first.size() == want;
}

// A car's paint (8254DAF8's blob, what 8254DCC0 applies; reports/J): u32
// 0x0A55B00B, u8 n and n bytes of the variant block (8253D840; 82230EC8 keeps
// it under 65), u8 parts, then per part u8 slot, u8 part, u8 colours and that
// many {u8 palette, u8 colour}. 8254DCC0 walks it by these counts alone:
// well-formed when the walk ends at its end (2.2.1). parts: each part's offset
// (its slot byte), for the checks against the car it goes on.
inline const char* PaintWalk(const uint8_t* d, size_t n, std::vector<uint16_t>* parts = nullptr) {
  if (n > kMaxPaint) return "longer than 320 bytes";
  if (n < 6) return "shorter than its header";
  if (d[0] != 0x0A || d[1] != 0x55 || d[2] != 0xB0 || d[3] != 0x0B) return "not a car look";
  if (d[4] > 64) return "a variant block over 64 bytes";
  size_t p = 5 + size_t(d[4]);
  if (p >= n) return "cut in its variant block";
  const unsigned count = d[p++];
  for (unsigned i = 0; i < count; ++i) {
    if (p + 3 > n) return "cut in a part";
    if (parts) parts->push_back(uint16_t(p));
    p += 3 + 2 * size_t(d[p + 2]);
    if (p > n) return "cut in a part's colours";
  }
  return p == n ? nullptr : "bytes past its last part";
}

struct AdmitMsg {
  AdmitRec head{};
  std::vector<uint16_t> models;
  std::vector<uint32_t> declined;
};
inline size_t WriteAdmit(const AdmitMsg& a, uint8_t* out, size_t cap) {
  if (a.models.size() > kMaxModels || a.declined.size() > kMaxDeclined) return 0;
  AdmitRec h = a.head;
  SetMagic(h.magic, "WWA1");
  h.n_models = uint8_t(a.models.size()); h.n_declined = uint8_t(a.declined.size());
  const size_t n = sizeof(h) + 2 * a.models.size() + 4 * a.declined.size();
  if (n > cap) return 0;
  uint8_t* p = out;
  std::memcpy(p, &h, sizeof(h)); p += sizeof(h);
  for (uint16_t m : a.models) { std::memcpy(p, &m, 2); p += 2; }
  for (uint32_t id : a.declined) { std::memcpy(p, &id, 4); p += 4; }
  return n;
}
inline bool ReadAdmit(const uint8_t* d, size_t n, AdmitMsg& a) {
  if (n < sizeof(AdmitRec) || !HasMagic(d, "WWA1")) return false;
  std::memcpy(&a.head, d, sizeof(a.head));
  const AdmitRec& h = a.head;
  if (h.n_models > kMaxModels || h.n_declined > kMaxDeclined || sizeof(h) + 2 * size_t(h.n_models) + 4 * size_t(h.n_declined) != n) return false;
  const uint8_t* p = d + sizeof(h);
  a.models.resize(h.n_models); for (uint16_t& m : a.models) { std::memcpy(&m, p, 2); p += 2; }
  a.declined.resize(h.n_declined); for (uint32_t& id : a.declined) { std::memcpy(&id, p, 4); p += 4; }
  return true;
}

// WWT1 (2.4): the head's counts and magic come from the vectors.
struct TruthMsg {
  TruthHeader head{};
  std::vector<TruthRec> recs;
  std::vector<TruthItem> items;
  std::vector<TruthProp> props;
};
inline size_t WriteTruth(const TruthMsg& t, uint8_t* out, size_t cap) {
  if (t.recs.size() > kTruthRecs || t.items.size() > kTruthItems || t.props.size() > kTruthProps) return 0;
  TruthHeader h = t.head;
  SetMagic(h.magic, "WWT1");
  h.n_recs = uint8_t(t.recs.size()); h.n_items = uint8_t(t.items.size()); h.n_props = uint8_t(t.props.size());
  const size_t n = sizeof(h) + sizeof(TruthRec) * t.recs.size() + sizeof(TruthItem) * t.items.size() + sizeof(TruthProp) * t.props.size();
  if (n > cap) return 0;
  uint8_t* p = out;
  std::memcpy(p, &h, sizeof(h)); p += sizeof(h);
  if (!t.recs.empty()) { std::memcpy(p, t.recs.data(), sizeof(TruthRec) * t.recs.size()); p += sizeof(TruthRec) * t.recs.size(); }
  if (!t.items.empty()) { std::memcpy(p, t.items.data(), sizeof(TruthItem) * t.items.size()); p += sizeof(TruthItem) * t.items.size(); }
  if (!t.props.empty()) std::memcpy(p, t.props.data(), sizeof(TruthProp) * t.props.size());
  return n;
}
inline bool ReadTruth(const uint8_t* d, size_t n, TruthMsg& t) {
  if (n < sizeof(TruthHeader) || !HasMagic(d, "WWT1")) return false;
  std::memcpy(&t.head, d, sizeof(t.head));
  const TruthHeader& h = t.head;
  if (h.n_recs > kTruthRecs || h.n_items > kTruthItems || h.n_props > kTruthProps ||
      sizeof(h) + sizeof(TruthRec) * size_t(h.n_recs) + sizeof(TruthItem) * size_t(h.n_items) + sizeof(TruthProp) * size_t(h.n_props) != n) return false;
  const uint8_t* p = d + sizeof(h);
  t.recs.resize(h.n_recs); if (h.n_recs) { std::memcpy(t.recs.data(), p, sizeof(TruthRec) * h.n_recs); p += sizeof(TruthRec) * h.n_recs; }
  t.items.resize(h.n_items); if (h.n_items) { std::memcpy(t.items.data(), p, sizeof(TruthItem) * h.n_items); p += sizeof(TruthItem) * h.n_items; }
  t.props.resize(h.n_props); if (h.n_props) std::memcpy(t.props.data(), p, sizeof(TruthProp) * h.n_props);
  return true;
}
inline int8_t QTruthVel(float v) { return int8_t(std::max(-126, std::min(126, Round(v / kTruthVelUnit)))); }
inline float UnqTruthVel(int8_t q) { return q * kTruthVelUnit; }
// A person's stance (+576, 0-226) by what a viewer sees (the sync checker
// compares classes: a copy plays its own walk / run variant): 1 stands still,
// 2 crouches still, 3 crouches moving, 4 moves upright, 5 other (actions,
// custom); 0 none (0xFF). The rows are notes/coop-2.3/stances_fix.md's table.
inline uint8_t StanceClass(uint8_t s) {
  if (s == 0xFF) return 0;
  if (s <= 3 || (s >= 62 && s <= 67) || s == 125 || s == 141) return 1;
  if (s == 4 || s == 97 || s == 102) return 2;
  if (s == 5 || (s >= 94 && s <= 96) || (s >= 98 && s <= 101)) return 3;
  if ((s >= 26 && s <= 54) || (s >= 68 && s <= 93) || s == 126) return 4;
  return 5;
}

// WWS1 messages from one tick's records: cars, then people on foot, then
// seated; a new message (with the header again) whenever the next record
// would not fit. A reader tells PedRec from SeatedRec by byte 11 (flags),
// and a FireExt after either by its weapon byte (kWeaponFire). peds_ext /
// seated_ext run parallel to peds / seated (empty: none; zero when absent).
struct UpdateMsg {
  UpdateHeader head{};
  std::vector<CarRec> cars;
  std::vector<PedRec> peds;
  std::vector<SeatedRec> seated;
  std::vector<FireExt> peds_ext, seated_ext;
};
inline std::vector<std::vector<uint8_t>> PackUpdates(UpdateHeader head, const std::vector<CarRec>& cars, const std::vector<PedRec>& peds, const std::vector<SeatedRec>& seated,
                                                     const std::vector<FireExt>& peds_ext = {}, const std::vector<FireExt>& seated_ext = {}) {
  std::vector<std::vector<uint8_t>> out;
  SetMagic(head.magic, "WWS1");
  size_t ci = 0, pi = 0, si = 0;
  auto fits = [](const std::vector<uint8_t>& m, size_t n) { return m.size() + n <= size_t(kMaxMessage); };
  auto add = [](std::vector<uint8_t>& m, const void* rec, size_t n, bool ext, const std::vector<FireExt>& exts, size_t i) {
    const auto* b = static_cast<const uint8_t*>(rec); m.insert(m.end(), b, b + n);
    if (ext) { FireExt f{}; if (i < exts.size()) f = exts[i]; const auto* e = reinterpret_cast<const uint8_t*>(&f); m.insert(m.end(), e, e + sizeof(FireExt)); }
  };
  while (ci < cars.size() || pi < peds.size() || si < seated.size()) {
    std::vector<uint8_t> m(sizeof(head));
    uint8_t nc = 0, np = 0;
    for (; ci < cars.size() && nc < 255 && fits(m, sizeof(CarRec)); ++ci, ++nc) { const auto* b = reinterpret_cast<const uint8_t*>(&cars[ci]); m.insert(m.end(), b, b + sizeof(CarRec)); }
    for (; pi < peds.size() && np < 255; ++pi, ++np) {
      const bool ext = peds[pi].weapon & kWeaponFire;
      if (!fits(m, sizeof(PedRec) + (ext ? sizeof(FireExt) : 0))) break;
      add(m, &peds[pi], sizeof(PedRec), ext, peds_ext, pi);
    }
    for (; si < seated.size() && np < 255; ++si, ++np) {
      const bool ext = seated[si].weapon & kWeaponFire;
      if (!fits(m, sizeof(SeatedRec) + (ext ? sizeof(FireExt) : 0))) break;
      add(m, &seated[si], sizeof(SeatedRec), ext, seated_ext, si);
    }
    head.n_cars = nc; head.n_people = np;
    std::memcpy(m.data(), &head, sizeof(head));
    out.push_back(std::move(m));
  }
  return out;
}
inline bool ReadUpdate(const uint8_t* d, size_t n, UpdateMsg& u) {
  if (n < sizeof(UpdateHeader) || !HasMagic(d, "WWS1")) return false;
  std::memcpy(&u.head, d, sizeof(u.head));
  u.cars.clear(); u.peds.clear(); u.seated.clear(); u.peds_ext.clear(); u.seated_ext.clear();
  size_t o = sizeof(u.head);
  for (int i = 0; i < u.head.n_cars; ++i) {
    if (o + sizeof(CarRec) > n) return false;
    CarRec c; std::memcpy(&c, d + o, sizeof(c)); o += sizeof(c); u.cars.push_back(c);
  }
  // (a record cut inside its FireExt is refused)
  auto ext = [&](uint8_t weapon, std::vector<FireExt>& out) {
    FireExt f{};
    if (weapon & kWeaponFire) { if (o + sizeof(FireExt) > n) return false; std::memcpy(&f, d + o, sizeof(f)); o += sizeof(f); }
    out.push_back(f);
    return true;
  };
  for (int i = 0; i < u.head.n_people; ++i) {
    if (o + sizeof(SeatedRec) > n) return false;
    if (d[o + 11] & kPedSeated) { SeatedRec s; std::memcpy(&s, d + o, sizeof(s)); o += sizeof(s); u.seated.push_back(s); if (!ext(s.weapon, u.seated_ext)) return false; }
    else { if (o + sizeof(PedRec) > n) return false; PedRec p; std::memcpy(&p, d + o, sizeof(p)); o += sizeof(p); u.peds.push_back(p); if (!ext(p.weapon, u.peds_ext)) return false; }
  }
  return o == n;
}

// Set digest: FNV-1a over the sorted ids; WWD1 messages of at most
// kDigestMaxIds each; the member collects the parts and diffs.
inline uint32_t DigestHash(const std::vector<uint32_t>& sorted_ids) {
  uint32_t h = 2166136261u;
  for (uint32_t id : sorted_ids) for (int i = 0; i < 4; ++i) { h ^= (id >> (8 * i)) & 255; h *= 16777619u; }
  return h;
}
inline std::vector<std::vector<uint8_t>> WriteDigest(uint8_t owner, uint32_t time, const std::vector<uint32_t>& sorted_ids) {
  std::vector<std::vector<uint8_t>> out;
  const size_t parts = std::max<size_t>(1, (sorted_ids.size() + kDigestMaxIds - 1) / kDigestMaxIds);
  if (parts > 255) return out;
  const uint32_t hash = DigestHash(sorted_ids);
  for (size_t p = 0; p < parts; ++p) {
    const size_t from = p * kDigestMaxIds, to = std::min(sorted_ids.size(), from + kDigestMaxIds);
    DigestRec h{}; SetMagic(h.magic, "WWD1"); h.owner = owner; h.part = uint8_t(p); h.parts = uint8_t(parts); h.n = uint8_t(to - from); h.time = time; h.hash = hash;
    std::vector<uint8_t> m(sizeof(h) + 4 * (to - from));
    std::memcpy(m.data(), &h, sizeof(h));
    if (to > from) std::memcpy(m.data() + sizeof(h), sorted_ids.data() + from, 4 * (to - from));
    out.push_back(std::move(m));
  }
  return out;
}
// Member side: the parts of one digest (same owner time). Add returns true once all are in.
struct DigestParts {
  uint32_t time = 0, hash = 0; uint8_t parts = 0;
  std::vector<std::vector<uint32_t>> got;
  std::vector<bool> have;
  bool Add(const uint8_t* d, size_t n) {
    if (n < sizeof(DigestRec) || !HasMagic(d, "WWD1")) return false;
    DigestRec h; std::memcpy(&h, d, sizeof(h));
    if (!h.parts || h.part >= h.parts || h.n > kDigestMaxIds || sizeof(h) + 4 * size_t(h.n) != n) return false;
    if (h.time != time || h.parts != parts || have.size() != h.parts) { time = h.time; parts = h.parts; hash = h.hash; got.assign(h.parts, {}); have.assign(h.parts, false); }
    got[h.part].resize(h.n);
    if (h.n) std::memcpy(got[h.part].data(), d + sizeof(h), 4 * h.n);
    have[h.part] = true;
    return std::all_of(have.begin(), have.end(), [](bool b) { return b; });
  }
  std::vector<uint32_t> Ids() const { std::vector<uint32_t> all; for (auto& g : got) all.insert(all.end(), g.begin(), g.end()); return all; }
};
// ids the owner lists that I don't hold (missing: a lost create) and ids I
// hold that it no longer lists (extra: a lost remove). Both inputs sorted.
inline void DigestDiff(const std::vector<uint32_t>& owner_ids, const std::vector<uint32_t>& my_ids, std::vector<uint32_t>& missing, std::vector<uint32_t>& extra) {
  missing.clear(); extra.clear();
  std::set_difference(owner_ids.begin(), owner_ids.end(), my_ids.begin(), my_ids.end(), std::back_inserter(missing));
  std::set_difference(my_ids.begin(), my_ids.end(), owner_ids.begin(), owner_ids.end(), std::back_inserter(extra));
}

// Scheduler constants: the MP's priority (-10 a metre, +500 in the cone or
// within 10 m, cone dot > 0.97: 0x820899BC / 0x820899C0 / 0x82038164) plus
// the age boost (ms since the last send / 10) and +200 for people whose
// model the member has resident (creates only); it orders the creates and
// breaks ties among updates (a fair queue per band keeps those from
// starving, see Scheduler). Bands by distance to the member's eye: 20 / 10 /
// 2 Hz targets, proportional shares when short.
constexpr float kPrioDist = -10.0f, kPrioCone = 500.0f, kPrioModel = 200.0f, kConeDot = 0.97f, kConeNear = 10.0f;
constexpr uint32_t kAgeDiv = 10;
constexpr uint32_t kTickMs = 50;
constexpr float kBandNear = 50.0f, kBandMid = 150.0f;
constexpr uint32_t kBandMs[3] = {50, 100, 500};   // near 20 Hz, mid 10 Hz, far 2 Hz
constexpr uint32_t kRestMs = 500;                // kEntryRest objects unless changed since the last send
constexpr float kTagMax = 60000.0f;              // a start tag's range (ms of the band's virtual time; a guard, not a limit)
constexpr int kCreatesPerTick = 4;
constexpr uint32_t kDeclineMs = 3000, kDigestMs = 10000, kBackpressureMs = 2000;
// A declined id waits kDeclineMs, doubled on each repeat decline up to
// kDeclineMaxMs (the count is forgotten once a create has held that long);
// a person is re-offered only while the member can load its model (it has it,
// or fewer than kModelSlots people models are loaded there).
constexpr uint32_t kDeclineMaxMs = 30000;
constexpr size_t kModelSlots = 11;
constexpr uint32_t kBudgetMinBps = 6000, kBudgetMaxBps = 24000;
constexpr uint32_t kBackpressureBacklog = 16 * 1024;
constexpr uint16_t kCreateBytesCar = 130, kCreateBytesPed = 170;   // charged per create until the game thread knows the look sizes (3.0: WWC1 +10 bytes)
constexpr uint16_t kDefaultFadeMs = 1000;
constexpr uint32_t kInactiveMs = 5000;   // an entry not streaming this long is removed where it was created
// (3.0, world_mode = shared: no areas) Interest per object (design 12/(19)):
// an owner offers an object to a member whose eye is within these (people /
// cars, metres; about the owners' own despawn rings) and removes it there
// (kRemoveInterest, faded) once it is past them plus kInterestHyst. A crew
// member goes by its car. Member.interest_* 0: no radius (area mode).
constexpr float kInterestPeople = 100.0f, kInterestCars = 150.0f, kInterestHyst = 20.0f;
// (3.0 phase 2b, world_mode = shared) The spawn veto (design 12/(21), (4)):
// every game's spawners run and each spawn spot belongs to one game, the
// nearest player by position (2D). Every player within kVetoMargin of the
// nearest one ties with it, and the lowest slot of those spawns, so two
// players standing together don't flip the split every frame. Only active
// players with a pose under kVetoPoseMs take part (the caller's list), each
// only within reach of the spot (its own spawner's ring: it can't fill a spot
// past it). Every game computes the same winner from the same positions, so
// a spot is filled once.
constexpr float kVetoMargin = 10.0f;
constexpr uint32_t kVetoPoseMs = 500;
// (phase 3, review_phase2b M3) A player stays in the veto list this long
// after its last fresh pose, where that pose put it: a pose lapse doesn't
// drop it at once (both games then spawned everything near it).
constexpr uint32_t kVetoHoldMs = 1500;
struct SpawnView { uint8_t slot = 0; float pos[3] = {0, 0, 0}; };
inline float Dist2(const float a[3], const float b[3]) { const float dx = a[0] - b[0], dz = a[2] - b[2]; return std::sqrt(dx * dx + dz * dz); }
// The slot that spawns at `spot`: `me` (its player at `own`) or one of the n
// `views`, each only within `reach` (2D) of it, `me` too (review_phase2b M2:
// every game then asks with the same players, whichever made the spot); `me`
// when nobody is within reach. An inactive `me` (its player dead, paused, in
// a cutscene: the others have dropped it already) leaves every spot within
// reach of an active player to that player (H1).
inline uint8_t SpawnWinner(const float spot[3], uint8_t me, const float own[3], const SpawnView* views, int n, float reach, bool me_active = true) {
  const float mine = Dist2(spot, own);
  const bool me_in = me_active && mine <= reach;
  float best = me_in ? mine : 1e30f;
  for (int i = 0; i < n; ++i) { const float d = Dist2(spot, views[i].pos); if (d <= reach && d < best) best = d; }
  if (best >= 1e30f) return me;
  uint8_t win = me_in && mine <= best + kVetoMargin ? me : 0xFF;
  for (int i = 0; i < n; ++i) {
    const float d = Dist2(spot, views[i].pos);
    if (d <= reach && d <= best + kVetoMargin && views[i].slot < win) win = views[i].slot;
  }
  return win;
}
// (phase 3, review_phase2b M6 / L5) Hood and special spawns (824290D0) run on
// one game of the players near each other, decided greedily by slot the same
// way on every game: an active player runs them unless a lower-slot player
// that runs them is within `radius` (2D). `all` holds every active player,
// this game's too (its index `me`); an inactive `me` (not in the others'
// lists) runs them only with no running player within `radius`.
inline bool HoodRuns(const SpawnView* all, int n, int me, bool me_active, float radius) {
  if (me < 0 || me >= n) return true;
  int order[kMaxSlots * 2]; int m = 0;
  for (int i = 0; i < n && m < kMaxSlots * 2; ++i) if (i != me || me_active) order[m++] = i;
  std::sort(order, order + m, [&](int a, int b) { return all[a].slot < all[b].slot; });
  bool runs[kMaxSlots * 2] = {};
  for (int k = 0; k < m; ++k) {
    const int i = order[k];
    bool blocked = false;
    for (int j = 0; j < k && !blocked; ++j) blocked = runs[order[j]] && Dist2(all[i].pos, all[order[j]].pos) <= radius;
    runs[i] = !blocked;
  }
  if (me_active) return runs[me];
  for (int k = 0; k < m; ++k) if (runs[order[k]] && Dist2(all[me].pos, all[order[k]].pos) <= radius) return false;
  return true;
}

// Bytes per 50 ms tick per member: world_upload_kbps shared by the members, clamped to 6-24 KB/s each.
inline uint32_t TickBudget(uint32_t kbps, int members) {
  const uint32_t bps = std::max(kBudgetMinBps, std::min(kBudgetMaxBps, kbps * 1000 / 8 / uint32_t(std::max(1, members))));
  return bps * kTickMs / 1000;
}
// (3.0 phase 2c, world_mode = shared) Budgets per viewer (design 12/(12),
// review_phase2a M5 / M6):
// - a viewer takes in kViewerBps from every owner together; its WWA1 gives
//   each owner a share of it (and of its pool caps) by the copies it holds of
//   each, with a floor (ShareOut);
// - an owner's world_upload_kbps is shared only by its busy viewers (holding
//   something of it, or something of it within their interest), not every
//   shared game on the map;
// - a client's stream to another client goes through the session host's
//   relay: the relay's upload (the same setting) is shared by every relayed
//   stream (RelayStreams: each of this owner's relayed viewers is taken to
//   stream to as many others as this owner does);
// - the busy viewers' share is at least 6 KB/s (TickBudget's floor), and the
//   relay's and the viewer's shares cap it after that floor (review_phase2c
//   M6: the floor raised both back to 6 KB/s from 7 players up, so a viewer
//   took in 66 KB/s instead of 36 and the relay estimate limited nothing),
//   down to kSharedMinBps; at most 24 KB/s.
constexpr uint32_t kViewerBps = 36000, kViewerFloorBps = 6000;
constexpr uint32_t kSharedMinBps = 1500;   // a viewer's stream from one owner never below this
constexpr uint8_t kInterestMinPct = 50;   // the interest radii shrink to half at most when a viewer's pools run low
// total shared out among n by weight: each gets the floor first (an even
// share when the floor doesn't fit), the rest in proportion to the weights
// (evenly when they are all 0). The parts sum to total (rounding goes to the
// first ones).
inline void ShareOut(const uint32_t* weight, int n, uint32_t total, uint32_t floor, uint32_t* out) {
  if (n <= 0) return;
  const uint32_t base = uint64_t(floor) * uint32_t(n) <= total ? floor : total / uint32_t(n);
  uint64_t wsum = 0;
  for (int i = 0; i < n; ++i) wsum += weight[i];
  const uint32_t rest = total - base * uint32_t(n);
  uint32_t given = 0;
  for (int i = 0; i < n; ++i) {
    const uint32_t extra = wsum ? uint32_t(uint64_t(rest) * weight[i] / wsum) : rest / uint32_t(n);
    out[i] = base + extra; given += out[i];
  }
  for (int i = 0; given < total; i = (i + 1) % n) { ++out[i]; ++given; }
}
inline int RelayStreams(int relayed_viewers) { return relayed_viewers > 0 ? relayed_viewers * (relayed_viewers + 1) : 0; }
// Bytes per 50 ms tick for one viewer: `busy` viewers share kbps; a relayed
// one (relay_streams > 0) also gets at most the relay's kbps / relay_streams;
// viewer_bps (its WWA1 share, 0: not told) caps it.
inline uint32_t SharedTickBudget(uint32_t kbps, int busy, int relay_streams, uint32_t viewer_bps) {
  uint64_t bps = std::max<uint64_t>(kBudgetMinBps, uint64_t(kbps) * 1000 / 8 / uint32_t(std::max(1, busy)));
  if (relay_streams > 0) bps = std::min<uint64_t>(bps, uint64_t(kbps) * 1000 / 8 / uint32_t(relay_streams));
  if (viewer_bps) bps = std::min<uint64_t>(bps, viewer_bps);
  return uint32_t(std::clamp<uint64_t>(bps, kSharedMinBps, kBudgetMaxBps)) * kTickMs / 1000;
}
// The viewer's backpressure (its WWA1's `received` against what the owner
// queued for it since the last one): over a quarter lost, of 4 KB or more.
inline bool ViewerBehind(uint32_t sent, uint32_t received) { return sent >= 4096 && uint64_t(received) * 4 < uint64_t(sent) * 3; }

// (3.0 phase 2c, world_mode = shared) The police response (design 12/(11)):
// wanted players (police level >= 1) within kResponseGroupM of each other
// (2D, chained: A near B near C is one group) share one response owner, the
// highest police level, then the lower slot. Its game's response chases the
// most wanted of the group (UpdateResponseTarget); the others' games start no
// response of their own while in it. A player alone is its own owner. Every
// game asks with the same positions and levels, so they agree.
constexpr float kResponseGroupM = 150.0f;
struct WantedView { uint8_t slot = 0; float pos[3] = {0, 0, 0}; uint8_t level = 0; };
// The group of v[me] as a slot mask (0 when v[me] isn't wanted), and its owner (0xFF: none).
inline uint32_t ResponseGroup(const WantedView* v, int n, int me, uint8_t* owner) {
  if (owner) *owner = 0xFF;
  if (me < 0 || me >= n || !v[me].level) return 0;
  bool in[kMaxSlots * 2] = {};
  const int cap = std::min(n, kMaxSlots * 2);
  in[me] = true;
  for (bool grew = true; grew;) {
    grew = false;
    for (int i = 0; i < cap; ++i) if (!in[i] && v[i].level)
      for (int j = 0; j < cap; ++j) if (in[j] && Dist2(v[i].pos, v[j].pos) <= kResponseGroupM) { in[i] = grew = true; break; }
  }
  uint32_t mask = 0; int best = -1;
  for (int i = 0; i < cap; ++i) if (in[i]) {
    if (v[i].slot < 32) mask |= 1u << v[i].slot;
    if (best < 0 || v[i].level > v[best].level || (v[i].level == v[best].level && v[i].slot < v[best].slot)) best = i;
  }
  if (owner && best >= 0) *owner = v[best].slot;
  return mask;
}

using Key = uint64_t;   // (owner slot, id): a record's name, the owner's handle in the owner's namespace
inline Key MakeKey(uint8_t owner, uint32_t id) { return (Key(owner) << 32) | id; }
inline uint8_t OwnerOf(Key k) { return uint8_t(k >> 32); }
inline uint32_t IdOf(Key k) { return uint32_t(k); }

// (3.0) NetId: an object's name for its whole life, whoever owns it
// (design 12/(8)): the slot that made it (origin), the origin's handle and
// the origin's join nonce (a new one each session there, so a slot that joins
// again, or a handle a later session uses again, names another object). It is
// not a handle: a game holds the object by its own (Entry.local, the copy's
// local). Packed origin << 56 | handle << 16 | nonce: map order is the
// origin's handle order (Plan's order as in 2.x).
using NetId = uint64_t;
inline NetId MakeNetId(uint8_t origin, uint32_t handle, uint16_t nonce) { return (NetId(origin) << 56) | (NetId(handle) << 16) | nonce; }
inline uint8_t NetOrigin(NetId n) { return uint8_t(n >> 56); }
inline uint32_t NetHandle(NetId n) { return uint32_t(n >> 16); }
inline uint16_t NetNonce(NetId n) { return uint16_t(n); }
// An event names a person by its NetId's handle (one word) and a tag: the
// full tag (a word) origin << 28 | (ownership epoch & 0xFFF) << 16 | nonce;
// where an event has only 16 bits left, the short tag (its top half: origin
// << 12 | epoch & 0xFFF), matched without the nonce. Origin 15: none
// (kNoPersonTag: a person the sender names to no one).
constexpr uint32_t kNoPersonTag = 0xF0000000u;
inline uint32_t NetTag(NetId n, uint16_t epoch) { return (uint32_t(NetOrigin(n) & 15) << 28) | (uint32_t(epoch & 0xFFF) << 16) | NetNonce(n); }
inline uint16_t NetShortTag(NetId n, uint16_t epoch) { return uint16_t(NetTag(n, epoch) >> 16); }
inline uint8_t TagOrigin(uint32_t tag) { return uint8_t(tag >> 28); }
inline uint16_t TagNonce(uint32_t tag) { return uint16_t(tag); }
inline uint16_t TagEpoch(uint32_t tag) { return uint16_t((tag >> 16) & 0xFFF); }
inline uint32_t TagFromShort(uint16_t s) { return uint32_t(s) << 16; }   // (a short tag as a full one without its nonce)
// The packing in events: a tag's short half for the top half of a word, a word's top half read back as a tag
// (without its nonce), and the tag of an origin alone (an argument: matched by origin and handle).
inline uint32_t ShortBits(uint32_t tag) { return tag & 0xFFFF0000u; }
inline uint32_t TagOfShortWord(uint32_t word) { return word & 0xFFFF0000u; }
inline uint32_t OriginTag(uint8_t origin) { return uint32_t(origin & 15) << 28; }
// The tag's epoch is older than `epoch` (12 bits, wrapping: an event a hand-over overtook).
inline bool TagOlder(uint32_t tag, uint16_t epoch) { return ((int(TagEpoch(tag)) - int(epoch & 0xFFF)) & 0x800) != 0; }

// (3.0 phase 3, world_mode = shared, mod.ini handover = 1) The hand-over in
// place (DESIGN 6 and 12/(9), (10), (15); handover_research.md 5). An
// object's owner (the game that simulates it) moves without a fade: the old
// owner's object becomes its copy of the new owner's, the new owner's copy
// becomes its own (out of co-op's copy tables, its brain back on, on the
// ambient or traffic list). Only the owner starts one; each step is one
// WWH1 (reliable, kMsgWorldHand; an older build ignores the type):
//  - Offer (owner -> candidate): the NetId, its epoch e, its record id there,
//    the state at T0. The owner simulates on.
//  - Accept (its copy's handle here: the record id once adopted, and its
//    crew's) or Decline(reason). Nothing changes on the candidate yet.
//  - Release (e + 1, the final state at T1, the slots that hold a copy): the
//    owner's object is its copy of the candidate's already (one update).
//    Nobody simulates it for the trip (two hops through the relay).
//  - The candidate adopts it and sends OwnerChanged (e + 1, its new id, the
//    old key) to everyone: the copies change key (their records and hits go to
//    it from then on). Return (e + 2) instead when it can't adopt at Release:
//    the old owner takes its copy back as its own.
//  - No Accept within kHandAcceptMs: the owner keeps it (nothing changed). No
//    OwnerChanged nor record within kHandReleaseMs of the Release: the old
//    owner takes it back at e + 2 (and says so with its own OwnerChanged).
//  - Cancel (owner -> candidate, e): an Accept that brings no Release (the
//    object changed since the offer, or the Accept came late): the candidate
//    drops its accepted offer at once (review_phase3 M3).
//  - A game that owns an object and hears of a newer epoch of it (or the same
//    epoch from a lower slot) gives it up to a copy of that game's (Yields).
//  - Owner gone (left the session, or it removed the object while it
//    evacuated): each holder that could own it claims it after
//    ClaimDelayMs(slot) unless it heard a claim first; equal epochs go to the
//    lower slot.
enum : uint8_t { kHandOffer = 1, kHandAccept, kHandDecline, kHandRelease, kHandReturn, kHandOwner, kHandCancel };
// Decline (and Return) reasons.
enum : uint8_t { kHandNoCopy = 0, kHandNoRoom, kHandNoNode, kHandNotLoaded, kHandTooFar, kHandBusy, kHandInactive, kHandOff, kHandStale, kHandReasons };
// HandRec.flags: the car is a player's (the authority path: no hysteresis, the player's game takes it, it keeps
// no lane); the offer is urgent (80 % of the owner's ring) or an evacuation's (at once, no hysteresis).
enum : uint8_t { kHandPlayerCar = 1, kHandUrgent = 2, kHandEvac = 4 };
constexpr int kHandMaxCrew = 4;
constexpr uint32_t kHandAcceptMs = 1000, kHandReleaseMs = 2000, kHandAcceptedMs = 3000;
#pragma pack(push, 1)
struct HandRec {
  char magic[4];        // WWH1
  uint8_t type, reason; // kHand*; a decline's / return's reason
  uint8_t kind, flags;  // the object's create kind (IsCar); kHand* flags
  uint8_t origin;       // NetId: the slot that made it
  uint16_t nonce;       // NetId: the origin's join nonce
  uint32_t net;         // NetId: the origin's handle
  uint16_t epoch;       // offer / accept / decline / cancel: e; release: e + 1; return: e + 2; owner: the new one
  uint32_t id;          // offer / release / return / cancel: the sender's record id; accept: the candidate's copy (its id once adopted); owner: the new owner's id
  uint8_t old_owner;    // owner: the key the copies held it by (owner, id)
  uint32_t old_id;
  uint32_t car;         // owner: a seated person's car under its new owner (0 on foot)
  uint32_t time;        // the sender's clock (ms): the new owner's records come after it
  uint16_t created;     // release: the slots that hold a copy (the new owner streams to them without a create)
  // The state (offer at T0, release at T1): what a fresh restart would lose (handover_research.md 3).
  float pos[3], vel[3]; // a car's linear velocity; a person's x / z in vel[0] / vel[2]
  float hp, hp_max;     // person +1912 / +1908; car +53512 / +53508
  uint8_t damage, team; // car: CarDamage bits; person: +232
  uint8_t combat_off, attack;   // person: +3692 & 0x08, +3697 & 0x70 (what MakePassive overwrites on a copy)
  uint16_t ai_mode, ai_sub;     // person: +3716 / +3720 (10: combat)
  uint8_t target_slot;  // a fighter's target: the player of that slot (0xFF: none or not a player)
  uint8_t n_crew;       // a car's crew in its seats (<= kHandMaxCrew)
  uint32_t crew[kHandMaxCrew];  // offer / release: the owner's ids; accept: the candidate's copies
  float crew_hp[kHandMaxCrew];
};
#pragma pack(pop)
static_assert(sizeof(HandRec) == 110, "wire format (WWH1)");
inline size_t WriteHand(const HandRec& r, uint8_t* out, size_t cap) {
  if (cap < sizeof(HandRec) || r.n_crew > kHandMaxCrew || r.type < kHandOffer || r.type > kHandCancel) return 0;
  HandRec h = r; SetMagic(h.magic, "WWH1");
  std::memcpy(out, &h, sizeof(h));
  return sizeof(h);
}
inline bool ReadHand(const uint8_t* d, size_t n, HandRec& r) {
  if (n != sizeof(HandRec) || !HasMagic(d, "WWH1")) return false;
  std::memcpy(&r, d, sizeof(r));
  if (r.type < kHandOffer || r.type > kHandCancel || r.n_crew > kHandMaxCrew || r.origin >= kMaxSlots || r.reason >= kHandReasons) return false;
  for (int i = 0; i < 3; ++i) if (!std::isfinite(r.pos[i]) || !std::isfinite(r.vel[i])) return false;
  return std::isfinite(r.hp) && std::isfinite(r.hp_max);
}
inline NetId HandNet(const HandRec& r) { return MakeNetId(r.origin, r.net, r.nonce); }
inline void SetHandNet(HandRec& r, NetId n) { r.origin = NetOrigin(n); r.net = NetHandle(n); r.nonce = NetNonce(n); }
// Ownership epochs (16 bits, wrapping): a is newer than b.
inline bool EpochNewer(uint16_t a, uint16_t b) { return int16_t(uint16_t(a - b)) > 0; }
// Two games say they own one object: (epoch a, slot sa) keeps it against (b, sb): the newer epoch, else the lower slot.
inline bool HandWins(uint16_t a, uint8_t sa, uint16_t b, uint8_t sb) { return EpochNewer(a, b) || (a == b && sa < sb); }
// An owner-less object's claim waits this long on a holder (the lower slot claims first; design 12/(9)).
inline uint32_t ClaimDelayMs(uint8_t slot) { return 2000u + 250u * slot; }

// The owner's side of one object's hand-over: idle, offered (to, e, when),
// released (to, e + 1, when). What the owner does next.
enum : uint8_t { kHandIdle = 0, kHandOffered, kHandReleased };
enum : uint8_t { kHandNothing = 0, kHandSendRelease, kHandForget, kHandTakeBack, kHandDone };
struct HandOut {
  uint8_t step = kHandIdle, to = 0xFF;
  uint16_t epoch = 0;   // offered: e; released: e + 1
  uint64_t at = 0;
  void Offer(uint8_t slot, uint16_t e, uint64_t now) { step = kHandOffered; to = slot; epoch = e; at = now; }
  // Accept from `from` for epoch e: release now (in time, the one asked, the epoch offered), else nothing.
  uint8_t OnAccept(uint8_t from, uint16_t e, uint64_t now) {
    if (step != kHandOffered || from != to || e != epoch || now - at > kHandAcceptMs) return kHandNothing;
    step = kHandReleased; epoch = uint16_t(e + 1); at = now;
    return kHandSendRelease;
  }
  uint8_t OnDecline(uint8_t from, uint16_t e) { if (step != kHandOffered || from != to || e != epoch) return kHandNothing; *this = HandOut{}; return kHandForget; }
  // OwnerChanged (or the first record) at e + 1 or later: over. From the new owner, or from any game: the
  // object moved on (or was claimed) meanwhile, so no take-back (review_phase3 M5).
  uint8_t OnOwner(uint8_t from, uint16_t e) { (void)from; if (step != kHandReleased || EpochNewer(epoch, e)) return kHandNothing; *this = HandOut{}; return kHandDone; }
  // Return from the new owner at e + 2: the old owner takes it back.
  uint8_t OnReturn(uint8_t from, uint16_t e) { if (step != kHandReleased || from != to || e != uint16_t(epoch + 1)) return kHandNothing; *this = HandOut{}; return kHandTakeBack; }
  // Timeouts: no answer to the offer (keep it), no word of the release (take it back at e + 2).
  uint8_t Tick(uint64_t now) {
    if (step == kHandOffered && now - at > kHandAcceptMs) { *this = HandOut{}; return kHandForget; }
    if (step == kHandReleased && now - at > kHandReleaseMs) { *this = HandOut{}; return kHandTakeBack; }
    return kHandNothing;
  }
};
// The candidate's side: an Accept sent (from, e, when); its Release must come
// from the same owner at e + 1 within kHandAcceptedMs, else the candidate
// returns it (Return, e + 2).
struct HandIn {
  uint8_t from = 0xFF; uint16_t epoch = 0; uint64_t at = 0;
  bool Matches(uint8_t slot, uint16_t release_epoch, uint64_t now) const { return slot == from && release_epoch == uint16_t(epoch + 1) && now - at <= kHandAcceptedMs; }
};

// Who should own an object next (design 12/(10)): the active viewer whose
// camera is nearest, holding the copy and within its keep radius; another
// game must be nearer than the owner's camera by the margin for kHandHoldMs,
// and an object moves at most every kHandEveryMs. Urgent (no hysteresis, no
// spacing): the object is past kHandUrgentPart of the owner's despawn ring.
struct HandRule { float keep, margin, ring; };
constexpr HandRule kHandPeople{75.0f, 25.0f, 90.0f}, kHandCars{120.0f, 40.0f, 130.0f};
constexpr uint32_t kHandHoldMs = 2000, kHandEveryMs = 5000;
constexpr float kHandUrgentPart = 0.8f;
struct HandView { uint8_t slot = 0xFF; float d = 0; bool ok = false; };   // a viewer: its camera's distance from the object; ok: active, holds a made copy
struct HandHyst { uint8_t slot = 0xFF; uint64_t since = 0, last = 0; };   // per object: the candidate and since when; the last hand-over
inline uint8_t HandPick(HandHyst& h, float d_own, const HandView* v, int n, const HandRule& r, uint64_t now, bool* urgent) {
  if (urgent) *urgent = false;
  int best = -1;
  for (int i = 0; i < n; ++i) if (v[i].ok && v[i].d <= r.keep && (best < 0 || v[i].d < v[best].d || (v[i].d == v[best].d && v[i].slot < v[best].slot))) best = i;
  if (best < 0) { h.slot = 0xFF; h.since = 0; return 0xFF; }
  const uint8_t s = v[best].slot;
  if (d_own > kHandUrgentPart * r.ring) { if (urgent) *urgent = true; return s; }
  if (v[best].d + r.margin > d_own) { h.slot = 0xFF; h.since = 0; return 0xFF; }
  if (h.slot != s) { h.slot = s; h.since = now; return 0xFF; }
  if (now - h.since < kHandHoldMs || (h.last && now - h.last < kHandEveryMs)) return 0xFF;
  return s;
}

// Per-member offer state of one entry.
struct Offer {
  uint64_t last_sent = 0;       // create or update time (the age boost)
  uint64_t offered_at = 0;      // the last create's time
  uint64_t declined_until = 0;  // declined by the member: no updates, re-offered after this when it has room
  float tag = 0;                // start tag (start-time fair queuing): ms of its band's virtual time until its turn; +interval a send
  uint8_t band = 255;           // the band it was last planned in (a change starts the tag over at the band's virtual now)
  uint8_t declines = 0;         // declines in a row (the hold doubles each time)
  bool created = false;         // a create went to this member and was not declined / removed since
};

struct Entry {
  uint8_t owner = 0, kind = 0, source = 0, authority = 0, flags = 0, seat = 0;   // owner: the slot that simulates it (3.0: a field, not the key's)
  int16_t chunk = -1;           // chunk list index when known (parked records' +56), else -1
  uint16_t model = 0;
  uint16_t create_bytes = 0;    // the member's create size (0: the default by kind)
  uint8_t extra = 0;            // people: bytes after the record this tick (a FireExt while it fired in the last second)
  NetId net = 0;                // (3.0) its name for life (the registry's key)
  uint16_t epoch = 0;           // (3.0) ownership epoch (0 in 1a)
  uint32_t local = 0, car = 0;  // local: this game's handle (the records' id while it owns it); car: a seated person's car (its local)
  uint32_t activations = 1;     // the same human coming through 8247A6D0 again (model reload, queued mission NPC)
  uint64_t created_at = 0, changed_at = 0;   // changed_at: hp / flags / occupant changed (a resting object sends at band rate once)
  uint64_t inactive_since = 0;  // not streaming since (Plan keeps it)
  float pos[3] = {0, 0, 0};
  std::string name;             // people: the model name for the create
  Offer offer[kMaxSlots];
  bool Seated() const { return !IsCar(kind) && car != 0; }
  size_t RecordBytes() const { return IsCar(kind) ? sizeof(CarRec) : (Seated() ? sizeof(SeatedRec) : sizeof(PedRec)) + extra; }
  uint16_t CreateBytes() const { return create_bytes ? create_bytes : IsCar(kind) ? kCreateBytesCar : kCreateBytesPed; }
  bool Streams() const { return !(flags & (kEntryInactive | kEntryExcluded)); }
};

// key: the record name the members hold it by (owner, local); epoch: the ownership epoch it went at.
struct Removal { Key key = 0; uint8_t kind = 0, reason = 0; uint16_t fade_ms = 0, epoch = 0; bool created[kMaxSlots] = {}; };

// Every game runs one; the owner streams from it. Keyed by NetId (3.0;
// map order makes Plan deterministic), found by this game's handle too.
class Registry {
 public:
  // Idempotent: the same object (by its handle here) again keeps created_at and counts an activation.
  Entry& Add(NetId net, uint8_t owner, uint32_t local, uint8_t kind, uint8_t source, uint64_t now) {
    // (review_final_area L7: find, never at(): a throw inside a game hook ends the game; a handle whose entry is gone is added again)
    if (auto l = local_.find(local); l != local_.end()) {
      if (auto it = entries_.find(l->second); it != entries_.end()) { Entry& e = it->second; ++e.activations; ++reactivated_; return e; }
      local_.erase(l);
    }
    Entry& e = entries_[net];
    local_[local] = net;
    e.net = net; e.owner = owner; e.local = local; e.kind = kind; e.source = source; e.authority = owner; e.created_at = now; e.changed_at = now; ++added_;
    return e;
  }
  // Gone in the owner's game (or fading there with fade_ms left): members it was created on get a WWR1.
  bool Remove(uint32_t local, uint16_t fade_ms = kDefaultFadeMs, uint8_t reason = kRemoveGone) {
    auto l = local_.find(local);
    if (l == local_.end()) return false;
    auto it = entries_.find(l->second);
    local_.erase(l);
    if (it == entries_.end()) return false;
    Removal r; r.key = MakeKey(it->second.owner, local); r.kind = it->second.kind; r.reason = reason; r.fade_ms = fade_ms; r.epoch = it->second.epoch;
    for (int s = 0; s < kMaxSlots; ++s) r.created[s] = it->second.offer[s].created;
    pending_.push_back(r);
    entries_.erase(it);
    ++removed_;
    return true;
  }
  // (phase 3) Out of the registry without a remove (its holders keep their
  // copies: a hand-over's release, a yield); its entry back, for the move.
  bool Detach(uint32_t local, Entry* out = nullptr) {
    auto l = local_.find(local);
    if (l == local_.end()) return false;
    auto it = entries_.find(l->second);
    local_.erase(l);
    if (it == entries_.end()) return false;
    if (out) *out = it->second;
    entries_.erase(it);
    return true;
  }
  // (phase 3) An object this game takes over (adopt, claim, take back): its
  // NetId stays, this game owns it at `epoch` and streams it to `created`
  // (slots that hold a copy) without a create. One with this handle already
  // in (it can't be, a copy is left out) is taken as it is.
  Entry& Adopt(NetId net, uint8_t owner, uint32_t local, uint8_t kind, uint8_t source, uint16_t epoch, uint16_t created, uint64_t now) {
    if (Entry* e = Find(local)) return *e;
    Entry& e = entries_[net];
    e = Entry{};
    local_[local] = net;
    e.net = net; e.owner = owner; e.local = local; e.kind = kind; e.source = source; e.authority = owner; e.epoch = epoch; e.created_at = now; e.changed_at = now;
    for (int s = 0; s < kMaxSlots; ++s) if ((created >> s) & 1) { Offer& o = e.offer[s]; o.created = true; o.offered_at = now > 1 ? now - 1 : 0; o.last_sent = now; }
    ++added_;
    return e;
  }
  // By this game's handle.
  Entry* Find(uint32_t local) { auto l = local_.find(local); if (l == local_.end()) return nullptr; auto it = entries_.find(l->second); return it == entries_.end() ? nullptr : &it->second; }
  const Entry* Find(uint32_t local) const { auto l = local_.find(local); if (l == local_.end()) return nullptr; auto it = entries_.find(l->second); return it == entries_.end() ? nullptr : &it->second; }
  // By NetId; FindOrigin by the origin's slot and handle alone (an event's short tag has no nonce).
  Entry* FindNet(NetId net) { auto it = entries_.find(net); return it == entries_.end() ? nullptr : &it->second; }
  Entry* FindOrigin(uint8_t origin, uint32_t handle) {
    auto it = entries_.lower_bound(MakeNetId(origin, handle, 0));
    return it != entries_.end() && NetOrigin(it->first) == origin && NetHandle(it->first) == handle ? &it->second : nullptr;
  }
  std::map<NetId, Entry>& Entries() { return entries_; }
  const std::map<NetId, Entry>& Entries() const { return entries_; }
  size_t Size() const { return entries_.size(); }
  void Clear() { entries_.clear(); local_.clear(); pending_.clear(); }
  std::vector<Removal> TakeRemovals() { std::vector<Removal> r; r.swap(pending_); return r; }
  // The owner line's churn baseline: the game's own creations and removals since the last take.
  void TakeCounts(uint32_t& added, uint32_t& removed, uint32_t& reactivated) { added = added_; removed = removed_; reactivated = reactivated_; added_ = removed_ = reactivated_ = 0; }
  int CountKind(uint8_t kind) const { int n = 0; for (auto& [k, e] : entries_) n += e.kind == kind; return n; }

 private:
  std::map<NetId, Entry> entries_;
  std::map<uint32_t, NetId> local_;   // this game's handle -> NetId
  std::vector<Removal> pending_;
  uint32_t added_ = 0, removed_ = 0, reactivated_ = 0;
};

// The owner's view of a member. eye / fwd from its poses (PosePacket.eye,
// cam row 3); caps, held, chunks and models from its WWA1.
struct BandStat { uint32_t objects = 0, planned = 0, bytes = 0, rest_objects = 0, rest_planned = 0; };   // summed over ticks; the 10 s line divides (rest_*: the 2 Hz resting ones among them)
struct Member {
  bool present = false, in_area = false;   // (in_area, 3.0 shared: a viewer, no areas: WorldOwnerTick's rule)
  float eye[3] = {0, 0, 0}, fwd[3] = {0, 0, 1};
  uint64_t chunks = ~0ull;      // all loaded until it reports
  uint16_t cars_held = 0, cars_cap = 0xFFFF, people_held = 0, people_cap = 0xFFFF;
  std::vector<uint16_t> models; // resident people models, sorted
  uint32_t budget = 570;        // bytes per tick (TickBudget)
  uint64_t halved_until = 0;    // backpressure: half budget until then
  uint64_t last_digest = 0, last_admit = 0;
  float interest_people = 0, interest_cars = 0;   // (3.0, shared) the per-object radius for this member; 0: none
  uint16_t viewer_bps = 0;                        // (2c, shared) its WWA1's share for this owner (bytes a second; 0: not told)
  uint8_t interest_people_pct = 100, interest_cars_pct = 100;   // (2c, shared) how much of the radii its pools have room for
  // Scheduler state and the 10 s line's counters.
  bool was_in_area = false;
  uint32_t ticks = 0;
  BandStat band[3];
  uint32_t creates = 0, reoffers = 0, declines = 0, removes = 0, bytes = 0, over_create_budget = 0, skipped_chunk = 0, skipped_far = 0, skipped_crew = 0, halvings = 0;
  uint32_t skipped_interest = 0, removes_interest = 0;   // (3.0, shared) entry-ticks out of its interest; removes for leaving it
  uint32_t removes_crew = 0;   // (3.0) removes of a crew member whose car the member doesn't hold
  bool HasRoom(uint8_t kind) const { return IsCar(kind) ? cars_held < cars_cap : people_held < people_cap; }
  bool HasModel(uint16_t model) const { return std::binary_search(models.begin(), models.end(), model); }
  void ResetStats() { ticks = 0; for (auto& b : band) b = {}; creates = reoffers = declines = removes = bytes = over_create_budget = skipped_chunk = skipped_far = skipped_crew = halvings = skipped_interest = removes_interest = removes_crew = 0; }
};

inline float Dist(const float a[3], const float b[3]) { const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2]; return std::sqrt(dx * dx + dy * dy + dz * dz); }
inline bool InCone(const float eye[3], const float fwd[3], const float pos[3], float d) {
  if (d < 1e-3f) return true;
  return ((pos[0] - eye[0]) * fwd[0] + (pos[1] - eye[1]) * fwd[1] + (pos[2] - eye[2]) * fwd[2]) / d > kConeDot;
}
inline int BandOf(float d) { return d <= kBandNear ? 0 : d <= kBandMid ? 1 : 2; }
// (3.0, shared) Whether e is of interest to member m (its offer o there): within the radius of its kind of the
// member's eye, plus kInterestHyst while it is created there. Always without a radius (area mode).
inline bool InInterest(const Member& m, const Entry& e, const Offer& o) {
  if (m.interest_people <= 0 && m.interest_cars <= 0) return true;
  const float r = IsCar(e.kind) ? m.interest_cars : m.interest_people;
  return Dist(e.pos, m.eye) <= r + (o.created ? kInterestHyst : 0.0f);
}

// (3.0, the fades fix) The owner's camera-aware hold: another game's camera may see p when p is within kSeenNear of
// its eye, or within interest (+ kInterestHyst) and in front of it (60 degrees of its forward row: wider than any
// view). within: within kSeenNear.
constexpr float kSeenNear = 30.0f;
inline bool SeenFrom(const float eye[3], const float fwd[3], const float p[3], float interest, bool* within = nullptr) {
  const float d = Dist(p, eye);
  if (within) *within = d <= kSeenNear;
  if (d <= kSeenNear) return true;
  return d <= interest + kInterestHyst && ((p[0] - eye[0]) * fwd[0] + (p[1] - eye[1]) * fwd[1] + (p[2] - eye[2]) * fwd[2]) > 0.5f * d;
}
// One object held from a removal path the game asks again and again (the off-screen cull every frame, the traffic
// housekeeping each pass): kHoldKeepMs at most from the hold's start; a path that leaves it alone for kHoldGapMs (the
// object came back on the owner's screen, near its player, or out of the others' view) starts the next one afresh,
// but not within kHoldCoolMs of the last one's end (review_fades_fix L1: the housekeeping, which asks only while it is
// over its count, could hold one car in 20 s slices for ever). At most kHoldPeople / kHoldCars held at once.
constexpr uint64_t kHoldKeepMs = 20000, kHoldGapMs = 2000, kHoldCoolMs = 30000;
constexpr unsigned kHoldPeople = 8, kHoldCars = 4;
// (fades fix 2) A person's spawn category at its limit (where the off-screen cull runs: a model swap into a full
// street) still takes a hold, up to kHoldAtLimit of that category at once (within kHoldPeople).
constexpr unsigned kHoldAtLimit = 4;
// (review_fades2 M2) The distance despawn's holds (area: a person past 90 m a member may see) take at most kHoldFar
// of kHoldPeople, so the off-screen cull's in-view holds near the members always keep kHoldPeople - kHoldFar.
constexpr unsigned kHoldFar = 4;
struct SeenHold { uint64_t since = 0, last = 0, ended = 0; bool over = false; };   // ended: when the last hold's 20 s ran out (0: none)
enum : uint8_t { kHoldNew, kHoldOn, kHoldEnded, kHoldOver };   // a new hold, held, its 20 s just ran out, ran out before (no hold)
// The last hold's end: its 20 s, or its last ask when the path left it alone first.
inline uint64_t HoldEnd(const SeenHold& h) { return h.over ? h.ended : h.last; }
// The next ask would start a hold (the path left the object alone 2 s, and not within kHoldCoolMs of the last end).
inline bool HoldStarts(const SeenHold& h, uint64_t now) {
  if (h.last && now >= h.last && now - h.last <= kHoldGapMs) return false;
  const uint64_t end = HoldEnd(h);
  return !h.last || !end || now < end || now - end >= kHoldCoolMs;
}
inline uint8_t HoldAsk(SeenHold& h, uint64_t now) {
  if (!h.last || now < h.last || now - h.last > kHoldGapMs) {
    if (!HoldStarts(h, now)) { h.ended = HoldEnd(h); h.last = now; h.over = true; return kHoldOver; }   // (cooling down: no hold)
    h.since = h.last = now; h.over = false; h.ended = 0; return kHoldNew;
  }
  h.last = now;
  if (h.over) return kHoldOver;
  if (now - h.since > kHoldKeepMs) { h.over = true; h.ended = now; return kHoldEnded; }
  return kHoldOn;
}
inline bool HoldActive(const SeenHold& h, uint64_t now) { return h.last && now >= h.last && now - h.last <= kHoldGapMs && !h.over; }
// (review_overnight M3) No hold during a cutscene (here, or the mission host's) or before quiet_until (a mission
// start's loads): a cutscene's or a mission's characters may wait on the slots a hold keeps.
inline bool HoldsPaused(uint64_t now, bool cutscene, uint64_t quiet_until) { return cutscene || now < quiet_until; }
// The member's side: where this camera has a copy its owner removed. In view (on this screen and within kSightPeople
// / kSightCars of the eye) it fades; behind (off the screen, within that) or far it goes at once, as the game's own
// traffic removal does out of view (spec 3.10). Far is past the copies' interest (+ kInterestHyst), where a copy no
// longer lives: whatever the screen shows of one fades (a person 60-120 m ahead popped with a 60 m cut-off).
enum : uint8_t { kSightView, kSightBehind, kSightFar, kSights };
constexpr float kSightPeople = kInterestPeople + kInterestHyst, kSightCars = kInterestCars + kInterestHyst;
inline uint8_t SightOf(bool on_screen, float d, bool car) { return !(d <= (car ? kSightCars : kSightPeople)) ? kSightFar : on_screen ? kSightView : kSightBehind; }
// (fades fix 2) A dead body removed in view isn't faded: it lies on until it is off this screen (then it goes at
// once) or kCorpseLingerMs pass (then it fades); kCorpseLinger at once at most (the rest fade as before). 4, not 8
// (review_fades2 M1): the game's corpse manager keeps 5 bodies and fades the rest on the screen itself.
constexpr uint64_t kCorpseLingerMs = 30000;
constexpr unsigned kCorpseLinger = 4;

struct Planned { Key key = 0; uint8_t kind = 0, band = 0; uint16_t bytes = 0; float priority = 0; bool seated = false; };   // bytes: the record's and its FireExt
struct PlannedRemove { Key key = 0; uint8_t reason = 0; uint16_t fade_ms = 0, epoch = 0; };   // (epoch: the ownership epoch, 3.0)
struct MemberPlan {
  std::vector<Planned> creates, updates;   // in send order (crew right after its car)
  std::vector<PlannedRemove> removes;
  std::vector<uint32_t> digest;            // sorted ids, when digest_due
  uint32_t digest_hash = 0;
  bool digest_due = false;
  uint32_t budget = 0, bytes = 0, messages = 0;   // this tick: the budget, what the plan costs, WWS1 messages needed
};

class Scheduler {
 public:
  // One tick for every member: creates (<= 4, charged against the budget;
  // the first always goes), then updates by band share and priority, then
  // the registry's removals and the digest. Plans index by slot.
  std::array<MemberPlan, kMaxSlots> Plan(uint64_t now, Registry& reg, std::array<Member, kMaxSlots>& members, const float ref[3]) {
    std::array<MemberPlan, kMaxSlots> plans;
    const std::vector<Removal> removals = reg.TakeRemovals();
    for (auto& [k, e] : reg.Entries()) { if (e.Streams()) e.inactive_since = 0; else if (!e.inactive_since) e.inactive_since = now; }
    for (int s = 0; s < kMaxSlots; ++s) {
      Member& m = members[s];
      MemberPlan& p = plans[s];
      for (const Removal& r : removals) if (r.created[s]) { p.removes.push_back({r.key, r.reason, r.fade_ms, r.epoch}); ++m.removes; }
      if (!(m.present && m.in_area)) {
        // Left the area (reason 3 for what it held) or the session (nothing): the offers start over.
        for (auto& [k, e] : reg.Entries()) {
          Offer& o = e.offer[s];
          if (o.created && m.was_in_area && m.present) { p.removes.push_back({MakeKey(e.owner, e.local), kRemoveAreaLeft, kDefaultFadeMs, e.epoch}); ++m.removes; }
          o = Offer{};
        }
        m.was_in_area = false;
        continue;
      }
      if (!m.was_in_area) { m.was_in_area = true; m.last_digest = now; }
      ++m.ticks;
      PlanMember(now, reg, m, s, ref, p);
    }
    return plans;
  }

  // A member's WWA1: caps, held, models, chunks; its declined ids are not
  // re-offered for 3 s (6, 12, ... 30 s when declined again) and then only
  // while it reports room (counting the creates planned since).
  void OnAdmit(uint64_t now, Registry& reg, Member& m, int slot, uint8_t owner, const AdmitMsg& a) {
    m.cars_held = a.head.cars_held; m.cars_cap = a.head.cars_cap; m.people_held = a.head.people_held; m.people_cap = a.head.people_cap;
    m.chunks = a.head.chunks;
    m.models = a.models; std::sort(m.models.begin(), m.models.end());
    m.last_admit = now;
    m.viewer_bps = a.head.budget_bps;
    m.interest_people_pct = a.head.interest_people_pct ? std::min<uint8_t>(a.head.interest_people_pct, 100) : 100;
    m.interest_cars_pct = a.head.interest_cars_pct ? std::min<uint8_t>(a.head.interest_cars_pct, 100) : 100;
    for (uint32_t id : a.declined)
      if (Entry* e = reg.Find(id); e && e->owner == owner) {
        Offer& o = e->offer[slot];
        o.created = false; o.declined_until = now + std::min<uint64_t>(uint64_t(kDeclineMs) << std::min<int>(o.declines, 4), kDeclineMaxMs);
        if (o.declines < 255) ++o.declines;
        ++m.declines;
      }
  }
  // Backpressure (LinkStats): half the budget for 2 s.
  static void Backpressure(uint64_t now, Member& m) { if (now >= m.halved_until) ++m.halvings; m.halved_until = now + kBackpressureMs; }

 private:
  struct Cand {
    Entry* e; Offer* o;
    float priority, d, tag;     // tag: the sort key (its own start tag, a crew member's car's)
    int band; uint16_t bytes, unit; uint32_t anchor, interval;   // unit: a car with its due crew (sent together)
    bool crew, seated, streams, due, sent, rest; size_t car;
  };
  // Plan order: start-time fair queuing per band (the smallest start tag
  // first; a send moves the tag on by the object's interval, a new object
  // starts at the band's virtual now, which is 0 after each tick's rebase):
  // nothing starves, a short band slows every object alike whatever its
  // rate, a newcomer gets its turn in the current round. Ties by the MP
  // priority, then id; crew right after its car.
  static bool Before(const Cand& a, const Cand& b) {
    if (a.tag != b.tag) return a.tag < b.tag;
    if (a.priority != b.priority) return a.priority > b.priority;
    if (a.anchor != b.anchor) return a.anchor < b.anchor;
    if (a.crew != b.crew) return !a.crew;
    return a.e->local < b.e->local;
  }

  void PlanMember(uint64_t now, Registry& reg, Member& m, int s, const float ref[3], MemberPlan& p) {
    uint32_t budget = m.budget;
    if (now < m.halved_until) budget /= 2;
    p.budget = budget;
    // Candidates: streamed entries the member can know about, with the MP
    // priority, their band and, once created, their interval and start tag.
    std::vector<Cand> cands;
    cands.reserve(reg.Size());
    std::map<uint32_t, size_t> car_index;   // car id -> cand (for its crew)
    for (auto& [k, e] : reg.Entries()) {
      if (!e.Streams()) {
        // Not streaming (inactive: its chunk or model unloaded, a mission
        // person after the mission; excluded): no records. A member it was
        // created on keeps it (the digest still lists it: a model reload is
        // over in under 5 s); after kInactiveMs it is removed there (reason 0)
        // and created again when it streams.
        Offer& o = e.offer[s];
        if (o.created && e.inactive_since && now - e.inactive_since >= kInactiveMs) { p.removes.push_back({MakeKey(e.owner, e.local), kRemoveGone, kDefaultFadeMs, e.epoch}); ++m.removes; o = Offer{}; }
        continue;
      }
      const bool chunk_off = e.chunk >= 0 && e.chunk < 64 && !((m.chunks >> e.chunk) & 1);
      const bool past_max = Dist(e.pos, ref) >= kPosMax;
      Offer& o = e.offer[s];
      if (m.interest_people > 0 || m.interest_cars > 0) {
        // (3.0, shared) Interest per object: out of it, removed where it was
        // created (faded there) and its offer starts over; a declined one keeps
        // its wait. A crew member by its car's place and offer: they go together.
        // (2c) Tested first: an object the member holds whose chunk it no
        // longer has (a respawn, a taxi, a follow teleport) or past kPosMax is
        // removed for interest too, not left frozen there (review_phase2a M3).
        const Entry* car = e.car ? reg.Find(e.car) : nullptr;
        if (chunk_off || past_max || !(car && IsCar(car->kind) ? InInterest(m, *car, car->offer[s]) : InInterest(m, e, o))) {
          if (o.created) { p.removes.push_back({MakeKey(e.owner, e.local), kRemoveInterest, kDefaultFadeMs, e.epoch}); ++m.removes; ++m.removes_interest; o = Offer{}; }
          if (chunk_off) ++m.skipped_chunk; else if (past_max) ++m.skipped_far; else ++m.skipped_interest;
          continue;
        }
      } else {
        if (chunk_off) { ++m.skipped_chunk; continue; }
        if (past_max) { ++m.skipped_far; continue; }
      }
      if (o.declined_until && (now < o.declined_until || !m.HasRoom(e.kind))) continue;
      if (o.declined_until && !IsCar(e.kind) && !m.HasModel(e.model) && m.models.size() >= kModelSlots) continue;   // (it would be declined "model" again)
      if (o.created && o.declines && now - o.offered_at >= kDeclineMaxMs) o.declines = 0;   // (held since: the next decline starts at 3 s)
      const float d = Dist(e.pos, m.eye);
      const bool cone = d < kConeNear || InCone(m.eye, m.fwd, e.pos, d);
      const uint64_t age = o.created ? now - o.last_sent : 0;
      float prio = kPrioDist * d + (cone ? kPrioCone : 0) + float(age) / kAgeDiv;
      if (!o.created && !IsCar(e.kind) && m.HasModel(e.model)) prio += kPrioModel;
      Cand c{&e, &o, prio, d, 0, BandOf(d), 0, 0, e.local, 0, false, false, false, false, false, false, size_t(-1)};
      c.seated = e.car && reg.Find(e.car);   // (a car that is gone: on foot until its own hook says otherwise)
      c.bytes = uint16_t(IsCar(e.kind) ? sizeof(CarRec) : (c.seated ? sizeof(SeatedRec) : sizeof(PedRec)) + e.extra);
      // Streams: created before this tick (the create carries the first record) and not a car a player drives.
      c.streams = o.created && o.offered_at != now && !(IsCar(e.kind) && (e.authority != e.owner || (e.flags & kEntryPlayerCar)));
      if (c.streams) {
        c.rest = (e.flags & kEntryRest) && e.changed_at <= o.last_sent;
        c.interval = c.rest ? std::max(kRestMs, kBandMs[c.band]) : kBandMs[c.band];
        if (o.band != c.band) { o.band = uint8_t(c.band); o.tag = 0; }
        c.tag = o.tag;
        c.due = now + kTickMs / 2 >= o.last_sent + c.interval;
      }
      if (IsCar(e.kind)) car_index[e.local] = cands.size();
      cands.push_back(c);
    }
    // Crew inherits its car's priority, band and place and sorts right after
    // it; a seated person whose car the member cannot have is not offered alone.
    // (3.0, MJQ's VK02 report) One created there while on foot that sat in a
    // car the member doesn't hold (left out: a mission car, a train; declined
    // there) is removed there (reason 0) and offered again with its car or on
    // foot: it got no record and no remove, and its copy stood frozen at the
    // door. A car the member holds and that only isn't a candidate this tick
    // (its chunk, kPosMax, a short inactive spell) keeps its crew as before.
    // Only for a car of this registry: one it doesn't know (a pose's car copy,
    // another owner's car) never makes a person seated here (c.seated), so it
    // goes on foot as before and is never removed for it.
    std::vector<size_t> idx;   // the candidates in plan order
    idx.reserve(cands.size());
    for (size_t i = 0; i < cands.size(); ++i) {
      Cand& c = cands[i];
      if (c.seated) {
        auto it = car_index.find(c.e->car);
        if (it == car_index.end()) {
          ++m.skipped_crew;
          const Entry* car = reg.Find(c.e->car);
          if (c.o->created && car && !car->offer[s].created) { p.removes.push_back({MakeKey(c.e->owner, c.e->local), kRemoveGone, kDefaultFadeMs, c.e->epoch}); ++m.removes; ++m.removes_crew; *c.o = Offer{}; }
          continue;
        }
        const Cand& car = cands[it->second];
        c.priority = car.priority; c.band = car.band; c.tag = car.tag; c.anchor = car.e->local; c.crew = true; c.car = it->second;
        if (c.streams && c.o->band != c.band) { c.o->band = uint8_t(c.band); c.o->tag = 0; }
      }
      idx.push_back(i);
    }
    std::stable_sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return Before(cands[a], cands[b]); });

    // Creates: priority order, at most kCreatesPerTick, charged against the
    // budget (the first always goes: catch-up must move on any budget). A crew
    // member waits for its car's create; a declined id is re-offered here.
    uint32_t spent = 0;
    int creates = 0;
    for (size_t i : idx) {
      Cand& c = cands[i];
      if (c.o->created || creates >= kCreatesPerTick) continue;
      if (c.crew && !cands[c.car].o->created) continue;
      if (c.o->declined_until && !m.HasRoom(c.e->kind)) continue;   // (the room the member reported is taken by this tick's creates)
      const uint16_t cost = c.e->CreateBytes();
      if (creates && spent + cost > budget) { ++m.over_create_budget; break; }
      spent += cost; ++creates;
      if (c.o->declined_until) ++m.reoffers;
      const uint8_t declines = c.o->declines;
      *c.o = Offer{}; c.o->created = true; c.o->offered_at = c.o->last_sent = now; c.o->declines = declines;
      // Counted as held until the member's next WWA1 says what it holds (a
      // re-offer only goes into reported room: one free slot, one re-offer).
      uint16_t& held = IsCar(c.e->kind) ? m.cars_held : m.people_held;
      if (held < 0xFFFF) ++held;
      p.creates.push_back({MakeKey(c.e->owner, c.e->local), c.e->kind, uint8_t(c.band), cost, c.priority});
      ++m.creates;
    }
    // Updates: due = the band's interval (500 ms at rest, unless changed) has
    // passed since the last send; shares per band proportional to the band's
    // target bytes, capped at its need; leftovers redistributed; within a
    // band in plan order (the smallest start tag first).
    uint32_t need[3] = {}, target[3] = {}, first[3] = {};   // first: the first due unit's bytes (a band's floor)
    for (size_t pi = 0; pi < idx.size(); ++pi) {
      Cand& c = cands[idx[pi]];
      if (!c.streams) continue;
      ++m.band[c.band].objects; m.band[c.band].rest_objects += c.rest;
      target[c.band] += c.bytes * kTickMs * 16 / c.interval;   // x16: integer shares of a 50 ms tick
      c.unit = c.bytes;
      if (IsCar(c.e->kind)) for (size_t q = pi + 1; q < idx.size(); ++q) { const Cand& k = cands[idx[q]]; if (!k.crew || k.anchor != c.e->local) break; if (k.streams && k.due) c.unit = uint16_t(c.unit + k.bytes); }
      if (c.due) { need[c.band] += c.bytes; if (!first[c.band]) first[c.band] = c.unit; }
    }
    // Record bytes: what is left after the creates, less a header per message the budget could fill.
    const uint32_t left = budget > spent ? budget - spent : 0;
    const uint32_t headers = uint32_t(sizeof(UpdateHeader)) * std::max<uint32_t>(1, (left + kMaxMessage - 1) / kMaxMessage);
    const uint32_t avail = left > headers ? left - headers : 0;
    uint32_t share[3] = {};
    if (need[0] + need[1] + need[2] <= avail) { for (int b = 0; b < 3; ++b) share[b] = need[b]; }
    else {
      // One record for every band with something due (a share by target can
      // be smaller than a record: two parked cars near a member), then the
      // rest in proportion to the bands' target bytes.
      uint32_t pool = avail;
      bool open[3] = {need[0] > 0, need[1] > 0, need[2] > 0};
      for (int b = 0; b < 3; ++b) if (open[b] && first[b] <= pool) { share[b] = first[b]; pool -= first[b]; if (share[b] >= need[b]) open[b] = false; }
      for (int pass = 0; pass < 3 && pool; ++pass) {
        uint64_t tsum = 0;
        for (int b = 0; b < 3; ++b) if (open[b]) tsum += target[b];
        if (!tsum) break;
        uint32_t given = 0;
        for (int b = 0; b < 3; ++b) {
          if (!open[b]) continue;
          uint32_t g = uint32_t(uint64_t(pool) * target[b] / tsum);
          if (share[b] + g >= need[b]) { g = need[b] - share[b]; open[b] = false; }
          share[b] += g; given += g;
        }
        pool -= given;
        if (!given) break;
      }
      // Rounding leftovers: the highest-priority due records that still fit.
      for (size_t i : idx) { const Cand& c = cands[i]; if (c.due && pool >= c.bytes && share[c.band] < need[c.band]) { share[c.band] += c.bytes; pool -= c.bytes; } }
    }
    uint32_t used[3] = {}, bytes = 0, msg = 0, messages = 0;
    float vnow[3] = {0, 0, 0};   // each band's virtual time: the start tag in service
    for (size_t i : idx) {
      Cand& c = cands[i];
      if (!c.due || used[c.band] + c.unit > share[c.band]) continue;
      if (c.crew) { const Cand& car = cands[c.car]; if (car.due && !car.sent) continue; }   // crew rides with its car
      c.sent = true;
      if (!messages || msg + c.bytes > uint32_t(kMaxMessage)) { ++messages; msg = uint32_t(sizeof(UpdateHeader)); }
      msg += c.bytes; used[c.band] += c.bytes; bytes += c.bytes;
      vnow[c.band] = std::max(vnow[c.band], c.tag);
      c.o->last_sent = now; c.o->tag = std::max(c.o->tag + float(c.interval), vnow[c.band]);
      ++m.band[c.band].planned; m.band[c.band].bytes += c.bytes; m.band[c.band].rest_planned += c.rest;
      p.updates.push_back({MakeKey(c.e->owner, c.e->local), c.e->kind, uint8_t(c.band), c.bytes, c.priority, c.seated});
    }
    // Rebase every band's tags on its virtual now (0 next tick): only the differences matter.
    for (size_t i : idx) { Cand& c = cands[i]; if (c.streams) c.o->tag = std::max(-kTagMax, std::min(kTagMax, c.o->tag - vnow[c.band])); }
    p.messages = messages;
    p.bytes = spent + bytes + messages * uint32_t(sizeof(UpdateHeader));
    m.bytes += p.bytes;
    // Digest every 10 s: the ids this member was offered (held or declined), sorted.
    if (now - m.last_digest >= kDigestMs) {
      m.last_digest = now;
      p.digest_due = true;
      for (auto& [k, e] : reg.Entries()) { const Offer& o = e.offer[s]; if (o.created || (e.Streams() && o.declined_until)) p.digest.push_back(e.local); }
      std::sort(p.digest.begin(), p.digest.end());
      p.digest_hash = DigestHash(p.digest);
    }
  }
};

// ---- 3.0: a player's climb (kEventClimb) and the sound pause watchdog (coop.cpp) ----
// research notes/coop-3/vault_climb_research.md 3.2-3.3 and audio_cutscene_research.md 3.

// Who started our player's climb (ClimbStart 82444270's return address): the controls on the ground (8216A038),
// a ledge grabbed in a jump (823F6990) or in a fall (82463100).
enum : uint8_t { kClimbOther = 0, kClimbGround = 1, kClimbJump = 2, kClimbFall = 3 };
inline uint8_t ClimbCallerOf(uint32_t lr) {
  return lr == 0x8216AC90u ? kClimbGround : lr == 0x823F6BB0u ? kClimbJump : lr == 0x824634A4u ? kClimbFall : kClimbOther;
}
inline const char* ClimbCallerName(uint8_t c) { return c == kClimbGround ? "ground" : c == kClimbJump ? "jump" : c == kClimbFall ? "fall" : "other"; }
// kEventClimb a[7]: the flag (bits 0-7), the state before (8-11), its sub-state (12-19), the new sub-state +520
// (20-23: 4 or 5), the caller (24-27).
struct ClimbInfo { uint8_t flag = 0, state = 0, sub = 0, new_sub = 0, caller = 0; };
inline uint32_t PackClimbInfo(const ClimbInfo& c) {
  return uint32_t(c.flag) | (uint32_t(c.state & 15) << 8) | (uint32_t(c.sub) << 12) | (uint32_t(c.new_sub & 15) << 20) | (uint32_t(c.caller & 15) << 24);
}
inline ClimbInfo UnpackClimbInfo(uint32_t v) {
  ClimbInfo c;
  c.flag = uint8_t(v); c.state = uint8_t((v >> 8) & 15); c.sub = uint8_t((v >> 12) & 255); c.new_sub = uint8_t((v >> 20) & 15); c.caller = uint8_t((v >> 24) & 15);
  return c;
}
// The receiver's replay: the character is put at the owner's start spot when it is within kClimbPlaceM of it, and
// the ledge its own finder (8234A1C8) gives there is the owner's when its point is within kClimbMatchM and its
// facing (frame row 2) within ~37 degrees.
constexpr float kClimbPlaceM = 3.0f, kClimbMatchM = 0.5f, kClimbFacingDot = 0.8f;
inline bool ClimbLedgeMatches(const float* pt_here, const float* pt_theirs, const float* fwd_here, const float* fwd_theirs) {
  float d2 = 0, dot = 0, a2 = 0, b2 = 0;
  for (int i = 0; i < 3; ++i) {
    const float d = pt_here[i] - pt_theirs[i];
    d2 += d * d; dot += fwd_here[i] * fwd_theirs[i]; a2 += fwd_here[i] * fwd_here[i]; b2 += fwd_theirs[i] * fwd_theirs[i];
  }
  if (!(d2 < kClimbMatchM * kClimbMatchM) || !(a2 > 1e-6f) || !(b2 > 1e-6f)) return false;   // (NaN: no match)
  return dot / std::sqrt(a2 * b2) > kClimbFacingDot;
}
// Upright rows facing yaw (forward (sin, 0, cos)), with the handedness of the character's own rows (the sign of
// row0 x row1 . row2): a right row of the wrong sign mirrors the character.
inline void ClimbRows(float yaw, const float* old_rows, float rows[9]) {
  const float* r = old_rows;
  const float cx = r[1] * r[5] - r[2] * r[4], cy = r[2] * r[3] - r[0] * r[5], cz = r[0] * r[4] - r[1] * r[3];
  const float h = cx * r[6] + cy * r[7] + cz * r[8] < 0.0f ? -1.0f : 1.0f;
  const float fx = std::sin(yaw), fz = std::cos(yaw);
  rows[0] = h * fz; rows[1] = 0; rows[2] = -h * fx; rows[3] = 0; rows[4] = 1; rows[5] = 0; rows[6] = fx; rows[7] = 0; rows[8] = fz;
}
// While the character plays the replayed climb itself: the owner's climb states (5/x), its drop (6/x) and the
// state 1 / 0 around them are not replayed on it, nor the climb and drop actions (523-537) and the landings (0x2A /
// 0x2E): the character's own 824623C8 / 82463100 play them (a replay restarted the animations).
inline bool ClimbDropsState(uint32_t state) { return state == 0 || state == 1 || state == 5 || state == 6; }
inline bool ClimbDropsAction(uint32_t id) { return (id >= 0x20B && id <= 0x219) || id == 0x2A || id == 0x2E; }
// The owner's climb is over: its landing 6/E, its 0/x, or a 1/x after its drop (6/x) began.
inline bool ClimbOwnerEnds(uint32_t state, uint32_t sub, bool owner_dropped) {
  return (state == 6 && sub == 0xE) || state == 0 || (state == 1 && owner_dropped);
}
// The replayed climb's hold, each update: aborted when the character left state 5 within kClimbWatchMs (refused
// or cut: the fallback takes over); handed back once the character is out of 5 / 6 after kClimbWatchMs (its own
// climb is over, whatever the owner sent: a climb that ends on top sends 5/x -> 1/0 with no drop, review_memfair
// M3), or kClimbAfterEndMs after the owner's end, or after kClimbCapMs in all. The owner's 5/x still to come after
// such a hand-back are its climb's tail (ClimbTail), not a new climb.
enum : uint8_t { kClimbHold = 0, kClimbHandBack = 1, kClimbAborted = 2 };
constexpr uint64_t kClimbWatchMs = 150, kClimbAfterEndMs = 400, kClimbCapMs = 2500, kClimbRepeatMs = 300;
constexpr float kClimbFollowGapM = 4.0f, kClimbSnapM = 1.5f;
inline uint8_t ClimbHoldStep(uint64_t now, uint64_t started, uint64_t owner_end, uint32_t own_state) {
  if (now - started < kClimbWatchMs && own_state != 5) return kClimbAborted;
  if (own_state != 5 && own_state != 6) return kClimbHandBack;
  if (owner_end && now - owner_end >= kClimbAfterEndMs) return kClimbHandBack;
  if (now - started >= kClimbCapMs) return kClimbHandBack;
  return kClimbHold;
}
// A replayed climb handed back before its owner's end: the owner's own climb states still to come (5/x, its drop
// 6/x) are its climb's tail, not played and no fallback started, until the owner's end (ClimbOwnerEnds) or
// tail_until (the climb's start + kClimbCapMs). Bits: kTailDrop this state isn't played, kTailOver the tail is over.
enum : uint8_t { kTailDrop = 1, kTailOver = 2 };
inline uint8_t ClimbTailStep(uint64_t now, uint64_t tail_until, uint32_t state, uint32_t sub, bool owner_dropped) {
  if (!tail_until) return 0;
  if (now >= tail_until) return kTailOver;
  return uint8_t((state == 5 || state == 6 ? kTailDrop : 0) | (ClimbOwnerEnds(state, sub, owner_dropped) ? kTailOver : 0));
}

// The game's own reasons for its sound pause count (0x8370CF84) to be above 0 (audio_cutscene_research.md 3.2).
enum : uint32_t { kSoundWorldPause = 1, kSoundCutscene = 2, kSoundLoading = 4, kSoundVideo = 8, kSoundDeath = 16, kSoundMenu = 32, kSoundOwed = 64 };
// Count above 0 with none of them for kSoundStuckMs in a row: stuck (the watchdog repairs it). Any reason, or the
// count back at 0, starts the wait over.
constexpr uint64_t kSoundStuckMs = 5000;
struct SoundWatch { bool waiting = false; uint64_t since = 0; };
inline bool SoundStuck(SoundWatch& w, uint64_t now, int32_t count, uint32_t reasons) {
  if (count <= 0 || reasons) { w.waiting = false; return false; }
  if (!w.waiting) { w.waiting = true; w.since = now; }
  return now - w.since >= kSoundStuckMs;
}
// Co-op's forced cutscene end (821F9700) waits while the start's deferred begin (0x8370D998 = 821F7640, or not yet
// running: 0x8370D990 = 0) is pending, up to kCutsceneEndWaitMs: an end in that window left the start's sound pause
// on and the begin ran after it. True: wait this update.
constexpr uint32_t kCutsceneBeginStep = 0x821F7640u;
constexpr uint64_t kCutsceneEndWaitMs = 1000;
inline bool CutsceneEndWaits(uint32_t step, bool running, uint64_t now, uint64_t& since) {
  if (step != kCutsceneBeginStep && running) { since = 0; return false; }
  if (!since) since = now + 1;   // (+1: 0 is "not waiting")
  if (now + 1 - since >= kCutsceneEndWaitMs) { since = 0; return false; }
  return true;
}

// (3.0, player_mission_cars: MJQ's VK02 report) A member's own twin of a host
// mission car it shows: the nearest of its own mission cars of the same model
// (the caller leaves out the ones in use, hidden by their script or paired).
// radius > 0 (a car a player sat in): only within radius of where the host's car
// was first seen here or is now, the nearest to either (the twin stands where
// the car was made); radius 0 (2.4's lists): the nearest to where it is now.
// Returns the index, or -1.
struct TwinCand { int32_t model = -1; float pos[3]{}; };
inline int PickTwin(const TwinCand* c, size_t n, int32_t model, const float here[3], const float first[3], float radius, float* d_out = nullptr) {
  int best = -1; float best_d2 = 1e30f;
  for (size_t i = 0; i < n; ++i) {
    if (c[i].model != model) continue;
    float d2 = 0, f2 = 0;
    for (int k = 0; k < 3; ++k) { const float d = c[i].pos[k] - here[k], f = c[i].pos[k] - first[k]; d2 += d * d; f2 += f * f; }
    if (radius > 0) { d2 = std::min(d2, f2); if (d2 > radius * radius) continue; }
    if (d2 < best_d2) { best_d2 = d2; best = int(i); }
  }
  if (d_out) *d_out = best >= 0 ? std::sqrt(best_d2) : 0;
  return best;
}

// ---- Player poses (protocol 12: research notes/coop-3/pose_bandwidth_design.md) ----
// A player's pose as coop.cpp's PlayerPose carries it, without the model and
// the look (those go in WAP1 when they change). The game side keeps its
// PlayerPose; only the wire is compact.
struct PoseData {
  uint32_t sequence = 0, time = 0;   // the sender's count; its clock, ms
  float pos[3]{}, rows[9]{};         // +20, +32 (right, up, forward rows)
  int32_t vehicle = -1;              // info index, -1 on foot
  uint32_t seat = 0;
  float vpos[3]{}, vrows[9]{}, vvel[3]{}, vang[3]{};
  uint32_t vowner = 0, vid = 0;      // the car's owner slot and its handle there
  uint32_t flags = 0;                // PlayerPose::flags
  float eye[3]{}, cam[9]{};          // the camera (0 = none)
  uint8_t appearance = 0;            // the sender's WAP1 version (0: none yet)
};
#pragma pack(push, 1)
struct PosePacket {      // WPP2 (kMsgPose, unreliable): 48 B on foot; a PoseCar follows with kFormCar
  char magic[4];
  uint32_t sequence;
  uint32_t time;
  float pos[3];          // float, at a fixed offset (12): the relay reads it
  int16_t rows[3];       // QRot (the right row negated first when mirrored: kFormRowsMirror)
  int16_t eye[3];        // QPos from pos (kFormEye clear: no eye, 0 as before)
  int16_t cam[3];        // QRot (kFormCam clear: no camera; kFormCamMirror)
  uint32_t flags;        // PlayerPose::flags, unchanged
  uint8_t form;          // kForm*
  uint8_t appearance;    // the sender's appearance version (WAP1), wrapping, never 0 once sent; 0 = none yet
};
struct PoseCar {         // 27 B after the PosePacket, in a car
  uint8_t vehicle;       // info index (< 256)
  uint8_t seat_owner;    // seat (low 4 bits, < 8) | vowner << 4 (< 12)
  uint32_t vid;
  int16_t vpos[3];       // QPos from pos (saturated: 0, the car at the player)
  int16_t vrows[3];      // QRot (kFormCarMirror)
  int16_t vvel[3];       // QVel, 1/128 m/s
  int8_t vang[3];        // QAng, 1/16 rad/s
};
struct AppearancePacket {   // WAP1 (kMsgAppearance, reliable on a change and to a joiner; unreliable every 5 s): + model[model_len], look[look_len]
  char magic[4];
  uint8_t version, model_len, look_len;
};
#pragma pack(pop)
enum : uint8_t { kFormCar = 1, kFormCam = 2, kFormEye = 4, kFormRowsMirror = 8, kFormCamMirror = 16, kFormCarMirror = 32 };
constexpr size_t kPoseFoot = sizeof(PosePacket), kPoseInCar = sizeof(PosePacket) + sizeof(PoseCar);
constexpr size_t kPoseTimeAt = offsetof(PosePacket, time), kPosePosAt = offsetof(PosePacket, pos), kPoseFlagsAt = offsetof(PosePacket, flags), kPoseFormAt = offsetof(PosePacket, form);
constexpr size_t kPoseVelAt = sizeof(PosePacket) + offsetof(PoseCar, vvel);
constexpr size_t kMaxModelName = 63, kMaxPlayerLook = 127;
static_assert(sizeof(PosePacket) == 48 && sizeof(PoseCar) == 27 && sizeof(AppearancePacket) == 7, "wire format");
static_assert(kPoseTimeAt == 8 && kPosePosAt == 12 && kPoseFlagsAt == 42 && kPoseFormAt == 46, "the relay's offsets");

inline float Det3(const float m[9]) { return m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) + m[2] * (m[3] * m[7] - m[4] * m[6]); }
// Rows as QRot: mirrored ones (the right row negated, the kPedSign convention:
// det < 0) are turned into a rotation first; true when they were. Rows that
// aren't one (non-finite, or |det| under 0.5: all zero for a matrix not set
// yet) go as identity, not as QRot's 180-degree roll.
inline bool QRows(const float rows[9], int16_t out[3]) {
  float r[9];
  std::memcpy(r, rows, sizeof(r));
  bool usable = true;
  for (float v : r) usable = usable && std::isfinite(v);
  if (!usable || std::fabs(Det3(r)) < 0.5f) { const float id[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}; std::memcpy(r, id, sizeof(r)); }
  const bool mirror = Det3(r) < 0;
  if (mirror) for (int i = 0; i < 3; ++i) r[i] = -r[i];
  QRot(r, out);
  return mirror;
}
inline void UnqRows(const int16_t in[3], bool mirror, float rows[9]) {
  UnqRot(in, rows);
  if (mirror) for (int i = 0; i < 3; ++i) rows[i] = -rows[i];
}
// A camera worth sending, orthonormalised (Gram-Schmidt from the forward row,
// then up; right = up x forward, negated for a mirrored one): false for "no
// camera" (all zero, coop.cpp's none), non-finite, or degenerate (a row norm
// under 0.1 or |det| under 0.1). Nothing confirms that the game's camera
// block is a pure rotation, so a scaled or skewed one is still sent.
constexpr float kCamMinNorm = 0.1f, kCamMinDet = 0.1f;
inline bool CamUsable(const float cam[9], float out[9]) {
  bool any = false;
  for (int i = 0; i < 9; ++i) { if (!std::isfinite(cam[i])) return false; any = any || cam[i] != 0; }
  if (!any) return false;
  auto dot = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
  for (int r = 0; r < 3; ++r) if (std::sqrt(dot(cam + 3 * r, cam + 3 * r)) < kCamMinNorm) return false;
  const float det = Det3(cam);
  if (std::fabs(det) < kCamMinDet) return false;
  float f[3], u[3];
  const float fl = std::sqrt(dot(cam + 6, cam + 6));
  for (int i = 0; i < 3; ++i) f[i] = cam[6 + i] / fl;
  const float uf = dot(cam + 3, f);
  for (int i = 0; i < 3; ++i) u[i] = cam[3 + i] - uf * f[i];
  const float ul = std::sqrt(dot(u, u));
  if (ul < 1e-3f) return false;
  for (float& v : u) v /= ul;
  const float s = det < 0 ? -1.0f : 1.0f;
  out[0] = s * (u[1] * f[2] - u[2] * f[1]); out[1] = s * (u[2] * f[0] - u[0] * f[2]); out[2] = s * (u[0] * f[1] - u[1] * f[0]);
  std::memcpy(out + 3, u, sizeof(u)); std::memcpy(out + 6, f, sizeof(f));
  return true;
}
inline bool CamUsable(const float cam[9]) { float o[9]; return CamUsable(cam, o); }
// What EncodePose left out of a pose (coop.cpp counts it in the Bandwidth line).
struct PoseEncodeNote {
  bool cam_refused = false;   // a camera was there (not all zero) but degenerate
  bool cam_no_eye = false;    // a usable camera, but no eye to send with it (over 655 m from the player): no camera either
  float cam_det = 0;          // the camera's det (before orthonormalising)
};
// The wire bytes of a pose (48 or 75, written to out; 0 when pos isn't usable).
// The camera goes only with the eye (the receiver writes both, +4112 / +4124).
// A car tail only for a seat under 8 and an owner slot under 12 (else on foot).
inline size_t EncodePose(const PoseData& p, uint8_t out[kPoseInCar], PoseEncodeNote* note = nullptr) {
  for (float v : p.pos) if (!std::isfinite(v) || std::fabs(v) >= 100000) return 0;
  PosePacket w{};
  SetMagic(w.magic, "WPP2");
  w.sequence = p.sequence; w.time = p.time; std::memcpy(w.pos, p.pos, sizeof(w.pos));
  if (QRows(p.rows, w.rows)) w.form |= kFormRowsMirror;
  if ((p.eye[0] || p.eye[1] || p.eye[2]) && std::isfinite(p.eye[0]) && std::isfinite(p.eye[1]) && std::isfinite(p.eye[2]) && QPos(p.pos, p.eye, w.eye)) w.form |= kFormEye;
  else std::memset(w.eye, 0, sizeof(w.eye));
  float cam[9];
  bool any_cam = false;
  for (float v : p.cam) any_cam = any_cam || v != 0;
  if (CamUsable(p.cam, cam)) {
    if (w.form & kFormEye) { w.form |= kFormCam; if (QRows(cam, w.cam)) w.form |= kFormCamMirror; }
    else if (note) note->cam_no_eye = true;
  } else if (any_cam && note) {
    note->cam_refused = true;
    note->cam_det = Det3(p.cam);
  }
  w.flags = p.flags; w.appearance = p.appearance;
  const bool car = p.vehicle >= 0 && p.vehicle < 256 && p.seat < 8 && p.vowner < uint32_t(kMaxSlots);
  if (car) w.form |= kFormCar;
  std::memcpy(out, &w, sizeof(w));
  if (!car) return sizeof(w);
  PoseCar c{};
  c.vehicle = uint8_t(p.vehicle);
  c.seat_owner = uint8_t(p.seat | p.vowner << 4);
  c.vid = p.vid;
  bool vfinite = true;
  for (float v : p.vpos) vfinite = vfinite && std::isfinite(v);
  if (!vfinite || !QPos(p.pos, p.vpos, c.vpos)) std::memset(c.vpos, 0, sizeof(c.vpos));
  if (QRows(p.vrows, c.vrows)) out[kPoseFormAt] |= kFormCarMirror;
  for (int i = 0; i < 3; ++i) {
    c.vvel[i] = std::isfinite(p.vvel[i]) ? QVel(p.vvel[i]) : 0;
    c.vang[i] = std::isfinite(p.vang[i]) ? QAng(p.vang[i]) : 0;
  }
  std::memcpy(out + sizeof(w), &c, sizeof(c));
  return kPoseInCar;
}
// The receive side: exactly 48 B, or 75 with kFormCar; false when malformed.
inline bool DecodePose(const uint8_t* d, size_t n, PoseData& p) {
  if (n != kPoseFoot && n != kPoseInCar) return false;
  PosePacket w;
  std::memcpy(&w, d, sizeof(w));
  if (!HasMagic(w.magic, "WPP2") || (n == kPoseInCar) != ((w.form & kFormCar) != 0)) return false;
  for (float v : w.pos) if (!std::isfinite(v) || std::fabs(v) >= 100000) return false;
  p = PoseData{};
  p.sequence = w.sequence; p.time = w.time; std::memcpy(p.pos, w.pos, sizeof(p.pos));
  UnqRows(w.rows, (w.form & kFormRowsMirror) != 0, p.rows);
  if (w.form & kFormEye) UnqPos(p.pos, w.eye, p.eye);
  if (w.form & kFormCam) UnqRows(w.cam, (w.form & kFormCamMirror) != 0, p.cam);
  p.flags = w.flags; p.appearance = w.appearance;
  if (!(w.form & kFormCar)) return true;
  PoseCar c;
  std::memcpy(&c, d + sizeof(w), sizeof(c));
  p.vehicle = c.vehicle; p.seat = c.seat_owner & 15u; p.vowner = c.seat_owner >> 4u; p.vid = c.vid;
  if (p.seat >= 8 || p.vowner >= uint32_t(kMaxSlots)) return false;
  UnqPos(p.pos, c.vpos, p.vpos);
  UnqRows(c.vrows, (w.form & kFormCarMirror) != 0, p.vrows);
  for (int i = 0; i < 3; ++i) { p.vvel[i] = UnqVel(c.vvel[i]); p.vang[i] = UnqAng(c.vang[i]); }
  return true;
}
// WAP1: the model name (printable, up to 63) and the look block (up to 127).
inline size_t WriteAppearance(uint8_t version, const std::string& model, const uint8_t* look, size_t look_len, uint8_t* out, size_t cap) {
  if (model.size() > kMaxModelName || look_len > kMaxPlayerLook || cap < sizeof(AppearancePacket) + model.size() + look_len) return 0;
  AppearancePacket h{};
  SetMagic(h.magic, "WAP1");
  h.version = version; h.model_len = uint8_t(model.size()); h.look_len = uint8_t(look_len);
  std::memcpy(out, &h, sizeof(h));
  std::memcpy(out + sizeof(h), model.data(), model.size());
  if (look_len) std::memcpy(out + sizeof(h) + model.size(), look, look_len);
  return sizeof(h) + model.size() + look_len;
}
inline bool ReadAppearance(const uint8_t* d, size_t n, uint8_t& version, std::string& model, std::vector<uint8_t>& look) {
  AppearancePacket h;
  if (n < sizeof(h)) return false;
  std::memcpy(&h, d, sizeof(h));
  if (!HasMagic(h.magic, "WAP1") || !h.version || h.model_len > kMaxModelName || h.look_len > kMaxPlayerLook || n != sizeof(h) + h.model_len + h.look_len) return false;
  model.assign(reinterpret_cast<const char*>(d + sizeof(h)), h.model_len);
  for (char& c : model) if (c < 32 || c > 126) c = '_';
  look.assign(d + sizeof(h) + h.model_len, d + n);
  version = h.version;
  return true;
}
// A WAP1 older than the one held (the 5 s unreliable refresh overtaken by the
// next change's reliable copy in the same packet, or a reordered relay hop):
// up to 8 versions back (wrapping); further back is taken (the sender's game
// started over). 0 held: anything goes.
constexpr int kAppearanceStaleWindow = 8;
inline bool AppearanceStale(uint8_t got, uint8_t have) {
  if (!have || got == have) return false;
  const int d = int8_t(uint8_t(got - have));
  return d < 0 && d >= -kAppearanceStaleWindow;
}

// ---- The relay's pose rates per receiver (protocol 12) ----
// The session host passes a pose on to a player by their distance (2D, from
// the pose's position to the target's last pose through this relay): every
// pose within 60 m, 10 a second to 170 m, 5 to 500 m, 3 beyond. A tier changes
// 10 m past its boundary. By the sender's clock (the pose's time): a pose
// goes when kRelayTierMs - 8 ms passed since the last one passed to that
// target. Passed at once whatever the tier: a change of the state bits, a car
// entered or left, another car / seat / car handle while in one, a jump of
// over 40 m; and one tier nearer in a fast car (over 22 m/s, until it is
// under 18 again: per pair). A target whose position isn't known yet gets
// everything.
constexpr float kRelayTierM[3] = {60.0f, 170.0f, 500.0f};
constexpr uint32_t kRelayTierMs[4] = {0, 100, 200, 333};
constexpr float kRelayTierHystM = 10.0f, kRelayJumpM = 40.0f, kRelayFastOnMps = 22.0f, kRelayFastOffMps = 18.0f;
constexpr size_t kPoseCarAt = sizeof(PosePacket);   // (the relay reads the tail's vehicle, seat_owner and vid)
constexpr uint32_t kRelaySlackMs = 8;
// PlayerPose::flags state bits (coop.cpp kPose*, static_assert there): 4 in
// the car, 6 shared, 7 active, 16 ragdoll, 19 dead, 22 host world, 23 world
// stream / evacuate.
constexpr uint32_t kPoseStateBits = 1u << 4 | 1u << 6 | 1u << 7 | 1u << 16 | 1u << 19 | 1u << 22 | 1u << 23;
inline bool IsPose(const uint8_t* d, size_t n) { return (n == kPoseFoot || n == kPoseInCar) && HasMagic(d, "WPP2"); }
// The tier for a distance (2D, m), from the last one (-1: none yet).
inline int RelayTier(float d, int last) {
  int t = 0;
  while (t < 3 && d > kRelayTierM[t]) ++t;
  if (last < 0 || last == t) return t;
  // Moving out: past the boundary + hysteresis; in: under the boundary - hysteresis.
  if (t > last) { int k = last; while (k < 3 && d > kRelayTierM[k] + kRelayTierHystM) ++k; return k; }
  int k = last; while (k > 0 && d < kRelayTierM[k - 1] - kRelayTierHystM) --k; return k;
}
class RelayTiers {
 public:
  unsigned passed[4] = {}, held[4] = {}, forced = 0;   // poses passed / held back by tier, passed at once (counts: the caller reads and clears them)
  void Reset() { *this = RelayTiers{}; }
  void Forget(int slot) {   // a player joined or left in that slot
    if (slot < 0 || slot >= kMaxSlots) return;
    seen_[slot] = false;
    for (int s = 0; s < kMaxSlots; ++s) { pair_[slot][s] = Pair{}; pair_[s][slot] = Pair{}; }
  }
  // A pose from origin (its position, for the tiers of poses to it).
  void Note(int origin, const uint8_t* d, size_t n) {
    if (origin < 0 || origin >= kMaxSlots || !IsPose(d, n)) return;
    float p[3]; std::memcpy(p, d + kPosePosAt, sizeof(p));
    if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) return;
    std::memcpy(pos_[origin], p, sizeof(p)); seen_[origin] = true;
  }
  // Pass this message from origin to target now? (Only poses are ever held.)
  bool Pass(int origin, int target, const uint8_t* d, size_t n) {
    if (origin < 0 || origin >= kMaxSlots || target < 0 || target >= kMaxSlots || origin == target || !IsPose(d, n)) return true;
    Note(origin, d, n);
    if (!seen_[origin]) return true;
    Pair& pr = pair_[origin][target];
    uint32_t time, flags; float p[3];
    std::memcpy(&time, d + kPoseTimeAt, 4); std::memcpy(&flags, d + kPoseFlagsAt, 4); std::memcpy(p, d + kPosePosAt, sizeof(p));
    const bool car = n == kPoseInCar && (d[kPoseFormAt] & kFormCar) != 0;   // (a 48-byte pose that says car has no tail to read: review_memory_steps L7)
    uint32_t vid = 0; uint16_t vseat = 0;   // (vehicle | seat_owner << 8)
    if (car) { std::memcpy(&vid, d + kPoseCarAt + offsetof(PoseCar, vid), 4); vseat = uint16_t(d[kPoseCarAt] | d[kPoseCarAt + 1] << 8); }
    int tier = 0;
    if (seen_[target]) {
      const float dx = p[0] - pos_[target][0], dz = p[2] - pos_[target][2];
      tier = RelayTier(std::sqrt(dx * dx + dz * dz), pr.any ? pr.tier : -1);
    }
    bool fast = false;
    if (car) {
      int16_t v[3]; std::memcpy(v, d + kPoseVelAt, sizeof(v));
      const float sx = UnqVel(v[0]), sy = UnqVel(v[1]), sz = UnqVel(v[2]);
      const float lim = pr.fast ? kRelayFastOffMps : kRelayFastOnMps;
      fast = sx * sx + sy * sy + sz * sz > lim * lim;
    }
    pr.fast = fast;
    int eff = tier;
    if (fast && eff > 0) --eff;
    bool go = !pr.any || !seen_[target];
    bool now = false;
    if (!go) {
      const int32_t dt = int32_t(time - pr.time);
      if (dt < 0 && dt >= -2000) { ++held[eff]; return false; }   // (older than the last one passed: the receiver drops it anyway; more than 2 s back: the sender's game started over)
      const float jx = p[0] - pr.pos[0], jy = p[1] - pr.pos[1], jz = p[2] - pr.pos[2];
      now = (flags & kPoseStateBits) != (pr.flags & kPoseStateBits) || car != pr.car || (car && (vid != pr.vid || vseat != pr.vseat)) ||
            jx * jx + jy * jy + jz * jz > kRelayJumpM * kRelayJumpM || dt < 0;
      go = now || time - pr.time + kRelaySlackMs >= kRelayTierMs[eff];
    }
    pr.tier = uint8_t(tier);
    if (!go) { ++held[eff]; pr.any = true; return false; }
    if (now) ++forced;
    ++passed[eff];
    pr.any = true; pr.time = time; pr.flags = flags; pr.car = car; pr.vid = vid; pr.vseat = vseat; std::memcpy(pr.pos, p, sizeof(p));
    return true;
  }

 private:
  struct Pair { bool any = false, car = false, fast = false; uint8_t tier = 0; uint16_t vseat = 0; uint32_t time = 0, flags = 0, vid = 0; float pos[3]{}; };
  float pos_[kMaxSlots][3]{};
  bool seen_[kMaxSlots]{};
  Pair pair_[kMaxSlots][kMaxSlots]{};
};

// ---- Protocol 13: emote sets and voice lines (kEventAnimSet, kEventVoice; coop.cpp) ----
// research notes/coop-3/emotes_voice_research.md 3.

// A name as the game compares them (82229278 sets, 8221A5C0 personas: without case): FNV-1a of the lower-cased
// name, at most max chars. 0 for an empty name.
inline uint32_t NameHashNoCase(const char* s, size_t max = 64) {
  if (!s || !*s) return 0;
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < max && s[i]; ++i) {
    const uint8_t c = uint8_t(s[i] >= 'A' && s[i] <= 'Z' ? s[i] - 'A' + 'a' : s[i]);
    h = (h ^ c) * 16777619u;
  }
  return h ? h : 1;
}

// kEventAnimSet: a[0] the set's index in the table (0x82C06D58, 12 B each), a[1] its name's hash (a game with
// another table, the SR Tweak Menu's SRT_Player added, finds it by name), a[2] 1 borrowed (an emote plays through
// it), 0 the player's own (or a mod's: put the character's own back).
struct AnimSetMsg { uint32_t index = 0, name_hash = 0; bool borrowed = false; };
inline void PackAnimSet(const AnimSetMsg& m, uint32_t a[8]) {
  for (int i = 0; i < 8; ++i) a[i] = 0;
  a[0] = m.index; a[1] = m.name_hash; a[2] = m.borrowed ? 1u : 0u;
}
inline AnimSetMsg UnpackAnimSet(const uint32_t a[8]) {
  AnimSetMsg m; m.index = a[0]; m.name_hash = a[1]; m.borrowed = (a[2] & 1) != 0 && a[1] != 0;   // (no name: nothing to borrow)
  return m;
}

// kEventVoice: a line our player said (SAY_LINE 82219298 returned 1). a[0] the situation (0-206: 824C7AE0's
// limit), a[1] the persona's index (0x82B3E420, 960 B each), a[2] r6 as passed (-1: the persona's own sound set for
// the situation), a[3] r5, a[4] r7 the distance class (-1: the situation's), a[5] the persona name's hash, a[6] the
// sound set it used (for the log).
constexpr uint32_t kVoiceSituationMax = 206;
struct VoiceMsg { uint32_t situation = 0, persona = 0, persona_hash = 0; int32_t sound_set = -1, extra = -1, distance_class = -1, used_set = -1; };
inline void PackVoice(const VoiceMsg& v, uint32_t a[8]) {
  for (int i = 0; i < 8; ++i) a[i] = 0;
  a[0] = v.situation; a[1] = v.persona; a[2] = uint32_t(v.sound_set); a[3] = uint32_t(v.extra); a[4] = uint32_t(v.distance_class);
  a[5] = v.persona_hash; a[6] = uint32_t(v.used_set);
}
// False for a line nobody here can say: a situation out of range, no persona name. r5 / r7 that aren't small
// numbers (an address in the sender's game means nothing here) go as -1.
inline bool UnpackVoice(const uint32_t a[8], VoiceMsg& v) {
  v = VoiceMsg{};
  if (a[0] > kVoiceSituationMax || !a[5]) return false;
  v.situation = a[0]; v.persona = a[1]; v.persona_hash = a[5];
  v.sound_set = int32_t(a[2]); v.used_set = int32_t(a[6]);
  v.extra = int32_t(a[3]) >= -1 && int32_t(a[3]) < 0x10000 ? int32_t(a[3]) : -1;
  v.distance_class = int32_t(a[4]) >= -1 && int32_t(a[4]) < 256 ? int32_t(a[4]) : -1;
  return true;
}
// One speaker's lines: at most per_sec in any second, and a repeat of the same situation within repeat_ms dropped
// (pain on every pellet of a shotgun). The sender: 4 a second, 300 ms; the receiver (per sender): 6, none.
class VoiceGate {
 public:
  unsigned dropped_rate = 0, dropped_repeat = 0;   // (counts: the caller reads and clears them)
  explicit VoiceGate(unsigned per_sec = 4, uint32_t repeat_ms = 300) : per_sec_(std::clamp(per_sec, 1u, 8u)), repeat_ms_(repeat_ms) {}
  bool Allow(uint32_t situation, uint64_t now) {
    if (repeat_ms_ && any_ && situation == last_situation_ && now - last_at_ < repeat_ms_) { ++dropped_repeat; return false; }
    if (count_ >= per_sec_ && now - times_[next_] < 1000) { ++dropped_rate; return false; }   // (the oldest of the last per_sec)
    times_[next_] = now; next_ = (next_ + 1) % per_sec_; if (count_ < per_sec_) ++count_;
    any_ = true; last_situation_ = situation; last_at_ = now;
    return true;
  }

 private:
  unsigned per_sec_, count_ = 0, next_ = 0;
  uint32_t repeat_ms_, last_situation_ = 0;
  uint64_t times_[8]{}, last_at_ = 0;
  bool any_ = false;
};
// A line goes to the players within the relay's second tier (2D, 170 m: the game's own cull by its distance class
// runs again on the receiver); a player whose position isn't known yet gets it.
constexpr float kVoiceRangeM = kRelayTierM[1];
inline bool VoiceInRange(const float* speaker, const float* listener) {
  const float dx = speaker[0] - listener[0], dz = speaker[2] - listener[2];
  const float d2 = dx * dx + dz * dz;
  return !std::isfinite(d2) || d2 <= kVoiceRangeM * kVoiceRangeM;
}

}  // namespace coopworld
