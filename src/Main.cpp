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
    bool rich = false;                                  // start with 500 credits
    bool shopTest = false;                              // auto-open + auto-buy (testing)
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
    float frameDt = 0;
    double time = 0;

    // upgrade shop (conveyor)
    bool shopOpen = false;
    struct CardAnim { uint8_t id = 0; float x = 0, y = 0; int lastSlot = 0; bool active = false; };
    CardAnim cardAnims[12]{};
    struct ShopBurnFx { float x, y, w, h; float ox, oy; double t0; int icon; };
    std::vector<ShopBurnFx> shopBurnFx;
    struct EjectFx { int icon, rarity; float x, y, vx, vy, ang, angVel; bool bounced; };
    std::vector<EjectFx> ejectFx;
    struct DebrisFx { float x, y, w, h; float vx, vy, ang, angVel; double t0; };
    std::vector<DebrisFx> debrisFx;
    double slatsBrokenUntil = 0;
    float shopPanel[4]{};                   // x, y, w, h
    struct DrawnCard { uint8_t id; int slot; float x, y; };
    DrawnCard drawnCards[NumOfferSlots]{};
    int drawnCardCount = 0;
    uint8_t lastClickedOfferId = 0;
    float lastClickX = 0, lastClickY = 0;
    int lastClickedIcon = -1;
    int lastTailIcon = 0, lastTailRarity = 0;
    double shopTestBuyAt = 0, shopTestNextOffer = 0;
    int shopTestOffersForced = 0;
    int texIconAtlas = -1;

    // camera lean (movement game feel)
    float camYawLean = 0, camPitchLean = 0;
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
void OpenShop();
void HandleShopClick(float mx, float my);

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
    o.rich = cmd.find("--rich") != std::string::npos;
    o.shopTest = cmd.find("--shoptest") != std::string::npos;
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
        p.money = in.money;
        memcpy(p.stats, in.stats, sizeof(p.stats));
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
        out.health = uint16_t(std::clamp(p.health, 0, 65535));
        out.score = p.score;
        out.money = p.money;
        memcpy(out.stats, p.stats, sizeof(out.stats));
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
        float ang = float(g.time) * 0.45f;
        in.moveX = sinf(ang);
        in.moveZ = cosf(ang);
        uint32_t t = g.game.tick;
        if (t % 75 < 2)
            in.buttons |= BtnFire;
        in.turretYaw = g.game.players[g.myId].hullYaw + 0.6f * sinf(float(g.time) * 0.7f);
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
    // don't fire while clicking inside the open shop panel
    if (g.shopOpen && g.mouseX >= g.shopPanel[0]
        && g.mouseX <= g.shopPanel[0] + g.shopPanel[2]
        && g.mouseY >= g.shopPanel[1]
        && g.mouseY <= g.shopPanel[1] + g.shopPanel[3])
        fire = false;
    if (fire) in.buttons |= BtnFire;
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
    g.game.rngState = uint32_t(GetTickCount64() | 1);
    g.prevTick = GameState{};
    g.haveSnap = g.haveTwoSnaps = false;
    g.pendingInputs.clear();
    g.inputSeq = 0;
    for (uint32_t& s : g.inputSeqs) s = 0;
    for (InputCmd& c : g.inputs) c = InputCmd{};
    for (auto& a : g.cardAnims) a.active = false;
    g.shopBurnFx.clear();
    g.ejectFx.clear();
    g.debrisFx.clear();
    g.lastClickedOfferId = 0;
    g.shopTestBuyAt = g.shopTestNextOffer = 0;
    g.shopTestOffersForced = 0;
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
    if (g.opt.rich || g.opt.shopTest)
        g.game.players[0].money = 500;
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
    if (g.opt.rich || g.opt.shopTest)
        g.game.players[0].money = 500;
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

void OpenShop()
{
    g.shopOpen = true;
    // fresh entrance: existing offers slide in from off-screen again
    for (auto& a : g.cardAnims)
        a.active = false;
}

