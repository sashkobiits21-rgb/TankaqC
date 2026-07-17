#include "net/Net.h"
#include "Log.h"
#include <algorithm>
#include <chrono>
#include <steam/steam_api.h>
#include <steam/isteamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>

namespace
{
double NowSeconds()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}
constexpr double PingIntervalSec = 4.0 / 64.0;   // every 4 server ticks
}

namespace tankaq::net
{

static Net* g_net = nullptr;

// Steam delivers connection status through the SteamAPI callback dispatcher
// (SteamAPI_RunCallbacks), so register with a CCallback.
class StatusListener
{
public:
    StatusListener() : m_cb(this, &StatusListener::OnChanged) {}
private:
    void OnChanged(SteamNetConnectionStatusChangedCallback_t* p)
    {
        if (g_net)
            g_net->HandleStatusChanged(p);
    }
    CCallback<StatusListener, SteamNetConnectionStatusChangedCallback_t, false> m_cb;
};
static StatusListener* g_statusListener = nullptr;

// Quick-match plumbing. CCallResult members need Steam types, so the state
// lives here in the .cpp (same pattern as StatusListener) instead of Net.h.
// The public lobby is a directory advert only: data = { game, ver, host,
// open, players, phase }. Searchers read "host" straight off the search
// results (lobby data comes with them) and never JoinLobby.
class LobbyWork
{
public:
    uint64_t lobbyId = 0;
    bool searching = false;
    int wantNeed = 0;      // searcher's chosen queue size
    int advertNeed = 0;    // our own lobby's queue size (0 = open host)
    int pass = 0;          // 1 = queues of wantNeed, 2 = open hosts (need==0)

    int wantMode = 0;
    int advertMode = 0;

    void Search(int need, int mode)
    {
        wantNeed = need;
        wantMode = mode;
        pass = 1;
        searching = true;
        Log("Net: quick match - pass 1, %d-player %s queues", need,
            mode ? "TEST" : "normal");
        Request(need);
    }

    void Create(int need, int mode)
    {
        advertNeed = need;
        advertMode = mode;
        m_createResult.Set(
            SteamMatchmaking()->CreateLobby(k_ELobbyTypePublic, MaxLobbyPlayers),
            this, &LobbyWork::OnCreated);
    }

private:
    void Request(int need)
    {
        ISteamMatchmaking* mm = SteamMatchmaking();
        mm->AddRequestLobbyListStringFilter("game", "tankaq",
                                            k_ELobbyComparisonEqual);
        mm->AddRequestLobbyListNumericalFilter("ver", ProtocolVersion,
                                               k_ELobbyComparisonEqual);
        mm->AddRequestLobbyListNumericalFilter("open", 1,
                                               k_ELobbyComparisonEqual);
        mm->AddRequestLobbyListNumericalFilter("need", need,
                                               k_ELobbyComparisonEqual);
        mm->AddRequestLobbyListNumericalFilter("mode", wantMode,
                                               k_ELobbyComparisonEqual);
        // Russia <-> America must see each other: no distance cutoff
        mm->AddRequestLobbyListDistanceFilter(k_ELobbyDistanceFilterWorldwide);
        m_listResult.Set(mm->RequestLobbyList(), this, &LobbyWork::OnList);
    }

