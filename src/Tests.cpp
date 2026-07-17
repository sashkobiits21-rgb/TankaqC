// Headless self-tests (--classtest). Pure sim exercises against GameState --
// no window, no renderer, no RNG luck. Exit code = failure count.
#include "AppState.h"
#include "net/Protocol.h"
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

    // ---- FISSION SHELLS: a bounce splits off one sterile half-damage twin
    {
        GameState g5{};
        g5.SpawnPlayer(0);
        g5.phase = PhasePlaying;
        g5.matchEndTick = g5.tick + 100000;
        Projectile pr{};
        pr.active = true; pr.owner = 0;
        pr.x = ArenaHalf - 0.2f; pr.z = 0; pr.y = 0.17f;
        pr.yaw = XM_PI * 0.5f;
        pr.life = 1.5f; pr.speed = 10.0f; pr.damage = 100; pr.bounces = 2;
        pr.splitChance = 2.0f;   // 200%: always TWO twins per bounce
        g5.projectiles[0] = pr;
        InputCmd idle[MaxPlayers]{};
        for (int t = 0; t < 8; ++t) g5.Tick(idle);
        int alive = 0, twin = -1;
        for (int i = 0; i < MaxProjectiles; ++i)
            if (g5.projectiles[i].active)
            {
                ++alive;
                if (i != 0) twin = i;
            }
        check(alive == 3, "200% chance splits off two twins on one bounce");
        check(twin >= 0 && g5.projectiles[twin].damage == 50,
              "twins carry half damage");
        check(twin >= 0 && g5.projectiles[twin].splitChance == 0.0f,
              "twins are sterile (never split again)");
        check(g5.projectiles[0].splitChance == 2.0f,
              "the REAL rocket keeps its split chance for later bounces");
        check(twin >= 0
              && fabsf(WrapAngle(g5.projectiles[twin].yaw
                                 - g5.projectiles[0].yaw)) > 0.1f,
              "twins exit at deviated angles");
        // the parent bounces AGAIN within its life: it splits again, and
        // only sterile twins accumulate (no chain reaction from twins)
        for (int t = 0; t < TickRate; ++t) g5.Tick(idle);
        int realWithChance = 0, sterile = 0;
        for (const Projectile& q : g5.projectiles)
            if (q.active)
                (q.splitChance > 0.0f ? realWithChance : sterile)++;
        check(realWithChance <= 1,
              "twins never became splitters themselves");
    }

    // ---- ghosts: 4 s fuse (escapable), soldiers rise for their killer ----
    {
        GameState g6{};
        g6.SpawnPlayer(0);
        g6.SpawnPlayer(1);
        g6.players[0].owned.push_back(uint8_t(UpgradeId::NecroClass));
        g6.RecalcStats(0);
        g6.phase = PhasePlaying;
        g6.matchEndTick = g6.tick + 100000000u;
        InputCmd idle[MaxPlayers]{};
        // a ghost far from its prey runs out of life before contact --
        // the 4 s fuse is the escape window (close spawns still connect)
        g6.ghosts[0] = GhostState{};
        g6.ghosts[0].active = true;
        g6.ghosts[0].owner = 0;
        g6.ghosts[0].x = g6.players[1].x + 14.0f;
        g6.ghosts[0].z = g6.players[1].z;
        for (int t = 0; t < TickRate * 4 + 8; ++t) g6.Tick(idle);
        check(!g6.ghosts[0].active, "ghost vanishes after its 4 s fuse");
        check(g6.players[1].possessTimer <= 0.0f,
              "distant ghost expired without possessing");
        // an enemy soldier killed by the necromancer's puddle rises
        for (GhostState& gh : g6.ghosts) gh.active = false;
        SoldierState& sl = g6.soldiers[0];
        sl = SoldierState{};
        sl.active = true;
        sl.owner = 1;
        sl.x = 0; sl.z = -10;
        sl.health = 2.0f;
        g6.puddles[0] = PuddleState{};
        g6.puddles[0].active = true;
        g6.puddles[0].owner = 0;
        g6.puddles[0].x = 0;
        g6.puddles[0].z = -10;
        g6.puddles[0].life = 3.0f;
        g6.puddles[0].dps = 6.0f;
        bool rose = false;
        for (int t = 0; t < TickRate * 2 && !rose; ++t)
        {
            g6.Tick(idle);
            for (const GhostState& gh : g6.ghosts)
                rose |= gh.active && gh.owner == 0;
        }
        check(rose, "a soldier slain by the necromancer rises as a ghost");
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

        // the countdown only runs with a live target inside: teleport the
        // victim out mid-charge and the lock must fully reset
        {
            Projectile pr{};
            pr.active = true;
            pr.owner = 0;
            pr.x = tgt.x;
            pr.z = tgt.z - 1.8f;
            pr.y = 0.17f;
            pr.yaw = 0;
            pr.life = 30.0f;
            pr.speed = 0.0f;
            pr.damage = 34;
            pr.radarRange = op.stats[int(Stat::RadarRange)];
            pr.radarDamage = op.stats[int(Stat::RadarDamage)];
            pr.radarLockNeed = 5.0f;   // long fuse: no detonation here
            pr.radarRings = 1;
            g4.projectiles[0] = pr;
        }
        for (int t = 0; t < 8; ++t) g4.Tick(idle);
        check(g4.projectiles[0].radarLock > 0.0f,
              "lock charges while a target sits in range");
        tgt.x = 60.0f; tgt.z = 60.0f;    // step out of every ring
        g4.Tick(idle);
        check(g4.projectiles[0].active
                  && g4.projectiles[0].radarLock == 0.0f
                  && g4.projectiles[0].radarLockFrac == 0.0f,
              "lock resets the moment no target is in range");
        g4.projectiles[0].active = false;
    }

    // ---- FRAG PACK: soldiers lob physics grenades over the field ----
    {
        GameState g9{};
        g9.SpawnPlayer(0);
        g9.SpawnPlayer(1);
        g9.phase = PhasePlaying;
        g9.matchEndTick = g9.tick + 100000000u;
        InputCmd idle[MaxPlayers]{};
        PlayerState& own = g9.players[0];
        own.owned.push_back(uint8_t(UpgradeId::SoldierClass));
        own.owned.push_back(uint8_t(UpgradeId::FragPack));
        g9.RecalcStats(0);
        check(int(own.stats[int(Stat::GrenadeCount)] + 0.5f) == 2,
              "FRAG PACK arms soldiers with 2 grenades per life");
        own.x = 0; own.z = -24;
        g9.players[1].x = 0; g9.players[1].z = -16;   // 8 u: lob range
        check(g9.SpawnSoldier(0), "soldier spawned for the grenade test");
        bool seen = false, armedMidair = false;
        for (int t = 0; t < TickRate * 8; ++t)
        {
            g9.Tick(idle);
            for (const GrenadeState& gr : g9.grenades)
            {
                seen |= gr.active;
                armedMidair |= gr.active && gr.fuse >= 0.0f;
            }
        }
        check(seen, "soldier lobbed a grenade");
        check(armedMidair, "first bounce armed the 2 s fuse");
        check(g9.soldiers[0].active && g9.soldiers[0].grenades == 0,
              "both grenades expended (2 per life, cooldown paced)");

        // a manual drop right beside the enemy: exact blast arithmetic
        // (2x the soldier rocket damage), no rockets muddying the water
        for (SoldierState& s : g9.soldiers) s.active = false;
        for (GrenadeState& gr : g9.grenades) gr.active = false;
        g9.players[0].soldierSpawnWait = 9999.0f;   // no fresh auto-summons
        g9.players[1].health = 100;   // phase-1 kills reset via respawn
        int hp1 = g9.players[1].health;
        GrenadeState& gr = g9.grenades[0];
        gr = GrenadeState{};
        gr.active = true;
        gr.owner = 0;
        gr.x = g9.players[1].x;
        gr.z = g9.players[1].z - 2.2f;   // clear of the hull collider
        gr.y = 1.0f;
        gr.vy = -4.0f;
        gr.dmg = 20.0f;   // = 2 x the 10-damage base rocket
        for (int t = 0; t < TickRate * 3; ++t) g9.Tick(idle);
        check(!g9.grenades[0].active, "grenade exploded 2 s after impact");
        check(g9.players[1].health == hp1 - 20,
              "blast hits for exactly double the rocket damage");
    }

    // ---- SHIELD PROFICIENCY: raise, slow, deflect, allegiance flip ----
    {
        GameState gsh{};
        gsh.SpawnPlayer(0);   // shieldman
        gsh.SpawnPlayer(1);   // shooter
        gsh.phase = PhasePlaying;
        gsh.matchEndTick = gsh.tick + 100000000u;
        PlayerState& sm = gsh.players[0];
        sm.owned.push_back(uint8_t(UpgradeId::ShieldClass));
        sm.owned.push_back(uint8_t(UpgradeId::WideBarrier));   // card grant
        gsh.RecalcStats(0);
        sm.x = 0; sm.z = -24; sm.turretYaw = 0;      // clear lane, facing +Z
        gsh.players[1].x = 0; gsh.players[1].z = -14;
        InputCmd cmds[MaxPlayers]{};
        cmds[0].buttons = BtnAbility1;
        cmds[0].turretYaw = 0;
        gsh.Tick(cmds);
        check(sm.shieldTimer > 3.5f, "pressing 1 raises the 4 s barrier");
        check(sm.shieldWait <= 0.0f,
              "the cooldown holds off while the barrier is up");
        cmds[0].buttons = 0;

        // 35% slow while raised: half a second of reverse along the lane
        float z0 = sm.z;
        cmds[0].moveZ = -1.0f;
        for (int t = 0; t < TickRate / 2; ++t) gsh.Tick(cmds);
        float moved = fabsf(sm.z - z0);
        cmds[0].moveZ = 0;
        check(moved > 1.9f && moved < 2.7f,
              "the raised shield drives at 65% speed");

        // enemy rocket head-on: deflect, flip, orange, +1 bounce, fly back
        Projectile pr{};
        pr.active = true;
        pr.owner = 1;
        pr.x = 0; pr.z = -18.0f; pr.y = 0.17f;
        pr.yaw = XM_PI;              // straight at the shieldman
        pr.speed = 10.0f;
        pr.life = 6.0f;
        pr.damage = 10;
        pr.bounces = 0;
        gsh.projectiles[0] = pr;
        int hpShield = sm.health;
        int hpShooter = gsh.players[1].health;
        bool orange = false, flipped = false, bonus = false;
        for (int t = 0; t < TickRate * 2; ++t)
        {
            gsh.Tick(cmds);
            const Projectile& q = gsh.projectiles[0];
            if (q.active && q.deflected)
            {
                orange = true;
                flipped |= q.owner == 0;
                bonus |= q.bounces >= 1;
            }
        }
        check(orange && flipped && bonus,
              "deflection: orange shell, allegiance flip, +1 bounce");
        check(sm.health == hpShield,
              "nothing gets through to the shieldman");
        check(gsh.players[1].health < hpShooter,
              "the returned rocket hits its original shooter");

        // let the barrier lapse: the cooldown starts at THAT moment
        for (int t = 0; t < TickRate * 2; ++t) gsh.Tick(cmds);
        check(sm.shieldTimer <= 0.0f && sm.shieldWait > 11.0f,
              "the cooldown starts counting when the use finishes");
        cmds[0].buttons = BtnAbility1;
        gsh.Tick(cmds);
        check(sm.shieldTimer <= 0.0f && sm.shieldWait > 0.0f,
              "the ability refuses to fire mid-cooldown");
        cmds[0].buttons = 0;
        for (int t = 0; t < TickRate * 13; ++t) gsh.Tick(cmds);
        cmds[0].buttons = BtnAbility1;
        gsh.Tick(cmds);
        check(sm.shieldTimer > 3.5f, "cooled down: the barrier rises again");
        cmds[0].buttons = 0;

        // a FAST shell (crosses the whole face band in one tick) must be
        // caught by the segment test -- the old point check tunneled
        gsh.players[1].health = 100;
        Projectile fast{};
        fast.active = true;
        fast.owner = 1;
        fast.x = 0; fast.z = -18.0f; fast.y = 0.17f;
        fast.yaw = XM_PI;
        fast.speed = 45.0f;           // 0.7 units per tick
        fast.life = 6.0f;
        fast.damage = 10;
        gsh.projectiles[0] = fast;
        bool caught = false;
        for (int t = 0; t < TickRate && !caught; ++t)
        {
            gsh.Tick(cmds);
            caught = gsh.projectiles[0].active
                  && gsh.projectiles[0].deflected != 0;
        }
        check(caught, "segment test catches a one-tick face crossing");
    }

    // ---- match length: default 10:00, the host pick sets the horn ----
    {
        GameState g7{};
        g7.SpawnPlayer(0);
        g7.StartMatch();
        check(g7.matchEndTick - g7.tick == 10u * 60u * TickRate,
              "fresh matches default to 10:00");
        g7.matchMinutes = 20;
        g7.StartMatch();
        check(g7.matchEndTick - g7.tick == 20u * 60u * TickRate,
              "the host 20-minute pick sets the horn");
    }

    // ---- UNIQUE rule-benders ----
    {
        // TRIPLE DOCTRINE: cap 3 classes, normal upgrades leave the pool
        GameState gu{};
        gu.SpawnPlayer(0);
        PlayerState& p = gu.players[0];
        p.owned.push_back(uint8_t(UpgradeId::TripleDoctrine));
        gu.RecalcStats(0);
        check(MaxClassesFor(p) == 3, "TRIPLE DOCTRINE raises the cap to 3");
        p.money = 999;
        bool normalSeen = false;
        for (int i = 0; i < 300; ++i)
        {
            gu.GenerateOffer(0);
            const Offer& o = p.offers[0];
            if (!o.active) continue;
            const UpgradeType& u = kUpgradePool[o.type];
            if (u.rarity <= 4 && u.classReq == ClassNone
                && u.classGrant == ClassNone)
                normalSeen = true;
        }
        check(!normalSeen,
              "no plain normal upgrades offered under TRIPLE DOCTRINE");

        // PURE ARSENAL: class cards vanish, normal upgrades hit twice as hard
        GameState ga{};
        ga.SpawnPlayer(0);
        ga.SpawnPlayer(1);
        int normalIdx = -1;
        for (int i = 0; i < UpgradeCount; ++i)
        {
            const UpgradeType& u = kUpgradePool[i];
            if (u.rarity <= 4 && u.classReq == ClassNone
                && u.classGrant == ClassNone && u.modCount == 1
                && u.mods[0].factor > 1.01f)
            { normalIdx = i; break; }
        }
        check(normalIdx >= 0, "found a factor-based normal upgrade to probe");
        Stat probeStat = kUpgradePool[normalIdx].mods[0].stat;
        float f = kUpgradePool[normalIdx].mods[0].factor;
        ga.players[1].owned.push_back(uint8_t(normalIdx));
        ga.RecalcStats(1);
        ga.players[0].owned.push_back(uint8_t(UpgradeId::PureArsenal));
        ga.players[0].owned.push_back(uint8_t(normalIdx));
        ga.RecalcStats(0);
        float plain = ga.players[1].stats[int(probeStat)];
        float amped = ga.players[0].stats[int(probeStat)];
        float wantRatio = (1.0f + (f - 1.0f) * 2.0f) / f;
        check(fabsf(amped / plain - wantRatio) < 0.01f,
              "PURE ARSENAL doubles a normal factor's bonus");
        ga.players[0].money = 999;
        bool cardSeen = false;
        for (int i = 0; i < 300; ++i)
        {
            ga.GenerateOffer(0);
            const Offer& o = ga.players[0].offers[0];
            if (o.active && kUpgradePool[o.type].classGrant != ClassNone)
                cardSeen = true;
        }
        check(!cardSeen, "no class cards offered under PURE ARSENAL");

        // VAMPIRE: lifesteal on hits, sunburn in the open, safe in shade
        GameState gv{};
        gv.SpawnPlayer(0);
        gv.SpawnPlayer(1);
        gv.phase = PhasePlaying;
        gv.matchEndTick = gv.tick + 100000000u;
        PlayerState& vamp = gv.players[0];
        vamp.owned.push_back(uint8_t(UpgradeId::Vampire));
        gv.RecalcStats(0);
        vamp.health = 50;
        gv.ApplyDamage(0, 1, 40, 0);
        check(vamp.health == 54, "VAMPIRE drinks 10% of the wound");
        const Obstacle& ob = kObstacles[0];
        float sx = ob.cx - 0.489f / 0.636f * ob.height * 0.5f;
        float sz = ob.cz - 0.372f / 0.636f * ob.height * 0.5f;
        check(!InSunlight(sx, sz), "a box shades its shadow volume");
        check(InSunlight(0.0f, -20.0f), "mid-lane ground is sunlit");
        vamp.x = 0; vamp.z = -20; vamp.health = 100;
        InputCmd idle[MaxPlayers]{};
        for (int t = 0; t < TickRate * 2; ++t) gv.Tick(idle);
        check(vamp.health <= 82 && vamp.health >= 78,
              "sunlight burns ~10%% max HP per second");

        // TERRORIST: the death blast falls off with distance
        GameState gt{};
        gt.SpawnPlayer(0);
        gt.SpawnPlayer(1);
        gt.SpawnPlayer(2);
        gt.phase = PhasePlaying;
        gt.matchEndTick = gt.tick + 100000000u;
        gt.players[0].owned.push_back(uint8_t(UpgradeId::Terrorist));
        gt.RecalcStats(0);
        gt.SpawnPlayer(3);
        gt.players[0].x = 0; gt.players[0].z = -24;
        gt.players[1].x = 0; gt.players[1].z = -21.5f; // 2.5 u: plateau
        gt.players[2].x = 20; gt.players[2].z = 20;    // far out of reach
        gt.players[3].x = 0; gt.players[3].z = -13.0f; // 11 u: gentle tail
        gt.ApplyDamage(1, 0, 10000, 0);
        check(gt.players[1].health == 0,
              "inside one tank length: the blast takes 100%% max HP");
        check(gt.players[0].score == 1,
              "the terrorist scores the posthumous kill");
        check(gt.players[3].health == 84,
              "11 u out: the wide wave still lands 16 damage");
        check(gt.players[2].health == 100,
              "outside the radius: untouched");

        // DRUNKEN wanders 0.8x..1.3x; STEALTH is a fixed 0.65x + damage cut
        GameState gd{};
        gd.SpawnPlayer(0);
        gd.SpawnPlayer(1);
        gd.SpawnPlayer(2);
        gd.phase = PhasePlaying;
        gd.matchEndTick = gd.tick + 100000000u;
        gd.players[0].owned.push_back(uint8_t(UpgradeId::Drunken));
        gd.players[2].owned.push_back(uint8_t(UpgradeId::Stealth));
        gd.RecalcStats(0);
        gd.RecalcStats(2);
        float dmg0 = gd.players[1].stats[int(Stat::Damage)];
        check(fabsf(gd.players[2].stats[int(Stat::Damage)] - dmg0 * 0.85f)
                  < 0.01f,
              "STEALTH cuts rocket damage by 15%%");
        // one PROVEN lane (z = -28 ran clean for stealth), used in turns
        // so box layouts can never skew a runner
        auto run3s = [&](int who)
        {
            for (int i = 0; i < 3; ++i)
            {
                gd.players[i].x = (i == who) ? -20.0f : 20.0f;
                gd.players[i].z = (i == who) ? -28.0f : 20.0f + i * 4.0f;
            }
            InputCmd go[MaxPlayers]{};
            go[who].moveX = 1;
            for (int t = 0; t < TickRate * 3; ++t) gd.Tick(go);
            return gd.players[who].x + 20.0f;
        };
        float dSober = run3s(1);
        float dDrunk = run3s(0);
        float dSneak = run3s(2);
        check(dDrunk > dSober * 0.72f && dDrunk < dSober * 1.35f,
              "DRUNKEN speed stays inside the -20%%..+30%% band");
        check(fabsf(dSneak - dSober * 0.65f) < dSober * 0.03f,
              "STEALTH drives at exactly 65%%");
    }

    // ---- packed snapshot wire: round-trip fidelity ----
    {
        net::MsgSnapshot a{};
        a.tick = 123456u;
        a.phase = PhasePlaying;
        a.matchMinutes = 15;
        a.projectiles[0].active = 1;
        a.projectiles[0].x = 13.37f;
        a.projectiles[0].z = -21.5f;
        a.projectiles[0].y = 0.17f;
        a.projectiles[0].yaw = 2.5f;
        a.projectiles[0].radar16 = 56;
        a.projectiles[0].lock255 = 200;
        a.projectiles[0].deflected = 1;
        a.projectiles[MaxProjectiles - 1].active = 1;
        a.projectiles[MaxProjectiles - 1].x = -29.9f;
        a.grenades[3].active = 1;
        a.grenades[3].owner = 2;
        a.grenades[3].x = 5.5f;
        a.grenades[3].y = 2.25f;
        a.grenades[3].fuse255 = 120;
        a.soldiers[7].active = 1;
        a.soldiers[7].health = 30;
        a.soldiers[7].yaw = -1.2f;
        static uint8_t buf[sizeof(net::MsgSnapshot) + 64];
        int n = net::PackSnapshot(a, buf);
        check(n > 0 && n < int(sizeof(net::MsgSnapshot)),
              "packed snapshot is smaller than the raw struct");
        net::MsgSnapshot b{};
        b.projectiles[5].active = 1;   // must be CLEARED by unpack
        check(net::UnpackSnapshot(buf, n, b),
              "unpack accepts its own packer's output");
        check(b.tick == 123456u && b.matchMinutes == 15
                  && !b.projectiles[5].active
                  && b.projectiles[MaxProjectiles - 1].active
                  && b.grenades[3].active && b.grenades[3].fuse255 == 120
                  && b.soldiers[7].health == 30,
              "round-trip preserves live slots and clears dead ones");
        check(fabsf(b.projectiles[0].x - 13.37f) < 0.02f
                  && fabsf(b.projectiles[0].yaw - 2.5f) < 0.001f
                  && b.projectiles[0].deflected == 1
                  && fabsf(b.soldiers[7].yaw + 1.2f) < 0.001f
                  && fabsf(b.grenades[3].y - 2.25f) < 0.04f,
              "quantized fields survive within tolerance");
    }

    Log("classtest done: %d failure(s)", fails);
    return fails;
}

} // namespace tankaq
