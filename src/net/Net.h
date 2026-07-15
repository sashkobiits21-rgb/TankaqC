#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "net/Protocol.h"

// Steam-based networking: host listens on a P2P socket (friends join via the
// game code = host SteamID64, relayed by Steam) and on a plain UDP socket
// (LAN / same-machine testing via ip:port). Clients connect with either.

struct SteamNetConnectionStatusChangedCallback_t;

namespace tankaq::net
{

enum class Mode { Offline, Host, Client };
enum class ClientState { Idle, Connecting, Connected, Failed };

class Net
{
public:
    struct Events
    {
        // host side
        std::function<void(int playerId, const char* name)> onPlayerJoined;
        std::function<void(int playerId)> onPlayerLeft;
        std::function<void(int playerId, const MsgInput&)> onInput;
        std::function<void(int playerId, int slot)> onPurchase;
        std::function<void(int playerId, bool ready)> onReady;
        // client side
        std::function<void(int myPlayerId)> onWelcome;
        std::function<void(const MsgSnapshot&)> onSnapshot;
        std::function<void(const std::string& reason)> onDisconnected;
    };

    bool InitSteam();                       // true if the Steam API came up
    void Shutdown();

    bool StartHost(uint16_t udpPort, std::string& error);
    bool ConnectToCode(uint64_t hostSteamId, std::string& error);
    bool ConnectToIP(const std::string& addrWithPort, std::string& error);
    void Disconnect();

    // ---- quick match (free Steam lobby directory; no dedicated servers) ----
    // A public Steam lobby is used purely as an advert: its data carries the
    // host's SteamID and the queue size ("need"). Searchers read that and
    // ConnectP2P as usual -- nobody ever joins the Steam lobby itself.
    // Async: one of the two callbacks below fires from inside Poll().
    // Search is two-pass: queues of the same size first (need == n), then
    // open-hosted games (need == 0, joinable at any size).
    void QuickMatch(int need);             // search worldwide, n players wanted
    bool CreatePublicLobby(int need);      // host: advertise (0 = open host)
    void UpdateLobbyAdvert(int players, int phase);
    void LeaveLobby();                     // stop advertising
    bool hasPublicLobby() const;
    void SetJoinCap(int cap);              // host: reject joins beyond this
    std::function<void(uint64_t hostSteamId)> onMatchFound;
    std::function<void()> onNoMatch;       // caller should host a public game

    void Poll(const Events& ev);            // pump callbacks + messages, once per frame

    void SendInputToHost(const MsgInput& msg);
    void SendPurchaseToHost(int slot);
    void SendReadyToHost(bool ready);
    void BroadcastSnapshot(const MsgSnapshot& snap);

    Mode mode() const { return m_mode; }
    ClientState clientState() const { return m_clientState; }
    bool steamAvailable() const { return m_steamOk; }
    // Steam Datagram Relay availability (100 = ready); detail gets the debug text.
    int relayStatus(std::string& detail) const;
    // Client-side live connection info: ping and Steam's route description
    // (shows whether the link is direct UDP or going through an SDR relay).
    bool clientConnectionStatus(int& pingMs, std::string& desc) const;
    // Averaged one-way latency from the ping/pong probes (ms; < 0 = unknown).
    float avgOneWayMs(int playerId) const;    // host: per connected player
    float hostAvgOneWayMs() const;            // client: to the host
    uint64_t mySteamId() const { return m_mySteamId; }
    std::string myName() const { return m_myName; }
    std::string joinCode() const;           // host's shareable code (SteamID64)
    int connectedClients() const;

    void HandleStatusChanged(SteamNetConnectionStatusChangedCallback_t* info);

private:
    struct PingProbe
    {
        uint32_t seq = 0;
        double sentAt = 0;
    };

    struct Client
    {
        uint32_t conn = 0;
        int playerId = -1;
        bool helloed = false;
        double nextPingAt = 0;
        uint32_t pingSeq = 0;
        PingProbe probes[8];
        float avgOneWayMs = -1.0f;
    };

    void SendPingOn(uint32_t conn, uint32_t& seqCounter, PingProbe* probes);
    static void NotePong(uint32_t seq, const PingProbe* probes, float& avgOneWayMs);

    int AllocatePlayerId() const;
    void DropClient(uint32_t conn, const Events& ev);

    Mode m_mode = Mode::Offline;
    ClientState m_clientState = ClientState::Idle;
    bool m_steamOk = false;
    uint64_t m_mySteamId = 0;
    std::string m_myName = "player";

    uint32_t m_listenP2P = 0;
    uint32_t m_listenIP = 0;
    uint32_t m_pollGroup = 0;
    uint32_t m_hostConn = 0;                // client's connection to the host
    double m_hostNextPingAt = 0;
    uint32_t m_hostPingSeq = 0;
    PingProbe m_hostProbes[8];
    float m_hostAvgOneWayMs = -1.0f;
    std::vector<Client> m_clients;
    const Events* m_events = nullptr;       // valid during Poll
    std::string m_disconnectReason;
    bool m_usedPlayerIds[MaxPlayers]{};
    int m_joinCap = MaxLobbyPlayers;
};

} // namespace tankaq::net
