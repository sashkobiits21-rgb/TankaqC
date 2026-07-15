// Headless self-tests (--classtest). Pure sim exercises against GameState --
// no window, no renderer, no RNG luck. Exit code = failure count.
#include "AppState.h"
#include "Log.h"
#include <cmath>

using namespace DirectX;

namespace tankaq
{

int RunClassTest()
{
    GameState gs{};
    gs.SpawnPlayer(0);
    PlayerState& p = gs.players[0];
    int fails = 0;
    auto check = [&](bool ok, const char* what)
    {
        Log("classtest %s: %s", ok ? "PASS" : "FAIL", what);
        if (!ok) ++fails;
    };
    const char* poolErr = ValidateUpgradePool();
    if (poolErr) Log("classtest: pool error: %s", poolErr);
    check(poolErr == nullptr, "ValidateUpgradePool clean");
    auto freeConveyor = [&]()   // clear burn holds so offers insert directly
    {
        for (Offer& o : p.offers) o = Offer{};
        p.pendingOffers.clear();
    };
    auto rollTypes = [&](int rolls, bool& lockedSeen, bool& cardSeen,
                         bool& demoLockedSeen)
    {
        lockedSeen = cardSeen = demoLockedSeen = false;
        for (int i = 0; i < rolls; ++i)
        {
            freeConveyor();
            gs.GenerateOffer(0);
            const UpgradeType& u = kUpgradePool[p.offers[0].type];
            if (u.classReq != ClassNone && !HasClass(p, u.classReq))
                lockedSeen = true;
            if (u.classReq == ClassBouncy)
                demoLockedSeen = true;
            if (u.classGrant != ClassNone)
                cardSeen = true;
        }
    };

    bool locked, card, demo;
    rollTypes(400, locked, card, demo);
    check(!locked, "no class-locked upgrade offered before owning a class");
    check(card, "class cards do appear in the offer stream");

    // buy the SOLDIER card through the real purchase path
    const uint8_t soldierIdx = uint8_t(UpgradeId::SoldierClass);
    const uint8_t bouncyIdx = uint8_t(UpgradeId::BouncyClass);
    freeConveyor();
    p.money = 999;
    Offer o;
    o.active = OfferActive; o.id = 1; o.type = soldierIdx; o.cost = 80;
    gs.InsertOffer(0, o);
    // a second class card sits on the conveyor during the purchase: buying
    // the first must burn it (consumed slot) rather than leave it purchasable
    Offer o2 = o;
    o2.id = 90; o2.type = bouncyIdx;
    gs.InsertOffer(0, o2);   // slot 0 = bouncy, slot 1 = soldier
    check(gs.TryPurchase(0, 1), "SOLDIER card purchase succeeds");
    check(p.owned.size() == 2, "card grants its base upgrade (owned == 2)");
    check(p.offers[0].active == OfferConsumed,
          "other class card on the conveyor burned by the purchase");
    check(!gs.TryPurchase(0, 0), "burned class card is not purchasable");
    check(fabsf(p.stats[int(Stat::SoldierCooldown)] - 8.82f) < 1e-3f,
          "granted RECRUITER applied add-first-mult-last ((10-0.2)*0.9)");
    check(HasClass(p, ClassSoldier) && CountClasses(p) == 1,
          "class ownership derived from owned list");

    // soldier-locked upgrades now offered; bouncy still locked; the owned
    // card never re-offered
    bool soldierSeen = false, cardAgain = false;
    for (int i = 0; i < 600; ++i)
    {
        freeConveyor();
        gs.GenerateOffer(0);
        const UpgradeType& u = kUpgradePool[p.offers[0].type];
        if (u.classReq == ClassSoldier) soldierSeen = true;
        if (u.classReq == ClassBouncy && !HasClass(p, ClassBouncy))
            demo = true;
        if (p.offers[0].type == soldierIdx) cardAgain = true;
    }
    check(soldierSeen, "soldier-locked upgrades offered after unlock");
    check(!demo, "bouncy-locked upgrades still gated");
    check(!cardAgain, "owned class card never re-offered");

    // second class, then the cap: no class card may ever appear again
    freeConveyor();
    p.money = 999;
    o.id = 2; o.type = bouncyIdx;
    gs.InsertOffer(0, o);
    check(gs.TryPurchase(0, 0), "BOUNCY card purchase succeeds");
    freeConveyor();
    p.money = 999;
    o.id = 3; o.type = soldierIdx;   // stale: already owned + at the cap
    gs.InsertOffer(0, o);
    check(!gs.TryPurchase(0, 0), "stale class card rejected at purchase time");
    freeConveyor();
    check(fabsf(p.stats[int(Stat::Bounces)] - 1.0f) < 1e-4f,
          "granted RICOCHET applied (1 bounce)");
    check(CountClasses(p) == kMaxClasses, "two classes owned");
    bool cardAfterCap = false, bouncySeen = false;
    for (int i = 0; i < 600; ++i)
    {
        freeConveyor();
        gs.GenerateOffer(0);
        const UpgradeType& u = kUpgradePool[p.offers[0].type];
        if (u.classGrant != ClassNone) cardAfterCap = true;
        if (u.classReq == ClassBouncy) bouncySeen = true;
    }
    check(!cardAfterCap, "no class card offered at the 2-class cap");
    check(bouncySeen, "bouncy-locked upgrades offered after unlock");

    // per-bounce multipliers: a fired rocket ricochets once and both its
    // speed and damage scale by the baked stats
    {
        p.owned.push_back(uint8_t(UpgradeId::RubberShells));   // x1.15 dmg/bounce
        p.owned.push_back(uint8_t(UpgradeId::Slingshot));      // x1.12 spd/bounce
        gs.RecalcStats(0);
        Projectile pr{};
        pr.active = true; pr.owner = 0;
        pr.x = ArenaHalf - 0.2f; pr.z = 0; pr.y = 0.17f;
        pr.yaw = XM_PI * 0.5f;   // heading +X into the wall
        pr.life = 1.0f; pr.speed = 10.0f; pr.damage = 100; pr.bounces = 1;
        pr.bounceDmg = p.stats[int(Stat::BounceDamage)];
        pr.bounceSpd = p.stats[int(Stat::BounceSpeed)];
        gs.projectiles[0] = pr;
        gs.phase = PhasePlaying;
        gs.matchEndTick = gs.tick + 100000;
        InputCmd idle[MaxPlayers]{};
        for (int t = 0; t < 8; ++t) gs.Tick(idle);
        const Projectile& out = gs.projectiles[0];
        check(out.active && out.bounces == 0, "test rocket ricocheted once");
        check(fabsf(out.speed - 11.2f) < 1e-3f, "SLINGSHOT speed applied on bounce");
        check(out.damage == 115, "RUBBER SHELLS damage applied on bounce");
    }

    // ---- soldier summon: spawn cadence, engagement, death, replacement ----
    {
        GameState g2{};
        g2.SpawnPlayer(0);
        g2.SpawnPlayer(1);   // enemy tank, parked
        PlayerState& owner = g2.players[0];
        owner.owned.push_back(uint8_t(UpgradeId::SoldierClass));
        g2.RecalcStats(0);
        g2.phase = PhasePlaying;
        g2.matchEndTick = g2.tick + 100000000u;
        InputCmd idle[MaxPlayers]{};
        auto countMine = [&]()
        {
            int n = 0;
            for (const SoldierState& s : g2.soldiers)
                if (s.active && s.owner == 0) ++n;
            return n;
        };

        g2.Tick(idle);
        check(countMine() == 1, "soldier spawns immediately after unlock");
        // 20 s: the worst-case spawn pocket needs a multi-hop detour around
        // its own obstacle + parked owner tank, then a peek, before the
        // first shot lands
        for (int t = 0; t < TickRate * 20; ++t) g2.Tick(idle);
        check(countMine() == 1, "SoldierMax 1 respected across the cooldown");
        SoldierState* s0 = nullptr;
        for (SoldierState& s : g2.soldiers) if (s.active) s0 = &s;
        check(s0 && fabsf(s0->health - 30.0f) < 1e-3f,
              "soldier health baked from the owner's stats");
        check(s0 && fabsf(s0->x) <= ArenaHalf && fabsf(s0->z) <= ArenaHalf,
              "soldier stayed inside the arena");
        check(s0 && (s0->state == SoldierCover || s0->state == SoldierMove
                     || s0->state == SoldierKite),
              "soldier engaged (cover/move/kite) with an enemy present");
        bool enemyHurt = g2.players[1].health < MaxHealthFor(g2.players[1])
                      || owner.score > 0;
        check(enemyHurt, "soldier fire damaged the enemy tank");

        if (s0)
        {
            s0->health = 0;   // execute it
            g2.Tick(idle);
            check(s0->state == SoldierDying, "soldier enters dying at 0 hp");
            for (int t = 0; t < TickRate * 2; ++t) g2.Tick(idle);
            // the cooldown may already be up, in which case the freed slot is
            // recycled by a REPLACEMENT within a tick: gone means either an
            // empty slot or a fresh full-health soldier occupying it
            bool despawned = !s0->active
                          || (s0->state != SoldierDying && s0->health >= 29.0f);
            check(despawned, "dead soldier despawned (slot freed or recycled)");
            for (int t = 0; t < TickRate * 11; ++t) g2.Tick(idle);
            check(countMine() == 1, "replacement arrives after the cooldown");
        }
    }

    // ---- necromancer: skulls -> acid, ghosts -> possession ----
    {
        GameState g3{};
        g3.SpawnPlayer(0);
        g3.SpawnPlayer(1);
        PlayerState& necro = g3.players[0];
        necro.owned.push_back(uint8_t(UpgradeId::NecroClass));
        g3.RecalcStats(0);
        g3.phase = PhasePlaying;
        g3.matchEndTick = g3.tick + 100000000u;
        InputCmd idle[MaxPlayers]{};

        // clear lane: the skull must BITE the tank (5 base) then puddle
        g3.players[0].x = 0; g3.players[0].z = -24;
        g3.players[1].x = 0; g3.players[1].z = -12;
        g3.Tick(idle);
        bool skullUp = false;
        for (const SkullState& sk : g3.skulls) skullUp |= sk.active;
        check(skullUp, "skull launched at the nearest enemy");
        int hpPre = g3.players[1].health;
        bool puddleUp = false;
        for (int t = 0; t < TickRate * 4; ++t)
        {
            g3.Tick(idle);
            for (const PuddleState& pu : g3.puddles) puddleUp |= pu.active;
        }
        check(g3.players[1].health <= hpPre - 5,
              "skull bite lands its 5 base contact damage");
        check(puddleUp, "skull burst into an acid puddle on contact");

        // park the enemy in a fresh puddle: DoT must tick at ~AcidDps
        PlayerState& victim = g3.players[1];
        for (PuddleState& pu : g3.puddles) pu.active = false;
        for (SkullState& sk : g3.skulls) sk.active = false;
        necro.owned.clear();           // stop new skulls interfering
        g3.RecalcStats(0);
        int hpBefore = victim.health;
        g3.puddles[0].active = true;
        g3.puddles[0].owner = 0;
        g3.puddles[0].x = victim.x;
        g3.puddles[0].z = victim.z;
        g3.puddles[0].life = 3.0f;
        g3.puddles[0].dps = 4.0f;
        for (int t = 0; t < TickRate * 2; ++t) g3.Tick(idle);
        check(victim.health <= hpBefore - 7, "acid burns ~4/s while inside");

        // ghost: spiral in, possess, chaos-drive, expire
        g3.ghosts[0] = GhostState{};
        g3.ghosts[0].active = true;
        g3.ghosts[0].owner = 0;
        g3.ghosts[0].x = victim.x + 3.0f;
        g3.ghosts[0].z = victim.z;
        hpBefore = victim.health;
        float px0 = victim.x, pz0 = victim.z;
        bool possessedSeen = false;
        for (int t = 0; t < TickRate * 6; ++t)
        {
            g3.Tick(idle);
            possessedSeen |= victim.possessTimer > 0.0f;
        }
        check(possessedSeen, "ghost closed in and possessed the enemy");
        check(fabsf(victim.x - px0) > 0.5f || fabsf(victim.z - pz0) > 0.5f,
              "possessed tank drives itself (deterministic chaos)");
        check(victim.health <= hpBefore - 4, "possession burns ~3/s for 2s");
        check(victim.possessTimer <= 0.0f, "possession expires");

        // a necromancer kill raises a ghost
        necro.owned.push_back(uint8_t(UpgradeId::NecroClass));
        g3.RecalcStats(0);
        for (GhostState& gh : g3.ghosts) gh.active = false;
        g3.ApplyDamage(0, 1, 10000, 0);
        bool ghostUp = false;
        for (const GhostState& gh : g3.ghosts) ghostUp |= gh.active;
        check(ghostUp, "a ghost rises from the necromancer's kill");
    }

    // ---- radar: ring lock detonates without contact; nested rings stack --
    {
        GameState g4{};
        g4.SpawnPlayer(0);
        g4.SpawnPlayer(1);
        PlayerState& op = g4.players[0];
        op.owned.push_back(uint8_t(UpgradeId::RadarClass));
        g4.RecalcStats(0);
        g4.phase = PhasePlaying;
        g4.matchEndTick = g4.tick + 100000000u;
        InputCmd idle[MaxPlayers]{};
        PlayerState& tgt = g4.players[1];
        tgt.x = 0; tgt.z = -10;        // parked in the open

        auto fireRadar = [&](float zOffset)
        {
            Projectile pr{};
            pr.active = true;
            pr.owner = 0;
            pr.x = -12.0f;
            pr.z = tgt.z + zOffset;
            pr.y = 0.17f;
            pr.yaw = XM_PI * 0.5f;     // flying +X, passing the target
            pr.life = 6.0f;
            pr.speed = 5.0f;           // slow: guarantees lock time
            pr.damage = 34;
            pr.radarRange = op.stats[int(Stat::RadarRange)];
            pr.radarDamage = op.stats[int(Stat::RadarDamage)];
            pr.radarLockNeed = op.stats[int(Stat::RadarLock)];
            pr.radarRings = 1;
            g4.projectiles[0] = pr;
        };

        // offset 3.0: inside the ROOT circle only -> exactly RadarDamage
        int hp0 = tgt.health;
        fireRadar(3.0f);
        bool detonated = false;
        for (int t = 0; t < TickRate * 5 && !detonated; ++t)
        {
            g4.Tick(idle);
            detonated = !g4.projectiles[0].active;
        }
        check(detonated, "radar rocket detonated from ring lock (no contact)");
        check(tgt.health == hp0 - 34,
              "root-only victim takes the rocket's own damage");

        // a rocket parked 1.8 away with ONE packed circle (which centers
        // on its parent): the victim sits inside root (34) AND the child
        // (half depth = 17) -- tree damage stacks to 51
        hp0 = tgt.health;
        {
            Projectile pr{};
            pr.active = true;
            pr.owner = 0;
            pr.x = tgt.x;
            pr.z = tgt.z - 1.8f;
            pr.y = 0.17f;
            pr.yaw = 0;
            pr.life = 6.0f;
            pr.speed = 0.0f;           // hovering: pure ring-lock test
            pr.damage = 34;
            pr.radarRange = op.stats[int(Stat::RadarRange)];
            pr.radarDamage = op.stats[int(Stat::RadarDamage)];
            pr.radarLockNeed = op.stats[int(Stat::RadarLock)];
            pr.radarRings = 1;
            g4.projectiles[0] = pr;
        }
        detonated = false;
        for (int t = 0; t < TickRate * 5 && !detonated; ++t)
        {
            g4.Tick(idle);
            detonated = !g4.projectiles[0].active;
        }
        check(detonated, "nested radar rocket detonated");
        check(tgt.health == hp0 - 51,
              "tree damage: rocket 34 + half-depth child 17");
    }

    Log("classtest done: %d failure(s)", fails);
    return fails;
}

} // namespace tankaq
