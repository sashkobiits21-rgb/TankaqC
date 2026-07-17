// Tankaq — multiplayer tank game.
// D3D11/D3D12 rasterized rendering with GPU auto-detection, Steam networking.
#include <windows.h>
#include <windowsx.h>
#include <cstdio>
#include <string>
#include <vector>
#include <cmath>
#include <algorithm>
#include <DirectXMath.h>

#include "Log.h"
#include "AppState.h"
#include "Sound.h"
#include <deque>

using namespace DirectX;
using namespace tankaq;

// App state, Options, the Screen enum and the UiHot click registry live in
// AppState.h; the supply-line/owned-strip UI is UiShop.cpp, the in-game HUD
// is UiHud.cpp, headless tests are Tests.cpp. This file owns the window,
// frame loop, rendering glue, prediction and session/net wiring.
namespace tankaq
{

App g;

// True while a game session exists, even when the pause/settings overlay is
// up. The sim, camera, and VFX all key off this instead of the screen: pause
// only overlays the running game (it can't stop in multiplayer anyway).
bool InSession()
{
    return g.sessionActive
        && (g.screen == Screen::InGame || g.screen == Screen::Paused
            || g.screen == Screen::Settings);
}

// small random pitch wobble so repeated sounds don't machine-gun identically
float SndJitter(float spread)
{
    static uint32_t r = 0xBEEF1234u;
    r ^= r << 13; r ^= r >> 17; r ^= r << 5;
    return 1.0f + (float(r >> 8) * (1.0f / 16777216.0f) - 0.5f) * spread;
}

// distance attenuation for world-space events, measured from the camera focus
float SndDistVol(float x, float z, float base)
{
    float dx = x - g.camFocus.x, dz = z - g.camFocus.z;
    return base / (1.0f + sqrtf(dx * dx + dz * dz) * 0.045f);
}

bool CreateAssets();
void UpdateTitle();
IRenderer* CreateBackend(Backend b, std::string& err);
void SwitchRenderer(Backend want);
void ApplyDisplayMode();
void ApplyResolution();
void CopyTextToClipboard(const std::string& text);
void OpenShop();
void HandleShopClick(float mx, float my);
void ToggleReady();

// stb_easy_font is ASCII-only: scrub names to printable characters
void SetPlayerName(int id, const char* raw)
{
    PlayerState& p = g.game.players[id];
    int n = 0;
    for (const char* c = raw; *c && n < 15; ++c)
        if (*c >= 32 && *c < 127)
            p.name[n++] = *c;
    p.name[n] = 0;
    if (n == 0)
        sprintf_s(p.name, "PLAYER %d", id + 1);
}

bool WorldToScreen(const XMFLOAT3& wp, const XMFLOAT4X4& viewProj,
                   float& sx, float& sy)
{
    XMVECTOR clip = XMVector4Transform(XMVectorSet(wp.x, wp.y, wp.z, 1.0f),
                                       XMLoadFloat4x4(&viewProj));
    float w = XMVectorGetW(clip);
    if (w <= 0.01f)
        return false;
    sx = (XMVectorGetX(clip) / w * 0.5f + 0.5f) * float(g.width);
    sy = (0.5f - XMVectorGetY(clip) / w * 0.5f) * float(g.height);
    return true;
}

// per-player identity colors (tanks + score squares)
static const XMFLOAT4 kPlayerTint[MaxLobbyPlayers] = {
    { 0.85f, 1.05f, 0.85f, 0 },   // P1 green
    { 1.15f, 0.75f, 0.72f, 0 },   // P2 red
    { 0.72f, 0.88f, 1.20f, 0 },   // P3 blue
    { 1.15f, 1.05f, 0.60f, 0 },   // P4 yellow
};
const UiColor kPlayerUiCol[MaxLobbyPlayers] = {
    { 0.45f, 0.80f, 0.45f, 1 },
    { 0.85f, 0.40f, 0.38f, 1 },
    { 0.42f, 0.60f, 0.90f, 1 },
    { 0.88f, 0.78f, 0.32f, 1 },
};

// ------------------------------------------------------------------ helpers

std::string GetArg(const std::string& cmd, const std::string& key)
{
    size_t p = cmd.find(key);
    if (p == std::string::npos)
        return "";
    p += key.size();
    size_t e = cmd.find_first_of(" \t", p);
    return cmd.substr(p, e == std::string::npos ? std::string::npos : e - p);
}

Options ParseOptions(const std::string& cmd)
{
    Options o;
    o.renderer = GetArg(cmd, "--renderer=");
    o.host = cmd.find("--host") != std::string::npos;
    o.solo = cmd.find("--solo") != std::string::npos;
    o.join = GetArg(cmd, "--join=");
    std::string port = GetArg(cmd, "--port=");
    if (!port.empty()) o.port = uint16_t(atoi(port.c_str()));
    std::string frames = GetArg(cmd, "--frames=");
    if (!frames.empty()) o.screenshotAfterFrames = atoi(frames.c_str());
    o.screenshotPath = GetArg(cmd, "--screenshot=");
    o.autoDrive = cmd.find("--drive") != std::string::npos;
    o.vsync = cmd.find("--novsync") == std::string::npos;
    std::string pos = GetArg(cmd, "--winpos=");
    if (!pos.empty())
        sscanf_s(pos.c_str(), "%d,%d", &o.winX, &o.winY);
    std::string v;
    if (!(v = GetArg(cmd, "--gi=")).empty()) o.gi = atoi(v.c_str());
    if (!(v = GetArg(cmd, "--ao=")).empty()) o.ao = atoi(v.c_str());
    if (!(v = GetArg(cmd, "--girays=")).empty()) o.giRays = atoi(v.c_str());
    if (!(v = GetArg(cmd, "--temporal=")).empty()) o.temporal = atoi(v.c_str());
    if (!(v = GetArg(cmd, "--gires=")).empty()) o.giHalf = (v == "full") ? 0 : 1;
    if (!(v = GetArg(cmd, "--shadows=")).empty()) o.shadows = atoi(v.c_str());
    if (!(v = GetArg(cmd, "--shadowres=")).empty()) o.shadowRes = atoi(v.c_str());
    if (!(v = GetArg(cmd, "--shadowfilter=")).empty()) o.shadowFilter = atoi(v.c_str());
    if (!(v = GetArg(cmd, "--aa=")).empty()) o.aa = atoi(v.c_str());
    if (!(v = GetArg(cmd, "--lagcomp=")).empty()) o.lagComp = atoi(v.c_str());
    o.boom = cmd.find("--boom") != std::string::npos;
    o.fullscreen = cmd.find("--fullscreen") != std::string::npos;
    o.rich = cmd.find("--rich") != std::string::npos;
    o.shopTest = cmd.find("--shoptest") != std::string::npos;
    if (!(v = GetArg(cmd, "--pausetest=")).empty()) o.pauseTest = atoi(v.c_str());
    o.quickMatch = cmd.find("--quickmatch") != std::string::npos;
    if (!(v = GetArg(cmd, "--quickmatch=")).empty())
        o.quickMatchNeed = atoi(v.c_str());
    if (!(v = GetArg(cmd, "--readytest=")).empty()) o.readyTest = atoi(v.c_str());
    if (!(v = GetArg(cmd, "--bounces=")).empty()) gDebugBounces = atoi(v.c_str());
    o.rigTest = cmd.find("--rigtest") != std::string::npos;
    o.classTest = cmd.find("--classtest") != std::string::npos;
    o.soldierTest = cmd.find("--soldiertest") != std::string::npos;
    o.demoClass = GetArg(cmd, "--demo=");
    if (!(v = GetArg(cmd, "--winsize=")).empty())
        sscanf_s(v.c_str(), "%dx%d", &o.winW, &o.winH);
    return o;
}

void ApplySnapshot(const net::MsgSnapshot& s)
{
    g.dbgSnapThisFrame = true;
    // keep the last two snapshots for remote-player render interpolation
    g.snapPrev = g.haveSnap ? g.snapCurr : s;
    g.snapCurr = s;
    g.snapCurrTime = g.time;
    g.haveTwoSnaps = g.haveSnap;
    g.haveSnap = true;

    g.game.tick = s.tick;
    g.game.phase = s.phase;
    g.game.winner = s.winner;
    g.game.targetPlayers = s.targetPlayers;
    g.game.matchMinutes = s.matchMinutes;
    g.game.matchEndTick = s.matchEndTick;
    for (int i = 0; i < MaxPlayers; ++i)
    {
        const net::PlayerNet& in = s.players[i];
        PlayerState& p = g.game.players[i];
        bool wasActive = p.active;
        p.active = in.active != 0;
        if (!p.active)
            continue;
        if (!wasActive)
        {
            // preserve the owned list: an OwnedSync (reliable) may have
            // arrived before this first snapshot marked the player active
            std::vector<uint8_t> keepOwned = std::move(p.owned);
            p = PlayerState{};
            p.active = true;
            p.owned = std::move(keepOwned);
            g.game.RecalcStats(i);   // derive stats locally (add, then mul)
        }
        memcpy(p.name, in.name, sizeof(p.name));
        p.name[15] = 0;
        p.ready = in.ready;
        p.health = in.health;
        p.score = in.score;
        p.money = in.money;
        for (int s = 0; s < NumOfferSlots; ++s)
        {
            p.offers[s].active = in.offers[s].active;
            p.offers[s].id = in.offers[s].id;
            p.offers[s].type = in.offers[s].type;
            p.offers[s].cost = in.offers[s].cost;
        }
        p.hitFlash = (in.flags & 1) ? 0.25f : std::max(0.0f, p.hitFlash - TickDt);
        p.muzzleFlash = (in.flags & 2) ? 0.10f : std::max(0.0f, p.muzzleFlash - TickDt);
        if (i != g.myId)
        {
            p.x = in.x; p.z = in.z;
            p.hullYaw = in.hullYaw;
            p.turretYaw = in.turretYaw;
            p.possessTimer = in.possess32 / 32.0f;   // ghost-tint on remotes
            p.shieldTimer = in.shield16 / 16.0f;     // barrier draw on remotes
            p.shieldWait = in.shieldCd4 / 4.0f;
        }
    }

    // necromancer entities + radar fields: authoritative adoption, visuals
    // interpolate from the snapshot pair
    for (int i = 0; i < MaxSkulls; ++i)
    {
        const net::SkullNet& in = s.skulls[i];
        SkullState& sk = g.game.skulls[i];
        sk.active = in.active != 0;
        if (sk.active) { sk.owner = in.owner; sk.x = in.x; sk.z = in.z; sk.yaw = in.yaw; }
    }
    for (int i = 0; i < MaxPuddles; ++i)
    {
        const net::PuddleNet& in = s.puddles[i];
        PuddleState& pu = g.game.puddles[i];
        pu.active = in.active != 0;
        if (pu.active) { pu.owner = in.owner; pu.x = in.x; pu.z = in.z;
                         pu.life = in.life16 / 16.0f; }
    }
    for (int i = 0; i < MaxGhosts; ++i)
    {
        const net::GhostNet& in = s.ghosts[i];
        GhostState& gh = g.game.ghosts[i];
        gh.active = in.active != 0;
        if (gh.active) { gh.owner = in.owner; gh.x = in.x; gh.z = in.z; }
    }
    for (int i = 0; i < MaxGrenades; ++i)
    {
        const net::GrenadeNet& in = s.grenades[i];
        GrenadeState& gr = g.game.grenades[i];
        gr.active = in.active != 0;
        if (gr.active)
        {
            gr.owner = in.owner;
            gr.x = in.x; gr.y = in.y; gr.z = in.z;
            gr.fuse = in.fuse255 == 255 ? -1.0f : in.fuse255 / 100.0f;
        }
    }
    for (int i = 0; i < MaxProjectiles; ++i)
    {
        g.game.projectiles[i].radarRange = s.projectiles[i].radar16 / 16.0f;
        g.game.projectiles[i].deflected = s.projectiles[i].deflected;
        g.game.projectiles[i].radarRings = s.projectiles[i].radarRings;
        g.game.projectiles[i].radarLockFrac = s.projectiles[i].lock255 / 255.0f;
    }

    // soldiers: adopt the authoritative state wholesale; rendering
    // interpolates positions from the snapPrev/snapCurr pair and drives
    // animation locally from `state` (fire-and-forget -- no corrections)
    for (int i = 0; i < MaxSoldiers; ++i)
    {
        const net::SoldierNet& in = s.soldiers[i];
        SoldierState& sl = g.game.soldiers[i];
        sl.active = in.active != 0;
        if (!sl.active)
            continue;
        sl.owner = in.owner;
        sl.state = in.state;
        sl.targetId = in.targetId;
        sl.health = float(in.health);
        sl.x = in.x; sl.z = in.z; sl.yaw = in.yaw;
        sl.muzzleFlash = (in.flags & 1) ? 0.10f
                                        : std::max(0.0f, sl.muzzleFlash - TickDt);
        sl.hitFlash = (in.flags & 2) ? 0.25f
                                     : std::max(0.0f, sl.hitFlash - TickDt);
    }

    // Reconciliation: adopt the authoritative state for our own tank, then
    // replay every input the host hasn't simulated yet. When prediction was
    // right (normal case) the result matches what we already show.
    const net::PlayerNet& own = s.players[g.myId];
    if (own.active)
    {
        PlayerState& me = g.game.players[g.myId];
        bool hadPred = g.predCurr.active;
        float beforeX = g.predCurr.x, beforeZ = g.predCurr.z;
        float beforeYaw = g.predCurr.hullYaw;

        me.x = own.x; me.z = own.z;
        me.hullYaw = own.hullYaw;
        me.turretYaw = own.turretYaw;
        // rebase the boost-fuel state too: the replay below re-integrates
        // drain/regen for the unacked ticks exactly like the host will
        me.boostFuel = (own.fuel255 / 255.0f)
                     * me.stats[int(Stat::BoostFuel)];
        me.boostRegenWait = own.regenWait32 / 32.0f;
        // ...and possession: the replay drives the same deterministic chaos
        me.possessTimer = own.possess32 / 32.0f;
        // ...and the shield: replayed activation/slow must match the host
        me.shieldTimer = own.shield16 / 16.0f;
        me.shieldWait = own.shieldCd4 / 4.0f;
        while (!g.pendingInputs.empty() && g.pendingInputs.front().seq <= own.ackSeq)
            g.pendingInputs.erase(g.pendingInputs.begin());
        for (const auto& pi : g.pendingInputs)
            g.game.AdvanceMovement(g.myId, pi.cmd);

        // Never snap the rendered tank: any difference between what we were
        // predicting and the reconciled result becomes a render-space error
        // offset that decays over ~80 ms (GetRenderPlayer adds it). Large
        // differences are real teleports (respawn) and do snap.
        float dx = beforeX - me.x, dz = beforeZ - me.z;
        bool playing = s.phase == PhasePlaying || s.phase == PhaseOvertime;
        if (hadPred && playing && dx * dx + dz * dz < 9.0f)
        {
            g.predErrX = std::clamp(g.predErrX + dx, -2.0f, 2.0f);
            g.predErrZ = std::clamp(g.predErrZ + dz, -2.0f, 2.0f);
            g.predErrYaw = std::clamp(
                g.predErrYaw + WrapAngle(beforeYaw - me.hullYaw), -1.2f, 1.2f);
        }
        else
        {
            g.predErrX = g.predErrZ = g.predErrYaw = 0.0f;
        }
        // CRITICAL: do NOT touch predPrev/predCurr here. Collapsing the pair
        // froze the interpolated render for the rest of the tick interval and
        // jumped it by the un-rendered remainder -- a hitch on EVERY snapshot
        // (20/s), even with a perfect prediction match. The pair evolves only
        // in the prediction tick; with the error offset above, the handoff at
        // the next tick boundary is seamless: me_new + offset == old target.
    }

    for (int i = 0; i < MaxProjectiles; ++i)
    {
        const net::ProjectileNet& in = s.projectiles[i];
        Projectile& pr = g.game.projectiles[i];
        pr.active = in.active != 0;
        pr.x = in.x; pr.y = in.y; pr.z = in.z; pr.yaw = in.yaw;
    }

    // Predicted-fire confirmation: a server rocket appearing for the first
    // time near a provisional IS that provisional. The slot gets VEILED --
    // the provisional keeps rendering with pure local physics for its whole
    // flight (zero corrections); the veiled slot only supplies authority.
    for (int i = 0; i < MaxProjectiles; ++i)
    {
        bool isNew = s.projectiles[i].active
                  && !g.snapPrev.projectiles[i].active;
        if (!isNew)
            continue;
        for (int pv = 0; pv < 8; ++pv)
        {
            App::ProvRocket& p = g.provRockets[pv];
            if (!p.sim.active || p.matchedSlot >= 0)
                continue;
            float dx = p.sim.x - s.projectiles[i].x;
            float dz = p.sim.z - s.projectiles[i].z;
            if (dx * dx + dz * dz > 9.0f
                || fabsf(WrapAngle(p.sim.yaw - s.projectiles[i].yaw)) > 0.5f)
                continue;
            p.matchedSlot = i;
            g.projVeiledBy[i] = pv;
            g.projSpawnPos[i] = p.spawn;   // in case the slot ever unveils
            g.projSpawnTime[i] = p.born;
            g.projMatchedProv[i] = true;   // skip the rising-edge sound/record
            break;
        }
    }
}

net::MsgSnapshot BuildSnapshot()
{
    net::MsgSnapshot s;
    s.tick = g.game.tick;
    s.phase = g.game.phase;
    s.winner = g.game.winner;
    s.targetPlayers = g.game.targetPlayers;
    s.matchMinutes = g.game.matchMinutes;
    s.matchEndTick = g.game.matchEndTick;
    for (int i = 0; i < MaxPlayers; ++i)
    {
        const PlayerState& p = g.game.players[i];
        net::PlayerNet& out = s.players[i];
        out.active = p.active ? 1 : 0;
        out.ready = p.ready;
        memcpy(out.name, p.name, sizeof(out.name));
        out.health = uint16_t(std::clamp(p.health, 0, 65535));
        out.score = p.score;
        out.money = p.money;
        float cap = std::max(p.stats[int(Stat::BoostFuel)], 0.01f);
        out.fuel255 = uint8_t(std::clamp(p.boostFuel / cap, 0.0f, 1.0f) * 255.0f);
        out.regenWait32 = uint8_t(std::min(p.boostRegenWait * 32.0f, 255.0f));
        out.possess32 = uint8_t(std::min(p.possessTimer * 32.0f, 255.0f));
        out.shield16 = uint8_t(std::min(p.shieldTimer * 16.0f, 255.0f));
        out.shieldCd4 = uint8_t(std::min(p.shieldWait * 4.0f, 255.0f));
        for (int s = 0; s < NumOfferSlots; ++s)
        {
            out.offers[s].active = p.offers[s].active;
            out.offers[s].id = p.offers[s].id;
            out.offers[s].type = p.offers[s].type;
            out.offers[s].cost = p.offers[s].cost;
        }
        out.ackSeq = g.inputSeqs[i];
        out.x = p.x; out.z = p.z;
        out.hullYaw = p.hullYaw;
        out.turretYaw = p.turretYaw;
        out.flags = uint8_t((p.hitFlash > 0 ? 1 : 0) | (p.muzzleFlash > 0 ? 2 : 0));
    }
    for (int i = 0; i < MaxProjectiles; ++i)
    {
        const Projectile& pr = g.game.projectiles[i];
        net::ProjectileNet& out = s.projectiles[i];
        out.active = pr.active ? 1 : 0;
        out.radar16 = uint8_t(std::min(pr.radarRange * 16.0f, 255.0f));
        out.radarRings = uint8_t(pr.radarRings);
        out.lock255 = uint8_t(std::clamp(pr.radarLockFrac, 0.0f, 1.0f) * 255.0f);
        out.deflected = pr.deflected;
        out.x = pr.x; out.y = pr.y; out.z = pr.z; out.yaw = pr.yaw;
    }
    for (int i = 0; i < MaxSkulls; ++i)
    {
        const SkullState& sk = g.game.skulls[i];
        net::SkullNet& out = s.skulls[i];
        out.active = sk.active ? 1 : 0;
        out.owner = sk.owner;
        out.x = sk.x; out.z = sk.z; out.yaw = sk.yaw;
    }
    for (int i = 0; i < MaxPuddles; ++i)
    {
        const PuddleState& pu = g.game.puddles[i];
        net::PuddleNet& out = s.puddles[i];
        out.active = pu.active ? 1 : 0;
        out.owner = pu.owner;
        out.life16 = uint8_t(std::min(pu.life * 16.0f, 255.0f));
        out.x = pu.x; out.z = pu.z;
    }
    for (int i = 0; i < MaxGhosts; ++i)
    {
        const GhostState& gh = g.game.ghosts[i];
        net::GhostNet& out = s.ghosts[i];
        out.active = gh.active ? 1 : 0;
        out.owner = gh.owner;
        out.x = gh.x; out.z = gh.z;
    }
    for (int i = 0; i < MaxGrenades; ++i)
    {
        const GrenadeState& gr = g.game.grenades[i];
        net::GrenadeNet& out = s.grenades[i];
        out.active = gr.active ? 1 : 0;
        out.owner = gr.owner;
        out.x = gr.x; out.y = gr.y; out.z = gr.z;
        out.fuse255 = gr.fuse < 0.0f
            ? 255
            : uint8_t(std::clamp(int(gr.fuse * 100.0f), 0, 254));
    }
    for (int i = 0; i < MaxSoldiers; ++i)
    {
        const SoldierState& sl = g.game.soldiers[i];
        net::SoldierNet& out = s.soldiers[i];
        out.active = sl.active ? 1 : 0;
        out.owner = sl.owner;
        out.state = sl.state;
        out.targetId = sl.targetId;
        out.health = uint8_t(std::clamp(sl.health, 0.0f, 255.0f));
        out.flags = uint8_t((sl.muzzleFlash > 0 ? 1 : 0)
                          | (sl.hitFlash > 0 ? 2 : 0));
        out.x = sl.x; out.z = sl.z; out.yaw = sl.yaw;
    }
    return s;
}

float LerpAngle(float a, float b, float t)
{
    return WrapAngle(a + WrapAngle(b - a) * t);
}

// Render-time state for a player: interpolated between fixed ticks (host/solo,
// and the predicted local tank on clients) or between the last two snapshots
// (remote tanks on clients).
void GetRenderPlayer(int id, float& x, float& z, float& hullYaw, float& turretYaw)
{
    const PlayerState& cur = g.game.players[id];
    x = cur.x; z = cur.z; hullYaw = cur.hullYaw; turretYaw = cur.turretYaw;

    if (!g.online || g.isHost)
    {
        const PlayerState& prev = g.prevTick.players[id];
        if (prev.active && cur.active)
        {
            float t = g.tickAlpha;
            x = prev.x + (cur.x - prev.x) * t;
            z = prev.z + (cur.z - prev.z) * t;
            hullYaw = LerpAngle(prev.hullYaw, cur.hullYaw, t);
            turretYaw = LerpAngle(prev.turretYaw, cur.turretYaw, t);
        }
        // zero-latency turret for the local tank
        if (id == g.myId
            && (g.game.phase == PhasePlaying || g.game.phase == PhaseOvertime)
            && cur.health > 0)
            turretYaw = g.aimYaw;
    }
    else if (id == g.myId)
    {
        float t = g.tickAlpha;
        // predicted pair + the decaying reconciliation-error offset: server
        // corrections glide out instead of freezing/teleporting the tank
        x = g.predPrev.x + (g.predCurr.x - g.predPrev.x) * t + g.predErrX;
        z = g.predPrev.z + (g.predCurr.z - g.predPrev.z) * t + g.predErrZ;
        hullYaw = WrapAngle(LerpAngle(g.predPrev.hullYaw, g.predCurr.hullYaw, t)
                            + g.predErrYaw);
        turretYaw = LerpAngle(g.predPrev.turretYaw, g.predCurr.turretYaw, t);
        // zero-latency turret: track the live aim directly while playing
        if ((g.game.phase == PhasePlaying || g.game.phase == PhaseOvertime)
            && cur.health > 0)
            turretYaw = g.aimYaw;
    }
    else if (g.haveTwoSnaps)
    {
        const net::PlayerNet& a = g.snapPrev.players[id];
        const net::PlayerNet& b = g.snapCurr.players[id];
        if (a.active && b.active)
        {
            float snapDt = SnapshotEveryTicks * TickDt;
            float t = std::clamp(float((g.time - g.snapCurrTime) / snapDt),
                                 0.0f, 1.0f);
            x = a.x + (b.x - a.x) * t;
            z = a.z + (b.z - a.z) * t;
            hullYaw = LerpAngle(a.hullYaw, b.hullYaw, t);
            turretYaw = LerpAngle(a.turretYaw, b.turretYaw, t);

            // Client-side lag compensation: remote tanks are one-way-latency
            // old, so project them forward along their snapshot velocity by
            // min(latency, 80 ms). Corrections blend in over ~10 ms so new
            // snapshots never teleport the tank.
            App::RemoteDisplay& rd = g.remoteDisplay[id];
            if (g.lagComp)
            {
                float oneWay = g.net.hostAvgOneWayMs();
                float extrap = std::clamp(oneWay, 0.0f, 80.0f) * 0.001f;
                float vx = (b.x - a.x) / snapDt;
                float vz = (b.z - a.z) / snapDt;
                float tx = x + vx * extrap;
                float tz = z + vz * extrap;
                if (!rd.valid)
                    rd = { tx, tz, hullYaw, turretYaw, true };
                float k = 1.0f - expf(-g.frameDt / 0.010f);
                rd.x += (tx - rd.x) * k;
                rd.z += (tz - rd.z) * k;
                rd.hullYaw = LerpAngle(rd.hullYaw, hullYaw, k);
                rd.turretYaw = LerpAngle(rd.turretYaw, turretYaw, k);
                x = rd.x;
                z = rd.z;
                hullYaw = rd.hullYaw;
                turretYaw = rd.turretYaw;
            }
            else
            {
                rd.valid = false;
            }
        }
        else
        {
            g.remoteDisplay[id].valid = false;
        }
    }
}

// Mouse aim: unproject the cursor onto the horizontal plane at barrel height.
void UpdateAim(const XMMATRIX& view, const XMMATRIX& proj)
{
    const PlayerState& me = g.game.players[g.myId];
    XMVECTOR nearPt = XMVector3Unproject(
        XMVectorSet(float(g.mouseX), float(g.mouseY), 0.0f, 1.0f),
        0, 0, float(g.width), float(g.height), 0, 1, proj, view, XMMatrixIdentity());
    XMVECTOR farPt = XMVector3Unproject(
        XMVectorSet(float(g.mouseX), float(g.mouseY), 1.0f, 1.0f),
        0, 0, float(g.width), float(g.height), 0, 1, proj, view, XMMatrixIdentity());
    XMVECTOR dir = XMVector3Normalize(XMVectorSubtract(farPt, nearPt));
    float planeY = g.game.turretPivot.y + g.game.muzzleOffset.y;
    float oy = XMVectorGetY(nearPt), dy = XMVectorGetY(dir);
    if (fabsf(dy) < 1e-4f)
        return;
    float t = (planeY - oy) / dy;
    if (t <= 0)
        return;
    XMVECTOR hit = XMVectorAdd(nearPt, XMVectorScale(dir, t));
    float hx = XMVectorGetX(hit), hz = XMVectorGetZ(hit);
    g.aimYaw = atan2f(hx - me.x, hz - me.z);
}

InputCmd BuildLocalInput()
{
    InputCmd in;
    if (g.opt.autoDrive)
    {
        float ang = float(g.time) * 0.45f;
        in.moveX = sinf(ang);
        in.moveZ = cosf(ang);
        uint32_t t = g.game.tick;
        if (t % 75 < 2)
            in.buttons |= BtnFire;
        if (t % 220 < 50)              // periodic boost bursts (tests fuel)
            in.buttons |= BtnBoost;
        in.turretYaw = g.game.players[g.myId].hullYaw + 0.6f * sinf(float(g.time) * 0.7f);
        in.aimYawFresh = in.turretYaw;
        return in;
    }
    // Screen-relative WASD resolved into a world-space vector (fixed camera
    // orientation: screen up = +Z, screen right = -X).
    float inX = 0, inZ = 0;
    if (g.keys['W'] || g.keys[VK_UP]) inZ += 1.0f;
    if (g.keys['S'] || g.keys[VK_DOWN]) inZ -= 1.0f;
    if (g.keys['A'] || g.keys[VK_LEFT]) inX -= 1.0f;
    if (g.keys['D'] || g.keys[VK_RIGHT]) inX += 1.0f;
    in.moveX = -inX;
    in.moveZ = inZ;
    float len = sqrtf(in.moveX * in.moveX + in.moveZ * in.moveZ);
    if (len > 1.0f) { in.moveX /= len; in.moveZ /= len; }

    bool fire = g.mouseDown || g.keys[VK_SPACE];
    // never fire through UI: the cursor over ANY registered clickable (shop
    // panel, owned-strip arrow, code banner, ...) suppresses the trigger --
    // one rule instead of a per-widget suppression list
    if (UiHotContains(float(g.mouseX), float(g.mouseY)))
        fire = false;
    if (fire) in.buttons |= BtnFire;
    if (g.keys[VK_SHIFT]) in.buttons |= BtnBoost;   // boost: 2x speed on fuel
    if (g.keys['1']) in.buttons |= BtnAbility1;     // ability slot 1: SHIELD
    in.turretYaw = g.aimYaw;
    in.aimYawFresh = g.aimYaw;
    return in;
}

void SpawnExplosion(const XMFLOAT3& raw)
{
    // Impacts can land slightly inside walls/obstacles (projectiles despawn on
    // overlap). Push the VFX origin back into open air so smoke doesn't start
    // embedded in geometry.
    XMFLOAT3 p = raw;
    p.x = std::clamp(p.x, -(ArenaHalf - 1.2f), ArenaHalf - 1.2f);
    p.z = std::clamp(p.z, -(ArenaHalf - 1.2f), ArenaHalf - 1.2f);
    p.y = std::max(p.y, 0.55f);
    for (const Obstacle& o : kObstacles)
    {
        float ex = o.hx + 0.5f, ez = o.hz + 0.5f;
        float dx = p.x - o.cx, dz = p.z - o.cz;
        if (fabsf(dx) >= ex || fabsf(dz) >= ez || p.y > o.height + 0.3f)
            continue;
        float pushX = (dx > 0 ? ex - dx : -ex - dx);
        float pushZ = (dz > 0 ? ez - dz : -ez - dz);
        if (fabsf(pushX) < fabsf(pushZ)) p.x += pushX;
        else p.z += pushZ;
    }

    if (g.bursts.size() >= 16)
        g.bursts.erase(g.bursts.begin());
    g.bursts.push_back({ p, g.time });
    if (g.scorches.size() >= 16)
        g.scorches.erase(g.scorches.begin());
    g.scorches.push_back({ raw, g.time });   // the burn mark stays at the impact

    // narrow jitter: pitching bass up too far would thin it out
    snd::Play(snd::Sfx::Explosion, SndDistVol(p.x, p.z, 0.9f), SndJitter(0.08f));
}

// Spawn VFX whenever a projectile slot flips active -> inactive. This works
// identically for solo, host (sim) and clients (snapshot inference).
void UpdateVfxFromSim()
{
    for (int i = 0; i < MaxProjectiles; ++i)
    {
        const Projectile& pr = g.game.projectiles[i];
        if (g.prevProjActive[i] && !pr.active && InSession())
        {
            SpawnExplosion(g.prevProjPos[i]);
            // RADAR detonation: one REAL explosion per circle in the tree,
            // each at its packed position (the root reuses the base boom)
            if (g.prevProjRadar[i] > 0.0f)
            {
                float ox[MaxRadarNodes], oz[MaxRadarNodes], rad[MaxRadarNodes];
                int dep[MaxRadarNodes];
                int nn = RadarTreeLayout(g.prevProjRadar[i],
                                         g.prevProjRings[i],
                                         g.prevProjYaw[i],
                                         ox, oz, rad, dep, MaxRadarNodes);
                for (int k = 1; k < nn; ++k)
                    SpawnExplosion({ g.prevProjPos[i].x + ox[k], 0.8f,
                                     g.prevProjPos[i].z + oz[k] });
            }
            // authority: the veiled server twin died, so our locally rendered
            // provisional dies with it (the explosion above is the boom)
            if (g.projVeiledBy[i] >= 0)
            {
                g.provRockets[g.projVeiledBy[i]].sim.active = false;
                g.projVeiledBy[i] = -1;
            }
        }
        if (pr.active)
        {
            g.prevProjRadar[i] = pr.radarRange;
            g.prevProjYaw[i] = pr.yaw;
            g.prevProjRings[i] = uint8_t(pr.radarRings);
        }
        else
            g.prevProjRadar[i] = 0.0f;
        if (!g.prevProjActive[i] && pr.active && InSession())
        {
            if (g.projMatchedProv[i])
            {
                // confirmed provisional: sound + spawn record already done
                g.projMatchedProv[i] = false;
            }
            else
            {
                snd::Play(snd::Sfx::Shoot, SndDistVol(pr.x, pr.z, 0.6f),
                          SndJitter(0.07f));
                // birth record for the squish/spring shader (host: exact
                // muzzle; client: first snapshot position)
                g.projSpawnPos[i] = XMFLOAT3(pr.x, pr.y, pr.z);
                g.projSpawnTime[i] = g.time;
            }
        }
        g.prevProjActive[i] = pr.active;
        if (pr.active)
            g.prevProjPos[i] = XMFLOAT3(pr.x, pr.y, pr.z);
    }
    // skulls burst into acid: one small puff where each one died
    for (int i = 0; i < MaxSkulls; ++i)
    {
        const SkullState& sk = g.game.skulls[i];
        if (g.prevSkullActive[i] && !sk.active && InSession())
        {
            if (g.bursts.size() >= 16)
                g.bursts.erase(g.bursts.begin());
            g.bursts.push_back({ g.prevSkullPos[i], g.time });
            snd::Play(snd::Sfx::Glass,
                      SndDistVol(g.prevSkullPos[i].x, g.prevSkullPos[i].z, 0.4f),
                      0.8f * SndJitter(0.1f));
        }
        g.prevSkullActive[i] = sk.active;
        if (sk.active)
            g.prevSkullPos[i] = XMFLOAT3(sk.x, SkullY, sk.z);
    }
    // grenades pop with the full explosion treatment where they fused out
    for (int i = 0; i < MaxGrenades; ++i)
    {
        const GrenadeState& gr = g.game.grenades[i];
        if (g.prevGrenadeActive[i] && !gr.active && InSession())
        {
            if (g.bursts.size() >= 16)
                g.bursts.erase(g.bursts.begin());
            g.bursts.push_back({ g.prevGrenadePos[i], g.time });
            snd::Play(snd::Sfx::Explosion,
                      SndDistVol(g.prevGrenadePos[i].x,
                                 g.prevGrenadePos[i].z, 0.9f),
                      0.85f * SndJitter(0.08f));
        }
        g.prevGrenadeActive[i] = gr.active;
        if (gr.active)
            g.prevGrenadePos[i] = XMFLOAT3(gr.x, gr.y, gr.z);
    }

    // prune dead effects
    while (!g.bursts.empty() && g.time - g.bursts.front().t0 > 3.0)
        g.bursts.erase(g.bursts.begin());
    while (!g.scorches.empty() && g.time - g.scorches.front().t0 > 30.0)
        g.scorches.erase(g.scorches.begin());

    if (g.opt.boom && InSession() && g.time - g.lastBoom > 2.2)
    {
        g.lastBoom = g.time;
        // at the local tank: exercises scorch-skips-dynamic-objects and
        // smoke colliding around the hull
        const PlayerState& me = g.game.players[g.myId];
        SpawnExplosion(me.active ? XMFLOAT3(me.x + 0.8f, 0.8f, me.z - 0.8f)
                                 : XMFLOAT3(6.0f, 0.8f, 18.4f));
    }
}

void ResetNetSimState()
{
    g.game = GameState{};
    g.game.turretPivot = g.tank.turretPivot;
    g.game.muzzleOffset = g.tank.muzzle;
    g.game.rngState = uint32_t(GetTickCount64() | 1);
    g.prevTick = GameState{};
    g.haveSnap = g.haveTwoSnaps = false;
    g.pendingInputs.clear();
    g.inputSeq = 0;
    for (uint32_t& s : g.inputSeqs) s = 0;
    for (InputCmd& c : g.inputs) c = InputCmd{};
    for (auto& q : g.inputQueue) q.clear();
    g.predErrX = g.predErrZ = g.predErrYaw = 0.0f;
    g.predPrev = g.predCurr = PlayerState{};
    g.predCooldown = 0;
    for (auto& pv : g.provRockets) pv.sim.active = false;
    for (int i = 0; i < MaxProjectiles; ++i)
    {
        g.projVeiledBy[i] = -1;
        g.projMatchedProv[i] = false;
    }
    for (auto& a : g.cardAnims) a.active = false;
    g.shopBurnFx.clear();
    g.ejectFx.clear();
    g.debrisFx.clear();
    g.slatsBroken = false;
    g.lastClickedOfferId = 0;
    g.shopTestBuyAt = g.shopTestNextOffer = 0;
    g.shopTestOffersForced = 0;
}

void LeaveToMenu(const std::string& why)
{
    g.net.Disconnect();
    g.screen = Screen::MainMenu;
    g.settingsReturn = Screen::MainMenu;
    g.sessionActive = false;
    g.online = false;
    g.isHost = false;
    g.searching = false;
    g.lastAdvertPlayers = g.lastAdvertPhase = -1;
    ResetNetSimState();
    g.statusLine = why;
}

void ToggleReady()
{
    if (g.screen != Screen::InGame || g.game.phase != PhaseLobby)
        return;
    snd::Play(snd::Sfx::Click, 0.5f, SndJitter(0.06f));
    PlayerState& me = g.game.players[g.myId];
    bool want = !me.ready;
    if (!g.online || g.isHost)
        me.ready = want ? 1 : 0;
    else
    {
        me.ready = want ? 1 : 0;   // optimistic; snapshot confirms
        g.net.SendReadyToHost(want);
    }
}

void StartSolo()
{
    g.myId = 0;
    g.isHost = true;
    g.online = false;
    ResetNetSimState();
    g.game.SpawnPlayer(0);
    SetPlayerName(0, "YOU");
    if (g.opt.rich || g.opt.shopTest)
        g.game.players[0].money = 500;
    if (g.opt.soldierTest || !g.opt.demoClass.empty())
    {
        // a parked target so the summons/rockets have someone to fight
        g.game.SpawnPlayer(1);
        SetPlayerName(1, "DUMMY");
        g.game.players[1].ready = 1;
    }
    g.screen = Screen::InGame;
    g.sessionActive = true;
    g.statusLine.clear();
}

void StartHost()
{
    std::string err;
    if (!g.net.StartHost(g.opt.port, err))
    {
        g.statusLine = "HOST FAILED: " + err;
        return;
    }
    g.myId = 0;
    g.isHost = true;
    g.online = true;
    ResetNetSimState();
    g.game.SpawnPlayer(0);
    SetPlayerName(0, g.net.myName().c_str());
    if (g.opt.rich || g.opt.shopTest)
        g.game.players[0].money = 500;
    g.screen = Screen::InGame;
    g.sessionActive = true;
    g.statusLine.clear();
}

// Quick match: search the free Steam lobby directory for an open public game
// of the chosen size; onMatchFound / onNoMatch (set in wWinMain) continue.
void StartFindMatch(int need)
{
    if (!g.net.steamAvailable())
    {
        g.statusLine = "steam is not available - cannot search for matches";
        g.screen = Screen::MainMenu;
        return;
    }
    g.searchNeed = std::clamp(need, 2, int(MaxLobbyPlayers));
    g.searching = true;
    g.screen = Screen::Connecting;
    g.connectStart = g.time;
    g.statusLine.clear();
    g.net.QuickMatch(g.searchNeed);
}

void StartJoin(const std::string& target)
{
    std::string err;
    bool ok;
    if (target.find_first_of(".:") != std::string::npos)
    {
        ok = g.net.ConnectToIP(target, err);
    }
    else
    {
        // game code path: validate digits-only SteamID64
        if (target.size() < 15 || target.size() > 20
            || target.find_first_not_of("0123456789") != std::string::npos)
        {
            g.statusLine = "INVALID CODE - paste the 17-digit game code or ip:port";
            return;
        }
        uint64_t code = strtoull(target.c_str(), nullptr, 10);
        if (code == g.net.mySteamId())
        {
            // Same Steam account (testing on one PC): Steam can't relay P2P to
            // itself, so fall back to the host's local UDP socket.
            Log("Join: code is our own SteamID, using 127.0.0.1 loopback instead");
            ok = g.net.ConnectToIP("127.0.0.1:" + std::to_string(g.opt.port), err);
        }
        else
        {
            ok = g.net.ConnectToCode(code, err);
        }
    }
    if (!ok)
    {
        g.statusLine = "JOIN FAILED: " + err;
        return;
    }
    g.isHost = false;
    g.online = true;
    ResetNetSimState();
    g.screen = Screen::Connecting;
    g.connectStart = g.time;
    g.statusLine = "CONNECTING...";
}

// ------------------------------------------------------------------ rendering

XMFLOAT4X4 Store(const XMMATRIX& m)
{
    XMFLOAT4X4 out;
    XMStoreFloat4x4(&out, m);
    return out;
}

void BuildScene(FrameData& frame, const XMMATRIX& view, const XMMATRIX& proj)
{
    XMMATRIX viewProj = XMMatrixMultiply(view, proj);
    frame.viewProj = Store(viewProj);
    frame.invViewProj = Store(XMMatrixInverse(nullptr, viewProj));
    frame.camPos = g.camPos;
    frame.vsync = g.vsyncOn;
    frame.post = g.post;
    // With GI on, the flat ambient term drops and bounce light fills it back in.
    frame.ambient = g.post.giEnabled ? 0.20f : 0.34f;

    // Sun at a lower angle for readable shadows. The ortho box follows the
    // camera focus (tighter box = sharper shadows) and its translation snaps
    // to the shadow texel grid so edges don't shimmer while driving.
    frame.sunDir = XMFLOAT3(0.489f, 0.636f, 0.372f);   // pre-normalized
    {
        const float BoxSize = 58.0f;
        XMVECTOR sun = XMVectorSet(frame.sunDir.x, frame.sunDir.y, frame.sunDir.z, 0);
        XMMATRIX lightRot = XMMatrixLookAtRH(XMVectorScale(sun, 90.0f),
                                             XMVectorZero(),
                                             XMVectorSet(0, 1, 0, 0));
        const PlayerState& me = g.game.players[g.myId];
        XMVECTOR focus = XMVectorSet(me.active ? me.x : 0, 0, me.active ? me.z : 0, 1);
        XMVECTOR lsFocus = XMVector3TransformCoord(focus, lightRot);
        float texel = BoxSize / float(g.post.shadowMapSize);
        float sx = floorf(XMVectorGetX(lsFocus) / texel) * texel;
        float sy = floorf(XMVectorGetY(lsFocus) / texel) * texel;
        XMMATRIX lightView = lightRot * XMMatrixTranslation(-sx, -sy, 0);
        XMMATRIX lightProj = XMMatrixOrthographicRH(BoxSize, BoxSize, 1.0f, 200.0f);
        frame.lightViewProj = Store(XMMatrixMultiply(lightView, lightProj));
    }

    // vfx data
    frame.time = float(g.time);
    for (const auto& b : g.bursts)
        frame.bursts.push_back({ b.pos, float(g.time - b.t0) });
    for (const auto& s : g.scorches)
        frame.scorches.push_back({ s.pos, float(g.time - s.t0) });

    // ground + obstacles + boundary walls (with NRA material maps)
    {
        RenderObject ro{ g.meshGround, g.texGround, Store(XMMatrixIdentity()),
                         { 1,1,1,0 } };
        ro.texNormal = g.texGroundNRA;
        frame.objects.push_back(ro);
    }
    for (size_t i = 0; i < g.meshObstacles.size(); ++i)
    {
        const Obstacle& o = kObstacles[i];
        RenderObject ro{ g.meshObstacles[i], g.texWall,
            Store(XMMatrixTranslation(o.cx, o.height * 0.5f, o.cz)), { 1,1,1,0 } };
        ro.texNormal = g.texWallNRA;
        frame.objects.push_back(ro);
    }
    for (size_t i = 0; i < g.meshWalls.size(); ++i)
    {
        float h = ArenaHalf;
        XMMATRIX m = (i == 0) ? XMMatrixTranslation(0, 0.6f, h)
                   : (i == 1) ? XMMatrixTranslation(0, 0.6f, -h)
                   : (i == 2) ? XMMatrixTranslation(h, 0.6f, 0)
                              : XMMatrixTranslation(-h, 0.6f, 0);
        RenderObject ro{ g.meshWalls[i], g.texWall, Store(m), { 1,1,1,0 } };
        ro.texNormal = g.texWallNRA;
        frame.objects.push_back(ro);
    }

    // tanks (positions/angles interpolated for render smoothness)
    for (int i = 0; i < MaxPlayers; ++i)
    {
        const PlayerState& p = g.game.players[i];
        if (!p.active)
            continue;
        float rx, rz, rHull, rTurret;
        GetRenderPlayer(i, rx, rz, rHull, rTurret);
        bool dead = p.health <= 0;
        float sink = dead ? -0.35f : 0.0f;
        XMFLOAT4 tint = kPlayerTint[i % MaxLobbyPlayers];   // per-player identity
        if (p.possessTimer > 0)
            tint = { 0.45f, 0.6f, 1.5f, 0.15f };            // ghost-ridden blue
        if (p.hitFlash > 0)
            tint = { 1.0f, 0.25f, 0.2f, 0.35f };
        if (dead)
            tint = { 0.35f, 0.33f, 0.3f, 0 };

        XMMATRIX hull = XMMatrixRotationY(rHull)
                      * XMMatrixTranslation(rx, sink, rz);
        frame.objects.push_back({ g.meshHull, g.texPalette, Store(hull), tint, true });

        float sh = sinf(rHull), ch = cosf(rHull);
        XMFLOAT3 piv = g.game.turretPivot;
        XMFLOAT3 pivWorld{ rx + piv.x * ch + piv.z * sh,
                           piv.y + sink,
                           rz + piv.z * ch - piv.x * sh };
        XMMATRIX turret = XMMatrixRotationY(rTurret)
                        * XMMatrixTranslation(pivWorld.x, pivWorld.y, pivWorld.z);
        frame.objects.push_back({ g.meshTurret, g.texPalette, Store(turret), tint, true });

        // SHIELD barrier: a pale energy lattice riding the turret facing.
        // See-through by construction (slats + posts), since the scene pass
        // draws opaque geometry only.
        if (!dead && p.shieldTimer > 0.0f && g.meshShieldSlat >= 0)
        {
            float w2 = p.stats[int(Stat::ShieldWidth)] * 0.5f;
            float fx = sinf(rTurret), fz = cosf(rTurret);
            float cx = rx + fx * ShieldDist, cz = rz + fz * ShieldDist;
            float rgx = cosf(rTurret), rgz = -sinf(rTurret);   // lateral
            float flick = 0.9f + 0.1f * sinf(float(g.time) * 9.0f + i * 2.3f);
            XMFLOAT4 glow{ 0.5f * flick, 0.85f * flick, 1.1f * flick, 0.8f };
            XMMATRIX rot = XMMatrixRotationY(rTurret);
            const float slatY[3] = { 0.16f, 0.72f, 1.28f };
            for (float y : slatY)
                frame.objects.push_back({ g.meshShieldSlat, g.texWhite,
                    Store(XMMatrixScaling(w2, 1.0f, 1.0f) * rot
                          * XMMatrixTranslation(cx, y, cz)),
                    glow, true });
            for (int sgn = -1; sgn <= 1; sgn += 2)
                frame.objects.push_back({ g.meshShieldPost, g.texWhite,
                    Store(rot * XMMatrixTranslation(cx + rgx * w2 * sgn,
                                                    0.72f,
                                                    cz + rgz * w2 * sgn)),
                    glow, true });
        }

        if (p.muzzleFlash > 0 && !dead)
        {
            PlayerState rp = p;
            rp.x = rx; rp.z = rz; rp.hullYaw = rHull; rp.turretYaw = rTurret;
            XMFLOAT3 mw = g.game.MuzzleWorld(rp);
            float s = 0.6f + 6.0f * p.muzzleFlash;
            frame.objects.push_back({ g.meshFlash, g.texWhite,
                Store(XMMatrixScaling(s, s, s) * XMMatrixTranslation(mw.x, mw.y, mw.z)),
                { 1.0f, 0.85f, 0.3f, 1.0f }, true });
        }
    }

    // projectiles (interpolated like the tanks)
    bool clientMode = g.online && !g.isHost;
    for (int i = 0; i < MaxProjectiles; ++i)
    {
        const Projectile& pr = g.game.projectiles[i];
        if (!pr.active)
            continue;
        if (g.projVeiledBy[i] >= 0)
            continue;   // our provisional renders this rocket, never the slot
        float px = pr.x, py = pr.y, pz = pr.z;
        if (clientMode)
        {
            if (g.haveTwoSnaps && g.snapPrev.projectiles[i].active
                && g.snapCurr.projectiles[i].active)
            {
                const net::ProjectileNet& a = g.snapPrev.projectiles[i];
                const net::ProjectileNet& b = g.snapCurr.projectiles[i];
                float t = std::clamp(float((g.time - g.snapCurrTime)
                                           / (SnapshotEveryTicks * TickDt)), 0.0f, 1.0f);
                px = a.x + (b.x - a.x) * t;
                py = a.y + (b.y - a.y) * t;
                pz = a.z + (b.z - a.z) * t;
            }
        }
        else if (g.prevTick.projectiles[i].active)
        {
            const Projectile& a = g.prevTick.projectiles[i];
            px = a.x + (pr.x - a.x) * g.tickAlpha;
            py = a.y + (pr.y - a.y) * g.tickAlpha;
            pz = a.z + (pr.z - a.z) * g.tickAlpha;
        }
        // rockets render larger than sim size for readability; the deform
        // shader works in local units, so distance is mapped back through
        // the LENGTH scale (the "pipe" follows the rocket's physical length).
        // Length (local +Z, the travel axis) is 10% shorter than the girth.
        const float kRocketScale = 1.6f;
        const float kRocketLenScale = kRocketScale * 0.9f;
        XMMATRIX m = XMMatrixScaling(kRocketScale, kRocketScale, kRocketLenScale)
                   * XMMatrixRotationY(pr.yaw)
                   * XMMatrixTranslation(px, py, pz);
        XMFLOAT4 shellTint = pr.deflected
            ? XMFLOAT4{ 1.0f, 0.45f, 0.08f, 0.45f }   // shield-flipped
            : XMFLOAT4{ 0.16f, 0.14f, 0.11f, 0.30f };
        RenderObject ro{ g.meshProj, g.texWhite, Store(m), shellTint, true };
        // squish/spring shader inputs: distance from the recorded muzzle
        // position and seconds since fired (see UpdateVfxFromSim tracking)
        float sdx = px - g.projSpawnPos[i].x;
        float sdz = pz - g.projSpawnPos[i].z;
        ro.deformDist = sqrtf(sdx * sdx + sdz * sdz) / kRocketLenScale;
        ro.deformAge = float(g.time - g.projSpawnTime[i]);
        frame.objects.push_back(ro);

        // RADAR rockets: the packed circle tree, drawn where it detects
        if (pr.radarRange > 0.0f)
        {
            float ox[MaxRadarNodes], oz[MaxRadarNodes], rad[MaxRadarNodes];
            int dep[MaxRadarNodes];
            int nn = RadarTreeLayout(pr.radarRange, pr.radarRings, pr.yaw,
                                     ox, oz, rad, dep, MaxRadarNodes);
            for (int k = 0; k < nn; ++k)
                frame.objects.push_back({ g.meshRing, g.texWhite,
                    Store(XMMatrixScaling(rad[k], 1.0f, rad[k])
                          * XMMatrixTranslation(px + ox[k], py, pz + oz[k])),
                    { 1.0f, 0.14f, 0.1f, 1.0f }, true });
            // countdown: the root circle fills CLOCKWISE with muted red
            // ground wedges as the lock charges (lit like the floor, so it
            // reads as a translucent stain)
            int wedges = int(pr.radarLockFrac * 24.0f + 0.5f);
            for (int k = 0; k < wedges; ++k)
                frame.objects.push_back({ g.meshWedge, g.texWhite,
                    Store(XMMatrixScaling(pr.radarRange, 1.0f, pr.radarRange)
                          * XMMatrixRotationY(pr.yaw + float(k)
                                              * (XM_2PI / 24.0f))
                          * XMMatrixTranslation(px, 0.06f, pz)),
                    { 0.62f, 0.30f, 0.27f, 0 }, true });
        }
    }

    // ------------------------------------------------ necromancer visuals
    // skulls: the rigged model chomping its OpenAndClose loop, homing at
    // head height (procedural cranium+jaw only if the GLB is missing)
    for (int i = 0; i < MaxSkulls; ++i)
    {
        const SkullState& sk = g.game.skulls[i];
        if (!sk.active)
            continue;
        float sx = sk.x, sz = sk.z, syaw = sk.yaw;
        if (clientMode)
        {
            if (g.haveTwoSnaps && g.snapPrev.skulls[i].active
                && g.snapCurr.skulls[i].active)
            {
                const net::SkullNet& a = g.snapPrev.skulls[i];
                const net::SkullNet& b = g.snapCurr.skulls[i];
                float t = std::clamp(float((g.time - g.snapCurrTime)
                                           / (SnapshotEveryTicks * TickDt)),
                                     0.0f, 1.0f);
                sx = a.x + (b.x - a.x) * t;
                sz = a.z + (b.z - a.z) * t;
                syaw = LerpAngle(a.yaw, b.yaw, t);
            }
        }
        else if (g.prevTick.skulls[i].active)
        {
            const SkullState& a = g.prevTick.skulls[i];
            sx = a.x + (sk.x - a.x) * g.tickAlpha;
            sz = a.z + (sk.z - a.z) * g.tickAlpha;
            syaw = LerpAngle(a.yaw, sk.yaw, g.tickAlpha);
        }
        float bob = sinf(float(g.time) * 5.0f + float(i)) * 0.08f;
        if (g.skullModel.valid && !g.meshSkullParts.empty())
        {
            Animator& an = g.skullAnim[i];
            an.Update(g.frameDt);
            frame.palettes.emplace_back();
            an.Pose(frame.palettes.back());
            int paletteIdx = int(frame.palettes.size()) - 1;
            XMMATRIX world =
                XMMatrixScaling(g.skullScale, g.skullScale, g.skullScale)
                * XMMatrixRotationY(syaw)
                * XMMatrixTranslation(sx, SkullY + bob, sz);
            for (size_t pi = 0; pi < g.meshSkullParts.size(); ++pi)
            {
                // textured when the export has UVs, necro-bone tint otherwise
                XMFLOAT4 tint = g.texSkull >= 0
                    ? XMFLOAT4{ 1.0f, 1.0f, 1.0f, 0 }
                    : XMFLOAT4{ 0.85f, 0.95f, 0.72f, 0 };
                RenderObject ro{ g.meshSkullParts[pi],
                                 g.texSkull >= 0 ? g.texSkull : g.texWhite,
                                 Store(world), tint, true };
                ro.paletteIndex = paletteIdx;
                if (g.texSkullNRA >= 0)
                    ro.texNormal = g.texSkullNRA;
                frame.objects.push_back(ro);
            }
        }
        else
        {
            XMFLOAT4 green{ 0.35f, 1.0f, 0.35f, 0.25f };
            frame.objects.push_back({ g.meshSkull, g.texWhite,
                Store(XMMatrixRotationY(syaw)
                      * XMMatrixTranslation(sx, SkullY + bob, sz)),
                green, true });
            float open = 0.30f
                       + 0.28f * sinf(float(g.time) * 8.0f + float(i) * 1.7f);
            XMMATRIX jaw = XMMatrixTranslation(0, 0, 0.20f)
                         * XMMatrixRotationX(open)
                         * XMMatrixTranslation(0, -0.24f, -0.03f)
                         * XMMatrixRotationY(syaw)
                         * XMMatrixTranslation(sx, SkullY + bob, sz);
            frame.objects.push_back({ g.meshJaw, g.texWhite, Store(jaw),
                                      green, true });
        }
    }

    // acid puddles: flat green discs, shrinking out in their last second
    for (int i = 0; i < MaxPuddles; ++i)
    {
        const PuddleState& pu = g.game.puddles[i];
        if (!pu.active)
            continue;
        float fade = std::clamp(pu.life, 0.0f, 1.0f);
        float pulse = 1.0f + 0.05f * sinf(float(g.time) * 4.0f + float(i));
        float s = PuddleRadius * pulse * (0.35f + 0.65f * fade);
        frame.objects.push_back({ g.meshPuddle, g.texWhite,
            Store(XMMatrixScaling(s, 1.0f, s)
                  * XMMatrixTranslation(pu.x, 0.0f, pu.z)),
            { 0.3f, 0.9f, 0.18f, 0.3f }, true });
    }

    // ghosts: pale wisps spiraling toward their victim (through walls)
    for (int i = 0; i < MaxGhosts; ++i)
    {
        const GhostState& gh = g.game.ghosts[i];
        if (!gh.active)
            continue;
        float gx = gh.x, gz = gh.z;
        if (clientMode)
        {
            if (g.haveTwoSnaps && g.snapPrev.ghosts[i].active
                && g.snapCurr.ghosts[i].active)
            {
                const net::GhostNet& a = g.snapPrev.ghosts[i];
                const net::GhostNet& b = g.snapCurr.ghosts[i];
                float t = std::clamp(float((g.time - g.snapCurrTime)
                                           / (SnapshotEveryTicks * TickDt)),
                                     0.0f, 1.0f);
                gx = a.x + (b.x - a.x) * t;
                gz = a.z + (b.z - a.z) * t;
            }
        }
        else if (g.prevTick.ghosts[i].active)
        {
            const GhostState& a = g.prevTick.ghosts[i];
            gx = a.x + (gh.x - a.x) * g.tickAlpha;
            gz = a.z + (gh.z - a.z) * g.tickAlpha;
        }
        float bob = sinf(float(g.time) * 2.6f + float(i) * 2.1f) * 0.18f;
        float sway = 1.0f + 0.06f * sinf(float(g.time) * 3.7f + float(i));
        frame.objects.push_back({ g.meshGhost, g.texWhite,
            Store(XMMatrixScaling(sway, 1.0f, sway)
                  * XMMatrixRotationY(float(g.time) * 1.6f + float(i))
                  * XMMatrixTranslation(gx, 0.30f + bob, gz)),
            { 0.75f, 0.85f, 1.15f, 0.85f }, true });
    }

    // grenades: interpolated ballistic hops, red pulse once the fuse is lit
    for (int i = 0; i < MaxGrenades; ++i)
    {
        const GrenadeState& gr = g.game.grenades[i];
        if (!gr.active)
            continue;
        float gx = gr.x, gy = gr.y, gz = gr.z;
        bool armed = gr.fuse >= 0.0f;
        if (clientMode)
        {
            if (g.haveTwoSnaps && g.snapPrev.grenades[i].active
                && g.snapCurr.grenades[i].active)
            {
                const net::GrenadeNet& a = g.snapPrev.grenades[i];
                const net::GrenadeNet& b = g.snapCurr.grenades[i];
                float t = std::clamp(float((g.time - g.snapCurrTime)
                                           / (SnapshotEveryTicks * TickDt)),
                                     0.0f, 1.0f);
                gx = a.x + (b.x - a.x) * t;
                gy = a.y + (b.y - a.y) * t;
                gz = a.z + (b.z - a.z) * t;
                armed = b.fuse255 != 255;
            }
        }
        else if (g.prevTick.grenades[i].active)
        {
            const GrenadeState& a = g.prevTick.grenades[i];
            gx = a.x + (gr.x - a.x) * g.tickAlpha;
            gy = a.y + (gr.y - a.y) * g.tickAlpha;
            gz = a.z + (gr.z - a.z) * g.tickAlpha;
        }
        XMFLOAT4 tint{ 0.36f, 0.42f, 0.28f, 0.3f };
        if (armed && fmodf(float(g.time) * 8.0f, 1.0f) < 0.45f)
            tint = { 1.0f, 0.25f, 0.15f, 0.6f };   // fuse lit: red strobe
        frame.objects.push_back({ g.meshGrenade >= 0 ? g.meshGrenade
                                                     : g.meshProj,
            g.texWhite,
            Store(XMMatrixScaling(g.grenadeScale, g.grenadeScale,
                                  g.grenadeScale)
                  * XMMatrixRotationY(float(g.time) * 6.0f + float(i))
                  * XMMatrixTranslation(gx, gy, gz)),
            tint, true });
    }

    // soldier summons: skinned, animated locally from the replicated state
    // (fire-and-forget visuals -- the host only ships position/state/flags)
    if (!g.meshRigParts.empty() && g.rigModel.valid)
    {
        for (int i = 0; i < MaxSoldiers; ++i)
        {
            const SoldierState& s = g.game.soldiers[i];
            if (!s.active)
                continue;

            // position/yaw interpolation, same pattern as the projectiles
            float sx = s.x, sz = s.z, syaw = s.yaw;
            if (clientMode)
            {
                if (g.haveTwoSnaps && g.snapPrev.soldiers[i].active
                    && g.snapCurr.soldiers[i].active)
                {
                    const net::SoldierNet& a = g.snapPrev.soldiers[i];
                    const net::SoldierNet& b = g.snapCurr.soldiers[i];
                    float t = std::clamp(float((g.time - g.snapCurrTime)
                                               / (SnapshotEveryTicks * TickDt)),
                                         0.0f, 1.0f);
                    sx = a.x + (b.x - a.x) * t;
                    sz = a.z + (b.z - a.z) * t;
                    syaw = LerpAngle(a.yaw, b.yaw, t);
                }
            }
            else if (g.prevTick.soldiers[i].active)
            {
                const SoldierState& a = g.prevTick.soldiers[i];
                sx = a.x + (s.x - a.x) * g.tickAlpha;
                sz = a.z + (s.z - a.z) * g.tickAlpha;
                syaw = LerpAngle(a.yaw, s.yaw, g.tickAlpha);
            }

            // drive the animator from the replicated state (transitions only)
            Animator& an = g.soldierAnim[i];
            const App::SoldierClips& sc = g.soldierClips;
            if (g.soldierAnimState[i] != s.state || an.layers[0].clip < 0)
            {
                g.soldierAnimState[i] = s.state;
                an.layers[1].targetWeight = 0;
                switch (s.state)
                {
                case SoldierCover:
                    if (sc.duck >= 0) an.PlayLayer(0, sc.duck, true, 1.0f);
                    break;
                case SoldierMove:
                case SoldierKite:
                case SoldierPeek:
                    if (sc.run >= 0) an.PlayLayer(0, sc.run, true, 1.0f);
                    if (sc.shoot >= 0)
                    {
                        // gun-up torso layered over the running legs
                        an.PlayLayer(1, sc.shoot, true, 1.0f);
                        an.layers[1].mask = MaskSubtree(g.rigModel, "Torso");
                    }
                    break;
                case SoldierDying:
                    if (sc.death >= 0)
                        an.PlayLayer(0, sc.death, false, 1.0f, true);
                    break;
                default:   // guard
                    if (sc.idle >= 0) an.PlayLayer(0, sc.idle, true, 1.0f);
                    break;
                }
            }

            // torso aim at the target tank while peeking/kiting
            bool aiming = (s.state == SoldierMove || s.state == SoldierKite
                           || s.state == SoldierPeek)
                       && s.targetId < MaxPlayers
                       && g.game.players[s.targetId].active;
            an.aim.active = aiming;
            if (aiming)
            {
                const PlayerState& t = g.game.players[s.targetId];
                // world -> soldier model space (inverse of RotY(yaw)+T)
                float dx = t.x - sx, dz = t.z - sz;
                float cy = cosf(syaw), sy = sinf(syaw);
                an.aim.target = XMFLOAT3(dx * cy - dz * sy, 1.2f,
                                         dz * cy + dx * sy);
            }

            an.Update(g.frameDt);
            frame.palettes.emplace_back();
            int paletteIdx = int(frame.palettes.size()) - 1;

            // PASS 1 (no IK): find where the chest ends up, so the launcher
            // can sit on the right shoulder and the hand can reach for it
            an.ik[0].active = false;
            an.Pose(frame.palettes.back());
            XMMATRIX launcherM{};
            bool haveLauncher = g.meshLauncher >= 0 && g.rigChest >= 0;
            if (haveLauncher)
            {
                XMMATRIX chestG = XMLoadFloat4x4(&g.rigChestBind)
                    * XMLoadFloat4x4(&frame.palettes[paletteIdx]
                                          .m[g.rigChest]);
                XMVECTOR cpos = chestG.r[3];   // model space (real units)
                // tube points where the torso aims (yaw only)
                float fyaw = 0.0f;
                if (an.aim.active)
                    fyaw = atan2f(an.aim.target.x, an.aim.target.z);
                float fx = sinf(fyaw), fz = cosf(fyaw);
                float rgx = cosf(fyaw), rgz = -sinf(fyaw);   // right axis
                // rear rides the shoulder: up + right of the chest, center
                // pushed forward so the back end sits ON the shoulder pad
                XMVECTOR pos = cpos
                    + XMVectorSet(rgx * 0.18f, 0.28f, rgz * 0.18f, 0)
                    + XMVectorSet(fx * 0.30f, 0, fz * 0.30f, 0);
                launcherM = XMMatrixScaling(g.launcherScale,
                                            g.launcherScale,
                                            g.launcherScale)
                          * XMMatrixRotationX(0.10f)   // nose dips a touch
                          * XMMatrixRotationY(fyaw)
                          * XMMatrixTranslationFromVector(pos);
                // the grip: under the tube, forward of center -- the right
                // hand IKs onto it, the left arm keeps its animation
                if (g.rigArmR[0] >= 0 && g.rigArmR[1] >= 0
                    && g.rigArmR[2] >= 0)
                {
                    XMVECTOR handle = XMVector3TransformCoord(
                        XMVectorSet(0, -0.10f / g.launcherScale,
                                    0.24f / g.launcherScale, 1),
                        launcherM);
                    an.ik[0].a = g.rigArmR[0];
                    an.ik[0].b = g.rigArmR[1];
                    an.ik[0].c = g.rigArmR[2];
                    an.ik[0].active = true;
                    an.ik[0].weight = 1.0f;
                    XMStoreFloat3(&an.ik[0].target, handle);
                    XMVECTOR pole = cpos
                        + XMVectorSet(rgx * 0.7f, -0.25f, rgz * 0.7f, 0)
                        + XMVectorSet(fx * 0.35f, 0, fz * 0.35f, 0);
                    XMStoreFloat3(&an.ik[0].pole, pole);
                    // PASS 2: same pose, right arm solved onto the grip
                    an.Pose(frame.palettes[paletteIdx]);
                }
            }

            XMMATRIX world = XMMatrixScaling(g.rigScale, g.rigScale,
                                             g.rigScale)
                           * XMMatrixRotationY(syaw)
                           * XMMatrixTranslation(sx, 0.0f, sz);
            for (size_t p = 0; p < g.meshRigParts.size(); ++p)
            {
                if (!g.rigPartVisible[p])
                    continue;
                const XMFLOAT4& c = g.rigPartColors[p];
                XMFLOAT4 tint{ c.x, c.y, c.z, 0 };
                if (s.hitFlash > 0)
                    tint = { 1.0f, 0.3f, 0.25f, 0.35f };
                RenderObject ro{ g.meshRigParts[p],
                                 g.texRig >= 0 ? g.texRig : g.texWhite,
                                 Store(world), tint, true };
                ro.paletteIndex = paletteIdx;
                frame.objects.push_back(ro);
            }

            // the launcher itself, on the shoulder (model space -> world)
            if (haveLauncher)
                frame.objects.push_back({ g.meshLauncher, g.texWhite,
                    Store(launcherM * world),
                    { 0.30f, 0.32f, 0.28f, 0.25f }, true });

            // launcher muzzle flash (the rocket itself is a real projectile
            // in the shared pool, drawn by the normal rocket path)
            if (s.muzzleFlash > 0 && s.state != SoldierDying)
            {
                float gx = sx + sinf(syaw) * 0.7f;
                float gz = sz + cosf(syaw) * 0.7f;
                float fs = 0.3f + 2.6f * s.muzzleFlash;
                frame.objects.push_back({ g.meshFlash, g.texWhite,
                    Store(XMMatrixScaling(fs, fs, fs)
                          * XMMatrixTranslation(gx, SoldierGunY, gz)),
                    { 1.0f, 0.85f, 0.3f, 1.0f }, true });
            }
        }
    }

    // animated test rig (--rigtest): posed once on the CPU, all parts share
    // the palette and carry their material base colors
    if (g.opt.rigTest && !g.meshRigParts.empty() && g.rigModel.valid)
    {
        // live constraint targets, world -> the soldier's model space
        {
            float rx, rz, rh, rt;
            GetRenderPlayer(g.myId, rx, rz, rh, rt);
            g.rigAnimator.aim.target = XMFLOAT3(
                (rx - g.rigPos.x) / g.rigScale, 1.2f,
                (rz - g.rigPos.z) / g.rigScale);
        }
        Animator& an = g.rigAnimator;
        frame.palettes.emplace_back();
        int paletteIdx = int(frame.palettes.size()) - 1;
        an.ik[0].active = false;
        an.Pose(frame.palettes.back());
        XMMATRIX rigLauncherM{};
        bool rigHaveL = g.meshLauncher >= 0 && g.rigChest >= 0;
        if (rigHaveL)
        {
            XMMATRIX chestG = XMLoadFloat4x4(&g.rigChestBind)
                * XMLoadFloat4x4(&frame.palettes[paletteIdx].m[g.rigChest]);
            XMVECTOR cpos = chestG.r[3];
            if (g.frameCounter % 240 == 0)
                Log("rigtest chest at (%.2f %.2f %.2f)",
                    XMVectorGetX(cpos), XMVectorGetY(cpos),
                    XMVectorGetZ(cpos));
            float fyaw = an.aim.active
                ? atan2f(an.aim.target.x, an.aim.target.z) : 0.0f;
            float fx = sinf(fyaw), fz = cosf(fyaw);
            float rgx = cosf(fyaw), rgz = -sinf(fyaw);
            XMVECTOR pos = cpos
                + XMVectorSet(rgx * 0.18f, 0.28f, rgz * 0.18f, 0)
                + XMVectorSet(fx * 0.30f, 0, fz * 0.30f, 0);
            rigLauncherM = XMMatrixScaling(g.launcherScale, g.launcherScale,
                                           g.launcherScale)
                         * XMMatrixRotationX(0.10f)
                         * XMMatrixRotationY(fyaw)
                         * XMMatrixTranslationFromVector(pos);
            if (g.rigArmR[0] >= 0 && g.rigArmR[1] >= 0 && g.rigArmR[2] >= 0)
            {
                XMVECTOR handle = XMVector3TransformCoord(
                    XMVectorSet(0, -0.10f / g.launcherScale,
                                0.24f / g.launcherScale, 1), rigLauncherM);
                an.ik[0].a = g.rigArmR[0];
                an.ik[0].b = g.rigArmR[1];
                an.ik[0].c = g.rigArmR[2];
                an.ik[0].active = true;
                an.ik[0].weight = 1.0f;
                XMStoreFloat3(&an.ik[0].target, handle);
                XMVECTOR pole = cpos
                    + XMVectorSet(rgx * 0.7f, -0.25f, rgz * 0.7f, 0)
                    + XMVectorSet(fx * 0.35f, 0, fz * 0.35f, 0);
                XMStoreFloat3(&an.ik[0].pole, pole);
                an.Pose(frame.palettes[paletteIdx]);
            }
        }
        XMMATRIX world = XMMatrixScaling(g.rigScale, g.rigScale, g.rigScale)
                       * XMMatrixTranslation(g.rigPos.x, g.rigPos.y, g.rigPos.z);
        if (rigHaveL)
            frame.objects.push_back({ g.meshLauncher, g.texWhite,
                Store(rigLauncherM * world),
                { 0.30f, 0.32f, 0.28f, 0.25f }, true });
        for (size_t p = 0; p < g.meshRigParts.size(); ++p)
        {
            if (!g.rigPartVisible[p])
                continue;
            const XMFLOAT4& c = g.rigPartColors[p];
            RenderObject ro{ g.meshRigParts[p],
                             g.texRig >= 0 ? g.texRig : g.texWhite,
                             Store(world), { c.x, c.y, c.z, 0 }, true };
            ro.paletteIndex = paletteIdx;
            frame.objects.push_back(ro);
        }
    }

    // provisional (predicted-fire) rockets: identical rendering; the server
    // twin adopts their spawn record on arrival so the deform is continuous
    for (const auto& pv : g.provRockets)
    {
        if (!pv.sim.active)
            continue;
        const float kRocketScale = 1.6f;
        const float kRocketLenScale = kRocketScale * 0.9f;
        XMMATRIX m = XMMatrixScaling(kRocketScale, kRocketScale, kRocketLenScale)
                   * XMMatrixRotationY(pv.sim.yaw)
                   * XMMatrixTranslation(pv.sim.x, pv.sim.y, pv.sim.z);
        RenderObject ro{ g.meshProj, g.texWhite, Store(m),
                         { 0.16f, 0.14f, 0.11f, 0.30f }, true };
        float sdx = pv.sim.x - pv.spawn.x;
        float sdz = pv.sim.z - pv.spawn.z;
        ro.deformDist = sqrtf(sdx * sdx + sdz * sdz) / kRocketLenScale;
        ro.deformAge = float(g.time - pv.born);
        frame.objects.push_back(ro);
        if (pv.sim.radarRange > 0.0f)
        {
            float ox[MaxRadarNodes], oz[MaxRadarNodes], rad[MaxRadarNodes];
            int dep[MaxRadarNodes];
            int nn = RadarTreeLayout(pv.sim.radarRange, pv.sim.radarRings,
                                     pv.sim.yaw, ox, oz, rad, dep,
                                     MaxRadarNodes);
            for (int k = 0; k < nn; ++k)
                frame.objects.push_back({ g.meshRing, g.texWhite,
                    Store(XMMatrixScaling(rad[k], 1.0f, rad[k])
                          * XMMatrixTranslation(pv.sim.x + ox[k], pv.sim.y,
                                                pv.sim.z + oz[k])),
                    { 1.0f, 0.14f, 0.1f, 1.0f }, true });
            // countdown fill: authority lives on the veiled server twin
            float frac = pv.matchedSlot >= 0
                ? g.game.projectiles[pv.matchedSlot].radarLockFrac : 0.0f;
            int wedges = int(frac * 24.0f + 0.5f);
            for (int k = 0; k < wedges; ++k)
                frame.objects.push_back({ g.meshWedge, g.texWhite,
                    Store(XMMatrixScaling(pv.sim.radarRange, 1.0f,
                                          pv.sim.radarRange)
                          * XMMatrixRotationY(pv.sim.yaw + float(k)
                                              * (XM_2PI / 24.0f))
                          * XMMatrixTranslation(pv.sim.x, 0.06f, pv.sim.z)),
                    { 0.62f, 0.30f, 0.27f, 0 }, true });
        }
    }
}

// Host: validate a purchase, then replicate the upgrade EVENT (not the
// resulting stats) -- every peer appends to that player's owned list and
// re-derives stats with the identical RecalcStats: additions first, then
// multiplications, always.
bool HostPurchase(int pid, int slot)
{
    uint8_t type = g.game.players[pid].offers[slot].type;
    if (!g.game.TryPurchase(pid, slot))
        return false;
    Log("Purchase: player %d type %d (owned %zu) -> broadcast", pid, int(type),
        g.game.players[pid].owned.size());
    if (g.online)
    {
        g.net.BroadcastUpgrade(pid, type);
        // Class cards also granted their base upgrade inside TryPurchase;
        // replicate it as a second plain upgrade event so every client's
        // owned list (and its RecalcStats) matches the host exactly.
        UpgradeId grant = kUpgradePool[type].grant;
        if (grant != UpgradeId::Count)
            g.net.BroadcastUpgrade(pid, uint8_t(grant));
    }
    return true;
}

std::vector<UiButton> MenuButtons()
{
    float w = float(g.width), h = float(g.height);
    float bw = 320, bh = 54, gap = 18;
    float x = w * 0.5f - bw * 0.5f;
    float y = h * 0.40f;
    std::vector<UiButton> buttons;
    if (g.screen == Screen::MainMenu)
    {
        y = h * 0.36f;   // six buttons: start a little higher
        buttons.push_back({ x, y + 0 * (bh + gap), bw, bh, "FIND MATCH" });
        buttons.push_back({ x, y + 1 * (bh + gap), bw, bh, "PLAY SOLO" });
        buttons.push_back({ x, y + 2 * (bh + gap), bw, bh, "HOST GAME" });
        buttons.push_back({ x, y + 3 * (bh + gap), bw, bh, "JOIN GAME" });
        buttons.push_back({ x, y + 4 * (bh + gap), bw, bh, "SETTINGS" });
        buttons.push_back({ x, y + 5 * (bh + gap), bw, bh, "QUIT" });
    }
    else if (g.screen == Screen::JoinEntry)
    {
        buttons.push_back({ x, y + 1.2f * (bh + gap), bw, bh, "CONNECT" });
        buttons.push_back({ x, y + 2.2f * (bh + gap), bw, bh, "BACK" });
    }
    else if (g.screen == Screen::Connecting)
    {
        buttons.push_back({ x, y + 2.2f * (bh + gap), bw, bh, "CANCEL" });
    }
    else if (g.screen == Screen::Paused)
    {
        buttons.push_back({ x, y + 0 * (bh + gap), bw, bh, "RESUME" });
        buttons.push_back({ x, y + 1 * (bh + gap), bw, bh, "SETTINGS" });
        buttons.push_back({ x, y + 2 * (bh + gap), bw, bh, "LEAVE GAME" });
        buttons.push_back({ x, y + 3 * (bh + gap), bw, bh, "QUIT" });
    }
    else if (g.screen == Screen::MatchSize)
    {
        buttons.push_back({ x, y + 0 * (bh + gap), bw, bh, "2 PLAYERS" });
        buttons.push_back({ x, y + 1 * (bh + gap), bw, bh, "3 PLAYERS" });
        buttons.push_back({ x, y + 2 * (bh + gap), bw, bh, "4 PLAYERS" });
        buttons.push_back({ x, y + 3 * (bh + gap), bw, bh, "BACK" });
    }
    else if (g.screen == Screen::Settings)
    {
        char buf[64];
        std::vector<std::string> labels;
        sprintf_s(buf, "RENDERER: %s",
                  g.currentBackend == Backend::D3D12 ? "D3D12" : "D3D11");
        labels.push_back(buf);
        sprintf_s(buf, "DISPLAY: %s", g.fullscreen ? "FULLSCREEN" : "WINDOWED");
        labels.push_back(buf);
        sprintf_s(buf, "RESOLUTION: %dx%d",
                  kResolutions[g.resIndex].w, kResolutions[g.resIndex].h);
        labels.push_back(buf);
        sprintf_s(buf, "ANTI-ALIASING: %s", g.post.aaEnabled ? "ON" : "OFF");
        labels.push_back(buf);
        sprintf_s(buf, "LAG COMP: %s", g.lagComp ? "ON" : "OFF");
        labels.push_back(buf);
        sprintf_s(buf, "GI: %s", g.post.giEnabled ? "ON" : "OFF");
        labels.push_back(buf);
        sprintf_s(buf, "GI RAYS: %d", g.post.giRays);
        labels.push_back(buf);
        sprintf_s(buf, "TEMPORAL SAMPLES: %d", g.post.temporalSamples);
        labels.push_back(buf);
        sprintf_s(buf, "GI RES: %s", g.post.giHalfRes ? "HALF" : "FULL");
        labels.push_back(buf);
        sprintf_s(buf, "SSAO: %s", g.post.aoEnabled ? "ON" : "OFF");
        labels.push_back(buf);
        sprintf_s(buf, "SHADOWS: %s", g.post.shadowsEnabled ? "ON" : "OFF");
        labels.push_back(buf);
        sprintf_s(buf, "SHADOW RES: %d", g.post.shadowMapSize);
        labels.push_back(buf);
        static const char* filterNames[3] = { "SHARP", "SOFT", "SOFTER" };
        sprintf_s(buf, "SHADOW FILTER: %s", filterNames[g.post.shadowFilter]);
        labels.push_back(buf);
        labels.push_back("BACK");

        float sbh = 50, sgap = 12;
        for (size_t i = 0; i < labels.size(); ++i)
        {
            int col = int(i) / 7, row = int(i) % 7;
            buttons.push_back({ w * 0.5f - 340 + col * 360,
                                h * 0.30f + row * (sbh + sgap),
                                bw, sbh, labels[i] });
        }
    }
    return buttons;
}

void BuildMenu()
{
    float w = float(g.width), h = float(g.height);
    bool overlay = InSession();   // pause/settings floating over a live game
    g.ui.Rect(0, 0, w, h, { 0.05f, 0.07f, 0.05f, overlay ? 0.55f : 0.35f });
    if (overlay)
    {
        if (g.screen == Screen::Paused)
        {
            g.ui.TextCentered(w * 0.5f, h * 0.16f, 5.0f, { 0.85f, 0.95f, 0.7f, 1 },
                              "PAUSED");
            g.ui.TextCentered(w * 0.5f, h * 0.16f + 46, 1.6f, { 1, 1, 1, 0.55f },
                              g.online ? "the match keeps running"
                                       : "the game keeps running");
        }
    }
    else
    {
        g.ui.TextCentered(w * 0.5f, h * 0.16f, 7.0f, { 0.85f, 0.95f, 0.7f, 1 }, "TANKAQ");
        g.ui.TextCentered(w * 0.5f, h * 0.16f + 58, 1.8f, { 1, 1, 1, 0.6f },
                          "multiplayer tank arena");

        char buf[256];
        sprintf_s(buf, "%s on %s", g.renderer->Name(), g.gpu.name.c_str());
        g.ui.TextCentered(w * 0.5f, h - 54, 1.5f, { 1, 1, 1, 0.5f }, buf);
        if (g.net.steamAvailable())
            sprintf_s(buf, "steam: %s", g.net.myName().c_str());
        else
            sprintf_s(buf, "steam: NOT AVAILABLE - start Steam to host or join");
        g.ui.TextCentered(w * 0.5f, h - 34, 1.5f,
                          g.net.steamAvailable() ? UiColor{ 0.7f, 0.9f, 0.7f, 0.8f }
                                                 : UiColor{ 1, 0.6f, 0.4f, 0.9f }, buf);
    }

    if (g.screen == Screen::JoinEntry)
    {
        float bw = 420, bh = 44;
        float x = w * 0.5f - bw * 0.5f, y = h * 0.40f;
        g.ui.TextCentered(w * 0.5f, y - 26, 1.8f, { 1, 1, 1, 0.85f },
                          "enter game code (or ip:port for LAN)");
        g.ui.Rect(x, y, bw, bh, { 0.1f, 0.12f, 0.1f, 0.95f });
        g.ui.RectOutline(x, y, bw, bh, 2, { 0.6f, 0.7f, 0.5f, 1 });
        std::string shown = g.joinText;
        if (fmod(g.time, 1.0) < 0.5)
            shown += "_";
        g.ui.Text(x + 10, y + 12, 2.4f, { 1, 1, 0.9f, 1 }, shown);
        g.ui.TextCentered(w * 0.5f, y + bh + 8, 1.4f, { 1, 1, 1, 0.5f },
                          "type or CTRL+V to paste");
    }
    if (g.screen == Screen::Connecting)
    {
        char cbuf[128];
        sprintf_s(cbuf, g.searching ? "SEARCHING FOR A MATCH... %.0fs"
                                    : "CONNECTING... %.0fs",
                  g.time - g.connectStart);
        g.ui.TextCentered(w * 0.5f, h * 0.45f, 2.6f, { 1, 1, 0.7f, 1 }, cbuf);
        std::string relayDetail;
        int avail = g.net.relayStatus(relayDetail);
        if (avail != 100)   // k_ESteamNetworkingAvailability_Current
        {
            sprintf_s(cbuf, "steam relay warming up (%d)...", avail);
            g.ui.TextCentered(w * 0.5f, h * 0.45f + 34, 1.6f, { 1, 0.85f, 0.5f, 0.9f }, cbuf);
        }
    }
    if (g.screen == Screen::Settings)
        g.ui.TextCentered(w * 0.5f, h * 0.30f - 40, 2.2f, { 0.9f, 1, 0.85f, 1 },
                          "SETTINGS - effective GI samples = rays x temporal");
    if (g.screen == Screen::MatchSize)
        g.ui.TextCentered(w * 0.5f, h * 0.36f - 44, 2.4f, { 0.9f, 1, 0.85f, 1 },
                          "MATCH SIZE - how many players?");

    if (!g.statusLine.empty())
        g.ui.TextCentered(w * 0.5f, h * 0.33f, 1.8f, { 1, 0.7f, 0.5f, 1 }, g.statusLine);

    auto buttons = MenuButtons();
    for (size_t i = 0; i < buttons.size(); ++i)
    {
        bool hov = buttons[i].Contains(float(g.mouseX), float(g.mouseY));
        if (hov)
            g.hoverKeyNow = 100 + int(g.screen) * 32 + int(i);
        DrawButton(g.ui, buttons[i], hov);
    }
}

void HandleMenuClick()
{
    auto buttons = MenuButtons();
    for (const UiButton& b : buttons)
    {
        if (!b.Contains(float(g.mouseX), float(g.mouseY)))
            continue;
        snd::Play(snd::Sfx::Click, 0.5f, SndJitter(0.06f));
        if (b.label == "FIND MATCH") { g.screen = Screen::MatchSize; g.statusLine.clear(); }
        else if (b.label == "2 PLAYERS") StartFindMatch(2);
        else if (b.label == "3 PLAYERS") StartFindMatch(3);
        else if (b.label == "4 PLAYERS") StartFindMatch(4);
        else if (b.label == "PLAY SOLO") StartSolo();
        else if (b.label == "HOST GAME")
        {
            // hosting counts as matchmaking: advertise as an open host
            // (need = 0, joinable by searchers of any size; toggleable in lobby)
            StartHost();
            if (g.screen == Screen::InGame && g.online)
                g.net.CreatePublicLobby(0);
        }
        else if (b.label == "JOIN GAME") { g.screen = Screen::JoinEntry; g.statusLine.clear(); }
        else if (b.label == "SETTINGS")
        {
            // remember where we came from so BACK/ESC overlays return there
            g.settingsReturn = g.screen;
            g.screen = Screen::Settings;
            g.statusLine.clear();
        }
        else if (b.label == "QUIT") g.wantQuit = true;
        else if (b.label == "RESUME") g.screen = Screen::InGame;
        else if (b.label == "LEAVE GAME") LeaveToMenu(g.online ? "left the game" : "");
        else if (b.label == "CONNECT") { if (!g.joinText.empty()) StartJoin(g.joinText); }
        else if (b.label == "BACK")
        {
            g.screen = g.screen == Screen::Settings ? g.settingsReturn
                                                    : Screen::MainMenu;
            g.statusLine.clear();
        }
        // MatchSize BACK falls through to the branch above (-> MainMenu)
        else if (b.label == "CANCEL") LeaveToMenu("join cancelled");
        else if (b.label.rfind("GI:", 0) == 0) g.post.giEnabled = !g.post.giEnabled;
        else if (b.label.rfind("GI RAYS:", 0) == 0)
        {
            static const int steps[] = { 1, 2, 4, 8, 16 };
            int cur = 0;
            for (int s = 0; s < 5; ++s) if (steps[s] == g.post.giRays) cur = s;
            g.post.giRays = steps[(cur + 1) % 5];
        }
        else if (b.label.rfind("TEMPORAL", 0) == 0)
        {
            static const int steps[] = { 2, 4, 8, 16 };
            int cur = 0;
            for (int s = 0; s < 4; ++s) if (steps[s] == g.post.temporalSamples) cur = s;
            g.post.temporalSamples = steps[(cur + 1) % 4];
        }
        else if (b.label.rfind("GI RES:", 0) == 0) g.post.giHalfRes = !g.post.giHalfRes;
        else if (b.label.rfind("SSAO:", 0) == 0) g.post.aoEnabled = !g.post.aoEnabled;
        else if (b.label.rfind("SHADOWS:", 0) == 0) g.post.shadowsEnabled = !g.post.shadowsEnabled;
        else if (b.label.rfind("SHADOW RES:", 0) == 0)
            g.post.shadowMapSize = g.post.shadowMapSize == 1024 ? 2048
                                 : g.post.shadowMapSize == 2048 ? 4096 : 1024;
        else if (b.label.rfind("SHADOW FILTER:", 0) == 0)
            g.post.shadowFilter = (g.post.shadowFilter + 1) % 3;
        else if (b.label.rfind("ANTI-ALIASING:", 0) == 0)
            g.post.aaEnabled = !g.post.aaEnabled;
        else if (b.label.rfind("LAG COMP:", 0) == 0)
            g.lagComp = !g.lagComp;
        else if (b.label.rfind("RENDERER:", 0) == 0)
            SwitchRenderer(g.currentBackend == Backend::D3D12 ? Backend::D3D11
                                                              : Backend::D3D12);
        else if (b.label.rfind("DISPLAY:", 0) == 0)
        {
            g.fullscreen = !g.fullscreen;
            ApplyDisplayMode();
        }
        else if (b.label.rfind("RESOLUTION:", 0) == 0)
        {
            g.resIndex = (g.resIndex + 1) % int(std::size(kResolutions));
            ApplyResolution();
        }
        break;
    }
}

// ------------------------------------------------------------------ win32

void CopyTextToClipboard(const std::string& text)
{
    if (!OpenClipboard(g.hwnd))
        return;
    EmptyClipboard();
    if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1))
    {
        if (void* p = GlobalLock(h))
        {
            memcpy(p, text.c_str(), text.size() + 1);
            GlobalUnlock(h);
            SetClipboardData(CF_TEXT, h);
        }
    }
    CloseClipboard();
    Log("Game code copied to clipboard");
}

