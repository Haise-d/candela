// What a screen plays the moment it's blacked out, following the Animation
// setting: a line snaps open into a panel where BLACKOUT types in; then the
// text corrupts, the panel tears and the screen goes black behind it in
// ragged slices, and the panel folds back into a line. Off: plain black at
// once.
//
// Nothing full-screen is redrawn per frame. The black window itself does the
// full-screen part (one color and opacity, or a region of slices), and the
// panel lives in a see-through layer only as big as it is. It plays once,
// when the black appears: bringing a window onto that screen or removing the
// blackout ends it at once.
//
// Designed on a 1080p screen; everything scales with the screen's shorter
// side (its height on a landscape screen, its width on a vertical one), so
// the panel always fits across.
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <memory>

#include "render.h"

namespace blackfx {

using namespace gfx;

namespace {

enum Kind { kNone, kGlitch };

Kind CurrentKind() { return g_cfg.animation == L"off" ? kNone : kGlitch; }

// ---- timing ------------------------------------------------------------------------------------------
double Clamp01(double p) { return glitch::Clamp01(p); }
double Lerp(double a, double b, double p) { return a + (b - a) * p; }
double Step(double t, double start, double dur) { return Clamp01((t - start) / dur); }
// CSS's cubic-bezier() timing functions (the same as the popups').
double Bezier(double x1, double y1, double x2, double y2, double x) {
    if (x <= 0) return 0;
    if (x >= 1) return 1;
    auto bx = [&](double t) { return 3 * (1 - t) * (1 - t) * t * x1 + 3 * (1 - t) * t * t * x2 + t * t * t; };
    auto by = [&](double t) { return 3 * (1 - t) * (1 - t) * t * y1 + 3 * (1 - t) * t * t * y2 + t * t * t; };
    double lo = 0, hi = 1, t = x;
    for (int i = 0; i < 30; i++) {
        t = (lo + hi) / 2;
        if (bx(t) < x) lo = t;
        else hi = t;
    }
    return by(t);
}
double EaseOut(double p) { return Bezier(0, 0, 0.58, 1, p); }
double EaseIn(double p) { return Bezier(0.42, 0, 1, 1, p); }

struct Rgb { double r, g, b; };  // 0..255
Rgb Mix(Rgb a, Rgb b, double p) { return {Lerp(a.r, b.r, p), Lerp(a.g, b.g, p), Lerp(a.b, b.b, p)}; }
D2D1_COLOR_F Col(Rgb c, double a) {
    return D2D1::ColorF((float)(c.r / 255), (float)(c.g / 255), (float)(c.b / 255), (float)Clamp01(a));
}

// A screen, in its own physical pixels: size, scale (1 = 1080p on the
// shorter side) and centre.
struct Geo {
    float w = 1920, h = 1080, k = 1, cx = 960, cy = 540;
    std::wstring number;  // "2" of \\.\DISPLAY2, for the header
};

// "DISPLAY 2" / ディスプレイ 2
std::wstring ScreenName(const Geo& g) {
    std::wstring name = Tr(L"DISPLAY", L"ディスプレイ");
    return g.number.empty() ? name : name + L" " + g.number;
}

// What the black windows show: one color at an opacity, or opaque black
// in slices (monitor-local rectangles) that re-roll every 33 ms.
struct Look {
    Rgb color{0, 0, 0};
    double alpha = 1;
    bool slices = false;
    int step = -1;  // which 33 ms roll the slices are from
    std::vector<RECT> rects;
};

// ---- the timings (Subtle runs the same moves in 60% of the time) -------------------------------------------
struct GlitchTimes { double line, open, typed, brk, bend, fold, out, b0, b1, dur, k; };
GlitchTimes Times() {
    double k = g_cfg.animation == L"subtle" ? 0.6 : 1.0;
    return {90 * k, 240 * k, 470 * k, 600 * k, 840 * k, 940 * k, 1020 * k, 620 * k, 880 * k, 1080 * k, k};
}

double Duration(Kind kind) {
    switch (kind) {
        case kGlitch: return Times().dur;
        default: return 0;
    }
}

// =============================================================================================
// BLACKOUT, glitched out.
// =============================================================================================
// The panel is drawn at 760 x 190 (1080p px). Its glitch (tears, blocks,
// edges) is worked out in units 1.8 times bigger, the size of a popup, so
// glitch.cpp's sizes fit it unchanged.
constexpr float kPW = 760, kPH = 190, kU = 1.8f;

struct Row { int y, h; double u; };
std::vector<Row> Rows(const Geo& g) {
    glitch::Rng r(0xB1AC);
    static const int hs[10] = {6, 8, 12, 16, 24, 32, 48, 64, 96, 128};
    std::vector<Row> rows;
    for (int y = 0; y < (int)g.h;) {
        int h = std::max(2, (int)std::lround(hs[r.range(0, 10)] * g.k));
        rows.push_back({y, std::min(h, (int)g.h - y), r.uniform()});
        y += h;
    }
    return rows;
}

Look GlitchLook(double t, const Geo& g, const std::vector<Row>& rows, uint32_t seed) {
    GlitchTimes T = Times();
    Look l;
    double cov = Step(t, T.b0, T.b1 - T.b0);
    if (cov >= 1) return l;  // all black
    l.slices = true;
    if (cov <= 0) return l;  // none yet (an empty region)
    l.step = (int)std::floor(std::max(0.0, t) / 33.0);
    glitch::Rng r = glitch::RngAt(seed, t, 7);
    double c = cov * 1.2 - 0.1;
    const int W = (int)g.w;
    for (auto& row : rows) {
        double j = (r.uniform() - 0.5) * 0.3;
        if (row.u < c + j) {
            l.rects.push_back({0, row.y, W, row.y + row.h});
        } else if (row.u < c + j + 0.12 && r.chance(0.55)) {
            int x = (int)(r.uniform() * W);  // a row torn part way across
            if (r.chance(0.5)) l.rects.push_back({0, row.y, x, row.y + row.h});
            else l.rects.push_back({x, row.y, W, row.y + row.h});
        }
    }
    return l;
}

glitch::Fx GlitchFx(double t, uint32_t seed) {
    const GlitchTimes T = Times();
    const auto& st = glitch::CurrentStyle();
    glitch::Fx fx;
    glitch::Rng r = glitch::RngAt(seed, t, 1);
    if (t < T.line) {
        fx.line = (float)EaseOut(t / T.line);
        fx.alpha = glitch::Flicker(r, st, 1);
        return fx;
    }
    if (t >= T.out) {
        fx.line = 0;
        return fx;
    }
    if (t >= T.fold) {  // the panel is a line again, shrinking away
        double p = (t - T.fold) / (T.out - T.fold);
        fx.line = (float)(1 - EaseIn(p));
        fx.alpha = (float)(1 - 0.5 * p);
        return fx;
    }
    double intensity;
    if (t < T.open) {
        fx.open = (float)EaseOut((t - T.line) / (T.open - T.line));
        fx.edges = true;
        intensity = 1;
        fx.alpha = glitch::Flicker(r, st, 1);
    } else if (t < T.typed) {
        intensity = 0.6 * (1 - (t - T.open) / (T.typed - T.open));
    } else if (t < T.brk) {
        intensity = 0;
    } else if (t < T.bend) {
        intensity = 0.5 + 0.5 * Clamp01((t - T.brk) / (T.bend - T.brk));
        fx.alpha = glitch::Flicker(r, st, intensity);
    } else {
        fx.open = (float)(1 - EaseIn((t - T.bend) / (T.fold - T.bend)));
        fx.edges = true;
        intensity = 1;
    }
    fx.tears = glitch::Tears(r, st, intensity, 0, kPH / kU);
    fx.blocks = glitch::Blocks(r, st, intensity * 0.8, kPW / kU, kPH / kU);
    return fx;
}

std::wstring Upper(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towupper(c);
    return s;
}

void DrawPanel(ID2D1RenderTarget* rt, Surface& s, const Geo& g, double t, uint32_t seed) {
    const GlitchTimes T = Times();
    const float k = g.k;
    auto P = [&](float v) { return v * k; };
    rt->Clear(C(g_pal.paper));
    glitch::Rng rng = glitch::RngAt(seed, t, 2);
    bool breaking = t >= T.brk;
    double amount = breaking ? 0.15 + 0.85 * Clamp01((t - T.brk) / (T.bend - T.brk)) : 0;
    auto vis = [&](const std::wstring& text, double from, double len, bool right = false) {
        std::wstring v = glitch::Decode(text, Step(t, from * T.k, len * T.k), rng, nullptr, right);
        return breaking ? glitch::Corrupt(v, amount, rng) : v;
    };
    const float px = std::max(1.f, std::round(k));
    Fill(rt, s, std::round(P(26)), std::round(P(26)), std::round(P(36)), std::round(P(36)), C(g_pal.ink));
    Text(rt, s, vis(ScreenName(g), 140, 160), D2D1::RectF(P(48), P(18), P(420), P(46)), {P(15), DWRITE_FONT_WEIGHT_SEMI_BOLD, P(4)}, C(g_pal.ink));
    Text(rt, s, vis(Tr(L"OTHER SCREENS", L"他画面"), 200, 200, true), D2D1::RectF(P(420), P(18), P(kPW - 26), P(46)),
         {P(12), DWRITE_FONT_WEIGHT_NORMAL, P(2.5f), kRight}, C(g_pal.ink_soft));
    Fill(rt, s, std::round(P(26)), std::round(P(54)), std::round(P(kPW - 26)), std::round(P(54)) + px, C(g_pal.ink));
    // BLACKOUT (ブラックアウト), centred, each letter in the slot the final
    // word gives it, so the glyphs it decodes through don't make it jitter
    // sideways.
    const std::wstring word = Tr(L"BLACKOUT", L"ブラックアウト");
    const size_t n = word.size();
    TextStyle big{P(70), DWRITE_FONT_WEIGHT_SEMI_BOLD, 0, kCenter};
    const float gap = P(Japanese() ? 12.f : 20.f);  // kana are wider than capitals already
    std::vector<float> widths(n);
    float total = gap * (float)(n - 1);
    for (size_t i = 0; i < n; i++) total += widths[i] = TextWidth(word.substr(i, 1), big);
    std::wstring shown = vis(word, 170, 300);
    float x = P(kPW / 2) - total / 2, end = x;
    for (size_t i = 0; i < shown.size() && i < n; i++) {
        Text(rt, s, shown.substr(i, 1), D2D1::RectF(x - P(30), P(70), x + widths[i] + P(30), P(154)), big, C(g_pal.ink));
        x += widths[i] + gap;
        end = x - gap;
    }
    double typed = Step(t, 170 * T.k, 300 * T.k);
    if (typed > 0 && typed < 1) Fill(rt, s, std::round(end + P(10)), std::round(P(86)), std::round(end + P(38)), std::round(P(138)), C(g_pal.ink));
    Fill(rt, s, std::round(P(26)), std::round(P(152)), std::round(P(kPW - 26)), std::round(P(152)) + px, C(g_pal.ink));
    std::wstring key = Upper(g_cfg.hotkey_blackout);
    std::wstring foot = Japanese() ? (key.empty() ? L"トレイメニューから復帰" : key + L" で復帰")
                                   : (key.empty() ? L"THE TRAY MENU BRINGS IT BACK" : key + L" BRINGS IT BACK");
    Text(rt, s, vis(foot, 260, 220), D2D1::RectF(P(26), P(158), P(kPW - 26), P(182)), {P(12), DWRITE_FONT_WEIGHT_NORMAL, P(2.5f)},
         C(g_pal.ink_soft));
}

// Returns the layer's opacity (the glitch's flicker).
float DrawGlitch(ID2D1RenderTarget* rt, Surface& s, const Geo& g, double t, uint32_t seed) {
    glitch::Fx fx = GlitchFx(t, seed);
    float cw = std::round(kPW * g.k), ch = std::round(kPH * g.k);
    float ox = std::round(g.cx - cw / 2), oy = std::round(g.cy - ch / 2);
    auto* scratch = s.Scratch((int)cw, (int)ch);
    if (!scratch) return fx.alpha;
    scratch->BeginDraw();
    DrawPanel(scratch, s, g, t, seed);
    scratch->EndDraw();
    ID2D1Bitmap* bmp = nullptr;
    if (SUCCEEDED(scratch->GetBitmap(&bmp))) {
        Compose(rt, s, bmp, ox, oy, cw, ch, fx, g.k * kU);
        bmp->Release();
    }
    return fx.alpha;
}

// ---- per kind ---------------------------------------------------------------------------------------------------
Look LookAt(Kind kind, double t, const Geo& g, const std::vector<Row>& rows, uint32_t seed) {
    if (t >= Duration(kind)) return Look();
    switch (kind) {
        case kGlitch: return GlitchLook(t, g, rows, seed);
        default: return Look();
    }
}

float DrawEffect(Kind kind, ID2D1RenderTarget* rt, Surface& s, const Geo& g, double t, uint32_t seed) {
    switch (kind) {
        case kGlitch: return DrawGlitch(rt, s, g, t, seed);
        default: return 1;
    }
}

// Where the layer goes on the screen (monitor-local px): only as big as what
// it draws, glows included.
RECT LayerRect(Kind kind, const Geo& g) {
    float x0, y0, x1, y1;
    const float k = g.k;
    (void)kind;
    x0 = g.cx - (kPW / 2 + 80) * k; x1 = g.cx + (kPW / 2 + 80) * k; y0 = g.cy - (kPH / 2 + 26) * k; y1 = g.cy + (kPH / 2 + 26) * k;
    return {std::max(0L, (LONG)std::floor(x0)), std::max(0L, (LONG)std::floor(y0)), std::min((LONG)g.w, (LONG)std::ceil(x1)),
            std::min((LONG)g.h, (LONG)std::ceil(y1))};
}

// =============================================================================================
// One blacked-out screen's animation.
// =============================================================================================
class Anim {
public:
    ~Anim() { surf_.Destroy(); }

