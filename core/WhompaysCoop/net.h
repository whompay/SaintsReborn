// Co-op v2 network layer: a session of up to 12 players over one datagram
// transport (Epic P2P or UDP), modelled on the game's own multiplayer (report Q):
// a star, where the session host (slot 0 when the session starts) talks to
// every client and relays what a client sends to everyone else.
//
// Host migration (co-op 3 phase 4, EnableMigration): the session host is a
// role, not slot 0. When it leaves (its kLeave, or 6 s of silence) every
// client picks the same successor: the most senior player in the succession
// order the session host shared in advance (kPeerAddr: every client's address,
// its seniority and a hash of its token). The successor becomes the session
// host and the relay, and the others link to it again (kRejoin, with the
// token) at the shared address. A successor that doesn't answer is passed
// over for the next one. Slots and names stay as they were; what a client had
// queued for the old host and never saw acked goes to the new one (each
// reliable game message carries its origin's own count, so nothing is
// delivered twice). Clients also send each other a bare packet once a second
// (warm links: Epic opens its P2P connections, NATs open their ports) so the
// re-link is quick; a session host that took over sends them too, so two
// session hosts after a split find each other and the less senior one yields.
//
// A link (host <-> one client) carries packets of at most kMaxPacket bytes: a
// header with acks for the other side's last 33 packets, then messages.
// Reliable messages are kept until a packet carrying them is acked, resent
// after a round trip, and delivered in order. Unreliable ones go once (state
// snapshots, where the newest wins). Every message names its origin slot and
// its target (one slot or everyone), so the host can relay it.
//
// No game code here: the same file runs in the mod and in the stand-alone test
// (tests/net_test.cpp). Pump() runs on the network thread; Send*/Poll on the
// game thread; a mutex covers the queues.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <vector>

