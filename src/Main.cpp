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
#include "Game.h"
#include "GpuDetect.h"
#include "AssetLoad.h"
#include "Ui.h"
#include "render/IRenderer.h"
#include "net/Net.h"

using namespace DirectX;
using namespace tankaq;

namespace
{

struct Options
{
    std::string renderer;        // "", "d3d11", "d3d12"
    bool host = false;
    bool solo = false;
    std::string join;            // code or ip[:port]
    uint16_t port = net::DefaultPort;
    int screenshotAfterFrames = 0;
    std::string screenshotPath;
    bool autoDrive = false;
    bool vsync = true;
    int winX = CW_USEDEFAULT, winY = CW_USEDEFAULT;
    int winW = 0, winH = 0;
    int gi = -1, ao = -1, giRays = -1, temporal = -1;   // -1 = default
    int giHalf = -1;                                    // -1 default, 0 full, 1 half
    int shadows = -1;
    int shadowRes = -1;
    int shadowFilter = -1;
    int aa = -1;
    bool boom = false;                                  // periodic test explosions
    bool fullscreen = false;
};

const struct { int w, h; } kResolutions[] = {
    { 1280, 720 }, { 1600, 900 }, { 1920, 1080 }, { 2560, 1440 }
};

enum class Screen { MainMenu, JoinEntry, Connecting, InGame, Settings };

struct App
{
    Options opt;
    HWND hwnd{};
    int width = 1280, height = 720;
    IRenderer* renderer = nullptr;
    GpuInfo gpu;
    Backend currentBackend = Backend::D3D11;
    bool vsyncOn = true;
    bool fullscreen = false;
    RECT windowedRect{};
    int resIndex = 0;

    // input
    bool keys[256]{};
    bool mouseDown = false;
    int mouseX = 0, mouseY = 0;
    bool wantQuit = false;
    bool clicked = false;        // one-shot left click this frame

    // assets / gpu handles
    TankModel tank;
    int meshHull = -1, meshTurret = -1, meshGround = -1, meshProj = -1, meshFlash = -1;
    std::vector<int> meshObstacles;
    std::vector<int> meshWalls;
    int texPalette = -1, texGround = -1, texWall = -1, texWhite = -1;

    // game
    GameState game;
    InputCmd inputs[MaxPlayers]{};
    net::Net net;
    Screen screen = Screen::MainMenu;
    int myId = 0;
    bool isHost = false;
    bool online = false;         // false in solo
    std::string statusLine;
    std::string joinText;
    double connectStart = 0;
    PostSettings post;           // GI / SSAO / shadow settings
    float aimYaw = 0;

    // fixed-tick render interpolation + client prediction
    GameState prevTick;                     // host/solo: state before latest tick
    float tickAlpha = 0;                    // fraction of a tick for rendering
    net::MsgSnapshot snapPrev{}, snapCurr{};// client: remote interpolation pair
    double snapCurrTime = 0;
    bool haveSnap = false, haveTwoSnaps = false;
    uint32_t inputSeq = 0;                  // client: next input sequence number
    struct PendingInput { uint32_t seq; InputCmd cmd; };
    std::vector<PendingInput> pendingInputs;
    PlayerState predPrev{}, predCurr{};     // client: local tank tick pair
    uint32_t inputSeqs[MaxPlayers]{};       // host: last input seq per player

    // game-code click-to-copy
    float codeRect[4]{};                    // x, y, w, h
    double codeCopiedAt = -10;

    // vfx
    struct Burst { XMFLOAT3 pos; double t0; };
    struct Scorch { XMFLOAT3 pos; double t0; };
    std::vector<Burst> bursts;
    std::vector<Scorch> scorches;
    bool prevProjActive[MaxProjectiles]{};
    XMFLOAT3 prevProjPos[MaxProjectiles]{};
    double lastBoom = 0;
    XMFLOAT3 camPos{ 0, 18, -16 };
    XMFLOAT3 camFocus{ 0, 0, 0 };
    bool camFocusValid = false;
    double time = 0;
    uint64_t frameCounter = 0;
    float fps = 0;

