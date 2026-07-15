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

    Log("classtest done: %d failure(s)", fails);
    return fails;
}

} // namespace tankaq
