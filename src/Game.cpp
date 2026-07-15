#include "Game.h"
#include <cmath>
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
    40.0f,              // SoldierHealth
    1.0f,               // SoldierFireRate: shots/s
    1.0f,               // SoldierMax: one soldier out at a time
    10.0f,              // SoldierCooldown: 10 s between spawns
    1.0f,               // BounceDamage: x1 per ricochet until upgraded
    1.0f,               // BounceSpeed
};

int gDebugBounces = 0;   // --bounces=N dev knob (added on top of the stat)

// The pool: mods may mix additive and multiplicative parts, and one upgrade
// can touch several stats (including tradeoffs).
//
// Class-locked entries carry classReq and are only offered to players who own
// that class's card. Buying a card unlocks the family and immediately grants
// its base upgrade (grantUpgrade -- keep these indices in sync with the pool).
constexpr int kIdxRicochet = 12;
constexpr int kIdxRecruiter = 21;
#define S(x) Stat::x
const UpgradeType kUpgradePool[] = {
    { "ENGINE",    "+5% MOVE SPEED",                  0, 30, 0,
      { { S(MoveSpeed), 0, 1.05f } }, 1 },
    { "TURBO",     "+0.5 MOVE SPEED",                 1, 40, 1,
      { { S(MoveSpeed), 0.5f, 1 } }, 1 },
    { "AP ROUNDS", "+20% SHELL DAMAGE",               2, 55, 2,
      { { S(Damage), 0, 1.20f } }, 1 },
    { "HEAVY SHELLS", "+12 DAMAGE, -10% SHELL SPEED", 2, 50, 3,
      { { S(Damage), 12, 1 }, { S(ProjSpeed), 0, 0.90f } }, 2 },
    { "AUTOLOADER", "-15% RELOAD TIME",               2, 55, 4,
      { { S(ReloadTime), 0, 0.85f } }, 1 },
    { "GREASED BREECH", "-0.1s RELOAD",               0, 25, 5,
      { { S(ReloadTime), -0.1f, 1 } }, 1 },
    { "PLATING",   "+30 MAX HEALTH",                  1, 40, 6,
      { { S(MaxHealth), 30, 1 } }, 1 },
    { "COMPOSITE", "+20% MAX HP, -5% SPEED",          3, 70, 7,
      { { S(MaxHealth), 0, 1.20f }, { S(MoveSpeed), 0, 0.95f } }, 2 },
    { "GYRO",      "+15% SHELL SPEED",                3, 60, 8,
      { { S(ProjSpeed), 0, 1.15f } }, 1 },
    { "REACTIVE ARMOR", "-10% DAMAGE TAKEN",          4, 85, 9,
      { { S(DamageTaken), 0, 0.90f } }, 1 },
    { "OVERDRIVE", "+8% SPEED, +10% DMG TAKEN",       4, 90, 10,
      { { S(MoveSpeed), 0, 1.08f }, { S(DamageTaken), 0, 1.10f } }, 2 },
    { "FIELD KIT", "+15 MAX HP, +0.15 SPEED",         0, 35, 11,
      { { S(MaxHealth), 15, 1 }, { S(MoveSpeed), 0.15f, 1 } }, 2 },
    { "RICOCHET",  "+1 WALL BOUNCE",                  2, 55, 12,
      { { S(Bounces), 1, 1 } }, 1, ClassBouncy },
    { "SUPERBALL", "+2 WALL BOUNCES, -15% DAMAGE",    4, 90, 13,
      { { S(Bounces), 2, 1 }, { S(Damage), 0, 0.85f } }, 2, ClassBouncy },
    { "NITRO TANK", "+0.5s BOOST FUEL",               2, 55, 14,
      { { S(BoostFuel), 0.5f, 1 } }, 1 },
    { "AFTERBURNER", "+20% BOOST SPEED",              3, 70, 15,
      { { S(BoostSpeed), 0, 1.20f } }, 1 },
    { "QUICK PUMP", "+0.3/s FUEL REGEN",              1, 40, 16,
      { { S(BoostRegen), 0.3f, 1 } }, 1 },
    { "PIT CREW",  "-0.4s REGEN DELAY",               0, 30, 17,
      { { S(BoostRegenDelay), -0.4f, 1 } }, 1 },
    { "FUEL INJECTION", "+25% FUEL REGEN",            2, 55, 18,
      { { S(BoostRegen), 0, 1.25f } }, 1 },
    // class cards: no mods of their own -- the granted base upgrade carries
    // the stats, the card itself is the unlock marker in `owned`
    { "SOLDIER CLASS", "UNLOCK SOLDIERS +RECRUITER",  RarityClass, 80, 19,
      {}, 0, ClassNone, ClassSoldier, kIdxRecruiter },
    { "BOUNCY CLASS", "UNLOCK BOUNCES +RICOCHET",     RarityClass, 80, 20,
      {}, 0, ClassNone, ClassBouncy, kIdxRicochet },
    // SOLDIER family
    { "RECRUITER", "-0.2s & -10% SPAWN TIME",         2, 55, 21,
      { { S(SoldierCooldown), -0.2f, 0.90f } }, 1, ClassSoldier },
    { "DOUBLE TIME", "+15% SOLDIER SPEED",            1, 40, 22,
      { { S(SoldierSpeed), 0, 1.15f } }, 1, ClassSoldier },
    { "FLAK VEST", "+15 SOLDIER HEALTH",              1, 40, 23,
      { { S(SoldierHealth), 15, 1 } }, 1, ClassSoldier },
    { "HOLLOW POINTS", "+25% SOLDIER DAMAGE",         2, 55, 24,
      { { S(SoldierDamage), 0, 1.25f } }, 1, ClassSoldier },
    { "RAPID FIRE", "+15% SOLDIER FIRE RATE",         2, 55, 25,
      { { S(SoldierFireRate), 0, 1.15f } }, 1, ClassSoldier },
    { "PLATOON",   "+1 MAX SOLDIERS",                 4, 95, 26,
      { { S(SoldierMax), 1, 1 } }, 1, ClassSoldier },
    // BOUNCY family (per-ricochet multipliers, multiplicative only)
    { "RUBBER SHELLS", "+15% DAMAGE PER BOUNCE",      2, 55, 27,
      { { S(BounceDamage), 0, 1.15f } }, 1, ClassBouncy },
    { "SLINGSHOT", "+12% SPEED PER BOUNCE",           2, 55, 28,
      { { S(BounceSpeed), 0, 1.12f } }, 1, ClassBouncy },
};
#undef S

