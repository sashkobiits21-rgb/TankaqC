#pragma once
#include <cstdint>
#include <vector>
#include <DirectXMath.h>

namespace tankaq
{

constexpr int   MaxPlayers = 8;
constexpr int   MaxLobbyPlayers = 4;         // join cap
constexpr int   MaxProjectiles = 96;   // shots must never fail to spawn
constexpr int   TickRate = 64;               // 64-tick server
constexpr float TickDt = 1.0f / TickRate;
constexpr int   StatRecalcTicks = 8;         // host recalcs stats every 8 ticks

// ------------------------------------------------------------------- match
enum : uint8_t
{
    PhaseLobby = 0,      // ready-up, tanks parked in a lineup
    PhasePlaying,        // timed kill-count match (host picks the length)
    PhaseOvertime,       // tied at the horn: first kill wins
    PhaseEnded,          // winner banner, auto-return to lobby
    PhaseGathering,      // quick-match queue: waiting for targetPlayers
};
// Match length: the host cycles through kMatchMinutes in the lobby.
constexpr uint8_t kMatchMinutes[] = { 5, 10, 15, 20 };
constexpr uint8_t DefaultMatchMinutes = 10;
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
    BtnAbility1 = 1 << 6,  // "1": first ability slot (SHIELD)
};

struct InputCmd
{
    uint8_t buttons = 0;      // fire bit; WASD bits kept for debugging
    float moveX = 0;          // desired world-space move direction (camera-
    float moveZ = 0;          //  relative WASD is resolved on the client)
    float turretYaw = 0.0f;   // desired absolute (world) turret yaw
    // freshest aim the host knows for this player (jitter-buffered inputs
    // lag turretYaw; the SHIELD deflects with this so a swinging client
    // aim still blocks what it visibly faces)
    float aimYawFresh = 0.0f;
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
    SplitChance,      // chance per bounce to split off ONE half-damage twin
    // NECROMANCER class
    SkullRate,        // seconds between skull launches
    SkullDamage,      // direct contact damage when a skull hits a body
    AcidDps,          // acid puddle damage per second (standing in it)
    AcidDuration,     // puddle lifetime in seconds
    PossessDps,       // damage per second while possessed by a ghost
    PossessDuration,  // possession length in seconds
    // RADAR class: rockets carry a TREE of detection circles
    RadarRange,       // root circle radius around the rocket
    RadarLock,        // seconds an enemy must stay inside to trigger
    RadarDamage,      // BONUS on the root circle (base = rocket damage;
                      // halves per tree level)
    RadarRings,       // extra circles packed inside (3 slots per parent)
    // SOLDIER legendary: physics grenades lobbed over cover
    GrenadeCount,     // grenades each soldier carries per life (0 = none)
    GrenadeCooldown,  // seconds between throws
    // SHIELD PROFICIENCY: the deflector barrier ability (key 1)
    ShieldWidth,      // barrier face width in world units
    ShieldDuration,   // seconds the barrier stays up
    ShieldCooldown,   // seconds between uses (starts at activation)
    Count
};
constexpr int StatCount = int(Stat::Count);
extern const float kBaseStats[StatCount];
extern int gDebugBounces;   // --bounces=N dev knob, added onto the stat
extern int gWallSpawns;     // --spawns=wall QA knob: 2 points flanking the
                            // long wall (stealth line-of-sight testing)

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
enum : uint8_t { ClassSoldier = 0, ClassBouncy, ClassNecro, ClassRadar,
                 ClassShield, ClassCount };
constexpr int kMaxClasses = 2;

// UNIQUE rule-benders ------------------------------------------------------
constexpr float TerroristRadius = 14.0f;  // death blast reach (huge)
constexpr float TerroristPlateau = 3.0f;  // 100% max HP inside one tank
                                          // length; beyond it the wave is
                                          // weaker: 60% at the edge of the
                                          // plateau, sliding gently to zero
