#include "Game.h"
#include <cmath>
#include <algorithm>

using namespace DirectX;

namespace tankaq
{

const UpgradeDef kUpgrades[NumUpgrades] = {
    { "ENGINE",   "+12% MOVE SPEED",        30, 0 },
    { "DAMAGE",   "+15% SHELL DAMAGE",      50, 2 },
    { "RELOAD",   "-12% RELOAD TIME",       50, 2 },
    { "PLATING",  "+25 MAX HEALTH",         40, 1 },
    { "VELOCITY", "+10% SHELL SPEED",       60, 3 },
    { "ARMOR",    "-8% DAMAGE TAKEN",       80, 4 },
};

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

void GameState::SpawnPlayer(int id)
{
    PlayerState& p = players[id];
    p = PlayerState{};
    p.active = true;
    SpawnPoint(id, p.x, p.z, p.hullYaw);
    p.turretYaw = p.hullYaw;
    p.health = MaxHealth;
}

bool GameState::TryPurchase(int id, int slot)
{
    if (id < 0 || id >= MaxPlayers || slot < 0 || slot >= NumUpgrades)
        return false;
    PlayerState& p = players[id];
    if (!p.active || p.upgrades[slot] >= MaxUpgradeLevel)
        return false;
    int cost = UpgradeCost(slot, p.upgrades[slot]);
    if (p.money < cost)
        return false;
    p.money = uint16_t(p.money - cost);
    ++p.upgrades[slot];
    if (slot == UpgHealth && p.health > 0)
        p.health += 25;   // plating grants its health immediately
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
        float speed = TankSpeed * (1.0f + 0.12f * p.upgrades[UpgEngine]);
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
                pr.speed = ProjectileSpeed * (1.0f + 0.10f * p.upgrades[UpgVelocity]);
                pr.damage = int(ProjectileDamage * (1.0f + 0.15f * p.upgrades[UpgDamage]));
                p.fireCooldown = FireCooldown * powf(0.88f, float(p.upgrades[UpgReload]));
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
                int dmg = int(pr.damage * (1.0f - 0.08f * t.upgrades[UpgArmor]));
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