    void Start(Kind kind, HMONITOR mon, const RECT& screen, const std::vector<HWND>& blacks) {
        kind_ = kind;
        mon_ = mon;
        screen_ = screen;
        blacks_ = blacks;
        g_.w = (float)(screen.right - screen.left);
        g_.h = (float)(screen.bottom - screen.top);
        g_.k = std::min(g_.w, g_.h) / 1080.f;  // not the height alone: a vertical screen would cut the panel off
        g_.cx = std::round(g_.w / 2);
        g_.cy = std::round(g_.h / 2);
        MONITORINFOEXW mi = {};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(mon, &mi)) {  // "\\.\DISPLAY2" -> "DISPLAY 2"
            std::wstring dev = mi.szDevice;
            size_t d = dev.find_last_not_of(L"0123456789");
            if (d != std::wstring::npos && d + 1 < dev.size()) g_.number = dev.substr(d + 1);
        }
        t0_ = NowMs();
        seed_ = (uint32_t)(t0_ * 7919.0) ^ (uint32_t)(uintptr_t)mon;
        if (kind_ == kGlitch) rows_ = Rows(g_);
        layer_ = LayerRect(kind_, g_);
        if (!surf_.Create(L"CandelaBlackoutFx", Proc, this, WS_EX_TRANSPARENT) ||
            !surf_.Resize(layer_.right - layer_.left, layer_.bottom - layer_.top)) {
            Log(L"Blackout: couldn't make the animation's layer; plain black instead");
            Finish();
            return;
        }
        SetTimer(surf_.hwnd, 1, timer_ms_, nullptr);
        Tick();
    }

