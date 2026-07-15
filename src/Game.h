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
    PhaseGathering,      // quick-match queue: waiting for targetPlayers
};
constexpr uint32_t MatchDurationTicks = 5 * 60 * TickRate;
constexpr uint32_t EndedReturnTicks = 6 * TickRate;
constexpr int   SnapshotEveryTicks = 3;      // 20 Hz
constexpr float ArenaHalf = 30.0f;
constexpr float TankSpeed = 7.0f;            // units/s
constexpr float HullFaceSpeed = 7.5f;        // rad/s, visual hull turn toward travel dir
constexpr float TurretTurnSpeed = 5.0f;      // rad/s
constexpr float TankRadius = 1.7f;           // collision circle
constexpr float ProjectileSpeed = 15.0f;   // slower: squish/spring readable
constexpr float ProjectileLife = 3.4f;     // range kept ~= speed * life
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
    BtnBoost   = 1 << 5,   // SHIFT: 2x speed, drains the fuel bar
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
    Bounces,          // wall bounces per rocket (never bounces off tanks)
    BoostSpeed,       // speed multiplier while boosting (SHIFT)
    BoostFuel,        // fuel capacity in seconds of boost
    BoostRegen,       // fuel restored per second (after the delay)
    BoostRegenDelay,  // seconds after boosting before regen starts
    // SOLDIER class (summon; gameplay entity lands with the summon AI)
    SoldierSpeed,     // units/s
    SoldierDamage,    // hit points per soldier shot
    SoldierHealth,    // hit points per soldier
    SoldierFireRate,  // soldier shots per second
    SoldierMax,       // max concurrent soldiers
    SoldierCooldown,  // seconds between soldier spawns
    // BOUNCY class: per-ricochet multipliers baked into the rocket at fire
    BounceDamage,     // damage multiplier applied on every wall bounce
    BounceSpeed,      // speed multiplier applied on every wall bounce
    Count
};
constexpr int StatCount = int(Stat::Count);
extern const float kBaseStats[StatCount];
extern int gDebugBounces;   // --bounces=N dev knob, added onto the stat

// One stat change: `amount` is added in the first pass, `factor` multiplied
// in the second. An upgrade holds several of these and may mix both kinds.
struct StatMod
{
    Stat stat;
    float amount = 0.0f;
    float factor = 1.0f;
};

// ------------------------------------------------------------------ classes
// Classes keep the pool clean as exotic content lands: class-locked upgrades
// only enter a player's offers after they buy that class's CARD (its own
// rarity band, teal). The card immediately grants the class's base upgrade
// and unlocks the family. A player holds at most kMaxClasses classes.
constexpr uint8_t ClassNone = 0xFF;
enum : uint8_t { ClassSoldier = 0, ClassBouncy, ClassCount };
constexpr int kMaxClasses = 2;
constexpr int RarityClass = 5;    // rolled between rare and epic

// Stable identity for every upgrade. The wire sends these as uint8 pool
// indices, so ORDER IS THE PROTOCOL: append new entries before Count, never
// reorder (reordering = ProtocolVersion bump). kUpgradePool is declared with
// this exact size and ValidateUpgradePool() checks pool[i].id == i at
// startup, so an enum/pool mismatch fails loudly instead of shipping
// swapped upgrades.
enum class UpgradeId : uint8_t
{
    Engine = 0, Turbo, ApRounds, HeavyShells, Autoloader, GreasedBreech,
    Plating, Composite, Gyro, ReactiveArmor, Overdrive, FieldKit,
    Ricochet, Superball, NitroTank, Afterburner, QuickPump, PitCrew,
    FuelInjection, SoldierClass, BouncyClass, Recruiter, DoubleTime,
    FlakVest, HollowPoints, RapidFire, Platoon, RubberShells, Slingshot,
    Count
};
constexpr int UpgradeCount = int(UpgradeId::Count);
static_assert(UpgradeCount <= 256, "upgrade type is a uint8 on the wire");

constexpr int MaxModsPerUpgrade = 3;
struct UpgradeType
{
    UpgradeId id;      // must equal this entry's pool position (validated)
    const char* name;
    const char* desc;
    int rarity;        // 0 common, 1 uncommon, 2 rare, 3 epic, 4 legendary,
                       // 5 class card (cards only, enforced)
    int baseCost;      // grows per owned copy of the same type
    StatMod mods[MaxModsPerUpgrade];
    int modCount;
    uint8_t classReq = ClassNone;        // offered only if the class is owned
    uint8_t classGrant = ClassNone;      // buying this card unlocks the class
    UpgradeId grant = UpgradeId::Count;  // base upgrade granted by this card
};
// Sized by the enum: a new UpgradeId without a pool entry leaves a null-named
// hole that validation rejects; an extra pool entry fails to compile.
// The icon atlas slot for an upgrade is simply its pool index.
extern const UpgradeType kUpgradePool[UpgradeCount];
inline const UpgradeType& UpgradeDef(UpgradeId id) { return kUpgradePool[int(id)]; }
// Returns nullptr when the pool is consistent, else a description of the
// first violation. Called at startup (fatal) and by --classtest.
const char* ValidateUpgradePool();

struct PlayerState;
bool HasClass(const PlayerState& p, uint8_t cls);
int CountClasses(const PlayerState& p);

// ------------------------------------------------------------------ offers
// Each player runs a conveyor of up to 6 offers; a new random one arrives
// every 5 seconds at slot 0 and pushes the rest along; overflow burns the
// tail. Offers carry a rolling id so clients can animate the shifts.
constexpr int NumOfferSlots = 6;
constexpr int OfferIntervalTicks = 3 * TickRate;

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
    float boostFuel = 0;         // current fuel (seconds of boost left)
    float boostRegenWait = 0;    // time until regen resumes
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
    int bounces = 0;                 // wall bounces left (baked at fire time)
    float bounceDmg = 1.0f;          // damage multiplier per ricochet (baked)
    float bounceSpd = 1.0f;          // speed multiplier per ricochet (baked)
};

struct Obstacle
{
    float cx, cz;      // center
    float hx, hz;      // half extents
    float height;
};

constexpr int NumObstacles = 7;
extern const Obstacle kObstacles[NumObstacles];

// One tick of projectile flight: lifetime, movement, and the wall/obstacle
// ricochet rules (each bounce consumes pr.bounces and multiplies speed and
// damage by the baked per-bounce stats). Returns false when spent. THE single
// copy of these rules: the host simulation and the client's provisional
// (predicted) rockets both call this, so they can never drift apart.
bool StepProjectile(Projectile& pr, float dt);

struct GameState
{
    PlayerState players[MaxPlayers];
    Projectile projectiles[MaxProjectiles];
    uint32_t tick = 0;
    uint8_t phase = PhaseLobby;
    uint8_t winner = 0xFF;
    uint8_t targetPlayers = 0;    // quick-match queue size (0 = no queue)
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
    // Host: quick-match queue -- tanks park but the ready-up lobby stays
    // hidden until `playersWanted` tanks have gathered, then ToLobby().
    void StartGathering(int playersWanted);
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
