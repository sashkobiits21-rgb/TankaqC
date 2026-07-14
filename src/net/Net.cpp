#include "net/Net.h"
#include "Log.h"
#include <steam/steam_api.h>
#include <steam/isteamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>

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
    for (int i = 1; i < MaxPlayers; ++i)
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
                                ev.onPlayerJoined(pid);
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
            }
            m->Release();
        }
    }
    else if (m_mode == Mode::Client && m_hostConn)
    {
        int n = s->ReceiveMessagesOnConnection(m_hostConn, msgs, 32);
        for (int i = 0; i < n; ++i)
        {
            SteamNetworkingMessage_t* m = msgs[i];
            const uint8_t* d = static_cast<const uint8_t*>(m->m_pData);
            if (m->m_cbSize >= 1)
            {
                MsgType t = MsgType(d[0]);
                if (t == MsgType::Welcome && m->m_cbSize >= int(sizeof(MsgWelcome)))
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
                else if (t == MsgType::Snapshot && m->m_cbSize >= int(sizeof(MsgSnapshot)))
                {
                    if (ev.onSnapshot)
                        ev.onSnapshot(*reinterpret_cast<const MsgSnapshot*>(d));
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

void Net::BroadcastSnapshot(const MsgSnapshot& snap)
{
    if (m_mode != Mode::Host)
        return;
    ISteamNetworkingSockets* s = SteamNetworkingSockets();
    for (const Client& c : m_clients)
        if (c.playerId >= 0)
            s->SendMessageToConnection(c.conn, &snap, sizeof(snap),
                                       k_nSteamNetworkingSend_Unreliable, nullptr);
}

} // namespace tankaq::net