    void Tick() {
        if (done_) return;
        double t = NowMs() - t0_;
        if (t >= Duration(kind_)) {
            Finish();
            return;
        }
        Apply(LookAt(kind_, t, g_, rows_, seed_));
        ID2D1RenderTarget* rt = surf_.Begin();
        if (!rt) return;
        rt->SetTransform(D2D1::Matrix3x2F::Translation(-(float)layer_.left, -(float)layer_.top));
        float alpha = DrawEffect(kind_, rt, surf_, g_, t, seed_);
        rt->SetTransform(D2D1::Matrix3x2F::Identity());
        surf_.End(screen_.left + layer_.left, screen_.top + layer_.top, alpha);
        // Keep it above the black (and anything else always-on-top that
        // appeared meanwhile) without repeating the call every frame.
        int beat = (int)(t / 250);
        if (beat != last_beat_) {
            last_beat_ = beat;
            SetWindowPos(surf_.hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        }
    }

    // Straight to the end: plain black, no layer.
    void Finish() {
        if (done_) return;
        done_ = true;
        if (surf_.hwnd) KillTimer(surf_.hwnd, 1);
        Apply(Look());
        surf_.Destroy();  // safe from inside its own timer message: nothing touches it after
    }

    bool Done() const { return done_; }
    HMONITOR Monitor() const { return mon_; }

private:
    void Apply(const Look& l) {
        BYTE alpha = (BYTE)std::clamp((int)std::lround(l.alpha * 255), 0, 255);
        COLORREF color = RGB((BYTE)std::lround(l.color.r), (BYTE)std::lround(l.color.g), (BYTE)std::lround(l.color.b));
        bool new_slices = l.slices && (l.step != last_step_ || !had_slices_);
        for (HWND h : blacks_) {
            if (!IsWindow(h)) continue;
            if (color != last_color_) {
                SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)color);  // windows.cpp paints the window in it
                InvalidateRect(h, nullptr, TRUE);
            }
            if (alpha != last_alpha_) SetLayeredWindowAttributes(h, 0, alpha, LWA_ALPHA);
            if (new_slices) {
                RECT wr;
                GetWindowRect(h, &wr);
                HRGN rgn = CreateRectRgn(0, 0, 0, 0);
                for (auto& r : l.rects) {
                    RECT sr = {screen_.left + r.left, screen_.top + r.top, screen_.left + r.right, screen_.top + r.bottom}, x;
                    if (!IntersectRect(&x, &sr, &wr)) continue;
                    HRGN part = CreateRectRgn(x.left - wr.left, x.top - wr.top, x.right - wr.left, x.bottom - wr.top);
                    CombineRgn(rgn, rgn, part, RGN_OR);
                    DeleteObject(part);
                }
                SetWindowRgn(h, rgn, TRUE);  // the window owns the region now
            } else if (!l.slices && had_slices_) {
                SetWindowRgn(h, nullptr, TRUE);
            }
        }
        last_color_ = color;
        last_alpha_ = alpha;
        last_step_ = l.step;
        had_slices_ = l.slices;
    }

