// Stand-alone test of the co-op v2 network layer (net.h): a host and up to 12
// clients over a simulated network that drops, duplicates, delays and reorders
// packets. No game, no sockets. Build (from the repo root, with the toolchain
// that Setup installs):
//   clang++ -std=c++20 -O1 core/WhompaysCoop/tests/net_test.cpp -o build/net_test.exe -lws2_32
// Exit code 0 = all checks passed. An argument picks another random seed.
#include "../net.h"
#include "../world.h"
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>
#include <set>

namespace {

struct Datagram { uint64_t at; sockaddr_in from, to; std::vector<uint8_t> bytes; };

// A lossy network shared by every endpoint, on a simulated clock.
struct Network {
  std::mt19937 rng{12345};
  double loss = 0.20, dup = 0.03;
  int min_delay = 10, max_delay = 90;
  std::vector<Datagram> air;
  std::map<uint64_t, std::deque<Datagram>> inbox;  // key = address
  uint64_t now = 0;
  bool cut[64] = {};                               // endpoint silenced (by index in the address)
  std::map<std::pair<int, int>, int> drop_next;    // (from, to) index: drop that many packets
  std::set<std::pair<int, int>> blocked;           // (from, to) index: one direction cut

  static uint64_t Key(const sockaddr_in& a) { return (uint64_t(a.sin_addr.s_addr) << 16) | a.sin_port; }
  void Send(const sockaddr_in& from, const sockaddr_in& to, const void* d, int n) {
    if (cut[ntohs(from.sin_port) & 63]) return;
    if (auto it = drop_next.find({ntohs(from.sin_port) & 63, ntohs(to.sin_port) & 63}); it != drop_next.end() && it->second > 0) { --it->second; return; }
    if (blocked.count({ntohs(from.sin_port) & 63, ntohs(to.sin_port) & 63})) return;
    std::uniform_real_distribution<double> u(0, 1);
    if (u(rng) < loss) return;
    const int copies = u(rng) < dup ? 2 : 1;
    for (int i = 0; i < copies; ++i) {
      std::uniform_int_distribution<int> delay(min_delay, max_delay);
      air.push_back({now + uint64_t(delay(rng)), from, to, std::vector<uint8_t>((const uint8_t*)d, (const uint8_t*)d + n)});
    }
  }
  void Deliver() {
    for (size_t i = 0; i < air.size();) {
      if (air[i].at <= now) {
        if (!cut[ntohs(air[i].to.sin_port) & 63]) inbox[Key(air[i].to)].push_back(std::move(air[i]));
        air[i] = std::move(air.back()); air.pop_back();
      } else ++i;
    }
  }
  int Recv(const sockaddr_in& me, char* buf, int cap, sockaddr_in& from) {
    auto& q = inbox[Key(me)];
    if (q.empty()) return -1;
    Datagram d = std::move(q.front()); q.pop_front();
    if (int(d.bytes.size()) > cap) return -1;
    std::memcpy(buf, d.bytes.data(), d.bytes.size());
    from = d.from;
    return int(d.bytes.size());
  }
};

sockaddr_in Addr(int i) {
  sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(0x0A000001); a.sin_port = htons(uint16_t(i));
  return a;
}

struct Endpoint {
  int index;
  sockaddr_in addr;
  std::unique_ptr<coopnet::Session> s;
  std::vector<coopnet::Received> got;
  std::vector<std::string> log;
};

int failures = 0;
void Check(bool ok, const std::string& what) {
  std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
  if (!ok) ++failures;
}

}  // namespace