    UiBuilder ui;
};

App g;

bool CreateAssets();
void UpdateTitle();
IRenderer* CreateBackend(Backend b, std::string& err);
void SwitchRenderer(Backend want);
void ApplyDisplayMode();
void ApplyResolution();
void CopyTextToClipboard(const std::string& text);

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
    o.boom = cmd.find("--boom") != std::string::npos;
    o.fullscreen = cmd.find("--fullscreen") != std::string::npos;
    if (!(v = GetArg(cmd, "--winsize=")).empty())
        sscanf_s(v.c_str(), "%dx%d", &o.winW, &o.winH);
    return o;
}

void ApplySnapshot(const net::MsgSnapshot& s)
{
    // keep the last two snapshots for remote-player render interpolation
    g.snapPrev = g.haveSnap ? g.snapCurr : s;
    g.snapCurr = s;
    g.snapCurrTime = g.time;
    g.haveTwoSnaps = g.haveSnap;
    g.haveSnap = true;

    g.game.tick = s.tick;
    for (int i = 0; i < MaxPlayers; ++i)
    {
        const net::PlayerNet& in = s.players[i];
        PlayerState& p = g.game.players[i];
        bool wasActive = p.active;
        p.active = in.active != 0;
        if (!p.active)
            continue;
        if (!wasActive)
            p = PlayerState{}, p.active = true;
        p.health = in.health;
        p.score = in.score;
        p.hitFlash = (in.flags & 1) ? 0.25f : std::max(0.0f, p.hitFlash - TickDt);
        p.muzzleFlash = (in.flags & 2) ? 0.10f : std::max(0.0f, p.muzzleFlash - TickDt);
        if (i != g.myId)
        {
            p.x = in.x; p.z = in.z;
            p.hullYaw = in.hullYaw;
            p.turretYaw = in.turretYaw;
        }
    }

    // Reconciliation: adopt the authoritative state for our own tank, then
    // replay every input the host hasn't simulated yet. When prediction was
    // right (normal case) the result matches what we already show.
    const net::PlayerNet& own = s.players[g.myId];
    if (own.active)
    {
        PlayerState& me = g.game.players[g.myId];
        me.x = own.x; me.z = own.z;
        me.hullYaw = own.hullYaw;
        me.turretYaw = own.turretYaw;
        while (!g.pendingInputs.empty() && g.pendingInputs.front().seq <= own.ackSeq)
            g.pendingInputs.erase(g.pendingInputs.begin());
        for (const auto& pi : g.pendingInputs)
            g.game.AdvanceMovement(g.myId, pi.cmd);
        g.predPrev = g.predCurr = me;
    }

    for (int i = 0; i < MaxProjectiles; ++i)
    {
        const net::ProjectileNet& in = s.projectiles[i];
        Projectile& pr = g.game.projectiles[i];
        pr.active = in.active != 0;
        pr.x = in.x; pr.y = in.y; pr.z = in.z; pr.yaw = in.yaw;
    }
}

