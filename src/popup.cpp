// The warmth popup: what a hotkey did (the new temperature, brightness,
// Darkroom, blackout...), in a square panel with the glitch animation.
// It has its own corner (Popups -> Warmth position); when the volume popup
// (media.cpp) is set to the same corner, showing one hides the other.
//
// It never takes focus and clicks go through it, so it can't interrupt a
// fullscreen app.
#include <algorithm>
#include <cmath>
#include <cwctype>

#include "render.h"

namespace popup {

using namespace gfx;

namespace {

constexpr double kHoldMs = 1300;  // stays readable this long after the last press

// Where a panel of w x h (unscaled px) goes, with `margin` of window around it
// for shadows and glows.
struct Place {
    int x, y;
    float s;
};
Place PlaceFor(float w, float h, float margin, const std::wstring& pos) {
    HMONITOR mon = ForegroundMonitor();
    MONITORINFO mi = MonitorInfoOf(mon);
    float s = DpiScaleForMonitor(mon);
    int ww = (int)(std::round(w * s) + 2 * std::round(margin * s));
    int wh = (int)(std::round(h * s) + 2 * std::round(margin * s));
    const RECT& r = mi.rcWork;
    int gap = (int)std::round(24 * s - margin * s);  // 24 px from the screen's edge to the panel
    bool top = pos.rfind(L"top", 0) == 0;
    bool left = pos.find(L"left") != std::wstring::npos, center = pos.find(L"center") != std::wstring::npos;
    int x = center ? (r.left + r.right - ww) / 2 : left ? r.left + gap : r.right - ww - gap;
    int y = top ? r.top + gap : r.bottom - wh - gap;
    return {x, y, s};
}

std::wstring Upper(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towupper(c);
    return s;
}

// =============================================================================================
// The square panel with the glitch animation (the same as the volume card).
// =============================================================================================
constexpr float kGW = 380, kGH = 124, kGM = 22;
constexpr float kValueY0 = 40, kValueY1 = 86;

class Glitchy {
public:
    void Init() { surf_.Create(L"CandelaPopup", Proc, this, WS_EX_TRANSPARENT); }

    void Show(const Content& in) {
        if (!surf_.hwnd) return;
        Content c = in;
        c.title = Upper(c.title);
        c.note = Upper(c.note);
        c.value = Upper(c.value);
        double now = NowMs();
        const auto& st = glitch::CurrentStyle();
        Phase ph = PhaseAt(now);
        bool up = ph == kOpen || ph == kChange || ph == kHold || (ph == kClose && now - close_at_ < st.break_ms);
        if (!up || placed_ != g_cfg.popup_position) {  // a new corner (the Position setting) moves it straight there
            Place p = PlaceFor(kGW, kGH, kGM, g_cfg.popup_position);
            placed_ = g_cfg.popup_position;
            s_ = p.s;
            x_ = p.x;
            y_ = p.y;
            surf_.Resize((int)(std::round(kGW * s_) + 2 * std::round(kGM * s_)),
                         (int)(std::round(kGH * s_) + 2 * std::round(kGM * s_)));
        }
        if (!up) {
            t_open_ = now;
            t_change_ = -1e9;
            old_ = Content();
        } else if (ph != kOpen) {
            old_ = Visible(now);
            t_change_ = now;
        }
        cur_ = c;
        seed_ = (uint32_t)(now * 104729.0) ^ 0x5EEDu;
        double anim_end = std::max(t_open_ + glitch::OpenMs(st), t_change_ + st.change_ms);
        close_at_ = std::max({now + kHoldMs, anim_end, up ? close_at_ : 0.0});
        SetTimer(surf_.hwnd, 1, 10, nullptr);
        drawn_ = false;
        Render(now);
    }

    void Hide() {
        t_open_ = -1;
        if (surf_.hwnd) KillTimer(surf_.hwnd, 1);
        surf_.Hide();
    }
    bool Owns(HWND h) const { return h && h == surf_.hwnd; }
    void Tick() { Render(NowMs()); }
    bool Save(const std::wstring& png) { return surf_.Visible() && surf_.SavePng(png); }
    void Destroy() { surf_.Destroy(); }

private:
    enum Phase { kNone, kOpen, kChange, kHold, kClose };

    Phase PhaseAt(double now) const {
        if (t_open_ < 0) return kNone;
        const auto& st = glitch::CurrentStyle();
        if (now >= close_at_) return now < close_at_ + glitch::CloseMs(st) ? kClose : kNone;
        if (now < t_change_ + st.change_ms) return kChange;
        if (now < t_open_ + glitch::OpenMs(st)) return kOpen;
        return kHold;
    }

