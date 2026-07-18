#include "Game.h"
#include <cmath>
#include <cstring>
#include <algorithm>

using namespace DirectX;

namespace tankaq
{

const float kBaseStats[StatCount] = {
    TankSpeed,          // MoveSpeed
    float(ProjectileDamage),
    FireCooldown,       // ReloadTime
    float(MaxHealth),
    ProjectileSpeed,    // ProjSpeed
    1.0f,               // DamageTaken multiplier
    0.0f,               // Bounces
    2.0f,               // BoostSpeed: 2x while boosting
    1.0f,               // BoostFuel: 1 second of boost
    0.4f,               // BoostRegen: fuel/s once regenerating
    1.2f,               // BoostRegenDelay: pause after boosting
    4.0f,               // SoldierSpeed
    10.0f,              // SoldierDamage
    30.0f,              // SoldierHealth: two-ish soldier rockets, one tank shell
    1.0f,               // SoldierFireRate: shots/s
    1.0f,               // SoldierMax: one soldier out at a time
    10.0f,              // SoldierCooldown: 10 s between spawns
    1.0f,               // BounceDamage: x1 per ricochet until upgraded
    1.0f,               // BounceSpeed
    0.0f,               // SplitChance: no fission until upgraded
    5.0f,               // SkullRate: a skull every 5 s
    5.0f,               // SkullDamage: direct contact damage
    4.0f,               // AcidDps
    3.0f,               // AcidDuration
    3.0f,               // PossessDps: 3 damage/s while possessed
    2.0f,               // PossessDuration: 2 s base
    3.5f,               // RadarRange
    0.6f,               // RadarLock: seconds inside a ring to trigger
    0.0f,               // RadarDamage: BONUS on the root circle -- the base
                        // is the rocket's own damage (halves per tree level)
    1.0f,               // RadarRings: one packed circle by default
    0.0f,               // GrenadeCount: soldiers carry none until FRAG PACK
    5.0f,               // GrenadeCooldown: a throw every 5 s
    4.0f,               // ShieldWidth
    4.0f,               // ShieldDuration
    12.0f,              // ShieldCooldown
};

int gDebugBounces = 0;   // --bounces=N dev knob (added on top of the stat)
int gWallSpawns = 0;     // --spawns=wall QA knob

// The pool: mods may mix additive and multiplicative parts, and one upgrade
// can touch several stats (including tradeoffs).
//
// Every entry names its UpgradeId first; ValidateUpgradePool() proves at
// startup that entry order matches the enum, that grants point at real
// family members, and that class cards are well-formed. Icon atlas slot ==
// pool index (no separate icon field to keep in sync).
#define S(x) Stat::x
#define U(x) UpgradeId::x
const UpgradeType kUpgradePool[UpgradeCount] = {
    { U(Engine),       "ENGINE",    "+5% MOVE SPEED",                  0, 30,
      { { S(MoveSpeed), 0, 1.05f } }, 1 },
    { U(Turbo),        "TURBO",     "+0.5 MOVE SPEED",                 1, 40,
      { { S(MoveSpeed), 0.5f, 1 } }, 1 },
    { U(ApRounds),     "AP ROUNDS", "+20% SHELL DAMAGE",               2, 55,
      { { S(Damage), 0, 1.20f } }, 1 },
    { U(HeavyShells),  "HEAVY SHELLS", "+12 DAMAGE, -10% SHELL SPEED", 2, 50,
      { { S(Damage), 12, 1 }, { S(ProjSpeed), 0, 0.90f } }, 2 },
    { U(Autoloader),   "AUTOLOADER", "-15% RELOAD TIME",               2, 55,
      { { S(ReloadTime), 0, 0.85f } }, 1 },
    { U(GreasedBreech), "GREASED BREECH", "-0.1s RELOAD",              0, 25,
      { { S(ReloadTime), -0.1f, 1 } }, 1 },
    { U(Plating),      "PLATING",   "+30 MAX HEALTH",                  1, 40,
      { { S(MaxHealth), 30, 1 } }, 1 },
    { U(Composite),    "COMPOSITE", "+20% MAX HP, -5% SPEED",          3, 70,
      { { S(MaxHealth), 0, 1.20f }, { S(MoveSpeed), 0, 0.95f } }, 2 },
    { U(Gyro),         "GYRO",      "+15% SHELL SPEED",                3, 60,
      { { S(ProjSpeed), 0, 1.15f } }, 1 },
    { U(ReactiveArmor), "REACTIVE ARMOR", "-10% DAMAGE TAKEN",         4, 85,
      { { S(DamageTaken), 0, 0.90f } }, 1 },
    { U(Overdrive),    "OVERDRIVE", "+8% SPEED, +10% DMG TAKEN",       4, 90,
      { { S(MoveSpeed), 0, 1.08f }, { S(DamageTaken), 0, 1.10f } }, 2 },
    { U(FieldKit),     "FIELD KIT", "+15 MAX HP, +0.15 SPEED",         0, 35,
      { { S(MaxHealth), 15, 1 }, { S(MoveSpeed), 0.15f, 1 } }, 2 },
    { U(Ricochet),     "RICOCHET",  "+1 WALL BOUNCE",                  2, 55,
      { { S(Bounces), 1, 1 } }, 1, ClassBouncy },
    { U(Superball),    "SUPERBALL", "+2 WALL BOUNCES, -15% DAMAGE",    4, 90,
      { { S(Bounces), 2, 1 }, { S(Damage), 0, 0.85f } }, 2, ClassBouncy },
    { U(NitroTank),    "NITRO TANK", "+0.5s BOOST FUEL",               2, 55,
      { { S(BoostFuel), 0.5f, 1 } }, 1 },
    { U(Afterburner),  "AFTERBURNER", "+20% BOOST SPEED",              3, 70,
      { { S(BoostSpeed), 0, 1.20f } }, 1 },
    { U(QuickPump),    "QUICK PUMP", "+0.3/s FUEL REGEN",              1, 40,
      { { S(BoostRegen), 0.3f, 1 } }, 1 },
    { U(PitCrew),      "PIT CREW",  "-0.4s REGEN DELAY",               0, 30,
      { { S(BoostRegenDelay), -0.4f, 1 } }, 1 },
    { U(FuelInjection), "FUEL INJECTION", "+25% FUEL REGEN",           2, 55,
      { { S(BoostRegen), 0, 1.25f } }, 1 },
    // class cards: no mods of their own -- the granted base upgrade carries
    // the stats, the card itself is the unlock marker in `owned`
    { U(SoldierClass), "SOLDIER CLASS", "UNLOCK SOLDIERS +RECRUITER",
      RarityClass, 80, {}, 0, ClassNone, ClassSoldier, U(Recruiter) },
    { U(BouncyClass),  "BOUNCY CLASS", "UNLOCK BOUNCES +RICOCHET",
      RarityClass, 80, {}, 0, ClassNone, ClassBouncy, U(Ricochet) },
    // SOLDIER family
    { U(Recruiter),    "RECRUITER", "-0.2s & -10% SPAWN TIME",         2, 55,
      { { S(SoldierCooldown), -0.2f, 0.90f } }, 1, ClassSoldier },
    { U(DoubleTime),   "DOUBLE TIME", "+15% SOLDIER SPEED",            1, 40,
      { { S(SoldierSpeed), 0, 1.15f } }, 1, ClassSoldier },
    { U(FlakVest),     "FLAK VEST", "+15 SOLDIER HEALTH",              1, 40,
      { { S(SoldierHealth), 15, 1 } }, 1, ClassSoldier },
    { U(HollowPoints), "HOLLOW POINTS", "+25% SOLDIER DAMAGE",         2, 55,
      { { S(SoldierDamage), 0, 1.25f } }, 1, ClassSoldier },
    { U(RapidFire),    "RAPID FIRE", "+15% SOLDIER FIRE RATE",         2, 55,
      { { S(SoldierFireRate), 0, 1.15f } }, 1, ClassSoldier },
    { U(Platoon),      "PLATOON",   "+1 MAX SOLDIERS",                 4, 95,
      { { S(SoldierMax), 1, 1 } }, 1, ClassSoldier },
    // BOUNCY family (per-ricochet multipliers, multiplicative only)
    { U(RubberShells), "RUBBER SHELLS", "+15% DAMAGE PER BOUNCE",      2, 55,
      { { S(BounceDamage), 0, 1.15f } }, 1, ClassBouncy },
    { U(Slingshot),    "SLINGSHOT", "+12% SPEED PER BOUNCE",           2, 55,
      { { S(BounceSpeed), 0, 1.12f } }, 1, ClassBouncy },
    // two more class cards
    { U(NecroClass),   "NECRO CLASS", "UNLOCK NECROMANCY +BONE FURNACE",
      RarityClass, 80, {}, 0, ClassNone, ClassNecro, U(BoneFurnace) },
    { U(RadarClass),   "RADAR CLASS", "UNLOCK RADAR ROCKETS +FAST LOCK",
      RarityClass, 80, {}, 0, ClassNone, ClassRadar, U(FastLock) },
    // NECROMANCER family
    { U(BoneFurnace),  "BONE FURNACE", "-20% SKULL COOLDOWN",          2, 55,
      { { S(SkullRate), 0, 0.80f } }, 1, ClassNecro },
    { U(CausticBrew),  "CAUSTIC BREW", "+3/s ACID DAMAGE",             2, 55,
      { { S(AcidDps), 3, 1 } }, 1, ClassNecro },
    { U(LingeringRot), "LINGERING ROT", "+1.5s ACID PUDDLE LIFE",      1, 40,
      { { S(AcidDuration), 1.5f, 1 } }, 1, ClassNecro },
    { U(DeepGrip),     "DEEP GRIP", "+0.6s POSSESSION",                3, 70,
      { { S(PossessDuration), 0.6f, 1 } }, 1, ClassNecro },
    { U(SoulLeech),    "SOUL LEECH", "+2/s POSSESSION DAMAGE",         3, 70,
      { { S(PossessDps), 2, 1 } }, 1, ClassNecro },
    // RADAR family
    { U(FastLock),     "FAST LOCK", "-20% RADAR LOCK TIME",            2, 55,
      { { S(RadarLock), 0, 0.80f } }, 1, ClassRadar },
    { U(WideScan),     "WIDE SCAN", "+20% RADAR RANGE",                2, 55,
      { { S(RadarRange), 0, 1.20f } }, 1, ClassRadar },
    { U(Payload),      "PAYLOAD", "+6 RADAR RING DAMAGE",              2, 55,
      { { S(RadarDamage), 6, 1 } }, 1, ClassRadar },
    { U(SharpPing),    "SHARP PING", "-0.1s RADAR LOCK TIME",          1, 40,
      { { S(RadarLock), -0.1f, 1 } }, 1, ClassRadar },
    { U(NestedArray),  "NESTED ARRAY", "+1 NESTED RADAR RING",         4, 95,
      { { S(RadarRings), 1, 1 } }, 1, ClassRadar },
    { U(FissionShells), "FISSION SHELLS", "+20% TWIN SPLIT PER BOUNCE",  3, 70,
      { { S(SplitChance), 0.20f, 1 } }, 1, ClassBouncy },
    { U(FragPack),     "FRAG PACK", "SOLDIERS LOB 2 GRENADES PER LIFE", 4, 95,
      { { S(GrenadeCount), 2, 1 } }, 1, ClassSoldier },
    // SHIELD PROFICIENCY: card + family (enum order is pool order)
    { U(ShieldClass),  "SHIELD CLASS", "UNLOCK THE BARRIER +WIDE BARRIER",
      RarityClass, 80, {}, 0, ClassNone, ClassShield, U(WideBarrier) },
    { U(WideBarrier),  "WIDE BARRIER", "+30% SHIELD WIDTH",           1, 30,
      { { S(ShieldWidth), 0, 1.3f } }, 1, ClassShield },
    { U(LongWatch),    "LONG WATCH", "+1.5S SHIELD DURATION",         2, 45,
      { { S(ShieldDuration), 1.5f, 1 } }, 1, ClassShield },
    { U(RapidRedeploy), "RAPID REDEPLOY", "-20% SHIELD COOLDOWN",     2, 45,
      { { S(ShieldCooldown), 0, 0.8f } }, 1, ClassShield },
    // UNIQUE rule-benders: gold band, 2% roll, one copy each, stackable
    // with other uniques (only the two contradictory ones exclude).
    { U(TripleDoctrine), "TRIPLE DOCTRINE",
      "3 CLASSES - NORMAL UPGRADES GONE",                             6, 120,
      {}, 0 },
    { U(PureArsenal),  "PURE ARSENAL",
      "NORMAL UPGRADES 2X - NO CLASSES",                              6, 120,
      {}, 0 },
    { U(Drunken),      "DRUNKEN",
      "SPEED AND CAMERA SWAY -20%..+30%",                             6, 120,
      {}, 0 },
    { U(Vampire),      "VAMPIRE",
      "10% LIFESTEAL - BURN IN SUNLIGHT",                             6, 120,
      {}, 0 },
    { U(Terrorist),    "TERRORIST",
      "DEATH BLAST - CLOSER IS DEADLIER",                             6, 120,
      {}, 0 },
    { U(Stealth),      "STEALTH",
      "WALLS HIDE YOU  -35% SPD -15% DMG",                            6, 120,
      {}, 0 },
    // MUTATIONS: light green class-pair fusions -- need both classes of one
    // of their pairs (kMutations), one copy, and ONE mutation per player EVER
    { U(Bubble),       "BUBBLE",
      "SHIELD IS A ONE-WAY TRAP DOME",                                7, 130,
      {}, 0 },
    { U(SpatialArmor), "SPATIAL ARMOR",
      "DEFLECTS HOME IN - 2X SPD+DMG",                                7, 130,
      {}, 0 },
    { U(HauntedSquad), "HAUNTED SQUAD",
      "DEAD SOLDIERS RISE AS HALF-GHOSTS",                            7, 130,
      {}, 0 },
    { U(BonePlatoon),  "BONE PLATOON",
      "SOLDIERS FIRE SKULLS, MIXED STATS",                            7, 130,
      {}, 0 },
    { U(Poltergeist),  "POLTERGEIST",
      "SKULLS PESTER FOREVER TIL SHOT",                               7, 130,
      {}, 0 },
    { U(AcidHound),    "ACID HOUND",
      "SKULLS BURST INTO A HUNTING BLOB",                             7, 130,
      {}, 0 },
    { U(RicochetDraft), "RICOCHET DRAFT",
      "1 SOLDIER - BOUNCES DRAFT TO 32",                              7, 130,
      {}, 0 },
    { U(Martyrdom),    "MARTYRDOM",
      "NO NADES - DEAD SOLDIERS EXPLODE",                             7, 130,
      {}, 0 },
    { U(RadarMines),   "RADAR MINES",
      "BOUNCES STAMP FADING RING MINES",                              7, 130,
      {}, 0 },
};
#undef S
#undef U

const char* ValidateUpgradePool()
{
    for (int i = 0; i < UpgradeCount; ++i)
    {
        const UpgradeType& u = kUpgradePool[i];
        if (int(u.id) != i)
            return "pool order does not match the UpgradeId enum";
        if (!u.name || !u.desc || !u.name[0])
            return "entry missing name/desc (enum grew past the pool?)";
        if (strlen(u.name) > 15 || strlen(u.desc) > 34)
            return "name/desc too long for the card/tooltip";
        bool isCard = u.classGrant != ClassNone;
        if (isCard != (u.rarity == RarityClass))
            return "class cards (and only cards) use the CLASS rarity";
        if (!isCard && (u.rarity < 0 || u.rarity > 4)
            && u.rarity != RarityUnique && u.rarity != RarityMutation)
            return "bad rarity";
        if (u.rarity == RarityMutation)
        {
            bool paired = false;
            for (const MutationPair& m : kMutations)
                paired |= m.id == UpgradeId(i);
            if (!paired)
                return "mutation missing from the kMutations pair table";
            if (u.modCount != 0)
                return "mutations are pure rule-benders, no stat mods";
        }
        if (u.baseCost <= 0 || u.modCount < 0 || u.modCount > MaxModsPerUpgrade)
            return "bad cost/modCount";
        if (u.classReq != ClassNone && u.classReq >= ClassCount)
            return "classReq is not a real class";
        if (isCard)
        {
            if (u.classGrant >= ClassCount) return "classGrant is not a real class";
            if (u.classReq != ClassNone) return "class card must not be class-locked";
            if (u.modCount != 0) return "class cards carry no mods of their own";
            if (u.grant == UpgradeId::Count) return "class card missing its base grant";
            const UpgradeType& gr = UpgradeDef(u.grant);
            if (gr.classGrant != ClassNone) return "grant target is itself a class card";
            if (gr.classReq != u.classGrant) return "grant target is not in the granted class";
        }
        else if (u.grant != UpgradeId::Count)
            return "only class cards grant extra upgrades";
    }
    return nullptr;
}

bool HasClass(const PlayerState& p, uint8_t cls)
{
    for (uint8_t t : p.owned)
        if (kUpgradePool[t].classGrant == cls)
            return true;
    return false;
}

bool HasAnyMutation(const PlayerState& p)
{
    for (uint8_t t : p.owned)
        if (kUpgradePool[t].rarity == RarityMutation)
            return true;
    return false;
}

bool MutationEligible(const PlayerState& p, UpgradeId id)
{
    if (HasAnyMutation(p))
        return false;             // ONE mutation per player, ever
    for (const MutationPair& m : kMutations)
        if (m.id == id && HasClass(p, m.a) && HasClass(p, m.b))
            return true;
    return false;
}

int CountClasses(const PlayerState& p)
{
    int n = 0;
    for (uint8_t t : p.owned)
        if (kUpgradePool[t].classGrant != ClassNone)
            ++n;
    return n;
}

// Heights sized so shadows are USABLE COVER for vampires: a box must
// throw a shade strip wider than a tank (shadow length ~= 1.9x height
// along the fixed low sun direction).
// The center is the TEMPLE (user-authored stepped pyramid, assets/Temple):
// its collision is a cross of three boxes -- the tiered body plus the two
// low staircase strips running N-S and E-W. Low boxes (< 2.0 tall) still
// block driving and rockets but are SKIPPED for sight lines / stealth LOS
// (you can see over a staircase).
// The two z-long side walls are GATEWAYS now (the user's arched entrance
// model): each is a pair of pillar boxes with a drivable 4.6-unit gap under
// a decorative arch (indices 5..8 -- the renderer draws one entrance model
// per pair and skips the boxes). Pillar numbers derive from the authored
// mesh at a 14-unit span: gap half 2.31, pillar center +-4.655, half 2.345,
// solid up to the arch spring line at 5.4.
// Hand-authored layout on the 120x120 field, 180-degree symmetric for
// fairness. Walls (7 x 1.0 x 3.0) come in DELIBERATE groups at the asked
// mix -- of 20 groups: 4 singles (20%), 8 doubles (40%), 4 triples (20%),
// 4 quads (20%). Doubles are Ls, straight runs and tight alleys; triples
// are U-bunkers and zigzags; quads are pinwheel courtyards and ramparts.
// Gateways sit on the east-west midline. QA contracts preserved: the
// z = -28 corridor stays clear across x in [-25, 5] (stealth speed lane)
// and the single at (14, 10) hosts the --spawns=wall stealth demo.
const Obstacle kObstacles[NumObstacles] = {
    {   0.0f,   0.0f, 4.0f,  4.0f,  4.7f }, // temple body (tiered pyramid)
    {   0.0f,   0.0f, 1.95f, 5.95f, 1.6f }, // temple stairs, N-S strip
    {   0.0f,   0.0f, 5.95f, 1.95f, 1.6f }, // temple stairs, E-W strip
    // gateways (indices 3..6): midline arches, 12.5u span, 4.1u gap
    { -30.0f,  4.155f, 0.5f, 2.095f, 4.2f },   // gate A, north pillar
    { -30.0f, -4.155f, 0.5f, 2.095f, 4.2f },   // gate A, south pillar
    {  30.0f, -4.155f, 0.5f, 2.095f, 4.2f },   // gate B, south pillar
    {  30.0f,  4.155f, 0.5f, 2.095f, 4.2f },   // gate B, north pillar
    // ---- singles (4 groups) ----
    {  14.0f,  10.0f, 3.5f, 0.5f, 3.0f },   // QA stealth-demo wall
    { -14.0f, -10.0f, 3.5f, 0.5f, 3.0f },
    {  44.0f,  22.0f, 0.5f, 3.5f, 3.0f },
    { -44.0f, -22.0f, 0.5f, 3.5f, 3.0f },
    // ---- doubles (8 groups) ----
    {  22.0f,  30.0f, 3.5f, 0.5f, 3.0f },   // L
    {  25.5f,  25.5f, 0.5f, 3.5f, 3.0f },
    { -22.0f, -30.0f, 3.5f, 0.5f, 3.0f },   // L (mirror)
    { -25.5f, -25.5f, 0.5f, 3.5f, 3.0f },
    { -23.5f,  16.0f, 3.5f, 0.5f, 3.0f },   // straight 14u run
    { -16.5f,  16.0f, 3.5f, 0.5f, 3.0f },
    {  23.5f, -16.0f, 3.5f, 0.5f, 3.0f },   // straight run (mirror)
    {  16.5f, -16.0f, 3.5f, 0.5f, 3.0f },
    { -38.0f,  34.0f, 0.5f, 3.5f, 3.0f },   // L
    { -34.5f,  37.5f, 3.5f, 0.5f, 3.0f },
    {  38.0f, -34.0f, 0.5f, 3.5f, 3.0f },   // L (mirror)
    {  34.5f, -37.5f, 3.5f, 0.5f, 3.0f },
    {  46.0f, -12.0f, 0.5f, 3.5f, 3.0f },   // tight alley pair
    {  41.0f, -12.0f, 0.5f, 3.5f, 3.0f },
    { -46.0f,  12.0f, 0.5f, 3.5f, 3.0f },   // alley (mirror)
    { -41.0f,  12.0f, 0.5f, 3.5f, 3.0f },
    // ---- triples (4 groups) ----
    {   0.0f, -40.0f, 3.5f, 0.5f, 3.0f },   // U-bunker, opening north
    {  -3.0f, -36.5f, 0.5f, 3.5f, 3.0f },
    {   3.0f, -36.5f, 0.5f, 3.5f, 3.0f },
    {   0.0f,  40.0f, 3.5f, 0.5f, 3.0f },   // U-bunker, opening south
    {   3.0f,  36.5f, 0.5f, 3.5f, 3.0f },
    {  -3.0f,  36.5f, 0.5f, 3.5f, 3.0f },
    { -52.0f,  -8.0f, 0.5f, 3.5f, 3.0f },   // zigzag
    { -48.5f,  -4.5f, 3.5f, 0.5f, 3.0f },
    { -45.0f,  -1.0f, 0.5f, 3.5f, 3.0f },
    {  52.0f,   8.0f, 0.5f, 3.5f, 3.0f },   // zigzag (mirror)
    {  48.5f,   4.5f, 3.5f, 0.5f, 3.0f },
    {  45.0f,   1.0f, 0.5f, 3.5f, 3.0f },
    // ---- quads (4 groups) ----
    {  16.0f, -28.0f, 3.5f, 0.5f, 3.0f },   // pinwheel courtyard
    {  24.0f, -32.0f, 0.5f, 3.5f, 3.0f },
    {  20.0f, -40.0f, 3.5f, 0.5f, 3.0f },
    {  12.0f, -36.0f, 0.5f, 3.5f, 3.0f },
    { -16.0f,  28.0f, 3.5f, 0.5f, 3.0f },   // pinwheel (mirror)
    { -24.0f,  32.0f, 0.5f, 3.5f, 3.0f },
    { -20.0f,  40.0f, 3.5f, 0.5f, 3.0f },
    { -12.0f,  36.0f, 0.5f, 3.5f, 3.0f },
    {  34.0f,  44.0f, 3.5f, 0.5f, 3.0f },   // rampart run + end hook
    {  41.0f,  44.0f, 3.5f, 0.5f, 3.0f },
    {  48.0f,  44.0f, 3.5f, 0.5f, 3.0f },
    {  51.5f,  40.5f, 0.5f, 3.5f, 3.0f },
    { -34.0f, -44.0f, 3.5f, 0.5f, 3.0f },   // rampart (mirror)
    { -41.0f, -44.0f, 3.5f, 0.5f, 3.0f },
    { -48.0f, -44.0f, 3.5f, 0.5f, 3.0f },
    { -51.5f, -40.5f, 0.5f, 3.5f, 3.0f },
};

float WrapAngle(float a)
{
    while (a > XM_PI) a -= XM_2PI;
    while (a < -XM_PI) a += XM_2PI;
    return a;
}

float MoveTowardsAngle(float current, float target, float maxDelta)
{
    float diff = WrapAngle(target - current);
    if (diff > maxDelta) diff = maxDelta;
    if (diff < -maxDelta) diff = -maxDelta;
    return WrapAngle(current + diff);
}

bool PointHitsObstacle(float x, float y, float z, float radius);

void GameState::GenerateSpawns()
{
    spawnCount = 0;
    if (gWallSpawns)
    {
        // QA: exactly two points flanking the long wall at (14, 10) --
        // guaranteed mutual occlusion for stealth testing
        spawnPX[0] = 14.0f; spawnPZ[0] = 13.6f;
        spawnPX[1] = 14.0f; spawnPZ[1] = 6.4f;
        spawnCount = 2;
        return;
    }
    for (int tries = 0; tries < 400 && spawnCount < MaxSpawns; ++tries)
    {
        float x = (float(NextRand() % 2000) / 1000.0f - 1.0f)
                * (ArenaHalf - 5.0f);
        float z = (float(NextRand() % 2000) / 1000.0f - 1.0f)
                * (ArenaHalf - 5.0f);
        if (PointHitsObstacle(x, 0.1f, z, TankRadius + 1.0f))
            continue;
        bool crowded = false;
        for (int s = 0; s < spawnCount && !crowded; ++s)
        {
            float dx = x - spawnPX[s], dz = z - spawnPZ[s];
            crowded = dx * dx + dz * dz < 8.0f * 8.0f;
        }
        if (crowded)
            continue;
        spawnPX[spawnCount] = x;
        spawnPZ[spawnCount] = z;
        ++spawnCount;
    }
    // pathological rolls: pad with the old ring so spawning never fails
    for (int id = 0; spawnCount < 4; ++id, ++spawnCount)
    {
        float ang = XM_2PI * float(id) / MaxPlayers + XM_PI / MaxPlayers;
        spawnPX[spawnCount] = sinf(ang) * (ArenaHalf - 6.0f);
        spawnPZ[spawnCount] = cosf(ang) * (ArenaHalf - 6.0f);
    }
}

void GameState::SpawnPoint(int id, float& x, float& z, float& yaw)
{
    if (spawnCount == 0)
        GenerateSpawns();
    int best = 0;
    float bestScore = -1.0f;
    for (int s = 0; s < spawnCount; ++s)
    {
        float nearest = 1e9f;
        for (int e = 0; e < MaxPlayers; ++e)
        {
            const PlayerState& o = players[e];
            if (e == id || !o.active || o.health <= 0)
                continue;
            float dx = spawnPX[s] - o.x, dz = spawnPZ[s] - o.z;
            nearest = std::min(nearest, dx * dx + dz * dz);
        }
        // no enemies alive: shuffle by rng so starts stay varied
        float score = nearest < 1e8f ? nearest
                                     : float(NextRand() % 1000);
        if (score > bestScore)
        {
            bestScore = score;
            best = s;
        }
    }
    x = spawnPX[best];
    z = spawnPZ[best];
    yaw = WrapAngle(atan2f(-x, -z));   // face the arena center
}

void LobbySpot(int id, float& x, float& z, float& yaw)
{
    x = (float(id) - (MaxLobbyPlayers - 1) * 0.5f) * 5.0f;
    z = -8.0f;
    yaw = XM_PI;                    // face the lobby camera (-Z)
}

uint32_t GameState::NextRand()
{
    rngState ^= rngState << 13;
    rngState ^= rngState >> 17;
    rngState ^= rngState << 5;
    return rngState;
}

void GameState::RecalcStats(int id)
{
    PlayerState& p = players[id];
    // stat -> summed amount and stat -> multiplied factor "dictionaries";
    // flat arrays because the key space is the Stat enum itself
    float addTotal[StatCount];
    float mulTotal[StatCount];
    for (int s = 0; s < StatCount; ++s)
    {
        addTotal[s] = 0.0f;
        mulTotal[s] = 1.0f;
    }
    bool arsenal = HasUpgrade(p, UpgradeId::PureArsenal);
    for (uint8_t type : p.owned)
    {
        const UpgradeType& u = kUpgradePool[type];
        // PURE ARSENAL: normal (class-free) upgrades land twice as hard --
        // amounts double, factors double their deviation from 1 (a 1.25x
        // becomes 1.5x, a 0.9x cooldown cut becomes 0.8x)
        float amp = (arsenal && u.rarity <= 4 && u.classReq == ClassNone
                     && u.classGrant == ClassNone) ? 2.0f : 1.0f;
        for (int m = 0; m < u.modCount; ++m)
        {
            addTotal[int(u.mods[m].stat)] += u.mods[m].amount * amp;
            mulTotal[int(u.mods[m].stat)] *=
                1.0f + (u.mods[m].factor - 1.0f) * amp;
        }
    }
    // order: base + additions first, then multiplications
    for (int s = 0; s < StatCount; ++s)
        p.stats[s] = (kBaseStats[s] + addTotal[s]) * mulTotal[s];

    // sanity floors so pathological stacks can't break the sim
    p.stats[int(Stat::MoveSpeed)] = std::max(0.5f, p.stats[int(Stat::MoveSpeed)]);
    p.stats[int(Stat::ReloadTime)] = std::max(0.15f, p.stats[int(Stat::ReloadTime)]);
    p.stats[int(Stat::MaxHealth)] = std::max(10.0f, p.stats[int(Stat::MaxHealth)]);
    p.stats[int(Stat::DamageTaken)] = std::max(0.1f, p.stats[int(Stat::DamageTaken)]);
    p.stats[int(Stat::Bounces)] =
        std::max(0.0f, p.stats[int(Stat::Bounces)] + float(gDebugBounces));
    p.stats[int(Stat::BoostSpeed)] = std::max(1.0f, p.stats[int(Stat::BoostSpeed)]);
    p.stats[int(Stat::BoostFuel)] = std::max(0.25f, p.stats[int(Stat::BoostFuel)]);
    p.stats[int(Stat::BoostRegen)] = std::max(0.05f, p.stats[int(Stat::BoostRegen)]);
    p.stats[int(Stat::BoostRegenDelay)] =
        std::max(0.0f, p.stats[int(Stat::BoostRegenDelay)]);
    p.stats[int(Stat::SoldierSpeed)] = std::max(0.5f, p.stats[int(Stat::SoldierSpeed)]);
    p.stats[int(Stat::SoldierDamage)] = std::max(1.0f, p.stats[int(Stat::SoldierDamage)]);
    p.stats[int(Stat::SoldierHealth)] = std::max(5.0f, p.stats[int(Stat::SoldierHealth)]);
    p.stats[int(Stat::SoldierFireRate)] = std::max(0.1f, p.stats[int(Stat::SoldierFireRate)]);
    p.stats[int(Stat::SoldierMax)] = std::max(1.0f, p.stats[int(Stat::SoldierMax)]);
    p.stats[int(Stat::SoldierCooldown)] = std::max(0.5f, p.stats[int(Stat::SoldierCooldown)]);
    p.stats[int(Stat::GrenadeCount)] = std::max(0.0f, p.stats[int(Stat::GrenadeCount)]);
    p.stats[int(Stat::GrenadeCooldown)] = std::max(1.0f, p.stats[int(Stat::GrenadeCooldown)]);
    p.stats[int(Stat::ShieldWidth)] = std::clamp(p.stats[int(Stat::ShieldWidth)], 1.5f, 14.0f);
    p.stats[int(Stat::ShieldDuration)] = std::clamp(p.stats[int(Stat::ShieldDuration)], 1.0f, 15.0f);
    p.stats[int(Stat::ShieldCooldown)] = std::max(3.0f, p.stats[int(Stat::ShieldCooldown)]);
    p.stats[int(Stat::SplitChance)] =
        std::clamp(p.stats[int(Stat::SplitChance)], 0.0f, 2.0f);
    if (HasUpgrade(p, UpgradeId::Stealth))   // fixed, not upgradeable away
        p.stats[int(Stat::Damage)] *= StealthDamageMul;
    p.stats[int(Stat::BounceDamage)] = std::max(0.25f, p.stats[int(Stat::BounceDamage)]);
    p.stats[int(Stat::BounceSpeed)] = std::max(0.25f, p.stats[int(Stat::BounceSpeed)]);
    p.stats[int(Stat::SplitChance)] = std::clamp(p.stats[int(Stat::SplitChance)],
                                                 0.0f, 1.0f);
    p.stats[int(Stat::SkullRate)] = std::max(0.8f, p.stats[int(Stat::SkullRate)]);
    p.stats[int(Stat::SkullDamage)] = std::max(1.0f, p.stats[int(Stat::SkullDamage)]);
    p.stats[int(Stat::AcidDps)] = std::max(0.5f, p.stats[int(Stat::AcidDps)]);
    p.stats[int(Stat::AcidDuration)] = std::max(0.5f, p.stats[int(Stat::AcidDuration)]);
    p.stats[int(Stat::PossessDps)] = std::max(0.5f, p.stats[int(Stat::PossessDps)]);
    p.stats[int(Stat::PossessDuration)] = std::max(0.3f, p.stats[int(Stat::PossessDuration)]);
    p.stats[int(Stat::RadarRange)] = std::max(1.0f, p.stats[int(Stat::RadarRange)]);
    p.stats[int(Stat::RadarLock)] = std::max(0.15f, p.stats[int(Stat::RadarLock)]);
    p.stats[int(Stat::RadarDamage)] = std::max(0.0f, p.stats[int(Stat::RadarDamage)]);
    p.stats[int(Stat::RadarRings)] = std::clamp(p.stats[int(Stat::RadarRings)],
                                                0.0f, float(MaxRadarExtra));
}

void GameState::SpawnPlayer(int id)
{
    PlayerState& p = players[id];
    char keepName[16];
    memcpy(keepName, p.name, sizeof(keepName));
    p = PlayerState{};
    memcpy(p.name, keepName, sizeof(p.name));
    p.active = true;
    SpawnPoint(id, p.x, p.z, p.hullYaw);
    p.turretYaw = p.hullYaw;
    RecalcStats(id);
    p.health = MaxHealthFor(p);
    p.boostFuel = p.stats[int(Stat::BoostFuel)];   // spawn with a full tank
    p.boostRegenWait = 0;
    p.nextOfferTick = tick + TickRate + uint32_t(id) * (TickRate / 4);
}

void GameState::StartMatch()
{
    GenerateSpawns();          // a fresh scattered set every match
    for (int id = 0; id < MaxPlayers; ++id)
        if (players[id].active)
            SpawnPlayer(id);   // full reset: money, upgrades, score, offers
    for (Projectile& pr : projectiles)
        pr.active = false;
    for (SoldierState& s : soldiers)
        s.active = false;
    for (SkullState& sk : skulls)
        sk.active = false;
    for (PuddleState& pu : puddles)
        pu.active = false;
    for (GhostState& gh : ghosts)
        gh.active = false;
    for (GrenadeState& gr : grenades)
        gr.active = false;
    phase = PhasePlaying;
    winner = 0xFF;
    matchEndTick = tick + uint32_t(matchMinutes) * 60u * TickRate;
}

void GameState::StartGathering(int playersWanted)
{
    targetPlayers = uint8_t(std::clamp(playersWanted, 2, MaxLobbyPlayers));
    phase = PhaseGathering;
}

void GameState::ToLobby()
{
    for (int id = 0; id < MaxPlayers; ++id)
        if (players[id].active)
        {
            players[id].ready = 0;
            players[id].health = std::max(1, players[id].health);
        }
    for (Projectile& pr : projectiles)
        pr.active = false;
    for (SoldierState& s : soldiers)
        s.active = false;
    for (SkullState& sk : skulls)
        sk.active = false;
    for (PuddleState& pu : puddles)
        pu.active = false;
    for (GhostState& gh : ghosts)
        gh.active = false;
    for (GrenadeState& gr : grenades)
        gr.active = false;
    phase = PhaseLobby;
}

bool GameState::AnyConsumed(int id) const
{
    for (const Offer& o : players[id].offers)
        if (o.active == OfferConsumed)
            return true;
    return false;
}

void GameState::InsertOffer(int id, const Offer& o)
{
    PlayerState& p = players[id];
    // conveyor: shift everything one slot toward the tail; overflow drops
    for (int s = NumOfferSlots - 1; s > 0; --s)
        p.offers[s] = p.offers[s - 1];
    p.offers[0] = o;
}

void GameState::GenerateOffer(int id)
{
    PlayerState& p = players[id];

    // Eligibility: class-locked upgrades need the class owned; class cards
    // are one-time and capped at kMaxClasses per player.
    auto eligible = [&](int i)
    {
        const UpgradeType& u = kUpgradePool[i];
        if (u.classGrant != ClassNone)
        {
            if (CountClasses(p) >= MaxClassesFor(p))
                return false;
            for (uint8_t t : p.owned)
                if (t == i)
                    return false;   // never re-offer an owned class card
            // PURE ARSENAL swore off classes entirely
            if (HasUpgrade(p, UpgradeId::PureArsenal))
                return false;
        }
        if (u.classReq != ClassNone && !HasClass(p, u.classReq))
            return false;
        if (u.rarity == RarityUnique)
        {
            for (uint8_t t : p.owned)
                if (t == i)
                    return false;   // one copy of each unique
            // the two contradictory rule-benders exclude each other
            if ((UpgradeId(i) == UpgradeId::TripleDoctrine
                 && HasUpgrade(p, UpgradeId::PureArsenal))
                || (UpgradeId(i) == UpgradeId::PureArsenal
                    && HasUpgrade(p, UpgradeId::TripleDoctrine)))
                return false;
        }
        // TRIPLE DOCTRINE: normal (class-free) upgrades are gone for good
        if (HasUpgrade(p, UpgradeId::TripleDoctrine) && u.rarity <= 4
            && u.classReq == ClassNone && u.classGrant == ClassNone)
            return false;
        // MUTATIONS: both classes of a pair, no mutation owned yet
        if (u.rarity == RarityMutation && !MutationEligible(p, UpgradeId(i)))
            return false;
        return true;
    };

    // rarity roll: common 35 / uncommon 23 / rare 17 / CLASS 10 / epic 8 /
    // legendary 5 / UNIQUE 2 -- gold rule-benders are the rarest sight
    uint32_t r = NextRand() % 100;
    int rarity = r < 32 ? 0
               : r < 55 ? 1
               : r < 72 ? 2
               : r < 82 ? RarityClass
               : r < 90 ? 3
               : r < 95 ? 4
               : r < 97 ? RarityUnique : RarityMutation;
    // uniform pick among eligible pool entries of that rarity; an empty class
    // band (both classes taken) degrades to rare, any other empty band falls
    // back to a uniform pick over everything eligible
    int candidates[UpgradeCount];
    int n = 0;
    auto gather = [&](int band)
    {
        n = 0;
        for (int i = 0; i < UpgradeCount; ++i)
            if (kUpgradePool[i].rarity == band && eligible(i))
                candidates[n++] = i;
    };
    gather(rarity);
    if (n == 0 && rarity == RarityClass)
        gather(2);
    if (n == 0)
        for (int i = 0; i < UpgradeCount; ++i)
            if (eligible(i))
                candidates[n++] = i;
    int type = (n > 0) ? candidates[NextRand() % n]
                       : int(NextRand() % UpgradeCount);

    // cost grows 25% per copy of the same type already owned (no level cap)
    int copies = 0;
    for (uint8_t t : p.owned)
        if (t == type) ++copies;
    float cost = float(kUpgradePool[type].baseCost);
    for (int c = 0; c < copies; ++c)
        cost *= 1.25f;

    Offer o;
    o.active = OfferActive;
    o.id = p.nextOfferId;
    p.nextOfferId = uint8_t(p.nextOfferId + 1);
    if (p.nextOfferId == 0) p.nextOfferId = 1;   // 0 = "none"
    o.type = uint8_t(type);
    o.cost = uint16_t(std::min(999.0f, cost));

    // Never shift the conveyor while a burn holds it: queue instead. Queued
    // offers keep their roll order and drain one by one afterwards.
    if (AnyConsumed(id))
        p.pendingOffers.push_back(o);
    else
        InsertOffer(id, o);
}

// The two contradictory rule-benders rewrite history when bought:
// PURE ARSENAL burns every owned class card and class-family upgrade;
// TRIPLE DOCTRINE burns every owned plain (class-free) upgrade. The host
// resyncs the whole owned list afterwards (append-only events cannot
// express removal), and every peer re-derives stats identically.
void StripForUnique(PlayerState& p, UpgradeId bought)
{
    if (bought != UpgradeId::PureArsenal
        && bought != UpgradeId::TripleDoctrine)
        return;
    bool arsenal = bought == UpgradeId::PureArsenal;
    for (size_t i = p.owned.size(); i-- > 0; )
    {
        const UpgradeType& u = kUpgradePool[p.owned[i]];
        bool isClassy = u.classGrant != ClassNone || u.classReq != ClassNone;
        // mutations live off class pairs: PURE ARSENAL burns them too
        isClassy |= u.rarity == RarityMutation;
        bool isNormal = u.rarity <= 4 && !isClassy;
        if ((arsenal && isClassy) || (!arsenal && isNormal))
            p.owned.erase(p.owned.begin() + i);
    }
}

// After any purchase, mutation offers still riding the conveyor may have
// become impossible (a mutation was bought, or PURE ARSENAL burned the
// classes). They vanish on the spot -- same spirit as stale class cards,
// but visible: an offer you can never buy is clutter, not choice.
static void SweepStaleMutationOffers(PlayerState& p)
{
    for (Offer& o : p.offers)
        if (o.active == OfferActive
            && kUpgradePool[o.type].rarity == RarityMutation
            && !MutationEligible(p, UpgradeId(o.type)))
            o = Offer{};
    for (size_t i = p.pendingOffers.size(); i-- > 0; )
        if (kUpgradePool[p.pendingOffers[i].type].rarity == RarityMutation
            && !MutationEligible(p, UpgradeId(p.pendingOffers[i].type)))
            p.pendingOffers.erase(p.pendingOffers.begin() + i);
}

bool GameState::TryPurchase(int id, int slot)
{
    if (id < 0 || id >= MaxPlayers || slot < 0 || slot >= NumOfferSlots)
        return false;
    PlayerState& p = players[id];
    if (!p.active || p.offers[slot].active != OfferActive)
        return false;
    const Offer& o = p.offers[slot];
    const UpgradeType& u = kUpgradePool[o.type];
    // A class card can go stale while sitting on the conveyor (its class
    // bought from another slot, or the cap reached), so eligibility is
    // re-validated at purchase time, not just at roll time.
    if (u.classGrant != ClassNone)
    {
        if (CountClasses(p) >= MaxClassesFor(p))
            return false;
        for (uint8_t t : p.owned)
            if (t == o.type)
                return false;
        if (HasUpgrade(p, UpgradeId::PureArsenal))
            return false;
    }
    if (u.rarity == RarityUnique)
    {
        for (uint8_t t : p.owned)
            if (t == o.type)
                return false;
        if ((UpgradeId(o.type) == UpgradeId::TripleDoctrine
             && HasUpgrade(p, UpgradeId::PureArsenal))
            || (UpgradeId(o.type) == UpgradeId::PureArsenal
                && HasUpgrade(p, UpgradeId::TripleDoctrine)))
            return false;
    }
    // MUTATIONS can also go stale on the conveyor (a class stripped, or a
    // mutation bought from another slot): re-validate here too
    if (u.rarity == RarityMutation && !MutationEligible(p, UpgradeId(o.type)))
        return false;
    if (p.money < o.cost)
        return false;
    p.money = uint16_t(p.money - o.cost);
    p.owned.push_back(o.type);
    // class cards also grant their base upgrade as a real owned copy (it
    // stacks and prices like any other; the host broadcasts it as a second
    // upgrade event so client owned lists stay identical)
    if (u.grant != UpgradeId::Count)
        p.owned.push_back(uint8_t(u.grant));
    StripForUnique(p, UpgradeId(o.type));
    SweepStaleMutationOffers(p);

    int prevMax = MaxHealthFor(p);
    RecalcStats(id);
    int newMax = MaxHealthFor(p);
    if (p.health > 0 && newMax > prevMax)
        p.health += newMax - prevMax;    // grant new max-HP immediately
    p.health = std::min(p.health, newMax);

    // Hold the slot while the burn animation plays; the conveyor compacts
    // only when it expires (see Tick), so the UI and array stay in agreement.
    p.offers[slot].active = OfferConsumed;
    p.offers[slot].consumedTick = tick + OfferBurnTicks;

    // Buying a class card burns every other class card on the conveyor and
    // drops any still queued: duplicates are dead after this purchase and at
    // the cap they all are. Consuming the slot plays the client's normal
    // burn animation for free.
    if (u.classGrant != ClassNone)
    {
        for (int s = 0; s < NumOfferSlots; ++s)
            if (s != slot && p.offers[s].active == OfferActive
                && kUpgradePool[p.offers[s].type].classGrant != ClassNone)
            {
                p.offers[s].active = OfferConsumed;
                p.offers[s].consumedTick = tick + OfferBurnTicks;
            }
        p.pendingOffers.erase(
            std::remove_if(p.pendingOffers.begin(), p.pendingOffers.end(),
                           [](const Offer& q)
                           { return kUpgradePool[q.type].classGrant != ClassNone; }),
            p.pendingOffers.end());
    }
    return true;
}

void GameState::RemovePlayer(int id)
{
    uint16_t score = players[id].score;
    players[id] = PlayerState{};
    (void)score;
}

XMFLOAT3 GameState::MuzzleWorld(const PlayerState& p) const
{
    // Yaw convention shared with rendering: forward(yaw) = (sin yaw, cos yaw);
    // a point (x,z) rotated by yaw lands at (x*cos + z*sin, z*cos - x*sin).
    float sh = sinf(p.hullYaw), ch = cosf(p.hullYaw);
    float st = sinf(p.turretYaw), ct = cosf(p.turretYaw);
    float pxr = turretPivot.x * ch + turretPivot.z * sh;   // pivot rotates with the hull
    float pzr = turretPivot.z * ch - turretPivot.x * sh;
    float mxr = muzzleOffset.x * ct + muzzleOffset.z * st;
    float mzr = muzzleOffset.z * ct - muzzleOffset.x * st;
    return XMFLOAT3(p.x + pxr + mxr,
                    turretPivot.y + muzzleOffset.y,
                    p.z + pzr + mzr);
}

static void CollideCircleObstacles(float& x, float& z, float radius)
{
    for (const Obstacle& o : kObstacles)
    {
        float ex = o.hx + radius, ez = o.hz + radius;
        float dx = x - o.cx, dz = z - o.cz;
        if (fabsf(dx) >= ex || fabsf(dz) >= ez)
            continue;
        float pushX = (dx > 0 ? ex - dx : -ex - dx);
        float pushZ = (dz > 0 ? ez - dz : -ez - dz);
        if (fabsf(pushX) < fabsf(pushZ)) x += pushX;
        else z += pushZ;
    }
}

bool PointHitsObstacle(float x, float y, float z, float radius)
{
    for (const Obstacle& o : kObstacles)
    {
        if (y > o.height)
            continue;
        if (fabsf(x - o.cx) < o.hx + radius && fabsf(z - o.cz) < o.hz + radius)
            return true;
    }
    return false;
}

// Walls and obstacle boxes ricochet while bounces remain, else the rocket is
// spent. Everything is axis-aligned, so a bounce mirrors one direction
// component: dir = (sin yaw, cos yaw), X-face hit -> yaw = -yaw, Z-face hit
// -> yaw = pi - yaw. Tank hits never bounce -- the host's hit loop detonates
// on contact. This is the ONLY copy of these rules: host projectiles and the
// client's provisional (predicted) rockets both step through here.
bool StepProjectile(Projectile& pr, float dt)
{
    pr.life -= dt;
    pr.x += sinf(pr.yaw) * pr.speed * dt;
    pr.z += cosf(pr.yaw) * pr.speed * dt;
    if (pr.life <= 0.0f)
        return false;

    const float r = ProjectileRadius;
    // BOUNCY class: every ricochet multiplies the rocket's damage and speed
    // by the owner's per-bounce stats (baked at fire time)
    auto onBounce = [&pr]()
    {
        pr.speed *= pr.bounceSpd;
        pr.damage = int(float(pr.damage) * pr.bounceDmg + 0.5f);
    };

    if (fabsf(pr.x) > ArenaHalf - r)
    {
        if (pr.bounces-- <= 0)
            return false;
        float lim = (pr.x > 0) ? ArenaHalf - r : r - ArenaHalf;
        pr.x = 2.0f * lim - pr.x;          // reflect off the face
        pr.yaw = WrapAngle(-pr.yaw);
        onBounce();
    }
    if (fabsf(pr.z) > ArenaHalf - r)
    {
        if (pr.bounces-- <= 0)
            return false;
        float lim = (pr.z > 0) ? ArenaHalf - r : r - ArenaHalf;
        pr.z = 2.0f * lim - pr.z;
        pr.yaw = WrapAngle(XM_PI - pr.yaw);
        onBounce();
    }
    for (const Obstacle& o : kObstacles)
    {
        if (pr.y > o.height)
            continue;
        float dx = pr.x - o.cx, dz = pr.z - o.cz;
        float ex = o.hx + r, ez = o.hz + r;
        if (fabsf(dx) >= ex || fabsf(dz) >= ez)
            continue;
        if (pr.bounces-- <= 0)
            return false;
        // resolve along the shallower penetration axis and mirror that
        // direction component
        float penX = ex - fabsf(dx);
        float penZ = ez - fabsf(dz);
        if (penX < penZ)
        {
            pr.x += (dx > 0 ? penX : -penX);
            pr.yaw = WrapAngle(-pr.yaw);
        }
        else
        {
            pr.z += (dz > 0 ? penZ : -penZ);
            pr.yaw = WrapAngle(XM_PI - pr.yaw);
        }
        onBounce();
        break;
    }
    return true;
}

// ------------------------------------------------------------- soldiers

static bool SegHitsBox(float x0, float z0, float x1, float z1,
                       const Obstacle& o, float inflate)
{
    // 2D slab test against the box expanded by `inflate`
    float hx = o.hx + inflate, hz = o.hz + inflate;
    float dx = x1 - x0, dz = z1 - z0;
    float tmin = 0.0f, tmax = 1.0f;
    if (fabsf(dx) < 1e-6f)
    {
        if (fabsf(x0 - o.cx) > hx) return false;
    }
    else
    {
        float inv = 1.0f / dx;
        float t1 = (o.cx - hx - x0) * inv, t2 = (o.cx + hx - x0) * inv;
        if (t1 > t2) std::swap(t1, t2);
        tmin = std::max(tmin, t1); tmax = std::min(tmax, t2);
        if (tmin > tmax) return false;
    }
    if (fabsf(dz) < 1e-6f)
    {
        if (fabsf(z0 - o.cz) > hz) return false;
    }
    else
    {
        float inv = 1.0f / dz;
        float t1 = (o.cz - hz - z0) * inv, t2 = (o.cz + hz - z0) * inv;
        if (t1 > t2) std::swap(t1, t2);
        tmin = std::max(tmin, t1); tmax = std::min(tmax, t2);
        if (tmin > tmax) return false;
    }
    return true;
}

bool SegmentBlockedByObstacles(float x0, float z0, float x1, float z1,
                               float inflate, float aboveHeight)
{
    for (const Obstacle& o : kObstacles)
    {
        if (aboveHeight > 0.0f && o.height < aboveHeight)
            continue;   // eyes see over low boxes (temple stairs)
        if (SegHitsBox(x0, z0, x1, z1, o, inflate))
            return true;
    }
    return false;
}

static void SpawnGhost(GameState& gs, int owner, float x, float z,
                       bool weak = false);

void GameState::ApplyDamage(int shooterId, int victimId, int rawDamage,
                            int hitMoney)
{
    PlayerState& t = players[victimId];
    if (!t.active || t.health <= 0)
        return;
    int dmg = int(rawDamage * t.stats[int(Stat::DamageTaken)] + 0.5f);
    t.health -= std::max(1, dmg);
    t.hitFlash = 0.35f;
    if (shooterId >= 0 && shooterId < MaxPlayers && shooterId != victimId
        && players[shooterId].active)
    {
        PlayerState& shooter = players[shooterId];
        shooter.money = uint16_t(std::min(999, shooter.money + hitMoney));
        // VAMPIRE: a tithe of every wound flows back as health
        if (HasUpgrade(shooter, UpgradeId::Vampire) && shooter.health > 0)
            shooter.health = std::min(
                MaxHealthFor(shooter),
                shooter.health
                    + std::max(1, int(dmg * VampireSteal(shooter) + 0.5f)));
        if (t.health <= 0)
        {
            ++shooter.score;
            shooter.money = uint16_t(std::min(999, shooter.money + 100));
            // NECROMANCER: a ghost rises from every tank the necromancer
            // kills and goes hunting for its next victim
            if (HasClass(shooter, ClassNecro))
                SpawnGhost(*this, shooterId, t.x, t.z);
            if (phase == PhaseOvertime)   // sudden death: first kill
            {
                phase = PhaseEnded;
                winner = uint8_t(shooterId);
                endedTick = tick;
            }
        }
    }
    if (t.health <= 0)
    {
        t.health = 0;
        t.respawnTimer = RespawnTime;
        t.possessTimer = 0;   // death exorcises
        t.dotAccum = 0;
        // TERRORIST: the corpse IS the weapon -- linear falloff blast with
        // posthumous kill credit (chained terrorists chain the recursion)
        if (HasUpgrade(t, UpgradeId::Terrorist))
        {
            float bx = t.x, bz = t.z;
            for (int id = 0; id < MaxPlayers; ++id)
            {
                if (id == victimId)
                    continue;
                PlayerState& e = players[id];
                if (!e.active || e.health <= 0)
                    continue;
                float dx = e.x - bx, dz = e.z - bz;
                float dist = sqrtf(dx * dx + dz * dz);
                if (dist >= TerroristRadius)
                    continue;
                // 100% of the victim's max HP point-blank; outside one
                // tank length the wave is weaker (60%) and slides gently
                // across a much larger radius
                float frac = dist <= TerroristPlateau
                    ? 1.0f
                    : TerroristFalloffTop
                      * (1.0f - (dist - TerroristPlateau)
                                / (TerroristRadius - TerroristPlateau));
                int blast = int(MaxHealthFor(e) * frac + 0.5f);
                if (blast > 0)
                    ApplyDamage(victimId, id, blast, 2);
            }
            for (SoldierState& s : soldiers)
            {
                if (!s.active || s.state >= SoldierDying
                    || s.owner == victimId)
                    continue;
                float dx = s.x - bx, dz = s.z - bz;
                float dist = sqrtf(dx * dx + dz * dz);
                if (dist >= TerroristRadius)
                    continue;
                float frac = dist <= TerroristPlateau
                    ? 1.0f
                    : TerroristFalloffTop
                      * (1.0f - (dist - TerroristPlateau)
                                / (TerroristRadius - TerroristPlateau));
                s.health -= 100.0f * frac;
                s.lastHitBy = uint8_t(victimId);
            }
        }
    }
}

// ------------------------------------------------- necromancer entities

static int NearestEnemyTank(const GameState& gs, int owner, float x, float z)
{
    int best = -1;
    float bestD2 = 1e18f;
    for (int id = 0; id < MaxPlayers; ++id)
    {
        const PlayerState& p = gs.players[id];
        if (!p.active || p.health <= 0 || id == owner)
            continue;
        float dx = p.x - x, dz = p.z - z;
        float d2 = dx * dx + dz * dz;
        if (d2 < bestD2) { bestD2 = d2; best = id; }
    }
    return best;
}

static void SpawnGhost(GameState& gs, int owner, float x, float z,
                       bool weak)
{
    for (GhostState& gh : gs.ghosts)
    {
        if (gh.active)
            continue;
        gh = GhostState{};
        gh.active = true;
        gh.owner = uint8_t(owner);
        gh.x = x;
        gh.z = z;
        gh.weak = weak;   // HAUNTED SQUAD: half possession bite
        return;
    }
}

static void SpawnPuddle(GameState& gs, int owner, float x, float z,
                        float lifeMul = 1.0f, float dpsMul = 1.0f)
{
    for (PuddleState& pu : gs.puddles)
    {
        if (pu.active)
            continue;
        const PlayerState& o = gs.players[owner];
        pu.active = true;
        pu.owner = uint8_t(owner);
        pu.x = std::clamp(x, -ArenaHalf + 0.5f, ArenaHalf - 0.5f);
        pu.z = std::clamp(z, -ArenaHalf + 0.5f, ArenaHalf - 0.5f);
        pu.life = o.stats[int(Stat::AcidDuration)] * lifeMul;
        pu.dps = o.stats[int(Stat::AcidDps)];
        // BONE PLATOON: acid bites with an equal blend of the two families
        if (HasUpgrade(o, UpgradeId::BonePlatoon))
            pu.dps = (o.stats[int(Stat::AcidDps)]
                      + o.stats[int(Stat::SoldierDamage)]) * 0.5f;
        pu.dps *= dpsMul;
        return;
    }
}

// ACID HOUND: the hunting blob a skull burst releases. Hops on the ground
// (the hop is theatre; the chase is the mechanic), steering toward the
// nearest enemy by bending its velocity with a constant-magnitude force --
// real inertia, so it overshoots corners and swings wide like a mad dog.
static void SpawnAcidBall(GameState& gs, int owner, float x, float z)
{
    for (AcidBallState& ab : gs.acidBalls)
    {
        if (ab.active)
            continue;
        ab = AcidBallState{};
        ab.active = true;
        ab.owner = uint8_t(owner);
        ab.x = std::clamp(x, -ArenaHalf + 0.6f, ArenaHalf - 0.6f);
        ab.z = std::clamp(z, -ArenaHalf + 0.6f, ArenaHalf - 0.6f);
        int tgt = NearestEnemyTank(gs, owner, x, z);
        if (tgt >= 0)
        {
            float dx = gs.players[tgt].x - x, dz = gs.players[tgt].z - z;
            float d = sqrtf(dx * dx + dz * dz);
            if (d > 1e-3f)
            {
                ab.vx = dx / d * AcidBallSpeed * 0.6f;
                ab.vz = dz / d * AcidBallSpeed * 0.6f;
            }
        }
        return;
    }
}

void TickAcidBall(GameState& gs, AcidBallState& ab)
{
    // steering: force vector toward the enemy, applied to the velocity
    int tgt = NearestEnemyTank(gs, ab.owner, ab.x, ab.z);
    if (tgt >= 0)
    {
        float dx = gs.players[tgt].x - ab.x, dz = gs.players[tgt].z - ab.z;
        float d = sqrtf(dx * dx + dz * dz);
        if (d > 1e-3f)
        {
            ab.vx += dx / d * AcidBallAccel * TickDt;
            ab.vz += dz / d * AcidBallAccel * TickDt;
        }
    }
    float sp = sqrtf(ab.vx * ab.vx + ab.vz * ab.vz);
    if (sp > AcidBallSpeed)
    {
        ab.vx *= AcidBallSpeed / sp;
        ab.vz *= AcidBallSpeed / sp;
    }
    float px = ab.x, pz = ab.z;
    ab.x += ab.vx * TickDt;
    ab.z += ab.vz * TickDt;
    // arena walls and obstacle boxes bounce the blob (not physics-accurate,
    // just a clean reflection -- the drama is in the chase)
    if (fabsf(ab.x) > ArenaHalf - 0.6f)
    {
        ab.x = std::clamp(ab.x, -ArenaHalf + 0.6f, ArenaHalf - 0.6f);
        ab.vx = -ab.vx;
    }
    if (fabsf(ab.z) > ArenaHalf - 0.6f)
    {
        ab.z = std::clamp(ab.z, -ArenaHalf + 0.6f, ArenaHalf - 0.6f);
        ab.vz = -ab.vz;
    }
    if (PointHitsObstacle(ab.x, 0.4f, ab.z, 0.5f))
    {
        bool hitX = PointHitsObstacle(ab.x, 0.4f, pz, 0.5f);
        bool hitZ = PointHitsObstacle(px, 0.4f, ab.z, 0.5f);
        if (hitX || !hitZ) ab.vx = -ab.vx;
        if (hitZ || !hitX) ab.vz = -ab.vz;
        ab.x = px;
        ab.z = pz;
    }
    // the hop: parabolic bob; every landing drips a WEAK puddle (20% life,
    // 20% dps -- 25x weaker) until the 40 hops are spent
    ab.hopT += TickDt;
    float u = std::min(ab.hopT / AcidBallHopTime, 1.0f);
    ab.y = AcidBallHopHeight * 4.0f * u * (1.0f - u);
    if (ab.hopT >= AcidBallHopTime)
    {
        ab.hopT -= AcidBallHopTime;
        SpawnPuddle(gs, ab.owner, ab.x, ab.z,
                    AcidBallPuddleMul, AcidBallPuddleMul);
        if (++ab.bounces >= AcidBallBounces)
            ab.active = false;
    }
}

// Skulls fly STRAIGHT at the nearest enemy (course refreshed every tick),
// slam into whatever is in the way -- walls included -- and burst into acid.
// ---------------------------------------------------------- unique helpers
// Sunlight: exact interval test of the point against every static shadow
// volume (obstacle boxes + the four arena walls) along the fixed sun
// direction. A point at (x,z) is shaded when some u in [0,1] puts
// (x,z) - u*L inside a footprint, L being the ground offset of the caster's
// top edge. Deterministic pure geometry: every peer agrees.
static bool ShadowedByBox(float px, float pz, float cx, float cz,
                          float hx, float hz, float height)
{
    // frame.sunDir (0.707, 0.460, 0.538), pre-normalized; rays travel -sun.
    // The low sun throws shadows ~1.9x the caster height (long vampire cover).
    const float Lx = -0.707f / 0.460f * height;
    const float Lz = -0.538f / 0.460f * height;
    float lo = 0.0f, hi = 1.0f;
    auto axis = [&](float p, float c, float h, float L)
    {
        if (fabsf(L) < 1e-6f)
            return p >= c - h && p <= c + h;
        float a = (p - (c + h)) / L;
        float b = (p - (c - h)) / L;
        if (a > b) std::swap(a, b);
        lo = std::max(lo, a);
        hi = std::min(hi, b);
        return lo <= hi;
    };
    return axis(px, cx, hx, Lx) && axis(pz, cz, hz, Lz);
}

bool InSunlight(float x, float z)
{
    for (const Obstacle& o : kObstacles)
        if (ShadowedByBox(x, z, o.cx, o.cz, o.hx, o.hz, o.height))
            return false;
    // TREES: no collision, no cover -- but every canopy is honest shade.
    // The canopy is approximated as a ground-up box (the bushy base reads
    // as shady anyway), scaled per tree.
    for (const TreeSpot& t : kTrees)
        if (ShadowedByBox(x, z, t.x, t.z, TreeShadeRadius * t.s,
                          TreeShadeRadius * t.s, TreeShadeHeight * t.s))
            return false;
    // the four arena walls (1.2 tall, thin bands on the boundary)
    if (ShadowedByBox(x, z, 0, ArenaHalf, ArenaHalf, 0.6f, 2.4f)) return false;
    if (ShadowedByBox(x, z, 0, -ArenaHalf, ArenaHalf, 0.6f, 2.4f)) return false;
    if (ShadowedByBox(x, z, ArenaHalf, 0, 0.6f, ArenaHalf, 2.4f)) return false;
    if (ShadowedByBox(x, z, -ArenaHalf, 0, 0.6f, ArenaHalf, 2.4f)) return false;
    return true;
}

// DRUNKEN sway: each 1.5 s segment hashes a fresh target factor; the value
// glides between segment targets with a smoothstep, so speed wanders
// -20%..+30% with no jolts. Pure (tick, id) function: prediction-safe.
float DrunkenFactor(uint32_t tick, int id)
{
    auto h01 = [](uint32_t seg, int pid)
    {
        uint32_t h = seg * 2654435761u ^ uint32_t(pid * 9176 + 41);
        h ^= h << 13; h ^= h >> 17; h ^= h << 5;
        return float(h & 0xFFFF) / 65535.0f;
    };
    uint32_t seg = tick / uint32_t(DrunkenSegTicks);
    float a = DrunkenMin + (DrunkenMax - DrunkenMin) * h01(seg, id);
    float b = DrunkenMin + (DrunkenMax - DrunkenMin) * h01(seg + 1, id);
    float u = float(tick % uint32_t(DrunkenSegTicks))
            / float(DrunkenSegTicks);
    u = u * u * (3.0f - 2.0f * u);
    return a + (b - a) * u;
}

void TickSkull(GameState& gs, SkullState& sk)
{
    sk.life -= TickDt;
    if (sk.life <= 0.0f)
    {
        sk.active = false;   // POLTERGEIST timeouts vanish dry: no puddle
        return;
    }
    // POLTERGEIST mutation: the skull is a permanent pest. It ricochets off
    // walls, BITES a tank and retreats to strike again, and NEVER drops
    // acid on its own -- only an enemy rocket ends it (and THAT drops the
    // puddle; handled in the projectile loop).
    bool polter = HasUpgrade(gs.players[sk.owner], UpgradeId::Poltergeist);
    sk.retreat = std::max(0.0f, sk.retreat - TickDt);
    int target = NearestEnemyTank(gs, sk.owner, sk.x, sk.z);
    if (target >= 0 && !(polter && sk.retreat > 0.0f))
    {
        const PlayerState& t = gs.players[target];
        sk.yaw = atan2f(t.x - sk.x, t.z - sk.z);
    }
    float mx = sinf(sk.yaw) * SkullSpeed * TickDt;
    float mz = cosf(sk.yaw) * SkullSpeed * TickDt;
    sk.x += mx;
    sk.z += mz;

    bool burst = false;
    if (fabsf(sk.x) > ArenaHalf - SkullRadius
        || fabsf(sk.z) > ArenaHalf - SkullRadius)
    {
        if (polter)                               // arena wall: ricochet
        {
            float dx = sinf(sk.yaw), dz = cosf(sk.yaw);
            if (fabsf(sk.x) > ArenaHalf - SkullRadius) dx = -dx;
            if (fabsf(sk.z) > ArenaHalf - SkullRadius) dz = -dz;
            sk.x = std::clamp(sk.x, -ArenaHalf + SkullRadius,
                              ArenaHalf - SkullRadius);
            sk.z = std::clamp(sk.z, -ArenaHalf + SkullRadius,
                              ArenaHalf - SkullRadius);
            sk.yaw = atan2f(dx, dz);
        }
        else
            burst = true;
    }
    if (!burst && PointHitsObstacle(sk.x, 0.5f, sk.z, SkullRadius))
    {
        if (polter)                               // obstacle box: ricochet
        {
            float px = sk.x - mx, pz = sk.z - mz;
            float dx = sinf(sk.yaw), dz = cosf(sk.yaw);
            bool hitX = PointHitsObstacle(sk.x, 0.5f, pz, SkullRadius);
            bool hitZ = PointHitsObstacle(px, 0.5f, sk.z, SkullRadius);
            if (hitX || !hitZ) dx = -dx;
            if (hitZ || !hitX) dz = -dz;
            sk.x = px;
            sk.z = pz;
            sk.yaw = atan2f(dx, dz);
        }
        else
            burst = true;
    }
    if (!burst)
        for (int id = 0; id < MaxPlayers && !burst; ++id)
        {
            const PlayerState& p = gs.players[id];
            if (!p.active || p.health <= 0 || id == sk.owner)
                continue;
            float dx = sk.x - p.x, dz = sk.z - p.z;
            float r = TankRadius + SkullRadius;
            if (dx * dx + dz * dz < r * r)         // enemy tank: BITE
            {
                gs.ApplyDamage(sk.owner, id, int(sk.dmg + 0.5f), 2);
                if (polter)
                {
                    // strike, disengage, come around for another pass
                    float d = std::max(1e-3f, sqrtf(dx * dx + dz * dz));
                    sk.yaw = atan2f(dx / d, dz / d);
                    sk.x = p.x + dx / d * (r + 0.15f);
                    sk.z = p.z + dz / d * (r + 0.15f);
                    sk.retreat = PoltergeistRetreat;
                }
                else
                    burst = true;
            }
        }
    if (!burst)
        for (SoldierState& s : gs.soldiers)
        {
            if (!s.active || s.state >= SoldierDying || s.owner == sk.owner)
                continue;
            float dx = sk.x - s.x, dz = sk.z - s.z;
            float r = SoldierRadius + SkullRadius;
            if (dx * dx + dz * dz < r * r)
            {
                s.health -= sk.dmg;
                s.lastHitBy = sk.owner;
                if (polter)
                {
                    float d = std::max(1e-3f, sqrtf(dx * dx + dz * dz));
                    sk.yaw = atan2f(dx / d, dz / d);
                    sk.retreat = PoltergeistRetreat;
                }
                else
                    burst = true;
                break;
            }
        }
    if (burst)
    {
        // ACID HOUND mutation: the burst releases the hunting blob instead
        // of dropping its acid on the spot
        if (HasUpgrade(gs.players[sk.owner], UpgradeId::AcidHound))
            SpawnAcidBall(gs, sk.owner, sk.x, sk.z);
        else
            SpawnPuddle(gs, sk.owner, sk.x, sk.z);
        sk.active = false;
    }
}

static void TickPuddle(GameState& gs, PuddleState& pu)
{
    pu.life -= TickDt;
    if (pu.life <= 0.0f)
    {
        pu.active = false;
        return;
    }
    for (int id = 0; id < MaxPlayers; ++id)
    {
        PlayerState& t = gs.players[id];
        if (!t.active || t.health <= 0 || id == pu.owner)
            continue;
        float dx = t.x - pu.x, dz = t.z - pu.z;
        float r = PuddleRadius + TankRadius * 0.5f;
        if (dx * dx + dz * dz < r * r)
        {
            // fractional DoT accumulates; whole points land via ApplyDamage
            t.dotAccum += pu.dps * TickDt;
            while (t.dotAccum >= 1.0f && t.health > 0)
            {
                t.dotAccum -= 1.0f;
                gs.ApplyDamage(pu.owner, id, 1, 0);
            }
        }
    }
    for (SoldierState& s : gs.soldiers)
    {
        if (!s.active || s.state >= SoldierDying || s.owner == pu.owner)
            continue;
        float dx = s.x - pu.x, dz = s.z - pu.z;
        float r = PuddleRadius + SoldierRadius;
        if (dx * dx + dz * dz < r * r)
        {
            s.health -= pu.dps * TickDt;
            s.lastHitBy = pu.owner;
        }
    }
}

// Ghosts spiral inward around their victim (through walls -- they are
// ghosts) and possess on contact: the tank takes PossessDps for
// PossessDuration, drives itself with deterministic chaos and cannot fire.
static void TickGhost(GameState& gs, GhostState& gh)
{
    gh.life -= TickDt;
    if (gh.life <= 0.0f)
    {
        gh.active = false;   // the window to escape it
        return;
    }
    if (gh.targetId >= MaxPlayers || !gs.players[gh.targetId].active
        || gs.players[gh.targetId].health <= 0)
    {
        int t = NearestEnemyTank(gs, gh.owner, gh.x, gh.z);
        if (t < 0)
        {
            gh.angle += 2.0f * TickDt;   // idle bob, wait for prey
            return;
        }
        gh.targetId = uint8_t(t);
        const PlayerState& p = gs.players[t];
        gh.orbitR = std::max(GhostOrbitStart * 0.6f,
                             sqrtf((p.x - gh.x) * (p.x - gh.x)
                                   + (p.z - gh.z) * (p.z - gh.z)));
        gh.angle = atan2f(gh.x - p.x, gh.z - p.z);
    }
    PlayerState& t = gs.players[gh.targetId];
    gh.angle += (GhostOrbitSpeed / std::max(1.0f, gh.orbitR)) * TickDt;
    gh.orbitR -= GhostCloseRate * TickDt;
    gh.x = t.x + sinf(gh.angle) * gh.orbitR;
    gh.z = t.z + cosf(gh.angle) * gh.orbitR;
    if (gh.orbitR <= TankRadius + 0.3f)
    {
        // POSSESSION: bake the necromancer's stats into the victim.
        // HAUNTED SQUAD half-ghosts possess at half strength.
        const PlayerState& o = gs.players[gh.owner];
        float mul = gh.weak ? 0.5f : 1.0f;
        t.possessTimer = o.stats[int(Stat::PossessDuration)] * mul;
        t.possessDps = o.stats[int(Stat::PossessDps)] * mul;
        t.possessedBy = gh.owner;
        gh.active = false;
    }
}

// ------------------------------------------------------------ radar tree

int RadarTreeLayout(float rootR, int extra, float yaw,
                    float* ox, float* oz, float* radius, int* depth, int cap)
{
    int n = std::min(1 + extra, cap);
    int parent[MaxRadarNodes];
    int kids[MaxRadarNodes]{};
    int slotUsed[MaxRadarNodes]{};
    ox[0] = 0; oz[0] = 0; radius[0] = rootR; depth[0] = 0; parent[0] = -1;
    for (int i = 1; i < n; ++i)
    {
        parent[i] = (i - 1) / 3;   // breadth-first, three slots per parent
        ++kids[parent[i]];
    }
    for (int i = 1; i < n; ++i)
    {
        int p = parent[i];
        int slot = slotUsed[p]++;
        float pr = radius[p];
        float dist, ang;
        if (kids[p] == 1)          // an only child sits centered
        {
            dist = 0.0f;
            ang = 0.0f;
        }
        else if (kids[p] == 2)     // two sit opposite each other
        {
            dist = pr * 0.5f;
            ang = yaw + float(slot) * DirectX::XM_PI;
        }
        else                       // three pack as a triangle
        {
            dist = pr * 0.5f;
            ang = yaw + DirectX::XM_PI * 0.5f
                + float(slot) * (DirectX::XM_2PI / 3.0f);
        }
        ox[i] = ox[p] + sinf(ang) * dist;
        oz[i] = oz[p] + cosf(ang) * dist;
        radius[i] = pr * 0.5f;
        depth[i] = depth[p] + 1;
    }
    return n;
}

// Detonate the ENTIRE ring tree at the rocket's position. Victims take
// (rocket damage + PAYLOAD) * (1/2)^depth for every circle that contains
// them. Called on a full lock charge -- and on DIRECT BODY HITS, so the
// class needs less precision: the rocket connecting means the ranges blow.
void DetonateRadar(GameState& gs, Projectile& pr)
{
    float rootDmg = float(pr.damage) + pr.radarDamage;
    float ox[MaxRadarNodes], oz[MaxRadarNodes], rad[MaxRadarNodes];
    int dep[MaxRadarNodes];
    int n = RadarTreeLayout(pr.radarRange, pr.radarRings, pr.yaw,
                            ox, oz, rad, dep, MaxRadarNodes);
    for (int id = 0; id < MaxPlayers; ++id)
    {
        const PlayerState& p = gs.players[id];
        if (!p.active || p.health <= 0 || id == pr.owner)
            continue;
        float sum = 0.0f;
        for (int k = 0; k < n; ++k)
        {
            float dx = p.x - (pr.x + ox[k]), dz = p.z - (pr.z + oz[k]);
            float rr = rad[k] + TankRadius * 0.5f;
            if (dx * dx + dz * dz < rr * rr)
                sum += rootDmg / float(1 << dep[k]);
        }
        if (sum > 0.0f)
            gs.ApplyDamage(pr.owner, id, int(sum + 0.5f), 5);
    }
    for (SoldierState& s : gs.soldiers)
    {
        if (!s.active || s.state >= SoldierDying || s.owner == pr.owner)
            continue;
        float sum = 0.0f;
        for (int k = 0; k < n; ++k)
        {
            float dx = s.x - (pr.x + ox[k]), dz = s.z - (pr.z + oz[k]);
            float rr = rad[k] + SoldierRadius;
            if (dx * dx + dz * dz < rr * rr)
                sum += rootDmg / float(1 << dep[k]);
        }
        if (sum > 0.0f)
        {
            s.health -= sum;
            s.lastHitBy = pr.owner;
        }
    }
    // POLTERGEIST skulls caught in any ring are shot down by the blast --
    // the radar answer to the pest (and THAT drops their puddle)
    for (SkullState& sk : gs.skulls)
    {
        if (!sk.active || sk.owner == pr.owner
            || !HasUpgrade(gs.players[sk.owner], UpgradeId::Poltergeist))
            continue;
        for (int k = 0; k < n; ++k)
        {
            float dx = sk.x - (pr.x + ox[k]), dz = sk.z - (pr.z + oz[k]);
            float rr = rad[k] + SkullRadius;
            if (dx * dx + dz * dz < rr * rr)
            {
                SpawnPuddle(gs, sk.owner, sk.x, sk.z);
                sk.active = false;
                break;
            }
        }
    }
    pr.active = false;   // the detonation consumes the rocket
}

// RADAR MINE: a stamped root ring. Fades over RadarMineLife; while an
// enemy body sits inside, the owner's lock charges; a full charge blasts
// EVERYONE inside for rocket-grade ring damage (Damage + PAYLOAD). Radius,
// lock and damage are read from the owner's stats LIVE -- both sides of
// the wire derive identical values from the replicated upgrades.
void TickRadarMine(GameState& gs, RadarMineState& rm)
{
    rm.life -= TickDt;
    const PlayerState& own = gs.players[rm.owner];
    if (rm.life <= 0.0f || !own.active)
    {
        rm.active = false;
        return;
    }
    float R = own.stats[int(Stat::RadarRange)];
    bool inside = false;
    for (int id = 0; id < MaxPlayers && !inside; ++id)
    {
        const PlayerState& p = gs.players[id];
        if (!p.active || p.health <= 0 || id == rm.owner)
            continue;
        float dx = p.x - rm.x, dz = p.z - rm.z;
        float rr = R + TankRadius * 0.5f;
        inside = dx * dx + dz * dz < rr * rr;
    }
    for (const SoldierState& s : gs.soldiers)
    {
        if (inside)
            break;
        if (!s.active || s.state >= SoldierDying || s.owner == rm.owner)
            continue;
        float dx = s.x - rm.x, dz = s.z - rm.z;
        float rr = R + SoldierRadius;
        inside = dx * dx + dz * dz < rr * rr;
    }
    if (!inside)
    {
        rm.lock = 0.0f;
        return;
    }
    rm.lock += TickDt;
    if (rm.lock < own.stats[int(Stat::RadarLock)])
        return;
    int dmg = int(own.stats[int(Stat::Damage)]
                  + own.stats[int(Stat::RadarDamage)] + 0.5f);
    for (int id = 0; id < MaxPlayers; ++id)
    {
        const PlayerState& p = gs.players[id];
        if (!p.active || p.health <= 0 || id == rm.owner)
            continue;
        float dx = p.x - rm.x, dz = p.z - rm.z;
        float rr = R + TankRadius * 0.5f;
        if (dx * dx + dz * dz < rr * rr)
            gs.ApplyDamage(rm.owner, id, dmg, 5);
    }
    for (SoldierState& s : gs.soldiers)
    {
        if (!s.active || s.state >= SoldierDying || s.owner == rm.owner)
            continue;
        float dx = s.x - rm.x, dz = s.z - rm.z;
        float rr = R + SoldierRadius;
        if (dx * dx + dz * dz < rr * rr)
        {
            s.health -= float(dmg);
            s.lastHitBy = rm.owner;
        }
    }
    for (SkullState& sk : gs.skulls)   // poltergeist pests pop here too
    {
        if (!sk.active || sk.owner == rm.owner
            || !HasUpgrade(gs.players[sk.owner], UpgradeId::Poltergeist))
            continue;
        float dx = sk.x - rm.x, dz = sk.z - rm.z;
        float rr = R + SkullRadius;
        if (dx * dx + dz * dz < rr * rr)
        {
            SpawnPuddle(gs, sk.owner, sk.x, sk.z);
            sk.active = false;
        }
    }
    // detonated: leave `life` untouched -- the host's event emitter tells
    // an early pop (life still high) apart from a silent timeout
    rm.active = false;
}

// One lock decides (the root circle contains every child, so it always
// charges first); a full charge detonates the ENTIRE tree.
void TickRadar(GameState& gs, Projectile& pr)
{
    bool inside = false;
    float rootRR = pr.radarRange + TankRadius * 0.5f;
    for (int id = 0; id < MaxPlayers && !inside; ++id)
    {
        const PlayerState& p = gs.players[id];
        if (!p.active || p.health <= 0 || id == pr.owner)
            continue;
        float dx = p.x - pr.x, dz = p.z - pr.z;
        inside = dx * dx + dz * dz < rootRR * rootRR;
    }
    for (const SoldierState& s : gs.soldiers)
    {
        if (inside)
            break;
        if (!s.active || s.state >= SoldierDying || s.owner == pr.owner)
            continue;
        float dx = s.x - pr.x, dz = s.z - pr.z;
        float rr = pr.radarRange + SoldierRadius;
        inside = dx * dx + dz * dz < rr * rr;
    }
    // POLTERGEIST skulls count as radar contacts too (they can be killed,
    // so the rings are allowed to see them)
    for (const SkullState& sk : gs.skulls)
    {
        if (inside)
            break;
        if (!sk.active || sk.owner == pr.owner
            || !HasUpgrade(gs.players[sk.owner], UpgradeId::Poltergeist))
            continue;
        float dx = sk.x - pr.x, dz = sk.z - pr.z;
        float rr = pr.radarRange + SkullRadius;
        inside = dx * dx + dz * dz < rr * rr;
    }
    if (!inside)
    {
        pr.radarLock = 0.0f;
        pr.radarLockFrac = 0.0f;
        return;
    }
    pr.radarLock += TickDt;
    pr.radarLockFrac =
        std::clamp(pr.radarLock / std::max(0.05f, pr.radarLockNeed), 0.0f, 1.0f);
    if (pr.radarLock < pr.radarLockNeed)
        return;
    DetonateRadar(gs, pr);
}

// Candidate hiding spots: points floated off every obstacle face (three per
// face). A spot is COVER from an enemy when the spot->enemy segment crosses
// an obstacle -- i.e. the wall eats the line of sight.
struct CoverSpot { float x, z; bool wide; };
constexpr int MaxCoverSpots = 700;   // 55 obstacles x 12 spots fits
static int GatherCoverSpots(CoverSpot* out, int cap)
{
    int n = 0;
    const float off = SoldierRadius + 0.45f;
    for (const Obstacle& o : kObstacles)
    {
        float xs[3] = { o.cx - o.hx * 0.6f, o.cx, o.cx + o.hx * 0.6f };
        float zs[3] = { o.cz - o.hz * 0.6f, o.cz, o.cz + o.hz * 0.6f };
        for (int i = 0; i < 3 && n + 4 <= cap; ++i)
        {
            out[n++] = { xs[i], o.cz - o.hz - off, false };
            out[n++] = { xs[i], o.cz + o.hz + off, false };
            out[n++] = { o.cx - o.hx - off, zs[i], false };
            out[n++] = { o.cx + o.hx + off, zs[i], false };
        }
        // corners: angle cover AND the natural detour waypoints -- a spot
        // pinned to a face is often only reachable by rounding a corner first
        if (n + 4 <= cap)
        {
            out[n++] = { o.cx - o.hx - off, o.cz - o.hz - off, false };
            out[n++] = { o.cx + o.hx + off, o.cz - o.hz - off, false };
            out[n++] = { o.cx - o.hx - off, o.cz + o.hz + off, false };
            out[n++] = { o.cx + o.hx + off, o.cz + o.hz + off, false };
        }
        // WIDE corners: a body length further out. Peek points and stepping
        // stones only, never hiding spots -- standing wide of the wall is
        // exactly the exposed, punishable moment of the peek.
        const float wide = off + 1.3f;
        if (n + 4 <= cap)
        {
            out[n++] = { o.cx - o.hx - wide, o.cz - o.hz - wide, true };
            out[n++] = { o.cx + o.hx + wide, o.cz - o.hz - wide, true };
            out[n++] = { o.cx - o.hx - wide, o.cz + o.hz + wide, true };
            out[n++] = { o.cx + o.hx + wide, o.cz + o.hz + wide, true };
        }
    }
    return n;
}

// SHIELD deflection: a raised barrier catches enemy rockets, hands them a
// bonus ricochet, flips their allegiance (they can never hurt the shieldman
// again -- but hurt everyone else, shooter included) and paints them orange.
// The owner's own rockets pass right through. Robustness: the face uses the
// FRESHEST aim (not the jitter-lagged turret), and the test is the tick's
// SEGMENT against the face plane -- fast shells cannot tunnel between two
// positions, and the shell's own radius counts at the edges.
// Raycast a movement segment (bubble-relative frame) against the dome wall:
// first t in [0,1] where |P0 + t*(P1-P0)| reaches radius r going OUT. The
// exit root also catches a full pass-through in one tick (enter + exit).
static bool SegmentExitsCircle(float p0x, float p0z, float p1x, float p1z,
                               float r, float& t)
{
    float dx = p1x - p0x, dz = p1z - p0z;
    float a = dx * dx + dz * dz;
    if (a < 1e-8f)
        return false;
    float b = 2.0f * (p0x * dx + p0z * dz);
    float c = p0x * p0x + p0z * p0z - r * r;
    float disc = b * b - 4.0f * a * c;
    if (disc < 0.0f)
        return false;
    t = (-b + sqrtf(disc)) / (2.0f * a);   // the OUTBOUND crossing
    return t >= 0.0f && t <= 1.0f;
}

float BubbleRadiusFor(const PlayerState& p)
{
    return BubbleRadius * p.stats[int(Stat::ShieldWidth)]
         / kBaseStats[int(Stat::ShieldWidth)];
}

void BubbleCenter(const PlayerState& p, float& cx, float& cz)
{
    float d = BubbleRadiusFor(p) + BubbleGap;
    cx = p.x + sinf(p.shieldAimYaw) * d;
    cz = p.z + cosf(p.shieldAimYaw) * d;
}

bool ShieldDeflectStep(GameState& gs, Projectile& pr)
{
    for (int id = 0; id < MaxPlayers; ++id)
    {
        PlayerState& sp = gs.players[id];
        if (!sp.active || sp.health <= 0 || sp.shieldTimer <= 0.0f)
            continue;
        // ---- BUBBLE mutation: a one-way trap dome instead of a face ----
        // Rockets pass IN freely (any owner, the dome owner's included);
        // any rocket crossing the wall from the INSIDE ricochets back,
        // gains bounces, and (enemy shells) flips to the dome owner.
        if (HasUpgrade(sp, UpgradeId::Bubble))
        {
            float bcx, bcz;
            BubbleCenter(sp, bcx, bcz);
            // continuous collision vs the MOVING dome: compare the rocket's
            // previous position against the dome's PREVIOUS center and the
            // current against the current -- crossings register even when a
            // boosting owner drags the wall several units in one tick
            float pcx = sp.bubblePrevValid ? sp.bubblePrevCx : bcx;
            float pcz = sp.bubblePrevValid ? sp.bubblePrevCz : bcz;
            float R = BubbleRadiusFor(sp);
            float step = pr.speed * TickDt;
            float vx = sinf(pr.yaw) * step, vz = cosf(pr.yaw) * step;
            float relPX = (pr.x - vx) - pcx, relPZ = (pr.z - vz) - pcz;
            float relNX = pr.x - bcx, relNZ = pr.z - bcz;
            float wall = R - ProjectileRadius;
            float tHit;
            if (!SegmentExitsCircle(relPX, relPZ, relNX, relNZ, wall, tHit))
                continue;                 // no outbound wall crossing
            // the RAYCAST HIT POINT (bubble frame) is both the reflection
            // anchor and where the rocket lands on xz
            float hx = relPX + (relNX - relPX) * tHit;
            float hz = relPZ + (relNZ - relPZ) * tHit;
            float hl = std::max(1e-4f, sqrtf(hx * hx + hz * hz));
            float nx = hx / hl, nz = hz / hl;
            // reflect the RELATIVE motion direction (bubble frame), so the
            // bounce is sound regardless of the dome's own velocity
            float rdx = relNX - relPX, rdz = relNZ - relPZ;
            float rl = sqrtf(rdx * rdx + rdz * rdz);
            if (rl < 1e-5f) { rdx = nx; rdz = nz; rl = 1.0f; }
            rdx /= rl; rdz /= rl;
            float rn = rdx * nx + rdz * nz;
            rdx -= 2.0f * rn * nx;
            rdz -= 2.0f * rn * nz;
            pr.yaw = atan2f(rdx, rdz);
            pr.x = bcx + nx * (wall - 0.05f);   // at the hit point, inside
            pr.z = bcz + nz * (wall - 0.05f);
            pr.bounces += BubbleBounceGain;
            if (pr.owner != id)
            {
                pr.owner = uint8_t(id);   // now it fights for the trapper
                pr.deflected = 1;         // orange from here on
            }
            return true;
        }
        if (id == pr.owner)
            continue;                     // flat face: own rockets pass
        float fx = sinf(sp.shieldAimYaw), fz = cosf(sp.shieldAimYaw);
        float cxp = sp.x + fx * ShieldDist;
        float czp = sp.z + fz * ShieldDist;
        float step = pr.speed * TickDt;
        float vx = sinf(pr.yaw) * step, vz = cosf(pr.yaw) * step;
        float vn = vx * fx + vz * fz;
        if (vn >= 0.0f)                   // outbound or parallel
            continue;
        float lonNow = (pr.x - cxp) * fx + (pr.z - czp) * fz;
        float lonPrev = lonNow - vn;      // where this tick started
        if (lonNow > 0.30f || lonPrev < -0.30f)
            continue;                     // never met the face this tick
        float latNow = (pr.x - cxp) * fz - (pr.z - czp) * fx;
        float latPrev = latNow - (vx * fz - vz * fx);
        float t = (lonPrev - lonNow) > 1e-6f
            ? std::clamp(lonPrev / (lonPrev - lonNow), 0.0f, 1.0f)
            : 0.0f;
        float lat = latPrev + (latNow - latPrev) * t;
        float halfW = sp.stats[int(Stat::ShieldWidth)] * 0.5f
                    + ProjectileRadius + 0.15f;
        if (fabsf(lat) > halfW)
            continue;
        float rvx = sinf(pr.yaw), rvz = cosf(pr.yaw);
        float rn = rvx * fx + rvz * fz;
        rvx -= 2.0f * rn * fx;
        rvz -= 2.0f * rn * fz;
        pr.yaw = atan2f(rvx, rvz);
        // re-emerge in front of the face at the crossing point
        pr.x = cxp + fz * lat + fx * 0.4f;
        pr.z = czp - fx * lat + fz * 0.4f;
        pr.bounces += 1;                  // one free ricochet, always
        pr.owner = uint8_t(id);           // now it fights for the shieldman
        pr.deflected = 1;                 // orange from here on
        // ---- SPATIAL ARMOR mutation: the barrier is a marksman's mirror.
        // The deflected shell leaves aimed square at the nearest enemy,
        // twice as fast and twice as hard -- stacking on every deflection
        // (speed capped so segment tests stay sound).
        if (HasUpgrade(sp, UpgradeId::SpatialArmor))
        {
            int tgt = -1;
            float bestD2 = 1e18f;
            for (int e = 0; e < MaxPlayers; ++e)
            {
                const PlayerState& ep = gs.players[e];
                if (!ep.active || ep.health <= 0 || e == id)
                    continue;
                float dx = ep.x - pr.x, dz = ep.z - pr.z;
                float d2 = dx * dx + dz * dz;
                if (d2 < bestD2) { bestD2 = d2; tgt = e; }
            }
            if (tgt >= 0)
                pr.yaw = atan2f(gs.players[tgt].x - pr.x,
                                gs.players[tgt].z - pr.z);
            pr.speed = std::min(pr.speed * SpatialArmorMul, SpatialSpeedCap);
            pr.damage *= 2;
        }
        return true;
    }
    return false;
}

bool GameState::SpawnSoldierAt(int ownerId, float x, float z)
{
    x = std::clamp(x, -ArenaHalf + SoldierRadius + 0.2f,
                   ArenaHalf - SoldierRadius - 0.2f);
    z = std::clamp(z, -ArenaHalf + SoldierRadius + 0.2f,
                   ArenaHalf - SoldierRadius - 0.2f);
    if (PointHitsObstacle(x, 0.1f, z, SoldierRadius))
        return SpawnSoldier(ownerId);      // blocked ricochet: draft at home
    SoldierState* slot = nullptr;
    for (SoldierState& s : soldiers)
        if (!s.active) { slot = &s; break; }
    if (!slot)
        return false;
    const PlayerState& own = players[ownerId];
    *slot = SoldierState{};
    slot->active = true;
    slot->owner = uint8_t(ownerId);
    slot->state = SoldierGuard;
    slot->x = x;
    slot->z = z;
    slot->yaw = own.hullYaw;
    slot->health = own.stats[int(Stat::SoldierHealth)];
    slot->speed = own.stats[int(Stat::SoldierSpeed)];
    slot->damage = own.stats[int(Stat::SoldierDamage)];
    slot->fireRate = own.stats[int(Stat::SoldierFireRate)];
    slot->grenades = int(own.stats[int(Stat::GrenadeCount)] + 0.5f);
    slot->grenadeWait = 1.5f;
    return true;
}

bool GameState::SpawnSoldier(int ownerId)
{
    SoldierState* slot = nullptr;
    for (SoldierState& s : soldiers)
        if (!s.active) { slot = &s; break; }
    if (!slot)
        return false;
    const PlayerState& own = players[ownerId];
    for (int k = 0; k < 8; ++k)   // ring of candidate spots beside the tank
    {
        float ang = own.hullYaw + XM_2PI * float(k) / 8.0f + XM_PI * 0.5f;
        float sx = own.x + sinf(ang) * 2.6f;
        float sz = own.z + cosf(ang) * 2.6f;
        if (fabsf(sx) > ArenaHalf - SoldierRadius
            || fabsf(sz) > ArenaHalf - SoldierRadius
            || PointHitsObstacle(sx, 0.1f, sz, SoldierRadius))
            continue;
        *slot = SoldierState{};
        slot->active = true;
        slot->owner = uint8_t(ownerId);
        slot->state = SoldierGuard;
        slot->x = sx;
        slot->z = sz;
        slot->yaw = own.hullYaw;
        slot->health = own.stats[int(Stat::SoldierHealth)];
        slot->speed = own.stats[int(Stat::SoldierSpeed)];
        slot->damage = own.stats[int(Stat::SoldierDamage)];
        slot->fireRate = own.stats[int(Stat::SoldierFireRate)];
        slot->grenades = int(own.stats[int(Stat::GrenadeCount)] + 0.5f);
        slot->grenadeWait = 1.5f;   // settle in before the first lob
        return true;
    }
    return false;
}

// Pick the next cover destination against the current enemies, two passes:
//   1. the best FULL-cover spot on the map (blocks every enemy's line of
//      sight), scored toward gun range of the target and away from the
//      current spot (novelty forces the peek-relocate rhythm);
//   2. if that goal is not reachable by a straight run (paths are tested
//      against body-inflated boxes -- greedy steering cannot plan around
//      geometry), route via a reachable STEPPING-STONE spot that strictly
//      closes distance to the goal. Multi-hop progression, no pathfinder.
// Returns false when no full cover exists or the soldier is cornered: kite.
static bool PickCover(const GameState& gs, SoldierState& s,
                      const int* enemies, int ne)
{
    CoverSpot spots[MaxCoverSpots];
    int n = GatherCoverSpots(spots, MaxCoverSpots);
    const float walk = SoldierRadius * 0.8f;

    auto usable = [&](const CoverSpot& c)
    {
        if (fabsf(c.x) > ArenaHalf - SoldierRadius
            || fabsf(c.z) > ArenaHalf - SoldierRadius)
            return false;
        // a spot with a tank parked on it is physically unreachable: the
        // hull push-out keeps the soldier at arm's length forever
        for (int id = 0; id < MaxPlayers; ++id)
        {
            const PlayerState& p = gs.players[id];
            if (!p.active || p.health <= 0)
                continue;
            float dx = c.x - p.x, dz = c.z - p.z;
            float minD = TankRadius + SoldierRadius + 0.3f;
            if (dx * dx + dz * dz < minD * minD)
                return false;
        }
        return true;
    };
    auto coverInfo = [&](const CoverSpot& c, int& blocked, float& nearestEnemy)
    {
        blocked = 0;
        nearestEnemy = 1e9f;
        for (int e = 0; e < ne; ++e)
        {
            const PlayerState& t = gs.players[enemies[e]];
            if (SegmentBlockedByObstacles(c.x, c.z, t.x, t.z))
                ++blocked;
            float dx = t.x - c.x, dz = t.z - c.z;
            nearestEnemy = std::min(nearestEnemy, sqrtf(dx * dx + dz * dz));
        }
    };

    // the nearest enemy anchors the "run is the peek" route preference
    int nearestId = -1;
    {
        float ndd = 1e18f;
        for (int e = 0; e < ne; ++e)
        {
            const PlayerState& t = gs.players[enemies[e]];
            float dx = t.x - s.x, dz = t.z - s.z;
            float d2 = dx * dx + dz * dz;
            if (d2 < ndd) { ndd = d2; nearestId = enemies[e]; }
        }
    }
    // which wall CLUSTER is the soldier hugging right now? Runs must go to
    // ANOTHER cluster -- composed groups (Ls, pinwheels...) are several
    // boxes, and bouncing between two legs of one L is the same deadlock
    // as orbiting one box: the enemy's sight line never gets crossed.
    auto obstacleAt = [](float x, float z)
    {
        int bi = 0, i = 0;
        float bd = 1e18f;
        for (const Obstacle& o : kObstacles)
        {
            float d = std::max(fabsf(x - o.cx) - o.hx,
                               fabsf(z - o.cz) - o.hz);
            if (d < bd) { bd = d; bi = i; }
            ++i;
        }
        return bi;
    };
    int hereWall = obstacleAt(s.x, s.z);
    float hereCx = kObstacles[hereWall].cx, hereCz = kObstacles[hereWall].cz;

    // pass 1: best full-cover spot anywhere
    float bestScore = -1e9f;
    int best = -1;
    bool bestReachable = false;
    for (int i = 0; i < n; ++i)
    {
        const CoverSpot& c = spots[i];
        if (c.wide || !usable(c))
            continue;   // wide spots are for peeking, never for hiding
        int blocked;
        float nearestEnemy;
        coverInfo(c, blocked, nearestEnemy);
        if (blocked < ne)
            continue;   // only full cover is worth ducking behind
        float dx = c.x - s.x, dz = c.z - s.z;
        float score = 4000.0f - sqrtf(dx * dx + dz * dz) * 30.0f;
        if (nearestEnemy < 6.0f)
            score -= 600.0f;               // don't hide in the enemy's lap
        // ADVANCE: cover that leaves the target out of gun range is nearly
        // worthless -- this pushes the soldier obstacle-to-obstacle toward
        // the fight (and across open ground, where it shoots)
        score -= std::max(0.0f, nearestEnemy - SoldierFireRange * 0.85f) * 120.0f;
        // ROAM WIDE: anything near the current hideout is stale, and the
        // SAME cluster is a dead loop -- group-to-group sprints only
        float cx = c.x - s.coverX, cz = c.z - s.coverZ;
        if (cx * cx + cz * cz < 144.0f)
            score -= 1200.0f;              // novelty: a whole group away
        int spotWall = obstacleAt(c.x, c.z);
        float wdx = kObstacles[spotWall].cx - hereCx;
        float wdz = kObstacles[spotWall].cz - hereCz;
        if (wdx * wdx + wdz * wdz < 81.0f)
            score -= 800.0f;               // same composed group: stale
        // the RUN is the peek: strongly prefer destinations whose route
        // swings through open ground where the gun can speak mid-sprint
        if (nearestId >= 0)
        {
            const PlayerState& tt = gs.players[nearestId];
            float mx = (c.x + s.x) * 0.5f, mz = (c.z + s.z) * 0.5f;
            float tdx = tt.x - mx, tdz = tt.z - mz;
            if (tdx * tdx + tdz * tdz
                    < SoldierFireRange * SoldierFireRange
                && !SegmentBlockedByObstacles(mx, mz, tt.x, tt.z, 0.2f))
                score += 1500.0f;
        }
        if (score > bestScore)
        {
            bestScore = score;
            best = i;
            bestReachable =
                !SegmentBlockedByObstacles(s.x, s.z, c.x, c.z, walk);
        }
    }
    if (best < 0)
        return false;                      // no full cover on the map: kite
    if (bestReachable)
    {
        s.coverX = spots[best].x;
        s.coverZ = spots[best].z;
        return true;
    }

    // pass 2: stepping stone toward the (unreachable) goal spot. Stones that
    // close distance are strongly preferred, but when a blocker (say, a tank
    // parked in the corridor) rules them all out, a DETOUR stone that moves
    // away first is still better than giving up -- greedy would trap here.
    const CoverSpot& goal = spots[best];
    float gdx = goal.x - s.x, gdz = goal.z - s.z;
    float dGoal = sqrtf(gdx * gdx + gdz * gdz);
    float bestStone = -1e9f;
    int stone = -1;
    for (int i = 0; i < n; ++i)
    {
        const CoverSpot& c = spots[i];
        if (i == best || !usable(c))
            continue;
        float sdx = c.x - s.x, sdz = c.z - s.z;
        if (sdx * sdx + sdz * sdz < 2.25f)
            continue;                      // already standing here
        if (SegmentBlockedByObstacles(s.x, s.z, c.x, c.z, walk))
            continue;
        float ddx = goal.x - c.x, ddz = goal.z - c.z;
        float dCG = sqrtf(ddx * ddx + ddz * ddz);
        int blocked;
        float nearestEnemy;
        coverInfo(c, blocked, nearestEnemy);
        float sc = -dCG * 30.0f + float(blocked) * 300.0f;
        if (dCG > dGoal - 0.5f)
            sc -= 3000.0f;                 // detour: allowed, never preferred
        float cx = c.x - s.coverX, cz = c.z - s.coverZ;
        if (cx * cx + cz * cz < 4.0f)
            sc -= 1200.0f;
        if (sc > bestStone) { bestStone = sc; stone = i; }
    }
    if (stone < 0)
        return false;                      // cornered: kite
    s.coverX = spots[stone].x;
    s.coverZ = spots[stone].z;
    return true;
}

// The WIDE ATTACK RUN: when the roam has not produced a shot in a while
// (a camped enemy whose corner no hidden route ever crosses), pick an
// EXPOSED firing point -- clear line to the target, inside gun range, one
// straight sprint away -- preferring the FARTHEST such point, so the run
// arcs wide around the wall instead of the old poke-out-and-back peek.
// The soldier fires the whole way there and tucks into fresh cover after.
static bool FindAttackRun(const GameState& gs, SoldierState& s, int targetId)
{
    const PlayerState& t = gs.players[targetId];
    CoverSpot spots[MaxCoverSpots];
    int n = GatherCoverSpots(spots, MaxCoverSpots);
    const float walk = SoldierRadius * 0.8f;
    float bestD = -1.0f;
    int best = -1;
    for (int i = 0; i < n; ++i)
    {
        const CoverSpot& c = spots[i];
        if (fabsf(c.x) > ArenaHalf - SoldierRadius
            || fabsf(c.z) > ArenaHalf - SoldierRadius)
            continue;
        float ex = t.x - c.x, ez = t.z - c.z;
        if (ex * ex + ez * ez > SoldierFireRange * SoldierFireRange * 0.81f)
            continue;                       // must end inside gun range
        if (SegmentBlockedByObstacles(c.x, c.z, t.x, t.z, 0.6f))
            continue;                       // must SEE the target there
        if (SegmentBlockedByObstacles(s.x, s.z, c.x, c.z, walk))
            continue;                       // must be a straight sprint
        bool occupied = false;
        for (int id = 0; id < MaxPlayers && !occupied; ++id)
        {
            const PlayerState& p = gs.players[id];
            if (!p.active || p.health <= 0)
                continue;
            float dx = c.x - p.x, dz = c.z - p.z;
            float minD = TankRadius + SoldierRadius + 0.3f;
            occupied = dx * dx + dz * dz < minD * minD;
        }
        if (occupied)
            continue;
        float dx = c.x - s.x, dz = c.z - s.z;
        float d = dx * dx + dz * dz;
        if (d < 4.0f || d > 196.0f)
            continue;                       // wide, but not a marathon
        if (d > bestD) { bestD = d; best = i; }
    }
    if (best >= 0)
    {
        s.coverX = spots[best].x;
        s.coverZ = spots[best].z;
        return true;
    }
    // OPEN-FIELD fallback: a sparse 120x120 map has camps with no cover
    // spot in gun range at all -- ring the target and sprint to any clear
    // firing point on it
    for (int k = 0; k < 8; ++k)
    {
        float ang = DirectX::XM_2PI * float(k) / 8.0f;
        float px = t.x + sinf(ang) * (SoldierFireRange * 0.7f);
        float pz = t.z + cosf(ang) * (SoldierFireRange * 0.7f);
        if (fabsf(px) > ArenaHalf - SoldierRadius
            || fabsf(pz) > ArenaHalf - SoldierRadius)
            continue;
        if (PointHitsObstacle(px, 0.1f, pz, SoldierRadius))
            continue;
        if (SegmentBlockedByObstacles(px, pz, t.x, t.z, 0.6f))
            continue;                       // must SEE the target there
        if (SegmentBlockedByObstacles(s.x, s.z, px, pz, walk))
            continue;                       // must be a straight sprint
        s.coverX = px;
        s.coverZ = pz;
        return true;
    }
    return false;
}

// Fire at the current target if the reload is ready, it is in range, and
// line of sight is clear. Soldiers shoot REAL ROCKETS from the shared
// projectile pool: full travel time, obstacle detonation, smoke, and kill
// credit through the same path as tank shells (owner = the summoner).
static void SoldierTryFire(GameState& gs, SoldierState& s)
{
    if (s.fireCooldown > 0.0f || s.targetId >= MaxPlayers)
        return;
    const PlayerState& t = gs.players[s.targetId];
    if (!t.active || t.health <= 0)
        return;
    float dx = t.x - s.x, dz = t.z - s.z;
    if (dx * dx + dz * dz > SoldierFireRange * SoldierFireRange)
        return;
    // clearance for the ROCKET, not a zero-width ray: a sight line grazing
    // a corner by less than the projectile radius means the rocket clips
    // the box and detonates on the corner instead of reaching the target
    if (SegmentBlockedByObstacles(s.x, s.z, t.x, t.z, ProjectileRadius + 0.1f))
        return;
    // BONE PLATOON mutation: the squad fires SKULLS instead of rockets.
    // Cadence, contact damage and (at burst) acid are an equal blend of the
    // two families: interval = avg(SkullRate, 1/fireRate), contact damage
    // = avg(soldier rocket damage, SkullDamage).
    const PlayerState& ownr = gs.players[s.owner];
    if (HasUpgrade(ownr, UpgradeId::BonePlatoon))
    {
        for (SkullState& sk : gs.skulls)
        {
            if (sk.active)
                continue;
            float yaw = atan2f(dx, dz);
            sk = SkullState{};
            sk.active = true;
            sk.owner = s.owner;
            sk.x = s.x + sinf(yaw) * 0.8f;
            sk.z = s.z + cosf(yaw) * 0.8f;
            sk.yaw = yaw;
            sk.life = 8.0f;
            sk.dmg = (s.damage + ownr.stats[int(Stat::SkullDamage)]) * 0.5f;
            s.fireCooldown = (ownr.stats[int(Stat::SkullRate)]
                              + 1.0f / std::max(0.1f, s.fireRate)) * 0.5f;
            s.muzzleFlash = 0.12f;
            s.yaw = yaw;
            s.sinceShot = 0.0f;
            break;
        }
        return;                    // no free skull slot = hold fire
    }
    for (Projectile& pr : gs.projectiles)
    {
        if (pr.active)
            continue;
        float yaw = atan2f(dx, dz);
        pr = Projectile{};
        pr.active = true;
        pr.owner = s.owner;
        pr.x = s.x + sinf(yaw) * 0.8f;   // off the shoulder launcher
        pr.y = SoldierGunY;
        pr.z = s.z + cosf(yaw) * 0.8f;
        pr.yaw = yaw;
        pr.life = ProjectileLife;
        pr.speed = SoldierRocketSpeed;
        pr.damage = int(s.damage + 0.5f);
        pr.bounces = 0;
        // class synergy: a SOLDIER+RADAR necromancer of war -- summons fire
        // ring rockets when their owner also holds the RADAR class
        const PlayerState& own = gs.players[s.owner];
        if (HasClass(own, ClassRadar))
        {
            pr.radarRange = own.stats[int(Stat::RadarRange)];
            pr.radarDamage = own.stats[int(Stat::RadarDamage)];
            pr.radarLockNeed = own.stats[int(Stat::RadarLock)];
            pr.radarRings = std::clamp(
                int(own.stats[int(Stat::RadarRings)] + 0.5f),
                0, MaxRadarExtra);
        }
        s.fireCooldown = 1.0f / std::max(0.1f, s.fireRate);
        s.muzzleFlash = 0.12f;
        s.yaw = yaw;   // square up to the shot
        s.sinceShot = 0.0f;
        break;
    }
}

// ------------------------------------------------------------- grenades
// Lob a grenade at the soldier's target: a ballistic arc with a random yaw
// offset (deterministic hash -- fire-and-forget entities replicate, so the
// host's roll is the only roll). No line-of-sight check on purpose: lobbing
// OVER walls is the whole point of a grenade.
static void SoldierTryGrenade(GameState& gs, SoldierState& s)
{
    if (s.grenades <= 0 || s.grenadeWait > 0.0f || s.targetId >= MaxPlayers)
        return;
    // MARTYRDOM traded the grenades away: the payload ships on death now
    if (HasUpgrade(gs.players[s.owner], UpgradeId::Martyrdom))
        return;
    const PlayerState& t = gs.players[s.targetId];
    if (!t.active || t.health <= 0)
        return;
    float dx = t.x - s.x, dz = t.z - s.z;
    float dist = sqrtf(dx * dx + dz * dz);
    if (dist < 3.5f || dist > GrenadeThrowRange)
        return;
    GrenadeState* slot = nullptr;
    for (GrenadeState& gr : gs.grenades)
        if (!gr.active) { slot = &gr; break; }
    if (!slot)
        return;
    uint32_t h = (gs.tick * 2654435761u) ^ uint32_t(s.owner * 977 + 31);
    h ^= h >> 16; h *= 2246822519u; h ^= h >> 13;
    float offset = (float(h & 0xFFFF) / 65535.0f - 0.5f) * 0.5f;
    float yaw = atan2f(dx, dz) + offset;
    // extra loft so the arc clears even the tallest (4 u) walls
    float T = 1.15f + 0.3f * (dist / GrenadeThrowRange);   // flight time
    GrenadeState gr{};
    gr.active = true;
    gr.owner = s.owner;
    gr.x = s.x; gr.y = 1.5f; gr.z = s.z;
    gr.vx = sinf(yaw) * (dist / T);
    gr.vz = cosf(yaw) * (dist / T);
    gr.vy = 0.5f * GrenadeGravity * T - 1.5f / T;   // land near y=0
    gr.fuse = -1.0f;                                // armed at first bounce
    gr.dmg = s.damage * 2.0f;                       // 2x the rocket hit
    *slot = gr;
    --s.grenades;
    s.grenadeWait = gs.players[s.owner].stats[int(Stat::GrenadeCooldown)];
}

static void GrenadeExplode(GameState& gs, GrenadeState& gr)
{
    for (int id = 0; id < MaxPlayers; ++id)
    {
        PlayerState& p = gs.players[id];
        if (!p.active || p.health <= 0 || id == gr.owner)
            continue;
        float dx = p.x - gr.x, dz = p.z - gr.z;
        float rr = GrenadeBlastRadius + TankRadius * 0.5f;
        if (dx * dx + dz * dz < rr * rr)
            gs.ApplyDamage(gr.owner, id, int(gr.dmg + 0.5f), 2);
    }
    for (SoldierState& s : gs.soldiers)
    {
        if (!s.active || s.state >= SoldierDying || s.owner == gr.owner)
            continue;
        float dx = s.x - gr.x, dz = s.z - gr.z;
        float rr = GrenadeBlastRadius + SoldierRadius;
        if (dx * dx + dz * dz < rr * rr)
        {
            s.health -= gr.dmg;
            s.lastHitBy = gr.owner;
        }
    }
    gr.active = false;
}

static void TickGrenade(GameState& gs, GrenadeState& gr)
{
    if (gr.fuse >= 0.0f)
    {
        gr.fuse -= TickDt;
        if (gr.fuse <= 0.0f)
        {
            GrenadeExplode(gs, gr);
            return;
        }
    }
    gr.vy -= GrenadeGravity * TickDt;
    gr.x += gr.vx * TickDt;
    gr.y += gr.vy * TickDt;
    gr.z += gr.vz * TickDt;
    bool bounced = false;

    // ground
    if (gr.y < GrenadeRadius && gr.vy < 0.0f)
    {
        gr.y = GrenadeRadius;
        gr.vy = -gr.vy * GrenadeRestitution;
        gr.vx *= GrenadeFriction;
        gr.vz *= GrenadeFriction;
        if (gr.vy < 0.6f) gr.vy = 0.0f;   // stop micro-bouncing, just slide
        bounced = true;
    }
    // arena walls
    float lim = ArenaHalf - GrenadeRadius;
    if (gr.x < -lim) { gr.x = -lim; gr.vx = -gr.vx * GrenadeRestitution; bounced = true; }
    if (gr.x >  lim) { gr.x =  lim; gr.vx = -gr.vx * GrenadeRestitution; bounced = true; }
    if (gr.z < -lim) { gr.z = -lim; gr.vz = -gr.vz * GrenadeRestitution; bounced = true; }
    if (gr.z >  lim) { gr.z =  lim; gr.vz = -gr.vz * GrenadeRestitution; bounced = true; }
    // obstacle boxes (only while below each box's own top face)
    for (const Obstacle& o : kObstacles)
        if (gr.y < o.height)
        {
            float px = gr.x - o.cx, pz = gr.z - o.cz;
            float ox = o.hx + GrenadeRadius - fabsf(px);
            float oz = o.hz + GrenadeRadius - fabsf(pz);
            if (ox <= 0.0f || oz <= 0.0f)
                continue;
            if (ox < oz)
            {
                gr.x += (px > 0 ? ox : -ox);
                gr.vx = -gr.vx * GrenadeRestitution;
            }
            else
            {
                gr.z += (pz > 0 ? oz : -oz);
                gr.vz = -gr.vz * GrenadeRestitution;
            }
            bounced = true;
        }
    // tanks: shove off the hull, radial reflection
    if (gr.y < 1.6f)
        for (int id = 0; id < MaxPlayers; ++id)
        {
            const PlayerState& p = gs.players[id];
            if (!p.active || p.health <= 0)
                continue;
            float dx = gr.x - p.x, dz = gr.z - p.z;
            float rr = TankRadius + GrenadeRadius;
            float d2 = dx * dx + dz * dz;
            if (d2 >= rr * rr || d2 < 1e-6f)
                continue;
            float d = sqrtf(d2);
            float nx = dx / d, nz = dz / d;
            gr.x = p.x + nx * rr;
            gr.z = p.z + nz * rr;
            float vn = gr.vx * nx + gr.vz * nz;
            if (vn < 0.0f)
            {
                gr.vx -= (1.0f + GrenadeRestitution) * vn * nx;
                gr.vz -= (1.0f + GrenadeRestitution) * vn * nz;
            }
            bounced = true;
        }
    if (bounced && gr.fuse < 0.0f)
        gr.fuse = GrenadeFuse;   // first contact arms the 2 s fuse
}

void GameState::TickSoldier(SoldierState& s)
{
    s.grenadeWait = std::max(0.0f, s.grenadeWait - TickDt);
    if (s.state != SoldierDying)
        SoldierTryGrenade(*this, s);
    s.fireCooldown = std::max(0.0f, s.fireCooldown - TickDt);
    s.muzzleFlash = std::max(0.0f, s.muzzleFlash - TickDt);
    s.hitFlash = std::max(0.0f, s.hitFlash - TickDt);
    s.sinceShot += TickDt;

    if (s.state == SoldierKamikaze)
    {
        // MARTYRDOM: a dead man hopping. Cartoon velocity straight at the
        // nearest enemy tank (the hop/flash/swell is client-side theatre),
        // then a grenade-count-scaled blast when the fuse runs out.
        s.deathTimer -= TickDt;
        int tgt = NearestEnemyTank(*this, s.owner, s.x, s.z);
        if (tgt >= 0)
        {
            const PlayerState& t = players[tgt];
            float dx = t.x - s.x, dz = t.z - s.z;
            float d = sqrtf(dx * dx + dz * dz);
            if (d > 1e-3f)
            {
                s.yaw = atan2f(dx, dz);
                s.x += dx / d * KamikazeSpeed * TickDt;
                s.z += dz / d * KamikazeSpeed * TickDt;
            }
        }
        s.x = std::clamp(s.x, -ArenaHalf + SoldierRadius,
                         ArenaHalf - SoldierRadius);
        s.z = std::clamp(s.z, -ArenaHalf + SoldierRadius,
                         ArenaHalf - SoldierRadius);
        CollideCircleObstacles(s.x, s.z, SoldierRadius);
        if (s.deathTimer <= 0.0f)
        {
            const PlayerState& own = players[s.owner];
            float count = own.stats[int(Stat::GrenadeCount)];
            float radius = KamikazeBaseRadius + 1.0f * count;
            int dmg = int(own.stats[int(Stat::SoldierDamage)]
                          * (1.0f + KamikazeDmgPerCount * count) + 0.5f);
            for (int id = 0; id < MaxPlayers; ++id)
            {
                const PlayerState& t = players[id];
                if (!t.active || t.health <= 0 || id == s.owner)
                    continue;
                float dx = t.x - s.x, dz = t.z - s.z;
                float rr = radius + TankRadius;
                if (dx * dx + dz * dz < rr * rr)
                    ApplyDamage(s.owner, id, dmg, 2);
            }
            for (SoldierState& e : soldiers)
            {
                if (!e.active || e.state >= SoldierDying
                    || e.owner == s.owner)
                    continue;
                float dx = e.x - s.x, dz = e.z - s.z;
                float rr = radius + SoldierRadius;
                if (dx * dx + dz * dz < rr * rr)
                {
                    e.health -= float(dmg);
                    e.lastHitBy = s.owner;
                }
            }
            s.active = false;
        }
        return;
    }
    if (s.state == SoldierDying)
    {
        s.deathTimer -= TickDt;
        if (s.deathTimer <= 0.0f)
            s.active = false;
        return;
    }
    if (s.health <= 0.0f || !players[s.owner].active)
    {
        // fallen soldiers rise for the necromancer who slew them
        if (s.health <= 0.0f && s.lastHitBy < MaxPlayers
            && s.lastHitBy != s.owner
            && players[s.lastHitBy].active
            && HasClass(players[s.lastHitBy], ClassNecro))
            SpawnGhost(*this, s.lastHitBy, s.x, s.z);
        // HAUNTED SQUAD mutation: the owner's dead soldiers rise as HALF
        // ghosts (possession duration and damage halved) and go hunting
        if (s.health <= 0.0f && players[s.owner].active
            && HasUpgrade(players[s.owner], UpgradeId::HauntedSquad))
            SpawnGhost(*this, s.owner, s.x, s.z, true);
        // MARTYRDOM mutation: a KILLED soldier goes kamikaze instead of
        // playing dead (owner-gone despawns still die quietly)
        if (s.health <= 0.0f && players[s.owner].active
            && HasUpgrade(players[s.owner], UpgradeId::Martyrdom))
        {
            s.state = SoldierKamikaze;
            s.deathTimer = KamikazeFuse;
            return;
        }
        s.state = SoldierDying;
        s.deathTimer = SoldierDeathTime;
        return;
    }

    // enemies of the owner
    int enemies[MaxPlayers], ne = 0;
    int nearest = -1;
    float nearestD2 = 1e18f;
    for (int id = 0; id < MaxPlayers; ++id)
    {
        const PlayerState& p = players[id];
        if (!p.active || p.health <= 0 || id == s.owner)
            continue;
        enemies[ne++] = id;
        float dx = p.x - s.x, dz = p.z - s.z;
        float d2 = dx * dx + dz * dz;
        if (d2 < nearestD2) { nearestD2 = d2; nearest = id; }
    }

    float moveX = 0, moveZ = 0;
    if (ne == 0)
    {
        // guard: loiter beside the owner
        s.state = SoldierGuard;
        s.targetId = 0xFF;
        const PlayerState& own = players[s.owner];
        float gx = own.x + sinf(own.hullYaw + XM_PI * 0.5f) * 2.6f;
        float gz = own.z + cosf(own.hullYaw + XM_PI * 0.5f) * 2.6f;
        float dx = gx - s.x, dz = gz - s.z;
        if (dx * dx + dz * dz > 2.0f) { moveX = dx; moveZ = dz; }
    }
    else
    {
        s.targetId = uint8_t(nearest);
        switch (s.state)
        {
        case SoldierGuard:
            // enemies appeared: get behind something
            if (PickCover(*this, s, enemies, ne))
            {
                s.state = SoldierMove;
                s.stateTimer = 2.0f;   // repath deadline (stall insurance)
            }
            else
            {
                s.state = SoldierKite;
                s.stateTimer = 0.6f;
            }
            break;
        case SoldierCover:
        {
            // ducked + hidden; face the threat, catch a breath -- then RUN.
            // No peeking: the loop is hide -> sprint WIDE to a hiding spot
            // behind ANOTHER wall, guns blazing the whole way across the
            // open (SoldierMove fires on the move).
            const PlayerState& t = players[nearest];
            s.yaw = atan2f(t.x - s.x, t.z - s.z);
            s.stateTimer -= TickDt;
            if (s.stateTimer <= 0.0f)
            {
                // a DRY roam means every hidden route dodges the enemy's
                // corner: force a wide attack run at an exposed point
                if (s.sinceShot > 3.0f && FindAttackRun(*this, s, nearest))
                {
                    s.state = SoldierMove;
                    s.stateTimer = 3.0f;
                }
                else if (s.sinceShot > 7.0f)
                {
                    // no reachable firing point AT ALL (walls box in every
                    // sprint): HUNT -- the kite state charges when starving
                    s.state = SoldierKite;
                    s.stateTimer = 2.5f;
                }
                else if (PickCover(*this, s, enemies, ne))
                {
                    s.state = SoldierMove;
                    s.stateTimer = 3.0f;   // longer runs: farther spots
                }
                else
                {
                    s.state = SoldierKite;
                    s.stateTimer = 0.6f;
                }
            }
            break;
        }
        case SoldierMove:
        {
            float dx = s.coverX - s.x, dz = s.coverZ - s.z;
            if (dx * dx + dz * dz < 0.16f)
            {
                s.state = SoldierCover;
                s.stateTimer = SoldierCoverPause;
            }
            else
            {
                moveX = dx; moveZ = dz;
                SoldierTryFire(*this, s);   // shoot while running
                // taking too long (blocked, target moved...): pick again --
                // the novelty penalty steers away from the stuck spot
                s.stateTimer -= TickDt;
                if (s.stateTimer <= 0.0f)
                {
                    if (!PickCover(*this, s, enemies, ne))
                        s.state = SoldierKite;
                    s.stateTimer = SoldierMove == s.state ? 2.0f : 0.6f;
                }
            }
            break;
        }
        case SoldierKite:
        {
            const PlayerState& t = players[nearest];
            float ax = s.x - t.x, az = s.z - t.z;
            float len = std::max(0.001f, sqrtf(ax * ax + az * az));
            ax /= len; az /= len;
            if (s.sinceShot > 7.0f)
            {
                // HUNT: a starving soldier works its way IN, guns hot.
                // Blind charging dies in concave corners (both axes of the
                // push-out cancel), so probe headings fanning out from the
                // charge direction and take the first open one.
                float base = atan2f(-ax, -az);
                static const float kFan[] = { 0.0f, 0.785f, -0.785f,
                                              1.571f, -1.571f, 2.356f,
                                              -2.356f, 3.1416f };
                for (float off : kFan)
                {
                    float hd = base + off;
                    float px = s.x + sinf(hd) * 1.6f;
                    float pz = s.z + cosf(hd) * 1.6f;
                    if (!PointHitsObstacle(px, 0.1f, pz, SoldierRadius))
                    {
                        moveX = sinf(hd);
                        moveZ = cosf(hd);
                        break;
                    }
                }
            }
            else
            {
                // no full cover: keep running (away + tangent) and shooting
                moveX = ax + az * 0.65f;    // slide sideways, backing off
                moveZ = az - ax * 0.65f;
            }
            SoldierTryFire(*this, s);
            s.stateTimer -= TickDt;
            if (s.stateTimer <= 0.0f)
            {
                if (PickCover(*this, s, enemies, ne))
                    s.state = SoldierMove;
                s.stateTimer = 0.6f;
            }
            break;
        }
        default:
            s.state = SoldierGuard;
            break;
        }
    }

    // steer around tanks (including the owner's): repulsion within a body
    // length blends with the goal direction into a tangential slide, so a
    // parked hull can't deadlock the push-out against the goal
    if (moveX != 0.0f || moveZ != 0.0f)
    {
        float gl = sqrtf(moveX * moveX + moveZ * moveZ);
        moveX /= gl; moveZ /= gl;
        const float avoid = TankRadius + SoldierRadius + 1.3f;
        for (int id = 0; id < MaxPlayers; ++id)
        {
            const PlayerState& p = players[id];
            if (!p.active || p.health <= 0)
                continue;
            float rx = s.x - p.x, rz = s.z - p.z;
            float d2 = rx * rx + rz * rz;
            if (d2 < avoid * avoid && d2 > 1e-6f)
            {
                float d = sqrtf(d2);
                float w = (avoid - d) / avoid;
                moveX += rx / d * w * 2.2f;
                moveZ += rz / d * w * 2.2f;
            }
        }
        // ...and around obstacle boxes (repulsion from the nearest point on
        // the box + the goal direction = a slide along the wall)
        for (const Obstacle& o : kObstacles)
        {
            float px = std::clamp(s.x, o.cx - o.hx, o.cx + o.hx);
            float pz = std::clamp(s.z, o.cz - o.hz, o.cz + o.hz);
            float rx = s.x - px, rz = s.z - pz;
            float d2 = rx * rx + rz * rz;
            // must stay below the spots' face offset (1.0) or the wall
            // repulsion fights the soldier's own arrivals
            const float avoidBox = SoldierRadius + 0.5f;
            if (d2 < avoidBox * avoidBox && d2 > 1e-6f)
            {
                float d = sqrtf(d2);
                float w = (avoidBox - d) / avoidBox;
                moveX += rx / d * w * 1.8f;
                moveZ += rz / d * w * 1.8f;
            }
        }
    }

    // movement + collision (soldiers never block tanks -- tanks push them)
    float ml2 = moveX * moveX + moveZ * moveZ;
    if (ml2 > 1e-6f)
    {
        float inv = 1.0f / sqrtf(ml2);
        s.x += moveX * inv * s.speed * TickDt;
        s.z += moveZ * inv * s.speed * TickDt;
        s.yaw = atan2f(moveX, moveZ);
    }
    s.x = std::clamp(s.x, -ArenaHalf + SoldierRadius, ArenaHalf - SoldierRadius);
    s.z = std::clamp(s.z, -ArenaHalf + SoldierRadius, ArenaHalf - SoldierRadius);
    CollideCircleObstacles(s.x, s.z, SoldierRadius);
    for (int id = 0; id < MaxPlayers; ++id)   // tanks shove soldiers aside
    {
        const PlayerState& p = players[id];
        if (!p.active || p.health <= 0)
            continue;
        float dx = s.x - p.x, dz = s.z - p.z;
        float d2 = dx * dx + dz * dz;
        float minD = TankRadius + SoldierRadius;
        if (d2 < minD * minD && d2 > 1e-6f)
        {
            float d = sqrtf(d2);
            s.x += dx / d * (minD - d);
            s.z += dz / d * (minD - d);
        }
    }
    for (SoldierState& o : soldiers)          // light mutual separation
    {
        if (!o.active || &o == &s)
            continue;
        float dx = s.x - o.x, dz = s.z - o.z;
        float d2 = dx * dx + dz * dz;
        float minD = SoldierRadius * 2.0f;
        if (d2 < minD * minD && d2 > 1e-6f)
        {
            float d = sqrtf(d2);
            float push = (minD - d) * 0.5f;
            s.x += dx / d * push;
            s.z += dz / d * push;
        }
    }
}

// Screen-relative movement: W/S/A/D push the tank up/down/left/right on
// screen. The camera sits at -Z looking toward +Z with a right-handed
// view, which makes screen-right equal world -X (and screen-up +Z).
// The hull only *visually* turns to face the direction of travel.
void GameState::AdvanceMovement(int id, const InputCmd& inRaw)
{
    PlayerState& p = players[id];
    // BUBBLE trap: remember where this tick started -- a tank that BEGAN
    // fully inside someone's dome is contained for the whole tick
    float bubblePrevX = p.x, bubblePrevZ = p.z;
    // POSSESSION: the ghost drives. Deterministic chaos derived from the
    // remaining-time bucket so client prediction replays the exact same
    // swerves after rebasing possessTimer from the snapshot.
    InputCmd in = inRaw;
    if (p.possessTimer > 0.0f)
    {
        p.possessTimer = std::max(0.0f, p.possessTimer - TickDt);
        uint32_t h = uint32_t(id) * 2654435761u
                   ^ uint32_t(int(p.possessTimer * 2.5f) + 1) * 40503u;
        h ^= h << 13; h ^= h >> 17; h ^= h << 5;
        float ang = float(h & 0xFFFF) * (DirectX::XM_2PI / 65535.0f);
        in.moveX = sinf(ang);
        in.moveZ = cosf(ang);
        in.turretYaw = ang;                        // head spins too
        in.buttons &= uint8_t(~(BtnFire | BtnBoost));
    }
    // SHIELD (key 1): timers advance HERE so client prediction replays them
    // identically after rebasing from the snapshot -- the same contract as
    // boost fuel and possession. Activation is input-driven and free of any
    // host-only randomness, so pressing 1 feels instant on clients too.
    if (p.shieldTimer > 0.0f)
    {
        p.shieldTimer = std::max(0.0f, p.shieldTimer - TickDt);
        if (p.shieldTimer <= 0.0f)   // the use is over: NOW the meter runs
            p.shieldWait = p.stats[int(Stat::ShieldCooldown)];
    }
    else
        p.shieldWait = std::max(0.0f, p.shieldWait - TickDt);
    if ((in.buttons & BtnAbility1) && p.shieldWait <= 0.0f
        && p.shieldTimer <= 0.0f && HasClass(p, ClassShield))
        p.shieldTimer = p.stats[int(Stat::ShieldDuration)];
    p.shieldAimYaw = in.aimYawFresh;

    // The client resolves camera-relative WASD into a world-space vector, so
    // the host (and prediction replay) integrate the same numbers.
    float dx = in.moveX, dz = in.moveZ;
    float len2 = dx * dx + dz * dz;
    bool moving = len2 > 1e-6f;

    // Boost (SHIFT): multiplies speed while fuel lasts; only drains while
    // actually moving. Regen waits BoostRegenDelay after the last boosted
    // tick, then refills at BoostRegen/s. Lives here so client prediction
    // and host simulation integrate fuel identically.
    bool boosting = moving && (in.buttons & BtnBoost) && p.boostFuel > 0.0f;
    if (boosting)
    {
        p.boostFuel = std::max(0.0f, p.boostFuel - TickDt);
        p.boostRegenWait = p.stats[int(Stat::BoostRegenDelay)];
    }
    else if (p.boostRegenWait > 0.0f)
    {
        p.boostRegenWait = std::max(0.0f, p.boostRegenWait - TickDt);
    }
    else
    {
        p.boostFuel = std::min(p.stats[int(Stat::BoostFuel)],
                               p.boostFuel
                                   + p.stats[int(Stat::BoostRegen)] * TickDt);
    }

    if (moving)
    {
        float len = sqrtf(len2);
        if (len > 1.0f) { dx /= len; dz /= len; }
        float speed = p.stats[int(Stat::MoveSpeed)];
        if (boosting)
        {
            float bs = p.stats[int(Stat::BoostSpeed)];
            if (p.shieldTimer > 0.0f)   // boost 20% weaker behind the barrier
                bs = 1.0f + (bs - 1.0f) * ShieldBoostMalus;
            speed *= bs;
        }
        if (p.shieldTimer > 0.0f)
            speed *= ShieldSlow;        // fixed 35% slow (not upgradeable)
        if (HasUpgrade(p, UpgradeId::Stealth))
            speed *= StealthSlow;       // the price of invisibility
        if (HasUpgrade(p, UpgradeId::Drunken))
            speed *= DrunkenFactor(tick, id);
        p.x += dx * speed * TickDt;
        p.z += dz * speed * TickDt;
        p.hullYaw = MoveTowardsAngle(p.hullYaw, atan2f(dx, dz),
                                     HullFaceSpeed * TickDt);
    }

    // BUBBLE mutation: a one-way dome. Driving IN is free; a tank that
    // started this tick fully inside is clamped back inside -- and dragged
    // along as the dome rides ahead of its owner's aim. Applied BEFORE
    // obstacle resolution so walls always win over the wall of the dome.
    for (int b = 0; b < MaxPlayers; ++b)
    {
        const PlayerState& bp = players[b];
        if (b == id || !bp.active || bp.health <= 0
            || bp.shieldTimer <= 0.0f || !HasUpgrade(bp, UpgradeId::Bubble))
            continue;
        float bcx, bcz;
        BubbleCenter(bp, bcx, bcz);
        // continuous containment: "was inside" is judged against the dome's
        // PREVIOUS center -- a lunging owner drags prisoners along instead
        // of teleporting the wall past them
        float pcx = bp.bubblePrevValid ? bp.bubblePrevCx : bcx;
        float pcz = bp.bubblePrevValid ? bp.bubblePrevCz : bcz;
        float inR = BubbleRadiusFor(bp) - TankRadius;
        if (inR <= 0.2f)
            continue;
        float pdx = bubblePrevX - pcx, pdz = bubblePrevZ - pcz;
        if (pdx * pdx + pdz * pdz > (inR + 0.10f) * (inR + 0.10f))
            continue;                     // was not fully inside: free
        // RAYCAST the tick's movement (bubble frame) against the wall and
        // park the tank AT the hit point on xz -- no wall passes at any
        // speed, the owner's or the prisoner's
        float ndx = p.x - bcx, ndz = p.z - bcz;
        float tHit;
        if (SegmentExitsCircle(pdx, pdz, ndx, ndz, inR, tHit))
        {
            float hx = pdx + (ndx - pdx) * tHit;
            float hz = pdz + (ndz - pdz) * tHit;
            float hl = std::max(1e-4f, sqrtf(hx * hx + hz * hz));
            p.x = bcx + hx / hl * (inR - 0.02f);
            p.z = bcz + hz / hl * (inR - 0.02f);
        }
        else
        {
            float d = sqrtf(ndx * ndx + ndz * ndz);
            if (d > inR && d > 1e-4f)     // numeric edge: radial fallback
            {
                p.x = bcx + ndx / d * inR;
                p.z = bcz + ndz / d * inR;
            }
        }
    }

    p.x = std::clamp(p.x, -ArenaHalf + TankRadius, ArenaHalf - TankRadius);
    p.z = std::clamp(p.z, -ArenaHalf + TankRadius, ArenaHalf - TankRadius);
    CollideCircleObstacles(p.x, p.z, TankRadius);

    // tank-tank separation
    for (int other = 0; other < MaxPlayers; ++other)
    {
        if (other == id || !players[other].active || players[other].health <= 0)
            continue;
        float sx = p.x - players[other].x, sz = p.z - players[other].z;
        float d2 = sx * sx + sz * sz;
        float minD = TankRadius * 2.0f;
        if (d2 < minD * minD && d2 > 1e-6f)
        {
            float d = sqrtf(d2);
            float push = (minD - d) * 0.5f;
            p.x += sx / d * push;
            p.z += sz / d * push;
        }
    }

    p.turretYaw = in.turretYaw;   // turret snaps to the aim instantly
}

void GameState::Tick(const InputCmd* inputs)
{
    ++tick;

    // ---------------- match phases ----------------
    if (phase == PhaseLobby || phase == PhaseGathering)
    {
        int active = 0, ready = 0;
        for (int id = 0; id < MaxPlayers; ++id)
        {
            PlayerState& p = players[id];
            if (!p.active)
                continue;
            ++active;
            if (p.ready) ++ready;
            LobbySpot(id, p.x, p.z, p.hullYaw);   // parked lineup
            p.turretYaw = p.hullYaw;
            if (tick % StatRecalcTicks == 0)
                RecalcStats(id);
            p.health = MaxHealthFor(p);
        }
        if (phase == PhaseGathering)
        {
            // queue filled: reveal the ready-up lobby
            if (targetPlayers > 0 && active >= int(targetPlayers))
                ToLobby();
        }
        else if (active > 0 && ready == active)
        {
            StartMatch();
        }
        return;   // no movement, firing, offers or money in the lobby
    }
    if (phase == PhaseEnded)
    {
        if (tick >= endedTick + EndedReturnTicks)
            ToLobby();
        return;
    }
    if (phase == PhasePlaying && tick >= matchEndTick)
    {
        // most kills wins; a tie goes to overtime (first kill takes it)
        int best = -1, bestId = 0xFF;
        bool tie = false;
        for (int id = 0; id < MaxPlayers; ++id)
        {
            if (!players[id].active)
                continue;
            int s = players[id].score;
            if (s > best) { best = s; bestId = id; tie = false; }
            else if (s == best) tie = true;
        }
        if (tie)
        {
            phase = PhaseOvertime;
        }
        else
        {
            phase = PhaseEnded;
            winner = uint8_t(bestId);
            endedTick = tick;
            return;
        }
    }

    // BUBBLE continuous collision: freeze every dome's center BEFORE any
    // movement this tick -- crossing tests then run prev-vs-prev /
    // now-vs-now (the bubble's relative frame), so a boosting owner cannot
    // drag the wall past rockets or slip trapped tanks out the back.
    for (int id = 0; id < MaxPlayers; ++id)
    {
        PlayerState& p = players[id];
        if (p.active && p.health > 0 && p.shieldTimer > 0.0f
            && HasUpgrade(p, UpgradeId::Bubble))
        {
            BubbleCenter(p, p.bubblePrevCx, p.bubblePrevCz);
            p.bubblePrevValid = 1;
        }
        else
            p.bubblePrevValid = 0;
    }

    for (int id = 0; id < MaxPlayers; ++id)
    {
        PlayerState& p = players[id];
        if (!p.active)
            continue;

        p.fireCooldown = std::max(0.0f, p.fireCooldown - TickDt);
        p.hitFlash = std::max(0.0f, p.hitFlash - TickDt);
        p.muzzleFlash = std::max(0.0f, p.muzzleFlash - TickDt);

        // VAMPIRE: daylight is lethal -- 10% of max HP per second unless a
        // wall or box shades the tank (accumulate fractions, bite in ints)
        if (HasUpgrade(p, UpgradeId::Vampire) && p.health > 0)
        {
            if (InSunlight(p.x, p.z))
            {
                p.sunAccum += VampireBurn(p) * TickDt;
                while (p.sunAccum >= 1.0f && p.health > 0)
                {
                    p.sunAccum -= 1.0f;
                    ApplyDamage(id, id, 1, 0);   // self: no score, no steal
                }
            }
            else
                p.sunAccum = 0.0f;
        }

        // stat cache refresh on the host cadence (and on purchase elsewhere)
        if (tick % StatRecalcTicks == 0)
            RecalcStats(id);

        // conveyor: a fresh random offer every 5 seconds
        if (tick >= p.nextOfferTick)
        {
            GenerateOffer(id);
            p.nextOfferTick = tick + OfferIntervalTicks;
        }

        // expire finished burns and compact their slots
        for (int s = 0; s < NumOfferSlots; ++s)
        {
            if (p.offers[s].active == OfferConsumed && tick >= p.offers[s].consumedTick)
            {
                for (int t2 = s; t2 < NumOfferSlots - 1; ++t2)
                    p.offers[t2] = p.offers[t2 + 1];
                p.offers[NumOfferSlots - 1] = Offer{};
                --s;   // re-check this slot (a consumed card may have shifted in)
            }
        }
        // drain queued arrivals once no burn holds the conveyor (0.25s apart)
        if (!p.pendingOffers.empty() && !AnyConsumed(id)
            && tick >= p.nextPendingDrainTick)
        {
            InsertOffer(id, p.pendingOffers.front());
            p.pendingOffers.erase(p.pendingOffers.begin());
            p.nextPendingDrainTick = tick + TickRate / 4;
        }

        if (p.health <= 0)
        {
            p.respawnTimer -= TickDt;
            if (p.respawnTimer <= 0.0f)
            {
                SpawnPoint(id, p.x, p.z, p.hullYaw);
                p.turretYaw = p.hullYaw;
                p.health = MaxHealthFor(p);   // keeps score/money/upgrades
                p.hitFlash = 0;
            }
            continue;
        }

        // passive income: 2 credits per second while alive
        if (tick % (TickRate / 2) == 0 && p.money < 999)
            ++p.money;

        // possession damage-over-time (attributed to the ghost's owner)
        if (p.possessTimer > 0.0f)
        {
            p.dotAccum += p.possessDps * TickDt;
            while (p.dotAccum >= 1.0f && p.health > 0)
            {
                p.dotAccum -= 1.0f;
                ApplyDamage(p.possessedBy, id, 1, 0);
            }
        }

        // NECROMANCER: launch a skull at the nearest enemy every SkullRate
        if (HasClass(p, ClassNecro))
        {
            p.skullWait = std::max(0.0f, p.skullWait - TickDt);
            if (p.skullWait <= 0.0f
                && NearestEnemyTank(*this, id, p.x, p.z) >= 0)
                for (SkullState& sk : skulls)
                {
                    if (sk.active)
                        continue;
                    sk = SkullState{};
                    sk.active = true;
                    sk.owner = uint8_t(id);
                    sk.x = p.x;
                    sk.z = p.z;
                    sk.yaw = p.turretYaw;
                    sk.life = HasUpgrade(p, UpgradeId::Poltergeist)
                        ? PoltergeistLife : 8.0f;
                    sk.dmg = p.stats[int(Stat::SkullDamage)];
                    p.skullWait = p.stats[int(Stat::SkullRate)];
                    break;
                }
        }

        // SOLDIER class summon: a fresh soldier every SoldierCooldown while
        // below the owner's SoldierMax (the first arrives immediately after
        // buying the card -- spawnWait starts at zero)
        if (HasClass(p, ClassSoldier))
        {
            p.soldierSpawnWait = std::max(0.0f, p.soldierSpawnWait - TickDt);
            int mine = 0;
            for (const SoldierState& s : soldiers)
                if (s.active && s.owner == id)
                    ++mine;
            // RICOCHET DRAFT: the tank itself fields ONE soldier only --
            // the rest of the army has to be drafted by ricochets
            int cap = HasUpgrade(p, UpgradeId::RicochetDraft)
                ? 1 : int(p.stats[int(Stat::SoldierMax)] + 0.5f);
            if (p.soldierSpawnWait <= 0.0f && mine < cap && SpawnSoldier(id))
                p.soldierSpawnWait = p.stats[int(Stat::SoldierCooldown)];
        }

        const InputCmd& in = inputs[id];

        // Server-side lag compensation: a freshly received direction change is
        // one-way-latency old, so the player has really been moving that way
        // for min(latency, 80 ms) already. Owe that distance and drain it over
        // ~2 ticks (a fast lerp, never a teleport); AdvanceMovement's clamps
        // and collision then apply to the caught-up position.
        if (lagCompEnabled && p.lagOneWayMs > 0.5f)
        {
            float nl2 = in.moveX * in.moveX + in.moveZ * in.moveZ;
            float pl2 = p.lastInMoveX * p.lastInMoveX + p.lastInMoveZ * p.lastInMoveZ;
            bool moving = nl2 > 0.25f;
            bool wasMoving = pl2 > 0.25f;
            bool newDirection = moving
                && (!wasMoving
                    || (in.moveX * p.lastInMoveX + in.moveZ * p.lastInMoveZ)
                           / sqrtf(nl2 * pl2) < 0.7f);
            if (newDirection)
            {
                float sec = std::min(p.lagOneWayMs, 80.0f) * 0.001f;
                float inv = 1.0f / sqrtf(nl2);
                float dist = p.stats[int(Stat::MoveSpeed)] * sec;
                p.catchupX += in.moveX * inv * dist;
                p.catchupZ += in.moveZ * inv * dist;
            }
        }
        p.lastInMoveX = in.moveX;
        p.lastInMoveZ = in.moveZ;
        if (p.catchupX != 0.0f || p.catchupZ != 0.0f)
        {
            float k = 0.55f;   // ~2-tick drain
            p.x += p.catchupX * k;
            p.z += p.catchupZ * k;
            p.catchupX *= (1.0f - k);
            p.catchupZ *= (1.0f - k);
            if (p.catchupX * p.catchupX + p.catchupZ * p.catchupZ < 1e-4f)
                p.catchupX = p.catchupZ = 0.0f;
        }

        AdvanceMovement(id, in);

        if ((in.buttons & BtnFire) && p.fireCooldown <= 0.0f
            && p.possessTimer <= 0.0f)
        {
            for (Projectile& pr : projectiles)
            {
                if (pr.active)
                    continue;
                XMFLOAT3 m = MuzzleWorld(p);
                pr = Projectile{};
                pr.active = true;
                pr.owner = uint8_t(id);
                pr.x = m.x; pr.y = m.y; pr.z = m.z;
                pr.yaw = p.turretYaw;
                pr.life = ProjectileLife;
                pr.speed = p.stats[int(Stat::ProjSpeed)];
                pr.damage = int(p.stats[int(Stat::Damage)] + 0.5f);
                pr.bounces = int(p.stats[int(Stat::Bounces)] + 0.5f);
                pr.bounceDmg = p.stats[int(Stat::BounceDamage)];
                pr.bounceSpd = p.stats[int(Stat::BounceSpeed)];
                pr.splitChance = p.stats[int(Stat::SplitChance)];
                if (HasClass(p, ClassRadar))
                {
                    pr.radarRange = p.stats[int(Stat::RadarRange)];
                    pr.radarDamage = p.stats[int(Stat::RadarDamage)];
                    pr.radarLockNeed = p.stats[int(Stat::RadarLock)];
                    pr.radarRings = std::clamp(
                        int(p.stats[int(Stat::RadarRings)] + 0.5f),
                        0, MaxRadarExtra);
                }
                p.fireCooldown = p.stats[int(Stat::ReloadTime)];
                p.muzzleFlash = 0.12f;
                break;
            }
        }
    }

    for (Projectile& pr : projectiles)
    {
        if (!pr.active)
            continue;
        int bouncesBefore = pr.bounces;
        if (!StepProjectile(pr, TickDt))
        {
            pr.active = false;
            continue;
        }
        // FISSION SHELLS: each bounce may split off ONE half-damage twin
        // exiting at deviated angles. REAL rockets can split on EVERY
        // bounce; the spawned twins are always sterile. Chance above 100%
        // spills into a chance for a SECOND twin on the same bounce
        // (120% = twin + 20% for another; 200% = always two twins).
        // RICOCHET DRAFT: every wall bounce of the tank's rockets has a 2%
        // chance to draft a soldier AT the ricochet (up to the 32 cap).
        // Deterministic hash, separate bits from the fission roll below.
        if (pr.bounces < bouncesBefore
            && HasUpgrade(players[pr.owner], UpgradeId::RicochetDraft))
        {
            int slot = int(&pr - &projectiles[0]);
            uint32_t dh = tick * 2246822519u ^ uint32_t(slot * 131 + 71);
            dh ^= dh << 13; dh ^= dh >> 17; dh ^= dh << 5;
            if (float(dh & 0xFFFF) / 65535.0f < RicochetDraftChance)
            {
                int mine = 0;
                for (const SoldierState& s : soldiers)
                    if (s.active && s.owner == pr.owner)
                        ++mine;
                if (mine < RicochetDraftCap)
                    SpawnSoldierAt(pr.owner, pr.x, pr.z);
            }
        }
        // RADAR MINES mutation (radar+bouncy): a bouncing ring rocket may
        // STAMP its root ring onto the wall it kissed -- 35% per bounce,
        // deterministic hash (third stream, independent of fission/draft)
        if (pr.bounces < bouncesBefore && pr.radarRange > 0.0f
            && HasUpgrade(players[pr.owner], UpgradeId::RadarMines))
        {
            int slot = int(&pr - &projectiles[0]);
            uint32_t mh = tick * 0x9E3779B9u ^ uint32_t(slot * 193 + 29);
            mh ^= mh << 13; mh ^= mh >> 17; mh ^= mh << 5;
            if (float(mh & 0xFFFF) / 65535.0f < RadarMineChance)
            {
                int free = -1, low = 0;
                for (int k = 0; k < MaxRadarMines; ++k)
                {
                    if (!radarMines[k].active) { free = k; break; }
                    if (radarMines[k].life < radarMines[low].life)
                        low = k;
                }
                RadarMineState& rm = radarMines[free >= 0 ? free : low];
                rm = RadarMineState{};
                rm.active = true;
                rm.owner = pr.owner;
                rm.x = pr.x;
                rm.z = pr.z;
                rm.life = RadarMineLife;
            }
        }
        if (pr.bounces < bouncesBefore && pr.splitChance > 0.0f)
        {
            int slot = int(&pr - &projectiles[0]);
            uint32_t h = tick * 2654435761u ^ uint32_t(slot * 97 + 13);
            h ^= h << 13; h ^= h >> 17; h ^= h << 5;
            uint32_t h2 = h * 2246822519u ^ 0x9E3779B9u;
            h2 ^= h2 >> 15; h2 *= 2654435761u; h2 ^= h2 >> 13;
            float p1 = std::min(pr.splitChance, 1.0f);
            float p2 = std::clamp(pr.splitChance - 1.0f, 0.0f, 1.0f);
            float roll1 = float(h & 0xFFFF) / 65535.0f;
            float roll2 = float(h2 & 0xFFFF) / 65535.0f;
            int spawn = (roll1 < p1 ? 1 : 0) + (roll2 < p2 ? 1 : 0);
            for (int k = 0; k < spawn; ++k)
                for (Projectile& c : projectiles)
                {
                    if (c.active)
                        continue;
                    c = pr;
                    c.damage = std::max(1, pr.damage / 2);
                    c.splitChance = 0.0f;      // twins never split
                    c.radarRange = 0.0f;       // and carry no radar tree
                    c.radarLock = 0.0f;
                    c.radarLockFrac = 0.0f;
                    c.radarRings = 0;
                    float dev = (k == 0 ? 0.28f : 0.52f)
                              * (((h >> 16 >> k) & 1) ? 1.0f : -1.0f);
                    c.yaw = WrapAngle(pr.yaw + dev);
                    break;
                }
        }
        // RADAR rockets scan while flying; a full ring lock detonates
        if (pr.radarRange > 0.0f)
        {
            TickRadar(*this, pr);
            if (!pr.active)
                continue;
        }
        ShieldDeflectStep(*this, pr);
        // POLTERGEIST skulls are shot down like drones -- and THAT is the
        // moment their acid finally lands. Any shell not fighting for the
        // skull's owner counts (deflected orange ones included).
        for (SkullState& sk : skulls)
        {
            if (!sk.active || sk.owner == pr.owner
                || !HasUpgrade(players[sk.owner], UpgradeId::Poltergeist))
                continue;
            float dx = sk.x - pr.x, dz = sk.z - pr.z;
            float r = SkullRadius + ProjectileRadius;
            if (dx * dx + dz * dz < r * r)
            {
                SpawnPuddle(*this, sk.owner, sk.x, sk.z);
                sk.active = false;
                pr.active = false;   // the shell is spent on the kill
                break;
            }
        }
        if (!pr.active)
            continue;
        bool spent = false;
        for (int id = 0; id < MaxPlayers && !spent; ++id)
        {
            PlayerState& t = players[id];
            if (!t.active || t.health <= 0 || id == pr.owner)
                continue;
            float dx = pr.x - t.x, dz = pr.z - t.z;
            if (dx * dx + dz * dz < TankRadius * TankRadius && pr.y < 2.2f)
            {
                // RADAR rockets: a direct hit detonates the RANGES, not the
                // bare rocket -- the whole tree blows at the impact point
                // (the victim sits in the root circle, so it takes at least
                // the full rocket damage + PAYLOAD; anyone else in the
                // rings shares the pain). Less precision required.
                if (pr.radarRange > 0.0f)
                    DetonateRadar(*this, pr);
                else
                {
                    ApplyDamage(pr.owner, id, pr.damage, 5);
                    pr.active = false;
                }
                spent = true;
            }
        }
        // rockets also detonate on enemy soldiers (never the owner's own);
        // a soldier kill pays a small bounty, no score
        for (SoldierState& s : soldiers)
        {
            if (spent)
                break;
            if (!s.active || s.state >= SoldierDying || s.owner == pr.owner)
                continue;
            float dx = pr.x - s.x, dz = pr.z - s.z;
            float r = SoldierRadius + ProjectileRadius;
            if (dx * dx + dz * dz < r * r && pr.y < 2.2f)
            {
                if (pr.radarRange > 0.0f)
                {
                    DetonateRadar(*this, pr);
                    spent = true;
                    break;
                }
                s.health -= float(pr.damage);
                s.lastHitBy = pr.owner;
                s.hitFlash = 0.3f;
                if (s.health <= 0.0f && pr.owner < MaxPlayers
                    && players[pr.owner].active)
                    players[pr.owner].money =
                        uint16_t(std::min(999, players[pr.owner].money + 10));
                pr.active = false;
                spent = true;
            }
        }
    }

    for (SoldierState& s : soldiers)
        if (s.active)
            TickSoldier(s);
    for (GrenadeState& gr : grenades)
        if (gr.active)
            TickGrenade(*this, gr);
    for (SkullState& sk : skulls)
        if (sk.active)
            TickSkull(*this, sk);
    for (AcidBallState& ab : acidBalls)
        if (ab.active)
            TickAcidBall(*this, ab);
    for (RadarMineState& rm : radarMines)
        if (rm.active)
            TickRadarMine(*this, rm);
    for (PuddleState& pu : puddles)
        if (pu.active)
            TickPuddle(*this, pu);
    for (GhostState& gh : ghosts)
        if (gh.active)
            TickGhost(*this, gh);
}

} // namespace tankaq
