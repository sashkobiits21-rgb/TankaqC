#pragma once
#include <cstdint>
#include <DirectXMath.h>

namespace tankaq
{

constexpr int   MaxPlayers = 8;
constexpr int   MaxProjectiles = 24;
constexpr int   TickRate = 60;
constexpr float TickDt = 1.0f / TickRate;
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

// ---------------------------------------------------------------- upgrades
constexpr int NumUpgrades = 6;
constexpr int MaxUpgradeLevel = 5;
enum : int
{
    UpgEngine = 0,     // +12% speed per level
    UpgDamage,         // +15% damage per level
    UpgReload,         // -12% cooldown per level
    UpgHealth,         // +25 max health per level
    UpgVelocity,       // +10% projectile speed per level
    UpgArmor,          // -8% damage taken per level
};

struct UpgradeDef
{
    const char* name;
    const char* desc;
    int baseCost;      // cost scales: baseCost * (level + 1)
    int rarity;        // 0 common, 1 uncommon, 2 rare, 3 epic, 4 legendary
};
extern const UpgradeDef kUpgrades[NumUpgrades];

inline int UpgradeCost(int slot, int level)
{
    return kUpgrades[slot].baseCost * (level + 1);
}

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
    uint8_t upgrades[NumUpgrades]{};
};

inline int MaxHealthFor(const PlayerState& p)
{
    return MaxHealth + 25 * p.upgrades[UpgHealth];
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
    // Host-side purchase validation; returns true and applies on success.
    bool TryPurchase(int id, int slot);
    DirectX::XMFLOAT3 MuzzleWorld(const PlayerState& p) const;
};

float WrapAngle(float a);
float MoveTowardsAngle(float current, float target, float maxDelta);

} // namespace tankaq
