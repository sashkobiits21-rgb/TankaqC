#pragma once
#include <cstdint>
#include "Game.h"

namespace tankaq::net
{

constexpr uint8_t ProtocolVersion = 8;   // v8: Bounces stat (stats[] resized)
constexpr uint16_t DefaultPort = 27500;

enum class MsgType : uint8_t
{
    Hello = 1,     // client -> host, reliable
    Welcome,       // host -> client, reliable
    Reject,        // host -> client, reliable
    Input,         // client -> host, unreliable
    Snapshot,      // host -> all, unreliable
    Purchase,      // client -> host, reliable
    Ready,         // client -> host, reliable
    Ping,          // either direction, unreliable (latency probe)
    Pong,          // echo of Ping
};

#pragma pack(push, 1)

struct MsgHello
{
    uint8_t type = uint8_t(MsgType::Hello);
    uint8_t version = ProtocolVersion;
    char name[16]{};
};

struct MsgWelcome
{
    uint8_t type = uint8_t(MsgType::Welcome);
    uint8_t playerId = 0;
};

struct MsgReject
{
    uint8_t type = uint8_t(MsgType::Reject);
    uint8_t reason = 0;    // 0 = full, 1 = version mismatch
};

struct MsgInput
{
    uint8_t type = uint8_t(MsgType::Input);
    uint8_t buttons = 0;
    uint32_t seq = 0;      // client input sequence number, echoed back as ack
    float moveX = 0;       // world-space move direction (camera-relative WASD
    float moveZ = 0;       //  resolved on the client)
    float turretYaw = 0;
};

struct MsgPurchase
{
    uint8_t type = uint8_t(MsgType::Purchase);
    uint8_t slot = 0;      // offer slot index
};

struct MsgReady
{
    uint8_t type = uint8_t(MsgType::Ready);
    uint8_t ready = 0;
};

// Latency probes: sent every 4 ticks (~16/s, ~10 bytes each). The receiver
// echoes a Pong; the sender takes RTT/2 as one-way latency and keeps an EMA.
struct MsgPing
{
    uint8_t type = uint8_t(MsgType::Ping);
    uint32_t seq = 0;
};

struct MsgPong
{
    uint8_t type = uint8_t(MsgType::Pong);
    uint32_t seq = 0;
};

struct OfferNet
{
    uint8_t active = 0;
    uint8_t id = 0;
    uint8_t type = 0;
    uint16_t cost = 0;
};

struct PlayerNet
{
    uint8_t active = 0;
    uint8_t ready = 0;
    char name[16]{};
    uint16_t health = 0;   // max health is uncapped now
    uint16_t score = 0;
    uint16_t money = 0;
    float stats[StatCount]{};          // cached finals (prediction + HUD)
    OfferNet offers[NumOfferSlots];    // this player's upgrade conveyor
    uint32_t ackSeq = 0;   // last input sequence the host simulated for this player
    float x = 0, z = 0;
    float hullYaw = 0, turretYaw = 0;
    uint8_t flags = 0;     // bit0 hitFlash, bit1 muzzleFlash
};

struct ProjectileNet
{
    uint8_t active = 0;
    float x = 0, y = 0, z = 0;
    float yaw = 0;
};

struct MsgSnapshot
{
    uint8_t type = uint8_t(MsgType::Snapshot);
    uint8_t phase = PhaseLobby;
    uint8_t winner = 0xFF;
    uint8_t targetPlayers = 0;    // gathering queue size (0 = no queue)
    uint32_t tick = 0;
    uint32_t matchEndTick = 0;
    PlayerNet players[MaxPlayers];
    ProjectileNet projectiles[MaxProjectiles];
};

#pragma pack(pop)

} // namespace tankaq::net