namespace coopnet {

constexpr int kMaxSlots = 12;            // host + 11 clients (the game's player pool)
constexpr uint8_t kVersion = 13;         // protocol version: a mismatch is refused at join (5: co-op 2.1.2, 6: 2.2's world stream, 7: 2.2 with Package C's records and events, 8: 2.2.8's kEventCarWrecked, 9: 2.2.18's people's stance in PedRec, 10: co-op 3 phase 1a's owners and NetIds in WWC1 / WWR1 / WWA1 and the person events, 11: co-op 3 phase 4's host migration and MessageHeader.oseq, 12: the compact pose WPP2, WAP1 and the relay's pose tiers, 13: voice lines and emote sets, kEventVoice / kEventAnimSet)
constexpr uint32_t kMagic = 0x32435253;  // "SRC2"
constexpr int kMaxPacket = 1170;         // Epic's P2P limit (EOS_P2P_MAX_PACKET_SIZE)
constexpr int kMaxMessage = kMaxPacket - 16 - 10;  // payload of one message (headers below)
constexpr uint8_t kEveryone = 0xFF;
constexpr uint8_t kNoSlot = 0xFE;
constexpr uint64_t kTimeoutMs = 6000;    // no packet for this long: the peer is gone
constexpr uint64_t kKeepAliveMs = 200;   // a packet at least this often (carries the acks)
constexpr uint64_t kJoinRetryMs = 500;
constexpr uint64_t kRejoinMs = 7000;       // migration: a client tries a successor this long, then the next one (none left: it joins the old host's address again, as before)
constexpr uint64_t kRejoinWaitMs = 20000;  // migration: the new host waits this long for each client to link again
constexpr uint64_t kWarmMs = 1000;         // migration: a bare packet to every other client this often
constexpr uint64_t kListMs = 3000;         // migration: a new player's address is shared this long before it is in the succession order
constexpr uint64_t kWarmHostMs = 60000;    // migration: a session host that took over warms a player that left (another host's, after a split) this long
constexpr int kMaxPacketsPerFlush = 8;   // per link and Pump; the rest waits for the next Pump
constexpr size_t kMaxUnreliableBacklog = 48 * 1024;  // per link: older unreliable messages beyond this are dropped
constexpr uint16_t kRelWindow = 1024;    // reliable messages in flight per link (the receiver holds up to twice that)
constexpr size_t kMaxRelBacklog = 16384; // reliable messages waiting on one link: past this the link is dropped
constexpr size_t kMaxName = 32;          // bytes of a player name

// Session messages (types below kFirstUserType are the session's own).
enum : uint8_t {
  kJoin = 1,        // client -> host, unreliable, repeated: u8 version, u64 token, name
  kJoinOk = 2,      // host -> client, reliable: u8 your slot, then u8 count, {u8 slot, u8 len, name}, then u8 the session's player cap (3.0 max_players; part of protocol 13)
  kJoinDeny = 3,    // host -> client, unreliable: reason text
  kPeerJoined = 4,  // host -> clients, reliable: u8 slot, name
  kPeerLeft = 5,    // host -> clients, reliable: u8 slot, u8 why (kLeftLeave / kLeftTimedOut; inbox: data[0] why)
  kLeave = 6,       // client -> host, unreliable, sent a few times (migration: the host -> clients too, with u8 the session host to link to instead when it yields; a client -> a client linking to it, on epoch 0: u8 its session host, u8 1 = "still here")
  kRejoin = 7,      // client -> the new session host, unreliable, repeated: u8 version, u8 its slot, u64 token, name (migration)
  kPeerAddr = 8,    // host -> clients, reliable, at a change: {u8 slot, u8 len, address, u32 seniority (0: not in the succession order yet), u64 token hash} for every client (migration)
  kHostMoved = 9,   // (inbox only) the session host left and slot `origin` took over: u8 the old one's slot (its kPeerLeft follows, unless it yielded)
  kRejoined = 10,   // (inbox only) a re-link after a migration: a client's to the new host (origin = the host), or the new host's from a client (origin = that client)
  kFirstUserType = 16,
};
// kPeerLeft's reason (review_final_shared H1): the player said it was leaving,
// or its link was lost (timed out, fell behind, joined again): it may be back
// in a moment as a new join.
enum : uint8_t { kLeftLeave = 0, kLeftTimedOut = 1 };

#pragma pack(push, 1)
struct PacketHeader {
  uint32_t magic;
  uint8_t version, slot;   // sender's slot (kNoSlot before a client has one)
  uint8_t flags;           // bit 0: ack / ack_bits are valid (the sender has received something); a warm packet: bit 1, the sender is a session host (ack_bits: its seniority)
  uint8_t epoch;           // the client's join attempt (both ways): a client that starts over gets a new one (0: a warm packet)
  uint16_t seq;            // this packet
  uint16_t ack;            // the newest packet received from the other side
  uint32_t ack_bits;       // bit i: packet ack-1-i received too
};
struct MessageHeader {
  uint8_t type, origin, target, flags;  // flags bit 0: reliable
  uint16_t id;                          // reliable: per-link message id
  uint16_t len;
  uint16_t oseq;                        // reliable game messages: the origin's own count, kept through relays (0 otherwise)
};
#pragma pack(pop)
static_assert(sizeof(PacketHeader) == 16 && sizeof(MessageHeader) == 10, "wire format");

inline bool SeqNewer(uint16_t a, uint16_t b) { return int16_t(uint16_t(a - b)) > 0; }

// Join attempts of this game, each newer than the last (1..255, wrapping; the
// host compares them as signed differences). A restarted game starts at a
// random one; the host then waits for the old link to time out at worst.
inline uint8_t NextEpoch() {
  static std::atomic<uint32_t> counter{uint32_t(std::chrono::steady_clock::now().time_since_epoch().count())};
  return uint8_t(1 + counter.fetch_add(1) % 255);
}

// (Migration) A client's secret for its re-link: sent to the session host in
// kJoin, shared with the other clients only as TokenHash (SHA-256, the first
// 8 bytes), so nobody else can re-link as that player.
inline uint64_t NewToken() {
  std::random_device rd;
  const uint64_t r = (uint64_t(rd()) << 32) ^ rd();
  return r ^ uint64_t(std::chrono::steady_clock::now().time_since_epoch().count()) * 0x9E3779B97F4A7C15ull;
}
inline uint64_t TokenHash(uint64_t token) {
  static const uint32_t k[64] = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
      0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
      0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
      0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
      0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
      0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
  auto rotr = [](uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
  uint8_t b[64]{};
  std::memcpy(b, &token, 8);
  b[8] = 0x80; b[63] = 64;   // (one block: the 8 bytes, the end bit, the length in bits)
  uint32_t w[64];
  for (int i = 0; i < 16; ++i) w[i] = uint32_t(b[4 * i]) << 24 | uint32_t(b[4 * i + 1]) << 16 | uint32_t(b[4 * i + 2]) << 8 | b[4 * i + 3];
  for (int i = 16; i < 64; ++i) {
    const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  uint32_t v[8];
  std::memcpy(v, h, sizeof(v));
  for (int i = 0; i < 64; ++i) {
    const uint32_t t1 = v[7] + (rotr(v[4], 6) ^ rotr(v[4], 11) ^ rotr(v[4], 25)) + ((v[4] & v[5]) ^ (~v[4] & v[6])) + k[i] + w[i];
    const uint32_t t2 = (rotr(v[0], 2) ^ rotr(v[0], 13) ^ rotr(v[0], 22)) + ((v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]));
    v[7] = v[6]; v[6] = v[5]; v[5] = v[4]; v[4] = v[3] + t1; v[3] = v[2]; v[2] = v[1]; v[1] = v[0]; v[0] = t1 + t2;
  }
  return uint64_t(h[0] + v[0]) << 32 | (h[1] + v[1]);
}

struct Received {
  uint8_t origin = 0, type = 0;
  std::vector<uint8_t> data;
};

struct Stats {
  uint32_t packets_out = 0, packets_in = 0, bytes_out = 0, bytes_in = 0;
  uint32_t resent = 0, dropped_unreliable = 0, bad_packets = 0;
  uint32_t host_moves = 0, rejoined = 0, rejoin_lost = 0, carried = 0, warm = 0;   // migration: the session host changed, re-links made / successors given up on, reliable messages carried to the new host, warm packets out
  uint32_t duplicates = 0;      // reliable game messages that came again (carried over a migration) and were dropped
  uint32_t rejoin_refused = 0;  // migration: kRejoin from the wrong address or with the wrong token
  uint32_t yields = 0;          // migration: this game gave the session host to a more senior one (a split)
  uint32_t relayed = 0, relayed_bytes = 0, relay_thinned = 0;   // the session host: messages passed on to other clients (bytes with their headers), unreliable ones the RelayFilter held back
  uint32_t still_with_host = 0; // migration (review_final_shared H1): told a game linking to this one that the session host is still here / told so by a successor (this game joins its host again)
  // (protocol 12) Bytes by game message type (kFirstUserType + i, headers
  // included): this game's own messages as queued (the host: once per link),
  // and the ones the session host passed on.
  static constexpr int kTypes = 32;
  uint32_t sent_type[kTypes] = {}, relayed_type[kTypes] = {};
};
// One link's state (the host: one per client; a client: its link to the
// host), for a sender that paces itself by it (2.2's world stream halves a
// member's budget while its backlog grows or its snapshots are dropped).
struct LinkStat {
  float rtt = 0;                  // ms, smoothed
  uint32_t backlog = 0;             // bytes of unreliable messages waiting for the next flush
  uint32_t in_flight = 0;           // reliable messages queued or sent and not acked yet
  uint32_t in_flight_bytes = 0;
  uint32_t dropped = 0;             // unreliable messages dropped past kMaxUnreliableBacklog, since the link began
};

class Session {
 public:
  using SendFn = std::function<int(const void*, int, const sockaddr_in&)>;
  using RecvFn = std::function<int(char*, int, sockaddr_in&)>;
  using LogFn = std::function<void(const std::string&)>;

  Session(SendFn send, RecvFn recv, LogFn log = nullptr) : send_(std::move(send)), recv_(std::move(recv)), log_(std::move(log)) {}
  // Called with a link's address when it is dropped (2.4: Epic closes the
  // connection, so what was queued for a player who is gone stops filling the
  // shared send queue).
  using DropFn = std::function<void(const sockaddr_in&)>;
  void OnDrop(DropFn f) { std::lock_guard l(m_); drop_ = std::move(f); }
  // Host migration on (before StartHost / StartClient; kept across them). A
  // client follows its session host's setting: the host's kPeerAddr turns it
  // on for that session.
  void EnableMigration(bool on) { std::lock_guard l(m_); migrate_ = on; }
  // How kPeerAddr writes and reads an address (Epic: the product user ID, as
  // each game maps IDs to stand-in addresses of its own). Default: the IPv4
  // address and port, 6 bytes.
  using AddrOutFn = std::function<std::string(const sockaddr_in&)>;
  using AddrInFn = std::function<bool(const std::string&, sockaddr_in&)>;
  void AddressCodec(AddrOutFn out, AddrInFn in) { std::lock_guard l(m_); addr_out_ = std::move(out); addr_in_ = std::move(in); }
  // (review_final_shared M8) The relay asks this before passing an
  // unreliable message on to one client (`target`): false holds it back for
  // that client (the game thins far players' poses). Called on the network
  // thread inside Pump, with the session locked: it must not call back in.
  // (Protocol 12) Also for the session host's own unreliable messages (Send,
  // origin = its slot), and once per unreliable message from a client with
  // target = kEveryone, which only notes it (the answer is ignored).
  using RelayFilterFn = std::function<bool(uint8_t type, uint8_t origin, uint8_t target, const uint8_t* data, uint16_t len)>;
  void RelayFilter(RelayFilterFn f) { std::lock_guard l(m_); relay_filter_ = std::move(f); }
  // (Protocol 12) A link's packets with only unreliable messages go at most
  // once per `ms` (0: at every Pump): with small poses the datagrams' own
  // headers were as large as what they carried. Reliable messages due go at
  // once (with what is queued).
  void PaceUnreliable(uint32_t ms) { std::lock_guard l(m_); pace_ms_ = ms; }
  // (3.0, mod.ini max_players) The session host takes at most this many
  // players, itself included (2-12); a join past it is refused. The slots
  // stay 12 (a session that moves to a host with a lower cap keeps everyone
  // linking again). A client learns its host's cap from kJoinOk (SessionMax).
  void MaxPlayers(int n) { std::lock_guard l(m_); max_players_ = n < 2 ? 2 : n > kMaxSlots ? kMaxSlots : n; }
  // The session's cap: the host's own; a client, the one its host sent (its
  // own until then, or with a host that sends none).
  int SessionMax() const { std::lock_guard l(m_); return host_ || !host_max_ ? max_players_ : host_max_; }

  // Host: the session's first slot (0), accepts joins. Client: joins the host at `host`.
  void StartHost(const std::string& name) {
    std::lock_guard l(m_);
    Reset();
    host_ = true; my_slot_ = 0; host_slot_ = 0; names_[0] = name.substr(0, kMaxName); present_[0] = true; my_name_ = names_[0];
    rank_[0] = next_rank_ = 1;   // (the most senior: the clients' seniority counts on from it)
    my_token_ = NewToken();
  }
  void StartClient(const sockaddr_in& host, const std::string& name) {
    std::lock_guard l(m_);
    Reset();
    host_ = false; my_slot_ = kNoSlot; names_[0].clear(); my_name_ = name.substr(0, kMaxName);
    // A new join attempt: the same UDP socket or Epic address can come back
    // while the host still has the last one.
    epoch_ = NextEpoch();
    my_token_ = NewToken();
    Link& k = links_[0]; k.used = true; k.addr = host; k.slot = 0;
  }
  // Leave: a client tells the host (three packets, no acks wait); then everything is cleared.
  // (Migration: the host tells every client, five packets, which then move on
  // at once; a client still linking to it again gets five of its own.)
  void Stop(uint64_t now) {
    std::lock_guard l(m_);
    if (!host_ && links_[0].used && !links_[0].pending && my_slot_ != kNoSlot)
      for (int i = 0; i < 3; ++i) { SendControl(links_[0], kLeave, nullptr, 0, false); Flush(links_[0], now); }
    if (host_ && Migrating())
      for (int s = 0; s < kMaxSlots; ++s) {
        Link& k = links_[s];
        if (!k.used) continue;
        if (!k.pending) for (int i = 0; i < 5; ++i) { SendControl(k, kLeave, nullptr, 0, false); Flush(k, now); }   // (five: a client that misses them all moves on 6 s later)
        else if (peer_ok_[s]) { k.addr = peer_addr_[s]; for (int i = 0; i < 5; ++i) SendAlone(k, kLeave, {}, now); }   // (review_phase4 M2: it tries the next successor at once, not after kRejoinMs)
      }
    Reset();
  }

  bool IsHost() const { std::lock_guard l(m_); return host_; }
  // The session host's slot (0 until a migration moves it; a client that
  // isn't joined: the one it tries).
  uint8_t HostSlot() const { std::lock_guard l(m_); return host_slot_; }
  // A migration is under way: a client links to the new host, or the new
  // host waits for clients to link again.
  bool Relinking() const { std::lock_guard l(m_); if (relink_) return true; for (auto& k : links_) if (k.used && k.pending) return true; return false; }
  // Client: the session host's address (Epic: the lobby goes to it).
  sockaddr_in HostAddress() const { std::lock_guard l(m_); return host_ || !links_[0].used ? sockaddr_in{} : links_[0].addr; }
  // Client: when the last packet from the session host came (0: none, or a host).
  uint64_t HostHeardAt() const { std::lock_guard l(m_); return host_ || !links_[0].used || links_[0].pending ? 0 : links_[0].last_recv; }
  uint8_t MySlot() const { std::lock_guard l(m_); return my_slot_; }
  bool Joined() const { std::lock_guard l(m_); return my_slot_ != kNoSlot; }
  bool Present(int slot) const { std::lock_guard l(m_); return slot >= 0 && slot < kMaxSlots && present_[slot]; }
  std::string Name(int slot) const { std::lock_guard l(m_); return slot >= 0 && slot < kMaxSlots ? names_[slot] : std::string(); }
  std::string DenyReason() const { std::lock_guard l(m_); return deny_reason_; }
  int PlayerCount() const { std::lock_guard l(m_); int n = 0; for (bool p : present_) n += p; return n; }
  Stats TakeStats() { std::lock_guard l(m_); Stats s = stats_; stats_ = {}; return s; }
  // The link to `slot` (a client: its host's), all zero when there is none.
  LinkStat LinkStats(int slot) const {
    std::lock_guard l(m_);
    LinkStat s;
    const int i = host_ ? slot : slot == host_slot_ ? 0 : -1;
    if (slot < 0 || slot >= kMaxSlots || i < 0 || !links_[i].used || links_[i].pending) return s;
    const auto& k = links_[i];
    s.rtt = k.rtt; s.backlog = uint32_t(k.unrel.size()); s.in_flight = uint32_t(k.rel_out.size()); s.dropped = k.dropped;
    s.in_flight_bytes = k.rel_bytes;
    return s;
  }

  // Game thread: queue a message for one slot or everyone (kEveryone). Clients
  // send through the host. Returns false when it can't be queued (too big, not joined).
  bool Send(uint8_t target, uint8_t type, const void* data, uint16_t len, bool reliable) {
    if (len > kMaxMessage) return false;
    std::lock_guard l(m_);
    if (my_slot_ == kNoSlot) return false;
    MessageHeader h{type, my_slot_, target, uint8_t(reliable ? 1 : 0), 0, len, 0};
    if (reliable && type >= kFirstUserType) h.oseq = ++oseq_out_;
    uint32_t* by_type = type >= kFirstUserType && type < kFirstUserType + Stats::kTypes ? &stats_.sent_type[type - kFirstUserType] : nullptr;
    if (host_) {
      // (protocol 12) The session host's own unreliable messages go through
      // the relay's filter too, per client (its pose by their distance).
      const bool filter = !reliable && relay_filter_ && type >= kFirstUserType;
      const uint8_t* bytes = static_cast<const uint8_t*>(data);
      if (filter) relay_filter_(type, my_slot_, kEveryone, bytes, len);   // (kEveryone: only noted)
      for (int s = 0; s < kMaxSlots; ++s)
        if (s != my_slot_ && links_[s].used && present_[s] && (target == kEveryone || target == s)) {
          if (filter && !relay_filter_(type, my_slot_, uint8_t(s), bytes, len)) { ++stats_.relay_thinned; continue; }
          Queue(links_[s], h, data);
          if (by_type) *by_type += uint32_t(sizeof(h) + len);
        }
    } else if (links_[0].used) {
      Queue(links_[0], h, data);
      if (by_type) *by_type += uint32_t(sizeof(h) + len);
    }
    return true;
  }

  // Game thread: the next received message (session and user types).
  bool Poll(Received& out) {
    std::lock_guard l(m_);
    if (inbox_.empty()) return false;
    out = std::move(inbox_.front());
    inbox_.pop_front();
    return true;
  }

  // Network thread: read everything waiting, then send what is due.
  void Pump(uint64_t now) {
    std::lock_guard l(m_);
    last_now_ = now;
    char buf[2048];
    for (int n = 0; n < 256; ++n) {
      sockaddr_in from{};
      const int got = recv_(buf, sizeof(buf), from);
      if (got <= 0) break;
      OnDatagram(reinterpret_cast<const uint8_t*>(buf), got, from, now);
    }
    // (The host's kLeave, handled here: not inside the packet that carried it.
    // A successor this game was still linking to: the next one.)
    if (host_left_) {
      host_left_ = false;
      const int to = host_left_to_;
      host_left_to_ = -1;
      if (!host_ && my_slot_ != kNoSlot && links_[0].used) {
        if (to < 0) left_at_[host_slot_] = now;   // (its warm packets still on the way are no session host's)
        if (links_[0].pending) Failover(now, "left", true);
        else HostGone(now, to >= 0 ? "gave the session to a more senior host" : "left", to);
      }
    }
    if (yield_to_ >= 0) { const int to = yield_to_; yield_to_ = -1; if (host_) GiveWay(now, to, yield_addr_); }
    // (review_final_shared H1) The successor this game links to still hears
    // the session host: only this game's link to it failed. It joins that
    // host again now (a new join) instead of trying every successor.
    if (host_alive_) {
      host_alive_ = false;
      if (!host_ && my_slot_ != kNoSlot && links_[0].used && links_[0].pending && host_timed_out_) { ++stats_.still_with_host; LoseSession(lost_host_, "a successor still hears the session host: only this game's link to it failed"); }
    }
    // (A different link first each time: with a full outgoing queue the
    // last ones would never get a turn.)
    flush_start_ = (flush_start_ + 1) % kMaxSlots;
    for (int i = 0; i < kMaxSlots; ++i) {
      const int s = (flush_start_ + i) % kMaxSlots;
      Link& k = links_[s];
      if (!k.used) continue;
      if (k.pending) {   // (migration)
        if (host_) { if (now - k.last_recv > kRejoinWaitMs) Drop(s, "didn't link again after the host left"); continue; }
        if (now - k.last_recv > kRejoinMs) { ++stats_.rejoin_lost; Failover(now, "didn't answer", false); continue; }
        if (!k.join_sent || now - k.join_sent >= kJoinRetryMs) {
          k.join_sent = now;
          std::vector<uint8_t> j{kVersion, my_slot_};
          j.resize(10); std::memcpy(&j[2], &my_token_, 8);
          j.insert(j.end(), my_name_.begin(), my_name_.end());
          SendAlone(k, kRejoin, j, now);
        }
        continue;
      }
      if (k.last_recv && now - k.last_recv > kTimeoutMs) {
        // A join attempt that has heard nothing yet keeps asking on the same
        // attempt (same epoch, same packet numbers): a host that still hears
        // us keeps its link and resends kJoinOk, instead of taking us for a
        // new player every 6 s (everyone saw "left" / "joined" over and over).
        if (!host_ && my_slot_ == kNoSlot && !k.recv_any) k.last_recv = now;
        else if (!host_ && Migrating() && my_slot_ != kNoSlot) { HostGone(now, "timed out"); continue; }
        else { Drop(s, "timed out"); continue; }
      }
      if (k.rel_out.size() > kMaxRelBacklog) { Drop(s, "fell too far behind"); continue; }
      if (!host_ && s == 0 && my_slot_ == kNoSlot && (!k.join_sent || now - k.join_sent >= kJoinRetryMs)) {
        k.join_sent = now;
        std::vector<uint8_t> j{kVersion};
        j.resize(9); std::memcpy(&j[1], &my_token_, 8);
        j.insert(j.end(), my_name_.begin(), my_name_.end());
        SendControl(k, kJoin, j.data(), uint16_t(j.size()), false);
        if (!k.last_recv) k.last_recv = now;  // start the join timeout
      }
      if (pace_ms_ && k.last_send && now - k.last_send < pace_ms_ && !ReliableDue(k, now)) continue;   // (protocol 12: paced)
      Flush(k, now);
    }
    if (host_ && rejoin_wait_) {
      // (Migration, review_phase4 L5) A new host nobody linked to while the
      // clients it waits for still warm it as clients do: they are with the
      // old host, only this game's link to it failed. It joins that one again.
      bool warmed = false;
      for (int s = 0; s < kMaxSlots; ++s) warmed |= links_[s].used && links_[s].pending && warm_in_[s] && now - warm_in_[s] < 1500;
      if (!rejoined_any_ && warmed && now - rejoin_wait_ > kTimeoutMs + 2000) { rejoin_wait_ = 0; LoseSession(lost_host_, "the others are still with the old session host"); }
    }
    // (Migration) A new host nobody linked to again: most likely only its own
    // link to the old host failed, so it joins that one again.
    if (host_ && rejoin_wait_ && now - rejoin_wait_ > kRejoinWaitMs) {
      rejoin_wait_ = 0;
      int others = 0;
      for (int s = 0; s < kMaxSlots; ++s) others += s != my_slot_ && links_[s].used;
      if (!others && !rejoined_any_) LoseSession(lost_host_, "nobody linked to this game as the new session host");
    }
    // (Migration, review_phase4 M3) A player is in the succession order once
    // its address has been shared for kListMs, the longest there first.
    if (host_ && Migrating()) {
      bool listed = false;
      for (;;) {
        int s = -1;
        for (int o = 0; o < kMaxSlots; ++o)
          if (list_due_[o] && now >= list_due_[o] && (s < 0 || list_due_[o] < list_due_[s])) s = o;
        if (s < 0) break;
        list_due_[s] = 0;
        if (links_[s].used && !links_[s].pending && present_[s]) { rank_[s] = ++next_rank_; listed = true; }
      }
      if (listed) ShareAddresses();
    }
    // Warm links: a bare packet (no messages, epoch 0) to every other client
    // whose address the host shared; a session host that took over sends
    // them (flagged, with its seniority) to the players not linked to it.
    if (Migrating() && my_slot_ != kNoSlot && now >= next_warm_ && (host_ || (links_[0].used && !links_[0].pending && !relink_))) {
      next_warm_ = now + kWarmMs;
      for (int s = 0; s < kMaxSlots; ++s) {
        if (s == my_slot_ || !peer_ok_[s]) continue;
        if (host_) { if (links_[s].used ? !links_[s].pending : !freed_at_[s] || now - freed_at_[s] > kWarmHostMs) continue; }
        else if (s == host_slot_ || !present_[s]) continue;
        const PacketHeader ph{kMagic, kVersion, my_slot_, uint8_t(host_ ? 2 : 0), 0, 0, 0, host_ ? rank_[my_slot_] : 0};
        send_(&ph, int(sizeof(ph)), peer_addr_[s]);
        ++stats_.warm; ++stats_.packets_out; stats_.bytes_out += uint32_t(sizeof(ph));
      }
    }
  }

 private:
  struct Out { uint16_t id; std::vector<uint8_t> bytes; uint64_t sent_at = 0; };
  struct Sent { bool used = false, acked = false; uint16_t seq = 0; uint64_t at = 0; std::vector<uint16_t> ids; };
  struct Link {
    bool used = false;
    bool pending = false;   // migration: a client's link to the new host before its first answer, or the new host's for a client not back yet (no address): reliable messages wait, unreliable ones are dropped
    sockaddr_in addr{};
    uint8_t slot = kNoSlot;
    uint64_t last_recv = 0, last_send = 0, join_sent = 0;
    uint8_t epoch = 0;                                   // host: the client's join attempt this link belongs to
    uint16_t send_seq = 0;
    bool recv_any = false; uint16_t recv_latest = 0; uint32_t recv_bits = 0;
    uint16_t rel_next = 0, rel_expect = 0;
    std::deque<Out> rel_out;
    uint32_t rel_bytes = 0;                              // what rel_out holds (LinkStat, kept as it changes)
    std::map<uint16_t, std::vector<uint8_t>> rel_hold;  // reliable messages that came early
    std::vector<Sent> sent = std::vector<Sent>(4096);   // by packet seq & 4095 (outlives a full send window)
    std::vector<uint8_t> unrel;                          // whole messages for the next flush
    float rtt = 150.0f;
    uint32_t dropped = 0;                                // unreliable messages dropped (LinkStat)
  };

  void Reset() {
    for (auto& k : links_) k = Link{};
    for (auto& n : names_) n.clear();
    present_.fill(false);
    peer_ok_.fill(false);
    oseq_any_.fill(false);
    rank_.fill(0); list_due_.fill(0); token_hash_.fill(0); freed_at_.fill(0); warm_in_.fill(0); left_at_.fill(0); failed_.fill(false);
    inbox_.clear();
    my_slot_ = kNoSlot; host_slot_ = 0; host_max_ = 0; deny_reason_.clear();
    rejoin_wait_ = 0; rejoined_any_ = false; host_left_ = false; host_alive_ = false; host_timed_out_ = false; host_left_to_ = -1; yield_to_ = -1; next_warm_ = 0;
    relink_ = false; migrate_by_host_ = false; next_rank_ = 0; moved_from_ = kNoSlot; lost_host_slot_ = kNoSlot;
  }
  void Log(const std::string& s) { if (log_) log_(s); }
  bool Migrating() const { return migrate_ || migrate_by_host_; }
  static bool SameAddress(const sockaddr_in& a, const sockaddr_in& b) { return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port; }
  // (Migration) `from` is where slot s was seen (the host's address list):
  // Epic, the same player (stand-ins are per product user ID, which Epic
  // authenticates); UDP, the same IP (a NAT can give another port).
  bool KnownAt(int s, const sockaddr_in& from) const {
    if (!peer_ok_[s]) return false;
    return addr_in_ ? SameAddress(peer_addr_[s], from) : peer_addr_[s].sin_addr.s_addr == from.sin_addr.s_addr;
  }
  // (Migration) Seniority: a lower rank first, 0 (not in the order yet) last; then the lower slot.
  static bool MoreSenior(uint32_t rank_a, int slot_a, uint32_t rank_b, int slot_b) {
    const uint32_t a = rank_a ? rank_a : UINT32_MAX, b = rank_b ? rank_b : UINT32_MAX;
    return a != b ? a < b : slot_a < slot_b;
  }

  void Queue(Link& k, MessageHeader h, const void* data) {
    if (h.len > kMaxMessage) return;   // could never be sent (and would block the reliable channel)
    if (k.pending && !(h.flags & 1)) { ++k.dropped; ++stats_.dropped_unreliable; return; }   // (a link not up again: snapshots would only pile up)
    std::vector<uint8_t> m(sizeof(h) + h.len);
    if (h.flags & 1) h.id = k.rel_next++;
    std::memcpy(m.data(), &h, sizeof(h));
    if (h.len) std::memcpy(m.data() + sizeof(h), data, h.len);
    if (h.flags & 1) { k.rel_bytes += uint32_t(m.size()); k.rel_out.push_back({h.id, std::move(m)}); }
    else k.unrel.insert(k.unrel.end(), m.begin(), m.end());
  }
  void SendControl(Link& k, uint8_t type, const void* data, uint16_t len, bool reliable) {
    MessageHeader h{type, my_slot_, k.slot, uint8_t(reliable ? 1 : 0), 0, len, 0};
    Queue(k, h, data);
  }
  // One packet with one unreliable message and nothing else (a pending link's
  // kRejoin: its reliable messages wait for the new host's answer), on the
  // link's own packet numbers.
  void SendAlone(Link& k, uint8_t type, const std::vector<uint8_t>& body, uint64_t now) {
    const MessageHeader h{type, my_slot_, k.slot, 0, 0, uint16_t(body.size()), 0};
    std::vector<uint8_t> p(sizeof(PacketHeader) + sizeof(h) + body.size());
    const PacketHeader ph{kMagic, kVersion, my_slot_, uint8_t(k.recv_any ? 1 : 0), host_ ? k.epoch : epoch_, k.send_seq, k.recv_latest, k.recv_bits};
    std::memcpy(p.data(), &ph, sizeof(ph));
    std::memcpy(p.data() + sizeof(ph), &h, sizeof(h));
    if (!body.empty()) std::memcpy(p.data() + sizeof(ph) + sizeof(h), body.data(), body.size());
    Sent& s = k.sent[k.send_seq & 4095];
    s.used = true; s.acked = false; s.seq = k.send_seq; s.at = now; s.ids.clear();
    ++k.send_seq;
    send_(p.data(), int(p.size()), k.addr);
    k.last_send = now;
    ++stats_.packets_out; stats_.bytes_out += uint32_t(p.size());
  }

  // A reliable message to send now on this link: the newest not sent yet
  // (Flush sends them in order) or the oldest's resend due. (Only those two:
  // a resend in between waits for the next paced flush, at most pace_ms_.)
  bool ReliableDue(const Link& k, uint64_t now) const {
    if (k.rel_out.empty()) return false;
    const uint64_t resend_after = uint64_t(std::max(100.0f, k.rtt * 1.5f));
    const Out& a = k.rel_out.front();
    const Out& b = k.rel_out.back();
    return now - a.sent_at >= resend_after || (!b.sent_at && uint16_t(b.id - a.id) < kRelWindow);
  }
  // Build and send this link's packets: the reliable messages that are due,
  // then the queued unreliable ones; an empty packet as a keep-alive.
  void Flush(Link& k, uint64_t now) {
    const uint64_t resend_after = uint64_t(std::max(100.0f, k.rtt * 1.5f));
    size_t ui = 0;  // position in k.unrel
    int packets = 0;
    for (;;) {
      std::vector<uint8_t> p(sizeof(PacketHeader));
      std::vector<uint16_t> ids;
      // At most kRelWindow ids past the oldest unacked one: the receiver holds
      // only so many early ones (more were lost for good and stalled the link).
      const uint16_t base = k.rel_out.empty() ? 0 : k.rel_out.front().id;
      for (auto& o : k.rel_out) {
        if (uint16_t(o.id - base) >= kRelWindow) break;
        if (o.sent_at && now - o.sent_at < resend_after) continue;
        if (p.size() + o.bytes.size() > size_t(kMaxPacket)) break;
        if (o.sent_at) ++stats_.resent;
        p.insert(p.end(), o.bytes.begin(), o.bytes.end());
        o.sent_at = now;
        ids.push_back(o.id);
      }
      while (ui < k.unrel.size()) {
        MessageHeader h; std::memcpy(&h, &k.unrel[ui], sizeof(h));
        const size_t n = sizeof(h) + h.len;
        if (p.size() + n > size_t(kMaxPacket)) break;
        p.insert(p.end(), k.unrel.begin() + ui, k.unrel.begin() + ui + n);
        ui += n;
      }
      const bool content = p.size() > sizeof(PacketHeader);
      if (!content && (packets || now - k.last_send < kKeepAliveMs)) break;
      // No acks before anything arrived: an ack of "packet 0" then dropped a
      // reliable message (the join reply) the other side never got.
      PacketHeader ph{kMagic, kVersion, my_slot_, uint8_t(k.recv_any ? 1 : 0), host_ ? k.epoch : epoch_, k.send_seq, k.recv_latest, k.recv_bits};
      std::memcpy(p.data(), &ph, sizeof(ph));
      Sent& s = k.sent[k.send_seq & 4095];
      s.used = true; s.acked = false; s.seq = k.send_seq; s.at = now; s.ids = std::move(ids);
      ++k.send_seq;
      send_(p.data(), int(p.size()), k.addr);
      k.last_send = now;
      ++stats_.packets_out; stats_.bytes_out += uint32_t(p.size());
      if (++packets >= kMaxPacketsPerFlush || ui >= k.unrel.size()) break;
    }
    // What didn't fit waits for the next Pump; past the backlog limit the
    // oldest go (a late snapshot is worth less than a new one).
    k.unrel.erase(k.unrel.begin(), k.unrel.begin() + ui);
    size_t drop = 0;
    while (k.unrel.size() - drop > kMaxUnreliableBacklog) {
      MessageHeader h; std::memcpy(&h, &k.unrel[drop], sizeof(h));
      drop += sizeof(h) + h.len;
      ++stats_.dropped_unreliable; ++k.dropped;
    }
    if (drop) k.unrel.erase(k.unrel.begin(), k.unrel.begin() + drop);
  }

  // The other side acked packet `seq`: the reliable messages it carried
  // arrived. (The entry stays, marked acked: review_phase4 L4 asks whether a
  // packet names one this game sent lately.)
  void Acked(Link& k, uint16_t seq, uint64_t now) {
    Sent& s = k.sent[seq & 4095];
    if (!s.used || s.seq != seq || s.acked) return;
    k.rtt += (float(now - s.at) - k.rtt) * 0.1f;
    for (uint16_t id : s.ids)
      for (auto it = k.rel_out.begin(); it != k.rel_out.end(); ++it)
        if (it->id == id) { k.rel_bytes -= uint32_t(it->bytes.size()); k.rel_out.erase(it); break; }
    s.ids.clear(); s.acked = true;
  }

  // Track which of the other side's packets arrived (for our acks). Returns
  // false for a packet already seen.
  bool MarkReceived(Link& k, uint16_t seq) {
    if (!k.recv_any) { k.recv_any = true; k.recv_latest = seq; k.recv_bits = 0; return true; }
    if (SeqNewer(seq, k.recv_latest)) {
      const uint16_t shift = uint16_t(seq - k.recv_latest);
      k.recv_bits = shift > 32 ? 0 : ((shift == 32 ? 0 : k.recv_bits << shift) | (1u << (shift - 1)));
      k.recv_latest = seq;
      return true;
    }
    const uint16_t back = uint16_t(k.recv_latest - seq);
    if (back == 0 || back > 32) return false;
    const uint32_t bit = 1u << (back - 1);
    if (k.recv_bits & bit) return false;
    k.recv_bits |= bit;
    return true;
  }

  int LinkFor(const sockaddr_in& from) const {
    for (int s = 0; s < kMaxSlots; ++s)
      if (links_[s].used && SameAddress(links_[s].addr, from)) return s;
    return -1;
  }

  void OnDatagram(const uint8_t* d, int n, const sockaddr_in& from, uint64_t now) {
    if (n < int(sizeof(PacketHeader))) { ++stats_.bad_packets; return; }
    PacketHeader ph; std::memcpy(&ph, d, sizeof(ph));
    if (ph.magic != kMagic) { ++stats_.bad_packets; return; }
    if (ph.epoch == 0 && n == int(sizeof(PacketHeader))) { OnWarm(ph, from, now); return; }
    int s = LinkFor(from);
    if (s >= 0 && host_ && ph.epoch != links_[s].epoch) {
      // A known address on another join attempt: the client started over (it
      // lost us first, or left and came back before we noticed). A newer
      // attempt with a join replaces the old link; a delayed packet of an
      // older attempt is ignored.
      if (ph.slot == kNoSlot && IsJoin(d, n) && int8_t(uint8_t(ph.epoch - links_[s].epoch)) > 0) {
        Drop(s, "joined again");
        s = AcceptJoin(d, n, ph, from, now);
        if (s < 0) return;
      } else {
        return;
      }
    }
    if (s < 0) {
      if (!host_) { StillWithHost(d, n, ph, from, now); return; }  // a client only listens to its host
      s = AcceptJoin(d, n, ph, from, now);
      if (s < 0) return;
    }
    if (!host_ && ph.epoch != epoch_) {
      // (Migration) A successor this game is still linking to stopped: its
      // kLeave, on no epoch of ours (it never heard from this game).
      if (ph.epoch == 0 && links_[s].pending && n >= int(sizeof(PacketHeader) + sizeof(MessageHeader)) && d[sizeof(PacketHeader)] == kLeave) {
        // (review_final_shared H1) Or it is no successor: it still hears the session host.
        MessageHeader lh; std::memcpy(&lh, d + sizeof(PacketHeader), sizeof(lh));
        const int body = int(sizeof(PacketHeader) + sizeof(MessageHeader));
        if (lh.len >= 2 && n >= body + 2 && d[body + 1] == 1) host_alive_ = true; else host_left_ = true;
      }
      return;  // meant for an earlier join attempt
    }
    Link& k = links_[s];
    ++stats_.packets_in; stats_.bytes_in += uint32_t(n);
    if (!MarkReceived(k, ph.seq)) return;
    k.last_recv = now;   // (only new packets: a dead link's old sequence numbers can't keep it alive)
    // (Migration, review_phase4 L2) The new host's first packet on this
    // game's epoch: it took the re-link. The link flushes as any other from
    // now on (acks, the carried messages); its kJoinOk follows.
    if (!host_ && k.pending) k.pending = false;
    {
      const Sent& a = k.sent[ph.ack & 4095];   // (review_phase4 L4: before Acked)
      ack_recent_ = (ph.flags & 1) && a.used && a.seq == ph.ack && now - a.at < 5000;
    }
    if (ph.flags & 1) {
      Acked(k, ph.ack, now);
      for (int i = 0; i < 32; ++i) if (ph.ack_bits & (1u << i)) Acked(k, uint16_t(ph.ack - 1 - i), now);
    }
    for (int o = sizeof(PacketHeader); o + int(sizeof(MessageHeader)) <= n;) {
      MessageHeader h; std::memcpy(&h, d + o, sizeof(h));
      const int total = int(sizeof(h)) + h.len;
      if (o + total > n || h.len > kMaxMessage) { ++stats_.bad_packets; break; }
      std::vector<uint8_t> msg(d + o, d + o + total);
      o += total;
      if (!(h.flags & 1)) { OnMessage(s, h, std::move(msg)); continue; }
      if (h.id == k.rel_expect) {
        ++k.rel_expect;
        OnMessage(s, h, std::move(msg));
        for (auto it = k.rel_hold.find(k.rel_expect); it != k.rel_hold.end(); it = k.rel_hold.find(k.rel_expect)) {
          MessageHeader hh; std::memcpy(&hh, it->second.data(), sizeof(hh));
          std::vector<uint8_t> held = std::move(it->second);
          k.rel_hold.erase(it);
          ++k.rel_expect;
          OnMessage(s, hh, std::move(held));
        }
      } else if (SeqNewer(h.id, k.rel_expect) && uint16_t(h.id - k.rel_expect) < 2 * kRelWindow) {
        k.rel_hold.emplace(h.id, std::move(msg));
      }  // else: an older copy, already delivered
    }
  }

  // (Migration) A warm packet. A new host notes the clients it waits for that
  // still warm it (they are with the old host: L5); a session host that hears
  // another, more senior one (after a split) yields to it (review_phase4 M3).
  void OnWarm(const PacketHeader& ph, const sockaddr_in& from, uint64_t now) {
    const uint8_t s = ph.slot;
    if (!host_ || !Migrating() || s >= kMaxSlots || s == my_slot_) return;
    const bool known = KnownAt(s, from) || (s == lost_host_slot_ && (addr_in_ ? SameAddress(lost_host_, from) : lost_host_.sin_addr.s_addr == from.sin_addr.s_addr));
    if (!known || (left_at_[s] && now - left_at_[s] < kTimeoutMs)) return;   // (one that just left: sent before its kLeave)
    if (!(ph.flags & 2)) { if (links_[s].used && links_[s].pending) warm_in_[s] = now; return; }
    if (MoreSenior(ph.ack_bits, s, rank_[my_slot_], my_slot_) && yield_to_ < 0) { yield_to_ = s; yield_addr_ = from; }   // (Pump yields: not inside the packet)
  }

  // The packet's first message is a well-formed join (or a re-link: kRejoin).
  static bool IsJoin(const uint8_t* d, int n) {
    if (n < int(sizeof(PacketHeader) + sizeof(MessageHeader))) return false;
    MessageHeader h; std::memcpy(&h, d + sizeof(PacketHeader), sizeof(h));
    return (h.type == kJoin || h.type == kRejoin) && h.len >= (h.type == kRejoin ? 10 : 1) && h.len <= kMaxMessage && int(sizeof(PacketHeader) + sizeof(h) + h.len) <= n;
  }
  // (Migration, review_final_shared H1) A client asked to link as the new
  // session host (a kRejoin from a player whose address the host shared)
  // while this game still hears the session host: that game's link alone
  // failed. It is told so (a kLeave on epoch 0: u8 the session host, u8 1)
  // and joins the host again at once, instead of trying every successor 7 s
  // each (with a fallback join that late, the others would claim its world).
  void StillWithHost(const uint8_t* d, int n, const PacketHeader& ph, const sockaddr_in& from, uint64_t now) {
    if (host_ || !Migrating() || my_slot_ == kNoSlot || relink_ || !links_[0].used || links_[0].pending || now - links_[0].last_recv > kTimeoutMs / 3) return;
    if (!IsJoin(d, n)) return;
    MessageHeader h; std::memcpy(&h, d + sizeof(PacketHeader), sizeof(h));
    const uint8_t* p = d + sizeof(PacketHeader) + sizeof(h);
    if (h.type != kRejoin || p[1] >= kMaxSlots || p[1] == my_slot_ || ph.slot != p[1] || !KnownAt(p[1], from)) return;
    const PacketHeader out{kMagic, kVersion, my_slot_, 0, 0, 0, 0, 0};
    const MessageHeader mh{kLeave, my_slot_, p[1], 0, 0, 2, 0};
    uint8_t b[sizeof(out) + sizeof(mh) + 2];
    std::memcpy(b, &out, sizeof(out)); std::memcpy(b + sizeof(out), &mh, sizeof(mh));
    b[sizeof(out) + sizeof(mh)] = host_slot_; b[sizeof(out) + sizeof(mh) + 1] = 1;
    send_(b, int(sizeof(b)), from);
    ++stats_.still_with_host; ++stats_.packets_out; stats_.bytes_out += uint32_t(sizeof(b));
  }
  // Host: a packet from an unknown address must carry a join.
  int AcceptJoin(const uint8_t* d, int n, const PacketHeader& ph, const sockaddr_in& from, uint64_t now) {
    if (!IsJoin(d, n)) return -1;
    MessageHeader h; std::memcpy(&h, d + sizeof(PacketHeader), sizeof(h));
    const uint8_t* p = d + sizeof(PacketHeader) + sizeof(h);
    const bool rejoin = h.type == kRejoin;
    if (rejoin && !Migrating()) return -1;   // (review_phase4 L1)
    if (!rejoin && ph.slot != kNoSlot) return -1;
    // (An older version's kJoin is u8 version, name: refused below.)
    const size_t skip = rejoin ? 10 : p[0] == kVersion && h.len >= 9 ? 9 : 1;
    uint64_t token = 0;
    if (skip > 1) std::memcpy(&token, p + skip - 8, 8);
    const std::string name(reinterpret_cast<const char*>(p + skip), std::min<size_t>(h.len - skip, kMaxName));
    // (Migration) A client of the old host linking again: its slot waits for
    // it (a pending link), its queues kept. One this game doesn't wait for (it
    // gave up on it, or another name) joins as a new player. One from another
    // address, or with the wrong token, is ignored (review_phase4 M4: anyone
    // could take a slot that waits, and the next migration with it).
    if (rejoin && p[0] == kVersion && p[1] < kMaxSlots && ph.slot == p[1] && p[1] != my_slot_) {
      const uint8_t slot = p[1];
      Link& k = links_[slot];
      if (k.used && k.pending && names_[slot] == name) {
        if (!KnownAt(slot, from) || (!addr_in_ && (!token_hash_[slot] || TokenHash(token) != token_hash_[slot]))) {
          if (stats_.rejoin_refused++ < 5) Log("a re-link as " + name + " (slot " + std::to_string(slot) + ") from another address or with the wrong token: ignored");
          return -1;
        }
        k.pending = false; k.addr = from; k.epoch = ph.epoch; k.last_recv = now;
        peer_addr_[slot] = from; peer_ok_[slot] = true; token_hash_[slot] = TokenHash(token);
        if (!rank_[slot]) list_due_[slot] = now + kListMs;
        rejoined_any_ = true; ++stats_.rejoined;
        SendJoinOk(k);
        ShareAddresses();
        inbox_.push_back({slot, kRejoined, {}});
        Log(name + " linked again as slot " + std::to_string(slot) + " (this game is the session host now)");
        return slot;
      }
      if (k.used && !k.pending && present_[slot]) return -1;   // (that player is linked: a late copy from another address, or a stranger)
    }
    std::string deny;
    if (p[0] != kVersion) deny = "different co-op version (host protocol " + std::to_string(kVersion) + ", you " + std::to_string(p[0]) + "; protocol 13 is co-op 3.0 of 2026-10-26 or newer, 12 an earlier 3.0: everyone needs the same)";
    // The lowest free slot; with migration the one freed longest ago (never
    // used first): a player who just left (the old session host too) isn't
    // followed by a stranger in the same slot at once (review_phase4 L3).
    int slot = -1, taken = 0;
    for (int s = 0; s < kMaxSlots; ++s) taken += s == my_slot_ || links_[s].used || present_[s];
    for (int s = 0; s < kMaxSlots && deny.empty(); ++s)
      if (s != my_slot_ && !links_[s].used && !present_[s] && (slot < 0 || (Migrating() && freed_at_[s] < freed_at_[slot]))) slot = s;
    // (3.0) The host's max_players: a slot waiting for a player to link again
    // after a migration counts as taken.
    if (deny.empty() && (slot < 0 || taken >= max_players_)) deny = "the session is full (" + std::to_string(max_players_) + " players, the session host's mod.ini max_players)";
    if (!deny.empty()) {
      Link tmp; tmp.used = true; tmp.addr = from; tmp.slot = kNoSlot; tmp.epoch = ph.epoch;
      SendControl(tmp, kJoinDeny, deny.data(), uint16_t(deny.size()), false);
      Flush(tmp, now);
      Log("refused a join from " + name + ": " + deny);
      return -1;
    }
    Link& k = links_[slot];
    k = Link{}; k.used = true; k.addr = from; k.slot = uint8_t(slot); k.last_recv = now; k.epoch = ph.epoch;
    names_[slot] = name; present_[slot] = true; oseq_any_[slot] = false;
    peer_addr_[slot] = from; peer_ok_[slot] = true; token_hash_[slot] = TokenHash(token);
    rank_[slot] = 0; list_due_[slot] = now + kListMs;   // (a new player is the least senior, whoever had the slot before)
    SendJoinOk(k);
    std::vector<uint8_t> pj{uint8_t(slot)};
    pj.insert(pj.end(), name.begin(), name.end());
    for (int s = 0; s < kMaxSlots; ++s) if (s != slot && s != my_slot_ && links_[s].used) SendControl(links_[s], kPeerJoined, pj.data(), uint16_t(pj.size()), true);
    inbox_.push_back({uint8_t(slot), kPeerJoined, std::vector<uint8_t>(pj.begin() + 1, pj.end())});
    ShareAddresses();
    Log(name + " joined as slot " + std::to_string(slot));
    return slot;
  }
  // kJoinOk: your slot, then everyone present.
  void SendJoinOk(Link& k) {
    std::vector<uint8_t> ok{k.slot, 0};
    for (int s = 0; s < kMaxSlots; ++s)
      if (present_[s]) {
        ++ok[1];
        ok.push_back(uint8_t(s)); ok.push_back(uint8_t(std::min<size_t>(names_[s].size(), 255)));
        ok.insert(ok.end(), names_[s].begin(), names_[s].begin() + ok.back());
      }
    ok.push_back(uint8_t(max_players_));   // (3.0, protocol 13: the session's cap, after the names)
    SendControl(k, kJoinOk, ok.data(), uint16_t(ok.size()), true);
  }
  // (Migration) Host: every client's address as this game sees it, its
  // seniority and its token's hash, to every client (a successor is linked
  // to at it, a re-link checked against it). A client not back yet after a
  // migration is listed as it was: it may be the next successor still.
  void ShareAddresses() {
    if (!Migrating()) return;
    std::vector<uint8_t> m;
    for (int s = 0; s < kMaxSlots; ++s) {
      const Link& k = links_[s];
      if (s == my_slot_ || !k.used || (k.pending && !peer_ok_[s])) continue;
      const sockaddr_in& at = k.pending ? peer_addr_[s] : k.addr;
      std::string a;
      if (addr_out_) a = addr_out_(at);
      else { a.resize(6); std::memcpy(&a[0], &at.sin_addr.s_addr, 4); std::memcpy(&a[4], &at.sin_port, 2); }
      if (a.empty() || a.size() > 255 || m.size() + 2 + a.size() + 12 > size_t(kMaxMessage)) continue;
      m.push_back(uint8_t(s)); m.push_back(uint8_t(a.size()));
      m.insert(m.end(), a.begin(), a.end());
      const size_t o = m.size();
      m.resize(o + 12);
      std::memcpy(&m[o], &rank_[s], 4); std::memcpy(&m[o + 4], &token_hash_[s], 8);
    }
    for (auto& k : links_) if (k.used) SendControl(k, kPeerAddr, m.data(), uint16_t(m.size()), true);
  }

  void OnMessage(int link_slot, const MessageHeader& h, std::vector<uint8_t> msg) {
    const uint8_t* p = msg.data() + sizeof(MessageHeader);
    if (h.type < kFirstUserType) { OnSession(link_slot, h, p); return; }
    if (host_) {
      // A client's message: the origin must be that client. Relay it on.
      if (h.origin != link_slot) return;
      if (!(h.flags & 1) && relay_filter_) relay_filter_(h.type, h.origin, kEveryone, p, h.len);   // (protocol 12: noted, also with nobody to pass it to: the sender's position for the tiers of what goes to it)
      if (h.target == kEveryone || (h.target != my_slot_ && h.target < kMaxSlots)) {
        for (int s = 0; s < kMaxSlots; ++s)
          if (s != link_slot && s != my_slot_ && links_[s].used && present_[s] && (h.target == kEveryone || h.target == s)) {
            if (!(h.flags & 1) && relay_filter_ && !relay_filter_(h.type, h.origin, uint8_t(s), p, h.len)) { ++stats_.relay_thinned; continue; }
            Queue(links_[s], h, p);
            ++stats_.relayed; stats_.relayed_bytes += uint32_t(sizeof(MessageHeader) + h.len);
            if (h.type < kFirstUserType + Stats::kTypes) stats_.relayed_type[h.type - kFirstUserType] += uint32_t(sizeof(MessageHeader) + h.len);
          }
      }
      if (h.target != kEveryone && h.target != my_slot_) return;
    }
    // A reliable message delivered already (a migration carries what the old
    // host never acked, which it may have relayed): at most once, in order.
    // Only with migration (review_final_area L1): without it nothing is
    // carried, and delivery stays as it was before co-op 3.
    if (Migrating() && (h.flags & 1) && h.origin < kMaxSlots) {
      if (oseq_any_[h.origin] && !SeqNewer(h.oseq, oseq_in_[h.origin])) { ++stats_.duplicates; return; }
      oseq_any_[h.origin] = true; oseq_in_[h.origin] = h.oseq;
    }
    inbox_.push_back({h.origin, h.type, std::vector<uint8_t>(p, p + h.len)});
  }

  void OnSession(int link_slot, const MessageHeader& h, const uint8_t* p) {
    switch (h.type) {
      case kJoinOk: {
        if (host_ || h.len < 2 || h.origin >= kMaxSlots) return;
        if (my_slot_ != kNoSlot && !relink_) return;   // (joined already: nothing to do)
        // (Migration) The new host's answer to a re-link: the same slot, and
        // whoever it has (a difference told as joins and leaves). Another slot:
        // the session as it was is lost, this is a new join.
        bool relink = relink_ && my_slot_ != kNoSlot;
        if (relink && p[0] != my_slot_) { inbox_.push_back({host_slot_, kPeerLeft, {kLeftTimedOut}}); relink = false; }
        links_[0].pending = false; relink_ = false;
        host_slot_ = h.origin;
        const std::array<bool, kMaxSlots> was = present_;
        const std::array<std::string, kMaxSlots> was_names = names_;
        my_slot_ = p[0];
        present_.fill(false);
        int o = 2;
        for (int i = 0; i < p[1] && o + 2 <= h.len; ++i) {
          const uint8_t s = p[o], len = p[o + 1];
          if (s < kMaxSlots && o + 2 + len <= h.len) { names_[s].assign(reinterpret_cast<const char*>(p + o + 2), len); present_[s] = true; }
          o += 2 + len;
        }
        host_max_ = o < h.len && p[o] >= 2 && p[o] <= kMaxSlots ? p[o] : 0;   // (3.0: the host's max_players)
        if (relink) {
          for (int s = 0; s < kMaxSlots; ++s) {
            if (s == my_slot_) continue;
            const bool other = was[s] && present_[s] && was_names[s] != names_[s];   // (another player in that slot meanwhile)
            if ((was[s] && !present_[s]) || other) { inbox_.push_back({uint8_t(s), kPeerLeft, {kLeftTimedOut}}); oseq_any_[s] = false; rank_[s] = 0; }
            if ((!was[s] && present_[s]) || other) { inbox_.push_back({uint8_t(s), kPeerJoined, std::vector<uint8_t>(names_[s].begin(), names_[s].end())}); oseq_any_[s] = false; rank_[s] = 0; }
          }
          failed_.fill(false);
          ++stats_.rejoined;
          inbox_.push_back({host_slot_, kRejoined, {}});
          Log("linked again to the new session host (slot " + std::to_string(host_slot_) + ")");
          return;
        }
        oseq_any_.fill(false); rank_.fill(0); failed_.fill(false);
        inbox_.push_back({host_slot_, kJoinOk, {my_slot_}});
        Log("joined as slot " + std::to_string(my_slot_));
        return;
      }
      case kJoinDeny:
        deny_reason_.assign(reinterpret_cast<const char*>(p), h.len);
        inbox_.push_back({host_slot_, kJoinDeny, std::vector<uint8_t>(p, p + h.len)});
        Log("join refused: " + deny_reason_);
        links_[0] = Link{};
        return;
      case kPeerJoined:
        if (host_ || h.len < 1 || p[0] >= kMaxSlots) return;
        names_[p[0]].assign(reinterpret_cast<const char*>(p + 1), h.len - 1);
        present_[p[0]] = true; oseq_any_[p[0]] = false; rank_[p[0]] = 0;
        inbox_.push_back({p[0], kPeerJoined, std::vector<uint8_t>(p + 1, p + h.len)});
        return;
      case kPeerLeft:
        if (host_ || h.len < 1 || p[0] >= kMaxSlots) return;
        present_[p[0]] = false; oseq_any_[p[0]] = false; rank_[p[0]] = 0; freed_at_[p[0]] = last_now_;
        inbox_.push_back({p[0], kPeerLeft, {uint8_t(h.len >= 2 ? p[1] : kLeftTimedOut)}});
        return;
      case kLeave:
        if (host_ && link_slot != my_slot_) Drop(link_slot, "left", kLeftLeave);
        else if (!host_ && Migrating() && ack_recent_) {   // (the session host: Pump moves on; review_phase4 L4: only from a packet that acks one this game sent lately)
          host_left_ = true;
          host_left_to_ = h.len >= 1 && p[0] < kMaxSlots ? p[0] : -1;
        }
        return;
      case kPeerAddr: {
        if (host_) return;
        migrate_by_host_ = true;   // (review_phase4 L9: the session host's setting counts)
        peer_ok_.fill(false); rank_.fill(0);
        for (int o = 0; o + 2 <= h.len;) {
          const uint8_t s = p[o], len = p[o + 1];
          if (o + 2 + len + 12 > h.len) break;
          const uint8_t* q = p + o + 2 + len;
          if (s < kMaxSlots) {
            std::memcpy(&rank_[s], q, 4);
            if (s != my_slot_) {
              const std::string a(reinterpret_cast<const char*>(p + o + 2), len);
              sockaddr_in addr{};
              if (addr_in_) peer_ok_[s] = addr_in_(a, addr);
              else if (len == 6) { addr.sin_family = AF_INET; std::memcpy(&addr.sin_addr.s_addr, a.data(), 4); std::memcpy(&addr.sin_port, a.data() + 4, 2); peer_ok_[s] = true; }
              if (peer_ok_[s]) { peer_addr_[s] = addr; std::memcpy(&token_hash_[s], q + 4, 8); }
            }
          }
          o += 2 + len + 12;
        }
        return;
      }
      default:
        return;
    }
  }

  void Drop(int s, const char* why, uint8_t reason = kLeftTimedOut) {
    Link& k = links_[s];
    if (!k.used) return;
    Log((!host_ ? std::string("host") : names_[s] + " (slot " + std::to_string(s) + ")") + " " + why);
    const sockaddr_in addr = k.addr;
    const bool had_address = !k.pending;
    k = Link{};
    if (drop_ && host_ && had_address && std::string(why) != "joined again") drop_(addr);   // (not when the same player's new join is on that connection)
    if (host_) {
      present_[s] = false; oseq_any_[s] = false; rank_[s] = 0; list_due_[s] = 0; freed_at_[s] = last_now_;
      const uint8_t slot = uint8_t(s), body[2] = {slot, reason};
      for (int o = 0; o < kMaxSlots; ++o) if (o != my_slot_ && links_[o].used) SendControl(links_[o], kPeerLeft, body, 2, true);
      inbox_.push_back({slot, kPeerLeft, {reason}});
      ShareAddresses();
    } else {
      LoseSession(addr, nullptr);
    }
  }
  // Lost the host: everyone is gone; the client keeps joining (as v1 did) at
  // `addr` (the host it had; after a migration that failed, the old host).
  void LoseSession(const sockaddr_in& addr, const char* why) {
    if (why) Log(std::string("session lost: ") + why);
    const bool was_joined = my_slot_ != kNoSlot;
    const uint8_t was_host = host_slot_;
    for (auto& l : links_) l = Link{};
    present_.fill(false); peer_ok_.fill(false); oseq_any_.fill(false);
    rank_.fill(0); list_due_.fill(0); token_hash_.fill(0); warm_in_.fill(0); left_at_.fill(0); failed_.fill(false);
    host_ = false; my_slot_ = kNoSlot; host_slot_ = 0; rejoin_wait_ = 0; relink_ = false; migrate_by_host_ = false; yield_to_ = -1; host_alive_ = false; host_timed_out_ = false;
    epoch_ = NextEpoch();   // a new join attempt
    my_token_ = NewToken();
    Link& k = links_[0]; k.used = true; k.addr = addr; k.slot = 0;
    if (was_joined) inbox_.push_back({was_host, kPeerLeft, {kLeftTimedOut}});
  }
  // (Migration) The next successor: the most senior player still there that
  // this game hasn't given up on in this migration and can reach (itself, or
  // a shared address); nobody in the order: the lowest such slot. -1: none.
  int NextCandidate() const {
    auto ok = [&](int s) { return present_[s] && !failed_[s] && (s == my_slot_ || peer_ok_[s]); };
    int best = -1;
    for (int s = 0; s < kMaxSlots; ++s) if (ok(s) && rank_[s] && (best < 0 || MoreSenior(rank_[s], s, rank_[best], best))) best = s;
    for (int s = 0; s < kMaxSlots && best < 0; ++s) if (ok(s)) best = s;
    return best;
  }
  // (Migration) Client: the session host left (`to`: it yielded to that
  // host, which this game links to instead). What the old host never acked
  // goes to the new one (to be relayed as before).
  void HostGone(uint64_t now, const char* why, int to = -1) {
    Link& k = links_[0];
    const uint8_t old = host_slot_;
    lost_host_ = k.addr; lost_host_slot_ = old;
    host_timed_out_ = std::strcmp(why, "timed out") == 0;
    if (drop_) drop_(lost_host_);   // (what is still queued for it goes: 2.4's shared send queue)
    std::deque<Out> carried = std::move(k.rel_out);
    failed_.fill(false); failed_[old] = true;
    moved_from_ = old;
    if (to < 0) { present_[old] = false; freed_at_[old] = now; rank_[old] = 0; }   // (a host that yielded links to that one too: still a player)
    MoveOn(now, why, to, std::move(carried), to < 0 ? int(old) : -1, std::strcmp(why, "left") == 0 ? kLeftLeave : kLeftTimedOut);
  }
  // (Migration, review_phase4 M2) Client: the successor it links to didn't
  // answer, or left: the next one (none left: the old host's address, as 2.x).
  void Failover(uint64_t now, const char* why, bool left) {
    const uint8_t was = host_slot_;
    failed_[was] = true;
    if (left) { present_[was] = false; freed_at_[was] = now; rank_[was] = 0; }
    std::deque<Out> carried = std::move(links_[0].rel_out);   // (all still unsent: a pending link doesn't flush)
    Log("successor (slot " + std::to_string(was) + ") " + why);
    MoveOn(now, why, -1, std::move(carried), left ? int(was) : -1, left ? kLeftLeave : kLeftTimedOut);
  }
  // (Migration) Link to the next session host, or become it. `to`: the one
  // asked for (a yield), if this game can reach it. `gone`: the slot whose
  // kPeerLeft follows the kHostMoved.
  void MoveOn(uint64_t now, const char* why, int to, std::deque<Out> carried, int gone, uint8_t reason = kLeftTimedOut) {
    const int next = to >= 0 && to < kMaxSlots && to != my_slot_ && peer_ok_[to] ? to : NextCandidate();
    if (next < 0) { LoseSession(lost_host_, (std::string("the host ") + why + ", no successor left").c_str()); return; }
    Log("host (slot " + std::to_string(host_slot_) + ") " + why + ": slot " + std::to_string(next) + " is the session host now" + (next == my_slot_ ? " (this game)" : ""));
    for (auto& l : links_) l = Link{};
    host_slot_ = uint8_t(next); present_[next] = true;
    relink_ = false;
    ++stats_.host_moves;
    inbox_.push_back({uint8_t(next), kHostMoved, {moved_from_}});
    if (gone >= 0) inbox_.push_back({uint8_t(gone), kPeerLeft, {reason}});
    if (next == my_slot_) {
      host_ = true; rejoin_wait_ = now; rejoined_any_ = false;
      next_rank_ = 0;
      for (uint32_t r : rank_) next_rank_ = std::max(next_rank_, r);
      for (int s = 0; s < kMaxSlots; ++s) {
        list_due_[s] = 0; warm_in_[s] = 0;
        if (s != my_slot_ && present_[s]) { Link& w = links_[s]; w.used = true; w.pending = true; w.slot = uint8_t(s); w.last_recv = now; }
      }
      for (auto& o : carried) {
        MessageHeader h; std::memcpy(&h, o.bytes.data(), sizeof(h));
        for (int s = 0; s < kMaxSlots; ++s)
          if (links_[s].used && (h.target == kEveryone || h.target == s)) { Queue(links_[s], h, o.bytes.data() + sizeof(h)); ++stats_.carried; }
      }
    } else {
      relink_ = true;
      Link& n = links_[0]; n.used = true; n.pending = true; n.addr = peer_addr_[next]; n.slot = uint8_t(next); n.last_recv = now;
      for (auto& o : carried) {
        MessageHeader h; std::memcpy(&h, o.bytes.data(), sizeof(h));
        if (gone >= 0 && h.target == gone) continue;   // (it was for the host that left alone)
        Queue(n, h, o.bytes.data() + sizeof(h)); ++stats_.carried;
      }
    }
  }
  // (Migration, review_phase4 M3) Host: a more senior session host is there
  // (a split: two halves each took a successor). This game's clients are sent
  // to it (kLeave naming it) and this game links to it as a client, with
  // what it sent that no client acked yet.
  void GiveWay(uint64_t now, int to, const sockaddr_in& at) {
    Log("slot " + std::to_string(to) + " is a more senior session host: this game links to it");
    ++stats_.yields;
    const uint8_t body = uint8_t(to);
    for (auto& k : links_) if (k.used && !k.pending) for (int i = 0; i < 5; ++i) { SendControl(k, kLeave, &body, 1, false); Flush(k, now); }
    std::map<uint16_t, std::vector<uint8_t>> mine;   // by age (the newest 0): each message once
    for (auto& k : links_)
      for (auto& o : k.rel_out) {
        MessageHeader h; std::memcpy(&h, o.bytes.data(), sizeof(h));
        if (h.origin == my_slot_ && h.type >= kFirstUserType) mine.emplace(uint16_t(oseq_out_ - h.oseq), o.bytes);
      }
    std::deque<Out> carried;
    for (auto it = mine.rbegin(); it != mine.rend(); ++it) carried.push_back({0, std::move(it->second)});
    const uint8_t me = my_slot_;
    host_ = false; rejoin_wait_ = 0; rejoined_any_ = false; host_timed_out_ = false;
    failed_.fill(false); failed_[me] = true;
    peer_addr_[to] = at; peer_ok_[to] = true; present_[to] = true;
    host_slot_ = me; moved_from_ = me;
    MoveOn(now, "yielded", to, std::move(carried), -1);
  }

  SendFn send_;
  DropFn drop_;
  RecvFn recv_;
  LogFn log_;
  mutable std::mutex m_;
  bool host_ = false;
  bool migrate_ = false, migrate_by_host_ = false;
  uint8_t my_slot_ = kNoSlot;
  uint8_t host_slot_ = 0;   // the session host's slot (migration moves it)
  uint64_t rejoin_wait_ = 0;   // migration, the new host: since then the clients link again
  bool rejoined_any_ = false, host_left_ = false, host_alive_ = false;
  bool host_timed_out_ = false;   // migration: this move began with the session host's silence (not its kLeave): a successor that still hears it is believed
  int host_left_to_ = -1;      // migration: the session host yielded to that slot (its kLeave)
  int yield_to_ = -1;          // migration, a host: a more senior session host heard (Pump yields to it)
  sockaddr_in yield_addr_{};
  bool relink_ = false;        // migration, client: linking to a new host, its kJoinOk not here yet
  bool ack_recent_ = false;    // the packet being read acks one this game sent in the last 5 s
  uint64_t last_now_ = 0;
  uint64_t next_warm_ = 0;
  sockaddr_in lost_host_{};   // migration: the host that left (a failed re-link joins it again)
  uint8_t lost_host_slot_ = kNoSlot, moved_from_ = kNoSlot;
  std::array<sockaddr_in, kMaxSlots> peer_addr_{};   // migration: each client's address as the host shared it (kPeerAddr; the host: as it sees it)
  std::array<bool, kMaxSlots> peer_ok_{};
  std::array<uint32_t, kMaxSlots> rank_{};          // migration: seniority (1 the most senior, 0 not in the succession order yet)
  uint32_t next_rank_ = 0;                          // migration, the host: the last seniority given
  std::array<uint64_t, kMaxSlots> list_due_{};      // migration, the host: a new player goes into the order then
  std::array<uint64_t, kMaxSlots> token_hash_{};    // migration: TokenHash of each client's token (the host: from kJoin / kRejoin; a client: kPeerAddr)
  std::array<uint64_t, kMaxSlots> freed_at_{};      // when each slot was left (0: never used)
  std::array<uint64_t, kMaxSlots> warm_in_{};       // migration, a new host: a warm packet from a client it waits for
  std::array<uint64_t, kMaxSlots> left_at_{};       // migration: a session host's kLeave came then
  std::array<bool, kMaxSlots> failed_{};            // migration: successors given up on in this migration
  uint64_t my_token_ = 0;
  uint16_t oseq_out_ = 0;                      // this game's reliable game messages (MessageHeader.oseq)
  std::array<uint16_t, kMaxSlots> oseq_in_{};  // the newest delivered from each origin...
  std::array<bool, kMaxSlots> oseq_any_{};     // ... once one came (cleared when the slot is a new player)
  AddrOutFn addr_out_;
  AddrInFn addr_in_;
  RelayFilterFn relay_filter_;
  uint32_t pace_ms_ = 0;   // (PaceUnreliable)
  int max_players_ = kMaxSlots, host_max_ = 0;   // (MaxPlayers; a client: its host's, 0 not known)
  uint8_t epoch_ = 1;      // client: this join attempt
  int flush_start_ = 0;
  std::string my_name_, deny_reason_;
  std::array<Link, kMaxSlots> links_{};  // host: index = client slot; client: index 0 = the host (whichever slot it is)
  std::array<std::string, kMaxSlots> names_{};
  std::array<bool, kMaxSlots> present_{};
  std::deque<struct Received> inbox_;
  Stats stats_;
};

// (review_final_shared H1) When the objects of a player who left may be
// claimed (co-op 3 phase 3: the games near them take them over). Fed every
// session message in order (Feed); asked per owner that left (State).
// - The session host a kHostMoved names as gone: its objects wait until this
//   game is linked again (kRejoined: a client's re-link, or the new host's
//   first client back). A game whose link alone failed never re-links: it
//   ends in a fresh join, and the copies fade with the session as before,
//   instead of being claimed while the old host still owns the originals.
// - A player that timed out (kPeerLeft kLeftTimedOut) may be joining again
//   under a new slot: its objects wait kTimedOutHoldMs more. A player with the
//   same name joining in that window (kCallOffMs, the claims' own delay
//   included) calls the claims off: the copies fade, and it brings its world
//   back as new objects.
class ClaimGate {
 public:
  enum : uint8_t { kClaim = 0, kWait = 1, kCalledOff = 2 };
  static constexpr uint64_t kTimedOutHoldMs = 8000;
  static constexpr uint64_t kCallOffMs = kTimedOutHoldMs + 5000;
  struct Counts { uint32_t relink_waits = 0, timed_out_waits = 0, called_off = 0; };

  void Reset() { *this = ClaimGate{}; }
  // `name`: the origin's name (kPeerLeft: the one it had; kPeerJoined: its new one).
  void Feed(const Received& m, uint8_t my_slot, const std::string& name, uint64_t now) {
    const int o = m.origin;
    switch (m.type) {
      case kJoinOk: Reset(); return;   // (a fresh join: another session)
      case kHostMoved: {
        const int old = m.data.empty() ? -1 : m.data[0];
        if (old >= 0 && old < kMaxSlots && old != my_slot && o != old) { relink_[old] = true; ++counts_.relink_waits; }
        return;
      }
      case kRejoined: relink_.fill(false); return;
      case kPeerLeft: {
        if (o < 0 || o >= kMaxSlots) return;
        off_[o] = false; hold_until_[o] = 0; name_until_[o] = 0;
        if (!m.data.empty() && m.data[0] == kLeftTimedOut) {
          if (!relink_[o]) { hold_until_[o] = now + kTimedOutHoldMs; ++counts_.timed_out_waits; }   // (the old host: its re-link wait covers it)
          names_[o] = name; name_until_[o] = now + kCallOffMs;
        }
        return;
      }
      case kPeerJoined: {
        if (o < 0 || o >= kMaxSlots) return;
        relink_[o] = false; off_[o] = false; hold_until_[o] = 0; name_until_[o] = 0;   // (that slot: a new player)
        if (name.empty()) return;
        for (int s = 0; s < kMaxSlots; ++s)
          if (s != o && name_until_[s] && now < name_until_[s] && names_[s] == name) { off_[s] = true; name_until_[s] = 0; hold_until_[s] = 0; ++counts_.called_off; }
        return;
      }
      default: return;
    }
  }
  uint8_t State(int slot, uint64_t now) const {
    if (slot < 0 || slot >= kMaxSlots) return kClaim;
    if (off_[slot]) return kCalledOff;
    return relink_[slot] || (hold_until_[slot] && now < hold_until_[slot]) ? kWait : kClaim;
  }
  Counts TakeCounts() { const Counts c = counts_; counts_ = {}; return c; }

 private:
  std::array<bool, kMaxSlots> relink_{}, off_{};
  std::array<uint64_t, kMaxSlots> hold_until_{}, name_until_{};
  std::array<std::string, kMaxSlots> names_{};
  Counts counts_;
};

}  // namespace coopnet
