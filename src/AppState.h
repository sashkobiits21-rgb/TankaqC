#pragma once
// Shared application state + cross-module UI declarations. Main.cpp owns the
// frame loop, window, prediction and session glue; UiShop.cpp / UiHud.cpp
// build the in-game UI; Tests.cpp holds the headless self-tests. Everything
// hangs off the single `g` App instance.
#include <windows.h>
#include <string>
#include <vector>
#include <deque>
#include <DirectXMath.h>

#include "Game.h"
#include "GpuDetect.h"
#include "AssetLoad.h"
#include "Ui.h"
#include "Anim.h"
#include "render/IRenderer.h"
#include "net/Net.h"

namespace tankaq
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
    bool autoReady = false;      // lobby: ready up automatically (loopback QA)
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
    bool classTest = false;                             // headless class-rule test
    int pauseTest = 0;           // 1 = force-pause at frame 200, 2 = + settings
    bool quickMatch = false;     // auto-click FIND MATCH at startup (testing)
    int quickMatchNeed = 2;      // queue size for --quickmatch
    int readyTest = 0;           // toggle ready once at this frame (testing)
    bool rigTest = false;        // show the animated test rig in the arena
    bool soldierTest = false;    // solo: grant SOLDIER class + dummy target
    std::string demoClass;       // --demo=necro|radar: grant + dummy (solo)
};

constexpr struct { int w, h; } kResolutions[] = {
    { 1280, 720 }, { 1600, 900 }, { 1920, 1080 }, { 2560, 1440 }
};

enum class Screen { MainMenu, JoinEntry, Connecting, InGame, Settings, Paused,
                    MatchMode, MatchSize };