// ---- shop drawing helpers (rotated quads share the plain triangle paths) ----

void RotatedRect(float cx, float cy, float w, float h, float ang, UiColor c)
{
    float ca = cosf(ang), sa = sinf(ang);
    float hx = w * 0.5f, hy = h * 0.5f;
    float xs[4] = { -hx, hx, hx, -hx };
    float ys[4] = { -hy, -hy, hy, hy };
    float px[4], py[4];
    for (int i = 0; i < 4; ++i)
    {
        px[i] = cx + xs[i] * ca - ys[i] * sa;
        py[i] = cy + xs[i] * sa + ys[i] * ca;
    }
    g.ui.Tri(px[0], py[0], px[1], py[1], px[2], py[2], c);
    g.ui.Tri(px[0], py[0], px[2], py[2], px[3], py[3], c);
}

void IconUv(int icon, float& u0, float& u1)
{
    u0 = float(icon) / float(UpgradePoolSize);
    u1 = float(icon + 1) / float(UpgradePoolSize);
}

void AddIconQuad(FrameData& frame, int icon, float cx, float cy, float half,
                 float alpha, float ang = 0.0f)
{
    float u0, u1;
    IconUv(icon, u0, u1);
    float ca = cosf(ang), sa = sinf(ang);
    float xs[4] = { -half, half, half, -half };
    float ys[4] = { -half, -half, half, half };
    float us[4] = { u0, u1, u1, u0 };
    float vs[4] = { 0, 0, 1, 1 };
    UiTexVertex v[4];
    for (int i = 0; i < 4; ++i)
        v[i] = { cx + xs[i] * ca - ys[i] * sa, cy + xs[i] * sa + ys[i] * ca,
                 us[i], vs[i], 1, 1, 1, alpha };
    frame.uiTex.push_back(v[0]); frame.uiTex.push_back(v[1]); frame.uiTex.push_back(v[2]);
    frame.uiTex.push_back(v[0]); frame.uiTex.push_back(v[2]); frame.uiTex.push_back(v[3]);
}

static const UiColor kRarityCol[5] = {
    { 0.35f, 0.37f, 0.35f, 0.96f },   // common
    { 0.15f, 0.40f, 0.20f, 0.96f },   // uncommon
    { 0.14f, 0.28f, 0.55f, 0.96f },   // rare
    { 0.40f, 0.17f, 0.52f, 0.96f },   // epic
    { 0.62f, 0.35f, 0.10f, 0.96f },   // legendary
};

constexpr float kCardW = 150, kCardH = 96, kCardGap = 10, kShopHeader = 34;
constexpr int kNumSlats = 4;

void SlotCell(int slot, float& x, float& y)
{
    int col = slot % 2, row = slot / 2;
    x = g.shopPanel[0] + kCardGap + col * (kCardW + kCardGap);
    y = g.shopPanel[1] + kShopHeader + kCardGap + row * (kCardH + kCardGap);
}

// Panel outline where the lower-right border is a set of breakable vertical
// slats: the conveyor's exit hatch. While broken, the slats are absent (their
// debris is flying around instead).
void DrawShopFrame()
{
    float px = g.shopPanel[0], py = g.shopPanel[1];
    float pw = g.shopPanel[2], ph = g.shopPanel[3];
    UiColor frameCol{ 0.5f, 0.58f, 0.42f, 1 };
    g.ui.Rect(px, py, pw, 2, frameCol);                       // top
    g.ui.Rect(px, py + ph - 2, pw, 2, frameCol);              // bottom
    g.ui.Rect(px, py + 2, 2, ph - 4, frameCol);               // left
    float hatchTop = py + kShopHeader + kCardGap + 2 * (kCardH + kCardGap);
    float hatchBottom = py + ph - 2;
    g.ui.Rect(px + pw - 2, py + 2, 2, hatchTop - (py + 2), frameCol);  // right, upper
    if (g.time >= g.slatsBrokenUntil)
    {
        float span = hatchBottom - hatchTop;
        for (int s = 0; s < kNumSlats; ++s)
            g.ui.Rect(px + pw - 2, hatchTop + span * (s + 0.15f) / kNumSlats,
                      2, span * 0.55f / kNumSlats, frameCol);
    }
}

