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
#include "Sound.h"

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
    int lagComp = -1;
    bool boom = false;                                  // periodic test explosions
    bool fullscreen = false;
    bool rich = false;                                  // start with 500 credits
    bool shopTest = false;                              // auto-open + auto-buy (testing)
    int pauseTest = 0;           // 1 = force-pause at frame 200, 2 = + settings
    bool quickMatch = false;     // auto-click FIND MATCH at startup (testing)
    int quickMatchNeed = 2;      // queue size for --quickmatch
    int readyTest = 0;           // toggle ready once at this frame (testing)
};

const struct { int w, h; } kResolutions[] = {
    { 1280, 720 }, { 1600, 900 }, { 1920, 1080 }, { 2560, 1440 }
};

enum class Screen { MainMenu, JoinEntry, Connecting, InGame, Settings, Paused,
                    MatchSize };

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
    // hover-sound edge detection: a stable id for whatever clickable thing the
    // mouse is over this frame (0 = nothing); play a blip when it changes
    int hoverKeyNow = 0;
    int hoverKeyPrev = 0;
    // hull angular velocity for the rotation sound layer
    float sndPrevHullYaw = 0;
    bool sndHullYawValid = false;
    // quick match state
    bool searching = false;              // lobby search in flight
    int searchNeed = 2;                  // chosen queue size (FIND MATCH)
    int lastAdvertPlayers = -1;          // last values pushed to the Steam
    int lastAdvertPhase = -1;            //  lobby advert (push on change only)
    float mmToggleRect[4]{};             // MATCHMAKING ON/OFF button (host)
    Screen screen = Screen::MainMenu;
    Screen settingsReturn = Screen::MainMenu;   // where BACK leads
    bool sessionActive = false;  // a game session exists (pause overlays it)
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
    struct CardAnim { uint8_t id = 0; float x = 0, y = 0; int lastSlot = 0;
                      bool active = false; bool burned = false; };
    CardAnim cardAnims[12]{};
    struct ShopBurnFx { float x, y, w, h; float ox, oy; double t0; int icon; int rarity; };
    std::vector<ShopBurnFx> shopBurnFx;
    struct EjectFx { int icon, rarity; float x, y, vx, vy, ang, angVel; bool bounced; };
    std::vector<EjectFx> ejectFx;
    struct DebrisFx { float x, y, w, h; float vx, vy, ang, angVel; double t0; };
    std::vector<DebrisFx> debrisFx;
    bool slatsBroken = false;   // restored only when the shop is reopened
    float shopPanel[4]{};                   // x, y, w, h
    struct DrawnCard { uint8_t id; int slot; float x, y; };
    DrawnCard drawnCards[NumOfferSlots]{};
    int drawnCardCount = 0;
    uint8_t lastClickedOfferId = 0;
    float lastClickX = 0, lastClickY = 0;
    int lastClickedIcon = -1;
    int lastClickedRarity = 0;
    float readyRect[4]{};
    uint8_t prevPhase = PhaseLobby;

    // lag compensation (toggleable): server input catch-up + client-side
    // extrapolation of remote tanks with a 10 ms correction lerp
    bool lagComp = true;
    struct RemoteDisplay { float x, z, hullYaw, turretYaw; bool valid; };
    RemoteDisplay remoteDisplay[MaxPlayers]{};
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
static const UiColor kPlayerUiCol[MaxLobbyPlayers] = {
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
    g.game.phase = s.phase;
    g.game.winner = s.winner;
    g.game.targetPlayers = s.targetPlayers;
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
            p = PlayerState{}, p.active = true;
        memcpy(p.name, in.name, sizeof(p.name));
        p.name[15] = 0;
        p.ready = in.ready;
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
    s.phase = g.game.phase;
    s.winner = g.game.winner;
    s.targetPlayers = g.game.targetPlayers;
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
            SpawnExplosion(g.prevProjPos[i]);
        if (!g.prevProjActive[i] && pr.active && InSession())
            snd::Play(snd::Sfx::Shoot, SndDistVol(pr.x, pr.z, 0.6f),
                      SndJitter(0.07f));
        g.prevProjActive[i] = pr.active;
        if (pr.active)
            g.prevProjPos[i] = XMFLOAT3(pr.x, pr.y, pr.z);
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
        XMFLOAT4 tint = kPlayerTint[i % MaxLobbyPlayers];   // per-player identity
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
    // fresh entrance: existing offers slide in from off-screen again,
    // and the broken exit hatch is whole again
    for (auto& a : g.cardAnims)
        a.active = false;
    g.slatsBroken = false;
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
    if (!g.slatsBroken)
    {
        // contiguous segments: together they form one whole border line,
        // but they break apart into individual pieces on ejection
        float span = hatchBottom - hatchTop;
        for (int s = 0; s < kNumSlats; ++s)
            g.ui.Rect(px + pw - 2, hatchTop + span * float(s) / kNumSlats,
                      2, span / kNumSlats, frameCol);
    }
}