    Content Visible(double now) const {
        const auto& st = glitch::CurrentStyle();
        Content c = cur_;
        if (!st.enabled) return c;
        Phase ph = PhaseAt(now);
        if (ph == kOpen) {
            double t = now - t_open_;
            glitch::Rng rng = glitch::RngAt(seed_, t, 1);
            c.title = glitch::Decode(cur_.title, glitch::TypeProgress(st, t, 0.0, 0.5), rng);
            c.note = glitch::Decode(cur_.note, glitch::TypeProgress(st, t, 0.35, 0.65), rng, nullptr, true);
            c.value = glitch::Decode(cur_.value, glitch::TypeProgress(st, t, 0.2, 0.8), rng);
            if (c.bar >= 0) c.bar = (float)(cur_.bar * glitch::Steps(glitch::TypeProgress(st, t, 0.2, 0.8), 6));
        } else if (ph == kChange) {
            double p = (now - t_change_) / st.change_ms;
            glitch::Rng rng = glitch::RngAt(seed_, now - t_change_, 2);
            c.title = glitch::Morph(old_.title, cur_.title, p * 1.4, rng);
            c.note = glitch::Morph(old_.note, cur_.note, p * 1.2, rng, false, true);
            c.value = glitch::Morph(old_.value, cur_.value, p, rng, true);  // a repeat press glitches the value only
            if (c.bar >= 0 && old_.bar >= 0) c.bar = (float)(old_.bar + (cur_.bar - old_.bar) * glitch::Steps(p / 0.6, 4));
        } else if (ph == kClose) {
            double t = now - close_at_;
            glitch::Rng rng = glitch::RngAt(seed_, t, 3);
            double amount = 0.15 + 0.85 * glitch::Clamp01(t / std::max(1.0, st.break_ms));
            c.title = glitch::Corrupt(c.title, amount, rng);
            c.note = glitch::Corrupt(c.note, amount, rng);
            c.value = glitch::Corrupt(c.value, amount, rng);
        }
        return c;
    }

    bool Typing(double now) const {
        Phase ph = PhaseAt(now);
        return glitch::CurrentStyle().enabled && (ph == kOpen || ph == kChange);
    }

    void Draw(ID2D1RenderTarget* rt, const Content& c, double now) {
        auto P = [&](float v) { return std::round(v * s_); };
        const float W = P(kGW), px = std::max(1.f, P(1));
        rt->Clear(C(g_pal.paper));
        // The header square carries the temperature's color, when there is one.
        D2D1_COLOR_F mark = c.tinted ? D2D1::ColorF(c.tint.r, c.tint.g, c.tint.b, 1) : C(g_pal.ink);
        Fill(rt, surf_, P(18), P(17), P(24), P(23), mark);
        Text(rt, surf_, c.title, D2D1::RectF(P(32), P(8), P(220), P(32)), {P(13), DWRITE_FONT_WEIGHT_SEMI_BOLD, P(3)}, C(g_pal.ink));
        Text(rt, surf_, c.note, D2D1::RectF(P(150), P(8), W - P(18), P(32)), {P(11), DWRITE_FONT_WEIGHT_NORMAL, P(1.5f), kRight},
             C(g_pal.ink_soft));
        Fill(rt, surf_, P(18), P(34), W - P(18), P(34) + px, C(g_pal.ink));
        TextStyle big{P(28), DWRITE_FONT_WEIGHT_SEMI_BOLD, P(1)};
        Text(rt, surf_, c.value, D2D1::RectF(P(18), P(kValueY0), W - P(18), P(kValueY1)), big, C(g_pal.ink));
        if (Typing(now)) {
            float tw = std::min(TextWidth(c.value, big), W - P(50));
            float cy = (P(kValueY0) + P(kValueY1)) / 2, half = std::round(big.size * 0.38f);
            Fill(rt, surf_, P(18) + tw + P(3), cy - half, P(18) + tw + P(3) + std::round(big.size * 0.4f), cy + half, C(g_pal.ink));
        }
        if (c.bar >= 0) {
            float x0 = P(18), x1 = W - P(18), y0 = P(92), y1 = P(102);
            Frame(rt, surf_, x0, y0, x1, y1, px, C(g_pal.ink));
            float fill = x0 + P(2) + (x1 - x0 - P(4)) * std::clamp(c.bar, 0.f, 1.f);
            Fill(rt, surf_, x0 + P(2), y0 + P(2), fill, y1 - P(2), C(g_pal.ink));
            for (int i = 1; i < 10; i++) {  // a tick every 10%
                float tx = std::round(x0 + (x1 - x0) * i / 10.f);
                Fill(rt, surf_, tx, y1 + P(2), tx + px, y1 + P(5), C(g_pal.ink_soft));
            }
        }
    }

