// In-game HUD: top bars, score circle, lobby/gathering overlays. Clickables
// register into the UiHot registry; dispatch lives in Main.cpp.
#include "AppState.h"
#include "Sound.h"
#include <cstdio>
#include <cmath>
#include <algorithm>

using namespace DirectX;

namespace tankaq
{

// Score squares arranged on a circle (2pi / player count). The layout
// degenerates nicely: 2 players = left/right, 3 = triangle, 4 = rotated
// square; the match timer sits in the circle's center.
static void DrawScoreCircle()
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
static void DrawMatchmakingToggle(float x, float y, float bw, float bh)
{
    UiButton btn{ x, y, bw, bh,
                  g.net.hasPublicLobby() ? "MATCHMAKING: ON"
                                         : "MATCHMAKING: OFF" };
    UiHotRect(UiIdMmToggle, btn.x, btn.y, btn.w, btn.h);
    bool hov = btn.Contains(float(g.mouseX), float(g.mouseY));
    if (hov)
        g.hoverKeyNow = 3003;
    DrawButton(g.ui, btn, hov);
}

// Quick-match queue: the ready-up lobby stays hidden until the queue fills.
static void BuildGatheringUi()
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

static void BuildLobbyUi(FrameData& frame)
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
    UiHotRect(UiIdReady, btn.x, btn.y, btn.w, btn.h);
    bool readyHov = btn.Contains(float(g.mouseX), float(g.mouseY));
    if (readyHov)
        g.hoverKeyNow = 3001;
    DrawButton(g.ui, btn, readyHov);
    if (g.isHost && g.online)
        DrawMatchmakingToggle(w * 0.5f + 160, h - 128, 260, 56);

    // match length: the host cycles 5/10/15/20; everyone sees the pick
    sprintf_s(buf, "MATCH: %d MIN", int(g.game.matchMinutes));
    if (g.isHost)
    {
        UiButton lenBtn{ w * 0.5f - 420, h - 128, 260, 56, buf };
        UiHotRect(UiIdMatchLen, lenBtn.x, lenBtn.y, lenBtn.w, lenBtn.h);
        bool lenHov = lenBtn.Contains(float(g.mouseX), float(g.mouseY));
        if (lenHov)
            g.hoverKeyNow = 3004;
        DrawButton(g.ui, lenBtn, lenHov);
    }
    else
        g.ui.TextCentered(w * 0.5f, 138, 1.6f, { 1, 1, 1, 0.7f }, buf);
    g.ui.TextCentered(w * 0.5f, h - 62, 1.4f, { 1, 1, 1, 0.5f },
                      "R toggles ready - match starts when everyone is ready");
}

void BuildHud(FrameData& frame)
{
    const PlayerState& me = g.game.players[g.myId];
    float w = float(g.width);

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
        float cr[4] = { w * 0.5f - tw * 0.5f - 12, 6, tw + 24, 28 };
        UiHotRect(UiIdCode, cr[0], cr[1], cr[2], cr[3]);
        bool hover = float(g.mouseX) >= cr[0] && float(g.mouseX) <= cr[0] + cr[2]
                  && float(g.mouseY) >= cr[1] && float(g.mouseY) <= cr[1] + cr[3];
        if (hover)
            g.hoverKeyNow = 3002;
        g.ui.Rect(cr[0], cr[1], cr[2], cr[3],
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

    // POSSESSED: the ghost is driving -- wash the screen blue and pulse
    if (me.possessTimer > 0.0f)
    {
        float pulse = 0.5f + 0.5f * sinf(float(g.time) * 9.0f);
        g.ui.Rect(0, 0, w, float(g.height),
                  { 0.25f, 0.45f, 1.0f, 0.20f + 0.08f * pulse });
        g.ui.RectOutline(6, 6, w - 12, float(g.height) - 12, 4,
                         { 0.45f, 0.65f, 1.0f, 0.30f + 0.25f * pulse });
        g.ui.TextCentered(w * 0.5f, g.height * 0.30f, 3.2f,
                          { 0.78f, 0.88f, 1.0f, 0.55f + 0.3f * pulse },
                          "POSSESSED");
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

    // fuel bar (SHIFT boost): orange while available, dimmed during the
    // regen delay; uses the locally predicted fuel so it reacts instantly
    {
        float cap = std::max(me.stats[int(Stat::BoostFuel)], 0.01f);
        float ff = std::clamp(me.boostFuel / cap, 0.0f, 1.0f);
        bool waiting = me.boostRegenWait > 0.0f && ff < 1.0f;
        g.ui.Rect(hx, hy - 16, 260, 10, { 0, 0, 0, 0.45f });
        g.ui.Rect(hx + 2, hy - 14, 256 * ff, 6,
                  waiting ? UiColor{ 0.9f, 0.45f, 0.15f, 0.45f }
                          : UiColor{ 1.0f, 0.55f, 0.15f, 0.95f });
    }

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
              "WASD drive   SHIFT boost   mouse aim   LMB/space fire   TAB supply   ESC menu");
    if (!g.statusLine.empty())
        g.ui.TextCentered(w * 0.5f, float(g.height) - 30, 1.7f, { 1, 0.8f, 0.4f, 0.95f },
                          g.statusLine);

    if (g.shopOpen)
        BuildShop(frame);
    BuildOwnedRow(frame);
}

} // namespace tankaq
