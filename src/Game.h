#pragma once
#include <cstdint>
#include <vector>
#include <DirectXMath.h>

namespace tankaq
{

constexpr int   MaxPlayers = 8;
constexpr int   MaxProjectiles = 24;
constexpr int   TickRate = 64;               // 64-tick server
constexpr float TickDt = 1.0f / TickRate;
constexpr int   StatRecalcTicks = 8;         // host recalcs stats every 8 ticks
constexpr int   SnapshotEveryTicks = 3;      // 20 Hz
constexpr float ArenaHalf = 30.0f;
constexpr float TankSpeed = 7.0f;            // units/s
constexpr float HullFaceSpeed = 7.5f;        // rad/s, visual hull turn toward travel dir
constexpr float TurretTurnSpeed = 5.0f;      // rad/s
constexpr float TankRadius = 1.7f;           // collision circle
constexpr float ProjectileSpeed = 22.0f;
constexpr float ProjectileLife = 2.5f;
constexpr float ProjectileRadius = 0.25f;
constexpr float FireCooldown = 0.9f;
constexpr int   MaxHealth = 100;
constexpr int   ProjectileDamage = 34;
constexpr float RespawnTime = 3.0f;

// input button bits
enum : uint8_t
{
    BtnForward = 1 << 0,
    BtnBack    = 1 << 1,
    BtnLeft    = 1 << 2,
    BtnRight   = 1 << 3,
    BtnFire    = 1 << 4,
};

struct InputCmd
{
    uint8_t buttons = 0;      // fire bit; WASD bits kept for debugging
    float moveX = 0;          // desired world-space move direction (camera-
    float moveZ = 0;          //  relative WASD is resolved on the client)
    float turretYaw = 0.0f;   // desired absolute (world) turret yaw
};

// ------------------------------------------------------------------- stats
// Upgrades never touch gameplay directly: they only contribute StatMods, and
// the final value of every stat is rebuilt from scratch as
//     final = (base + sum of all amounts) * product of all factors
// i.e. additions/subtractions first, then multiplications/divisions. The two
// accumulation "dictionaries" (stat -> summed amount, stat -> product of
// factors) are flat arrays indexed by the Stat enum: the key space is tiny
// and fixed, so this is the same mapping with no hashing and no allocation.
enum class Stat : uint8_t
{
    MoveSpeed = 0,    // units/s
    Damage,           // hit points per shell
    ReloadTime,       // seconds between shots (lower = faster)
    MaxHealth,        // hit points
    ProjSpeed,        // units/s
    DamageTaken,      // incoming damage multiplier (lower = tankier)
    Count
};
constexpr int StatCount = int(Stat::Count);
extern const float kBaseStats[StatCount];

// One stat change: `amount` is added in the first pass, `factor` multiplied
// in the second. An upgrade holds several of these and may mix both kinds.
struct StatMod
{
    Stat stat;
    float amount = 0.0f;
    float factor = 1.0f;
};

constexpr int MaxModsPerUpgrade = 3;
struct UpgradeType
{
    const char* name;
    const char* desc;
    int rarity;        // 0 common, 1 uncommon, 2 rare, 3 epic, 4 legendary
    int baseCost;      // grows per owned copy of the same type
    int icon;          // index into the icon atlas
    StatMod mods[MaxModsPerUpgrade];
    int modCount;
};
extern const UpgradeType kUpgradePool[];
extern const int UpgradePoolSize;

// ------------------------------------------------------------------ offers
// Each player runs a conveyor of up to 6 offers; a new random one arrives
// every 5 seconds at slot 0 and pushes the rest along; overflow burns the
// tail. Offers carry a rolling id so clients can animate the shifts.
constexpr int NumOfferSlots = 6;
constexpr int OfferIntervalTicks = 5 * TickRate;

struct Offer
{
    uint8_t active = 0;
    uint8_t id = 0;        // rolling per-player identity for UI animation
    uint8_t type = 0;      // index into kUpgradePool
    uint16_t cost = 0;
};

struct PlayerState
{
    bool active = false;
    float x = 0, z = 0;
    float hullYaw = 0;        // 0 = +Z, positive rotates +Z toward +X
    float turretYaw = 0;      // absolute world yaw
    int health = 0;
    float respawnTimer = 0;
    float fireCooldown = 0;
    float hitFlash = 0;       // seconds of red flash remaining
    float muzzleFlash = 0;    // seconds of muzzle glow remaining
    uint16_t score = 0;
    uint16_t money = 0;
    float stats[StatCount]{};             // cached finals (replicated)
    Offer offers[NumOfferSlots];          // replicated to the owning client
    std::vector<uint8_t> owned;           // purchased upgrade types (host only)
    uint8_t nextOfferId = 1;
    uint32_t nextOfferTick = 0;
};

inline int MaxHealthFor(const PlayerState& p)
{
    return int(p.stats[int(Stat::MaxHealth)] + 0.5f);
}

struct Projectile
{
    bool active = false;
    uint8_t owner = 0;
    float x = 0, y = 0, z = 0;
    float yaw = 0;
    float life = 0;
    float speed = ProjectileSpeed;   // host-side, baked from owner upgrades
    int damage = ProjectileDamage;
};

struct Obstacle
{
    float cx, cz;      // center
    float hx, hz;      // half extents
    float height;
};

constexpr int NumObstacles = 7;
extern const Obstacle kObstacles[NumObstacles];

struct GameState
{
    PlayerState players[MaxPlayers];
    Projectile projectiles[MaxProjectiles];
    uint32_t tick = 0;

    // world-space muzzle parameters, set once from the loaded model
    DirectX::XMFLOAT3 turretPivot{};
    DirectX::XMFLOAT3 muzzleOffset{};

    void SpawnPlayer(int id);
    void RemovePlayer(int id);
    // Advances the full simulation one tick. `inputs` is indexed by player id.
    void Tick(const InputCmd* inputs);
    // Movement + turret only (no timers/firing): shared by the host simulation
    // and client-side prediction so both integrate identically.
    void AdvanceMovement(int id, const InputCmd& in);
    // Rebuild a player's cached stats from base + owned upgrades:
    // additions first, then multiplications.
    void RecalcStats(int id);
    // Host-side purchase validation; returns true and applies on success.
    bool TryPurchase(int id, int slot);
    // Push a fresh random offer into slot 0 (conveyor shift, tail drops).
    void GenerateOffer(int id);
    DirectX::XMFLOAT3 MuzzleWorld(const PlayerState& p) const;

    uint32_t rngState = 0x9E3779B9u;
    uint32_t NextRand();   // xorshift, host-side offer rolls
};

float WrapAngle(float a);
float MoveTowardsAngle(float current, float target, float maxDelta);

} // namespace tankaq
