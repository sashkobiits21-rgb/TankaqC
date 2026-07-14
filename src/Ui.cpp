#include "Ui.h"
#include <cstring>

#include "stb_easy_font.h"

namespace tankaq
{

void UiBuilder::Reset(int screenW, int screenH)
{
    m_verts.clear();
    m_w = screenW;
    m_h = screenH;
}

void UiBuilder::Rect(float x, float y, float w, float h, UiColor c)
{
    UiVertex a{ x,     y,     c.r, c.g, c.b, c.a };
    UiVertex b{ x + w, y,     c.r, c.g, c.b, c.a };
    UiVertex d{ x,     y + h, c.r, c.g, c.b, c.a };
    UiVertex e{ x + w, y + h, c.r, c.g, c.b, c.a };
    m_verts.push_back(a); m_verts.push_back(b); m_verts.push_back(e);
    m_verts.push_back(a); m_verts.push_back(e); m_verts.push_back(d);
}

void UiBuilder::RectOutline(float x, float y, float w, float h, float t, UiColor c)
{
    Rect(x, y, w, t, c);
    Rect(x, y + h - t, w, t, c);
    Rect(x, y + t, t, h - 2 * t, c);
    Rect(x + w - t, y + t, t, h - 2 * t, c);
}

void UiBuilder::Text(float x, float y, float scale, UiColor c, const std::string& text)
{
    if (text.empty())
        return;
    static char buffer[240000];
    int quads = stb_easy_font_print(0.0f, 0.0f, const_cast<char*>(text.c_str()),
                                    nullptr, buffer, sizeof(buffer));
    struct EasyVert { float x, y, z; unsigned char col[4]; };
    const EasyVert* v = reinterpret_cast<const EasyVert*>(buffer);
    for (int q = 0; q < quads; ++q)
    {
        const EasyVert* p = v + q * 4;
        UiVertex out[4];
        for (int i = 0; i < 4; ++i)
            out[i] = { x + p[i].x * scale, y + p[i].y * scale, c.r, c.g, c.b, c.a };
        m_verts.push_back(out[0]); m_verts.push_back(out[1]); m_verts.push_back(out[2]);
        m_verts.push_back(out[0]); m_verts.push_back(out[2]); m_verts.push_back(out[3]);
    }
}

float UiBuilder::TextWidth(float scale, const std::string& text) const
{
    return stb_easy_font_width(const_cast<char*>(text.c_str())) * scale;
}

void UiBuilder::TextCentered(float cx, float y, float scale, UiColor c, const std::string& text)
{
    Text(cx - TextWidth(scale, text) * 0.5f, y, scale, c, text);
}

void DrawButton(UiBuilder& ui, const UiButton& b, bool hovered)
{
    UiColor bg = hovered ? UiColor{ 0.32f, 0.42f, 0.28f, 0.95f }
                         : UiColor{ 0.16f, 0.20f, 0.15f, 0.92f };
    ui.Rect(b.x, b.y, b.w, b.h, bg);
    ui.RectOutline(b.x, b.y, b.w, b.h, 2.0f,
                   hovered ? UiColor{ 0.85f, 0.95f, 0.6f, 1.0f }
                           : UiColor{ 0.55f, 0.62f, 0.45f, 1.0f });
    float scale = 2.4f;
    float th = 7.0f * scale;
    ui.TextCentered(b.x + b.w * 0.5f, b.y + (b.h - th) * 0.5f, scale,
                    hovered ? UiColor{ 1, 1, 0.9f, 1 } : UiColor{ 0.85f, 0.9f, 0.8f, 1 },
                    b.label);
}

} // namespace tankaq