    static LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        auto* self = (Anim*)GetWindowLongPtrW(h, GWLP_USERDATA);
        if (msg == WM_TIMER && self) {
            self->Tick();
            return 0;
        }
        if (msg == WM_NCHITTEST) return HTTRANSPARENT;
        return DefWindowProcW(h, msg, wp, lp);
    }

    Surface surf_;
    Kind kind_ = kNone;
    HMONITOR mon_ = nullptr;
    RECT screen_ = {}, layer_ = {};
    std::vector<HWND> blacks_;
    Geo g_;
    std::vector<Row> rows_;
    double t0_ = 0;
    uint32_t seed_ = 1;
    bool done_ = false;
    int last_alpha_ = -1, last_step_ = -3, last_beat_ = -1;
    COLORREF last_color_ = CLR_INVALID;
    bool had_slices_ = false;
    UINT timer_ms_ = 10;
};

std::vector<std::unique_ptr<Anim>> g_anims;

void Prune() {
    g_anims.erase(std::remove_if(g_anims.begin(), g_anims.end(), [](const std::unique_ptr<Anim>& a) { return a->Done(); }),
                  g_anims.end());
}

}  // namespace

bool Wanted() { return CurrentKind() != kNone; }

void Start(HMONITOR mon, const RECT& screen, const std::vector<HWND>& blacks) {
    Prune();
    auto a = std::make_unique<Anim>();
    Kind kind = CurrentKind();
    if (kind == kNone) kind = kGlitch;  // the caller asked Wanted() first; never leave the black see-through
    a->Start(kind, mon, screen, blacks);
    if (!a->Done()) g_anims.push_back(std::move(a));
}