constexpr float TerroristFalloffTop = 0.6f;
constexpr float VampireLifesteal = 0.20f; // fraction of damage dealt
constexpr float VampireBurnPerSec = 5.0f; // flat HP per second in the sun
constexpr float StealthSlow = 0.65f;      // -35% speed, fixed
constexpr float StealthDamageMul = 0.85f; // -15% rocket damage, fixed
constexpr float DrunkenMin = 0.80f;       // speed wander band
constexpr float DrunkenMax = 1.30f;
constexpr int   DrunkenSegTicks = 96;     // new sway target every 1.5 s
constexpr int RarityClass = 5;    // rolled between rare and epic
constexpr int RarityUnique = 6;   // gold rule-benders, 2% band, stackable

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
    NecroClass, RadarClass,
    BoneFurnace, CausticBrew, LingeringRot, DeepGrip, SoulLeech,
    FastLock, WideScan, Payload, SharpPing, NestedArray,
    FissionShells,
    FragPack,
    ShieldClass,
    WideBarrier,
    LongWatch,
    RapidRedeploy,
    TripleDoctrine,
    PureArsenal,
    Drunken,
    Vampire,
    Terrorist,
    Stealth,
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

    // host-only: seconds until the next soldier summon (SOLDIER class)
    float soldierSpawnWait = 0;
    // host-only: seconds until the next skull launch (NECROMANCER class)
    float skullWait = 0;
    // host-only: fractional damage-over-time accumulator (acid + possession)
    float dotAccum = 0;

    // POSSESSION (ghost hit): while > 0 the tank ignores its owner's input
    // and drives itself with deterministic pseudo-random movement, cannot
    // fire, and takes PossessDps from the possessing ghost's owner. Lives in
    // AdvanceMovement so client prediction replays it identically (the value
    // is replicated quantized and rebased like boost fuel).
    float possessTimer = 0;
    float shieldTimer = 0;        // barrier remaining (input-driven, predicted)
    float shieldWait = 0;         // ability cooldown remaining
    float shieldAimYaw = 0;       // host: freshest aim, drives the deflector
    float sunAccum = 0;           // VAMPIRE: fractional sun burn carry
    float possessDps = 0;         // host-only, baked from the ghost's owner
    uint8_t possessedBy = 0xFF;   // host-only, damage attribution
};

inline int MaxHealthFor(const PlayerState& p)
{
    return int(p.stats[int(Stat::MaxHealth)] + 0.5f);
}

constexpr int MaxRadarExtra = 12;    // circles packed inside the root
constexpr int MaxRadarNodes = 1 + MaxRadarExtra;

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
    // RADAR class (baked at fire time; 0 = not a radar rocket). The rocket
    // carries a TREE of circles: the root (radarRange) plus radarRings extra
    // circles packed inside their parents at HALF radius (1 child centers,
    // 2 sit opposite, 3 form a triangle; 3 slots per parent, breadth-first).
    // The root contains every child, so one lock decides: a full charge
    // detonates the whole tree, and victims take radarDamage * (1/2)^depth
    // for every circle containing them.
    float radarRange = 0.0f;
    float radarDamage = 0.0f;
    float radarLockNeed = 0.0f;      // seconds inside the root to trigger
    int radarRings = 0;              // extra circles (0..MaxRadarExtra)
    float radarLock = 0.0f;
    float radarLockFrac = 0.0f;      // lock progress 0..1 (replicated for
                                     // the clockwise countdown fill)
    // BOUNCY: chance per bounce to split off ONE half-damage twin exiting
    // at a deviated angle. A rocket splits at most once; twins never split.
    float splitChance = 0.0f;
    uint8_t deflected = 0;     // shield ricochet: orange, new allegiance
};

// Lay out the radar circle tree for a rocket (sim, rendering and VFX all
// call this, so they can never disagree). Writes offsets relative to the
// rocket, radii and tree depth per node; returns the node count.
int RadarTreeLayout(float rootR, int extra, float yaw,
                    float* ox, float* oz, float* radius, int* depth, int cap);

struct Obstacle
{
    float cx, cz;      // center
    float hx, hz;      // half extents
    float height;
};

constexpr int NumObstacles = 7;
extern const Obstacle kObstacles[NumObstacles];

// --------------------------------------------------------------- soldiers
// The SOLDIER class summon: a host-authoritative infantry unit that hides
// behind obstacles, peeks out by RUNNING to its next cover spot while
// shooting at enemy tanks, and kites (keeps running + firing) when no spot
// blocks line of sight to every enemy. Visuals are fire-and-forget: clients
// drive animation locally from the replicated state, never corrected.
constexpr int   MaxSoldiers = 12;          // across all players
constexpr float SoldierRadius = 0.55f;
constexpr float SoldierFireRange = 15.0f;
constexpr float SoldierGunY = 1.05f;       // launcher muzzle height
constexpr float SoldierRocketSpeed = 13.0f;
constexpr float SoldierDeathTime = 1.1f;   // Death clip before despawn
constexpr float SoldierCoverPause = 1.5f;  // ducked seconds between peeks

enum : uint8_t
{
    SoldierGuard = 0,   // no enemies: loiter near the owner tank
    SoldierCover,       // ducked behind cover, hidden, holding fire
    SoldierMove,        // running to the next cover spot, firing on the move
    SoldierKite,        // no full cover exists: keep running + firing
    SoldierPeek,        // stepping OUT to a sight-line point for one shot
    SoldierDying,       // death animation playing, despawns after
};

