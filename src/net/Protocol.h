#pragma once
#include <cstdint>
#include "Game.h"

namespace tankaq::net
{

constexpr uint8_t ProtocolVersion = 3;   // v3: input sequence numbers + acks
                                         //     (client-side prediction)
constexpr uint16_t DefaultPort = 27500;

enum class MsgType : uint8_t
{
    Hello = 1,     // client -> host, reliable
    Welcome,       // host -> client, reliable
    Reject,        // host -> client, reliable
    Input,         // client -> host, unreliable
    Snapshot,      // host -> all, unreliable
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
    float turretYaw = 0;
};

struct PlayerNet
{
    uint8_t active = 0;
    uint8_t health = 0;
    uint16_t score = 0;
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
    uint32_t tick = 0;
    PlayerNet players[MaxPlayers];
    ProjectileNet projectiles[MaxProjectiles];
};

#pragma pack(pop)

} // namespace tankaq::net
