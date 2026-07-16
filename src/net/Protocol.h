#pragma once
#include <cstdint>
#include "Game.h"

namespace tankaq::net
{

// v9: stats leave the wire entirely. Upgrades (the source of truth) replicate
// as reliable events; every peer derives stats locally with the identical
// deterministic RecalcStats (additions first, then multiplications). Adding
// stats no longer changes the protocol.
// v10: boost (SHIFT) -- BtnBoost input bit + quantized fuel/regen-wait in
// PlayerNet so prediction replay rebases the fuel state.
// v11: class system -- no wire change, but upgrade indices remap (soldier +
// bouncy families), and upgrades replicate by pool index, so mixed-version
// peers must be rejected.
// v12: soldier summons -- SoldierNet array in snapshots. Clients drive the
// animation locally from this state (fire-and-forget visuals).
// v13: soldiers fire pool rockets, base SoldierHealth 30 -- kBaseStats is
// part of the derived-stats contract, so mixed versions must not pair.
// v14: NECROMANCER (skulls / acid puddles / possessing ghosts) and RADAR
// (ring detonation rockets): Skull/Puddle/Ghost arrays, possession timer in
// PlayerNet, radar range + ring count per projectile.
// v15: SkullDamage stat, radar circles become a packed TREE (rings field =
// extra circle count), skull contact damage -- derived-stat contract change.
// v16: radar lock progress byte per projectile (countdown fill visual).
// v17: radar root damage = the rocket's own damage (+ RadarDamage bonus,
// base 0) -- derived-stat contract change.
// v18: FISSION SHELLS (SplitChance stat), ghost 2 s fuse + faster spiral,
// necro-killed soldiers rise as ghosts -- derived-stat contract change.
constexpr uint8_t ProtocolVersion = 21;
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
    Upgrade,       // host -> all, reliable: a purchase was applied
    OwnedReset,    // host -> all, reliable: match start wiped all upgrades
    OwnedSync,     // host -> one client, reliable: full owned list (late join)
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

// A validated purchase, broadcast so every peer appends to that player's
// owned list and re-derives stats locally (add first, multiply last).
struct MsgUpgrade
{
    uint8_t type = uint8_t(MsgType::Upgrade);
    uint8_t playerId = 0;
    uint8_t upgradeType = 0;
};

struct MsgOwnedReset
{
    uint8_t type = uint8_t(MsgType::OwnedReset);
};

// Full owned list for one player (late-join sync). Sent truncated to the
// actual count; kMaxOwnedSync bounds the message.
constexpr int kMaxOwnedSync = 512;
struct MsgOwnedSync
{
    uint8_t type = uint8_t(MsgType::OwnedSync);
    uint8_t playerId = 0;
    uint16_t count = 0;
    uint8_t types[kMaxOwnedSync]{};
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
    // stats are NOT replicated: peers derive them from the owned-upgrade
    // events (MsgUpgrade / OwnedReset / OwnedSync) via local RecalcStats
    OfferNet offers[NumOfferSlots];    // this player's upgrade conveyor
    uint32_t ackSeq = 0;   // last input sequence the host simulated for this player
    float x = 0, z = 0;
    float hullYaw = 0, turretYaw = 0;
    uint8_t flags = 0;     // bit0 hitFlash, bit1 muzzleFlash
    uint8_t fuel255 = 0;       // boost fuel / capacity, quantized to 0..255
    uint8_t regenWait32 = 0;   // regen delay remaining, 1/32 s units
    uint8_t possess32 = 0;     // possession remaining, 1/32 s (0 = free)
    uint8_t shield16 = 0;      // barrier remaining, 1/16 s (0 = down)
    uint8_t shieldCd4 = 0;     // ability cooldown remaining, 1/4 s
};

struct ProjectileNet
{
    uint8_t active = 0;
    uint8_t radar16 = 0;      // radar ring radius * 16 (0 = not radar)
    uint8_t radarRings = 0;   // packed extra circles
    uint8_t lock255 = 0;      // lock progress * 255 (countdown fill)
    uint8_t deflected = 0;    // shield ricochet: renders orange
    float x = 0, y = 0, z = 0;
    float yaw = 0;
};

struct SkullNet
{
    uint8_t active = 0;
    uint8_t owner = 0;
    float x = 0, z = 0;
    float yaw = 0;
};

struct PuddleNet
{
    uint8_t active = 0;
    uint8_t owner = 0;
    uint8_t life16 = 0;   // remaining life * 16, capped (fade-out on client)
    float x = 0, z = 0;
};

struct GhostNet
{
    uint8_t active = 0;
    uint8_t owner = 0;
    float x = 0, z = 0;
};

struct SoldierNet
{
    uint8_t active = 0;
    uint8_t owner = 0;
    uint8_t state = 0;        // SoldierGuard/Cover/Move/Kite/Dying
    uint8_t targetId = 0xFF;  // aimed-at tank (drives the client aim pose)
    uint8_t health = 0;       // clamped to 255 for the wire
    uint8_t flags = 0;        // bit0 muzzleFlash, bit1 hitFlash
    float x = 0, z = 0;
    float yaw = 0;
};

struct GrenadeNet
{
    uint8_t active = 0;
    uint8_t owner = 0;
    uint8_t fuse255 = 255;    // 255 = not armed yet; else fuse * 100
    float x = 0, y = 0, z = 0;
};

struct MsgSnapshot
{
    uint8_t type = uint8_t(MsgType::Snapshot);
    uint8_t phase = PhaseLobby;
    uint8_t winner = 0xFF;
    uint8_t targetPlayers = 0;    // gathering queue size (0 = no queue)
    uint8_t matchMinutes = 10;    // host lobby pick (5/10/15/20)
    uint32_t tick = 0;
    uint32_t matchEndTick = 0;
    PlayerNet players[MaxPlayers];
    ProjectileNet projectiles[MaxProjectiles];
    SoldierNet soldiers[MaxSoldiers];
    SkullNet skulls[MaxSkulls];
    PuddleNet puddles[MaxPuddles];
    GhostNet ghosts[MaxGhosts];
    GrenadeNet grenades[MaxGrenades];
};

#pragma pack(pop)

} // namespace tankaq::net
