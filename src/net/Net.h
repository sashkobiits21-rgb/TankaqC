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
        std::function<void(int playerId)> onPlayerJoined;
        std::function<void(int playerId)> onPlayerLeft;
        std::function<void(int playerId, const MsgInput&)> onInput;
        std::function<void(int playerId, int slot)> onPurchase;
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

    void Poll(const Events& ev);            // pump callbacks + messages, once per frame

    void SendInputToHost(const MsgInput& msg);
    void SendPurchaseToHost(int slot);
    void BroadcastSnapshot(const MsgSnapshot& snap);

    Mode mode() const { return m_mode; }
    ClientState clientState() const { return m_clientState; }
    bool steamAvailable() const { return m_steamOk; }
    // Steam Datagram Relay availability (100 = ready); detail gets the debug text.
    int relayStatus(std::string& detail) const;
    uint64_t mySteamId() const { return m_mySteamId; }
    std::string myName() const { return m_myName; }
    std::string joinCode() const;           // host's shareable code (SteamID64)
    int connectedClients() const;

    void HandleStatusChanged(SteamNetConnectionStatusChangedCallback_t* info);

private:
    struct Client
    {
        uint32_t conn = 0;
        int playerId = -1;
        bool helloed = false;
    };

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
    std::vector<Client> m_clients;
    const Events* m_events = nullptr;       // valid during Poll
    std::string m_disconnectReason;
    bool m_usedPlayerIds[MaxPlayers]{};
};

} // namespace tankaq::net