void Finish(HMONITOR mon) {
    for (auto& a : g_anims)
        if (a->Monitor() == mon && !a->Done()) {
            Log(L"Blackout: a window came to a screen still animating; it goes plain black now");
            a->Finish();
        }
}

void StopAll() { g_anims.clear(); }

// Developer frames for --dump: each style over a stand-in desktop at 960 x
// 540, with the black windows' part drawn in.
void DebugDump(const std::wstring& dir) {
    Surface s;
    if (!s.Create(L"CandelaBlackoutDump", DefWindowProcW, nullptr) || !s.Resize(960, 540)) return;
    Geo g;
    g.w = 960; g.h = 540; g.k = 0.5f; g.cx = 480; g.cy = 270; g.number = L"2";
    std::wstring anim = g_cfg.animation;
    struct Run { const wchar_t* name; Kind kind; const wchar_t* style; };
    const Run runs[] = {{L"blackout_full", kGlitch, L"full"}, {L"blackout_subtle", kGlitch, L"subtle"}};
    std::vector<Row> rows = Rows(g);
    wchar_t name[96];
    for (auto& run : runs) {
        g_cfg.animation = run.style;
        double dur = Duration(run.kind);
        for (int i = 0; i * 16.0 <= dur + 32; i++) {
            double t = i * 16.0;
            ID2D1RenderTarget* rt = s.Begin();
            if (!rt) break;
            // A stand-in desktop: dusky wallpaper, a light window and a dark one.
            for (int b = 0; b < 20; b++)
                Fill(rt, s, 0, 27.f * b, 960, 27.f * (b + 1), Col(Mix({28, 42, 70}, {201, 135, 107}, b / 19.0), 1));
            Fill(rt, s, 70, 50, 570, 380, Col({245, 244, 241}, 1));
            for (int l = 0; l < 8; l++) Fill(rt, s, 110, 110.f + l * 22, 520.f - (l % 3) * 60, 118.f + l * 22, Col({200, 197, 190}, 1));
            Fill(rt, s, 430, 160, 890, 440, Col({30, 31, 35}, 1));
            for (int l = 0; l < 10; l++) Fill(rt, s, 460, 190.f + l * 22, 700.f + (l % 4) * 40, 197.f + l * 22, Col({130, 170, 255}, 1));
            Look look = LookAt(run.kind, t, g, rows, 0x5EED);
            if (look.slices) {
                for (auto& r : look.rects) Fill(rt, s, (float)r.left, (float)r.top, (float)r.right, (float)r.bottom, Col({0, 0, 0}, 1));
            } else if (look.alpha > 0) {
                Fill(rt, s, 0, 0, 960, 540, Col(look.color, look.alpha));
            }
            if (t < dur) DrawEffect(run.kind, rt, s, g, t, 0x5EED);
            s.End(0, 0, 1.f);
            swprintf(name, 96, L"\\%ls_%04d.png", run.name, i);
            s.SavePng(dir + name);
        }
    }
    g_cfg.animation = anim;
    s.Destroy();
}

}  // namespace blackfx