void PasteClipboard()
{
    if (!OpenClipboard(g.hwnd))
        return;
    if (HANDLE h = GetClipboardData(CF_TEXT))
    {
        if (const char* txt = static_cast<const char*>(GlobalLock(h)))
        {
            for (const char* c = txt; *c && g.joinText.size() < 32; ++c)
                if (isalnum(uint8_t(*c)) || *c == '.' || *c == ':')
                    g.joinText += *c;
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_SIZE:
        g.width = LOWORD(lp);
        g.height = HIWORD(lp);
        if (g.renderer && wp != SIZE_MINIMIZED)
            g.renderer->Resize(g.width, g.height);
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE)
            memset(g.keys, 0, sizeof(g.keys));
        return 0;
    case WM_KEYDOWN:
        if (wp < 256) g.keys[wp] = true;
        if (wp == VK_ESCAPE)
        {
            if (g.screen == Screen::InGame)
            {
                // pause OVERLAYS the running game; nothing is torn down
                g.screen = Screen::Paused;
                g.shopOpen = false;
            }
            else if (g.screen == Screen::Paused)
                g.screen = Screen::InGame;
            else if (g.screen == Screen::Settings)
                g.screen = g.settingsReturn;
            else if (g.screen == Screen::JoinEntry
                     || g.screen == Screen::MatchSize)
                g.screen = Screen::MainMenu;
        }
        if (wp == VK_F5) g.post.giEnabled = !g.post.giEnabled;
        if (wp == VK_F6) g.post.aoEnabled = !g.post.aoEnabled;
        if (wp == VK_F7) g.post.shadowsEnabled = !g.post.shadowsEnabled;
        if (wp == VK_TAB && g.screen == Screen::InGame
            && (g.game.phase == PhasePlaying || g.game.phase == PhaseOvertime))
        {
            if (g.shopOpen) g.shopOpen = false;
            else OpenShop();
        }
        if (wp == 'R')
            ToggleReady();
        if (g.screen == Screen::JoinEntry)
        {
            if (wp == VK_BACK && !g.joinText.empty())
                g.joinText.pop_back();
            if (wp == VK_RETURN && !g.joinText.empty())
                StartJoin(g.joinText);
            if (wp == 'V' && (GetKeyState(VK_CONTROL) & 0x8000))
                PasteClipboard();
        }
        return 0;
    case WM_KEYUP:
        if (wp < 256) g.keys[wp] = false;
        return 0;
    case WM_CHAR:
        if (g.screen == Screen::JoinEntry && g.joinText.size() < 32)
        {
            char c = char(wp);
            if (isalnum(uint8_t(c)) || c == '.' || c == ':')
                g.joinText += c;
        }
        return 0;
    case WM_MOUSEMOVE:
        g.mouseX = GET_X_LPARAM(lp);
        g.mouseY = GET_Y_LPARAM(lp);
        return 0;
    case WM_LBUTTONDOWN:
        g.mouseDown = true;
        g.clicked = true;
        SetCapture(hwnd);
        return 0;
    case WM_LBUTTONUP:
        g.mouseDown = false;
        ReleaseCapture();
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

bool CreateAssets()
{
    g.meshObstacles.clear();   // re-entrant: renderer switches recreate assets
    g.meshWalls.clear();
    g.meshRigParts.clear();    // these grew forever on every settings-menu
    g.rigPartColors.clear();   // renderer switch: the first batch turned into
    g.rigPartVisible.clear();  // stale handles = white garbage soldiers
    g.meshSkullParts.clear();
    g.tank = LoadTankModel("assets/tank/tank_baked.glb", "assets/tank/tank_meta.txt");
    if (!g.tank.valid)
        return false;
    g.game.turretPivot = g.tank.turretPivot;
    g.game.muzzleOffset = g.tank.muzzle;

    IRenderer* r = g.renderer;
    g.meshHull = r->CreateMesh(g.tank.hull.verts.data(), g.tank.hull.verts.size(),
                               g.tank.hull.indices.data(), g.tank.hull.indices.size());
    g.meshTurret = r->CreateMesh(g.tank.turret.verts.data(), g.tank.turret.verts.size(),
                                 g.tank.turret.indices.data(), g.tank.turret.indices.size());

    // 2x the arena so the straight-down camera never sees past the floor edge
    MeshData ground = MakeGroundPlane(ArenaHalf * 2.0f, 60.0f);
    g.meshGround = r->CreateMesh(ground.verts.data(), ground.verts.size(),
                                 ground.indices.data(), ground.indices.size());
    MeshData proj = MakeRocket();
    g.meshProj = r->CreateMesh(proj.verts.data(), proj.verts.size(),
                               proj.indices.data(), proj.indices.size());
    MeshData flash = MakeSphere(0.22f, 10, 6);
    g.meshFlash = r->CreateMesh(flash.verts.data(), flash.verts.size(),
                                flash.indices.data(), flash.indices.size());

    for (const Obstacle& o : kObstacles)
    {
        MeshData box = MakeBox(o.hx, o.height * 0.5f, o.hz, 0.5f);
        g.meshObstacles.push_back(r->CreateMesh(box.verts.data(), box.verts.size(),
                                                box.indices.data(), box.indices.size()));
    }
    // boundary walls: +Z, -Z (long in X), +X, -X (long in Z)
    for (int i = 0; i < 4; ++i)
    {
        MeshData wall = (i < 2) ? MakeBox(ArenaHalf + 0.6f, 0.6f, 0.6f, 0.5f)
                                : MakeBox(0.6f, 0.6f, ArenaHalf + 0.6f, 0.5f);
        g.meshWalls.push_back(r->CreateMesh(wall.verts.data(), wall.verts.size(),
                                            wall.indices.data(), wall.indices.size()));
    }

    ImageData palette = g.tank.palette;
    g.texPalette = r->CreateTexture(palette.rgba.data(), palette.width, palette.height);
    ImageData groundTex = MakeGroundTexture(256);
    g.texGround = r->CreateTexture(groundTex.rgba.data(), groundTex.width, groundTex.height);
    ImageData wallTex = MakeWallTexture(256);   // higher res: the normal map
                                                // needs mortar-line detail
    g.texWall = r->CreateTexture(wallTex.rgba.data(), wallTex.width, wallTex.height);
    ImageData white = MakeSolidTexture(255, 255, 255);
    g.texWhite = r->CreateTexture(white.rgba.data(), white.width, white.height);

    // NRA maps: brick walls get a strong emboss (mortar lines recessed,
    // rough; brick faces smoother), the ground a subtle grain, everything
    // else a flat semi-rough default.
    ImageData wallNRA = MakeNormalRoughFromTexture(wallTex, 2.6f, 0.62f, 0.95f);
    g.texWallNRA = r->CreateTexture(wallNRA.rgba.data(), wallNRA.width, wallNRA.height);
    ImageData groundNRA = MakeNormalRoughFromTexture(groundTex, 0.9f, 0.78f, 0.96f);
    g.texGroundNRA = r->CreateTexture(groundNRA.rgba.data(), groundNRA.width, groundNRA.height);
    ImageData flatNRA = MakeFlatNRA(0.55f);
    g.texFlatNRA = r->CreateTexture(flatNRA.rgba.data(), flatNRA.width, flatNRA.height);
    ImageData icons = MakeIconAtlas(32, UpgradeCount);
    g.texIconAtlas = r->CreateTexture(icons.rgba.data(), icons.width, icons.height);

    // necromancer + radar visuals: green skull (cranium + jaw), acid disc,
    // and the thin red radar annulus (scaled per ring at draw time)
    {
        MeshData cranium = MakeSphere(0.50f, 14, 10);
        g.meshSkull = r->CreateMesh(cranium.verts.data(), cranium.verts.size(),
                                    cranium.indices.data(), cranium.indices.size());
        MeshData jawM = MakeBox(0.28f, 0.12f, 0.22f, 1.0f);
        g.meshJaw = r->CreateMesh(jawM.verts.data(), jawM.verts.size(),
                                  jawM.indices.data(), jawM.indices.size());
        MeshData disc = MakeDisc(1.0f, 0.04f, 24);
        g.meshPuddle = r->CreateMesh(disc.verts.data(), disc.verts.size(),
                                     disc.indices.data(), disc.indices.size());
        MeshData ring = MakeRing(1.0f, 0.06f, 48);
        g.meshRing = r->CreateMesh(ring.verts.data(), ring.verts.size(),
                                   ring.indices.data(), ring.indices.size());
        MeshData spook = MakeGhostMesh();
        g.meshGhost = r->CreateMesh(spook.verts.data(), spook.verts.size(),
                                    spook.indices.data(), spook.indices.size());
        MeshData wedge = MakePieWedge();
        g.meshWedge = r->CreateMesh(wedge.verts.data(), wedge.verts.size(),
                                    wedge.indices.data(), wedge.indices.size());
    }

    // shield barrier lattice pieces (unit slat, scaled per width)
    {
        MeshData slat = MakeBox(1.0f, 0.055f, 0.05f, 1.0f);
        g.meshShieldSlat = r->CreateMesh(slat.verts.data(), slat.verts.size(),
                                         slat.indices.data(),
                                         slat.indices.size());
        MeshData post = MakeBox(0.07f, 0.72f, 0.07f, 1.0f);
        g.meshShieldPost = r->CreateMesh(post.verts.data(), post.verts.size(),
                                         post.indices.data(),
                                         post.indices.size());
    }

    // grenade prop (FRAG PACK): CC0 "Frag Grenade West" by Pichuliru
    {
        MeshData gm = LoadStaticGLB("assets/Grenade.glb");
        if (!gm.verts.empty())
        {
            float ext = 0.01f, minY = 1e9f, maxY = -1e9f;
            for (const Vertex& v : gm.verts)
            {
                ext = std::max({ ext, fabsf(v.px), fabsf(v.pz) });
                minY = std::min(minY, v.py);
                maxY = std::max(maxY, v.py);
            }
            float cy = (minY + maxY) * 0.5f;
            for (Vertex& v : gm.verts)
                v.py -= cy;
            ext = std::max(ext, (maxY - minY) * 0.5f);
            g.meshGrenade = r->CreateMesh(gm.verts.data(), gm.verts.size(),
                                          gm.indices.data(),
                                          gm.indices.size());
            g.grenadeScale = 0.30f / ext;
            Log("Grenade prop: %zu verts, scale %.3f", gm.verts.size(),
                g.grenadeScale);
        }
        else
            Log("Assets: no Grenade.glb -- rocket-mesh fallback");
    }

    // the user-authored rigged skull: 3-bone jaw, one OpenAndClose clip
    // looping forever (until the collision that ends the skull). The current
    // export has NO UVs, so its textures cannot map yet -- parts draw with a
    // bone tint until a re-export with UVs lands (texture hookup is ready).
    {
        g.skullModel = LoadSkinnedGLB("assets/Skull/Skull.glb");
        if (g.skullModel.valid)
        {
            float mx = 0.01f;
            float minY = 1e9f, maxY = -1e9f;
            bool hasUv = false;
            for (const SkinnedPart& part : g.skullModel.parts)
                for (const SkinnedVertex& v : part.verts)
                {
                    mx = std::max({ mx, fabsf(v.px), fabsf(v.py), fabsf(v.pz) });
                    minY = std::min(minY, v.py);
                    maxY = std::max(maxY, v.py);
                    hasUv |= v.u != 0.0f || v.v != 0.0f;
                }
            if (!hasUv)
            {
                // no UVs in the export: generate a spherical projection
                // (u = azimuth, v = 0 at the crown) and paint a matching
                // procedural bone texture -- blood scratches on top included
                float spanY = std::max(0.01f, maxY - minY);
                for (SkinnedPart& part : g.skullModel.parts)
                    for (SkinnedVertex& v : part.verts)
                    {
                        v.u = atan2f(v.px, v.pz) * (0.5f / XM_PI) + 0.5f;
                        v.v = 1.0f - (v.py - minY) / spanY;
                    }
            }
            for (const SkinnedPart& part : g.skullModel.parts)
                g.meshSkullParts.push_back(
                    r->CreateSkinnedMesh(part.verts.data(), part.verts.size(),
                                         part.indices.data(),
                                         part.indices.size()));
            g.skullScale = 0.62f / mx;   // normalize to gameplay size
            if (hasUv)
            {
                // the export carries UVs: bind the authored texture set
                ImageData col = LoadImageFile(
                    "assets/Skull/DefaultMaterial_Base_color.png");
                if (col.width > 0)
                    g.texSkull = r->CreateTexture(col.rgba.data(),
                                                  col.width, col.height);
                ImageData nrm = LoadImageFile(
                    "assets/Skull/DefaultMaterial_Normal_OpenGL.png");
                ImageData rgh = LoadImageFile(
                    "assets/Skull/DefaultMaterial_Specular_roughness.png");
                ImageData nra = MakeNraFromMaps(nrm, rgh);
                if (nra.width > 0)
                    g.texSkullNRA = r->CreateTexture(nra.rgba.data(),
                                                     nra.width, nra.height);
                Log("Skull textures bound: color %dx%d, nra %dx%d",
                    col.width, col.height, nra.width, nra.height);
            }
            else
            {
                // painted PNGs cannot map without authored UVs; use the
                // generated projection + procedural bone/blood texture (the
                // PNG set takes over automatically once UVs are exported)
                ImageData bone = MakeSkullTexture(512);
                g.texSkull = r->CreateTexture(bone.rgba.data(),
                                              bone.width, bone.height);
                ImageData nra = MakeNormalRoughFromTexture(bone, 1.8f,
                                                           0.55f, 0.9f);
                g.texSkullNRA = r->CreateTexture(nra.rgba.data(),
                                                 nra.width, nra.height);
                Log("Skull.glb has no UVs: spherical projection + procedural "
                    "bone/blood texture (authored PNGs need a UV export)");
            }
            int clip = g.skullModel.FindClip("OpenAndClose");
            if (clip < 0)
                clip = g.skullModel.clips.empty() ? -1 : 0;
            for (int i = 0; i < MaxSkulls; ++i)
            {
                Animator& an = g.skullAnim[i];
                an.model = &g.skullModel;
                if (clip >= 0)
                {
                    an.PlayLayer(0, clip, true, 1.0f);
                    an.layers[0].time = float(i) * 0.37f;   // staggered chomp
                }
            }
            Log("Skull rig ready: %zu parts, %zu joints, scale %.3f",
                g.skullModel.parts.size(), g.skullModel.joints.size(),
                g.skullScale);
        }
        else
            Log("Assets: no Skull/Skull.glb -- procedural skull fallback");
    }

    // The soldier rig is GAMEPLAY now (summons), not just the --rigtest demo:
    // always load it, resolve the clips the summon needs BY NAME (loud on
    // miss), and pre-configure one Animator per soldier slot.
    {
        g.rigModel = LoadSkinnedGLB("assets/soldier.glb");   // SWAT operator
        g.rigScale = 1.0f;
        if (!g.rigModel.valid)
            g.rigModel = LoadSkinnedGLB("assets/test_rig.glb");
        if (g.rigModel.valid)
        {
            // character rigs carry a whole arsenal of hand-attached weapons;
            // show exactly one (guess which)
            static const char* kWeapons[] = {
                "Revolver", "Sniper", "Pistol", "SMG", "GrenadeLauncher",
                "ShortCannon", "Shotgun", "AK", "Shovel", "Knife",
                "RocketLauncher",
            };
            for (const SkinnedPart& part : g.rigModel.parts)
            {
                g.meshRigParts.push_back(
                    r->CreateSkinnedMesh(part.verts.data(), part.verts.size(),
                                         part.indices.data(),
                                         part.indices.size()));
                g.rigPartColors.push_back(part.baseColor);
                bool isWeapon = false;
                for (const char* w : kWeapons)
                    if (part.name.find(w) != std::string::npos)
                        isWeapon = true;
                g.rigPartVisible.push_back(
                    !isWeapon || part.name == "RocketLauncher");
            }
            if (g.rigModel.texture.width > 0)
                g.texRig = r->CreateTexture(g.rigModel.texture.rgba.data(),
                                            g.rigModel.texture.width,
                                            g.rigModel.texture.height);
            // measure the bind-pose height THROUGH rootTransform: FBX
            // conversions park a scale-100/axis-flip there, and the palette
            // applies it -- raw vertex extents alone would double the scale
            float rigMaxY = 0.01f;
            XMMATRIX rigRoot = XMLoadFloat4x4(&g.rigModel.rootTransform);
            for (const SkinnedPart& part : g.rigModel.parts)
                for (const SkinnedVertex& v : part.verts)
                {
                    XMVECTOR p = XMVector3TransformCoord(
                        XMVectorSet(v.px, v.py, v.pz, 1), rigRoot);
                    rigMaxY = std::max(rigMaxY, XMVectorGetY(p));
                }
            g.rigScale = 2.15f / rigMaxY;   // beefy: reads next to a tank
            Log("Rig height (root-transformed): %.3f -> scale %.3f",
                rigMaxY, g.rigScale);

            // the launcher prop: geometry lifted from the old toon soldier
            // (also CC0), rebased into hand-local space via the inverse bind
            // of the joint it was skinned to -- so it can ride ANY rig
            {
                SkinnedModel toon = LoadSkinnedGLB("assets/soldier_toon.glb");
                if (toon.valid)
                    for (const SkinnedPart& part : toon.parts)
                    {
                        if (part.name.find("RocketLauncher")
                                == std::string::npos
                            || part.verts.empty())
                            continue;
                        int hand = part.verts[0].joints[0];
                        XMMATRIX inv = XMLoadFloat4x4(
                            &toon.joints[hand].inverseBind);
                        MeshData md;
                        md.verts.reserve(part.verts.size());
                        for (const SkinnedVertex& sv : part.verts)
                        {
                            Vertex v{};
                            XMVECTOR p = XMVector3TransformCoord(
                                XMVectorSet(sv.px, sv.py, sv.pz, 1), inv);
                            XMVECTOR nr = XMVector3Normalize(
                                XMVector3TransformNormal(
                                    XMVectorSet(sv.nx, sv.ny, sv.nz, 0),
                                    inv));
                            v.px = XMVectorGetX(p);
                            v.py = XMVectorGetY(p);
                            v.pz = XMVectorGetZ(p);
                            v.nx = XMVectorGetX(nr);
                            v.ny = XMVectorGetY(nr);
                            v.nz = XMVectorGetZ(nr);
                            v.u = sv.u; v.v = sv.v;
                            md.verts.push_back(v);
                        }
                        // canonicalize: center at the origin, longest
                        // axis rotated onto +Z -- placement becomes plain
                        // yaw/pitch/offsets instead of per-export guesswork
                        XMFLOAT3 mn{ 1e9f, 1e9f, 1e9f };
                        XMFLOAT3 mx{ -1e9f, -1e9f, -1e9f };
                        for (const Vertex& v : md.verts)
                        {
                            mn.x = std::min(mn.x, v.px);
                            mn.y = std::min(mn.y, v.py);
                            mn.z = std::min(mn.z, v.pz);
                            mx.x = std::max(mx.x, v.px);
                            mx.y = std::max(mx.y, v.py);
                            mx.z = std::max(mx.z, v.pz);
                        }
                        XMFLOAT3 c{ (mn.x + mx.x) * 0.5f,
                                    (mn.y + mx.y) * 0.5f,
                                    (mn.z + mx.z) * 0.5f };
                        float ex = mx.x - mn.x, ey = mx.y - mn.y,
                              ez = mx.z - mn.z;
                        for (Vertex& v : md.verts)
                        {
                            float px = v.px - c.x, py = v.py - c.y,
                                  pz = v.pz - c.z;
                            float nx = v.nx, ny = v.ny, nz = v.nz;
                            if (ex >= ey && ex >= ez)
                            {   // RotY(90): +X -> -Z (det +1, no mirror)
                                v.px = pz;  v.py = py;  v.pz = -px;
                                v.nx = nz;  v.ny = ny;  v.nz = -nx;
                            }
                            else if (ey >= ex && ey >= ez)
                            {   // RotX(90): +Y -> -Z
                                v.px = px;  v.py = pz;  v.pz = -py;
                                v.nx = nx;  v.ny = nz;  v.nz = -ny;
                            }
                            else
                            {
                                v.px = px;  v.py = py;  v.pz = pz;
                            }
                        }
                        float rawLen = std::max({ ex, ey, ez, 0.001f });
                        g.launcherScale = 1.15f / rawLen;   // ~1.15 u tube
                        md.indices = part.indices;
                        ComputeTangents(md);
                        g.meshLauncher = r->CreateMesh(md.verts.data(),
                                                       md.verts.size(),
                                                       md.indices.data(),
                                                       md.indices.size());
                        break;
                    }
                g.rigChest = FindJoint(g.rigModel, "Chest");
                if (g.rigChest < 0)
                    g.rigChest = FindJoint(g.rigModel, "Torso");
                if (g.rigChest >= 0)
                {
                    XMMATRIX ib = XMLoadFloat4x4(
                        &g.rigModel.joints[g.rigChest].inverseBind);
                    XMStoreFloat4x4(&g.rigChestBind,
                                    XMMatrixInverse(nullptr, ib));
                }
                g.rigArmR[0] = FindJoint(g.rigModel, "UpperArm.R");
                g.rigArmR[1] = FindJoint(g.rigModel, "LowerArm.R");
                g.rigArmR[2] = FindJoint(g.rigModel, "Wrist.R");
                Log("Launcher prop: mesh %d, chest %d, arm %d/%d/%d",
                    g.meshLauncher, g.rigChest, g.rigArmR[0], g.rigArmR[1],
                    g.rigArmR[2]);
            }
            for (size_t j = 0; j < g.rigModel.jointNames.size(); ++j)
                Log("Rig joint %zu: %s (parent %d)", j,
                    g.rigModel.jointNames[j].c_str(),
                    g.rigModel.joints[j].parent);

            // All rig references resolve BY NAME, once, right here -- and
            // every miss logs. Raw indices are per-export and a re-exported
            // GLB would silently bend the wrong bone.
            auto clip = [&](const char* n)
            {
                int c = g.rigModel.FindClip(n);
                if (c < 0) Log("rig: clip '%s' not in model", n);
                return c;
            };
            auto joint = [&](const char* n)
            {
                int j = FindJoint(g.rigModel, n);
                if (j < 0) Log("rig: joint '%s' not in skeleton", n);
                return j;
            };

            // clips the soldier summon runs on
            g.soldierClips.idle = clip("Idle");
            g.soldierClips.run = clip("Run");
            g.soldierClips.duck = clip("Duck");
            g.soldierClips.shoot = g.rigModel.FindClip("Idle_Shoot");
            if (g.soldierClips.shoot < 0) g.soldierClips.shoot = clip("Shoot");
            g.soldierClips.death = clip("Death");
            if (g.soldierClips.duck < 0)   // SWAT set: cover = gun-ready idle
                g.soldierClips.duck = g.rigModel.FindClip("Idle_Gun");

            // shared aim chain (torso twist toward the target)
            int aimChain[2] = { joint("Abdomen"), joint("Torso") };
            for (Animator& an : g.soldierAnim)
            {
                an.model = &g.rigModel;
                an.aim.chainCount = 0;
                for (int j : aimChain)
                    if (j >= 0 && an.aim.chainCount < 4)
                        an.aim.chain[an.aim.chainCount++] = j;
                an.aim.forward = XMFLOAT3(0, 0, 1);
                an.aim.maxAngle = 1.2f;
            }

            if (g.opt.rigTest)
            {
                // full animation system demo: run legs + shoot torso (masked
                // layer) + aim constraint at the player tank + left-hand
                // two-bone IK onto an orbiting marker, weight ramping in/out
                Animator& an = g.rigAnimator;
                an.model = &g.rigModel;
                if (g.soldierClips.run >= 0)
                    an.PlayLayer(0, g.soldierClips.run, true, 1.0f);
                if (g.soldierClips.shoot >= 0)
                {
                    an.PlayLayer(1, g.soldierClips.shoot, true, 1.0f);
                    an.layers[1].mask = MaskSubtree(g.rigModel, "Torso");
                }
                an.aim.active = true;
                an.aim.chainCount = 0;
                for (int j : aimChain)
                    if (j >= 0 && an.aim.chainCount < 4)
                        an.aim.chain[an.aim.chainCount++] = j;
                an.aim.forward = XMFLOAT3(0, 0, 1);
                an.aim.maxAngle = 1.2f;
                int ua = joint("UpperArm.L");
                int la = joint("LowerArm.L");
                int ha = FindJoint(g.rigModel, "Hand.L");
                if (ha < 0)
                    ha = FindJoint(g.rigModel, "Wrist.L");
                if (ha < 0) ha = joint("Index1.L");   // log only if both miss
                if (ua >= 0 && la >= 0 && ha >= 0)
                {
                    an.ik[0].active = true;
                    an.ik[0].a = ua; an.ik[0].b = la; an.ik[0].c = ha;
                }
                else
                    Log("rigtest: IK chain incomplete, hand IK disabled");
            }
        }
        else
            Log("Assets: no soldier rig -- summons will be invisible");
    }
    return g.meshHull >= 0 && g.meshTurret >= 0 && g.texPalette >= 0;
}

void UpdateTitle()
{
    std::wstring title = L"Tankaq - ";
    title += (g.currentBackend == Backend::D3D12) ? L"D3D12" : L"D3D11";
    SetWindowTextW(g.hwnd, title.c_str());
}

IRenderer* CreateBackend(Backend b, std::string& err)
{
    IRenderer* r = (b == Backend::D3D12) ? CreateRendererD3D12() : CreateRendererD3D11();
    if (!r->Init(g.hwnd, g.width, g.height, err))
    {
        delete r;
        return nullptr;
    }
    return r;
}

// Live backend switch: only one swapchain may own the HWND, so the old
// renderer is destroyed first; on failure the previous backend is restored.
void SwitchRenderer(Backend want)
{
    if (want == g.currentBackend || !g.renderer)
        return;
    Backend old = g.currentBackend;
    delete g.renderer;
    g.renderer = nullptr;

    std::string err;
    IRenderer* r = CreateBackend(want, err);
    if (r)
    {
        g.currentBackend = want;
        g.statusLine.clear();
    }
    else
    {
        Log("Renderer switch to %s failed: %s",
            want == Backend::D3D12 ? "D3D12" : "D3D11", err.c_str());
        g.statusLine = "RENDERER SWITCH FAILED: " + err;
        r = CreateBackend(old, err);
        if (!r)
        {
            MessageBoxA(g.hwnd, ("Renderer restore failed: " + err).c_str(),
                        "Tankaq", MB_ICONERROR);
            g.wantQuit = true;
            return;
        }
    }
    g.renderer = r;
    CreateAssets();
    UpdateTitle();
    Log("Renderer now: %s", g.currentBackend == Backend::D3D12 ? "D3D12" : "D3D11");
}

void ApplyDisplayMode()
{
    if (g.fullscreen)
    {
        GetWindowRect(g.hwnd, &g.windowedRect);
        HMONITOR mon = MonitorFromWindow(g.hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfoW(mon, &mi);
        SetWindowLongPtrW(g.hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(g.hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }
    else
    {
        SetWindowLongPtrW(g.hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        RECT r = g.windowedRect;
        if (r.right - r.left < 200)
            r = { 80, 80, 80 + 1296, 80 + 759 };
        SetWindowPos(g.hwnd, HWND_TOP, r.left, r.top,
                     r.right - r.left, r.bottom - r.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }
}

void ApplyResolution()
{
    if (g.fullscreen)
        return;   // takes effect when switching back to windowed
    RECT rect{ 0, 0, kResolutions[g.resIndex].w, kResolutions[g.resIndex].h };
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    SetWindowPos(g.hwnd, nullptr, 0, 0, rect.right - rect.left,
                 rect.bottom - rect.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_FRAMECHANGED);
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int)
{
    // Run relative to the executable so assets/shaders resolve.
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (wchar_t* slash = wcsrchr(exePath, L'\\'))
        *slash = 0;
    SetCurrentDirectoryW(exePath);

    char cmdUtf8[2048]{};
    WideCharToMultiByte(CP_UTF8, 0, GetCommandLineW(), -1, cmdUtf8, sizeof(cmdUtf8) - 1,
                        nullptr, nullptr);
    g.opt = ParseOptions(cmdUtf8);
    if (g.opt.winW > 0 && g.opt.winH > 0)
    {
        g.width = g.opt.winW;
        g.height = g.opt.winH;
    }
    Log("Tankaq starting. cmdline: %s", cmdUtf8);
    // the pool is data the whole game trusts: refuse to run if inconsistent
    if (const char* poolErr = ValidateUpgradePool())
    {
        Log("FATAL: upgrade pool invalid: %s", poolErr);
        MessageBoxA(nullptr, poolErr, "Tankaq: upgrade pool invalid", MB_ICONERROR);
        return 1;
    }
    if (g.opt.classTest)
        return RunClassTest();   // headless sim test, exit code = failures

    g.gpu = DetectGpu(g.opt.renderer);
    if (!g.gpu.adapter)
    {
        MessageBoxW(nullptr, L"No GPU adapter found.", L"Tankaq", MB_ICONERROR);
        return 1;
    }

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"TankaqWindow";
    RegisterClassW(&wc);

    RECT rect{ 0, 0, g.width, g.height };
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    g.hwnd = CreateWindowW(L"TankaqWindow", L"Tankaq", WS_OVERLAPPEDWINDOW,
                           g.opt.winX, g.opt.winY,
                           rect.right - rect.left, rect.bottom - rect.top,
                           nullptr, nullptr, hInstance, nullptr);
    if (!g.hwnd)
        return 1;
    ShowWindow(g.hwnd, SW_SHOW);

    // Create the selected backend, fall back to D3D11 if D3D12 init fails.
    std::string err;
    Backend backend = g.gpu.chosen;
    g.renderer = CreateBackend(backend, err);
    if (!g.renderer && backend == Backend::D3D12)
    {
        Log("D3D12 init failed (%s), falling back to D3D11", err.c_str());
        backend = Backend::D3D11;
        g.renderer = CreateBackend(backend, err);
    }
    if (!g.renderer)
    {
        MessageBoxA(g.hwnd, ("Renderer init failed: " + err).c_str(), "Tankaq",
                    MB_ICONERROR);
        return 1;
    }
    g.currentBackend = backend;
    UpdateTitle();

    g.vsyncOn = g.opt.vsync;
    for (int i = 0; i < int(std::size(kResolutions)); ++i)
        if (kResolutions[i].w == g.width && kResolutions[i].h == g.height)
            g.resIndex = i;
    if (g.opt.fullscreen)
    {
        g.fullscreen = true;
        ApplyDisplayMode();
    }

    if (!CreateAssets())
    {
        MessageBoxW(g.hwnd, L"Failed to load assets (assets/tank/tank_baked.glb).",
                    L"Tankaq", MB_ICONERROR);
        return 1;
    }

    if (!snd::Init())
        Log("Audio init failed - continuing silent");

    // Apply CLI render-setting overrides.
    if (g.opt.gi >= 0) g.post.giEnabled = g.opt.gi != 0;
    if (g.opt.ao >= 0) g.post.aoEnabled = g.opt.ao != 0;
    if (g.opt.giRays > 0) g.post.giRays = std::clamp(g.opt.giRays, 1, 16);
    if (g.opt.temporal > 0) g.post.temporalSamples = std::clamp(g.opt.temporal, 2, 16);
    if (g.opt.giHalf >= 0) g.post.giHalfRes = g.opt.giHalf != 0;
    if (g.opt.shadows >= 0) g.post.shadowsEnabled = g.opt.shadows != 0;
    if (g.opt.shadowRes == 1024 || g.opt.shadowRes == 2048 || g.opt.shadowRes == 4096)
        g.post.shadowMapSize = g.opt.shadowRes;
    if (g.opt.shadowFilter >= 0) g.post.shadowFilter = std::clamp(g.opt.shadowFilter, 0, 2);
    if (g.opt.aa >= 0) g.post.aaEnabled = g.opt.aa != 0;
    if (g.opt.lagComp >= 0) g.lagComp = g.opt.lagComp != 0;

    bool steamOk = g.net.InitSteam();
    if (!steamOk)
        g.statusLine = "steam not available - solo only";

    // Quick-match continuations. Both fire from inside net.Poll(); ignore
    // stale results if the user cancelled out of the search meanwhile.
    g.net.onMatchFound = [](uint64_t hostId)
    {
        if (!g.searching)
            return;
        g.searching = false;
        // Join through StartJoin so ALL the client session bookkeeping runs
        // (online flag, host flag, sim reset, watchdog). Skipping it was the
        // ready-up bug: with g.online still false, ToggleReady/purchases took
        // the offline branch and were never sent to the host, so the next
        // snapshot instantly reverted the optimistic state. StartJoin also
        // handles the own-SteamID -> 127.0.0.1 loopback fallback.
        StartJoin(std::to_string(hostId));
        if (g.screen == Screen::MainMenu)   // join failed: surface the error
            g.statusLine = "MATCH JOIN FAILED - try again";
    };
    g.net.onNoMatch = []()
    {
        if (!g.searching)
            return;
        g.searching = false;
        // nobody out there: host a gathering queue of the chosen size. The
        // ready-up lobby stays hidden until the queue fills.
        StartHost();
        if (g.screen == Screen::InGame)
        {
            g.net.SetJoinCap(g.searchNeed);
            g.game.StartGathering(g.searchNeed);
            g.net.CreatePublicLobby(g.searchNeed);
            g.statusLine.clear();
        }
    };

    // Command-line shortcuts for testing.
    if (g.opt.solo)
        StartSolo();
    else if (g.opt.host)
    {
        (steamOk ? StartHost() : StartSolo());
        if (g.online)
            g.net.CreatePublicLobby(0);   // hosting counts as matchmaking
    }
    else if (!g.opt.join.empty() && steamOk)
        StartJoin(g.opt.join);
    else if (g.opt.quickMatch && steamOk)
        StartFindMatch(g.opt.quickMatchNeed);

    net::Net::Events ev;
    ev.onPlayerJoined = [](int pid, const char* name)
    {
        g.game.SpawnPlayer(pid);
        SetPlayerName(pid, name);
        // late-join sync: ship every player's owned-upgrade list so the new
        // client can derive all stats locally
        for (int i = 0; i < MaxPlayers; ++i)
        {
            const PlayerState& pl = g.game.players[i];
            if (pl.active && !pl.owned.empty())
                g.net.SendOwnedSyncTo(pid, i, pl.owned.data(), pl.owned.size());
        }
        if (g.game.phase == PhaseLobby || g.game.phase == PhaseGathering)
        {
            float x, z, yaw;
            LobbySpot(pid, x, z, yaw);
            g.game.players[pid].x = x;
            g.game.players[pid].z = z;
            g.game.players[pid].hullYaw = g.game.players[pid].turretYaw = yaw;
        }
    };
    ev.onPlayerLeft = [](int pid)
    {
        g.game.RemovePlayer(pid);
        g.inputQueue[pid].clear();
        g.inputs[pid] = InputCmd{};
    };
    ev.onReady = [](int pid, bool ready)
    {
        Log("Host: ready message from player %d -> %d (phase=%d)",
            pid, ready ? 1 : 0, int(g.game.phase));
        if (g.game.phase == PhaseLobby)
            g.game.players[pid].ready = ready ? 1 : 0;
    };
    ev.onInput = [](int pid, const net::MsgInput& m)
    {
        // queue in order; the tick loop consumes exactly one per tick
        auto& q = g.inputQueue[pid];
        uint32_t lastSeen = q.empty() ? g.inputSeqs[pid] : q.back().seq;
        if (m.seq <= lastSeen)
            return;                     // stale or out-of-order packet
        q.push_back(m);
        while (q.size() > 8)            // pathological backlog: fast-forward
        {
            g.inputSeqs[pid] = q.front().seq;
            q.pop_front();
        }
    };
    ev.onPurchase = [](int pid, int slot)
    {
        if (HostPurchase(pid, slot))
            Log("Player %d bought offer slot %d (owned %zu upgrades)", pid, slot,
                g.game.players[pid].owned.size());
    };
    ev.onUpgrade = [](int pid, uint8_t type)
    {
        if (g.isHost)
            return;
        g.game.players[pid].owned.push_back(type);
        g.game.RecalcStats(pid);   // additions first, multiplications last
        Log("Upgrade event: player %d bought type %d (owned %zu, speed %.2f)",
            pid, int(type), g.game.players[pid].owned.size(),
            g.game.players[pid].stats[int(Stat::MoveSpeed)]);
    };
    ev.onOwnedReset = []()
    {
        if (g.isHost)
            return;
        for (int i = 0; i < MaxPlayers; ++i)
        {
            g.game.players[i].owned.clear();
            g.game.RecalcStats(i);
        }
        Log("OwnedReset event: all upgrade lists cleared");
    };
    ev.onOwnedSync = [](int pid, const uint8_t* types, int count)
    {
        if (g.isHost)
            return;
        g.game.players[pid].owned.assign(types, types + count);
        g.game.RecalcStats(pid);
        Log("OwnedSync event: player %d has %d upgrades (speed %.2f)",
            pid, count, g.game.players[pid].stats[int(Stat::MoveSpeed)]);
    };
    ev.onWelcome = [](int myId)
    {
        g.myId = myId;
        g.screen = Screen::InGame;
        g.sessionActive = true;
        g.statusLine.clear();
        Log("Joined as player %d", myId);
    };
    ev.onSnapshot = [](const net::MsgSnapshot& s)
    {
        if (InSession() || g.screen == Screen::Connecting)
            ApplySnapshot(s);
    };
    ev.onDisconnected = [](const std::string& why) { LeaveToMenu("DISCONNECTED: " + why); };

    LARGE_INTEGER freq, prev, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&prev);
    double accumulator = 0;
    double fpsTimer = 0;
    int fpsFrames = 0;

    MSG msg{};
    while (!g.wantQuit)
    {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
                g.wantQuit = true;
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (g.wantQuit)
            break;

        QueryPerformanceCounter(&now);
        double dt = double(now.QuadPart - prev.QuadPart) / double(freq.QuadPart);
        prev = now;
        dt = std::min(dt, 0.25);
        g.time += dt;
        g.frameDt = float(dt);
        accumulator += dt;
        fpsTimer += dt;
        ++fpsFrames;
        if (fpsTimer >= 0.5)
        {
            g.fps = float(fpsFrames / fpsTimer);
            fpsTimer = 0;
            fpsFrames = 0;
        }

        LARGE_INTEGER dbgA, dbgB, dbgC, dbgD;
        QueryPerformanceCounter(&dbgA);
        g.net.Poll(ev);
        QueryPerformanceCounter(&dbgB);

        // keep the public-lobby advert fresh (push on change only)
        if (g.isHost && g.online && g.net.hasPublicLobby())
        {
            int n = 0;
            for (int i = 0; i < MaxPlayers; ++i)
                if (g.game.players[i].active)
                    ++n;
            if (n != g.lastAdvertPlayers || int(g.game.phase) != g.lastAdvertPhase)
            {
                g.net.UpdateLobbyAdvert(n, int(g.game.phase));
                g.lastAdvertPlayers = n;
                g.lastAdvertPhase = int(g.game.phase);
            }
        }

        // joining watchdog
        if (g.screen == Screen::Connecting && g.time - g.connectStart > 30.0)
            LeaveToMenu("JOIN TIMED OUT - check the code and that the host is in game");

        // fixed timestep simulation -- keeps running under the pause/settings
        // overlay (multiplayer can't stop); an overlaid player just goes idle
        while (accumulator >= TickDt)
        {
            accumulator -= TickDt;
            if (!InSession())
                continue;
            InputCmd local;
            if (g.screen == Screen::InGame)
                local = BuildLocalInput();
            else
                local.turretYaw = g.aimYaw;   // hold last aim; no move, no fire
            if (g.isHost)
            {
                g.inputs[g.myId] = local;
                // Jitter buffer: consume exactly one queued input per client
                // per tick, in order, acking only what was actually simulated
                // -- the client's replay then matches the host's integration.
                // Starved (packet late): reuse the previous input and leave
                // the ack alone so the client keeps replaying that tick.
                for (int pid = 1; pid < MaxLobbyPlayers; ++pid)
                {
                    auto& q = g.inputQueue[pid];
                    while (q.size() > 3)   // creeping backlog: fast-forward
                    {
                        g.inputSeqs[pid] = q.front().seq;
                        q.pop_front();
                    }
                    // inert inputs (no movement, no fire) simulate to nothing,
                    // so acking them without simulation is drift-free -- drain
                    // them whenever something newer waits behind. This removes
                    // the FIFO buffer's movement-onset delay: the first moving
                    // input after standing still is consumed THIS tick.
                    while (q.size() > 1 && q.front().buttons == 0
                           && q.front().moveX == 0 && q.front().moveZ == 0)
                    {
                        g.inputSeqs[pid] = q.front().seq;
                        q.pop_front();
                    }
                    if (q.empty())
                        continue;
                    const net::MsgInput& m = q.front();
                    g.inputs[pid].buttons = m.buttons;
                    g.inputs[pid].moveX = m.moveX;
                    g.inputs[pid].moveZ = m.moveZ;
                    g.inputs[pid].turretYaw = m.turretYaw;
                    g.inputSeqs[pid] = m.seq;
                    q.pop_front();
                    // the SHIELD face tracks the freshest aim in the queue,
                    // not the jitter-delayed one being simulated
                    g.inputs[pid].aimYawFresh =
                        q.empty() ? m.turretYaw : q.back().turretYaw;
                }
                g.game.lagCompEnabled = g.lagComp;
                for (int pid = 1; pid < MaxLobbyPlayers; ++pid)
                    g.game.players[pid].lagOneWayMs =
                        std::max(0.0f, g.net.avgOneWayMs(pid));
                g.prevTick = g.game;
                g.game.Tick(g.inputs);
                if (g.online && g.game.tick % SnapshotEveryTicks == 0)
                {
                    net::MsgSnapshot snap = BuildSnapshot();
                    g.net.BroadcastSnapshot(snap);
                }
            }
            else
            {
                net::MsgInput m;
                m.buttons = local.buttons;
                m.moveX = local.moveX;
                m.moveZ = local.moveZ;
                m.turretYaw = local.turretYaw;
                m.seq = ++g.inputSeq;
                g.net.SendInputToHost(m);

                // client-side prediction: move our tank immediately with the
                // same integration the host runs; snapshots reconcile later.
                // Only during play -- in the lobby the host parks everyone.
                PlayerState& me = g.game.players[g.myId];
                bool playing = g.game.phase == PhasePlaying
                            || g.game.phase == PhaseOvertime;
                if (!playing || !me.active || me.health <= 0)
                {
                    // not predicting (lobby/dead): the render pair follows
                    // the snapshot state directly -- reconciliation no longer
                    // maintains it (see ApplySnapshot)
                    g.pendingInputs.clear();
                    g.predPrev = g.predCurr = me;
                    g.predCooldown = 0;
                    for (auto& pv : g.provRockets)
                        pv.sim.active = false;
                }
                else
                {
                    g.pendingInputs.push_back({ g.inputSeq, local });
                    if (g.pendingInputs.size() > 256)   // ~4 s of unacked input
                        g.pendingInputs.erase(g.pendingInputs.begin());
                    g.predPrev = me;
                    g.game.AdvanceMovement(g.myId, local);
                    g.predCurr = me;

                    // predicted firing: the rocket exists locally the moment
                    // the input is pressed; the server's rocket adopts it on
                    // arrival (see ApplySnapshot matching)
                    g.predCooldown = std::max(0.0f, g.predCooldown - TickDt);
                    if ((local.buttons & BtnFire) && g.predCooldown <= 0.0f
                        && me.possessTimer <= 0.0f)
                    {
                        for (auto& pv : g.provRockets)
                        {
                            if (pv.sim.active)
                                continue;
                            XMFLOAT3 mz = g.game.MuzzleWorld(me);
                            pv.sim = Projectile{};
                            pv.sim.active = true;
                            pv.matchedSlot = -1;
                            pv.sim.x = mz.x; pv.sim.y = mz.y; pv.sim.z = mz.z;
                            pv.sim.yaw = local.turretYaw;
                            pv.sim.speed = me.stats[int(Stat::ProjSpeed)];
                            pv.sim.bounces = int(me.stats[int(Stat::Bounces)] + 0.5f);
                            pv.sim.bounceSpd = me.stats[int(Stat::BounceSpeed)];
                            pv.sim.bounceDmg = me.stats[int(Stat::BounceDamage)];
                            if (HasClass(me, ClassRadar))
                            {
                                // rings render on the predicted rocket too
                                pv.sim.radarRange = me.stats[int(Stat::RadarRange)];
                                pv.sim.radarRings = std::clamp(
                                    int(me.stats[int(Stat::RadarRings)] + 0.5f),
                                    0, MaxRadarExtra);
                            }
                            pv.sim.life = ProjectileLife;
                            pv.born = g.time;
                            pv.spawn = mz;
                            g.predCooldown = me.stats[int(Stat::ReloadTime)];
                            me.muzzleFlash = 0.12f;
                            snd::Play(snd::Sfx::Shoot, 0.6f, SndJitter(0.07f));
                            break;
                        }
                    }
                }
                // advance provisional rockets (pure local physics, full
                // flight). Unmatched past the window = the server rejected
                // the fire: expire quietly. If OUR physics end it first
                // (rare drift), unveil the twin so the tail is server-drawn.
                for (auto& pv : g.provRockets)
                {
                    if (!pv.sim.active)
                        continue;
                    bool aliveLocal = StepProjectile(pv.sim, TickDt);
                    bool expired = pv.matchedSlot < 0 && g.time - pv.born > 0.7;
                    if (!aliveLocal || expired)
                    {
                        if (pv.matchedSlot >= 0
                            && g.projVeiledBy[pv.matchedSlot] >= 0)
                            g.projVeiledBy[pv.matchedSlot] = -1;
                        pv.matchedSlot = -1;
                        pv.sim.active = false;
                    }
                }
            }
        }
        QueryPerformanceCounter(&dbgC);
        g.tickAlpha = float(accumulator / TickDt);

        // glide the reconciliation-error render offset to zero (~80 ms)
        {
            float k = 1.0f - expf(-g.frameDt * 12.0f);
            g.predErrX -= g.predErrX * k;
            g.predErrZ -= g.predErrZ * k;
            g.predErrYaw -= g.predErrYaw * k;
        }

        // Camera: smooth-follow of the tank's *interpolated* render position.
        // The target is continuous (tick-interpolated), so smoothing can't
        // fight a stair-stepping signal anymore. Twitch-proofing:
        //  - exponential smoothing with dt (frame-rate independent, can never
        //    overshoot or oscillate),
        //  - a single smoothed focus point drives BOTH the camera position and
        //    the look-at target, so they always agree (no relative twist),
        //  - snap to the target when within a millimeter to kill float noise.
        XMFLOAT3 target{ 0, 0, 0 };
        bool lobbyCam = InSession() && (g.game.phase == PhaseLobby
                                        || g.game.phase == PhaseGathering);
        if (lobbyCam)
            target = XMFLOAT3(0, 0, -8.0f);   // lineup center
        else if (InSession() && g.game.players[g.myId].active)
        {
            float rx, rz, rh, rt;
            GetRenderPlayer(g.myId, rx, rz, rh, rt);
            target = XMFLOAT3(rx, 0, rz);
        }
        if (!g.camFocusValid || !InSession())
        {
            g.camFocus = target;
            g.camFocusValid = true;
        }
        else
        {
            float k = 1.0f - expf(-float(dt) * 9.0f);   // ~110 ms settle
            float ex = target.x - g.camFocus.x;
            float ez = target.z - g.camFocus.z;
            if (ex * ex + ez * ez < 1e-6f)
                g.camFocus = target;                    // epsilon snap: no jitter at rest
            else
            {
                g.camFocus.x += ex * k;
                g.camFocus.z += ez * k;
            }
        }

        // Movement lean: strafing rotates the camera slightly around Y,
        // forward/back pitches it around its local X. Twitch-proofing: the
        // targets come only from held input (zero at rest), smoothing is
        // dt-exponential (can't overshoot or oscillate), and values snap to
        // the target within an epsilon so float noise never wobbles the view.
        {
            float tYaw = 0, tPitch = 0;
            if (g.screen == Screen::InGame)
            {
                const InputCmd& li = g.isHost ? g.inputs[g.myId]
                                              : (g.pendingInputs.empty()
                                                     ? InputCmd{}
                                                     : g.pendingInputs.back().cmd);
                float screenX = -li.moveX;    // world -X = screen right
                float screenZ = li.moveZ;
                tYaw = screenX * 0.028f;      // ~1.6 deg lean into strafe
                tPitch = -screenZ * 0.022f;   // ~1.3 deg, reversed per feel
            }
            float kl = 1.0f - expf(-float(dt) * 5.0f);
            float dy = tYaw - g.camYawLean;
            float dp = tPitch - g.camPitchLean;
            g.camYawLean = (fabsf(dy) < 0.0004f) ? tYaw : g.camYawLean + dy * kl;
            g.camPitchLean = (fabsf(dp) < 0.0004f) ? tPitch : g.camPitchLean + dp * kl;
        }
        XMMATRIX view;
        if (g.opt.rigTest)
        {
            // rig inspection: low 3/4 orbit around the posed soldier
            float oa = float(g.time) * 0.35f;
            g.camPos = XMFLOAT3(g.rigPos.x + sinf(oa) * 4.6f, 2.4f,
                                g.rigPos.z + cosf(oa) * 4.6f);
            view = XMMatrixLookAtRH(
                XMVectorSet(g.camPos.x, g.camPos.y, g.camPos.z, 1),
                XMVectorSet(g.rigPos.x, 1.15f, g.rigPos.z, 1),
                XMVectorSet(0, 1, 0, 0));
        }
        else if (lobbyCam)
        {
            g.camPos = XMFLOAT3(0, 8.5f, -21.0f);
            view = XMMatrixLookAtRH(
                XMVectorSet(g.camPos.x, g.camPos.y, g.camPos.z, 1),
                XMVectorSet(0, 1.0f, -8.0f, 1),
                XMVectorSet(0, 1, 0, 0));
        }
        else
        {
            // true top-down battle camera: directly above the tank, facing -Y.
            // Up = +Z keeps screen-up = world +Z and screen-right = world -X,
            // so the screen-relative WASD mapping is unchanged.
            g.camPos = XMFLOAT3(g.camFocus.x, 27.0f, g.camFocus.z);
            view = XMMatrixLookAtRH(
                XMVectorSet(g.camPos.x, g.camPos.y, g.camPos.z, 1),
                XMVectorSet(g.camFocus.x, 0, g.camFocus.z, 1),
                XMVectorSet(0, 0, 1, 0));
            // apply the movement lean in view space (local axes)
            view = view * XMMatrixRotationY(g.camYawLean)
                        * XMMatrixRotationX(g.camPitchLean);
        }
        XMMATRIX proj = XMMatrixPerspectiveFovRH(XMConvertToRadians(46.0f),
                                                 float(g.width) / float(g.height),
                                                 0.1f, 300.0f);

        if (g.screen == Screen::InGame)
            UpdateAim(view, proj);

        // billboard basis from the view matrix (row-vector convention: columns)
        XMFLOAT4X4 vm;
        XMStoreFloat4x4(&vm, view);
        XMFLOAT3 camRight{ vm._11, vm._21, vm._31 };
        XMFLOAT3 camUp{ vm._12, vm._22, vm._32 };

        // phase bookkeeping: close the shop outside play, auto-ready in tests,
        // re-grant test money at match start
        {
            uint8_t ph = g.game.phase;
            bool playing = ph == PhasePlaying || ph == PhaseOvertime;
            if (!playing)
                g.shopOpen = false;
            if (g.prevPhase != PhasePlaying && ph == PhasePlaying && g.isHost)
            {
                // match start wiped everyone's upgrades (StartMatch); tell
                // clients BEFORE any purchase events (same reliable channel,
                // so ordering is guaranteed)
                if (g.online)
                    g.net.BroadcastOwnedReset();
                if (g.opt.rich || g.opt.shopTest)
                    g.game.players[g.myId].money = 500;
                if (g.opt.soldierTest)
                {
                    // grant AFTER the match wipe: SOLDIER class + PLATOON
                    PlayerState& me = g.game.players[g.myId];
                    me.owned.push_back(uint8_t(UpgradeId::SoldierClass));
                    me.owned.push_back(uint8_t(UpgradeId::Recruiter));
                    me.owned.push_back(uint8_t(UpgradeId::Platoon));
                    g.game.RecalcStats(g.myId);
                    Log("soldiertest: granted SOLDIER class (max %d, cooldown %.2f)",
                        int(me.stats[int(Stat::SoldierMax)] + 0.5f),
                        me.stats[int(Stat::SoldierCooldown)]);
                }
                if (g.opt.demoClass == "necro")
                {
                    PlayerState& me = g.game.players[g.myId];
                    me.owned.push_back(uint8_t(UpgradeId::NecroClass));
                    me.owned.push_back(uint8_t(UpgradeId::CausticBrew));
                    g.game.RecalcStats(g.myId);
                    Log("demo: granted NECRO class");
                }
                else if (g.opt.demoClass == "radar")
                {
                    PlayerState& me = g.game.players[g.myId];
                    me.owned.push_back(uint8_t(UpgradeId::RadarClass));
                    me.owned.push_back(uint8_t(UpgradeId::NestedArray));
                    me.owned.push_back(uint8_t(UpgradeId::NestedArray));
                    g.game.RecalcStats(g.myId);
                    Log("demo: granted RADAR class (rings %d)",
                        int(me.stats[int(Stat::RadarRings)] + 0.5f));
                }
                else if (g.opt.demoClass == "soldier")
                {
                    PlayerState& me = g.game.players[g.myId];
                    me.owned.push_back(uint8_t(UpgradeId::SoldierClass));
                    me.owned.push_back(uint8_t(UpgradeId::FragPack));
                    g.game.RecalcStats(g.myId);
                    Log("demo: granted SOLDIER class + FRAG PACK");
                }
                else if (g.opt.demoClass == "shield")
                {
                    PlayerState& me = g.game.players[g.myId];
                    me.owned.push_back(uint8_t(UpgradeId::ShieldClass));
                    me.owned.push_back(uint8_t(UpgradeId::WideBarrier));
                    g.game.RecalcStats(g.myId);
                    Log("demo: granted SHIELD class");
                }
                if (!g.opt.demoClass.empty() && g.game.players[1].active)
                {
                    // park the dummy on-camera so the whole exchange films
                    g.game.players[1].x = 15.0f;
                    g.game.players[1].z = 14.0f;
                }
            }
            if ((g.opt.shopTest || g.opt.autoDrive || g.opt.soldierTest
                 || !g.opt.demoClass.empty())
                && g.screen == Screen::InGame
                && ph == PhaseLobby && !g.game.players[g.myId].ready)
                ToggleReady();
            g.prevPhase = ph;
        }

        // ready test hook: simulate exactly one READY UP click
        if (g.opt.readyTest > 0 && g.frameCounter == uint64_t(g.opt.readyTest))
        {
            Log("ReadyTest: toggling ready (phase=%d)", int(g.game.phase));
            ToggleReady();
        }

        // pause-overlay test hook: simulate ESC (and SETTINGS) for screenshots
        if (g.opt.pauseTest && g.frameCounter == 200 && g.screen == Screen::InGame)
        {
            g.screen = Screen::Paused;
            g.shopOpen = false;
        }
        if (g.opt.pauseTest == 2 && g.frameCounter == 260
            && g.screen == Screen::Paused)
        {
            g.settingsReturn = Screen::Paused;
            g.screen = Screen::Settings;
        }

        // one-shot clicks: menus, or whatever UiHot clickable is topmost
        // under the cursor (rects registered by last frame's UI build)
        if (g.clicked)
        {
            g.clicked = false;
            if (g.screen != Screen::InGame)
            {
                HandleMenuClick();
            }
            else switch (UiHotHit(float(g.mouseX), float(g.mouseY)))
            {
            case UiIdMmToggle:
                // matchmaking toggle: is this game advertised to searchers?
                if (g.isHost && g.online)
                {
                    snd::Play(snd::Sfx::Click, 0.5f, SndJitter(0.06f));
                    if (g.net.hasPublicLobby())
                    {
                        g.net.LeaveLobby();
                    }
                    else
                    {
                        g.net.CreatePublicLobby(g.game.targetPlayers);
                        g.lastAdvertPlayers = g.lastAdvertPhase = -1;
                    }
                }
                break;
            case UiIdReady:
                if (g.game.phase == PhaseLobby)
                    ToggleReady();
                break;
            case UiIdMatchLen:
                // host cycles the match length: 5 -> 10 -> 15 -> 20 -> 5
                if (g.isHost && g.game.phase == PhaseLobby)
                {
                    snd::Play(snd::Sfx::Click, 0.5f, SndJitter(0.06f));
                    constexpr int n = int(sizeof(kMatchMinutes));
                    int cur = 0;
                    for (int k = 0; k < n; ++k)
                        if (kMatchMinutes[k] == g.game.matchMinutes)
                            cur = k;
                    g.game.matchMinutes = kMatchMinutes[(cur + 1) % n];
                }
                break;
            case UiIdOwnedArrow:
                g.ownedRowHidden = !g.ownedRowHidden;
                snd::Play(snd::Sfx::Click, 0.45f, SndJitter(0.06f));
                break;
            case UiIdShopPanel:
                if (g.shopOpen)
                    HandleShopClick(float(g.mouseX), float(g.mouseY));
                break;
            case UiIdCode:
                if (g.isHost && g.online)
                {
                    snd::Play(snd::Sfx::Click, 0.5f, SndJitter(0.06f));
                    CopyTextToClipboard(g.net.joinCode());
                    g.codeCopiedAt = g.time;
                }
                break;
            default:
                break;
            }
        }

        // class demos: periodically pull the local trigger so the radar
        // rings ride a rocket across the frame (space = the real fire path),
        // aimed at the dummy so locks (and the countdown fill) happen
        if (!g.opt.demoClass.empty() && g.screen == Screen::InGame)
        {
            g.keys[VK_SPACE] = (g.frameCounter % 200) < 10;
            if (g.opt.demoClass == "shield")   // lean on the ability key
                g.keys['1'] = true;
            const PlayerState& me = g.game.players[g.myId];
            const PlayerState& du = g.game.players[1];
            if (me.active && du.active)
            {
                // aim a NEAR MISS: the rocket passes beside the dummy so the
                // radar lock (and its countdown fill) plays out visibly
                float dx = du.x - me.x, dz = du.z - me.z;
                float len = std::max(0.001f, sqrtf(dx * dx + dz * dz));
                float txp = du.x + (-dz / len) * 2.6f;
                float tzp = du.z + (dx / len) * 2.6f;
                g.aimYaw = atan2f(txp - me.x, tzp - me.z);
            }
        }

        UpdateVfxFromSim();

        // shop test automation: open the shop, buy one card mid-burn, then
        // force-feed offers until the conveyor overflows and ejects the tail
        if (g.opt.shopTest && g.screen == Screen::InGame && g.isHost)
        {
            if (!g.shopOpen && g.time > 1.0)
                OpenShop();
            // two buys: an early one (tests the late-join OwnedSync path when
            // a client connects afterwards) and one at 14 s (tests the live
            // MsgUpgrade broadcast to already-connected clients)
            if (g.shopTestBuyAt == 0 && g.time > 2.4 && g.drawnCardCount > 0)
            {
                g.shopTestBuyAt = g.time;
                const App::DrawnCard& c = g.drawnCards[0];
                HandleShopClick(c.x + kCardW * 0.6f, c.y + kCardH * 0.45f);
            }
            else if (g.shopTestBuyAt > 0 && g.shopTestBuyAt < 10.0
                     && g.time > 14.0 && g.drawnCardCount > 0)
            {
                g.shopTestBuyAt = g.time;   // > 10: second buy consumed
                const App::DrawnCard& c = g.drawnCards[0];
                HandleShopClick(c.x + kCardW * 0.6f, c.y + kCardH * 0.45f);
            }
            if (g.time > 3.0 && g.shopTestOffersForced < 7
                && g.time > g.shopTestNextOffer)
            {
                g.shopTestNextOffer = g.time + 0.45;
                ++g.shopTestOffersForced;
                if (g.shopTestOffersForced == 1)
                {
                    // guaranteed class card first: visual check of the teal
                    // band + card glyph without relying on the rarity roll
                    Offer o;
                    o.active = OfferActive;
                    o.id = 200;
                    o.type = uint8_t(UpgradeId::SoldierClass);
                    o.cost = uint16_t(UpgradeDef(UpgradeId::SoldierClass).baseCost);
                    g.game.InsertOffer(g.myId, o);
                }
                else
                    g.game.GenerateOffer(g.myId);   // host-side direct feed
            }
        }

        FrameData frame;
        frame.objects.reserve(64);
        frame.camRight = camRight;
        frame.camUp = camUp;
        frame.uiTexTexture = g.texIconAtlas;
        frame.defaultNormalTex = g.texFlatNRA;
        BuildScene(frame, view, proj);
        g.ui.Reset(g.width, g.height);
        g.uiHotCount = 0;   // clickable registry rebuilds with the UI
        if (g.screen == Screen::InGame)
        {
            BuildHud(frame);
        }
        else
        {
            if (InSession())
                BuildHud(frame);   // live game HUD stays visible under the overlay
            BuildMenu();
        }
        frame.ui = g.ui.vertices();

        // audio bookkeeping: hover-blip edge detection (UI passes above set
        // hoverKeyNow) and the engine hum following local movement input
        {
            if (g.hoverKeyNow != g.hoverKeyPrev && g.hoverKeyNow != 0)
                snd::Play(snd::Sfx::Hover, 0.22f, SndJitter(0.08f));
            g.hoverKeyPrev = g.hoverKeyNow;
            g.hoverKeyNow = 0;

            // soldier fire: rising muzzle edge -> a lighter, snappier crack
            for (int i = 0; i < MaxSoldiers; ++i)
            {
                const SoldierState& s = g.game.soldiers[i];
                bool muzzle = s.active && s.muzzleFlash > 0;
                if (muzzle && !g.soldierPrevMuzzle[i])
                    snd::Play(snd::Sfx::Shoot,
                              SndDistVol(s.x, s.z, 0.34f),
                              1.25f * SndJitter(0.10f));
                g.soldierPrevMuzzle[i] = muzzle;
            }

            // possession start: an eerie shatter for the victim's speakers
            {
                bool possessed = InSession()
                              && g.game.players[g.myId].possessTimer > 0.0f;
                if (possessed && !g.prevPossessed)
                    snd::Play(snd::Sfx::Glass, 0.6f, 0.55f);
                g.prevPossessed = possessed;
            }

            float intensity = 0.0f;
            float turn = 0.0f;
            const PlayerState& me = g.game.players[g.myId];
            bool playing = g.game.phase == PhasePlaying
                        || g.game.phase == PhaseOvertime;
            if (g.screen == Screen::InGame && playing && me.active
                && me.health > 0)
            {
                const InputCmd& li = g.isHost
                    ? g.inputs[g.myId]
                    : (g.pendingInputs.empty() ? InputCmd{}
                                               : g.pendingInputs.back().cmd);
                intensity = std::min(1.0f, sqrtf(li.moveX * li.moveX
                                                 + li.moveZ * li.moveZ));
                // boosting revs the engine hum
                if ((li.buttons & BtnBoost) && me.boostFuel > 0.0f)
                    intensity = std::min(1.0f, intensity * 1.5f);

                // hull rotation rate from the same interpolated yaw the
                // renderer draws, normalized by the max visual turn speed
                float rx, rz, rh, rt;
                GetRenderPlayer(g.myId, rx, rz, rh, rt);
                if (g.sndHullYawValid && g.frameDt > 1e-5f)
                {
                    float w = fabsf(WrapAngle(rh - g.sndPrevHullYaw)) / g.frameDt;
                    turn = std::min(1.0f, w / HullFaceSpeed);
                    if (turn < 0.06f)
                        turn = 0.0f;   // deadband: interp jitter stays silent
                }
                g.sndPrevHullYaw = rh;
                g.sndHullYawValid = true;
            }
            else
            {
                g.sndHullYawValid = false;
            }
            snd::SetEngine(intensity);
            snd::SetTurn(turn);
            snd::Update(g.frameDt);
            if (g.rigModel.valid)
                g.rigAnimator.Update(g.frameDt);
        }

        g.renderer->RenderFrame(frame);
        QueryPerformanceCounter(&dbgD);

        // Background throttle. A covered/unfocused window's Present(1) stops
        // blocking on vblank (DXGI occlusion), so a background instance runs
        // unthrottled (~1400 fps measured) and starves the GPU -- the focused
        // instance then stutters. Classic symptom when testing host + client
        // on one PC. Cap background instances at ~60 fps; the sim accumulator
        // is fixed-timestep, so simulation and networking are unaffected.
        if (GetForegroundWindow() != g.hwnd)
            Sleep(16);

        // frame spike diagnostics: when a frame takes 3x the rolling average
        // (and > 8 ms), log where the time went so render / sim / network
        // hitches are distinguishable
        {
            static double avgMs = 5.0;
            static double prevPoll = 0, prevSim = 0, prevRender = 0;
            double fq = double(freq.QuadPart);
            double curPoll = double(dbgB.QuadPart - dbgA.QuadPart) * 1000.0 / fq;
            double curSim = double(dbgC.QuadPart - dbgB.QuadPart) * 1000.0 / fq;
            double curRender = double(dbgD.QuadPart - dbgC.QuadPart) * 1000.0 / fq;
            double frameMs = g.frameDt * 1000.0;   // previous frame's total
            if (frameMs > avgMs * 3.0 && frameMs > 8.0 && g.frameCounter > 400)
            {
                Log("SPIKE %.2fms (avg %.2f) prev[poll=%.2f sim=%.2f rend=%.2f]"
                    " cur[poll=%.2f sim=%.2f rend=%.2f] snap=%d pend=%zu t=%.1f",
                    frameMs, avgMs, prevPoll, prevSim, prevRender,
                    curPoll, curSim, curRender,
                    g.dbgSnapThisFrame ? 1 : 0,
                    g.pendingInputs.size(), g.time);
            }
            avgMs += (frameMs - avgMs) * 0.02;
            prevPoll = curPoll; prevSim = curSim; prevRender = curRender;
            g.dbgSnapThisFrame = false;
        }
        ++g.frameCounter;

        if (g.opt.screenshotAfterFrames > 0
            && g.frameCounter == uint64_t(g.opt.screenshotAfterFrames))
        {
            std::string path = g.opt.screenshotPath.empty() ? "screenshot.png"
                                                            : g.opt.screenshotPath;
            g.renderer->SaveBackbufferPNG(path);
            g.wantQuit = true;
        }
    }

    snd::Shutdown();
    g.net.Shutdown();
    delete g.renderer;
    Log("Tankaq exiting cleanly");
    return 0;
}