bool HasClass(const PlayerState& p, uint8_t cls)
{
    for (uint8_t t : p.owned)
        if (kUpgradePool[t].classGrant == cls)
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
const int UpgradePoolSize = int(sizeof(kUpgradePool) / sizeof(kUpgradePool[0]));

const Obstacle kObstacles[NumObstacles] = {
    {   0.0f,   0.0f, 3.0f, 3.0f, 2.6f },   // center block
    {  14.0f,  10.0f, 4.5f, 1.2f, 2.0f },
    { -14.0f, -10.0f, 4.5f, 1.2f, 2.0f },
    { -14.0f,  12.0f, 1.2f, 4.5f, 2.2f },
    {  14.0f, -12.0f, 1.2f, 4.5f, 2.2f },
    {  -6.0f, -20.0f, 3.0f, 1.0f, 1.6f },
    {   6.0f,  20.0f, 3.0f, 1.0f, 1.6f },
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

static void SpawnPoint(int id, float& x, float& z, float& yaw)
{
    float ang = XM_2PI * float(id) / MaxPlayers + XM_PI / MaxPlayers;
    x = sinf(ang) * (ArenaHalf - 6.0f);
    z = cosf(ang) * (ArenaHalf - 6.0f);
    yaw = WrapAngle(ang + XM_PI);   // face arena center
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
    for (uint8_t type : p.owned)
    {
        const UpgradeType& u = kUpgradePool[type];
        for (int m = 0; m < u.modCount; ++m)
        {
            addTotal[int(u.mods[m].stat)] += u.mods[m].amount;
            mulTotal[int(u.mods[m].stat)] *= u.mods[m].factor;
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
    p.stats[int(Stat::BounceDamage)] = std::max(0.25f, p.stats[int(Stat::BounceDamage)]);
    p.stats[int(Stat::BounceSpeed)] = std::max(0.25f, p.stats[int(Stat::BounceSpeed)]);
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
    for (int id = 0; id < MaxPlayers; ++id)
        if (players[id].active)
            SpawnPlayer(id);   // full reset: money, upgrades, score, offers
    for (Projectile& pr : projectiles)
        pr.active = false;
    phase = PhasePlaying;
    winner = 0xFF;
    matchEndTick = tick + MatchDurationTicks;
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
            if (CountClasses(p) >= kMaxClasses)
                return false;
            for (uint8_t t : p.owned)
                if (t == i)
                    return false;   // never re-offer an owned class card
        }
        if (u.classReq != ClassNone && !HasClass(p, u.classReq))
            return false;
        return true;
    };

    // rarity roll: common 36 / uncommon 23 / rare 17 / CLASS 10 / epic 9 /
    // legendary 5 -- class cards sit between rare and epic as requested
    uint32_t r = NextRand() % 100;
    int rarity = r < 36 ? 0
               : r < 59 ? 1
               : r < 76 ? 2
               : r < 86 ? RarityClass
               : r < 95 ? 3 : 4;
    // uniform pick among eligible pool entries of that rarity; an empty class
    // band (both classes taken) degrades to rare, any other empty band falls
    // back to a uniform pick over everything eligible
    int candidates[32];
    int n = 0;
    auto gather = [&](int band)
    {
        n = 0;
        for (int i = 0; i < UpgradePoolSize && n < 32; ++i)
            if (kUpgradePool[i].rarity == band && eligible(i))
                candidates[n++] = i;
    };
    gather(rarity);
    if (n == 0 && rarity == RarityClass)
        gather(2);
    if (n == 0)
        for (int i = 0; i < UpgradePoolSize && n < 32; ++i)
            if (eligible(i))
                candidates[n++] = i;
    int type = (n > 0) ? candidates[NextRand() % n]
                       : int(NextRand() % UpgradePoolSize);

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

bool GameState::TryPurchase(int id, int slot)
{
    if (id < 0 || id >= MaxPlayers || slot < 0 || slot >= NumOfferSlots)
        return false;
    PlayerState& p = players[id];
    if (!p.active || p.offers[slot].active != OfferActive)
        return false;
    const Offer& o = p.offers[slot];
    if (p.money < o.cost)
        return false;
    p.money = uint16_t(p.money - o.cost);
    p.owned.push_back(o.type);
    // class cards also grant their base upgrade as a real owned copy (it
    // stacks and prices like any other; the host broadcasts it as a second
    // upgrade event so client owned lists stay identical)
    int grant = kUpgradePool[o.type].grantUpgrade;
    if (grant >= 0)
        p.owned.push_back(uint8_t(grant));

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

static bool PointHitsObstacle(float x, float y, float z, float radius)
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

// Screen-relative movement: W/S/A/D push the tank up/down/left/right on
// screen. The camera sits at -Z looking toward +Z with a right-handed
// view, which makes screen-right equal world -X (and screen-up +Z).
// The hull only *visually* turns to face the direction of travel.
void GameState::AdvanceMovement(int id, const InputCmd& in)
{
    PlayerState& p = players[id];
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
            speed *= p.stats[int(Stat::BoostSpeed)];
        p.x += dx * speed * TickDt;
        p.z += dz * speed * TickDt;
        p.hullYaw = MoveTowardsAngle(p.hullYaw, atan2f(dx, dz),
                                     HullFaceSpeed * TickDt);
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

    for (int id = 0; id < MaxPlayers; ++id)
    {
        PlayerState& p = players[id];
        if (!p.active)
            continue;

        p.fireCooldown = std::max(0.0f, p.fireCooldown - TickDt);
        p.hitFlash = std::max(0.0f, p.hitFlash - TickDt);
        p.muzzleFlash = std::max(0.0f, p.muzzleFlash - TickDt);

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

        // passive income: 1 credit per second while alive
        if (tick % TickRate == 0 && p.money < 999)
            ++p.money;

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

        if ((in.buttons & BtnFire) && p.fireCooldown <= 0.0f)
        {
            for (Projectile& pr : projectiles)
            {
                if (pr.active)
                    continue;
                XMFLOAT3 m = MuzzleWorld(p);
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
        pr.life -= TickDt;
        pr.x += sinf(pr.yaw) * pr.speed * TickDt;
        pr.z += cosf(pr.yaw) * pr.speed * TickDt;
        if (pr.life <= 0.0f)
        {
            pr.active = false;
            continue;
        }

        // Walls and obstacle boxes ricochet while bounces remain, else the
        // rocket detonates. Everything is axis-aligned, so a bounce mirrors
        // one direction component: dir = (sin yaw, cos yaw), X-face hit ->
        // yaw = -yaw, Z-face hit -> yaw = pi - yaw. Tanks never bounce --
        // the hit loop below always detonates on contact.
        {
            const float r = ProjectileRadius;
            bool dead = false;
            // BOUNCY class: every ricochet multiplies the rocket's damage and
            // speed by the owner's per-bounce stats (baked at fire time)
            auto onBounce = [&pr]()
            {
                pr.speed *= pr.bounceSpd;
                pr.damage = int(float(pr.damage) * pr.bounceDmg + 0.5f);
            };

            if (fabsf(pr.x) > ArenaHalf - r)
            {
                if (pr.bounces-- > 0)
                {
                    float lim = (pr.x > 0) ? ArenaHalf - r : r - ArenaHalf;
                    pr.x = 2.0f * lim - pr.x;      // reflect off the face
                    pr.yaw = WrapAngle(-pr.yaw);
                    onBounce();
                }
                else dead = true;
            }
            if (!dead && fabsf(pr.z) > ArenaHalf - r)
            {
                if (pr.bounces-- > 0)
                {
                    float lim = (pr.z > 0) ? ArenaHalf - r : r - ArenaHalf;
                    pr.z = 2.0f * lim - pr.z;
                    pr.yaw = WrapAngle(XM_PI - pr.yaw);
                    onBounce();
                }
                else dead = true;
            }
            if (!dead)
            {
                for (const Obstacle& o : kObstacles)
                {
                    if (pr.y > o.height)
                        continue;
                    float dx = pr.x - o.cx, dz = pr.z - o.cz;
                    float ex = o.hx + r, ez = o.hz + r;
                    if (fabsf(dx) >= ex || fabsf(dz) >= ez)
                        continue;
                    if (pr.bounces-- > 0)
                    {
                        // resolve along the shallower penetration axis and
                        // mirror that direction component
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
                    }
                    else dead = true;
                    break;
                }
            }
            if (dead)
            {
                pr.active = false;
                continue;
            }
        }
        for (int id = 0; id < MaxPlayers; ++id)
        {
            PlayerState& t = players[id];
            if (!t.active || t.health <= 0 || id == pr.owner)
                continue;
            float dx = pr.x - t.x, dz = pr.z - t.z;
            if (dx * dx + dz * dz < TankRadius * TankRadius && pr.y < 2.2f)
            {
                int dmg = int(pr.damage * t.stats[int(Stat::DamageTaken)] + 0.5f);
                t.health -= std::max(1, dmg);
                t.hitFlash = 0.35f;
                pr.active = false;
                if (pr.owner < MaxPlayers && players[pr.owner].active)
                {
                    PlayerState& shooter = players[pr.owner];
                    shooter.money = uint16_t(std::min(999, shooter.money + 5));
                    if (t.health <= 0)
                    {
                        ++shooter.score;
                        shooter.money = uint16_t(std::min(999, shooter.money + 100));
                        if (phase == PhaseOvertime)   // sudden death: first kill
                        {
                            phase = PhaseEnded;
                            winner = pr.owner;
                            endedTick = tick;
                        }
                    }
                }
                if (t.health <= 0)
                {
                    t.health = 0;
                    t.respawnTimer = RespawnTime;
                }
                break;
            }
        }
    }
}

} // namespace tankaq