int main(int argc, char** argv) {
  Network net;
  if (argc > 1) net.rng.seed(unsigned(std::strtoul(argv[1], nullptr, 10)));  // another seed: net_test 7
  std::vector<std::unique_ptr<Endpoint>> eps;
  auto make = [&](int index) {
    auto e = std::make_unique<Endpoint>();
    e->index = index; e->addr = Addr(index);
    Endpoint* raw = e.get();
    e->s = std::make_unique<coopnet::Session>(
        [&net, raw](const void* d, int n, const sockaddr_in& to) { net.Send(raw->addr, to, d, n); return n; },
        [&net, raw](char* b, int cap, sockaddr_in& from) { return net.Recv(raw->addr, b, cap, from); },
        [raw, &net](const std::string& l) { raw->log.push_back(std::to_string(net.now) + " " + l); });
    eps.push_back(std::move(e));
    return raw;
  };
  auto step = [&](int ms) {
    for (int t = 0; t < ms; t += 5) {
      net.now += 5;
      net.Deliver();
      for (auto& e : eps) {
        e->s->Pump(net.now);
        coopnet::Received r;
        while (e->s->Poll(r)) e->got.push_back(std::move(r));
      }
    }
  };

  // 1. A host and 11 clients join.
  Endpoint* host = make(1);
  host->s->StartHost("host");
  std::vector<Endpoint*> clients;
  for (int i = 0; i < 11; ++i) { clients.push_back(make(2 + i)); clients.back()->s->StartClient(host->addr, "player" + std::to_string(i + 1)); }
  step(4000);
  bool all_joined = true; std::set<int> slots;
  for (auto* c : clients) { all_joined &= c->s->Joined(); slots.insert(c->s->MySlot()); }
  Check(all_joined, "11 clients joined through 20% loss");
  Check(slots.size() == 11 && !slots.count(0), "each client has its own slot 1-11");
  bool counts = host->s->PlayerCount() == 12;
  for (auto* c : clients) counts &= c->s->PlayerCount() == 12;
  Check(counts, "everyone sees 12 players");

  // 2. A 13th player is refused.
  Endpoint* extra = make(20);
  extra->s->StartClient(host->addr, "late");
  step(3000);
  Check(!extra->s->Joined() && extra->s->DenyReason().find("full") != std::string::npos, "a 13th player is refused: " + extra->s->DenyReason());

  // 3. A different protocol version is refused (a hand-made join packet): any other, and the one before (co-op 3's 11 refuses 10).
  for (const uint8_t version : {uint8_t(99), uint8_t(coopnet::kVersion - 1)}) {
    // A raw address, not an Endpoint: nothing must read its inbox before the check.
    struct { sockaddr_in addr; } oldep{Addr(version == 99 ? 21 : 22)}; auto* old = &oldep;
    std::vector<uint8_t> p(sizeof(coopnet::PacketHeader) + sizeof(coopnet::MessageHeader) + 4);
    coopnet::PacketHeader ph{coopnet::kMagic, 0, coopnet::kNoSlot, 0, 0, 0, 0, 0};
    coopnet::MessageHeader mh{coopnet::kJoin, coopnet::kNoSlot, 0, 0, 0, 4, 0};
    std::memcpy(p.data(), &ph, sizeof(ph));
    std::memcpy(p.data() + sizeof(ph), &mh, sizeof(mh));
    const uint8_t body[4] = {version, 'o', 'l', 'd'};
    std::memcpy(p.data() + sizeof(ph) + sizeof(mh), body, 4);
    const double loss = net.loss; net.loss = 0;
    net.Send(old->addr, host->addr, p.data(), int(p.size()));
    step(500);
    net.loss = loss;
    bool denied = false;
    for (auto& d : net.inbox[Network::Key(old->addr)]) {
      coopnet::MessageHeader h; std::memcpy(&h, d.bytes.data() + sizeof(coopnet::PacketHeader), sizeof(h));
      denied |= h.type == coopnet::kJoinDeny;
    }
    Check(denied, "a different co-op version is refused (" + std::to_string(version) + " against " + std::to_string(coopnet::kVersion) + ")");
  }

  // 4. Every client sends 200 reliable messages to everyone; all arrive in order, once.
  for (auto& e : eps) e->got.clear();
  const int kCount = 200;
  for (int n = 0; n < kCount; ++n) {
    for (auto* c : clients) {
      uint8_t payload[8];
      const uint32_t v = uint32_t(n);
      std::memcpy(payload, &v, 4);
      payload[4] = c->s->MySlot();
      c->s->Send(coopnet::kEveryone, 100, payload, 5, true);
      c->s->Send(coopnet::kEveryone, 101, payload, 5, false);  // unreliable: may be lost
    }
    step(20);
  }
  step(8000);
  bool in_order = true, complete = true; int unreliable_seen = 0, unreliable_sent = kCount * 10 * 12;
  for (auto& e : eps) {
    if (e.get() == extra || !e->s->Joined()) continue;
    std::map<int, uint32_t> next;
    std::map<int, int> got;
    for (auto& r : e->got) {
      if (r.type == 101) { ++unreliable_seen; continue; }
      if (r.type != 100) continue;
      uint32_t v; std::memcpy(&v, r.data.data(), 4);
      if (v != next[r.origin]) in_order = false;
      next[r.origin] = v + 1;
      ++got[r.origin];
    }
    for (auto* c : clients)
      if (c != e.get() && got[c->s->MySlot()] != kCount) complete = false;
  }
  Check(complete, "200 reliable messages from each client reached every other player");
  Check(in_order, "reliable messages arrived in order and exactly once");
  std::printf("      unreliable messages delivered: %d of %d (losses expected)\n", unreliable_seen, unreliable_sent);
  Check(unreliable_seen > unreliable_sent / 2, "most unreliable messages arrive");

  // 5. The host's own messages reach every client.
  for (auto& e : eps) e->got.clear();
  for (int n = 0; n < 50; ++n) { uint32_t v = uint32_t(n); host->s->Send(coopnet::kEveryone, 102, &v, 4, true); step(10); }
  step(5000);
  bool host_msgs = true;
  for (auto* c : clients) { int k = 0; for (auto& r : c->got) if (r.type == 102 && r.origin == 0) ++k; host_msgs &= k == 50; }
  Check(host_msgs, "50 reliable messages from the host reached every client");

  // 5b. A burst of full-size unreliable messages (the host's traffic: 9 parts
  // of 1120 bytes at once) isn't cut short by the per-Pump packet limit.
  for (auto& e : eps) e->got.clear();
  {
    const double loss = net.loss, dup = net.dup; net.loss = 0; net.dup = 0;
    std::vector<uint8_t> big(coopnet::kMaxMessage, 0xAB);
    for (int n = 0; n < 20; ++n) { big[0] = uint8_t(n); host->s->Send(coopnet::kEveryone, 104, big.data(), uint16_t(big.size()), false); }
    step(1000);
    net.loss = loss; net.dup = dup;
    bool all = true;
    for (auto* c : clients) { int k = 0; for (auto& r : c->got) if (r.type == 104 && r.data.size() == big.size()) ++k; all &= k == 20; }
    Check(all, "a burst of 20 full-size unreliable messages reaches every client whole");
  }

  // 5c. LinkStats (2.2: the world stream's backpressure reads it): a link that
  // can't keep up shows its unreliable backlog cut to the limit, the drops and
  // the reliable messages in flight; they are acked once it is back.
  {
    Endpoint* c = clients[8];
    const uint8_t slot = c->s->MySlot();
    const double loss = net.loss, dup = net.dup; net.loss = 0; net.dup = 0;
    step(500);
    const coopnet::LinkStat before = host->s->LinkStats(slot);
    net.cut[c->index] = true;
    std::vector<uint8_t> big(coopnet::kMaxMessage, 0xCD);
    for (int n = 0; n < 100; ++n) host->s->Send(slot, 105, big.data(), uint16_t(big.size()), false);
    for (int n = 0; n < 20; ++n) { uint32_t v = uint32_t(n); host->s->Send(slot, 106, &v, 4, true); }
    step(5);
    const coopnet::LinkStat cut = host->s->LinkStats(slot);
    net.cut[c->index] = false;
    step(2000);
    const coopnet::LinkStat after = host->s->LinkStats(slot);
    net.loss = loss; net.dup = dup;
    Check(before.backlog == 0 && cut.dropped > before.dropped && cut.backlog <= coopnet::kMaxUnreliableBacklog && cut.in_flight >= 20 && cut.in_flight_bytes > 0,
          "LinkStats: a flooded link shows its backlog cut to the limit, the drops and the reliable messages in flight (" + std::to_string(cut.dropped) + " dropped, " + std::to_string(cut.backlog) + " B waiting, " + std::to_string(cut.in_flight) + " in flight)");
    Check(after.in_flight == 0 && after.backlog == 0 && after.rtt > 0 && host->s->LinkStats(0).in_flight == 0 && host->s->LinkStats(13).rtt == 0,
          "LinkStats: back in, everything is acked and the backlog is empty; the host's own slot and a bad slot read as no link");
  }

  // 6. A message to one slot reaches only that slot.
  for (auto& e : eps) e->got.clear();
  { uint32_t v = 7; clients[0]->s->Send(clients[5]->s->MySlot(), 103, &v, 4, true); }
  step(3000);
  int receivers = 0; bool right = false;
  for (auto& e : eps) for (auto& r : e->got) if (r.type == 103) { ++receivers; right = e.get() == clients[5]; }
  Check(receivers == 1 && right, "a message for one slot reaches only that slot (through the host)");

  // 7. A client leaves cleanly; a client vanishes (timeout).
  for (auto& e : eps) e->got.clear();
  const uint8_t leaving = clients[2]->s->MySlot(), vanishing = clients[3]->s->MySlot();
  clients[2]->s->Stop(net.now);
  net.cut[clients[3]->index] = true;
  step(9000);
  bool left_seen = true, timeout_seen = true;
  for (auto* c : {clients[0], clients[1], clients[4]}) {
    bool l = false, t = false;
    for (auto& r : c->got) if (r.type == coopnet::kPeerLeft) { l |= r.origin == leaving; t |= r.origin == vanishing; }
    left_seen &= l && !c->s->Present(leaving);
    timeout_seen &= t && !c->s->Present(vanishing);
  }
  Check(left_seen, "a player who leaves is gone for everyone");
  Check(timeout_seen, "a player who vanishes times out for everyone");
  Check(host->s->PlayerCount() == 10, "the host counts 10 players after two left");

  // 8. A new player takes a freed slot.
  extra->s->StartClient(host->addr, "late again");
  step(4000);
  Check(extra->s->Joined() && (extra->s->MySlot() == leaving || extra->s->MySlot() == vanishing), "a new player gets a freed slot");
  if (!extra->s->Joined()) {
    const coopnet::Stats xs = extra->s->TakeStats();
    std::printf("      extra: slot %d, refused: %s; packets in %u out %u bad %u\n", extra->s->MySlot(), extra->s->DenyReason().c_str(), xs.packets_in, xs.packets_out, xs.bad_packets);
    for (auto& l : extra->log) std::printf("      extra log: %s\n", l.c_str());
    for (size_t i = host->log.size() > 12 ? host->log.size() - 12 : 0; i < host->log.size(); ++i) std::printf("      host log: %s\n", host->log[i].c_str());
  }

  // 8b. The host's first packet to a new player (the join reply) is lost: it
  // is sent again (a client that had received nothing used to ack it).
  {
    Endpoint* late = make(30);
    net.drop_next[{host->index, late->index}] = 1;
    clients[6]->s->Stop(net.now);  // a free slot
    step(500);
    late->s->StartClient(host->addr, "unlucky");
    step(3000);
    Check(late->s->Joined(), "a player whose join reply is lost still joins");
  }

  // 8c. Only the host's packets to one client are lost for 7 s: the client
  // gives up on the host and joins again from the same address while the
  // host still hears it (it used to hang on "joining" for good).
  {
    for (auto& e : eps) e->got.clear();
    Endpoint* c = clients[4];
    const uint8_t before = c->s->MySlot();
    net.blocked.insert({host->index, c->index});
    step(7000);
    const bool dropped_by_client = !c->s->Joined();
    net.blocked.erase({host->index, c->index});
    step(4000);
    int left = 0, joined = 0;
    for (auto& r : clients[0]->got) { if (r.type == coopnet::kPeerLeft && r.origin == before) ++left; if (r.type == coopnet::kPeerJoined && r.origin == c->s->MySlot()) ++joined; }
    Check(dropped_by_client && c->s->Joined(), "a client cut off one way for 7 s joins again (the host still heard it)");
    Check(left == 1 && joined == 1 && host->s->PlayerCount() == clients[0]->s->PlayerCount(), "the others see it leave once and join once; counts agree");
  }

  // 8c2. The same for 40 s: the client keeps asking on one join attempt, so
  // the others see it leave once and join once (not every 6 s).
  {
    for (auto& e : eps) e->got.clear();
    Endpoint* c = clients[4];
    const uint8_t before = c->s->MySlot();
    net.blocked.insert({host->index, c->index});
    step(40000);
    net.blocked.erase({host->index, c->index});
    step(4000);
    int left = 0, joined = 0;
    for (auto& r : clients[0]->got) { if (r.type == coopnet::kPeerLeft && r.origin == before) ++left; if (r.type == coopnet::kPeerJoined) ++joined; }
    std::printf("      40 s one-way outage: %d left, %d joined\n", left, joined);
    Check(c->s->Joined() && left == 1 && joined == 1, "a 40 s one-way outage: one leave and one join seen by the others, then back in");
  }

  // 8d. A client leaves and joins again from the same address right away,
  // and its leave messages are lost: the host takes the new attempt at once.
  {
    Endpoint* c = clients[5];
    net.drop_next[{c->index, host->index}] = 3;   // the three leave packets
    c->s->Stop(net.now);
    c->s->StartClient(host->addr, "again");
    step(3000);
    Check(c->s->Joined(), "a client that left (leave lost) and joined again from the same address is back within 3 s");
    Check(host->s->PlayerCount() == clients[0]->s->PlayerCount(), "no ghost slot is left behind");
  }

  // 8e. A burst of reliable messages through a 5 s outage (over 2000 waiting):
  // every one arrives once and in order afterwards (the receiver holds only
  // 2048 early ones; without a send window the link stalled for good).
  {
    for (auto& e : eps) e->got.clear();
    Endpoint* a = clients[0];
    Endpoint* b = clients[1];
    net.cut[a->index] = true;
    for (int n = 0; n < 3000; ++n) { const uint32_t v = uint32_t(n); a->s->Send(coopnet::kEveryone, 105, &v, 4, true); if (n % 100 == 0) step(150); }
    net.cut[a->index] = false;
    step(20000);
    uint32_t next = 0; bool ordered = true; int got = 0;
    for (auto& r : b->got) if (r.type == 105 && r.origin == a->s->MySlot()) { uint32_t v; std::memcpy(&v, r.data.data(), 4); ordered &= v == next; next = v + 1; ++got; }
    std::printf("      after the outage: %d of 3000 arrived\n", got);
    Check(a->s->Joined() && got == 3000 && ordered, "3000 reliable messages queued through a 5 s outage arrive once, in order");
  }

  // 8f. A join with a 1500-byte name: the name is cut and nobody's reliable
  // channel stalls on an oversized message.
  {
    for (auto& e : eps) e->got.clear();
    clients[7]->s->Stop(net.now);   // a free slot
    step(1000);
    std::vector<uint8_t> p(sizeof(coopnet::PacketHeader) + sizeof(coopnet::MessageHeader) + 1 + 1000);
    coopnet::PacketHeader ph{coopnet::kMagic, coopnet::kVersion, coopnet::kNoSlot, 0, 7, 0, 0, 0};
    coopnet::MessageHeader mh{coopnet::kJoin, coopnet::kNoSlot, 0, 0, 0, uint16_t(1 + 1000), 0};
    std::memcpy(p.data(), &ph, sizeof(ph));
    std::memcpy(p.data() + sizeof(ph), &mh, sizeof(mh));
    p[sizeof(ph) + sizeof(mh)] = coopnet::kVersion;
    std::memset(p.data() + sizeof(ph) + sizeof(mh) + 1, 'x', 1000);
    const double loss = net.loss; net.loss = 0;
    net.Send(Addr(40), host->addr, p.data(), int(p.size()));
    net.loss = loss;
    step(1000);
    for (int n = 0; n < 20; ++n) { const uint32_t v = uint32_t(n); host->s->Send(coopnet::kEveryone, 106, &v, 4, true); }
    step(4000);
    int got = 0; size_t longest = 0;
    for (auto& r : clients[0]->got) { if (r.type == 106) ++got; if (r.type == coopnet::kPeerJoined) longest = std::max(longest, r.data.size()); }
    Check(got == 20 && longest > 0 && longest <= coopnet::kMaxName, "an oversized join name is cut to 32 bytes and reliable messages keep flowing");
  }

  // 9. The host vanishes: clients notice and keep trying to join.
  for (auto& e : eps) e->got.clear();
  net.cut[host->index] = true;
  step(9000);
  bool lost = true;
  for (auto* c : {clients[0], clients[1]}) {
    bool l = false;
    for (auto& r : c->got) l |= r.type == coopnet::kPeerLeft && r.origin == 0;
    lost &= l && !c->s->Joined();
  }
  Check(lost, "clients notice that the host is gone");
  net.cut[host->index] = false;
  host->s->StartHost("host again");
  step(5000);
  Check(clients[0]->s->Joined() && clients[1]->s->Joined(), "clients join again when the host is back");

  // A session host and n clients with migration on, joined one after the
  // other, so each client's slot is its seniority too (g[i] is slot i once
  // the caller checked it). `codec`: addresses through an AddressCodec, as
  // online (a re-link is then checked by its address, not its token).
  auto migration_group = [&](std::vector<int> index, const std::string& tag, bool codec) {
    std::vector<Endpoint*> g;
    for (size_t i = 0; i < index.size(); ++i) {
      Endpoint* e = make(index[i]);
      e->s->EnableMigration(true);
      if (codec)
        e->s->AddressCodec([](const sockaddr_in& a) { std::string t(6, '\0'); std::memcpy(&t[0], &a.sin_addr.s_addr, 4); std::memcpy(&t[4], &a.sin_port, 2); return t; },
                           [](const std::string& t, sockaddr_in& a) { if (t.size() != 6) return false; a = sockaddr_in{}; a.sin_family = AF_INET; std::memcpy(&a.sin_addr.s_addr, t.data(), 4); std::memcpy(&a.sin_port, t.data() + 4, 2); return true; });
      if (i == 0) e->s->StartHost(tag + "host"); else e->s->StartClient(g[0]->addr, tag + std::to_string(i));
      g.push_back(e);
      for (int t = 0; t < 5000 && i && !e->s->Joined(); t += 100) step(100);
      step(300);
    }
    step(4000);   // (the address list, the succession order and a few warm packets)
    return g;
  };
  auto group_ok = [](const std::vector<Endpoint*>& g) {
    bool ok = true;
    for (size_t i = 1; i < g.size(); ++i) ok &= g[i]->s->Joined() && g[i]->s->MySlot() == i && g[i]->s->PlayerCount() == int(g.size());
    return ok;
  };
  auto stop_group = [&](const std::vector<Endpoint*>& g) { for (auto* e : g) e->s->Stop(net.now); step(500); };
  auto print_log = [](Endpoint* e, size_t n) { for (size_t k = e->log.size() > n ? e->log.size() - n : 0; k < e->log.size(); ++k) std::printf("      slot %d log: %s\n", e->s->MySlot(), e->log[k].c_str()); };

  // 10. Host migration (co-op 3 phase 4): a session host and 4 clients with
  // migration on. The host vanishes: everyone picks slot 1 (the most
  // senior), which becomes the host and the relay; the slots stay; what was
  // sent meanwhile arrives once, in order. Then that host leaves cleanly
  // (slot 2 next, at once), a new player joins the moved session, and a
  // client whose own link to the host fails (the host still there) gets back
  // in without moving the session.
  {
    std::vector<Endpoint*> g = migration_group({50, 51, 52, 53, 54}, "m", false);
    Endpoint* mh = g[0];
    std::vector<Endpoint*> m(g.begin() + 1, g.end());   // (m[i] is slot i + 1)
    Check(group_ok(g), "migration: a host and 4 clients joined (slots 1-4)");
    for (auto& e : eps) e->got.clear();
    std::array<uint8_t, 4> slot_before{};
    for (int i = 0; i < 4; ++i) slot_before[i] = m[i]->s->MySlot();
    net.cut[mh->index] = true;
    // Sent during the gap (never acked by the old host): carried to the new one.
    for (int n = 0; n < 60; ++n) { const uint32_t v = uint32_t(n); m[2]->s->Send(coopnet::kEveryone, 110, &v, 4, true); step(100); }
    step(10000);
    bool moved = true, same_slots = true, counts = true;
    for (int i = 0; i < 4; ++i) {
      bool hm = false, left = false;
      for (auto& r : m[i]->got) { hm |= r.type == coopnet::kHostMoved && r.origin == 1 && !r.data.empty() && r.data[0] == 0; left |= r.type == coopnet::kPeerLeft && r.origin == 0; }
      moved &= hm && left && m[i]->s->HostSlot() == 1 && m[i]->s->IsHost() == (i == 0) && !m[i]->s->Relinking();
      same_slots &= m[i]->s->MySlot() == slot_before[i] && m[i]->s->Joined();
      counts &= m[i]->s->PlayerCount() == 4 && !m[i]->s->Present(0);
    }
    Check(moved, "migration: the host vanishes; everyone picks slot 1 (the most senior), which is the session host now");
    Check(same_slots && counts, "migration: every client keeps its slot; everyone counts 4 players");
    // The relay: what slot 3 sent during the gap, then more, reaches the others once and in order.
    for (int n = 60; n < 100; ++n) { const uint32_t v = uint32_t(n); m[2]->s->Send(coopnet::kEveryone, 110, &v, 4, true); step(20); }
    { const uint32_t v = 7; m[0]->s->Send(coopnet::kEveryone, 111, &v, 4, true); m[1]->s->Send(m[3]->s->MySlot(), 112, &v, 4, true); }
    step(5000);
    bool relayed = true, own = true, one = true;
    for (int i : {0, 1, 3}) {
      uint32_t next = 0; int got = 0; bool ordered = true;
      for (auto& r : m[i]->got) if (r.type == 110 && r.origin == m[2]->s->MySlot()) { uint32_t v; std::memcpy(&v, r.data.data(), 4); ordered &= v == next; next = v + 1; ++got; }
      relayed &= got == 100 && ordered;
      if (i != 0) { int k = 0; for (auto& r : m[i]->got) k += r.type == 111 && r.origin == 1; own &= k == 1; }
    }
    { int k = 0; for (auto* c : m) for (auto& r : c->got) k += r.type == 112; int k3 = 0; for (auto& r : m[3]->got) k3 += r.type == 112; one = k == 1 && k3 == 1; }
    const coopnet::Stats ms = m[2]->s->TakeStats();
    std::printf("      migration: slot 3 carried %u reliable messages to the new host, %u warm packets\n", ms.carried, ms.warm);
    Check(relayed, "migration: 100 reliable messages from slot 3 (60 sent while the host was gone) reach every other player once, in order");
    Check(own && one, "migration: the new host's own messages reach everyone; a message for one slot is relayed to that slot alone");
    // The new host leaves cleanly: slot 2 at once (its kLeave), not after 6 s.
    for (auto& e : eps) e->got.clear();
    m[0]->s->Stop(net.now);
    step(3000);
    bool next_moved = true;
    for (int i = 1; i < 4; ++i) {
      bool hm = false;
      for (auto& r : m[i]->got) hm |= r.type == coopnet::kHostMoved && r.origin == 2;
      const bool ok = hm && m[i]->s->HostSlot() == 2 && m[i]->s->Joined() && m[i]->s->PlayerCount() == 3;
      next_moved &= ok;
      if (!ok) { std::printf("      slot %d: moved %d, host slot %d, joined %d, players %d\n", m[i]->s->MySlot(), hm, m[i]->s->HostSlot(), m[i]->s->Joined(), m[i]->s->PlayerCount()); print_log(m[i], 6); }
    }
    Check(next_moved && m[1]->s->IsHost(), "migration: the new host leaves; within 3 s slot 2 is the session host, 3 players left");
    // A new player joins the moved session at its host's address: a slot
    // nobody had (5), not one just left (review_phase4 L3).
    Endpoint* late = make(58);
    late->s->EnableMigration(true);
    late->s->StartClient(m[1]->addr, "mlate");
    step(4000);
    Check(late->s->Joined() && late->s->HostSlot() == 2 && late->s->MySlot() == 5 && m[3]->s->PlayerCount() == 4,
          "migration: a new player joins the moved session (slot " + std::to_string(late->s->MySlot()) + ": one nobody had; the host is slot 2)");
    step(4000);   // (it is in the succession order now, the least senior)
    // Only the host's packets to slot 4 are lost for 7 s: slot 4 takes the
    // host for gone and tries the next successor (slot 3), which is no host
    // and doesn't answer; then itself, until the host (which dropped it)
    // warms it as a session host: it yields and joins again. Nobody else moves.
    for (auto& e : eps) e->got.clear();
    Endpoint* c4 = m[3];
    net.blocked.insert({m[1]->index, c4->index});
    step(7000);
    net.blocked.erase({m[1]->index, c4->index});
    step(25000);
    bool others_stayed = true;
    for (auto* e : {m[1], m[2], late}) for (auto& r : e->got) others_stayed &= r.type != coopnet::kHostMoved;
    bool c4_tried = false;
    for (auto& r : c4->got) c4_tried |= r.type == coopnet::kHostMoved;
    Check(c4_tried && c4->s->Joined() && c4->s->HostSlot() == 2 && others_stayed && m[1]->s->PlayerCount() == 4 && c4->s->PlayerCount() == 4,
          "migration: a client whose link alone fails tries the successors, then joins the host again; nobody else moves");
    if (!(c4_tried && c4->s->Joined() && c4->s->HostSlot() == 2)) print_log(c4, 10);
    // The same for the newest player: it tries slot 3, then takes itself for
    // the successor; nobody links to it, and it joins the host again.
    for (auto& e : eps) e->got.clear();
    net.blocked.insert({m[1]->index, late->index});
    step(7000);
    net.blocked.erase({m[1]->index, late->index});
    step(30000);
    bool stayed = true, late_tried = false;
    for (auto* e : {m[1], m[2], m[3]}) for (auto& r : e->got) stayed &= r.type != coopnet::kHostMoved;
    for (auto& r : late->got) late_tried |= r.type == coopnet::kHostMoved;
    Check(late_tried && !late->s->IsHost() && late->s->Joined() && late->s->HostSlot() == 2 && stayed && m[1]->s->PlayerCount() == 4,
          "migration: the newest player's link alone fails: it tries the successors, then joins the host again; nobody else moves");
    if (!(late_tried && late->s->Joined() && late->s->HostSlot() == 2)) print_log(late, 10);
    stop_group(g);
    late->s->Stop(net.now);
    step(500);
  }

  // 11. (review_phase4 M1) The host is cut while slot 3 streams reliable
  // messages, and its acks to slot 3 were lost for a second before: slot 3
  // carries messages the old host had relayed already. Every receiver gets
  // each message at most once, in order, and everything sent after the re-link.
  // (Losses stay losses: what the old host held out of order, acked, and
  // never delivered is gone, as before.)
  {
    std::vector<Endpoint*> g = migration_group({23, 24, 25, 26, 27}, "d", false);
    Check(group_ok(g), "carry-over: a host and 4 clients joined");
    for (auto& e : eps) e->got.clear();
    uint32_t sent = 0;
    auto stream = [&](int ms) { for (int t = 0; t < ms; t += 20) { const uint32_t v = sent++; g[3]->s->Send(coopnet::kEveryone, 120, &v, 4, true); step(20); } };
    stream(1000);
    const double loss = net.loss; net.loss = 0;   // (so the old host surely has, and relays, what slot 3 sends now)
    net.blocked.insert({g[0]->index, g[3]->index});   // (slot 3's acks lost; the host still relays)
    stream(1000);
    net.loss = loss;
    net.cut[g[0]->index] = true;
    stream(2000);
    net.blocked.erase({g[0]->index, g[3]->index});
    step(12000);
    const uint32_t after = sent;
    stream(1000);
    step(4000);
    bool once = true, newest = true; uint32_t dups = 0;
    for (int i : {1, 2, 4}) {
      int64_t last = -1; bool tail = false;
      for (auto& r : g[i]->got) if (r.type == 120 && r.origin == 3) { uint32_t v; std::memcpy(&v, r.data.data(), 4); once &= int64_t(v) > last; last = v; tail |= v == after; }
      newest &= tail && last == int64_t(sent - 1);
      dups += g[i]->s->TakeStats().duplicates;
    }
    std::printf("      carry-over: %u reliable messages dropped as delivered already\n", dups);
    if (!dups || !once || !newest) for (auto* e : g) print_log(e, 6);
    Check(once && dups > 0, "carry-over: messages carried that the old host had relayed reach everyone at most once, in order (" + std::to_string(dups) + " duplicates dropped)");
    Check(newest, "carry-over: everything sent after the re-link arrives");
    stop_group(g);
  }

  // 12. (review_phase4 M2) The host and slot 1 drop together: every game
  // tries slot 1, gives up on it after kRejoinMs, and slot 2 takes over.
  // (With an AddressCodec, as online: the re-link is checked by its address.)
  {
    std::vector<Endpoint*> g = migration_group({31, 32, 33, 34, 35}, "t", true);
    Check(group_ok(g), "two drops: a host and 4 clients joined (address codec)");
    for (auto& e : eps) e->got.clear();
    net.cut[g[0]->index] = true; net.cut[g[1]->index] = true;
    step(35000);   // (6 s to notice, 7 s on slot 1, 20 s until the new host stops waiting for it)
    bool ok = true;
    for (int i = 2; i <= 4; ++i) {
      const bool k = g[i]->s->Joined() && g[i]->s->MySlot() == i && g[i]->s->HostSlot() == 2 && g[i]->s->PlayerCount() == 3 && !g[i]->s->Relinking();
      ok &= k;
      if (!k) print_log(g[i], 8);
    }
    Check(ok && g[2]->s->IsHost(), "two drops: the host and slot 1 drop together; slot 2 takes over, slots kept, 3 players once it stops waiting for slot 1");
    stop_group(g);
  }

  // 13. (review_phase4 M2) The successor leaves 3 s after taking over, while
  // one client (slot 2, its link to the successor cut) still links to it: its
  // kLeave reaches that client too, and everyone ends up under slot 2.
  {
    std::vector<Endpoint*> g = migration_group({41, 42, 43, 44, 45}, "s", false);
    Check(group_ok(g), "successor leaves: a host and 4 clients joined");
    for (auto& e : eps) e->got.clear();
    net.blocked.insert({g[2]->index, g[1]->index});
    net.cut[g[0]->index] = true;
    step(9000);
    const bool pending = g[2]->s->Relinking() && g[1]->s->IsHost();
    g[1]->s->Stop(net.now);
    step(5000);
    net.blocked.erase({g[2]->index, g[1]->index});
    step(3000);
    bool ok = true;
    for (int i = 2; i <= 4; ++i) {
      const bool k = g[i]->s->Joined() && g[i]->s->MySlot() == i && g[i]->s->HostSlot() == 2 && g[i]->s->PlayerCount() == 3 && !g[i]->s->Relinking();
      ok &= k;
      if (!k) print_log(g[i], 8);
    }
    Check(pending && ok && g[2]->s->IsHost(), "successor leaves: it stops 3 s after taking over with slot 2 still linking; all end up under slot 2 within 5 s");
    stop_group(g);
  }

  // 14. A player joins the new host while a client is still linking to it
  // again: it gets a slot nobody waits for, and once the late client is back
  // everyone counts 5.
  {
    std::vector<Endpoint*> g = migration_group({36, 37, 38, 39, 46}, "j", false);
    Check(group_ok(g), "join during migration: a host and 4 clients joined");
    for (auto& e : eps) e->got.clear();
    net.blocked.insert({g[4]->index, g[1]->index});
    net.cut[g[0]->index] = true;
    step(7000);
    Endpoint* joiner = make(47);
    joiner->s->EnableMigration(true);
    joiner->s->StartClient(g[1]->addr, "jnew");
    step(3000);
    const bool waits = g[4]->s->Relinking();
    net.blocked.erase({g[4]->index, g[1]->index});
    step(4000);
    bool counts = joiner->s->Joined() && joiner->s->MySlot() >= 5 && joiner->s->HostSlot() == 1;
    for (int i = 1; i <= 4; ++i) counts &= g[i]->s->Joined() && g[i]->s->MySlot() == i && g[i]->s->PlayerCount() == 5;
    Check(waits && counts, "join during migration: the new player gets slot " + std::to_string(joiner->s->MySlot()) + " (not a slot that waits); the late client is back; everyone counts 5");
    stop_group(g);
    joiner->s->Stop(net.now);
    step(500);
  }

  // 15. (review_phase4 M4) A stranger (another port, the same IP) spams
  // kRejoin as slot 3 with its name but not its token, before and during a
  // migration (the real slot 3 late): refused; the real slot 3 links again;
  // nobody new appears.
  // TokenHash is SHA-256's first 8 bytes.
  {
    Check(coopnet::TokenHash(0) == 0xaf5570f5a1810b7aull && coopnet::TokenHash(0x0123456789ABCDEFull) == 0xa85ba2b36261d0dcull, "rejoin auth: TokenHash is SHA-256 (two known values)");
    std::vector<Endpoint*> g = migration_group({14, 15, 16, 17, 18}, "a", false);
    Check(group_ok(g), "rejoin auth: a host and 4 clients joined");
    for (auto& e : eps) e->got.clear();
    const sockaddr_in spoof = Addr(19);
    auto spam = [&](int ms) {
      for (int t = 0; t < ms; t += 100) {
        const std::string name = "a3";
        std::vector<uint8_t> p(sizeof(coopnet::PacketHeader) + sizeof(coopnet::MessageHeader) + 10 + name.size());
        const coopnet::PacketHeader ph{coopnet::kMagic, coopnet::kVersion, 3, 0, 9, uint16_t(t / 100), 0, 0};
        const coopnet::MessageHeader mh{coopnet::kRejoin, 3, 1, 0, 0, uint16_t(10 + name.size()), 0};
        std::memcpy(p.data(), &ph, sizeof(ph));
        std::memcpy(p.data() + sizeof(ph), &mh, sizeof(mh));
        uint8_t* b = p.data() + sizeof(ph) + sizeof(mh);
        b[0] = coopnet::kVersion; b[1] = 3; std::memset(b + 2, 0x5A, 8); std::memcpy(b + 10, name.data(), name.size());
        net.Send(spoof, g[1]->addr, p.data(), int(p.size()));
        step(100);
      }
    };
    spam(1000);
    net.blocked.insert({g[3]->index, g[1]->index});   // (the real slot 3 can't reach the new host at first: the stranger's kRejoin comes while its slot waits)
    net.cut[g[0]->index] = true;
    spam(9000);
    net.blocked.erase({g[3]->index, g[1]->index});
    spam(3000);
    step(3000);
    const coopnet::Stats as = g[1]->s->TakeStats();
    bool ok = g[1]->s->IsHost() && g[1]->s->PlayerCount() == 4;
    for (int i = 2; i <= 4; ++i) ok &= g[i]->s->Joined() && g[i]->s->MySlot() == i && g[i]->s->HostSlot() == 1 && g[i]->s->PlayerCount() == 4;
    std::printf("      rejoin auth: %u re-links refused\n", as.rejoin_refused);
    Check(ok && as.rejoin_refused > 0, "rejoin auth: a stranger's kRejoin with the wrong token is refused; the real slot 3 links again; 4 players");
    stop_group(g);
  }

  // 16. (review_final_shared H1) Claims after a player leaves, with each
  // game's ClaimGate fed its session messages and a game claiming a player's
  // objects as HandClaimLater / HandTick do: 2 s (the claim's own delay) after
  // the gate first lets it, if it still does then. Slot 3's link to the host fails one way for 7 s: it takes
  // the host for gone, tries the successors and ends in a fresh join; the
  // host drops it as timed out. Nobody claims what slot 3 still owns, and slot
  // 3 never claims the host's (no doubled ownership). Then a player that
  // leaves cleanly is claimed 2 s later, and one that times out and stays
  // away 8 s later than that.
  {
    std::vector<Endpoint*> g = migration_group({60, 61, 62, 63}, "c", false);
    Check(group_ok(g), "claims: a host and 3 clients joined");
    const std::array<std::string, 4> names = {"chost", "c1", "c2", "c3"};
    struct Claims {
      coopnet::ClaimGate gate;
      size_t fed = 0;
      std::array<bool, coopnet::kMaxSlots> waiting{};
      std::array<uint64_t, coopnet::kMaxSlots> due{};
      std::array<std::string, coopnet::kMaxSlots> left_name{};
      std::set<std::string> claimed;
      uint32_t called_off = 0;
    };
    std::map<Endpoint*, Claims> cl;
    for (auto* e : g) { e->got.clear(); cl[e]; }
    auto run = [&](int ms) {
      for (int t = 0; t < ms; t += 100) {
        step(100);
        for (auto* e : g) {
          Claims& c = cl[e];
          for (; c.fed < e->got.size(); ++c.fed) {
            const coopnet::Received& r = e->got[c.fed];
            if (r.type >= coopnet::kFirstUserType) continue;
            const std::string name = r.type == coopnet::kPeerJoined ? std::string(r.data.begin(), r.data.end()) : r.type == coopnet::kPeerLeft ? e->s->Name(r.origin) : std::string();
            c.gate.Feed(r, e->s->MySlot(), name, net.now);
            if (r.type == coopnet::kPeerLeft && r.origin < coopnet::kMaxSlots) { c.waiting[r.origin] = true; c.due[r.origin] = 0; c.left_name[r.origin] = name; }
            if (r.type == coopnet::kPeerJoined && r.origin < coopnet::kMaxSlots) c.waiting[r.origin] = false;
            if (r.type == coopnet::kJoinOk) c.waiting.fill(false);
          }
          c.called_off += c.gate.TakeCounts().called_off;
          for (int s = 0; s < coopnet::kMaxSlots; ++s) {
            if (!c.waiting[s] || !e->s->Joined() || e->s->Present(s)) continue;
            const uint8_t st = c.gate.State(s, net.now);
            if (st == coopnet::ClaimGate::kCalledOff) c.waiting[s] = false;
            else if (st == coopnet::ClaimGate::kWait) c.due[s] = 0;
            else if (!c.due[s]) c.due[s] = net.now + 2000;
            else if (net.now >= c.due[s]) { c.claimed.insert(c.left_name[s]); c.waiting[s] = false; }
          }
        }
      }
    };
    auto in_session = [&](const std::string& n) { for (size_t i = 0; i < g.size(); ++i) if (names[i] == n && g[i]->s->Joined() && g[i]->s->HostSlot() == g[0]->s->MySlot()) return true; return false; };   // (one cut off, its own session host now: out of the session)
    auto doubled = [&]() {
      std::string d;
      for (size_t i = 0; i < g.size(); ++i) for (auto& n : cl[g[i]].claimed) if (in_session(n)) d += " " + names[i] + " claimed " + n + ";";
      return d;
    };
    Endpoint* x = g[3];
    net.blocked.insert({g[0]->index, x->index});
    run(7000);
    net.blocked.erase({g[0]->index, x->index});
    run(30000);
    bool x_moved = false;
    for (auto& r : x->got) x_moved |= r.type == coopnet::kHostMoved;
    const std::string d1 = doubled();
    std::printf("      claims: slot 3 back as slot %d (a successor told it the host is still there: %u); the same name called off at the host %u, slot 1 %u, slot 2 %u\n", int(x->s->MySlot()), x->s->TakeStats().still_with_host, cl[g[0]].called_off, cl[g[1]].called_off, cl[g[2]].called_off);
    Check(x_moved && x->s->Joined() && x->s->MySlot() != 3 && x->s->HostSlot() == 0 && g[0]->s->PlayerCount() == 4,
          "claims: slot 3's link to the host fails one way; it tries the successors and joins again as a new player");
    Check(d1.empty() && cl[g[1]].called_off && cl[g[2]].called_off && cl[g[0]].called_off, "claims: nobody claims the objects of the player who comes back, and it claims none of the host's (no doubled ownership)" + d1);
    if (!d1.empty()) for (auto* e : g) print_log(e, 8);
    // A player that leaves cleanly: claimed 2 s later.
    { const double loss = net.loss; net.loss = 0; g[2]->s->Stop(net.now); net.loss = loss; }   // (its kLeave surely arrives)
    run(4000);
    Check(cl[g[0]].claimed.count("c2") && cl[g[1]].claimed.count("c2") && cl[x].claimed.count("c2"), "claims: a player that leaves cleanly is claimed 2 s later");
    // A player that times out and stays away: claimed 8 s later than that.
    net.cut[g[1]->index] = true;
    run(12000);
    const bool early = cl[g[0]].claimed.count("c1") || cl[x].claimed.count("c1");
    run(6000);
    const std::string d2 = doubled();
    Check(!early && cl[g[0]].claimed.count("c1") && cl[x].claimed.count("c1") && d2.empty(), "claims: a player that times out and stays away is claimed 8 s later than one that left" + d2);
    net.cut[g[1]->index] = false;
    stop_group(g);
  }

  // 17. (review_final_shared M8) The relay's filter: the host holds back one
  // client's unreliable messages of a type for another client; reliable ones
  // and the other targets are untouched; the relay counts what it passed on.
  {
    std::vector<Endpoint*> g = migration_group({48, 49, 55}, "r", false);
    Check(group_ok(g), "relay filter: a host and 2 clients joined");
    g[0]->s->RelayFilter([](uint8_t type, uint8_t origin, uint8_t target, const uint8_t*, uint16_t) { return !(type == 130 && origin == 1 && target == 2); });
    g[0]->s->TakeStats();
    for (auto& e : eps) e->got.clear();
    for (int n = 0; n < 20; ++n) { const uint32_t v = uint32_t(n); g[1]->s->Send(coopnet::kEveryone, 130, &v, 4, n % 2 == 0); g[2]->s->Send(coopnet::kEveryone, 130, &v, 4, false); step(50); }
    step(3000);
    int to2_rel = 0, to2_unrel_from1 = 0, at_host = 0, to1 = 0;
    for (auto& r : g[2]->got) if (r.type == 130 && r.origin == 1) { uint32_t v; std::memcpy(&v, r.data.data(), 4); (v % 2 == 0 ? to2_rel : to2_unrel_from1)++; }
    for (auto& r : g[0]->got) at_host += r.type == 130 && r.origin == 1;
    for (auto& r : g[1]->got) to1 += r.type == 130 && r.origin == 2;
    const coopnet::Stats rs = g[0]->s->TakeStats();
    std::printf("      relay filter: relayed %u (%u bytes), held back %u; slot 2 got %d reliable, %d unreliable from slot 1; slot 1 got %d from slot 2\n", rs.relayed, rs.relayed_bytes, rs.relay_thinned, to2_rel, to2_unrel_from1, to1);
    Check(to2_rel == 10 && to2_unrel_from1 == 0 && at_host >= 10 && to1 > 0 && rs.relay_thinned > 0 && rs.relayed > 0, "relay filter: the held-back messages never reach that client; reliable ones and the other direction do");
    stop_group(g);
  }

  // 18. (Protocol 12) The relay's pose tiers (world.h RelayTiers as the
  // filter): the host at 0 m and clients at 20 / 100 / 700 m send WPP2 poses
  // ~29 a second for 10 s. Each gets the host's own pose and the others' by
  // distance (30 / 10 / 3 Hz); a state bit flip passes at once; with
  // PaceUnreliable(10) a link carries at most ~100 packets a second.
  {
    const double loss = net.loss, dup = net.dup;
    net.loss = 0; net.dup = 0;
    std::vector<Endpoint*> g = migration_group({19, 28, 29, 40}, "p", false);
    Check(group_ok(g), "pose tiers: a host and 3 clients joined");
    auto tiers = std::make_shared<coopworld::RelayTiers>();
    g[0]->s->RelayFilter([tiers](uint8_t, uint8_t origin, uint8_t target, const uint8_t* d, uint16_t n) {
      if (target >= coopnet::kMaxSlots) { tiers->Note(origin, d, n); return true; }
      return tiers->Pass(origin, target, d, n);
    });
    g[0]->s->PaceUnreliable(10);
    const float at[4] = {0, 20, 100, 700};
    for (auto& e : eps) e->got.clear();
    g[0]->s->TakeStats();
    const uint64_t start = net.now;
    uint32_t seq = 0, flip_at = 0;
    for (int t = 0; t < 10000; t += 5) {
      if (t % 35 == 0) {
        ++seq;
        for (int i = 0; i < 4; ++i) {
          coopworld::PoseData p; p.sequence = seq; p.time = uint32_t(net.now); p.pos[0] = at[i]; p.rows[0] = p.rows[4] = p.rows[8] = 1;
          if (i == 3 && t >= 5005) { p.flags = 1u << 19; if (!flip_at) flip_at = seq; }   // (slot 3 dies at 5 s)
          uint8_t b[coopworld::kPoseInCar];
          g[i]->s->Send(coopnet::kEveryone, coopnet::kFirstUserType, b, uint16_t(coopworld::EncodePose(p, b)), false);
        }
      }
      step(5);
    }
    step(500);
    const double secs = (net.now - start - 500) / 1000.0;
    const coopnet::Stats ts = g[0]->s->TakeStats();
    int from[4][4] = {};   // [receiver][origin]
    uint32_t first_dead[4] = {};   // the first dead pose of slot 3 each got (its sequence)
    for (int i = 1; i < 4; ++i)
      for (auto& r : g[size_t(i)]->got)
        if (r.type == coopnet::kFirstUserType && r.origin < 4) {
          ++from[i][r.origin];
          coopworld::PoseData p;
          if (r.origin == 3 && coopworld::DecodePose(r.data.data(), r.data.size(), p) && (p.flags & (1u << 19)) && (!first_dead[i] || p.sequence < first_dead[i])) first_dead[i] = p.sequence;
        }
    // (The flip itself, or the next pose when the network reordered them: the relay holds a pose older than one it
    // passed. Without passing at once it would be ~10 poses later at 700 m.)
    bool flip_seen[4] = {};
    for (int i = 1; i < 3; ++i) flip_seen[i] = first_dead[i] && first_dead[i] <= flip_at + 2;
    auto hz = [&](int n) { return n / secs; };
    std::printf("      pose tiers: slot 1 (20 m) gets host %.1f, slot 2 %.1f, slot 3 %.1f Hz; slot 2 (100 m) gets host %.1f, slot 3 %.1f; slot 3 (700 m) gets host %.1f, slot 1 %.1f Hz; held back %u, host packets %.0f/s on 3 links\n",
                hz(from[1][0]), hz(from[1][2]), hz(from[1][3]), hz(from[2][0]), hz(from[2][3]), hz(from[3][0]), hz(from[3][1]), ts.relay_thinned, ts.packets_out / secs);
    auto close_to = [](double v, double want) { return v >= want * 0.88 && v <= want * 1.12; };
    Check(close_to(hz(from[1][0]), 28.6) && close_to(hz(from[1][2]), 9.5) && close_to(hz(from[2][0]), 9.5) && close_to(hz(from[3][0]), 2.86) && close_to(hz(from[3][1]), 2.86) && close_to(hz(from[1][3]), 2.86) && close_to(hz(from[2][3]), 2.86),
          "pose tiers: the host's own pose and the relayed ones go by distance (20 m every pose, 100 m 10 a second, 700 m 3 a second)");
    Check(flip_seen[1] && flip_seen[2], "pose tiers: a state bit flip (slot 3 dead at 700 m) passes at once");
    Check(ts.packets_out / secs < 3 * 105, "pose tiers: PaceUnreliable(10): at most ~100 packets a second per link");
    g[0]->s->PaceUnreliable(0);
    stop_group(g);
    net.loss = loss; net.dup = dup;
  }

  // 19. mod.ini max_players (3.0): a host that takes 3 players refuses the
  // fourth with a notice that says why, and its clients learn the cap from
  // kJoinOk (the player list's CO-OP n/3). When a player leaves, the next
  // one gets in.
  {
    Endpoint* h = make(13);
    h->s->MaxPlayers(3);
    h->s->StartHost("cap");
    std::vector<Endpoint*> in;
    for (int i : {56, 57}) { in.push_back(make(i)); in.back()->s->StartClient(h->addr, "cap" + std::to_string(i)); }
    step(3000);
    Endpoint* late = make(59);
    late->s->StartClient(h->addr, "cap59");
    step(3000);
    Check(in[0]->s->Joined() && in[1]->s->Joined() && h->s->PlayerCount() == 3, "max_players 3: the host and 2 clients are in");
    Check(!late->s->Joined() && late->s->DenyReason().find("full (3 players") != std::string::npos, "max_players 3: a 4th player is refused: " + late->s->DenyReason());
    Check(in[0]->s->SessionMax() == 3 && in[1]->s->SessionMax() == 3 && h->s->SessionMax() == 3 && late->s->SessionMax() == coopnet::kMaxSlots,
          "max_players 3: the clients learn the host's cap (a client not in keeps its own)");
    in[1]->s->Stop(net.now);
    step(7000);
    late->s->StartClient(h->addr, "cap59");
    step(3000);
    Check(late->s->Joined() && h->s->PlayerCount() == 3, "max_players 3: a player who left makes room for the next");
    h->s->MaxPlayers(40);
    Check(h->s->SessionMax() == coopnet::kMaxSlots, "max_players: clamped to the 12 slots");
    for (auto* e : {h, in[0], late}) e->s->Stop(net.now);
    step(500);
  }

  // 20. Protocol 13: kEventAnimSet and kEventVoice (world.h PackAnimSet /
  // PackVoice) encode and decode, the voice gate and range, and both through
  // the session as coop.cpp sends them (its 48-byte EventPacket, reliable):
  // the set before the action it is for, a line only to the player in range.
  {
    using namespace coopworld;
    Check(NameHashNoCase("PML1") == NameHashNoCase("pml1") && NameHashNoCase("PML1") != NameHashNoCase("PFL1") && NameHashNoCase("") == 0 &&
          NameHashNoCase(nullptr) == 0 && NameHashNoCase("SP_BMPLAYER") != 0 && NameHashNoCase("SP_BMPLAYERXX", 11) == NameHashNoCase("sp_bmplayer"),
          "voice / emote sets: names hash without case (as 82229278 / 8221A5C0 compare them), bounded");
    uint32_t a[8]; std::memset(a, 0xEE, sizeof(a));
    PackAnimSet(AnimSetMsg{37, NameHashNoCase("PML1"), true}, a);
    const AnimSetMsg s = UnpackAnimSet(a);
    std::memset(a, 0xEE, sizeof(a));
    PackAnimSet(AnimSetMsg{5, NameHashNoCase("GML1"), false}, a);
    const AnimSetMsg own = UnpackAnimSet(a);
    const uint32_t noname[8] = {3, 0, 1, 0, 0, 0, 0, 0};
    Check(s.index == 37 && s.name_hash == NameHashNoCase("pml1") && s.borrowed && !own.borrowed && own.index == 5 && a[3] == 0 && a[7] == 0 && !UnpackAnimSet(noname).borrowed,
          "kEventAnimSet: index, name hash and borrowed pack and back (unused words 0; no name: not borrowed)");
    VoiceMsg v; v.situation = 57; v.persona = 12; v.persona_hash = NameHashNoCase("BM_VK1"); v.sound_set = -1; v.extra = -1; v.distance_class = 2; v.used_set = 1234;
    PackVoice(v, a);
    VoiceMsg u;
    const bool ok = UnpackVoice(a, u);
    Check(ok && u.situation == 57 && u.persona == 12 && u.persona_hash == v.persona_hash && u.sound_set == -1 && u.extra == -1 && u.distance_class == 2 && u.used_set == 1234 && a[7] == 0,
          "kEventVoice: situation, persona index / hash, r5-r7 and the sound set used pack and back");
    uint32_t bad[8]; std::memcpy(bad, a, sizeof(a)); bad[0] = kVoiceSituationMax + 1;
    uint32_t nop[8]; std::memcpy(nop, a, sizeof(a)); nop[5] = 0;
    uint32_t addr[8]; std::memcpy(addr, a, sizeof(a)); addr[3] = 0x82B3E420u; addr[4] = 0x40000000u;
    VoiceMsg w;
    const bool bad_ok = UnpackVoice(bad, w), nop_ok = UnpackVoice(nop, w), addr_ok = UnpackVoice(addr, w);
    Check(!bad_ok && !nop_ok && addr_ok && w.extra == -1 && w.distance_class == -1 && w.situation == 57,
          "kEventVoice: a situation past 206 or no persona name refused; an address in r5 / r7 goes as -1");
    VoiceGate g(4, 300);
    int passed = 0;
    for (uint64_t t = 0; t < 1000; t += 100) passed += g.Allow(uint32_t(t / 100), 10000 + t);   // (10 different lines in a second)
    const bool rep_a = g.Allow(70, 11000), rep_b = g.Allow(70, 11100), rep_c = g.Allow(70, 11400);
    Check(passed == 4 && g.dropped_rate == 6 && rep_a && !rep_b && rep_c && g.dropped_repeat == 1,
          "voice gate: 4 lines a second, the same situation again within 300 ms dropped, after it allowed");
    VoiceGate rx(6, 0);
    int rx_passed = 0;
    for (int i = 0; i < 10; ++i) rx_passed += rx.Allow(70, 5000 + uint64_t(i));
    Check(rx_passed == 6 && rx.dropped_repeat == 0, "voice gate (receiver): 6 a second, repeats allowed");
    const float p0[3] = {0, 0, 0}, p1[3] = {100, 50, 100}, p2[3] = {0, 0, 171}, pn[3] = {NAN, 0, 0};
    Check(VoiceInRange(p0, p1) && !VoiceInRange(p0, p2) && VoiceInRange(p0, pn), "voice range: 2D within 170 m (the relay's second tier); unknown position: sent");

#pragma pack(push, 1)
    struct EventPacket { char magic[4]; uint32_t sequence; uint32_t kind, a[8]; float f; };   // (coop.cpp's kMsgEvent body)
#pragma pack(pop)
    static_assert(sizeof(EventPacket) == 48);
    constexpr uint8_t kMsgEventTest = 120;
    Endpoint* h = make(64 + 13);   // (the port's low 6 bits index the network's per-endpoint switches: 13, 56, 57 are test 19's, stopped and clean)
    h->s->StartHost("voice");
    std::vector<Endpoint*> in;
    for (int i : {64 + 56, 64 + 57}) { in.push_back(make(i)); in.back()->s->StartClient(h->addr, "voice" + std::to_string(i)); }
    step(3000);
    Check(in[0]->s->Joined() && in[1]->s->Joined(), "protocol 13 events: a host and 2 clients");
    auto event = [](uint32_t kind, const uint32_t* args, uint32_t seq) {
      EventPacket ep{}; std::memcpy(ep.magic, "WEV1", 4); ep.sequence = seq; ep.kind = kind; std::memcpy(ep.a, args, sizeof(ep.a));
      return ep;
    };
    uint32_t set_args[8]; PackAnimSet(AnimSetMsg{37, NameHashNoCase("PML1"), true}, set_args);
    const uint32_t action_args[8] = {0x10F, 0, 1, 0, 0, 0, 0, 0};
    const EventPacket e1 = event(52, set_args, 1), e2 = event(1, action_args, 2), e3 = event(51, a, 3);
    in[0]->s->Send(coopnet::kEveryone, kMsgEventTest, &e1, sizeof(e1), true);
    in[0]->s->Send(coopnet::kEveryone, kMsgEventTest, &e2, sizeof(e2), true);
    in[0]->s->Send(h->s->MySlot(), kMsgEventTest, &e3, sizeof(e3), true);   // (the voice only to the host: the other client is out of range)
    step(3000);
    std::vector<EventPacket> got_h, got_c;
    for (auto [ep, got] : {std::pair{h, &got_h}, std::pair{in[1], &got_c}})
      for (const auto& r : ep->got) if (r.type == kMsgEventTest && r.data.size() == sizeof(EventPacket)) { EventPacket p; std::memcpy(&p, r.data.data(), sizeof(p)); got->push_back(p); }
    const bool order_c = got_c.size() == 2 && got_c[0].kind == 52 && got_c[1].kind == 1 && UnpackAnimSet(got_c[0].a).borrowed && UnpackAnimSet(got_c[0].a).name_hash == NameHashNoCase("pml1");
    VoiceMsg hv;
    const bool voice_h = got_h.size() == 3 && got_h[0].kind == 52 && got_h[1].kind == 1 && got_h[2].kind == 51 && UnpackVoice(got_h[2].a, hv) && hv.situation == 57 && hv.persona == 12 && hv.distance_class == 2;
    Check(order_c && voice_h, "protocol 13 events through the session: the set before its action (everyone), the line to its target only, decoded the same");
    for (auto* e : {h, in[0], in[1]}) e->s->Stop(net.now);
    step(500);
  }

  const coopnet::Stats hs = host->s->TakeStats();
  std::printf("      host totals: %u packets out, %u in, %u resent, %u unreliable batches dropped\n", hs.packets_out, hs.packets_in, hs.resent, hs.dropped_unreliable);
  std::printf("%s: %d failure(s)\n", failures ? "FAILED" : "ALL PASSED", failures);
  return failures ? 1 : 0;
}
