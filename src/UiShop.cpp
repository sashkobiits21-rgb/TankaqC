// Supply line (upgrade conveyor) UI + the owned-items strip. Pure UI over
// the replicated offer/owned state in `g.game`; the only mutations are the
// purchase requests sent from HandleShopClick and the card animation state.
#include "AppState.h"
#include "Log.h"
#include "Sound.h"
#include <cstdio>
#include <cmath>
#include <algorithm>

using namespace DirectX;

namespace tankaq
{

const UiColor kRarityCol[8] = {
    { 0.35f, 0.37f, 0.35f, 0.96f },   // common
    { 0.15f, 0.40f, 0.20f, 0.96f },   // uncommon
    { 0.14f, 0.28f, 0.55f, 0.96f },   // rare
    { 0.40f, 0.17f, 0.52f, 0.96f },   // epic
    { 0.62f, 0.35f, 0.10f, 0.96f },   // legendary
    { 0.07f, 0.44f, 0.47f, 0.96f },   // class card (teal)
    { 0.72f, 0.58f, 0.09f, 0.96f },   // UNIQUE (gold rule-benders)
    { 0.42f, 0.72f, 0.34f, 0.96f },   // MUTATION (light green fusions)
};

void OpenShop()
{
    g.shopOpen = true;
    // fresh entrance: existing offers slide in from off-screen again,
    // and the broken exit hatch is whole again
    for (auto& a : g.cardAnims)
        a.active = false;
    g.slatsBroken = false;
}

// ---- drawing helpers (rotated quads share the plain triangle paths) ----

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
    u0 = float(icon) / float(UpgradeCount);
    u1 = float(icon + 1) / float(UpgradeCount);
}

void AddIconQuad(FrameData& frame, int icon, float cx, float cy, float half,
                 float alpha, float ang)
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

void SlotCell(int slot, float& x, float& y)
{
    int col = slot % 2, row = slot / 2;
    x = g.shopPanel[0] + kCardGap + col * (kCardW + kCardGap);
    y = g.shopPanel[1] + kShopHeader + kCardGap + row * (kCardH + kCardGap);
}

// Panel outline where the lower-right border is a set of breakable vertical
// slats: the conveyor's exit hatch. While broken, the slats are absent (their
// debris is flying around instead).
static void DrawShopFrame()
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

static void BreakSlats()
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

// TEST mode: the whole pool on one panel; hover for details, one click
// grants one copy (host-validated, replicated like a purchase).
// The pool outgrew a fixed 6-wide grid: on short windows the NEWEST rows
// (the mutations) fell off the bottom of the screen. Columns now widen so
// every entry always fits the window height.
static int TestGridCols()
{
    float availH = float(g.height) - 8.0f - 46.0f - 50.0f;
    int rowsFit = std::max(1, int(availH / 54.0f));
    int cols = (UpgradeCount + rowsFit - 1) / rowsFit;
    return std::max(6, cols);
}

