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
};

// The pool: mods may mix additive and multiplicative parts, and one upgrade
// can touch several stats (including tradeoffs).
#define S(x) Stat::x
const UpgradeType kUpgradePool[] = {
    { "ENGINE",    "+15% MOVE SPEED",                 0, 30, 0,
      { { S(MoveSpeed), 0, 1.15f } }, 1 },
    { "TURBO",     "+1.5 MOVE SPEED",                 1, 40, 1,
      { { S(MoveSpeed), 1.5f, 1 } }, 1 },
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
    { "OVERDRIVE", "+25% SPEED, +10% DMG TAKEN",      4, 90, 10,
      { { S(MoveSpeed), 0, 1.25f }, { S(DamageTaken), 0, 1.10f } }, 2 },
    { "FIELD KIT", "+15 MAX HP, +0.5 SPEED",          0, 35, 11,
      { { S(MaxHealth), 15, 1 }, { S(MoveSpeed), 0.5f, 1 } }, 2 },
};
#undef S
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
}

void GameState::SpawnPlayer(int id)
{
    PlayerState& p = players[id];
    p = PlayerState{};
    p.active = true;
    SpawnPoint(id, p.x, p.z, p.hullYaw);
    p.turretYaw = p.hullYaw;
    RecalcStats(id);
    p.health = MaxHealthFor(p);
    p.nextOfferTick = tick + TickRate + uint32_t(id) * (TickRate / 4);
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
    // rarity roll: 40/25/18/11/6 %
    uint32_t r = NextRand() % 100;
    int rarity = r < 40 ? 0 : r < 65 ? 1 : r < 83 ? 2 : r < 94 ? 3 : 4;
    // uniform pick among pool entries of that rarity (fall back to any)
    int candidates[32];
    int n = 0;
    for (int i = 0; i < UpgradePoolSize && n < 32; ++i)
        if (kUpgradePool[i].rarity == rarity)
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
    if (len2 > 1e-6f)
    {
        float len = sqrtf(len2);
        if (len > 1.0f) { dx /= len; dz /= len; }
        float speed = p.stats[int(Stat::MoveSpeed)];
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

    p.turretYaw = MoveTowardsAngle(p.turretYaw, in.turretYaw, TurretTurnSpeed * TickDt);
}

void GameState::Tick(const InputCmd* inputs)
{
    ++tick;

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

        // passive income: 2 credits per second while alive
        if (tick % (TickRate / 2) == 0 && p.money < 999)
            ++p.money;

        const InputCmd& in = inputs[id];
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
        if (pr.life <= 0.0f
            || fabsf(pr.x) > ArenaHalf || fabsf(pr.z) > ArenaHalf
            || PointHitsObstacle(pr.x, pr.y, pr.z, ProjectileRadius))
        {
            pr.active = false;
            continue;
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
                    shooter.money = uint16_t(std::min(999, shooter.money + 10));
                    if (t.health <= 0)
                    {
                        ++shooter.score;
                        shooter.money = uint16_t(std::min(999, shooter.money + 40));
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
