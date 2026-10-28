// Epic Online Services link for the co-op session: anonymous device-ID login,
// a lobby whose ID is the join code, and P2P packets (NAT traversal, Epic's
// relays as fallback). Used by NetSend / NetRecv in coop.cpp when a session
// is started online. Everything runs on the session's network thread.
//
// The EOS SDK DLL (EOSSDK-Win64-Shipping.dll) is loaded at run time from the
// mod folder; credentials come from eos.ini next to it ([eos] product,
// sandbox, deployment, client_id, client_secret). Without them the online
// options just report that EOS isn't set up.
#pragma once
#ifdef WHOMPAYS_EOS
#include "eos_sdk.h"
#include "eos_logging.h"
#include "eos_connect.h"
#include "eos_lobby.h"
#include "eos_p2p.h"
#include <atomic>
#include <cstring>

namespace eos {

// ---- SDK functions, loaded from the DLL ----
#define EOS_FUNCS(X) \
  X(EOS_Initialize) X(EOS_Platform_Create) X(EOS_Platform_Tick) X(EOS_Platform_GetConnectInterface) \
  X(EOS_Platform_GetLobbyInterface) X(EOS_Platform_GetP2PInterface) X(EOS_Logging_SetCallback) \
  X(EOS_Logging_SetLogLevel) X(EOS_EResult_ToString) X(EOS_ProductUserId_ToString) \
  X(EOS_Connect_CreateDeviceId) X(EOS_Connect_Login) X(EOS_Connect_CreateUser) \
  X(EOS_Lobby_CreateLobby) X(EOS_Lobby_JoinLobbyById) X(EOS_Lobby_LeaveLobby) X(EOS_Lobby_DestroyLobby) \
  X(EOS_Lobby_CopyLobbyDetailsHandle) X(EOS_LobbyDetails_GetLobbyOwner) X(EOS_LobbyDetails_Release) \
  X(EOS_Lobby_PromoteMember) X(EOS_Lobby_AddNotifyLobbyMemberStatusReceived) X(EOS_Lobby_RemoveNotifyLobbyMemberStatusReceived) \
  X(EOS_ProductUserId_FromString) \
  X(EOS_P2P_SendPacket) X(EOS_P2P_ReceivePacket) X(EOS_P2P_GetNextReceivedPacketSize) \
  X(EOS_P2P_AcceptConnection) X(EOS_P2P_AddNotifyPeerConnectionRequest) X(EOS_P2P_RemoveNotifyPeerConnectionRequest) \
  X(EOS_P2P_AddNotifyPeerConnectionEstablished) X(EOS_P2P_RemoveNotifyPeerConnectionEstablished) \
  X(EOS_P2P_SetPacketQueueSize) X(EOS_P2P_GetPacketQueueInfo) X(EOS_P2P_ClearPacketQueue) X(EOS_P2P_CloseConnection) \
  X(EOS_Platform_Release)
#define EOS_PTR(name) inline decltype(&::name) p_##name = nullptr;
EOS_FUNCS(EOS_PTR)
#undef EOS_PTR

enum class State { Off, Starting, LoggingIn, MakingLobby, JoiningLobby, Ready, Failed };
inline State state = State::Off;
inline bool loaded = false, platform_ok = false, is_host = false;
inline HMODULE dll = nullptr;
inline EOS_HPlatform platform = nullptr;
inline EOS_HConnect connect = nullptr;
inline EOS_HLobby lobby = nullptr;
inline EOS_HP2P p2p = nullptr;
inline EOS_ProductUserId me = nullptr;
inline EOS_NotificationId request_note = EOS_INVALID_NOTIFICATIONID;
inline EOS_NotificationId established_note = EOS_INVALID_NOTIFICATIONID;
inline EOS_NotificationId member_note = EOS_INVALID_NOTIFICATIONID;
// Host migration (co-op 3 phase 4, world_mode = shared): the lobby keeps going
// when its owner leaves (EOS picks a new owner). The session's own rule picks
// the session host (the lowest slot still there: net.h); the lobby's owner
// follows it: an owner that isn't the session host promotes it (Promote), so
// new players, who connect to the lobby's owner, find the session host.
inline bool migrate = false;
inline std::atomic<bool> owner{false};    // this game owns the lobby (made it, or EOS / a promotion made it the owner)
inline std::atomic<uint32_t> owner_changes{0};
inline EOS_P2P_SocketId socket_id{};
// How the link runs (1.71): 0 not yet known, 1 direct (internet or local
// network), 2 through Epic's relay servers. Packets the full outgoing queue
// refused. Both read by the Perf line.
inline std::atomic<int> link_kind{0};
inline std::atomic<uint32_t> send_refused{0};
// Outgoing queue cap (1.71): unlimited before, so whatever the link couldn't
// carry waited in line and everything arrived later and later (in the 1.69
// test the other player's shots 20-30 s late, their poses 2-3 s old). A full
// queue refuses new packets instead: a lost pose is better than a late one,
// and events are sent 3 times.
constexpr uint64_t kOutgoingQueueBytes = 64 * 1024;
inline std::string lobby_id, code, error;
inline std::mutex text_mutex;           // code / error / state text are read by the overlay
inline std::string status_text;
// Remote players: product user ID <-> stand-in address 10.255.0.N (N = index + 1).
inline std::vector<EOS_ProductUserId> peers;
inline std::vector<bool> peer_from_text;   // (migration) a handle made from the address list's text (FromString): the first handle Epic gives for that player replaces it
inline EOS_ProductUserId owner_id = nullptr;   // the lobby's owner (a joiner connects to it)
inline unsigned promote_logged = 0;
inline void (*log_fn)(const char*) = nullptr;

inline void Log(const std::string& s) { if (log_fn) log_fn(("EOS: " + s).c_str()); }
inline void SetText(const std::string& s) { std::lock_guard l(text_mutex); status_text = s; Log(s); }
inline std::string Text() { std::lock_guard l(text_mutex); return status_text; }
inline std::string Code() { std::lock_guard l(text_mutex); return code; }
inline const char* R(EOS_EResult r) { return p_EOS_EResult_ToString ? p_EOS_EResult_ToString(r) : "?"; }
inline void Fail(const std::string& why) { state = State::Failed; SetText("Online failed: " + why); }

inline sockaddr_in AddressOf(size_t index) {
  sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(1);
  a.sin_addr.s_addr = htonl((10u << 24) | (255u << 16) | uint32_t(index + 1));
  return a;
}
inline bool SameUser(EOS_ProductUserId a, EOS_ProductUserId b);
inline sockaddr_in AddressOf(EOS_ProductUserId id) {
  for (size_t i = 0; i < peers.size(); ++i) if (peers[i] == id) return AddressOf(i);
  // (Migration) Another handle of a player known from the address list: its
  // text compared once, then Epic's handle is the one kept (review_phase4
  // L13: not two ToString per peer on every packet).
  for (size_t i = 0; i < peers.size(); ++i)
    if (peer_from_text[i] && SameUser(peers[i], id)) { peers[i] = id; peer_from_text[i] = false; return AddressOf(i); }
  peers.push_back(id); peer_from_text.push_back(false);
  char s[EOS_PRODUCTUSERID_MAX_LENGTH + 1]{}; int32_t n = sizeof(s);
  if (p_EOS_ProductUserId_ToString) p_EOS_ProductUserId_ToString(id, s, &n);
  Log(std::string("remote player ") + s + " = 10.255.0." + std::to_string(peers.size()));
  return AddressOf(peers.size() - 1);
}
inline EOS_ProductUserId IdOf(const sockaddr_in& a) {
  const uint32_t ip = ntohl(a.sin_addr.s_addr);
  if ((ip >> 16) != ((10u << 8) | 255u)) return nullptr;
  const uint32_t n = ip & 0xFFFF;
  return n >= 1 && n <= peers.size() ? peers[n - 1] : nullptr;
}

inline std::string Ini(const std::string& file, const char* key) {
  char v[256]{};
  GetPrivateProfileStringA("eos", key, "", v, sizeof(v), file.c_str());
  std::string s(v);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\r')) s.pop_back();
  return s;
}

// The game (rexruntime: System Link over the internet, MULTIPLAYER > PLAYERS)
// runs its own EOS platform on its own thread; a second one next to it
// corrupts the heap. So co-op online borrows Epic from the game for the
// session (SrEosLend: the game leaves its lobby and releases its platform)
// and gives it back at the end (SrEosReturn).
inline bool lent = false;
inline FARPROC RuntimeFunction(const char* name) {
  HMODULE m = GetModuleHandleA("rexruntime.dll");
  return m ? GetProcAddress(m, name) : nullptr;
}
inline bool Borrow() {
  if (lent) return true;
  if (auto lend = reinterpret_cast<int (*)(int)>(RuntimeFunction("SrEosLend"))) {
    lent = true;
    if (!lend(5000)) {
      error = "the game is still using Epic - try again in a few seconds";
      return false;
    }
    Log("the game handed Epic over for co-op");
    return true;
  }
  // An older game without SrEosLend.
  if (!loaded && GetModuleHandleA("EOSSDK-Win64-Shipping.dll")) {
    error = "System Link online already uses Epic in this game run - restart the game to play co-op online";
    return false;
  }
  return true;
}
inline void GiveBack() {
  if (!lent) return;
  lent = false;
  if (auto give = reinterpret_cast<void (*)()>(RuntimeFunction("SrEosReturn"))) give();
  Log("Epic handed back to the game");
}

// Load the DLL (once per game run) and create the platform (per session).
inline bool Platform(const std::string& folder) {
  if (platform_ok) return true;
  if (!Borrow()) return false;
  if (!loaded) {
    dll = LoadLibraryA((folder + "\\eos\\EOSSDK-Win64-Shipping.dll").c_str());
    if (!dll) { error = "eos\\EOSSDK-Win64-Shipping.dll missing in the mod folder"; return false; }
#define EOS_LOAD(name) p_##name = reinterpret_cast<decltype(p_##name)>(GetProcAddress(dll, #name)); \
    if (!p_##name) { error = "EOS DLL has no " #name; return false; }
    EOS_FUNCS(EOS_LOAD)
#undef EOS_LOAD
    loaded = true;
    EOS_InitializeOptions init{};
    init.ApiVersion = EOS_INITIALIZE_API_LATEST;
    init.ProductName = "Saints Reborn";
    init.ProductVersion = "1.0";
    const EOS_EResult r = p_EOS_Initialize(&init);
    if (r != EOS_EResult::EOS_Success && r != EOS_EResult::EOS_AlreadyConfigured) { error = std::string("EOS_Initialize ") + R(r); return false; }
    p_EOS_Logging_SetCallback([](const EOS_LogMessage* m) {
      if (m && m->Message) Log(std::string("[") + (m->Category ? m->Category : "") + "] " + m->Message);
    });
    p_EOS_Logging_SetLogLevel(EOS_ELogCategory::EOS_LC_ALL_CATEGORIES, EOS_ELogLevel::EOS_LOG_Warning);
  }
  const std::string ini = folder + "\\eos.ini";
  const std::string product = Ini(ini, "product"), sandbox = Ini(ini, "sandbox"), deployment = Ini(ini, "deployment"),
                    client = Ini(ini, "client_id"), secret = Ini(ini, "client_secret");
  if (product.empty() || sandbox.empty() || deployment.empty() || client.empty() || secret.empty()) {
    error = "eos.ini (product, sandbox, deployment, client_id, client_secret) missing or incomplete";
    return false;
  }
  EOS_Platform_Options o{};
  o.ApiVersion = EOS_PLATFORM_OPTIONS_API_LATEST;
  o.ProductId = product.c_str(); o.SandboxId = sandbox.c_str(); o.DeploymentId = deployment.c_str();
  o.ClientCredentials.ClientId = client.c_str(); o.ClientCredentials.ClientSecret = secret.c_str();
  o.bIsServer = EOS_FALSE;
  o.Flags = EOS_PF_DISABLE_OVERLAY;
  platform = p_EOS_Platform_Create(&o);
  if (!platform) { error = "EOS_Platform_Create failed (check the IDs in eos.ini)"; return false; }
  connect = p_EOS_Platform_GetConnectInterface(platform);
  lobby = p_EOS_Platform_GetLobbyInterface(platform);
  p2p = p_EOS_Platform_GetP2PInterface(platform);
  {
    // Cap the outgoing queue (see kOutgoingQueueBytes); the incoming one keeps
    // Epic's size.
    EOS_P2P_GetPacketQueueInfoOptions qi{}; qi.ApiVersion = EOS_P2P_GETPACKETQUEUEINFO_API_LATEST;
    EOS_P2P_PacketQueueInfo now{};
    const bool known = p_EOS_P2P_GetPacketQueueInfo(p2p, &qi, &now) == EOS_EResult::EOS_Success;
    EOS_P2P_SetPacketQueueSizeOptions q{}; q.ApiVersion = EOS_P2P_SETPACKETQUEUESIZE_API_LATEST;
    q.IncomingPacketQueueMaxSizeBytes = known ? now.IncomingPacketQueueMaxSizeBytes : EOS_P2P_MAX_QUEUE_SIZE_UNLIMITED;
    q.OutgoingPacketQueueMaxSizeBytes = kOutgoingQueueBytes;
    const EOS_EResult r = p_EOS_P2P_SetPacketQueueSize(p2p, &q);
    Log("packet queues: outgoing capped at " + std::to_string(kOutgoingQueueBytes / 1024) + " KB (was " +
        (known ? (now.OutgoingPacketQueueMaxSizeBytes ? std::to_string(now.OutgoingPacketQueueMaxSizeBytes / 1024) + " KB" : std::string("unlimited")) : std::string("?")) +
        "), incoming " + (known ? (now.IncomingPacketQueueMaxSizeBytes ? std::to_string(now.IncomingPacketQueueMaxSizeBytes / 1024) + " KB" : std::string("unlimited")) : std::string("?")) +
        ": " + R(r));
  }
  std::snprintf(socket_id.SocketName, sizeof(socket_id.SocketName), "WHOMPAYSCOOP");
  socket_id.ApiVersion = EOS_P2P_SOCKETID_API_LATEST;
  platform_ok = true;
  return true;
}

inline std::string NewCode() {
  static const char k[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";  // no 0/O, 1/I
  std::string c;
  LARGE_INTEGER t; QueryPerformanceCounter(&t);
  uint64_t x = uint64_t(t.QuadPart) ^ (uint64_t(GetCurrentProcessId()) << 32) ^ GetTickCount64() * 0x9E3779B97F4A7C15ull;
  for (int i = 0; i < 6; ++i) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; c += k[x % 32]; }
  return c;
}
inline std::string LobbyIdFor(const std::string& c) { return "saintsreborn-" + c; }

inline void AcceptFrom(EOS_ProductUserId remote) {
  EOS_P2P_AcceptConnectionOptions a{};
  a.ApiVersion = EOS_P2P_ACCEPTCONNECTION_API_LATEST;
  a.LocalUserId = me; a.RemoteUserId = remote; a.SocketId = &socket_id;
  p_EOS_P2P_AcceptConnection(p2p, &a);
}

inline void Online();  // after login: make or join the lobby

// Two handles of one player (FromString makes a new handle each time).
inline bool SameUser(EOS_ProductUserId a, EOS_ProductUserId b) {
  if (a == b) return true;
  if (!a || !b || !p_EOS_ProductUserId_ToString) return false;
  char ta[EOS_PRODUCTUSERID_MAX_LENGTH + 1]{}, tb[EOS_PRODUCTUSERID_MAX_LENGTH + 1]{}; int32_t na = sizeof(ta), nb = sizeof(tb);
  return p_EOS_ProductUserId_ToString(a, ta, &na) == EOS_EResult::EOS_Success && p_EOS_ProductUserId_ToString(b, tb, &nb) == EOS_EResult::EOS_Success && !std::strcmp(ta, tb);
}
// (Migration) An address as text for the session's address list (kPeerAddr):
// the product user ID, which every game maps to a stand-in address of its own.
inline std::string AddressText(const sockaddr_in& a) {
  const EOS_ProductUserId id = IdOf(a);
  if (!id || !p_EOS_ProductUserId_ToString) return std::string();
  char t[EOS_PRODUCTUSERID_MAX_LENGTH + 1]{}; int32_t n = sizeof(t);
  return p_EOS_ProductUserId_ToString(id, t, &n) == EOS_EResult::EOS_Success ? std::string(t) : std::string();
}
inline bool AddressFrom(const std::string& text, sockaddr_in& out) {
  if (text.empty() || text.size() > EOS_PRODUCTUSERID_MAX_LENGTH || !p_EOS_ProductUserId_FromString) return false;
  const EOS_ProductUserId id = p_EOS_ProductUserId_FromString(text.c_str());
  if (!id || SameUser(id, me)) return false;
  for (size_t i = 0; i < peers.size(); ++i) if (SameUser(peers[i], id)) { out = AddressOf(i); return true; }   // (the handle already known)
  out = AddressOf(id);
  peer_from_text.back() = true;   // (a new stand-in: AddressOf added it last)
  return true;
}
// (Migration) This game owns the lobby but another game is the session host:
// the lobby goes to it.
inline void Promote(const sockaddr_in& session_host) {
  if (!owner.load() || !lobby || !me || lobby_id.empty()) return;
  const EOS_ProductUserId target = IdOf(session_host);
  if (!target || SameUser(target, me)) return;
  EOS_Lobby_PromoteMemberOptions o{};
  o.ApiVersion = EOS_LOBBY_PROMOTEMEMBER_API_LATEST; o.LobbyId = lobby_id.c_str(); o.LocalUserId = me; o.TargetUserId = target;
  p_EOS_Lobby_PromoteMember(lobby, &o, nullptr, [](const EOS_Lobby_PromoteMemberCallbackInfo* d) {
    if (promote_logged++ < 5) Log(std::string("the lobby given to the session host: ") + R(d->ResultCode));   // (review_phase4 L12: not every 10 s for the whole session)
  });
}
// The lobby's owner in this game's copy of the lobby (local, no Epic round
// trip). (Migration, review_phase4 L12 / M7) Read again every few seconds:
// `owner` follows it, and a joiner not in the session yet follows the owner.
inline EOS_ProductUserId LobbyOwner() {
  if (!lobby || !me || lobby_id.empty() || state != State::Ready) return nullptr;
  EOS_Lobby_CopyLobbyDetailsHandleOptions c{};
  c.ApiVersion = EOS_LOBBY_COPYLOBBYDETAILSHANDLE_API_LATEST; c.LobbyId = lobby_id.c_str(); c.LocalUserId = me;
  EOS_HLobbyDetails details = nullptr;
  if (p_EOS_Lobby_CopyLobbyDetailsHandle(lobby, &c, &details) != EOS_EResult::EOS_Success || !details) return nullptr;
  EOS_LobbyDetails_GetLobbyOwnerOptions o{}; o.ApiVersion = EOS_LOBBYDETAILS_GETLOBBYOWNER_API_LATEST;
  const EOS_ProductUserId id = p_EOS_LobbyDetails_GetLobbyOwner(details, &o);
  p_EOS_LobbyDetails_Release(details);
  return id;
}
inline void RefreshOwner() {
  const EOS_ProductUserId id = LobbyOwner();
  if (!id) return;
  const bool changed = !SameUser(id, owner_id);
  owner_id = id; owner = SameUser(id, me);
  if (changed) { ++owner_changes; Log(std::string("the lobby's owner is ") + (owner.load() ? "this game" : "another player") + " (read again)"); }
}

inline void OnLogin(const EOS_Connect_LoginCallbackInfo* d) {
  if (d->ResultCode == EOS_EResult::EOS_Success) { me = d->LocalUserId; Online(); return; }
  if (d->ResultCode == EOS_EResult::EOS_InvalidUser && d->ContinuanceToken) {
    EOS_Connect_CreateUserOptions c{};
    c.ApiVersion = EOS_CONNECT_CREATEUSER_API_LATEST; c.ContinuanceToken = d->ContinuanceToken;
    p_EOS_Connect_CreateUser(connect, &c, nullptr, [](const EOS_Connect_CreateUserCallbackInfo* u) {
      if (u->ResultCode != EOS_EResult::EOS_Success) { Fail(std::string("creating the player ID: ") + R(u->ResultCode)); return; }
      me = u->LocalUserId; Online();
    });
    return;
  }
  Fail(std::string("login: ") + R(d->ResultCode));
}

inline std::string display_name;
inline void Login() {
  state = State::LoggingIn; SetText("Logging in to Epic Online Services...");
  EOS_Connect_CreateDeviceIdOptions c{};
  c.ApiVersion = EOS_CONNECT_CREATEDEVICEID_API_LATEST; c.DeviceModel = "PC";
  p_EOS_Connect_CreateDeviceId(connect, &c, nullptr, [](const EOS_Connect_CreateDeviceIdCallbackInfo* d) {
    if (d->ResultCode != EOS_EResult::EOS_Success && d->ResultCode != EOS_EResult::EOS_DuplicateNotAllowed) {
      Fail(std::string("device ID: ") + R(d->ResultCode)); return;
    }
    static EOS_Connect_Credentials cred{};
    cred.ApiVersion = EOS_CONNECT_CREDENTIALS_API_LATEST; cred.Token = nullptr; cred.Type = EOS_EExternalCredentialType::EOS_ECT_DEVICEID_ACCESS_TOKEN;
    static EOS_Connect_UserLoginInfo info{};
    info.ApiVersion = EOS_CONNECT_USERLOGININFO_API_LATEST; info.DisplayName = display_name.c_str();
    EOS_Connect_LoginOptions l{};
    l.ApiVersion = EOS_CONNECT_LOGIN_API_LATEST; l.Credentials = &cred; l.UserLoginInfo = &info;
    p_EOS_Connect_Login(connect, &l, nullptr, [](const EOS_Connect_LoginCallbackInfo* r) { OnLogin(r); });
  });
}

inline std::string wanted_code;
inline uint32_t max_players = 2;

inline void FindHost() {
  EOS_Lobby_CopyLobbyDetailsHandleOptions c{};
  c.ApiVersion = EOS_LOBBY_COPYLOBBYDETAILSHANDLE_API_LATEST; c.LobbyId = lobby_id.c_str(); c.LocalUserId = me;
  EOS_HLobbyDetails details = nullptr;
  if (p_EOS_Lobby_CopyLobbyDetailsHandle(lobby, &c, &details) != EOS_EResult::EOS_Success || !details) { Fail("could not read the lobby"); return; }
  EOS_LobbyDetails_GetLobbyOwnerOptions o{}; o.ApiVersion = EOS_LOBBYDETAILS_GETLOBBYOWNER_API_LATEST;
  owner_id = p_EOS_LobbyDetails_GetLobbyOwner(details, &o);
  p_EOS_LobbyDetails_Release(details);
  if (!owner_id) { Fail("lobby has no host"); return; }
  owner = SameUser(owner_id, me);
  AddressOf(owner_id);  // the host is always 10.255.0.1 on a joiner
  AcceptFrom(owner_id);
  state = State::Ready; SetText("Joined " + Code() + ", connecting to the host...");
}

inline void Online() {
  // Incoming P2P connections are accepted from anyone in our lobby (for now: anyone with the code).
  EOS_P2P_AddNotifyPeerConnectionRequestOptions n{};
  n.ApiVersion = EOS_P2P_ADDNOTIFYPEERCONNECTIONREQUEST_API_LATEST; n.LocalUserId = me; n.SocketId = &socket_id;
  request_note = p_EOS_P2P_AddNotifyPeerConnectionRequest(p2p, &n, nullptr, [](const EOS_P2P_OnIncomingConnectionRequestInfo* i) {
    AcceptFrom(i->RemoteUserId);
    Log("accepted a connection");
  });
  // How each connection runs (1.71): directly, or through Epic's relay
  // servers (more delay; on a Mac without the Local Network permission the
  // direct path between two machines at home can't be used).
  EOS_P2P_AddNotifyPeerConnectionEstablishedOptions e{};
  e.ApiVersion = EOS_P2P_ADDNOTIFYPEERCONNECTIONESTABLISHED_API_LATEST; e.LocalUserId = me; e.SocketId = &socket_id;
  established_note = p_EOS_P2P_AddNotifyPeerConnectionEstablished(p2p, &e, nullptr, [](const EOS_P2P_OnPeerConnectionEstablishedInfo* i) {
    const bool direct = i->NetworkType == EOS_ENetworkConnectionType::EOS_NCT_DirectConnection;
    const bool relayed = i->NetworkType == EOS_ENetworkConnectionType::EOS_NCT_RelayedConnection;
    link_kind.store(direct ? 1 : relayed ? 2 : 0);
    Log(std::string(i->ConnectionType == EOS_EConnectionEstablishedType::EOS_CET_Reconnection ? "reconnected" : "connected") +
        (direct ? " directly" : relayed ? " through Epic's relay servers" : " (no connection type)"));
  });
  // (Migration) Who owns the lobby now: EOS picks a new owner when the old
  // one leaves, a promotion moves it.
  if (migrate) {
    EOS_Lobby_AddNotifyLobbyMemberStatusReceivedOptions m{}; m.ApiVersion = EOS_LOBBY_ADDNOTIFYLOBBYMEMBERSTATUSRECEIVED_API_LATEST;
    member_note = p_EOS_Lobby_AddNotifyLobbyMemberStatusReceived(lobby, &m, nullptr, [](const EOS_Lobby_LobbyMemberStatusReceivedCallbackInfo* i) {
      if (!i || i->CurrentStatus != EOS_ELobbyMemberStatus::EOS_LMS_PROMOTED || !i->LobbyId || lobby_id != i->LobbyId) return;
      owner_id = i->TargetUserId;
      owner = SameUser(i->TargetUserId, me);
      ++owner_changes;
      Log(std::string("the lobby's owner changed: ") + (owner.load() ? "this game" : "another player"));
    });
  }
  if (is_host) {
    state = State::MakingLobby; SetText("Creating the lobby...");
    { std::lock_guard l(text_mutex); code = NewCode(); }
    lobby_id = LobbyIdFor(Code());
    EOS_Lobby_CreateLobbyOptions c{};
    c.ApiVersion = EOS_LOBBY_CREATELOBBY_API_LATEST;
    c.LocalUserId = me; c.MaxLobbyMembers = max_players;
    c.PermissionLevel = EOS_ELobbyPermissionLevel::EOS_LPL_JOINVIAPRESENCE;
    c.bPresenceEnabled = EOS_FALSE; c.bAllowInvites = EOS_TRUE;
    c.BucketId = "saintsreborn:coop"; c.bDisableHostMigration = migrate ? EOS_FALSE : EOS_TRUE;   // (co-op 3 phase 4: on in world_mode = shared)
    c.LobbyId = lobby_id.c_str(); c.bEnableJoinById = EOS_TRUE;
    p_EOS_Lobby_CreateLobby(lobby, &c, nullptr, [](const EOS_Lobby_CreateLobbyCallbackInfo* d) {
      if (d->ResultCode != EOS_EResult::EOS_Success) { Fail(std::string("creating the lobby: ") + R(d->ResultCode)); return; }
      owner = true;
      state = State::Ready; SetText("Online. Join code: " + Code() + "  (waiting for a player)");
    });
  } else {
    state = State::JoiningLobby; SetText("Joining " + Code() + "...");
    lobby_id = LobbyIdFor(Code());
    EOS_Lobby_JoinLobbyByIdOptions j{};
    j.ApiVersion = EOS_LOBBY_JOINLOBBYBYID_API_LATEST;
    j.LobbyId = lobby_id.c_str(); j.LocalUserId = me; j.bPresenceEnabled = EOS_FALSE;
    p_EOS_Lobby_JoinLobbyById(lobby, &j, nullptr, [](const EOS_Lobby_JoinLobbyByIdCallbackInfo* d) {
      if (d->ResultCode != EOS_EResult::EOS_Success) {
        Fail(d->ResultCode == EOS_EResult::EOS_NotFound ? "no game with that code" : std::string("joining: ") + R(d->ResultCode));
        return;
      }
      FindHost();
    });
  }
}

// Start an online session (network thread). Returns false with `error` set.
inline bool Begin(const std::string& folder, bool host, const std::string& join_code, const std::string& name, uint32_t players, bool migration = false) {
  peers.clear(); peer_from_text.clear(); owner_id = nullptr; lobby_id.clear(); error.clear(); promote_logged = 0;
  is_host = host; max_players = players; migrate = migration; owner = false; owner_changes = 0; display_name = name.substr(0, EOS_CONNECT_USERLOGININFO_DISPLAYNAME_MAX_LENGTH);
  if (display_name.empty()) display_name = "Player";
  { std::lock_guard l(text_mutex); code = host ? "" : join_code; }
  state = State::Starting;
  if (!Platform(folder)) { GiveBack(); Fail(error); return false; }
  if (me) Online(); else Login();
  return true;
}

inline void Tick() { if (platform_ok) p_EOS_Platform_Tick(platform); }

// A player who is gone (the session dropped them, host): what is still queued
// for them goes and the connection closes. The send queue is shared by every
// player and capped (kOutgoingQueueBytes): a player who stopped answering
// filled it in the first 4-player test and the others' packets were refused
// (one of them dropped too). They can join again (AcceptFrom).
inline void Drop(const sockaddr_in& to) {
  if (!platform_ok || !p2p || !me) return;
  const EOS_ProductUserId remote = IdOf(to);
  if (!remote) return;
  EOS_P2P_ClearPacketQueueOptions q{};
  q.ApiVersion = EOS_P2P_CLEARPACKETQUEUE_API_LATEST; q.LocalUserId = me; q.RemoteUserId = remote; q.SocketId = &socket_id;
  const EOS_EResult rq = p_EOS_P2P_ClearPacketQueue(p2p, &q);
  EOS_P2P_CloseConnectionOptions c{};
  c.ApiVersion = EOS_P2P_CLOSECONNECTION_API_LATEST; c.LocalUserId = me; c.RemoteUserId = remote; c.SocketId = &socket_id;
  const EOS_EResult rc = p_EOS_P2P_CloseConnection(p2p, &c);
  Log(std::string("a player's connection closed (queue cleared: ") + R(rq) + ", closed: " + R(rc) + ")");
}
inline int Send(const void* data, int length, const sockaddr_in& to) {
  if (state != State::Ready || !me) return -1;
  EOS_ProductUserId remote = IdOf(to);
  if (!remote) return -1;
  EOS_P2P_SendPacketOptions s{};
  s.ApiVersion = EOS_P2P_SENDPACKET_API_LATEST;
  s.LocalUserId = me; s.RemoteUserId = remote; s.SocketId = &socket_id; s.Channel = 0;
  s.DataLengthBytes = uint32_t(length); s.Data = data;
  s.bAllowDelayedDelivery = EOS_TRUE;
  s.Reliability = EOS_EPacketReliability::EOS_PR_UnreliableUnordered;
  s.bDisableAutoAcceptConnection = EOS_FALSE;
  const EOS_EResult r = p_EOS_P2P_SendPacket(p2p, &s);
  if (r == EOS_EResult::EOS_LimitExceeded) ++send_refused;  // the capped queue is full
  return r == EOS_EResult::EOS_Success ? length : -1;
}

// Current queue sizes (network thread): what waits to be sent / to be read.
inline bool QueueInfo(EOS_P2P_PacketQueueInfo& out) {
  if (!platform_ok || !p2p) return false;
  EOS_P2P_GetPacketQueueInfoOptions o{}; o.ApiVersion = EOS_P2P_GETPACKETQUEUEINFO_API_LATEST;
  return p_EOS_P2P_GetPacketQueueInfo(p2p, &o, &out) == EOS_EResult::EOS_Success;
}

inline int Receive(char* buffer, int capacity, sockaddr_in& from) {
  Tick();
  if (!me || !p2p) return -1;
  EOS_P2P_ReceivePacketOptions r{};
  r.ApiVersion = EOS_P2P_RECEIVEPACKET_API_LATEST; r.LocalUserId = me; r.MaxDataSizeBytes = uint32_t(capacity); r.RequestedChannel = nullptr;
  EOS_ProductUserId peer = nullptr; EOS_P2P_SocketId sid{}; uint8_t channel = 0; uint32_t written = 0;
  if (p_EOS_P2P_ReceivePacket(p2p, &r, &peer, &sid, &channel, buffer, &written) != EOS_EResult::EOS_Success || !peer) return -1;
  from = AddressOf(peer);
  return int(written);
}

// Leave / close the lobby (network thread, at the end of the session).
inline void End() {
  if (!platform_ok) { GiveBack(); state = State::Off; return; }
  if (request_note != EOS_INVALID_NOTIFICATIONID) { p_EOS_P2P_RemoveNotifyPeerConnectionRequest(p2p, request_note); request_note = EOS_INVALID_NOTIFICATIONID; }
  if (established_note != EOS_INVALID_NOTIFICATIONID) { p_EOS_P2P_RemoveNotifyPeerConnectionEstablished(p2p, established_note); established_note = EOS_INVALID_NOTIFICATIONID; }
  if (member_note != EOS_INVALID_NOTIFICATIONID) { p_EOS_Lobby_RemoveNotifyLobbyMemberStatusReceived(lobby, member_note); member_note = EOS_INVALID_NOTIFICATIONID; }
  link_kind.store(0);
  if (me && !lobby_id.empty()) {
    if (is_host && !migrate) {   // (migration: the owner leaves too, and the lobby goes on with the others; it ends when the last one leaves)
      EOS_Lobby_DestroyLobbyOptions d{}; d.ApiVersion = EOS_LOBBY_DESTROYLOBBY_API_LATEST; d.LocalUserId = me; d.LobbyId = lobby_id.c_str();
      p_EOS_Lobby_DestroyLobby(lobby, &d, nullptr, [](const EOS_Lobby_DestroyLobbyCallbackInfo*) {});
    } else {
      EOS_Lobby_LeaveLobbyOptions d{}; d.ApiVersion = EOS_LOBBY_LEAVELOBBY_API_LATEST; d.LocalUserId = me; d.LobbyId = lobby_id.c_str();
      p_EOS_Lobby_LeaveLobby(lobby, &d, nullptr, [](const EOS_Lobby_LeaveLobbyCallbackInfo*) {});
    }
    for (int i = 0; i < 60; ++i) { Tick(); Sleep(5); }
  }
  lobby_id.clear(); peers.clear(); peer_from_text.clear(); owner_id = nullptr; owner = false;
  // Release the platform so the game can use Epic again (PLAYERS list,
  // System Link) without a restart; the next co-op session logs in again.
  p_EOS_Platform_Release(platform);
  platform = nullptr; connect = nullptr; lobby = nullptr; p2p = nullptr; me = nullptr;
  platform_ok = false;
  GiveBack();
  state = State::Off;
  { std::lock_guard l(text_mutex); code.clear(); status_text.clear(); }
}

}  // namespace eos
#endif