static void BuildTestGrid(FrameData& frame)
{
    const int kCols = TestGridCols();
    constexpr float kCell = 54.0f, kPad = 10.0f;
    int rows = (UpgradeCount + kCols - 1) / kCols;
    float panelW = kPad * 2 + kCols * kCell;
    float panelH = 46.0f + rows * kCell + kPad + 40.0f;
    float px = 14;
    float py = std::max(8.0f, (float(g.height) - panelH) * 0.5f);
    g.shopPanel[0] = px; g.shopPanel[1] = py;
    g.shopPanel[2] = panelW; g.shopPanel[3] = panelH;
    UiHotRect(UiIdShopPanel, px, py, panelW, panelH);
    g.ui.Rect(px, py, panelW, panelH, { 0.07f, 0.09f, 0.07f, 0.88f });
    g.ui.Text(px + 10, py + 9, 2.2f, { 1, 0.95f, 0.6f, 1 },
              "TEST ARSENAL - click = +1 copy");
    int hover = -1;
    for (int i = 0; i < UpgradeCount; ++i)
    {
        float cx = px + kPad + (i % kCols) * kCell;
        float cy = py + 46.0f + (i / kCols) * kCell;
        const UpgradeType& u = kUpgradePool[i];
        UiColor bg = kRarityCol[std::clamp(u.rarity, 0, 7)];
        bool hov = g.mouseX >= cx && g.mouseX < cx + kCell - 4
                && g.mouseY >= cy && g.mouseY < cy + kCell - 4;
        if (hov) hover = i;
        bg.a = hov ? 1.0f : 0.85f;
        g.ui.Rect(cx, cy, kCell - 4, kCell - 4, bg);
        if (hov)
            g.ui.RectOutline(cx, cy, kCell - 4, kCell - 4, 2,
                             { 1, 1, 1, 0.9f });
        AddIconQuad(frame, i, cx + (kCell - 4) * 0.5f,
                    cy + (kCell - 4) * 0.5f, 18, 1.0f, 0.0f);
    }
    if (hover >= 0)
    {
        char buf[128];
        sprintf_s(buf, "%s - %s", kUpgradePool[hover].name,
                  kUpgradePool[hover].desc);
        g.ui.Text(px + 10, py + panelH - 30.0f, 1.5f,
                  { 1, 1, 1, 0.92f }, buf);
        g.hoverKeyNow = 5000 + hover;
    }
}

void HandleTestGridClick(float mx, float my)
{
    const int kCols = TestGridCols();
    constexpr float kCell = 54.0f, kPad = 10.0f;
    float px = g.shopPanel[0], py = g.shopPanel[1];
    int col = int((mx - px - kPad) / kCell);
    int row = int((my - py - 46.0f) / kCell);
    if (col < 0 || col >= kCols || row < 0)
        return;
    float cx = px + kPad + col * kCell, cy = py + 46.0f + row * kCell;
    if (mx > cx + kCell - 4 || my > cy + kCell - 4)
        return;   // the gap between cells
    int idx = row * kCols + col;
    if (idx >= 0 && idx < UpgradeCount)
        RequestTestGrant(uint8_t(idx));
}

void BuildShop(FrameData& frame)
{
    if (g.game.testMode)
    {
        BuildTestGrid(frame);
        return;
    }
    const PlayerState& me = g.game.players[g.myId];
    float panelW = kCardGap * 3 + kCardW * 2;
    float panelH = kShopHeader + kCardGap + 3 * kCardH + 2 * kCardGap + kCardGap;
    float px = 14;
    float py = (float(g.height) - panelH) * 0.5f;
    g.shopPanel[0] = px; g.shopPanel[1] = py;
    g.shopPanel[2] = panelW; g.shopPanel[3] = panelH;
    UiHotRect(UiIdShopPanel, px, py, panelW, panelH);

    g.ui.Rect(px, py, panelW, panelH, { 0.07f, 0.09f, 0.07f, 0.85f });
    DrawShopFrame();
    char buf[96];
    sprintf_s(buf, "SUPPLY LINE   $ %u", unsigned(me.money));
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
                                    g.time, int(o.type),
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
        AddIconQuad(frame, int(o.type), anim->x + kCardW * 0.5f, anim->y + 34, 20, 1.0f);
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
        g.lastTailIcon = int(me.offers[NumOfferSlots - 1].type);
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
        UiColor bg = kRarityCol[std::clamp(fx.rarity, 0, 7)];
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
        static const char* rarityNames[8] = { "COMMON", "UNCOMMON", "RARE",
                                              "EPIC", "LEGENDARY", "CLASS",
                                              "UNIQUE", "MUTATION" };
        g.ui.Text(tx + 10, ty + 74, 1.4f, { 1, 1, 1, 0.6f },
                  rarityNames[std::clamp(def.rarity, 0, 7)]);
    }
}

