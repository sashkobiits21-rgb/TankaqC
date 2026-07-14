#pragma once
#include <cstdint>
#include <vector>
#include <DirectXMath.h>

namespace tankaq
{

constexpr int   MaxPlayers = 8;
constexpr int   MaxLobbyPlayers = 4;         // join cap
constexpr int   MaxProjectiles = 24;
constexpr int   TickRate = 64;               // 64-tick server
constexpr float TickDt = 1.0f / TickRate;
constexpr int   StatRecalcTicks = 8;         // host recalcs stats every 8 ticks

// ------------------------------------------------------------------- match
enum : uint8_t
{
    PhaseLobby = 0,      // ready-up, tanks parked in a lineup
    PhasePlaying,        // 5-minute kill-count match
    PhaseOvertime,       // tied at the horn: first kill wins
    PhaseEnded,          // winner banner, auto-return to lobby
};
constexpr uint32_t MatchDurationTicks = 5 * 60 * TickRate;
constexpr uint32_t EndedReturnTicks = 6 * TickRate;
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

// Offer lifecycle: 0 = empty, 1 = purchasable, 2 = consumed (purchased, the
// burn animation is playing; the slot is held and the conveyor must not shift
// until the host expires it -- this is what keeps UI animations and the array
// in agreement).
enum : uint8_t { OfferNone = 0, OfferActive = 1, OfferConsumed = 2 };
constexpr uint32_t OfferBurnTicks = uint32_t(0.55f * TickRate + 0.5f);

struct Offer
{
    uint8_t active = OfferNone;
    uint8_t id = 0;        // rolling per-player identity for UI animation
    uint8_t type = 0;      // index into kUpgradePool
    uint16_t cost = 0;
    uint32_t consumedTick = 0;   // host-only: when the burn finishes
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
    char name[16]{};                      // display name (from Hello / persona)
    uint8_t ready = 0;                    // lobby ready flag
    float stats[StatCount]{};             // cached finals (replicated)
    Offer offers[NumOfferSlots];          // replicated to the owning client
    std::vector<uint8_t> owned;           // purchased upgrade types (host only)
    std::vector<Offer> pendingOffers;     // host: arrivals queued during burns
    uint8_t nextOfferId = 1;
    uint32_t nextOfferTick = 0;
    uint32_t nextPendingDrainTick = 0;

    // host-only lag compensation state (not replicated)
    float lagOneWayMs = 0;                // averaged one-way latency to this player
    float lastInMoveX = 0, lastInMoveZ = 0;
    float catchupX = 0, catchupZ = 0;     // owed pre-travel, drained over ~2 ticks
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
    uint8_t phase = PhaseLobby;
    uint8_t winner = 0xFF;
    uint32_t matchEndTick = 0;
    uint32_t endedTick = 0;
    bool lagCompEnabled = true;   // host: input catch-up on direction changes

    // world-space muzzle parameters, set once from the loaded model
    DirectX::XMFLOAT3 turretPivot{};
    DirectX::XMFLOAT3 muzzleOffset{};

    void SpawnPlayer(int id);
    void RemovePlayer(int id);
    // Host: fresh 5-minute match (full per-player reset, ring spawns).
    void StartMatch();
    // Host: back to the ready-up lineup.
    void ToLobby();
    // Advances the full simulation one tick. `inputs` is indexed by player id.
    void Tick(const InputCmd* inputs);
    // Movement + turret only (no timers/firing): shared by the host simulation
    // and client-side prediction so both integrate identically.
    void AdvanceMovement(int id, const InputCmd& in);
    // Rebuild a player's cached stats from base + owned upgrades:
    // additions first, then multiplications.
    void RecalcStats(int id);
    // Host-side purchase validation; returns true and applies on success.
    // The offer is marked consumed and its slot is held until the burn
    // animation duration elapses; only then does the conveyor compact.
    bool TryPurchase(int id, int slot);
    // Roll a fresh random offer. Inserts at slot 0 immediately, or queues it
    // while any burn animation holds the conveyor.
    void GenerateOffer(int id);
    void InsertOffer(int id, const Offer& o);
    bool AnyConsumed(int id) const;
    DirectX::XMFLOAT3 MuzzleWorld(const PlayerState& p) const;

    uint32_t rngState = 0x9E3779B9u;
    uint32_t NextRand();   // xorshift, host-side offer rolls
};

float WrapAngle(float a);
float MoveTowardsAngle(float current, float target, float maxDelta);
// Lobby lineup slot for a player id (tanks face the lobby camera).
void LobbySpot(int id, float& x, float& z, float& yaw);

} // namespace tankaq