// ---------------------------------------------------------------- click ids
// Every clickable overlay registers its rect each frame (UiHotRect); ONE
// dispatcher routes clicks and the fire input is suppressed while the cursor
// is over any registered rect. Adding a clickable = register it and handle
// its id -- no per-widget rect fields, no separate fire-suppression list.
enum : int
{
    UiIdNone = 0,
    UiIdReady,        // lobby READY UP button
    UiIdMmToggle,     // host MATCHMAKING ON/OFF
    UiIdShopPanel,    // whole supply-line panel (cards resolved inside)
    UiIdOwnedArrow,   // owned-strip collapse arrow
    UiIdCode,         // click-to-copy game code banner
    UiIdMatchLen,     // host lobby MATCH: N MIN cycle button
};

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
    // NRA material maps (normal rgb + roughness a)
    int texFlatNRA = -1, texWallNRA = -1, texGroundNRA = -1;
    // skinned test rig (--rigtest; the same path future rigged units use)
    SkinnedModel rigModel;
    Animator rigAnimator;
    std::vector<int> meshRigParts;          // one GPU mesh per skinned part
    std::vector<DirectX::XMFLOAT4> rigPartColors;   // material color per part
    std::vector<bool> rigPartVisible;       // weapon toggling
    int texRig = -1;
    float rigScale = 1.0f;
    DirectX::XMFLOAT3 rigPos{ 1.2f, 0.0f, -13.5f };   // near the menu cam

    // soldier summons: clip handles resolved once at load (loud on miss),
    // one Animator per sim slot driven from the replicated state, and a
    // muzzle edge-detector for the fire sound
    struct SoldierClips { int idle = -1, run = -1, duck = -1, shoot = -1,
                          death = -1; };
    SoldierClips soldierClips;
    Animator soldierAnim[MaxSoldiers];
    uint8_t soldierAnimState[MaxSoldiers]{};   // last state fed to the animator
    bool soldierPrevMuzzle[MaxSoldiers]{};

    // necromancer + radar visuals
    int meshSkull = -1, meshJaw = -1, meshPuddle = -1, meshRing = -1;
    int meshGhost = -1, meshWedge = -1;
    // grenades (FRAG PACK) + the launcher prop welded to the soldier's hand
    int meshGrenade = -1;
    int meshShieldSlat = -1, meshShieldPost = -1;   // barrier lattice
    float grenadeScale = 1.0f;
    std::vector<std::pair<int, int>> launcherParts;   // mesh, texture
    float launcherScale = 1.0f;                // canonical -> model units
    int rigChest = -1;                         // launcher shoulder anchor
    DirectX::XMFLOAT4X4 rigChestBind;          // inverse of its inverseBind
    int rigArmR[3]{ -1, -1, -1 };              // UpperArm/LowerArm/Wrist .R
    // rigged skull (assets/Skull/Skull.glb): 3-bone jaw, one looping
    // OpenAndClose clip; falls back to the procedural cranium+jaw if absent
    SkinnedModel skullModel;
    std::vector<int> meshSkullParts;
    float skullScale = 1.0f;
    int texSkull = -1;                         // base color (needs UVs)
    int texSkullNRA = -1;                      // normal+roughness (needs UVs)
    Animator skullAnim[MaxSkulls];             // staggered chomp per slot
    bool prevSkullActive[MaxSkulls]{};         // burst on skull death
    DirectX::XMFLOAT3 prevSkullPos[MaxSkulls]{};
    bool prevGrenadeActive[MaxGrenades]{};     // boom on grenade fuse-out
    DirectX::XMFLOAT3 prevGrenadePos[MaxGrenades]{};
    int prevPlayerHealth[MaxPlayers]{};        // TERRORIST death detection
    std::vector<std::pair<DirectX::XMFLOAT3, double>> shockwaves;
    float prevProjRadar[MaxProjectiles]{};     // per-circle explosion VFX
    float prevProjYaw[MaxProjectiles]{};       // tree layout at death time
    uint8_t prevProjRings[MaxProjectiles]{};
    bool prevPossessed = false;                // possession-start sound

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
    // frame spike diagnostics
    bool dbgSnapThisFrame = false;       // a snapshot applied this frame
    // per-slot rocket birth records (drive the squish/spring shader)
    DirectX::XMFLOAT3 projSpawnPos[MaxProjectiles]{};
    double projSpawnTime[MaxProjectiles]{};
    // client-predicted firing: provisional rockets spawn the moment fire is
    // pressed; when the server's rocket shows up in a snapshot it adopts the
    // provisional's spawn record and a decaying render offset hides the seam
    struct ProvRocket
    {
        Projectile sim;         // flight state stepped by StepProjectile --
                                // the same rules as the host, by construction
        int matchedSlot = -1;   // confirmed server twin (veiled while we live)
        double born = 0;
        DirectX::XMFLOAT3 spawn{};
    };
    ProvRocket provRockets[8];
    float predCooldown = 0;                    // client-side reload prediction
    // Own rockets render 100% locally for their whole flight: the confirmed
    // server twin is VEILED (never drawn) and only supplies authority -- when
    // it dies, the provisional dies and the explosion plays. No corrections.
    int projVeiledBy[MaxProjectiles];          // provisional index or -1
    bool projMatchedProv[MaxProjectiles]{};    // skip double sound/record
    // quick match state
    bool searching = false;              // lobby search in flight
    int searchNeed = 2;
    int searchTest = 0;           // FIND MATCH mode pick: 0 normal, 1 TEST                  // chosen queue size (FIND MATCH)
    int lastAdvertPlayers = -1;          // last values pushed to the Steam
    int lastAdvertPhase = -1;            //  lobby advert (push on change only)
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
    uint32_t inputSeqs[MaxPlayers]{};       // host: last input seq SIMULATED
    // host: per-client input jitter buffer -- exactly one input is consumed
    // per tick, in order, so no client input is ever duplicated or skipped
    // by arrival-timing jitter (that was a steady source of prediction drift)
    std::deque<net::MsgInput> inputQueue[MaxPlayers];
    // client: reconciliation corrections become a render-space offset that
    // decays over ~80 ms instead of snapping the visual (freeze + teleport)
    float predErrX = 0, predErrZ = 0, predErrYaw = 0;

    // game-code click-to-copy feedback
    double codeCopiedAt = -10;

    // vfx
    struct Burst { DirectX::XMFLOAT3 pos; double t0; };
    struct Scorch { DirectX::XMFLOAT3 pos; double t0; };
    std::vector<Burst> bursts;
    std::vector<Scorch> scorches;
    bool prevProjActive[MaxProjectiles]{};
    DirectX::XMFLOAT3 prevProjPos[MaxProjectiles]{};
    double lastBoom = 0;
    DirectX::XMFLOAT3 camPos{ 0, 18, -16 };
    DirectX::XMFLOAT3 camFocus{ 0, 0, 0 };
    bool camFocusValid = false;
    float frameDt = 0;
    double time = 0;

    // upgrade shop (supply line conveyor)
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
    float shopPanel[4]{};                   // x, y, w, h (card layout origin)
    struct DrawnCard { uint8_t id; int slot; float x, y; };
    DrawnCard drawnCards[NumOfferSlots]{};
    int drawnCardCount = 0;
    uint8_t lastClickedOfferId = 0;
    float lastClickX = 0, lastClickY = 0;
    int lastClickedIcon = -1;
    int lastClickedRarity = 0;
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

    // owned-items strip (right edge): half-transparent, collapsible
    bool ownedRowHidden = false;

    // clickable-rect registry (see the UiId enum above); rebuilt every frame
    struct UiHot { int id; float x, y, w, h; };
    UiHot uiHot[16];
    int uiHotCount = 0;

    // camera lean (movement game feel)
    float camYawLean = 0, camPitchLean = 0;
    uint64_t frameCounter = 0;
    float fps = 0;

    UiBuilder ui;
};