struct SoldierState
{
    bool active = false;
    uint8_t owner = 0;
    uint8_t state = SoldierGuard;
    uint8_t targetId = 0xFF;      // current enemy tank (0xFF = none)
    float x = 0, z = 0;
    float yaw = 0;                // facing (movement direction while running)
    float health = 0;
    float stateTimer = 0;         // cover pause / kite re-evaluation
    float fireCooldown = 0;
    float coverX = 0, coverZ = 0; // destination cover spot
    float muzzleFlash = 0;        // seconds of muzzle flash left (visual)
    float hitFlash = 0;
    float deathTimer = 0;
    uint8_t lastHitBy = 0xFF;   // killer attribution (necromancer ghosts)
    // baked from the owner's stats at spawn time
    float speed = 4.0f, damage = 10.0f, fireRate = 1.0f;
    int grenades = 0;             // throws left this life (FRAG PACK)
    float grenadeWait = 0;        // seconds until the next throw
};

// A lobbed grenade: real ballistics (gravity, restitution) bouncing off the
// ground, arena walls, obstacle boxes and tanks. The 2 s fuse only starts
// at the FIRST bounce; the blast hits every enemy in the radius for double
// the throwing soldier's rocket damage. Fire-and-forget: host-simulated,
// replicated, never corrected.
struct GrenadeState
{
    bool active = false;
    uint8_t owner = 0;            // summoner (damage + kill credit)
    float x = 0, y = 0, z = 0;
    float vx = 0, vy = 0, vz = 0;
    float fuse = -1.0f;           // <0 until the first bounce arms it
    float dmg = 20.0f;            // baked: 2x the soldier's rocket damage
};

// 2D line-of-sight: does the segment cross any obstacle box (expanded by
// `inflate` -- pass a body radius to test WALKABILITY instead of sight)?
// Everything relevant flies below the shortest obstacle; height is ignored.
bool PointHitsObstacle(float x, float y, float z, float radius);
bool SegmentBlockedByObstacles(float x0, float z0, float x1, float z1,
                               float inflate = 0.0f);

// ------------------------------------------------------------ necromancer
// Skulls: launched from the tank every SkullRate seconds, fly STRAIGHT at
// the nearest enemy (course updated each tick), happily smack into walls,
// and burst into an acid puddle on ANY contact. Puddles deal AcidDps to
// enemies standing in them. Ghosts: one rises wherever the necromancer
// scores a tank kill; it orbits the nearest enemy, spiraling inward through
// walls, and on reaching the hull possesses it (see PlayerState).
constexpr int   MaxSkulls = 12;
constexpr int   MaxPuddles = 24;
constexpr int   MaxGhosts = 8;
constexpr float SkullSpeed = 9.0f;
constexpr float SkullRadius = 0.62f;
constexpr float SkullY = 1.2f;            // hover height
constexpr float PuddleRadius = 1.7f;
constexpr float GhostOrbitStart = 5.5f;   // spiral start radius
constexpr float GhostCloseRate = 2.4f;    // radius shrink per second
constexpr float GhostOrbitSpeed = 7.0f;   // tangential units/s
constexpr float GhostLifetime = 4.0f;     // seconds before it gives up
constexpr float ShieldDist = 2.3f;        // barrier face offset from hull
constexpr float ShieldSlow = 0.65f;       // fixed 35% slow while raised
constexpr float ShieldBoostMalus = 0.8f;  // boost 20% weaker on top of that
constexpr int   MaxGrenades = 12;
constexpr float GrenadeRadius = 0.22f;
constexpr float GrenadeGravity = 22.0f;   // gamey arcs: fast up, fast down
constexpr float GrenadeRestitution = 0.42f;
constexpr float GrenadeFriction = 0.72f;  // ground-contact horizontal damping
constexpr float GrenadeFuse = 2.0f;       // armed at the FIRST bounce
constexpr float GrenadeBlastRadius = 2.6f;
constexpr float GrenadeThrowRange = 15.0f;
// grenades duck under nothing: each box blocks up to ITS OWN height

struct SkullState
{
    bool active = false;
    uint8_t owner = 0;
    float x = 0, z = 0;
    float yaw = 0;
    float life = 0;
    float dmg = 0;   // direct contact damage, baked from the owner
};

struct PuddleState
{
    bool active = false;
    uint8_t owner = 0;
    float x = 0, z = 0;
    float life = 0;
    float dps = 0;
};

struct GhostState
{
    bool active = false;
    uint8_t owner = 0;
    uint8_t targetId = 0xFF;
    float x = 0, z = 0;
    float angle = 0;      // orbit phase around the target
    float orbitR = GhostOrbitStart;
    float life = GhostLifetime;   // vanishes when it runs out: RUN
};