net::MsgSnapshot BuildSnapshot()
{
    net::MsgSnapshot s;
    s.tick = g.game.tick;
    for (int i = 0; i < MaxPlayers; ++i)
    {
        const PlayerState& p = g.game.players[i];
        net::PlayerNet& out = s.players[i];
        out.active = p.active ? 1 : 0;
        out.health = uint8_t(p.health);
        out.score = p.score;
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
        out.x = pr.x; out.y = pr.y; out.z = pr.z; out.yaw = pr.yaw;
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
    }
    else if (id == g.myId)
    {
        float t = g.tickAlpha;
        x = g.predPrev.x + (g.predCurr.x - g.predPrev.x) * t;
        z = g.predPrev.z + (g.predCurr.z - g.predPrev.z) * t;
        hullYaw = LerpAngle(g.predPrev.hullYaw, g.predCurr.hullYaw, t);
        turretYaw = LerpAngle(g.predPrev.turretYaw, g.predCurr.turretYaw, t);
    }
    else if (g.haveTwoSnaps)
    {
        const net::PlayerNet& a = g.snapPrev.players[id];
        const net::PlayerNet& b = g.snapCurr.players[id];
        if (a.active && b.active)
        {
            float t = std::clamp(float((g.time - g.snapCurrTime)
                                       / (SnapshotEveryTicks * TickDt)), 0.0f, 1.0f);
            x = a.x + (b.x - a.x) * t;
            z = a.z + (b.z - a.z) * t;
            hullYaw = LerpAngle(a.hullYaw, b.hullYaw, t);
            turretYaw = LerpAngle(a.turretYaw, b.turretYaw, t);
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
        in.buttons |= BtnForward;
        in.buttons |= (fmod(g.time, 6.0) < 3.0) ? BtnLeft : BtnRight;
        uint32_t t = g.game.tick;
        if (t % 75 < 2)
            in.buttons |= BtnFire;
        in.turretYaw = g.game.players[g.myId].hullYaw + 0.6f * sinf(float(g.time) * 0.7f);
        return in;
    }
    if (g.keys['W'] || g.keys[VK_UP]) in.buttons |= BtnForward;
    if (g.keys['S'] || g.keys[VK_DOWN]) in.buttons |= BtnBack;
    if (g.keys['A'] || g.keys[VK_LEFT]) in.buttons |= BtnLeft;
    if (g.keys['D'] || g.keys[VK_RIGHT]) in.buttons |= BtnRight;
    if (g.mouseDown || g.keys[VK_SPACE]) in.buttons |= BtnFire;
    in.turretYaw = g.aimYaw;
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
}

// Spawn VFX whenever a projectile slot flips active -> inactive. This works
// identically for solo, host (sim) and clients (snapshot inference).
void UpdateVfxFromSim()
{
    for (int i = 0; i < MaxProjectiles; ++i)
    {
        const Projectile& pr = g.game.projectiles[i];
        if (g.prevProjActive[i] && !pr.active && g.screen == Screen::InGame)
            SpawnExplosion(g.prevProjPos[i]);
        g.prevProjActive[i] = pr.active;
        if (pr.active)
            g.prevProjPos[i] = XMFLOAT3(pr.x, pr.y, pr.z);
    }
    // prune dead effects
    while (!g.bursts.empty() && g.time - g.bursts.front().t0 > 3.0)
        g.bursts.erase(g.bursts.begin());
    while (!g.scorches.empty() && g.time - g.scorches.front().t0 > 30.0)
        g.scorches.erase(g.scorches.begin());

    if (g.opt.boom && g.screen == Screen::InGame && g.time - g.lastBoom > 2.2)
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
    g.prevTick = GameState{};
    g.haveSnap = g.haveTwoSnaps = false;
    g.pendingInputs.clear();
    g.inputSeq = 0;
    for (uint32_t& s : g.inputSeqs) s = 0;
    for (InputCmd& c : g.inputs) c = InputCmd{};
}

void LeaveToMenu(const std::string& why)
{
    g.net.Disconnect();
    g.screen = Screen::MainMenu;
    g.online = false;
    g.isHost = false;
    ResetNetSimState();
    g.statusLine = why;
}

void StartSolo()
{
    g.myId = 0;
    g.isHost = true;
    g.online = false;
    ResetNetSimState();
    g.game.SpawnPlayer(0);
    g.screen = Screen::InGame;
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
    g.screen = Screen::InGame;
    g.statusLine.clear();
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

    // ground + obstacles + boundary walls
    frame.objects.push_back({ g.meshGround, g.texGround, Store(XMMatrixIdentity()), { 1,1,1,0 } });
    for (size_t i = 0; i < g.meshObstacles.size(); ++i)
    {
        const Obstacle& o = kObstacles[i];
        frame.objects.push_back({ g.meshObstacles[i], g.texWall,
            Store(XMMatrixTranslation(o.cx, o.height * 0.5f, o.cz)), { 1,1,1,0 } });
    }
    for (size_t i = 0; i < g.meshWalls.size(); ++i)
    {
        float h = ArenaHalf;
        XMMATRIX m = (i == 0) ? XMMatrixTranslation(0, 0.6f, h)
                   : (i == 1) ? XMMatrixTranslation(0, 0.6f, -h)
                   : (i == 2) ? XMMatrixTranslation(h, 0.6f, 0)
                              : XMMatrixTranslation(-h, 0.6f, 0);
        frame.objects.push_back({ g.meshWalls[i], g.texWall, Store(m), { 1,1,1,0 } });
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
        XMFLOAT4 tint{ 1, 1, 1, 0 };
        if (i == g.myId)
            tint = { 0.85f, 1.0f, 0.85f, 0 };          // greenish self
        else
            tint = { 1.0f, 0.88f, 0.80f, 0 };          // warm enemies
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
        XMMATRIX m = XMMatrixScaling(1, 1, 3)
                   * XMMatrixRotationY(pr.yaw)
                   * XMMatrixTranslation(px, py, pz);
        frame.objects.push_back({ g.meshProj, g.texWhite, Store(m),
                                  { 0.15f, 0.13f, 0.1f, 0.25f }, true });
    }
}

void BuildHud()
{
    const PlayerState& me = g.game.players[g.myId];
    float w = float(g.width);

    char buf[256];
    sprintf_s(buf, "%s  |  %s  |  %.0f FPS", g.renderer->Name(), g.gpu.name.c_str(), g.fps);
    g.ui.Text(10, 10, 1.6f, { 1, 1, 1, 0.75f }, buf);
    sprintf_s(buf, "GI %s %s (%d rays x %d temporal = %d eff)  SSAO %s  SHADOWS %s   [F5/F6/F7]",
              g.post.giEnabled ? "ON" : "OFF", g.post.giHalfRes ? "HALF-RES" : "FULL-RES",
              g.post.giRays, g.post.temporalSamples,
              g.post.giRays * g.post.temporalSamples, g.post.aoEnabled ? "ON" : "OFF",
              g.post.shadowsEnabled ? "ON" : "OFF");
    g.ui.Text(10, 28, 1.4f, { 1, 1, 1, 0.55f }, buf);

    if (g.isHost && g.online)
    {
        std::string code = "GAME CODE: " + g.net.joinCode();
        float tw = g.ui.TextWidth(2.0f, code);
        bool justCopied = g.time - g.codeCopiedAt < 1.5;
        g.codeRect[0] = w * 0.5f - tw * 0.5f - 12;
        g.codeRect[1] = 6;
        g.codeRect[2] = tw + 24;
        g.codeRect[3] = 28;
        bool hover = float(g.mouseX) >= g.codeRect[0]
                  && float(g.mouseX) <= g.codeRect[0] + g.codeRect[2]
                  && float(g.mouseY) >= g.codeRect[1]
                  && float(g.mouseY) <= g.codeRect[1] + g.codeRect[3];
        g.ui.Rect(g.codeRect[0], g.codeRect[1], g.codeRect[2], g.codeRect[3],
                  hover ? UiColor{ 0.12f, 0.16f, 0.1f, 0.8f } : UiColor{ 0, 0, 0, 0.55f });
        g.ui.TextCentered(w * 0.5f, 12, 2.0f,
                          justCopied ? UiColor{ 0.6f, 1.0f, 0.6f, 1 }
                                     : UiColor{ 1, 0.95f, 0.6f, 1 }, code);
        g.ui.TextCentered(w * 0.5f, 38, 1.5f, { 1, 1, 1, 0.7f },
                          justCopied ? "COPIED TO CLIPBOARD"
                                     : "click the code to copy it");
        sprintf_s(buf, "players connected: %d", g.net.connectedClients() + 1);
        g.ui.TextCentered(w * 0.5f, 56, 1.5f, { 1, 1, 1, 0.7f }, buf);
    }

    // health bar
    float hx = 10, hy = float(g.height) - 46;
    g.ui.Rect(hx, hy, 260, 26, { 0, 0, 0, 0.55f });
    float frac = float(me.health) / MaxHealth;
    g.ui.Rect(hx + 3, hy + 3, 254 * std::max(0.0f, frac), 20,
              frac > 0.5f ? UiColor{ 0.3f, 0.8f, 0.25f, 0.95f }
              : frac > 0.25f ? UiColor{ 0.9f, 0.75f, 0.2f, 0.95f }
                             : UiColor{ 0.9f, 0.2f, 0.15f, 0.95f });
    sprintf_s(buf, "HP %d", me.health);
    g.ui.Text(hx + 8, hy + 6, 2.0f, { 0, 0, 0, 0.9f }, buf);

    // reload bar
    float rf = 1.0f - me.fireCooldown / FireCooldown;
    g.ui.Rect(hx, hy - 14, 260, 8, { 0, 0, 0, 0.45f });
    g.ui.Rect(hx + 2, hy - 12, 256 * std::clamp(rf, 0.0f, 1.0f), 4, { 0.95f, 0.9f, 0.5f, 0.9f });

    // scoreboard
    float sy = 60;
    for (int i = 0; i < MaxPlayers; ++i)
    {
        const PlayerState& p = g.game.players[i];
        if (!p.active)
            continue;
        sprintf_s(buf, "%s P%d   kills %u %s", i == g.myId ? ">" : " ", i + 1,
                  unsigned(p.score), p.health <= 0 ? "(down)" : "");
        g.ui.Text(w - 230, sy, 1.7f, i == g.myId ? UiColor{ 0.8f, 1, 0.8f, 0.95f }
                                                 : UiColor{ 1, 1, 1, 0.8f }, buf);
        sy += 18;
    }

    if (me.health <= 0 && me.active)
    {
        sprintf_s(buf, "DESTROYED - respawn in %.1f", std::max(0.0f, me.respawnTimer));
        g.ui.TextCentered(w * 0.5f, g.height * 0.42f, 3.0f, { 1, 0.3f, 0.25f, 1 }, buf);
    }

    // crosshair
    g.ui.Rect(float(g.mouseX) - 9, float(g.mouseY) - 1, 18, 2, { 1, 1, 1, 0.8f });
    g.ui.Rect(float(g.mouseX) - 1, float(g.mouseY) - 9, 2, 18, { 1, 1, 1, 0.8f });

    g.ui.Text(10, float(g.height) - 74, 1.4f, { 1, 1, 1, 0.45f },
              "WASD drive   mouse aim   LMB/space fire   ESC menu");
    if (!g.statusLine.empty())
        g.ui.TextCentered(w * 0.5f, float(g.height) - 30, 1.7f, { 1, 0.8f, 0.4f, 0.95f },
                          g.statusLine);
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
        buttons.push_back({ x, y + 0 * (bh + gap), bw, bh, "PLAY SOLO" });
        buttons.push_back({ x, y + 1 * (bh + gap), bw, bh, "HOST GAME" });
        buttons.push_back({ x, y + 2 * (bh + gap), bw, bh, "JOIN GAME" });
        buttons.push_back({ x, y + 3 * (bh + gap), bw, bh, "SETTINGS" });
        buttons.push_back({ x, y + 4 * (bh + gap), bw, bh, "QUIT" });
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
    g.ui.Rect(0, 0, w, h, { 0.05f, 0.07f, 0.05f, 0.35f });
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
        sprintf_s(cbuf, "CONNECTING... %.0fs", g.time - g.connectStart);
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

    if (!g.statusLine.empty())
        g.ui.TextCentered(w * 0.5f, h * 0.33f, 1.8f, { 1, 0.7f, 0.5f, 1 }, g.statusLine);

    auto buttons = MenuButtons();
    for (const UiButton& b : buttons)
        DrawButton(g.ui, b, b.Contains(float(g.mouseX), float(g.mouseY)));
}

void HandleMenuClick()
{
    auto buttons = MenuButtons();
    for (const UiButton& b : buttons)
    {
        if (!b.Contains(float(g.mouseX), float(g.mouseY)))
            continue;
        if (b.label == "PLAY SOLO") StartSolo();
        else if (b.label == "HOST GAME") StartHost();
        else if (b.label == "JOIN GAME") { g.screen = Screen::JoinEntry; g.statusLine.clear(); }
        else if (b.label == "SETTINGS") { g.screen = Screen::Settings; g.statusLine.clear(); }
        else if (b.label == "QUIT") g.wantQuit = true;
        else if (b.label == "CONNECT") { if (!g.joinText.empty()) StartJoin(g.joinText); }
        else if (b.label == "BACK") { g.screen = Screen::MainMenu; g.statusLine.clear(); }
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
                LeaveToMenu(g.online ? "left the game" : "");
            else if (g.screen == Screen::JoinEntry || g.screen == Screen::Settings)
                g.screen = Screen::MainMenu;
        }
        if (wp == VK_F5) g.post.giEnabled = !g.post.giEnabled;
        if (wp == VK_F6) g.post.aoEnabled = !g.post.aoEnabled;
        if (wp == VK_F7) g.post.shadowsEnabled = !g.post.shadowsEnabled;
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

    MeshData ground = MakeGroundPlane(ArenaHalf, 30.0f);
    g.meshGround = r->CreateMesh(ground.verts.data(), ground.verts.size(),
                                 ground.indices.data(), ground.indices.size());
    MeshData proj = MakeSphere(0.16f, 12, 8);
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
    ImageData wallTex = MakeWallTexture(128);
    g.texWall = r->CreateTexture(wallTex.rgba.data(), wallTex.width, wallTex.height);
    ImageData white = MakeSolidTexture(255, 255, 255);
    g.texWhite = r->CreateTexture(white.rgba.data(), white.width, white.height);
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

    bool steamOk = g.net.InitSteam();
    if (!steamOk)
        g.statusLine = "steam not available - solo only";

    // Command-line shortcuts for testing.
    if (g.opt.solo)
        StartSolo();
    else if (g.opt.host)
        (steamOk ? StartHost() : StartSolo());
    else if (!g.opt.join.empty() && steamOk)
        StartJoin(g.opt.join);

    net::Net::Events ev;
    ev.onPlayerJoined = [](int pid) { g.game.SpawnPlayer(pid); };
    ev.onPlayerLeft = [](int pid) { g.game.RemovePlayer(pid); };
    ev.onInput = [](int pid, const net::MsgInput& m)
    {
        if (m.seq > g.inputSeqs[pid])   // ignore late/out-of-order packets
        {
            g.inputs[pid].buttons = m.buttons;
            g.inputs[pid].turretYaw = m.turretYaw;
            g.inputSeqs[pid] = m.seq;
        }
    };
    ev.onWelcome = [](int myId)
    {
        g.myId = myId;
        g.screen = Screen::InGame;
        g.statusLine.clear();
        Log("Joined as player %d", myId);
    };
    ev.onSnapshot = [](const net::MsgSnapshot& s)
    {
        if (g.screen == Screen::InGame || g.screen == Screen::Connecting)
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
        accumulator += dt;
        fpsTimer += dt;
        ++fpsFrames;
        if (fpsTimer >= 0.5)
        {
            g.fps = float(fpsFrames / fpsTimer);
            fpsTimer = 0;
            fpsFrames = 0;
        }

        g.net.Poll(ev);

        // joining watchdog
        if (g.screen == Screen::Connecting && g.time - g.connectStart > 30.0)
            LeaveToMenu("JOIN TIMED OUT - check the code and that the host is in game");

        // fixed timestep simulation
        while (accumulator >= TickDt)
        {
            accumulator -= TickDt;
            if (g.screen != Screen::InGame)
                continue;
            InputCmd local = BuildLocalInput();
            if (g.isHost)
            {
                g.inputs[g.myId] = local;
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
                m.turretYaw = local.turretYaw;
                m.seq = ++g.inputSeq;
                g.net.SendInputToHost(m);

                // client-side prediction: move our tank immediately with the
                // same integration the host runs; snapshots reconcile later
                PlayerState& me = g.game.players[g.myId];
                if (me.active && me.health > 0)
                {
                    InputCmd cmd{ local.buttons, local.turretYaw };
                    g.pendingInputs.push_back({ g.inputSeq, cmd });
                    if (g.pendingInputs.size() > 120)
                        g.pendingInputs.erase(g.pendingInputs.begin());
                    g.predPrev = me;
                    g.game.AdvanceMovement(g.myId, cmd);
                    g.predCurr = me;
                }
            }
        }
        g.tickAlpha = float(accumulator / TickDt);

        // Camera: smooth-follow of the tank's *interpolated* render position.
        // The target is continuous (tick-interpolated), so smoothing can't
        // fight a stair-stepping signal anymore. Twitch-proofing:
        //  - exponential smoothing with dt (frame-rate independent, can never
        //    overshoot or oscillate),
        //  - a single smoothed focus point drives BOTH the camera position and
        //    the look-at target, so they always agree (no relative twist),
        //  - snap to the target when within a millimeter to kill float noise.
        XMFLOAT3 target{ 0, 0, 0 };
        if (g.screen == Screen::InGame && g.game.players[g.myId].active)
        {
            float rx, rz, rh, rt;
            GetRenderPlayer(g.myId, rx, rz, rh, rt);
            target = XMFLOAT3(rx, 0, rz);
        }
        if (!g.camFocusValid || g.screen != Screen::InGame)
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
        g.camPos = XMFLOAT3(g.camFocus.x, 19.0f, g.camFocus.z - 14.5f);

        XMMATRIX view = XMMatrixLookAtRH(
            XMVectorSet(g.camPos.x, g.camPos.y, g.camPos.z, 1),
            XMVectorSet(g.camFocus.x, 0, g.camFocus.z + 2.0f, 1),
            XMVectorSet(0, 1, 0, 0));
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

        // one-shot clicks: menus, or the copyable game code in the HUD
        if (g.clicked)
        {
            g.clicked = false;
            if (g.screen != Screen::InGame)
            {
                HandleMenuClick();
            }
            else if (g.isHost && g.online
                     && g.mouseX >= g.codeRect[0]
                     && g.mouseX <= g.codeRect[0] + g.codeRect[2]
                     && g.mouseY >= g.codeRect[1]
                     && g.mouseY <= g.codeRect[1] + g.codeRect[3])
            {
                CopyTextToClipboard(g.net.joinCode());
                g.codeCopiedAt = g.time;
            }
        }

        UpdateVfxFromSim();

        FrameData frame;
        frame.objects.reserve(64);
        frame.camRight = camRight;
        frame.camUp = camUp;
        BuildScene(frame, view, proj);
        g.ui.Reset(g.width, g.height);
        if (g.screen == Screen::InGame)
            BuildHud();
        else
            BuildMenu();
        frame.ui = g.ui.vertices();
        g.renderer->RenderFrame(frame);
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

    g.net.Shutdown();
    delete g.renderer;
    Log("Tankaq exiting cleanly");
    return 0;
}