// Owned-items strip: a half-transparent horizontal row of everything bought,
// anchored to the right screen edge. Icons are deduped with an xN count,
// hovering shows the card's description, and the (also half-transparent)
// arrow at the far right collapses the row.
void BuildOwnedRow(FrameData& frame)
{
    if (!InSession())
        return;
    const PlayerState& me = g.game.players[g.myId];
    if (me.owned.empty())
        return;

    // dedupe by type, first-purchase order
    uint8_t types[64];
    int counts[64], n = 0;
    for (uint8_t t : me.owned)
    {
        int f = -1;
        for (int i = 0; i < n; ++i)
            if (types[i] == t) { f = i; break; }
        if (f >= 0) ++counts[f];
        else if (n < 64) { types[n] = t; counts[n] = 1; ++n; }
    }

    const float tile = 36, gap = 5, arrowW = 16, y = 12;
    float ax = float(g.width) - 10 - arrowW;
    UiHotRect(UiIdOwnedArrow, ax, y, arrowW, tile);
    bool ah = float(g.mouseX) >= ax && float(g.mouseX) <= ax + arrowW
           && float(g.mouseY) >= y && float(g.mouseY) <= y + tile;
    g.ui.Rect(ax, y, arrowW, tile, { 0.10f, 0.12f, 0.10f, ah ? 0.8f : 0.5f });
    g.ui.Text(ax + 5, y + tile * 0.5f - 6, 1.6f, { 1, 1, 1, ah ? 0.95f : 0.5f },
              g.ownedRowHidden ? "<" : ">");
    if (g.ownedRowHidden)
        return;

    int hoverIdx = -1;
    float hoverX = 0;
    float x = ax - gap - tile;
    for (int i = 0; i < n; ++i, x -= tile + gap)
    {
        const UpgradeType& u = kUpgradePool[types[i]];
        bool hov = float(g.mouseX) >= x && float(g.mouseX) <= x + tile
                && float(g.mouseY) >= y && float(g.mouseY) <= y + tile;
        UiColor bg = kRarityCol[std::clamp(u.rarity, 0, 7)];
        bg.a = hov ? 0.85f : 0.5f;
        g.ui.Rect(x, y, tile, tile, bg);
        AddIconQuad(frame, types[i], x + tile * 0.5f, y + tile * 0.5f, 12,
                    hov ? 1.0f : 0.55f);
        if (counts[i] > 1)
        {
            char b[8];
            sprintf_s(b, "x%d", counts[i]);
            g.ui.Text(x + 2, y + tile - 10, 1.2f,
                      { 1, 1, 1, hov ? 0.95f : 0.6f }, b);
        }
        // TEST mode: right-click = -1 copy (removes the entry at 1)
        if (hov && g.rightClicked && g.game.testMode)
        {
            g.rightClicked = false;
            RequestTestRevoke(types[i]);
        }
        if (hov) { hoverIdx = i; hoverX = x; }
    }

    if (hoverIdx >= 0)   // description tooltip under the strip
    {
        const UpgradeType& u = kUpgradePool[types[hoverIdx]];
        const float tw = 250, th = 66;
        float tx = std::min(hoverX, float(g.width) - tw - 8);
        float ty = y + tile + 6;
        g.ui.Rect(tx, ty, tw, th, { 0.05f, 0.06f, 0.05f, 0.94f });
        g.ui.RectOutline(tx, ty, tw, th, 2,
                         kRarityCol[std::clamp(u.rarity, 0, 7)]);
        g.ui.Text(tx + 8, ty + 8, 1.8f, { 1, 1, 1, 1 }, u.name);
        g.ui.Text(tx + 8, ty + 28, 1.4f, { 0.85f, 0.9f, 0.85f, 1 }, u.desc);
        char own[24];
        sprintf_s(own, "OWNED x%d", counts[hoverIdx]);
        g.ui.Text(tx + 8, ty + 46, 1.3f, { 1, 1, 1, 0.6f }, own);
    }
}

void HandleShopClick(float mx, float my)
{
    if (g.game.testMode)
    {
        HandleTestGridClick(mx, my);
        return;
    }
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
        g.lastClickedIcon = int(o.type);
        g.lastClickedRarity = kUpgradePool[o.type].rarity;
        g.lastClickX = mx;
        g.lastClickY = my;
        if (!g.online || g.isHost)
            HostPurchase(g.myId, c.slot);
        else
            g.net.SendPurchaseToHost(c.slot);   // optimistic; host validates
        return;
    }
}

} // namespace tankaq