    void OnList(LobbyMatchList_t* r, bool ioFailure);
    void OnCreated(LobbyCreated_t* r, bool ioFailure);
    CCallResult<LobbyWork, LobbyMatchList_t> m_listResult;
    CCallResult<LobbyWork, LobbyCreated_t> m_createResult;
};
static LobbyWork* g_lobbyWork = nullptr;

void LobbyWork::OnList(LobbyMatchList_t* r, bool ioFailure)
{
    int n = ioFailure ? 0 : int(r->m_nLobbiesMatching);
    Log("Net: lobby search pass %d finished: %d result(s)", pass, n);
    ISteamMatchmaking* mm = SteamMatchmaking();
    for (int i = 0; i < n; ++i)
    {
        CSteamID lobby = mm->GetLobbyByIndex(i);
        uint64_t host = strtoull(mm->GetLobbyData(lobby, "host"), nullptr, 10);
        if (host != 0)
        {
            searching = false;
            Log("Net: match found, host %llu", (unsigned long long)host);
            if (g_net && g_net->onMatchFound)
                g_net->onMatchFound(host);
            return;
        }
    }
    if (pass == 1)
    {
        // no queue of that size: fall back to open-hosted games (need == 0),
        // which accept players regardless of the searcher's chosen size
        pass = 2;
        Log("Net: quick match - pass 2, open hosts");
        Request(0);
        return;
    }
    searching = false;
    if (g_net && g_net->onNoMatch)
        g_net->onNoMatch();
}

void LobbyWork::OnCreated(LobbyCreated_t* r, bool ioFailure)
{
    if (ioFailure || r->m_eResult != k_EResultOK)
    {
        Log("Net: public lobby creation failed (%d)",
            ioFailure ? -1 : int(r->m_eResult));
        return;
    }
    lobbyId = r->m_ulSteamIDLobby;
    ISteamMatchmaking* mm = SteamMatchmaking();
    CSteamID lobby(lobbyId);
    char buf[32];
    mm->SetLobbyData(lobby, "game", "tankaq");
    sprintf_s(buf, "%u", unsigned(ProtocolVersion));
    mm->SetLobbyData(lobby, "ver", buf);
    sprintf_s(buf, "%llu", (unsigned long long)(g_net ? g_net->mySteamId() : 0));
    mm->SetLobbyData(lobby, "host", buf);
    sprintf_s(buf, "%d", advertNeed);
    mm->SetLobbyData(lobby, "need", buf);
    mm->SetLobbyData(lobby, "open", "1");
    mm->SetLobbyData(lobby, "players", "1");
    mm->SetLobbyData(lobby, "phase", "0");
    sprintf_s(buf, "%d", advertMode);
    mm->SetLobbyData(lobby, "mode", buf);
    Log("Net: public lobby advertised (%llu, need=%d)",
        (unsigned long long)lobbyId, advertNeed);
}

static void SteamDebugOut(ESteamNetworkingSocketsDebugOutputType type, const char* msg)
{
    Log("SteamNet[%d]: %s", int(type), msg);
}

bool Net::InitSteam()
{
    if (m_steamOk)
        return true;
    if (!SteamAPI_Init())
    {
        Log("Net: SteamAPI_Init failed (is Steam running? steam_appid.txt present?)");
        return false;
    }
    m_steamOk = true;
    m_mySteamId = SteamUser()->GetSteamID().ConvertToUint64();
    m_myName = SteamFriends()->GetPersonaName();
    SteamNetworkingUtils()->SetDebugOutputFunction(
        k_ESteamNetworkingSocketsDebugOutputType_Important, SteamDebugOut);
    SteamNetworkingUtils()->InitRelayNetworkAccess();
    g_net = this;
    if (!g_statusListener)
        g_statusListener = new StatusListener();
    if (!g_lobbyWork)
        g_lobbyWork = new LobbyWork();
    Log("Net: Steam up as '%s' (%llu)", m_myName.c_str(),
        (unsigned long long)m_mySteamId);
    return true;
}

int Net::relayStatus(std::string& detail) const
{
    if (!m_steamOk)
    {
        detail = "steam offline";
        return -1;
    }
    SteamRelayNetworkStatus_t s{};
    SteamNetworkingUtils()->GetRelayNetworkStatus(&s);
    detail = s.m_debugMsg;
    return int(s.m_eAvail);   // k_ESteamNetworkingAvailability_Current == 100
}

void Net::Shutdown()
{
    Disconnect();
    delete g_lobbyWork;
    g_lobbyWork = nullptr;
    delete g_statusListener;
    g_statusListener = nullptr;
    if (m_steamOk)
    {
        SteamAPI_Shutdown();
        m_steamOk = false;
    }
    g_net = nullptr;
}

bool Net::StartHost(uint16_t udpPort, std::string& error)
{
    if (!m_steamOk) { error = "Steam is not available"; return false; }
    Disconnect();
    m_joinCap = MaxLobbyPlayers;   // queues lower this via SetJoinCap

    ISteamNetworkingSockets* s = SteamNetworkingSockets();

    m_listenP2P = s->CreateListenSocketP2P(0, 0, nullptr);

    SteamNetworkingIPAddr addr{};
    addr.Clear();
    addr.m_port = udpPort;
    SteamNetworkingConfigValue_t optsIP[1]; int nIP = 0;
    optsIP[nIP].SetInt32(k_ESteamNetworkingConfig_IP_AllowWithoutAuth, 1); ++nIP;
    m_listenIP = s->CreateListenSocketIP(addr, nIP, optsIP);

    m_pollGroup = s->CreatePollGroup();
    if (m_listenP2P == k_HSteamListenSocket_Invalid &&
        m_listenIP == k_HSteamListenSocket_Invalid)
    {
        error = "failed to create listen sockets";
        return false;
    }
    m_mode = Mode::Host;
    for (bool& b : m_usedPlayerIds) b = false;
    m_usedPlayerIds[0] = true;   // host is always player 0
    Log("Net: hosting (P2P %s, UDP port %u). Game code: %llu",
        m_listenP2P != k_HSteamListenSocket_Invalid ? "on" : "OFF",
        udpPort, (unsigned long long)m_mySteamId);
    return true;
}

bool Net::ConnectToCode(uint64_t hostSteamId, std::string& error)
{
    if (!m_steamOk) { error = "Steam is not available"; return false; }
    Disconnect();
    SteamNetworkingIdentity id;
    id.Clear();
    id.SetSteamID64(hostSteamId);
    // Relay warm-up can take a while on first launch; give rendezvous extra time.
    SteamNetworkingConfigValue_t opts[1]; int n = 0;
    opts[n].SetInt32(k_ESteamNetworkingConfig_TimeoutInitial, 25000); ++n;
    m_hostConn = SteamNetworkingSockets()->ConnectP2P(id, 0, n, opts);
    if (m_hostConn == k_HSteamNetConnection_Invalid)
    {
        error = "ConnectP2P failed";
        return false;
    }
    m_mode = Mode::Client;
    m_clientState = ClientState::Connecting;
    Log("Net: connecting to code %llu", (unsigned long long)hostSteamId);
    return true;
}

bool Net::ConnectToIP(const std::string& addrWithPort, std::string& error)
{
    if (!m_steamOk) { error = "Steam is not available"; return false; }
    Disconnect();
    SteamNetworkingIPAddr addr{};
    std::string full = addrWithPort;
    if (full.find(':') == std::string::npos)
        full += ":" + std::to_string(DefaultPort);
    if (!addr.ParseString(full.c_str()))
    {
        error = "could not parse address " + full;
        return false;
    }
    SteamNetworkingConfigValue_t opts[1]; int n = 0;
    opts[n].SetInt32(k_ESteamNetworkingConfig_IP_AllowWithoutAuth, 1); ++n;
    m_hostConn = SteamNetworkingSockets()->ConnectByIPAddress(addr, n, opts);
    if (m_hostConn == k_HSteamNetConnection_Invalid)
    {
        error = "ConnectByIPAddress failed";
        return false;
    }
    m_mode = Mode::Client;
    m_clientState = ClientState::Connecting;
    Log("Net: connecting to %s", full.c_str());
    return true;
}

void Net::Disconnect()
{
    if (!m_steamOk)
        return;
    LeaveLobby();   // stop advertising a game that no longer exists
    ISteamNetworkingSockets* s = SteamNetworkingSockets();
    for (Client& c : m_clients)
        s->CloseConnection(c.conn, 0, "host shutting down", false);
    m_clients.clear();
    if (m_hostConn) { s->CloseConnection(m_hostConn, 0, "leaving", false); m_hostConn = 0; }
    if (m_listenP2P) { s->CloseListenSocket(m_listenP2P); m_listenP2P = 0; }
    if (m_listenIP) { s->CloseListenSocket(m_listenIP); m_listenIP = 0; }
    if (m_pollGroup) { s->DestroyPollGroup(m_pollGroup); m_pollGroup = 0; }
    m_mode = Mode::Offline;
    m_clientState = ClientState::Idle;
}

void Net::QuickMatch(int need, int testMode)
{
    if (!m_steamOk || !g_lobbyWork || g_lobbyWork->searching)
    {
        if (onNoMatch)
            onNoMatch();
        return;
    }
    g_lobbyWork->Search(need, testMode);
}

bool Net::CreatePublicLobby(int need, int testMode)
{
    if (!m_steamOk || !g_lobbyWork || g_lobbyWork->lobbyId)
        return false;
    g_lobbyWork->Create(need, testMode);
    return true;
}

void Net::UpdateLobbyAdvert(int players, int phase)
{
    if (!m_steamOk || !g_lobbyWork || !g_lobbyWork->lobbyId)
        return;
    ISteamMatchmaking* mm = SteamMatchmaking();
    CSteamID lobby(g_lobbyWork->lobbyId);
    char buf[16];
    int cap = g_lobbyWork->advertNeed > 0 ? g_lobbyWork->advertNeed
                                          : MaxLobbyPlayers;
    mm->SetLobbyData(lobby, "open", players < cap ? "1" : "0");
    sprintf_s(buf, "%d", players);
    mm->SetLobbyData(lobby, "players", buf);
    sprintf_s(buf, "%d", phase);
    mm->SetLobbyData(lobby, "phase", buf);
}

void Net::SetJoinCap(int cap)
{
    m_joinCap = std::clamp(cap, 1, int(MaxLobbyPlayers));
}

void Net::LeaveLobby()
{
    if (!m_steamOk || !g_lobbyWork || !g_lobbyWork->lobbyId)
        return;
    SteamMatchmaking()->LeaveLobby(CSteamID(g_lobbyWork->lobbyId));
    g_lobbyWork->lobbyId = 0;
    Log("Net: public lobby closed");
}

bool Net::hasPublicLobby() const
{
    return g_lobbyWork && g_lobbyWork->lobbyId != 0;
}

void Net::SendPingOn(uint32_t conn, uint32_t& seqCounter, PingProbe* probes)
{
    MsgPing ping;
    ping.seq = ++seqCounter;
    probes[ping.seq % 8] = { ping.seq, NowSeconds() };
    SteamNetworkingSockets()->SendMessageToConnection(
        conn, &ping, sizeof(ping), k_nSteamNetworkingSend_UnreliableNoNagle, nullptr);
}

void Net::NotePong(uint32_t seq, const PingProbe* probes, float& avgOneWayMs)
{
    const PingProbe& probe = probes[seq % 8];
    if (probe.seq != seq)
        return;   // too old, overwritten
    float oneWay = float((NowSeconds() - probe.sentAt) * 1000.0 * 0.5);
    avgOneWayMs = (avgOneWayMs < 0.0f) ? oneWay
                                       : avgOneWayMs * 0.8f + oneWay * 0.2f;
}

float Net::avgOneWayMs(int playerId) const
{
    for (const Client& c : m_clients)
        if (c.playerId == playerId)
            return c.avgOneWayMs;
    return -1.0f;
}

float Net::hostAvgOneWayMs() const
{
    return m_hostAvgOneWayMs;
}

bool Net::clientConnectionStatus(int& pingMs, std::string& desc) const
{
    if (!m_steamOk || m_mode != Mode::Client || !m_hostConn
        || m_clientState != ClientState::Connected)
        return false;
    ISteamNetworkingSockets* s = SteamNetworkingSockets();
    SteamNetConnectionRealTimeStatus_t rt{};
    if (s->GetConnectionRealTimeStatus(m_hostConn, &rt, 0, nullptr) != k_EResultOK)
        return false;
    pingMs = rt.m_nPing;
    SteamNetConnectionInfo_t info{};
    if (s->GetConnectionInfo(m_hostConn, &info))
        desc = info.m_szConnectionDescription;
    return true;
}

std::string Net::joinCode() const
{
    return std::to_string(m_mySteamId);
}

int Net::connectedClients() const
{
    int n = 0;
    for (const Client& c : m_clients)
        if (c.playerId >= 0) ++n;
    return n;
}

int Net::AllocatePlayerId() const
{
    // up to 4 players; a gathering queue caps at its chosen size
    for (int i = 1; i < std::min(m_joinCap, int(MaxLobbyPlayers)); ++i)
        if (!m_usedPlayerIds[i])
            return i;
    return -1;
}

void Net::DropClient(uint32_t conn, const Events& ev)
{
    for (size_t i = 0; i < m_clients.size(); ++i)
    {
        if (m_clients[i].conn != conn)
            continue;
        int pid = m_clients[i].playerId;
        if (pid >= 0)
        {
            m_usedPlayerIds[pid] = false;
            if (ev.onPlayerLeft)
                ev.onPlayerLeft(pid);
        }
        m_clients.erase(m_clients.begin() + i);
        return;
    }
}

void Net::HandleStatusChanged(SteamNetConnectionStatusChangedCallback_t* info)
{
    ISteamNetworkingSockets* s = SteamNetworkingSockets();
    const auto state = info->m_info.m_eState;

    if (m_mode == Mode::Host)
    {
        if (state == k_ESteamNetworkingConnectionState_Connecting)
        {
            if (s->AcceptConnection(info->m_hConn) == k_EResultOK)
            {
                s->SetConnectionPollGroup(info->m_hConn, m_pollGroup);
                m_clients.push_back({ info->m_hConn, -1, false });
                Log("Net: incoming connection accepted");
            }
            else
            {
                s->CloseConnection(info->m_hConn, 0, "accept failed", false);
            }
        }
        else if (state == k_ESteamNetworkingConnectionState_ClosedByPeer ||
                 state == k_ESteamNetworkingConnectionState_ProblemDetectedLocally)
        {
            if (m_events)
                DropClient(info->m_hConn, *m_events);
            s->CloseConnection(info->m_hConn, 0, nullptr, false);
            Log("Net: client connection closed (%s)", info->m_info.m_szEndDebug);
        }
    }
    else if (m_mode == Mode::Client && info->m_hConn == m_hostConn)
    {
        if (state == k_ESteamNetworkingConnectionState_Connected)
        {
            MsgHello hello;
            strncpy_s(hello.name, m_myName.c_str(), _TRUNCATE);
            s->SendMessageToConnection(m_hostConn, &hello, sizeof(hello),
                                       k_nSteamNetworkingSend_Reliable, nullptr);
            Log("Net: connected to host, sent hello");
        }
        else if (state == k_ESteamNetworkingConnectionState_ClosedByPeer ||
                 state == k_ESteamNetworkingConnectionState_ProblemDetectedLocally)
        {
            char reason[192];
            snprintf(reason, sizeof(reason), "%s (code %d)",
                     info->m_info.m_szEndDebug[0] ? info->m_info.m_szEndDebug
                                                  : "connection closed",
                     info->m_info.m_eEndReason);
            m_disconnectReason = reason;
            s->CloseConnection(m_hostConn, 0, nullptr, false);
            m_hostConn = 0;
            m_clientState = ClientState::Failed;
            Log("Net: disconnected from host: %s", m_disconnectReason.c_str());
        }
    }
}

void Net::Poll(const Events& ev)
{
    if (!m_steamOk)
        return;
    m_events = &ev;
    SteamAPI_RunCallbacks();
    ISteamNetworkingSockets* s = SteamNetworkingSockets();

    SteamNetworkingMessage_t* msgs[32];

    if (m_mode == Mode::Host && m_pollGroup)
    {
        // latency probes to every connected player
        double now = NowSeconds();
        for (Client& c : m_clients)
            if (c.playerId >= 0 && now >= c.nextPingAt)
            {
                c.nextPingAt = now + PingIntervalSec;
                SendPingOn(c.conn, c.pingSeq, c.probes);
            }

        int n = s->ReceiveMessagesOnPollGroup(m_pollGroup, msgs, 32);
        for (int i = 0; i < n; ++i)
        {
            SteamNetworkingMessage_t* m = msgs[i];
            const uint8_t* d = static_cast<const uint8_t*>(m->m_pData);
            Client* client = nullptr;
            for (Client& c : m_clients)
                if (c.conn == m->m_conn) { client = &c; break; }

            if (client && m->m_cbSize >= 1)
            {
                MsgType t = MsgType(d[0]);
                if (t == MsgType::Ping && m->m_cbSize >= int(sizeof(MsgPing)))
                {
                    MsgPong pong;
                    pong.seq = reinterpret_cast<const MsgPing*>(d)->seq;
                    s->SendMessageToConnection(m->m_conn, &pong, sizeof(pong),
                                               k_nSteamNetworkingSend_UnreliableNoNagle,
                                               nullptr);
                }
                else if (t == MsgType::Pong && m->m_cbSize >= int(sizeof(MsgPong)))
                {
                    NotePong(reinterpret_cast<const MsgPong*>(d)->seq,
                             client->probes, client->avgOneWayMs);
                }
                if (t == MsgType::Hello && m->m_cbSize >= int(sizeof(MsgHello)))
                {
                    const MsgHello* hello = reinterpret_cast<const MsgHello*>(d);
                    if (hello->version != ProtocolVersion)
                    {
                        MsgReject rej; rej.reason = 1;
                        s->SendMessageToConnection(m->m_conn, &rej, sizeof(rej),
                                                   k_nSteamNetworkingSend_Reliable, nullptr);
                        s->CloseConnection(m->m_conn, 0, "version mismatch", true);
                    }
                    else if (!client->helloed)
                    {
                        int pid = AllocatePlayerId();
                        if (pid < 0)
                        {
                            MsgReject rej; rej.reason = 0;
                            s->SendMessageToConnection(m->m_conn, &rej, sizeof(rej),
                                                       k_nSteamNetworkingSend_Reliable, nullptr);
                            s->CloseConnection(m->m_conn, 0, "server full", true);
                        }
                        else
                        {
                            client->helloed = true;
                            client->playerId = pid;
                            m_usedPlayerIds[pid] = true;
                            MsgWelcome w; w.playerId = uint8_t(pid);
                            s->SendMessageToConnection(m->m_conn, &w, sizeof(w),
                                                       k_nSteamNetworkingSend_Reliable, nullptr);
                            if (ev.onPlayerJoined)
                                ev.onPlayerJoined(pid, hello->name);
                            Log("Net: player %d joined ('%s')", pid, hello->name);
                        }
                    }
                }
                else if (t == MsgType::Input && m->m_cbSize >= int(sizeof(MsgInput))
                         && client->playerId >= 0)
                {
                    if (ev.onInput)
                        ev.onInput(client->playerId,
                                   *reinterpret_cast<const MsgInput*>(d));
                }
                else if (t == MsgType::Purchase && m->m_cbSize >= int(sizeof(MsgPurchase))
                         && client->playerId >= 0)
                {
                    if (ev.onPurchase)
                        ev.onPurchase(client->playerId,
                                      reinterpret_cast<const MsgPurchase*>(d)->slot);
                }
                else if (t == MsgType::TestGrant
                         && m->m_cbSize >= int(sizeof(MsgTestGrant))
                         && client->playerId >= 0)
                {
                    if (ev.onTestGrant)
                        ev.onTestGrant(client->playerId,
                            reinterpret_cast<const MsgTestGrant*>(d)->upgrade);
                }
                else if (t == MsgType::Ready && m->m_cbSize >= int(sizeof(MsgReady))
                         && client->playerId >= 0)
                {
                    if (ev.onReady)
                        ev.onReady(client->playerId,
                                   reinterpret_cast<const MsgReady*>(d)->ready != 0);
                }
            }
            m->Release();
        }
    }
    else if (m_mode == Mode::Client && m_hostConn)
    {
        // latency probes to the host
        double now = NowSeconds();
        if (m_clientState == ClientState::Connected && now >= m_hostNextPingAt)
        {
            m_hostNextPingAt = now + PingIntervalSec;
            SendPingOn(m_hostConn, m_hostPingSeq, m_hostProbes);
        }

        int n = s->ReceiveMessagesOnConnection(m_hostConn, msgs, 32);
        for (int i = 0; i < n; ++i)
        {
            SteamNetworkingMessage_t* m = msgs[i];
            const uint8_t* d = static_cast<const uint8_t*>(m->m_pData);
            if (m->m_cbSize >= 1)
            {
                MsgType t = MsgType(d[0]);
                if (t == MsgType::Ping && m->m_cbSize >= int(sizeof(MsgPing)))
                {
                    MsgPong pong;
                    pong.seq = reinterpret_cast<const MsgPing*>(d)->seq;
                    s->SendMessageToConnection(m_hostConn, &pong, sizeof(pong),
                                               k_nSteamNetworkingSend_UnreliableNoNagle,
                                               nullptr);
                }
                else if (t == MsgType::Pong && m->m_cbSize >= int(sizeof(MsgPong)))
                {
                    NotePong(reinterpret_cast<const MsgPong*>(d)->seq,
                             m_hostProbes, m_hostAvgOneWayMs);
                }
                else if (t == MsgType::Welcome && m->m_cbSize >= int(sizeof(MsgWelcome)))
                {
                    m_clientState = ClientState::Connected;
                    if (ev.onWelcome)
                        ev.onWelcome(reinterpret_cast<const MsgWelcome*>(d)->playerId);
                }
                else if (t == MsgType::Reject && m->m_cbSize >= int(sizeof(MsgReject)))
                {
                    m_disconnectReason = reinterpret_cast<const MsgReject*>(d)->reason == 1
                        ? "version mismatch" : "server full";
                    m_clientState = ClientState::Failed;
                }
                else if (t == MsgType::Snapshot)
                {
                    static MsgSnapshot snap;   // 6 KB: keep off the stack
                    if (UnpackSnapshot(d, m->m_cbSize, snap) && ev.onSnapshot)
                        ev.onSnapshot(snap);
                }
                else if (t == MsgType::Upgrade && m->m_cbSize >= int(sizeof(MsgUpgrade)))
                {
                    const MsgUpgrade* u = reinterpret_cast<const MsgUpgrade*>(d);
                    if (ev.onUpgrade)
                        ev.onUpgrade(u->playerId, u->upgradeType);
                }
                else if (t == MsgType::OwnedReset && m->m_cbSize >= 1)
                {
                    if (ev.onOwnedReset)
                        ev.onOwnedReset();
                }
                else if (t == MsgType::OwnedSync && m->m_cbSize >= 4)
                {
                    const MsgOwnedSync* o = reinterpret_cast<const MsgOwnedSync*>(d);
                    int count = std::min<int>(o->count, kMaxOwnedSync);
                    if (m->m_cbSize >= 4 + count && ev.onOwnedSync)
                        ev.onOwnedSync(o->playerId, o->types, count);
                }
            }
            m->Release();
        }
        if (m_clientState == ClientState::Failed && ev.onDisconnected)
        {
            ev.onDisconnected(m_disconnectReason);
            m_clientState = ClientState::Idle;
            m_mode = Mode::Offline;
        }
    }
    m_events = nullptr;
}

void Net::SendInputToHost(const MsgInput& msg)
{
    if (m_mode != Mode::Client || !m_hostConn || m_clientState != ClientState::Connected)
        return;
    SteamNetworkingSockets()->SendMessageToConnection(
        m_hostConn, &msg, sizeof(msg), k_nSteamNetworkingSend_Unreliable, nullptr);
}

void Net::SendPurchaseToHost(int slot)
{
    if (m_mode != Mode::Client || !m_hostConn || m_clientState != ClientState::Connected)
        return;
    MsgPurchase msg;
    msg.slot = uint8_t(slot);
    SteamNetworkingSockets()->SendMessageToConnection(
        m_hostConn, &msg, sizeof(msg), k_nSteamNetworkingSend_Reliable, nullptr);
}

void Net::SendTestGrantToHost(uint8_t upgrade)
{
    if (m_mode != Mode::Client || !m_hostConn
        || m_clientState != ClientState::Connected)
        return;
    MsgTestGrant msg;
    msg.upgrade = upgrade;
    SteamNetworkingSockets()->SendMessageToConnection(
        m_hostConn, &msg, sizeof(msg), k_nSteamNetworkingSend_Reliable,
        nullptr);
}

void Net::BroadcastUpgrade(int playerId, uint8_t upgradeType)
{
    if (m_mode != Mode::Host)
        return;
    MsgUpgrade msg;
    msg.playerId = uint8_t(playerId);
    msg.upgradeType = upgradeType;
    ISteamNetworkingSockets* s = SteamNetworkingSockets();
    for (const Client& c : m_clients)
        if (c.playerId >= 0)
            s->SendMessageToConnection(c.conn, &msg, sizeof(msg),
                                       k_nSteamNetworkingSend_Reliable, nullptr);
}

void Net::BroadcastOwnedReset()
{
    if (m_mode != Mode::Host)
        return;
    MsgOwnedReset msg;
    ISteamNetworkingSockets* s = SteamNetworkingSockets();
    for (const Client& c : m_clients)
        if (c.playerId >= 0)
            s->SendMessageToConnection(c.conn, &msg, sizeof(msg),
                                       k_nSteamNetworkingSend_Reliable, nullptr);
}

void Net::SendOwnedSyncTo(int toPlayerId, int aboutPlayerId,
                          const uint8_t* types, size_t count)
{
    if (m_mode != Mode::Host)
        return;
    MsgOwnedSync msg;
    msg.playerId = uint8_t(aboutPlayerId);
    msg.count = uint16_t(std::min<size_t>(count, kMaxOwnedSync));
    memcpy(msg.types, types, msg.count);
    int bytes = 4 + int(msg.count);   // header + used entries only
    ISteamNetworkingSockets* s = SteamNetworkingSockets();
    for (const Client& c : m_clients)
        if (c.playerId == toPlayerId)
        {
            s->SendMessageToConnection(c.conn, &msg, uint32_t(bytes),
                                       k_nSteamNetworkingSend_Reliable, nullptr);
            return;
        }
}

void Net::SendReadyToHost(bool ready)
{
    if (m_mode != Mode::Client || !m_hostConn || m_clientState != ClientState::Connected)
        return;
    MsgReady msg;
    msg.ready = ready ? 1 : 0;
    SteamNetworkingSockets()->SendMessageToConnection(
        m_hostConn, &msg, sizeof(msg), k_nSteamNetworkingSend_Reliable, nullptr);
}

// ------------------------------------------------- packed snapshot wire
// Only LIVE entities travel; positions quantize to 1/128 units (arena is
// +-30), yaw to 1/10430 rad. Little-endian throughout (x64 only anyway).
namespace
{
    inline void W8(uint8_t*& p, uint8_t v)   { *p++ = v; }
    inline void W16(uint8_t*& p, uint16_t v) { memcpy(p, &v, 2); p += 2; }
    inline void W16s(uint8_t*& p, int16_t v) { memcpy(p, &v, 2); p += 2; }
    inline void W32(uint8_t*& p, uint32_t v) { memcpy(p, &v, 4); p += 4; }
    inline uint8_t R8(const uint8_t*& p)     { return *p++; }
    inline uint16_t R16(const uint8_t*& p)   { uint16_t v; memcpy(&v, p, 2); p += 2; return v; }
    inline int16_t R16s(const uint8_t*& p)   { int16_t v; memcpy(&v, p, 2); p += 2; return v; }
    inline uint32_t R32(const uint8_t*& p)   { uint32_t v; memcpy(&v, p, 4); p += 4; return v; }
    inline int16_t QPos(float v)   { return int16_t(std::clamp(v, -250.0f, 250.0f) * 128.0f); }
    inline float UQPos(int16_t v)  { return float(v) / 128.0f; }
    inline uint16_t QYaw(float v)  { return uint16_t(int(WrapAngle(v) * 10430.0f + 32768.5f)); }
    inline float UQYaw(uint16_t v) { return (float(v) - 32768.0f) / 10430.0f; }
}

int PackSnapshot(const MsgSnapshot& s, uint8_t* out)
{
    uint8_t* p = out;
    W8(p, s.type); W8(p, s.phase); W8(p, s.winner);
    W8(p, s.targetPlayers); W8(p, s.matchMinutes); W8(p, s.testMode);
    W32(p, s.tick); W32(p, s.matchEndTick);
    memcpy(p, s.players, sizeof(s.players)); p += sizeof(s.players);

    uint8_t* cnt = p; W8(p, 0);
    for (int i = 0; i < MaxProjectiles; ++i)
    {
        const ProjectileNet& q = s.projectiles[i];
        if (!q.active) continue;
        ++*cnt;
        W8(p, uint8_t(i));
        W16s(p, QPos(q.x)); W16s(p, QPos(q.z));
        W8(p, uint8_t(std::clamp(q.y, 0.0f, 3.9f) * 64.0f));
        W16(p, QYaw(q.yaw));
        W8(p, q.radar16); W8(p, q.radarRings);
        W8(p, q.lock255); W8(p, q.deflected);
    }
    cnt = p; W8(p, 0);
    for (int i = 0; i < MaxSoldiers; ++i)
    {
        const SoldierNet& q = s.soldiers[i];
        if (!q.active) continue;
        ++*cnt;
        W8(p, uint8_t(i)); W8(p, q.owner); W8(p, q.state);
        W8(p, q.targetId); W8(p, q.health); W8(p, q.flags);
        W16s(p, QPos(q.x)); W16s(p, QPos(q.z)); W16(p, QYaw(q.yaw));
    }
    cnt = p; W8(p, 0);
    for (int i = 0; i < MaxSkulls; ++i)
    {
        const SkullNet& q = s.skulls[i];
        if (!q.active) continue;
        ++*cnt;
        W8(p, uint8_t(i)); W8(p, q.owner);
        W16s(p, QPos(q.x)); W16s(p, QPos(q.z)); W16(p, QYaw(q.yaw));
    }
    cnt = p; W8(p, 0);
    for (int i = 0; i < MaxPuddles; ++i)
    {
        const PuddleNet& q = s.puddles[i];
        if (!q.active) continue;
        ++*cnt;
        W8(p, uint8_t(i)); W8(p, q.owner); W8(p, q.life16);
        W16s(p, QPos(q.x)); W16s(p, QPos(q.z));
    }
    cnt = p; W8(p, 0);
    for (int i = 0; i < MaxGhosts; ++i)
    {
        const GhostNet& q = s.ghosts[i];
        if (!q.active) continue;
        ++*cnt;
        W8(p, uint8_t(i)); W8(p, q.owner);
        W16s(p, QPos(q.x)); W16s(p, QPos(q.z));
    }
    cnt = p; W8(p, 0);
    for (int i = 0; i < MaxGrenades; ++i)
    {
        const GrenadeNet& q = s.grenades[i];
        if (!q.active) continue;
        ++*cnt;
        W8(p, uint8_t(i)); W8(p, q.owner); W8(p, q.fuse255);
        W16s(p, QPos(q.x)); W16s(p, QPos(q.z));
        W8(p, uint8_t(std::clamp(q.y, 0.0f, 7.9f) * 32.0f));
    }
    return int(p - out);
}

bool UnpackSnapshot(const uint8_t* data, int size, MsgSnapshot& out)
{
    constexpr int kHead = 6 + 8 + int(sizeof(out.players));
    if (size < kHead + 6)
        return false;
    const uint8_t* p = data;
    const uint8_t* end = data + size;
    out = MsgSnapshot{};
    out.type = R8(p); out.phase = R8(p); out.winner = R8(p);
    out.targetPlayers = R8(p); out.matchMinutes = R8(p);
    out.testMode = R8(p);
    out.tick = R32(p); out.matchEndTick = R32(p);
    memcpy(out.players, p, sizeof(out.players)); p += sizeof(out.players);

    auto need = [&](int n) { return end - p >= n; };
    if (!need(1)) return false;
    for (int n = R8(p); n-- > 0; )
    {
        if (!need(12)) return false;
        int slot = R8(p);
        ProjectileNet& q = out.projectiles[slot % MaxProjectiles];
        q.active = 1;
        q.x = UQPos(R16s(p)); q.z = UQPos(R16s(p));
        q.y = float(R8(p)) / 64.0f;
        q.yaw = UQYaw(R16(p));
        q.radar16 = R8(p); q.radarRings = R8(p);
        q.lock255 = R8(p); q.deflected = R8(p);
    }
    if (!need(1)) return false;
    for (int n = R8(p); n-- > 0; )
    {
        if (!need(12)) return false;
        int slot = R8(p);
        SoldierNet& q = out.soldiers[slot % MaxSoldiers];
        q.active = 1;
        q.owner = R8(p); q.state = R8(p); q.targetId = R8(p);
        q.health = R8(p); q.flags = R8(p);
        q.x = UQPos(R16s(p)); q.z = UQPos(R16s(p)); q.yaw = UQYaw(R16(p));
    }
    if (!need(1)) return false;
    for (int n = R8(p); n-- > 0; )
    {
        if (!need(8)) return false;
        int slot = R8(p);
        SkullNet& q = out.skulls[slot % MaxSkulls];
        q.active = 1;
        q.owner = R8(p);
        q.x = UQPos(R16s(p)); q.z = UQPos(R16s(p)); q.yaw = UQYaw(R16(p));
    }
    if (!need(1)) return false;
    for (int n = R8(p); n-- > 0; )
    {
        if (!need(7)) return false;
        int slot = R8(p);
        PuddleNet& q = out.puddles[slot % MaxPuddles];
        q.active = 1;
        q.owner = R8(p); q.life16 = R8(p);
        q.x = UQPos(R16s(p)); q.z = UQPos(R16s(p));
    }
    if (!need(1)) return false;
    for (int n = R8(p); n-- > 0; )
    {
        if (!need(6)) return false;
        int slot = R8(p);
        GhostNet& q = out.ghosts[slot % MaxGhosts];
        q.active = 1;
        q.owner = R8(p);
        q.x = UQPos(R16s(p)); q.z = UQPos(R16s(p));
    }
    if (!need(1)) return false;
    for (int n = R8(p); n-- > 0; )
    {
        if (!need(8)) return false;
        int slot = R8(p);
        GrenadeNet& q = out.grenades[slot % MaxGrenades];
        q.active = 1;
        q.owner = R8(p); q.fuse255 = R8(p);
        q.x = UQPos(R16s(p)); q.z = UQPos(R16s(p));
        q.y = float(R8(p)) / 32.0f;
    }
    return true;
}

void Net::BroadcastSnapshot(const MsgSnapshot& snap)
{
    if (m_mode != Mode::Host)
        return;
    uint8_t buf[sizeof(MsgSnapshot) + 64];
    int n = PackSnapshot(snap, buf);
    ISteamNetworkingSockets* s = SteamNetworkingSockets();
    for (const Client& c : m_clients)
        if (c.playerId >= 0)
            s->SendMessageToConnection(c.conn, buf, uint32_t(n),
                                       k_nSteamNetworkingSend_Unreliable, nullptr);
}

} // namespace tankaq::net