void BreakSlats()
{
    float px = g.shopPanel[0], pw = g.shopPanel[2], py = g.shopPanel[1],
          ph = g.shopPanel[3];
    float hatchTop = py + kShopHeader + kCardGap + 2 * (kCardH + kCardGap);
    float span = (py + ph - 2) - hatchTop;
    g.slatsBrokenUntil = g.time + 2.2;
    for (int s = 0; s < kNumSlats; ++s)
    {
        App::DebrisFx d{};
        d.x = px + pw - 1;
        d.y = hatchTop + span * (s + 0.4f) / kNumSlats;
        d.w = 3;
        d.h = span * 0.55f / kNumSlats;
        uint32_t r = uint32_t(s * 2654435761u + uint32_t(g.time * 977.0));
        auto rnd = [&r]() { r ^= r << 13; r ^= r >> 17; r ^= r << 5;
                            return float(r & 0xFFFF) / 65535.0f; };
        d.vx = 350.0f + 650.0f * rnd();
        d.vy = -420.0f + 300.0f * rnd();
        d.angVel = (rnd() - 0.5f) * 16.0f;
        d.ang = 0;
        d.t0 = g.time;
        g.debrisFx.push_back(d);
    }
}

void BuildShop(FrameData& frame)
{
    const PlayerState& me = g.game.players[g.myId];
    float panelW = kCardGap * 3 + kCardW * 2;
    float panelH = kShopHeader + kCardGap + 3 * kCardH + 2 * kCardGap + kCardGap;
    float px = 14;
    float py = (float(g.height) - panelH) * 0.5f;
    g.shopPanel[0] = px; g.shopPanel[1] = py;
    g.shopPanel[2] = panelW; g.shopPanel[3] = panelH;

    g.ui.Rect(px, py, panelW, panelH, { 0.07f, 0.09f, 0.07f, 0.85f });
    DrawShopFrame();
    char buf[96];
    sprintf_s(buf, "UPGRADES   $ %u", unsigned(me.money));
    g.ui.Text(px + 10, py + 9, 2.2f, { 1, 0.95f, 0.6f, 1 }, buf);

    // empty sockets
    for (int s = 0; s < NumOfferSlots; ++s)
    {
        float sx, sy;
        SlotCell(s, sx, sy);
        g.ui.Rect(sx, sy, kCardW, kCardH, { 0, 0, 0, 0.30f });
        g.ui.RectOutline(sx, sy, kCardW, kCardH, 1, { 1, 1, 1, 0.08f });
    }

    // ---- conveyor: match offers to card animations by rolling id ----
    float k = 1.0f - expf(-g.frameDt * 10.0f);
    g.drawnCardCount = 0;
    bool offerSeen[12]{};
    int hoverSlot = -1;
    float hoverX = 0, hoverY = 0;

    for (int s = 0; s < NumOfferSlots; ++s)
    {
        const Offer& o = me.offers[s];
        if (!o.active)
            continue;
        float tx, ty;
        SlotCell(s, tx, ty);

        // find or create this offer's animation entry
        App::CardAnim* anim = nullptr;
        for (auto& a : g.cardAnims)
            if (a.active && a.id == o.id) { anim = &a; break; }
        if (!anim)
        {
            for (auto& a : g.cardAnims)
                if (!a.active) { anim = &a; break; }
            if (!anim) anim = &g.cardAnims[0];
            *anim = { o.id, -220.0f, ty, s, true };   // enter from off-screen left
        }
        for (auto& a : g.cardAnims)
            if (&a == anim)
            {
                float ex = tx - a.x, ey = ty - a.y;
                if (ex * ex + ey * ey < 0.25f) { a.x = tx; a.y = ty; }
                else { a.x += ex * k; a.y += ey * k; }
                a.lastSlot = s;
            }
        offerSeen[anim - g.cardAnims] = true;

        const UpgradeType& def = kUpgradePool[o.type];
        UiColor bg = kRarityCol[def.rarity];
        bool afford = me.money >= o.cost;
        bool hover = float(g.mouseX) >= anim->x && float(g.mouseX) <= anim->x + kCardW
                  && float(g.mouseY) >= anim->y && float(g.mouseY) <= anim->y + kCardH;
        if (hover) { hoverSlot = s; hoverX = anim->x; hoverY = anim->y; }

        UiColor bgDraw = bg;
        if (hover) { bgDraw.r *= 1.3f; bgDraw.g *= 1.3f; bgDraw.b *= 1.3f; }
        g.ui.Rect(anim->x, anim->y, kCardW, kCardH, bgDraw);
        g.ui.RectOutline(anim->x, anim->y, kCardW, kCardH, 2,
                         hover ? UiColor{ 1, 1, 0.85f, 1 }
                               : UiColor{ 0.08f, 0.08f, 0.08f, 0.9f });
        AddIconQuad(frame, def.icon, anim->x + kCardW * 0.5f, anim->y + 34, 20, 1.0f);
        g.ui.TextCentered(anim->x + kCardW * 0.5f, anim->y + 62, 1.6f,
                          { 1, 1, 1, 0.95f }, def.name);
        if (!afford)
            g.ui.Rect(anim->x, anim->y, kCardW, kCardH, { 0, 0, 0, 0.38f });

        g.drawnCards[g.drawnCardCount++] = { o.id, s, anim->x, anim->y };
    }

    // stale animations = offers that vanished: purchased -> burn from the
    // click point; pushed off the tail -> physical ejection through the hatch
    for (int a = 0; a < 12; ++a)
    {
        App::CardAnim& anim = g.cardAnims[a];
        if (!anim.active || offerSeen[a])
            continue;
        // identify what the card looked like (type unknown now; remember via
        // drawn history isn't kept -- use the burn/eject info stored on click
        // or assume tail eject uses the last snapshot type; approximate with
        // a neutral card when unknown)
        if (anim.id == g.lastClickedOfferId)
        {
            App::ShopBurnFx fx{ anim.x, anim.y, kCardW, kCardH,
                                g.lastClickX, g.lastClickY, g.time,
                                g.lastClickedIcon };
            g.shopBurnFx.push_back(fx);
            g.lastClickedOfferId = 0;
        }
        else if (anim.lastSlot == NumOfferSlots - 1)
        {
            App::EjectFx e{};
            e.icon = g.lastTailIcon;
            e.rarity = g.lastTailRarity;
            e.x = anim.x + kCardW * 0.5f;
            e.y = anim.y + kCardH * 0.5f;
            e.vx = 2300.0f;
            e.vy = -140.0f;
            e.ang = 0;
            e.angVel = 1.1f;
            e.bounced = false;
            g.ejectFx.push_back(e);
            BreakSlats();
        }
        anim.active = false;
    }
    // remember the tail card's look while it still exists (for ejection)
    for (int s = 0; s < NumOfferSlots; ++s)
        if (s == NumOfferSlots - 1 && me.offers[s].active)
        {
            g.lastTailIcon = kUpgradePool[me.offers[s].type].icon;
            g.lastTailRarity = kUpgradePool[me.offers[s].type].rarity;
        }

    // ---- burn effects (purchases) ----
    for (size_t i = 0; i < g.shopBurnFx.size();)
    {
        App::ShopBurnFx& fx = g.shopBurnFx[i];
        float progress = float((g.time - fx.t0) / 0.55);
        if (progress >= 1.0f)
        {
            g.shopBurnFx.erase(g.shopBurnFx.begin() + i);
            continue;
        }
        float dx = std::max(fx.ox - fx.x, fx.x + fx.w - fx.ox);
        float dy = std::max(fx.oy - fx.y, fx.y + fx.h - fx.oy);
        float maxR = sqrtf(dx * dx + dy * dy) + 8.0f;
        UiColor bg = kRarityCol[std::clamp(fx.icon >= 0 ? 2 : 0, 0, 4)];
        UiBurnQuad q{ fx.x, fx.y, fx.w, fx.h, bg.r, bg.g, bg.b, bg.a,
                      fx.ox, fx.oy, progress, maxR };
        frame.uiBurn.push_back(q);
        if (fx.icon >= 0)
        {
            float u0, u1;
            IconUv(fx.icon, u0, u1);
            UiBurnQuad iq{ fx.x + fx.w * 0.5f - 20, fx.y + 14, 40, 40,
                           1, 1, 1, 1, fx.ox, fx.oy, progress, maxR,
                           u0, 0, u1, 1 };
            frame.uiBurn.push_back(iq);
        }
        ++i;
    }

    // ---- ejection physics (overflow) ----
    for (size_t i = 0; i < g.ejectFx.size();)
    {
        App::EjectFx& e = g.ejectFx[i];
        e.vy += 2400.0f * g.frameDt;            // gravity
        e.x += e.vx * g.frameDt;
        e.y += e.vy * g.frameDt;
        e.ang += e.angVel * g.frameDt;
        if (!e.bounced && e.x + kCardW * 0.5f >= float(g.width))
        {
            e.x = float(g.width) - kCardW * 0.5f;
            float impact = e.vx;
            e.vx = -impact * 0.38f;             // punched back, velocity-scaled
            e.vy -= 160.0f;                     // pop upward a little
            e.angVel = -impact * 0.004f;        // spin from the impact
            e.bounced = true;
        }
        if (e.y > float(g.height) + 220.0f)
        {
            g.ejectFx.erase(g.ejectFx.begin() + i);
            continue;
        }
        UiColor bg = kRarityCol[e.rarity];
        RotatedRect(e.x, e.y, kCardW + 4, kCardH + 4, e.ang,
                    { 0.08f, 0.08f, 0.08f, 0.35f });   // shadow first
        RotatedRect(e.x, e.y, kCardW, kCardH, e.ang, bg);
        AddIconQuad(frame, e.icon, e.x, e.y, 22, 1.0f, e.ang);
        ++i;
    }

    // ---- border debris (broken slats) ----
    for (size_t i = 0; i < g.debrisFx.size();)
    {
        App::DebrisFx& d = g.debrisFx[i];
        d.vy += 2000.0f * g.frameDt;
        d.x += d.vx * g.frameDt;
        d.y += d.vy * g.frameDt;
        d.ang += d.angVel * g.frameDt;
        float age = float(g.time - d.t0);
        if (age > 1.4f || d.y > float(g.height) + 60.0f)
        {
            g.debrisFx.erase(g.debrisFx.begin() + i);
            continue;
        }
        float alpha = 1.0f - age / 1.4f;
        RotatedRect(d.x, d.y, d.w, d.h, d.ang, { 0.5f, 0.58f, 0.42f, alpha });
        ++i;
    }

    // ---- hover tooltip: description + cost ----
    if (hoverSlot >= 0 && me.offers[hoverSlot].active)
    {
        const Offer& o = me.offers[hoverSlot];
        const UpgradeType& def = kUpgradePool[o.type];
        float tx = px + panelW + 10;
        float ty = hoverY;
        g.ui.Rect(tx, ty, 260, 92, { 0.05f, 0.06f, 0.05f, 0.94f });
        g.ui.RectOutline(tx, ty, 260, 92, 2, kRarityCol[def.rarity]);
        g.ui.Text(tx + 10, ty + 10, 2.0f, { 1, 1, 1, 1 }, def.name);
        g.ui.Text(tx + 10, ty + 32, 1.5f, { 0.85f, 0.9f, 0.85f, 1 }, def.desc);
        char tip[64];
        sprintf_s(tip, "COST $ %u", unsigned(o.cost));
        bool afford = me.money >= o.cost;
        g.ui.Text(tx + 10, ty + 56, 1.8f,
                  afford ? UiColor{ 0.55f, 1, 0.55f, 1 }
                         : UiColor{ 1, 0.45f, 0.4f, 1 }, tip);
        int copies = 0;   // owned copies are host-side; show rarity instead
        (void)copies;
        static const char* rarityNames[5] = { "COMMON", "UNCOMMON", "RARE",
                                              "EPIC", "LEGENDARY" };
        g.ui.Text(tx + 10, ty + 74, 1.4f, { 1, 1, 1, 0.6f },
                  rarityNames[def.rarity]);
    }
}