extern App g;

// ------------------------------------------------------- clickable registry
inline void UiHotRect(int id, float x, float y, float w, float h)
{
    if (g.uiHotCount < 16)
        g.uiHot[g.uiHotCount++] = { id, x, y, w, h };
}

// Topmost (= last registered) rect under the point, UiIdNone if none.
inline int UiHotHit(float mx, float my)
{
    for (int i = g.uiHotCount - 1; i >= 0; --i)
    {
        const App::UiHot& r = g.uiHot[i];
        if (mx >= r.x && mx <= r.x + r.w && my >= r.y && my <= r.y + r.h)
            return r.id;
    }
    return UiIdNone;
}

inline bool UiHotContains(float mx, float my)
{
    return UiHotHit(mx, my) != UiIdNone;
}

// ------------------------------------------------------------ shared layout
constexpr float kCardW = 150, kCardH = 96, kCardGap = 10, kShopHeader = 34;
constexpr int kNumSlats = 4;
extern const UiColor kRarityCol[7];   // + UNIQUE gold               // indexed by UpgradeType::rarity
extern const UiColor kPlayerUiCol[MaxLobbyPlayers];

// ------------------------------------------------------- cross-module calls
// state / session (Main.cpp)
bool InSession();
float SndJitter(float spread);
float SndDistVol(float x, float z, float base);
bool WorldToScreen(const DirectX::XMFLOAT3& wp,
                   const DirectX::XMFLOAT4X4& viewProj, float& sx, float& sy);
void ToggleReady();
bool HostPurchase(int pid, int slot);
void CopyTextToClipboard(const std::string& text);

// supply line + owned strip (UiShop.cpp)
void OpenShop();
void BuildShop(FrameData& frame);
void BuildOwnedRow(FrameData& frame);
void HandleShopClick(float mx, float my);
void RotatedRect(float cx, float cy, float w, float h, float ang, UiColor c);
void RequestTestGrant(uint8_t upgrade);   // TEST mode: 1 click = 1 copy
void IconUv(int icon, float& u0, float& u1);
void AddIconQuad(FrameData& frame, int icon, float cx, float cy, float half,
                 float alpha, float ang = 0.0f);
void SlotCell(int slot, float& x, float& y);

// HUD / lobby (UiHud.cpp)
void BuildHud(FrameData& frame);

// headless self-tests (Tests.cpp)
int RunClassTest();

} // namespace tankaq
