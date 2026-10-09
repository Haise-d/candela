// Layered windows drawn with Direct2D, and the drawing helpers the UIs share.
#pragma once
#include "glitch.h"
#include "candela.h"

namespace gfx {

bool Init();
void Shutdown();
extern ID2D1Factory* d2d;
extern IDWriteFactory* dwrite;
extern IWICImagingFactory* wic;

inline D2D1_COLOR_F C(const Color& c, float a = 1.f) { return D2D1::ColorF(c.r, c.g, c.b, c.a * a); }

// A borderless, always-on-top window that never takes focus, whose pixels
// (with per-pixel alpha) come from a Direct2D drawing. Coordinates are
// physical pixels; callers scale by the monitor's DPI themselves.
class Surface {
public:
    // overlay: always on top, never takes focus (popups). Otherwise a normal
    // window you can click into and find in the taskbar (the settings).
    bool Create(const wchar_t* cls, WNDPROC proc, void* user, DWORD ex_extra = 0, HWND owner = nullptr,
                bool overlay = true);
    bool Resize(int w, int h);
    ID2D1RenderTarget* Begin();                 // cleared to transparent
    void End(int x, int y, float alpha);        // draw finished: show it at (x, y)
    void SetAlpha(float alpha);                 // re-show the last picture at a new opacity
    void Hide();
    void Destroy();
    bool Visible() const { return visible_; }
    bool SavePng(const std::wstring& path);  // developer screenshots (--dump)

    ID2D1SolidColorBrush* Brush(D2D1_COLOR_F c);
    // A scratch bitmap the size of a panel: draw the panel here, then copy
    // slices of it onto the surface for the tearing.
    ID2D1BitmapRenderTarget* Scratch(int w, int h);
    ID2D1RenderTarget* Target() { return rt_; }

    HWND hwnd = nullptr;
    int w = 0, h = 0;
    int generation = 0;  // bumps when the graphics driver restarted; old bitmaps are useless then

private:
    bool EnsureTarget();
    void ReleaseTarget();
    HDC dc_ = nullptr;
    HBITMAP bmp_ = nullptr, old_ = nullptr;
    void* bits_ = nullptr;
    ID2D1DCRenderTarget* rt_ = nullptr;
    ID2D1SolidColorBrush* brush_ = nullptr;
    ID2D1BitmapRenderTarget* scratch_ = nullptr;
    int scratch_w_ = 0, scratch_h_ = 0;
    int x_ = 0, y_ = 0;
    bool visible_ = false;
    bool overlay_ = true;
};

// ---- text ---------------------------------------------------------------------------------
enum Align { kLeft, kCenter, kRight };
struct TextStyle {
    float size;                    // px
    DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL;
    float spacing = 0;             // extra px between letters (spaced capitals)
    Align align = kLeft;
    bool wrap = false;             // otherwise one line, cut with "..."
    float line = 0;                // wrapped text: px from line to line (0 = the font's own)
    const wchar_t* font = nullptr; // nullptr: the font setting (in Japanese: the Japanese one)
};
// Draws text inside `box`, vertically centred when it's one line.
void Text(ID2D1RenderTarget* rt, Surface& s, const std::wstring& text, D2D1_RECT_F box,
          const TextStyle& style, D2D1_COLOR_F color);
float TextWidth(const std::wstring& text, const TextStyle& style);
// While p < 1, Text() draws every string as glyph noise settling into it,
// left to right (0: all noise), re-rolled every 33 ms of `t_ms`. For a whole
// window changing at once (switching the language). ScrambleText(1, ...) ends it.
void ScrambleText(double p, uint32_t seed, double t_ms);
// Picks the Japanese font and loads it now, so the first frame that uses it
// doesn't stall (and skip the start of an animation). Nothing in English.
void WarmFonts();
// Japanese fonts: whether Rodin is installed, and the name of the one in use
// ("rodin" for Rodin, else its family name).
bool RodinInstalled();
bool FontInstalled(const wchar_t* family);
std::wstring JapaneseFont();

// ---- shapes ---------------------------------------------------------------------------------
void Fill(ID2D1RenderTarget* rt, Surface& s, float x0, float y0, float x1, float y1, D2D1_COLOR_F c);
void Frame(ID2D1RenderTarget* rt, Surface& s, float x0, float y0, float x1, float y1, float w, D2D1_COLOR_F c);
void ClearRect(ID2D1RenderTarget* rt, float x0, float y0, float x1, float y1);  // punch a see-through hole

// An anti-aliased line.
void Line(ID2D1RenderTarget* rt, Surface& s, float x0, float y0, float x1, float y1, float w, D2D1_COLOR_F c);

// Copies a finished panel (`content`, cw x ch px) onto `rt` at (ox, oy) with
// the glitch applied: the line, the opening band, torn slices, noise blocks
// and edges. `scale` turns the Fx's unscaled px into physical px.
// `backdrop`: what torn-away parts show; nullptr = see-through (a popup on
// the desktop), a color = the panel it sits on (a preview inside a window).
void Compose(ID2D1RenderTarget* rt, Surface& s, ID2D1Bitmap* content, float ox, float oy,
             float cw, float ch, const glitch::Fx& fx, float scale, bool shadow = true,
             const D2D1_COLOR_F* backdrop = nullptr);

// A window icon as a bitmap: its own colors, or duotone in the palette.
ID2D1Bitmap* IconBitmap(ID2D1RenderTarget* rt, HICON icon, int size_px, bool duotone);

}  // namespace gfx