void HandleShopClick(float mx, float my)
{
    const PlayerState& me = g.game.players[g.myId];
    for (int i = 0; i < g.drawnCardCount; ++i)
    {
        const App::DrawnCard& c = g.drawnCards[i];
        if (mx < c.x || mx > c.x + kCardW || my < c.y || my > c.y + kCardH)
            continue;
        const Offer& o = me.offers[c.slot];
        if (!o.active || o.id != c.id || me.money < o.cost)
            return;
        if (!g.online || g.isHost)
            g.game.TryPurchase(g.myId, c.slot);
        else
            g.net.SendPurchaseToHost(c.slot);   // optimistic; host validates
        g.lastClickedOfferId = c.id;
        g.lastClickX = mx;
        g.lastClickY = my;
        g.lastClickedIcon = kUpgradePool[o.type].icon;
        return;
    }
}

void BuildHud(FrameData& frame)
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

    // health bar + credits
    float hx = 10, hy = float(g.height) - 46;
    g.ui.Rect(hx, hy, 260, 26, { 0, 0, 0, 0.55f });
    float frac = float(me.health) / float(MaxHealthFor(me));
    g.ui.Rect(hx + 3, hy + 3, 254 * std::max(0.0f, frac), 20,
              frac > 0.5f ? UiColor{ 0.3f, 0.8f, 0.25f, 0.95f }
              : frac > 0.25f ? UiColor{ 0.9f, 0.75f, 0.2f, 0.95f }
                             : UiColor{ 0.9f, 0.2f, 0.15f, 0.95f });
    sprintf_s(buf, "HP %d", me.health);
    g.ui.Text(hx + 8, hy + 6, 2.0f, { 0, 0, 0, 0.9f }, buf);
    sprintf_s(buf, "$ %u", unsigned(me.money));
    g.ui.Rect(hx + 268, hy, 90, 26, { 0, 0, 0, 0.55f });
    g.ui.Text(hx + 276, hy + 6, 2.0f, { 1, 0.92f, 0.5f, 1 }, buf);

    // reload bar
    float rf = 1.0f - me.fireCooldown
                      / std::max(0.01f, me.stats[int(Stat::ReloadTime)]);
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
              "WASD drive   mouse aim   LMB/space fire   TAB shop   ESC menu");
    if (!g.statusLine.empty())
        g.ui.TextCentered(w * 0.5f, float(g.height) - 30, 1.7f, { 1, 0.8f, 0.4f, 0.95f },
                          g.statusLine);

    if (g.shopOpen)
        BuildShop(frame);
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
        if (wp == VK_TAB && g.screen == Screen::InGame)
        {
            if (g.shopOpen) g.shopOpen = false;
            else OpenShop();
        }
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
    ImageData icons = MakeIconAtlas(32, UpgradePoolSize);
    g.texIconAtlas = r->CreateTexture(icons.rgba.data(), icons.width, icons.height);
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
            g.inputs[pid].moveX = m.moveX;
            g.inputs[pid].moveZ = m.moveZ;
            g.inputs[pid].turretYaw = m.turretYaw;
            g.inputSeqs[pid] = m.seq;
        }
    };
    ev.onPurchase = [](int pid, int slot)
    {
        if (g.game.TryPurchase(pid, slot))
            Log("Player %d bought offer slot %d (owned %zu upgrades)", pid, slot,
                g.game.players[pid].owned.size());
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
                m.moveX = local.moveX;
                m.moveZ = local.moveZ;
                m.turretYaw = local.turretYaw;
                m.seq = ++g.inputSeq;
                g.net.SendInputToHost(m);

                // client-side prediction: move our tank immediately with the
                // same integration the host runs; snapshots reconcile later
                PlayerState& me = g.game.players[g.myId];
                if (me.active && me.health > 0)
                {
                    g.pendingInputs.push_back({ g.inputSeq, local });
                    if (g.pendingInputs.size() > 120)
                        g.pendingInputs.erase(g.pendingInputs.begin());
                    g.predPrev = me;
                    g.game.AdvanceMovement(g.myId, local);
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
                tYaw = screenX * 0.055f;      // ~3.2 deg lean into strafe
                tPitch = screenZ * 0.045f;    // ~2.6 deg nose-dip forward
            }
            float kl = 1.0f - expf(-float(dt) * 5.0f);
            float dy = tYaw - g.camYawLean;
            float dp = tPitch - g.camPitchLean;
            g.camYawLean = (fabsf(dy) < 0.0004f) ? tYaw : g.camYawLean + dy * kl;
            g.camPitchLean = (fabsf(dp) < 0.0004f) ? tPitch : g.camPitchLean + dp * kl;
        }
        g.camPos = XMFLOAT3(g.camFocus.x, 19.0f, g.camFocus.z - 14.5f);

        XMMATRIX view = XMMatrixLookAtRH(
            XMVectorSet(g.camPos.x, g.camPos.y, g.camPos.z, 1),
            XMVectorSet(g.camFocus.x, 0, g.camFocus.z + 2.0f, 1),
            XMVectorSet(0, 1, 0, 0));
        // apply the lean in view space (local axes)
        view = view * XMMatrixRotationY(g.camYawLean)
                    * XMMatrixRotationX(g.camPitchLean);
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
            else if (g.shopOpen && g.mouseX >= g.shopPanel[0]
                     && g.mouseX <= g.shopPanel[0] + g.shopPanel[2]
                     && g.mouseY >= g.shopPanel[1]
                     && g.mouseY <= g.shopPanel[1] + g.shopPanel[3])
            {
                HandleShopClick(float(g.mouseX), float(g.mouseY));
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

        // shop test automation: open the shop, buy one card mid-burn, then
        // force-feed offers until the conveyor overflows and ejects the tail
        if (g.opt.shopTest && g.screen == Screen::InGame && g.isHost)
        {
            if (!g.shopOpen && g.time > 1.0)
                OpenShop();
            if (g.shopTestBuyAt == 0 && g.time > 2.4 && g.drawnCardCount > 0)
            {
                g.shopTestBuyAt = g.time;
                const App::DrawnCard& c = g.drawnCards[0];
                HandleShopClick(c.x + kCardW * 0.6f, c.y + kCardH * 0.45f);
            }
            if (g.time > 3.0 && g.shopTestOffersForced < 7
                && g.time > g.shopTestNextOffer)
            {
                g.shopTestNextOffer = g.time + 0.45;
                ++g.shopTestOffersForced;
                g.game.GenerateOffer(g.myId);   // host-side direct feed
            }
        }

        FrameData frame;
        frame.objects.reserve(64);
        frame.camRight = camRight;
        frame.camUp = camUp;
        frame.uiTexTexture = g.texIconAtlas;
        BuildScene(frame, view, proj);
        g.ui.Reset(g.width, g.height);
        if (g.screen == Screen::InGame)
            BuildHud(frame);
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