void BreakSlats()
{
    if (g.slatsBroken)
        return;   // already open: the card flies through the existing hole
    g.slatsBroken = true;
    snd::Play(snd::Sfx::Glass, 0.7f, SndJitter(0.10f));
    float px = g.shopPanel[0], pw = g.shopPanel[2], py = g.shopPanel[1],
          ph = g.shopPanel[3];
    float hatchTop = py + kShopHeader + kCardGap + 2 * (kCardH + kCardGap);
    float span = (py + ph - 2) - hatchTop;
    for (int s = 0; s < kNumSlats; ++s)
    {
        App::DebrisFx d{};
        d.x = px + pw - 1;
        d.y = hatchTop + span * (s + 0.5f) / kNumSlats;
        d.w = 3;
        d.h = span / kNumSlats;
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

        // consumed = purchased, burn playing while the slot is held: spawn the
        // fx once at this card's settled position, then draw nothing normal
        if (o.active == OfferConsumed)
        {
            if (!anim->burned)
            {
                anim->burned = true;
                bool mine = (o.id == g.lastClickedOfferId);
                App::ShopBurnFx fx{ anim->x, anim->y, kCardW, kCardH,
                                    mine ? g.lastClickX : anim->x + kCardW * 0.5f,
                                    mine ? g.lastClickY : anim->y + kCardH * 0.5f,
                                    g.time, kUpgradePool[o.type].icon,
                                    kUpgradePool[o.type].rarity };
                g.shopBurnFx.push_back(fx);
                snd::Play(snd::Sfx::Burn, 0.55f, SndJitter(0.10f));
                if (mine)
                    g.lastClickedOfferId = 0;
            }
            continue;
        }
        anim->burned = false;

        UiColor bg = kRarityCol[def.rarity];
        bool afford = me.money >= o.cost;
        bool hover = float(g.mouseX) >= anim->x && float(g.mouseX) <= anim->x + kCardW
                  && float(g.mouseY) >= anim->y && float(g.mouseY) <= anim->y + kCardH;
        if (hover)
        {
            hoverSlot = s; hoverX = anim->x; hoverY = anim->y;
            g.hoverKeyNow = 1000 + o.id;
        }

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
        if (anim.burned)
        {
            anim.active = false;   // burn already played; host compacted
            continue;
        }
        if (anim.id == g.lastClickedOfferId)
        {
            // client fallback: consumed state never arrived (edge case)
            App::ShopBurnFx fx{ anim.x, anim.y, kCardW, kCardH,
                                g.lastClickX, g.lastClickY, g.time,
                                g.lastClickedIcon, g.lastClickedRarity };
            g.shopBurnFx.push_back(fx);
            snd::Play(snd::Sfx::Burn, 0.55f, SndJitter(0.10f));
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
    if (me.offers[NumOfferSlots - 1].active == OfferActive)
    {
        g.lastTailIcon = kUpgradePool[me.offers[NumOfferSlots - 1].type].icon;
        g.lastTailRarity = kUpgradePool[me.offers[NumOfferSlots - 1].type].rarity;
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
        UiColor bg = kRarityCol[std::clamp(fx.rarity, 0, 4)];
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
        snd::Play(snd::Sfx::Click, 0.45f, SndJitter(0.06f));
        if (o.active != OfferActive || o.id != c.id || me.money < o.cost)
            return;
        // Capture the card's identity BEFORE purchasing: TryPurchase compacts
        // the offers array in place, so `o` would afterwards alias the next
        // card that shifted into this slot (the icon-swap bug).
        g.lastClickedOfferId = o.id;
        g.lastClickedIcon = kUpgradePool[o.type].icon;
        g.lastClickedRarity = kUpgradePool[o.type].rarity;
        g.lastClickX = mx;
        g.lastClickY = my;
        if (!g.online || g.isHost)
            g.game.TryPurchase(g.myId, c.slot);
        else
            g.net.SendPurchaseToHost(c.slot);   // optimistic; host validates
        return;
    }
}

// Score squares arranged on a circle (2pi / player count). The layout
// degenerates nicely: 2 players = left/right, 3 = triangle, 4 = rotated
// square; the match timer sits in the circle's center.
void DrawScoreCircle()
{
    int ids[MaxLobbyPlayers];
    int n = 0;
    for (int i = 0; i < MaxPlayers && n < MaxLobbyPlayers; ++i)
        if (g.game.players[i].active)
            ids[n++] = i;
    if (n == 0)
        return;
    float cx = float(g.width) * 0.5f, cy = 86.0f;
    float radius = 60.0f;
    char buf[32];

    for (int k = 0; k < n; ++k)
    {
        float ang = (n == 1) ? XM_PI
                  : (n == 2) ? (k == 0 ? XM_PI : 0.0f)
                             : (-XM_PI * 0.5f + XM_2PI * float(k) / float(n));
        float x = cx + cosf(ang) * radius;
        float y = cy + sinf(ang) * radius;
        const PlayerState& p = g.game.players[ids[k]];
        UiColor col = kPlayerUiCol[ids[k] % MaxLobbyPlayers];
        g.ui.Rect(x - 21, y - 21, 42, 42, { col.r * 0.55f, col.g * 0.55f,
                                            col.b * 0.55f, 0.92f });
        g.ui.RectOutline(x - 21, y - 21, 42, 42, 2,
                         ids[k] == g.myId ? UiColor{ 1, 1, 1, 1 } : col);
        sprintf_s(buf, "%u", unsigned(p.score));
        g.ui.TextCentered(x, y - 9, 2.6f, { 1, 1, 1, 1 }, buf);
        std::string nm(p.name);
        if (nm.size() > 10) nm.resize(10);
        g.ui.TextCentered(x, y + 26, 1.3f, { 1, 1, 1, 0.75f }, nm);
    }

    if (g.game.phase == PhaseOvertime)
    {
        float pulse = 0.6f + 0.4f * sinf(float(g.time) * 6.0f);
        g.ui.TextCentered(cx, cy - 10, 2.8f, { 1, 0.35f, 0.3f, pulse }, "OT");
        g.ui.TextCentered(cx, cy + 14, 1.3f, { 1, 0.6f, 0.5f, 0.9f }, "FIRST KILL WINS");
    }
    else if (g.game.phase == PhasePlaying)
    {
        uint32_t left = g.game.matchEndTick > g.game.tick
                            ? g.game.matchEndTick - g.game.tick : 0;
        int secs = int(left / TickRate);
        sprintf_s(buf, "%d:%02d", secs / 60, secs % 60);
        g.ui.TextCentered(cx, cy - 10, 2.6f,
                          secs <= 30 ? UiColor{ 1, 0.5f, 0.4f, 1 }
                                     : UiColor{ 1, 1, 1, 0.95f }, buf);
    }
}

// Host-only MATCHMAKING ON/OFF toggle: is this game advertised to searchers?
void DrawMatchmakingToggle(float x, float y, float bw, float bh)
{
    UiButton btn{ x, y, bw, bh,
                  g.net.hasPublicLobby() ? "MATCHMAKING: ON"
                                         : "MATCHMAKING: OFF" };
    g.mmToggleRect[0] = btn.x; g.mmToggleRect[1] = btn.y;
    g.mmToggleRect[2] = btn.w; g.mmToggleRect[3] = btn.h;
    bool hov = btn.Contains(float(g.mouseX), float(g.mouseY));
    if (hov)
        g.hoverKeyNow = 3003;
    DrawButton(g.ui, btn, hov);
}

// Quick-match queue: the ready-up lobby stays hidden until the queue fills.
void BuildGatheringUi()
{
    float w = float(g.width), h = float(g.height);
    g.ui.Rect(0, 0, w, h, { 0.05f, 0.07f, 0.05f, 0.45f });

    int active = 0;
    for (int i = 0; i < MaxPlayers; ++i)
        if (g.game.players[i].active)
            ++active;
    int need = g.game.targetPlayers > 0 ? g.game.targetPlayers
                                        : int(MaxLobbyPlayers);
    char buf[96];
    int dots = 1 + int(fmod(g.time, 3.0));
    sprintf_s(buf, "FINDING PLAYERS  %d / %d %.*s", active, need, dots, "...");
    g.ui.TextCentered(w * 0.5f, h * 0.34f, 3.2f, { 0.9f, 1, 0.85f, 1 }, buf);
    g.ui.TextCentered(w * 0.5f, h * 0.34f + 44, 1.7f, { 1, 1, 1, 0.6f },
                      "the lobby opens when everyone is here");

    if (g.isHost && g.online)
    {
        DrawMatchmakingToggle(w * 0.5f - 140, h * 0.52f, 280, 50);
        g.ui.TextCentered(w * 0.5f, h * 0.52f + 60, 1.4f, { 1, 1, 1, 0.5f },
                          "matchmaking off = invite-only (share the game code)");
    }
    g.ui.TextCentered(w * 0.5f, h - 40, 1.5f, { 1, 1, 1, 0.5f },
                      "ESC  menu");
}

void BuildLobbyUi(FrameData& frame)
{
    float w = float(g.width), h = float(g.height);
    char buf[96];
    int active = 0, ready = 0;
    for (int i = 0; i < MaxPlayers; ++i)
        if (g.game.players[i].active)
        {
            ++active;
            if (g.game.players[i].ready) ++ready;
        }
    int cap = g.game.targetPlayers > 0 ? g.game.targetPlayers
                                       : int(MaxLobbyPlayers);
    sprintf_s(buf, "LOBBY  %d / %d PLAYERS   %d READY", active, cap, ready);
    g.ui.TextCentered(w * 0.5f, 84, 2.4f, { 0.9f, 1, 0.85f, 1 }, buf);
    if (g.net.hasPublicLobby())
        g.ui.TextCentered(w * 0.5f, 112, 1.5f, { 0.7f, 0.95f, 1, 0.8f },
                          "PUBLIC MATCH - players can find this game");

    // name + ready tag floating above each tank in the lineup
    for (int i = 0; i < MaxPlayers; ++i)
    {
        const PlayerState& p = g.game.players[i];
        if (!p.active)
            continue;
        float lx, lz, lyaw;
        LobbySpot(i, lx, lz, lyaw);
        float sx, sy;
        if (!WorldToScreen(XMFLOAT3(lx, 3.4f, lz), frame.viewProj, sx, sy))
            continue;
        UiColor col = kPlayerUiCol[i % MaxLobbyPlayers];
        std::string nm(p.name);
        if (nm.size() > 12) nm.resize(12);
        float tw = g.ui.TextWidth(1.9f, nm) + 20;
        g.ui.Rect(sx - tw * 0.5f, sy - 14, tw, 26, { 0, 0, 0, 0.55f });
        g.ui.TextCentered(sx, sy - 8, 1.9f, col, nm);
        g.ui.TextCentered(sx, sy + 18, 1.5f,
                          p.ready ? UiColor{ 0.5f, 1, 0.5f, 1 }
                                  : UiColor{ 1, 1, 1, 0.45f },
                          p.ready ? "READY" : "NOT READY");
        if (i == g.myId)
            g.ui.TextCentered(sx, sy + 36, 1.3f, { 1, 1, 1, 0.5f }, "(you)");
    }

    // ready button
    const PlayerState& me = g.game.players[g.myId];
    UiButton btn{ w * 0.5f - 140, h - 128, 280, 56,
                  me.ready ? "UNREADY" : "READY UP" };
    g.readyRect[0] = btn.x; g.readyRect[1] = btn.y;
    g.readyRect[2] = btn.w; g.readyRect[3] = btn.h;
    bool readyHov = btn.Contains(float(g.mouseX), float(g.mouseY));
    if (readyHov)
        g.hoverKeyNow = 3001;
    DrawButton(g.ui, btn, readyHov);
    if (g.isHost && g.online)
        DrawMatchmakingToggle(w * 0.5f + 160, h - 128, 260, 56);
    g.ui.TextCentered(w * 0.5f, h - 62, 1.4f, { 1, 1, 1, 0.5f },
                      "R toggles ready - match starts when everyone is ready");
}

void BuildHud(FrameData& frame)
{
    const PlayerState& me = g.game.players[g.myId];
    float w = float(g.width);
    g.mmToggleRect[2] = 0;   // only clickable on frames where it's drawn

    char buf[256];
    sprintf_s(buf, "%s  |  %s  |  %.0f FPS", g.renderer->Name(), g.gpu.name.c_str(), g.fps);
    g.ui.Text(10, 10, 1.6f, { 1, 1, 1, 0.75f }, buf);
    if (g.online && !g.isHost)
    {
        int ping = 0;
        std::string route;
        if (g.net.clientConnectionStatus(ping, route))
        {
            sprintf_s(buf, "PING %d ms   %s", ping, route.c_str());
            g.ui.Text(10, 46, 1.4f, { 0.75f, 0.95f, 0.75f, 0.8f }, buf);
        }
    }
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
        if (hover)
            g.hoverKeyNow = 3002;
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

    if (g.game.phase == PhaseGathering)
    {
        BuildGatheringUi();
        if (!g.statusLine.empty())
            g.ui.TextCentered(w * 0.5f, float(g.height) - 66, 1.7f,
                              { 1, 0.8f, 0.4f, 0.95f }, g.statusLine);
        return;
    }
    if (g.game.phase == PhaseLobby)
    {
        BuildLobbyUi(frame);
        if (!g.statusLine.empty())
            g.ui.TextCentered(w * 0.5f, float(g.height) - 30, 1.7f,
                              { 1, 0.8f, 0.4f, 0.95f }, g.statusLine);
        return;
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

    // kill scores on the circle + match timer in its center
    DrawScoreCircle();

    if (g.game.phase == PhaseEnded)
    {
        const char* winName = (g.game.winner < MaxPlayers)
                                  ? g.game.players[g.game.winner].name : "?";
        if (g.game.winner == g.myId)
            sprintf_s(buf, "YOU WIN");
        else
            sprintf_s(buf, "%s WINS", winName);
        g.ui.TextCentered(w * 0.5f, g.height * 0.36f, 5.0f,
                          { 1, 0.95f, 0.55f, 1 }, buf);
        g.ui.TextCentered(w * 0.5f, g.height * 0.36f + 52, 1.7f,
                          { 1, 1, 1, 0.7f }, "returning to lobby...");
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
        if (g.game.phase == PhaseLobby || g.game.phase == PhaseGathering)
        {
            float x, z, yaw;
            LobbySpot(pid, x, z, yaw);
            g.game.players[pid].x = x;
            g.game.players[pid].z = z;
            g.game.players[pid].hullYaw = g.game.players[pid].turretYaw = yaw;
        }
    };
    ev.onPlayerLeft = [](int pid) { g.game.RemovePlayer(pid); };
    ev.onReady = [](int pid, bool ready)
    {
        Log("Host: ready message from player %d -> %d (phase=%d)",
            pid, ready ? 1 : 0, int(g.game.phase));
        if (g.game.phase == PhaseLobby)
            g.game.players[pid].ready = ready ? 1 : 0;
    };
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

        g.net.Poll(ev);

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
                if (!playing)
                {
                    g.pendingInputs.clear();
                }
                else if (me.active && me.health > 0)
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
        if (lobbyCam)
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
            if (g.prevPhase != PhasePlaying && ph == PhasePlaying
                && g.isHost && (g.opt.rich || g.opt.shopTest))
                g.game.players[g.myId].money = 500;
            if ((g.opt.shopTest || g.opt.autoDrive) && g.screen == Screen::InGame
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

        // one-shot clicks: menus, or the copyable game code in the HUD
        if (g.clicked)
        {
            g.clicked = false;
            if (g.screen != Screen::InGame)
            {
                HandleMenuClick();
            }
            else if (g.isHost && g.online && g.mmToggleRect[2] > 0
                     && g.mouseX >= g.mmToggleRect[0]
                     && g.mouseX <= g.mmToggleRect[0] + g.mmToggleRect[2]
                     && g.mouseY >= g.mmToggleRect[1]
                     && g.mouseY <= g.mmToggleRect[1] + g.mmToggleRect[3])
            {
                // matchmaking toggle: is this game advertised to searchers?
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
            else if (g.game.phase == PhaseLobby
                     && g.mouseX >= g.readyRect[0]
                     && g.mouseX <= g.readyRect[0] + g.readyRect[2]
                     && g.mouseY >= g.readyRect[1]
                     && g.mouseY <= g.readyRect[1] + g.readyRect[3])
            {
                ToggleReady();
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
                snd::Play(snd::Sfx::Click, 0.5f, SndJitter(0.06f));
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
        }

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

    snd::Shutdown();
    g.net.Shutdown();
    delete g.renderer;
    Log("Tankaq exiting cleanly");
    return 0;
}