    void Render(double now) {
        const auto& st = glitch::CurrentStyle();
        Phase ph = PhaseAt(now);
        if (ph == kNone) {
            Hide();
            return;
        }
        glitch::Fx fx;
        if (ph == kOpen) {
            fx = glitch::OpenFx(st, now - t_open_, seed_, kGW, kGH);
        } else if (ph == kChange) {
            double p = (now - t_change_) / st.change_ms;
            glitch::Rng rng = glitch::RngAt(seed_, now - t_change_, 5);
            bool full = g_cfg.animation == L"full";
            fx.tears = glitch::Tears(rng, st, 1 - p, full ? 0 : kValueY0, full ? kGH : kValueY1);
            fx.blocks = glitch::Blocks(rng, st, 0.5 * (1 - p), kGW, kGH);
            fx.alpha = glitch::Flicker(rng, st, 0.5 * (1 - p));
        } else if (ph == kClose) {
            fx = glitch::CloseFx(st, now - close_at_, seed_, kGW, kGH);
        }
        Content vis = Visible(now);
        bool typing = Typing(now);
        if (ph == kHold && drawn_ && vis.title == last_.title && vis.value == last_.value && vis.note == last_.note &&
            vis.bar == last_.bar && fx == last_fx_ && typing == last_typing_ && surf_.Visible()) {
            Schedule(now, ph);
            return;
        }
        last_ = vis;
        last_fx_ = fx;
        last_typing_ = typing;
        drawn_ = true;
        float cw = std::round(kGW * s_), ch = std::round(kGH * s_), m = std::round(kGM * s_);
        ID2D1RenderTarget* rt = surf_.Begin();
        if (!rt) return;
        if (auto* scratch = surf_.Scratch((int)cw, (int)ch)) {
            scratch->BeginDraw();
            Draw(scratch, vis, now);
            scratch->EndDraw();
            ID2D1Bitmap* bmp = nullptr;
            if (SUCCEEDED(scratch->GetBitmap(&bmp))) {
                Compose(rt, surf_, bmp, m, m, cw, ch, fx, s_);
                bmp->Release();
            }
        }
        surf_.End(x_, y_, fx.alpha);
        Schedule(now, ph);
    }

    // The next frame: every 10 ms while it animates; while it just sits
    // there, nothing moves until it starts closing.
    void Schedule(double now, Phase ph) {
        double next = ph == kHold ? close_at_ - now : 10;
        SetTimer(surf_.hwnd, 1, (UINT)std::clamp(next, 10.0, 1000.0), nullptr);
    }

    static LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        auto* self = (Glitchy*)GetWindowLongPtrW(h, GWLP_USERDATA);
        if (msg == WM_TIMER && self) {
            self->Tick();
            return 0;
        }
        if (msg == WM_NCHITTEST) return HTTRANSPARENT;
        return DefWindowProcW(h, msg, wp, lp);
    }

    Surface surf_;
    Content cur_, old_, last_;
    glitch::Fx last_fx_;
    bool last_typing_ = false, drawn_ = false;
    double t_open_ = -1, t_change_ = -1e9, close_at_ = 0;
    uint32_t seed_ = 1;
    float s_ = 1;
    int x_ = 0, y_ = 0;
    std::wstring placed_;  // the corner it was placed in
};

Glitchy g_glitchy;

}  // namespace

bool Init() {
    g_glitchy.Init();
    return true;
}

void Show(const Content& c) {
    // Over a game, anything drawn on top can cost a frame or knock it out of
    // exclusive fullscreen. The change itself still happens.
    if (!g_cfg.popup_in_games && GameInFront()) return;
    if (core::MediaCorner(g_cfg) == g_cfg.popup_position) media::Hide();  // same corner: one at a time
    g_glitchy.Show(c);
}

void Hide() { g_glitchy.Hide(); }

bool IsOwnWindow(HWND h) { return g_glitchy.Owns(h); }

void DebugTick() { g_glitchy.Tick(); }

bool DebugSave(const std::wstring& png) { return g_glitchy.Save(png); }

void Shutdown() { g_glitchy.Destroy(); }

}  // namespace popup
