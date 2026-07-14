#pragma once
#include <string>
#include <vector>
#include "render/IRenderer.h"

namespace tankaq
{

struct UiColor { float r, g, b, a; };

class UiBuilder
{
public:
    void Reset(int screenW, int screenH);
    void Rect(float x, float y, float w, float h, UiColor c);
    void RectOutline(float x, float y, float w, float h, float thickness, UiColor c);
    // stb_easy_font text; scale 1 = ~7px tall glyphs
    void Text(float x, float y, float scale, UiColor c, const std::string& text);
    void TextCentered(float cx, float y, float scale, UiColor c, const std::string& text);
    float TextWidth(float scale, const std::string& text) const;

    int width() const { return m_w; }
    int height() const { return m_h; }
    const std::vector<UiVertex>& vertices() const { return m_verts; }

private:
    std::vector<UiVertex> m_verts;
    int m_w = 0, m_h = 0;
};

struct UiButton
{
    float x = 0, y = 0, w = 0, h = 0;
    std::string label;
    bool Contains(float mx, float my) const
    {
        return mx >= x && mx <= x + w && my >= y && my <= y + h;
    }
};

void DrawButton(UiBuilder& ui, const UiButton& b, bool hovered);

} // namespace tankaq