// One tick of projectile flight: lifetime, movement, and the wall/obstacle
// ricochet rules (each bounce consumes pr.bounces and multiplies speed and
// damage by the baked per-bounce stats). Returns false when spent. THE single
// copy of these rules: the host simulation and the client's provisional
// (predicted) rockets both call this, so they can never drift apart.
bool StepProjectile(Projectile& pr, float dt);
// One tick of SHIELD deflection for a rocket: checks every raised barrier,
// reflects/flips/paints on a hit. THE single copy: the host projectile loop
// and the client's provisional (predicted) rockets both call it, so the
// bounce is visible the instant the shell meets the face on every screen.
struct GameState;
bool ShieldDeflectStep(GameState& gs, Projectile& pr);
// Buying PURE ARSENAL / TRIPLE DOCTRINE deletes the now-forbidden owned
// upgrades (classes / normals respectively); no-op for anything else.
void StripForUnique(PlayerState& p, UpgradeId bought);

// Owned-upgrade scan (uniques are one-copy, so presence == effect).
inline bool HasUpgrade(const PlayerState& p, UpgradeId u)
{
    for (uint8_t t : p.owned)
        if (t == uint8_t(u))
            return true;
    return false;
}
// TRIPLE DOCTRINE raises the class cap for its owner alone.
inline int MaxClassesFor(const PlayerState& p)
{
    return HasUpgrade(p, UpgradeId::TripleDoctrine) ? 3 : kMaxClasses;
}
// Sim-side sunlight test against the STATIC geometry (obstacles + walls),
// exact interval math along the fixed sun direction. Deterministic: the
// VAMPIRE burn must resolve identically on every peer.
bool InSunlight(float x, float z);
// DRUNKEN speed sway: deterministic from (tick, player) so prediction
// replays the exact same stagger. Smoothly wanders DrunkenMin..DrunkenMax.
float DrunkenFactor(uint32_t tick, int id);

struct GameState
{
    PlayerState players[MaxPlayers];
    Projectile projectiles[MaxProjectiles];
    SoldierState soldiers[MaxSoldiers];
    SkullState skulls[MaxSkulls];
    PuddleState puddles[MaxPuddles];
    GhostState ghosts[MaxGhosts];
    GrenadeState grenades[MaxGrenades];
    uint32_t tick = 0;
    uint8_t phase = PhaseLobby;
    uint8_t winner = 0xFF;
    uint8_t targetPlayers = 0;    // quick-match queue size (0 = no queue)
    uint8_t matchMinutes = DefaultMatchMinutes;   // host lobby choice
    uint8_t testMode = 0;         // TEST match: sandbox, free upgrade picks
    // spawn points: random but PRE-GENERATED once per match; picks favor
    // the point farthest from living enemy tanks
    static constexpr int MaxSpawns = 10;
    float spawnPX[MaxSpawns]{};
    float spawnPZ[MaxSpawns]{};
    int spawnCount = 0;
    uint32_t matchEndTick = 0;
    uint32_t endedTick = 0;
    bool lagCompEnabled = true;   // host: input catch-up on direction changes

    // world-space muzzle parameters, set once from the loaded model
    DirectX::XMFLOAT3 turretPivot{};
    DirectX::XMFLOAT3 muzzleOffset{};

    void SpawnPlayer(int id);
    // Roll a fresh scattered spawn set (obstacle-clear, well spaced).
    void GenerateSpawns();
    // Place a (re)spawning tank: the pre-generated point with the LARGEST
    // minimum distance to living enemies; yaw faces the arena center.
    void SpawnPoint(int id, float& x, float& z, float& yaw);
    void RemovePlayer(int id);
    // Host: fresh match, matchMinutes long (full per-player reset, ring spawns).
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
    // Host: place a fresh soldier for this owner beside their tank; returns
    // false when no slot or no clear spawn spot exists.
    bool SpawnSoldier(int ownerId);
    // One tick of a soldier's cover-point AI (host authority only).
    void TickSoldier(SoldierState& s);
    // Roll a fresh random offer. Inserts at slot 0 immediately, or queues it
    // while any burn animation holds the conveyor.
    void GenerateOffer(int id);
    void InsertOffer(int id, const Offer& o);
    bool AnyConsumed(int id) const;
    DirectX::XMFLOAT3 MuzzleWorld(const PlayerState& p) const;
    // Damage + rewards in one place (rockets and soldier fire): applies the
    // victim's DamageTaken, pays the shooter, handles kills + overtime.
    void ApplyDamage(int shooterId, int victimId, int rawDamage, int hitMoney);

    uint32_t rngState = 0x9E3779B9u;
    uint32_t NextRand();   // xorshift, host-side offer rolls
};

float WrapAngle(float a);
float MoveTowardsAngle(float current, float target, float maxDelta);
// Lobby lineup slot for a player id (tanks face the lobby camera).
void LobbySpot(int id, float& x, float& z, float& yaw);

} //