// The settings window: sections as tabs, the section's settings as a list,
// and a framed panel explaining whichever one is selected.
//
// Made to read at a glance, without giving up the look:
// - Today's schedule runs across the top, always: a flat 24-hour band, each
//   part in the warmth it puts on screen, with the switch times under it and
//   a wedge at now. Clicking a part goes to its setting.
// - Every row shows what kind of control it is: an ON | OFF pair, a block
//   gauge for temperatures and percentages, ‹ VALUE › with a pip per choice,
//   a field for what you type, keycaps for hotkeys, a framed button for what
//   runs something, and plain text for what only reports.
//
// Hover selects, click changes (right-click goes backwards), and the
// keyboard works too: arrows, Enter/Space, Tab for the next section, Esc to
// close. Every change is saved straight away.
#include <algorithm>
#include <cmath>
#include <cwctype>

#include "settings_ui.h"

namespace ui {

using namespace gfx;

namespace {

constexpr float kW = 1040, kH = 730, kMargin = 28;
constexpr float kPad = 40;
constexpr float kStripY = 84, kStripH = 26;            // today's band; its labels sit under it
constexpr float kTabY = 136, kTabH = 38;
constexpr float kBodyY = 192, kBodyY1 = 666;           // ten rows
constexpr float kRowX0 = 62, kRowX1 = 600, kRowH = 42, kRowGap = 6;
constexpr float kPanX0 = 628, kPanX1 = 1000;
constexpr float kMiniW = 220, kMiniH = 84;

std::wstring Upper(std::wstring s) {
    CharUpperBuffW(s.data(), (DWORD)s.size());
    return s;
}

D2D1_COLOR_F Mix(const Color& a, const Color& b, float p, float alpha = 1.f) {
    return D2D1::ColorF(a.r + (b.r - a.r) * p, a.g + (b.g - a.g) * p, a.b + (b.b - a.b) * p, alpha);
}

enum Act { kTab, kRow, kReplay, kClose, kRemove, kAdd, kStrip };
struct Hit {
    D2D1_RECT_F r;  // unscaled panel coordinates
    Act act;
    int a;
    std::wstring key;  // kStrip: the setting it goes to
};

class SettingsWindow : public Window {
public:
    void Open(const POINT* center) override {
        if (!EnsureWindow()) return;
        HWND front = LastUserForeground();
        front_ = front ? ProcessExeName(front) : L"";
        if (!_wcsicmp(front_.c_str(), L"explorer.exe")) front_.clear();
        Rebuild();
        double now = NowMs();
        if (open_ && !closing_) {
            if (IsIconic(win_.hwnd)) ShowWindow(win_.hwnd, SW_RESTORE);
            SetForegroundWindow(win_.hwnd);
            Render(now);
            return;
        }
        open_ = true;
        closing_ = false;
        strip_tz_++;  // the clock or time zone may have changed while it was closed
        t_open_ = t_preview_ = now;
        seed_ = (uint32_t)(now * 7727.0) ^ 0x5E77u;
        changes_.clear();
        Placement p = Place(kW, kH, kMargin, center);
        s_ = p.s;
        win_.Resize(p.w, p.h);
        SetWindowPos(win_.hwnd, nullptr, p.x, p.y, p.w, p.h, SWP_NOZORDER | SWP_NOACTIVATE);
        SetTimer(win_.hwnd, 1, 10, nullptr);
        Render(now);
        SetForegroundWindow(win_.hwnd);
    }

    // A section ("system"), or a setting ("lat") to start on.
    void ShowSection(const std::wstring& key) override {
        secs_ = opts::Build();
        for (int i = 0; i < (int)secs_.size(); i++) {
            if (secs_[i].key == key) {
                cat_ = i;
                sel_ = 0;
            }
            for (int r = 0; r < (int)secs_[i].opts.size(); r++)
                if (secs_[i].opts[r].key == key) {
                    cat_ = i;
                    sel_ = r;
                }
        }
    }

    void Close(bool animate) override {
        if (!open_) return;
        opts::EndEdit(EditingOption(), true);
        opts::StopRecording();
        if (!animate) {
            open_ = closing_ = false;
            KillTimer(win_.hwnd, 1);
            win_.Hide();
            return;
        }
        if (!closing_) {
            closing_ = true;
            t_close_ = NowMs();
            SetTimer(win_.hwnd, 1, 10, nullptr);
        }
    }

    bool IsOpen() const override { return open_ && !closing_; }
    bool Owns(HWND h) const override { return h && h == win_.hwnd; }
    POINT Center() const override {
        RECT r;
        GetWindowRect(win_.hwnd, &r);
        return {(r.left + r.right) / 2, (r.top + r.bottom) / 2};
    }
    void Refresh() override {
        if (!open_) return;
        strip_tz_++;  // the band is worked out again too (a clock change lands here)
        Rebuild();
        Render(NowMs());
    }
    void Shutdown() override {
        if (mini_) mini_->Release();
        mini_ = nullptr;
        win_.Destroy();
    }

    void DebugDump(const std::wstring& dir) override {
        if (!EnsureWindow()) return;
        Open(nullptr);
        front_ = L"game.exe";
        seed_ = 99;
        wchar_t name[80];
        auto tab = [&](const wchar_t* key) {
            for (int i = 0; i < (int)secs_.size(); i++)
                if (secs_[i].key == key) return i;
            return 0;
        };
        for (int i = 0; i < 120; i++) {
            double now = NowMs();
            if (i == 40) Select(2, now);
            if (i == 55) SwitchTab(tab(L"switcher"), now);
            if (i == 70) SwitchTab(tab(L"hotkeys"), now);
            if (i == 85) SwitchTab(tab(L"popups"), now);
            if (i == 100) {
                SwitchTab(0, now);
                Select(3, now);
                opts::g_edit.key = L"night";
                opts::g_edit.buffer = L"23:";
            }
            if (i == 110) {
                opts::g_edit.key.clear();
                SwitchTab(tab(L"system"), now);
            }
            Render(now);
            swprintf(name, 80, L"\\settings_%04d.png", i);
            win_.SavePng(dir + name);
            g_fake_now += 16;
        }
        // Switching the language there and back (settings_lang_*).
        SwitchTab(tab(L"system"), NowMs());
        for (int r = 0; r < (int)Cat().opts.size(); r++)
            if (Cat().opts[r].key == L"language") Select(r, NowMs());
        for (int i = 0; i < 80; i++) {
            if (i == 10 || i == 50) Activate(1);
            Render(NowMs());
            swprintf(name, 80, L"\\settings_lang_%04d.png", i);
            win_.SavePng(dir + name);
            g_fake_now += 16;
        }
        // Then every setting once, settled, to check each description fits.
        for (int c = 0; c < (int)secs_.size(); c++) {
            SwitchTab(c, NowMs());
            for (int r = 0; r < (int)Cat().opts.size(); r++) {
                sel_ = -1;
                Select(r, NowMs());
                g_fake_now += 1500;
                Render(NowMs());
                swprintf(name, 80, L"\\settings_option_%d_%02d.png", c, r);
                win_.SavePng(dir + name);
            }
        }
        Close(false);
    }

private:
    bool EnsureWindow() {
        if (win_.hwnd) return true;
        if (!win_.Create(L"CandelaSettings", Proc, this, 0, nullptr, false)) return false;
        SetWindowTextW(win_.hwnd, L"Candela");
        SetIcons(win_.hwnd);
        return true;
    }

    void Rebuild() {
        secs_ = opts::Build();
        cat_ = std::clamp(cat_, 0, (int)secs_.size() - 1);
        sel_ = std::clamp(sel_, 0, (int)Cat().opts.size() - 1);
    }

    const opts::Section& Cat() const { return secs_[cat_]; }
    const opts::Option& Sel() const { return Cat().opts[std::clamp(sel_, 0, (int)Cat().opts.size() - 1)]; }
    const opts::Option* Find(const std::wstring& key) const {
        for (auto& s : secs_)
            for (auto& o : s.opts)
                if (o.key == key) return &o;
        return nullptr;
    }
    const opts::Option* EditingOption() const { return opts::g_edit.Active() ? Find(opts::g_edit.key) : nullptr; }
    const opts::Option* RecordingOption() const { return opts::g_recording.empty() ? nullptr : Find(opts::g_recording); }

    float P(float v) const { return std::round(v * s_); }
    float Px() const { return std::max(1.f, P(1)); }
    // Switching language glitches this long (none with the animation off).
    static double LangMs(const glitch::Style& st) { return st.enabled ? st.decode_ms + st.expand_ms : 0; }

    bool Animating(double now) const {
        const auto& st = glitch::CurrentStyle();
        if (closing_) return true;
        double ch = std::max(st.change_ms, 200.0);
        if (now - t_open_ < glitch::OpenMs(st) + 60) return true;
        if (now - t_tab_ < 360 || now - t_sel_ < 320 || now - t_preview_ < glitch::OpenMs(st) + 60) return true;
        if (now - t_lang_ < LangMs(st) + 60) return true;
        for (auto& c : changes_)
            if (now - c.t < ch) return true;
        return false;
    }

    // ---- drawing helpers ---------------------------------------------------------------------------------
    void Hitbox(float x0, float y0, float x1, float y1, Act act, int a, const std::wstring& key = L"") {
        hits_.push_back({D2D1::RectF(x0, y0, x1, y1), act, a, key});
    }

    void Brackets(ID2D1RenderTarget* rt, float x0, float y0, float x1, float y1, D2D1_COLOR_F c) {
        float L = P(10), T = std::max(1.f, P(2)), o = P(5);
        x0 -= o; y0 -= o; x1 += o; y1 += o;
        Fill(rt, win_, x0, y0, x0 + L, y0 + T, c); Fill(rt, win_, x0, y0, x0 + T, y0 + L, c);
        Fill(rt, win_, x1 - L, y0, x1, y0 + T, c); Fill(rt, win_, x1 - T, y0, x1, y0 + L, c);
        Fill(rt, win_, x0, y1 - T, x0 + L, y1, c); Fill(rt, win_, x0, y1 - L, x0 + T, y1, c);
        Fill(rt, win_, x1 - L, y1 - T, x1, y1, c); Fill(rt, win_, x1 - T, y1 - L, x1, y1, c);
    }

    void Wedge(ID2D1RenderTarget* rt, float cx, float cy, float w, float h, bool down, D2D1_COLOR_F c) {
        rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        ID2D1PathGeometry* geo = nullptr;
        if (FAILED(d2d->CreatePathGeometry(&geo))) return;
        ID2D1GeometrySink* sink = nullptr;
        if (SUCCEEDED(geo->Open(&sink))) {
            if (down) {
                sink->BeginFigure(D2D1::Point2F(cx - w / 2, cy - h / 2), D2D1_FIGURE_BEGIN_FILLED);
                D2D1_POINT_2F pts[] = {{cx + w / 2, cy - h / 2}, {cx, cy + h / 2}};
                sink->AddLines(pts, 2);
            } else {  // the cursor: pointing right, with a notch
                sink->BeginFigure(D2D1::Point2F(cx, cy - h / 2), D2D1_FIGURE_BEGIN_FILLED);
                D2D1_POINT_2F pts[] = {{cx + w, cy}, {cx, cy + h / 2}, {cx + w * 0.28f, cy}};
                sink->AddLines(pts, 3);
            }
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            sink->Close();
            sink->Release();
            if (auto* b = win_.Brush(c)) rt->FillGeometry(geo, b);
        }
        geo->Release();
        rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
    }

    // The value as text, scrambling from what it was if it just changed.
    std::wstring Shown(const opts::Option& o, const std::wstring& v, double now) const {
        const auto& st = glitch::CurrentStyle();
        for (auto& c : changes_)
            if (c.key == o.key && st.enabled) {
                double p = (now - c.t) / std::max(st.change_ms, 200.0);
                if (p < 1) {
                    glitch::Rng rng = glitch::RngAt(seed_, now - c.t, 11);
                    return glitch::Morph(c.old, v, p, rng, true);
                }
            }
        return v;
    }

    // A small framed box with text in it (a keycap, a field, a button). Returns its width.
    float Box(ID2D1RenderTarget* rt, float x1, float cy, const std::wstring& text, D2D1_COLOR_F fg, bool filled, D2D1_COLOR_F fill_text,
              float min_w = 0, float pad = 10, float h = 24) {
        TextStyle ts{P(11), DWRITE_FONT_WEIGHT_SEMI_BOLD, P(1.5f), kCenter};
        float w = std::max(P(min_w), TextWidth(text, ts) + 2 * P(pad));
        float x0 = x1 - w, y0 = cy - P(h) / 2, y1 = y0 + P(h);
        if (filled) Fill(rt, win_, x0, y0, x1, y1, fg);
        else Frame(rt, win_, x0, y0, x1, y1, Px(), fg);
        Text(rt, win_, text, D2D1::RectF(x0, y0, x1, y1), ts, filled ? fill_text : fg);
        return w;
    }

    // ---- what each row shows on its right -------------------------------------------------------------------------
    // fg: the row's ink (paper on the selected row); bg: the row's own color.
    // Returns where the control starts, so the label stops short of it.
    float DrawControl(ID2D1RenderTarget* rt, const opts::Option& o, float y0, float y1, D2D1_COLOR_F fg, D2D1_COLOR_F bg, bool on_row,
                      double now) {
        const float x1 = P(kRowX1 - 14), cy = (y0 + y1) / 2, px = Px();
        D2D1_COLOR_F soft = fg;
        soft.a *= 0.55f;
        TextStyle val{P(13), DWRITE_FONT_WEIGHT_NORMAL, P(2), kRight};
        // ‹ VALUE ›, the arrows drawn rather than typed (not every Japanese font has them).
        auto chevron = [&](float cx, bool right) {
            float w = P(3), h = P(4), t = std::max(1.f, P(1.2f));
            float tip = right ? cx + w / 2 : cx - w / 2, back = right ? cx - w / 2 : cx + w / 2;
            Line(rt, win_, back, cy - h, tip, cy, t, fg);
            Line(rt, win_, tip, cy, back, cy + h, t, fg);
        };
        auto arrows_text = [&](const std::wstring& v, float right) {
            std::wstring s = Shown(o, Upper(v), now);
            float w = TextWidth(s, val), gap = P(14);
            chevron(right - P(4), true);
            float tx1 = right - gap;
            Text(rt, win_, s, D2D1::RectF(tx1 - w - P(4), y0, tx1, y1), val, fg);
            chevron(tx1 - w - gap + P(4), false);
            return w + 2 * gap;
        };
        // Being typed or recorded: a field with the text and a blinking cursor.
        if (opts::g_edit.key == o.key || opts::g_recording == o.key) {
            bool rec = opts::g_recording == o.key;
            std::wstring t = rec ? (std::fmod(now, 800.0) < 520 ? Tr(L"PRESS KEYS", L"キー入力待機中") : L"")
                                 : opts::g_edit.buffer + (std::fmod(now, 1000.0) < 500 ? L"_" : L" ");
            float w = P(rec ? 170 : 150), bx0 = x1 - w;
            Fill(rt, win_, bx0, cy - P(13), x1, cy + P(13), fg);
            Text(rt, win_, t, D2D1::RectF(bx0 + P(10), cy - P(13), x1 - P(10), cy + P(13)), {P(13), DWRITE_FONT_WEIGHT_SEMI_BOLD, P(1.5f)}, bg);
            return bx0;
        }
        switch (o.kind) {
            case opts::kToggle: {
                // ON | OFF, the one that's set filled in.
                bool is_on = o.on ? o.on() : false;
                TextStyle ts{P(11), DWRITE_FONT_WEIGHT_SEMI_BOLD, P(2), kCenter};
                float cw = P(44), ch = P(22), top = cy - ch / 2;
                float off_x0 = x1 - cw, on_x0 = off_x0 - cw;
                Frame(rt, win_, on_x0, top, x1, top + ch, px, soft);
                Fill(rt, win_, is_on ? on_x0 : off_x0, top, is_on ? off_x0 : x1, top + ch, fg);
                Text(rt, win_, L"ON", D2D1::RectF(on_x0, top, off_x0, top + ch), ts, is_on ? bg : soft);
                Text(rt, win_, L"OFF", D2D1::RectF(off_x0, top, x1, top + ch), ts, is_on ? soft : bg);
                return on_x0;
            }
            case opts::kSlider: {
                // ‹ 4800K › after a gauge of blocks.
                float tw = arrows_text(o.value(), x1);
                const int n = 16;
                float bw = P(5), gap = P(2), gw = n * bw + (n - 1) * gap;
                float gx1 = x1 - tw - P(14), gx0 = gx1 - gw;
                double f = o.get ? std::clamp((o.get() - o.lo) / (o.hi - o.lo), 0.0, 1.0) : 0;
                int filled = (int)std::lround(f * n);
                if (f > 0 && filled == 0) filled = 1;
                for (int i = 0; i < n; i++) {
                    float bx = gx0 + i * (bw + gap);
                    if (i < filled) Fill(rt, win_, bx, cy - P(6), bx + bw, cy + P(6), fg);
                    else Fill(rt, win_, bx, cy - P(1), bx + bw, cy + P(1), soft);
                }
                if (o.kelvin && o.get) {  // the warmth itself, as a swatch
                    Color t = FromTint(core::Tint((int)o.get(), false));
                    float sx1 = gx0 - P(10), sx0 = sx1 - P(10);
                    Fill(rt, win_, sx0, cy - P(5), sx1, cy + P(5), D2D1::ColorF(t.r, t.g, t.b, fg.a));
                    return sx0;
                }
                return gx0;
            }
            case opts::kChoice: {
                float tw = arrows_text(o.value(), x1);
                // A pip per choice, the current one filled.
                int n = (int)o.choices.size(), cur = 0;
                for (int i = 0; i < n; i++)
                    if (o.current && o.choices[i].value == o.current()) cur = i;
                float ps = P(6), pg = P(4), pw = n * ps + (n - 1) * pg;
                float px1 = x1 - tw - P(12), px0 = px1 - pw;
                for (int i = 0; i < n; i++) {
                    float bx = px0 + i * (ps + pg);
                    if (i == cur) Fill(rt, win_, bx, cy - ps / 2, bx + ps, cy + ps / 2, fg);
                    else Frame(rt, win_, bx, cy - ps / 2, bx + ps, cy + ps / 2, px, soft);
                }
                return px0;
            }
            case opts::kStepper:
            case opts::kTime: return x1 - arrows_text(o.value(), x1);
            case opts::kText: {
                // A field: framed, with an underline where you'd type.
                float w = P(150), bx0 = x1 - w;
                Frame(rt, win_, bx0, cy - P(13), x1, cy + P(13), px, soft);
                Fill(rt, win_, bx0, cy + P(13) - P(2), x1, cy + P(13), fg);
                Text(rt, win_, Shown(o, o.value(), now), D2D1::RectF(bx0 + P(10), cy - P(13), x1 - P(10), cy + P(13)),
                     {P(13), DWRITE_FONT_WEIGHT_NORMAL, P(1.5f), kRight}, fg);
                return bx0;
            }
            case opts::kHotkey: {
                // Keycaps: [ALT] + [SHIFT] + [PGDN].
                std::wstring keys = o.value();
                std::vector<std::wstring> parts;
                if (o.hotkey && !(g_cfg.*(o.hotkey)).empty()) {
                    std::wstring part;
                    for (wchar_t c : keys + L"+") {
                        if (c == L'+') {
                            if (!part.empty()) parts.push_back(Upper(part));
                            part.clear();
                        } else {
                            part += c;
                        }
                    }
                }
                if (parts.empty()) {
                    Text(rt, win_, Upper(keys), D2D1::RectF(x1 - P(200), y0, x1, y1), val, soft);
                    return x1 - TextWidth(Upper(keys), val);
                }
                float x = x1;
                TextStyle plus{P(11), DWRITE_FONT_WEIGHT_NORMAL, 0, kCenter};
                for (int i = (int)parts.size() - 1; i >= 0; i--) {
                    x -= Box(rt, x, cy, parts[i], fg, false, bg, 30, 8, 24);
                    if (i > 0) {
                        Text(rt, win_, L"+", D2D1::RectF(x - P(16), y0, x, y1), plus, soft);
                        x -= P(16);
                    }
                }
                return x;
            }
            case opts::kAction: {
                // A framed button; Quit fills in once it's armed.
                bool armed = o.danger && opts::QuitArmed();
                std::wstring label = Shown(o, Upper(o.value()), now);
                float w = Box(rt, x1, cy, label, fg, armed, bg, 84, 14, 26);
                if (o.accent) {  // Darkroom: its red in the corner
                    Color red = FromTint(core::Tint(core::kDarkroom, false));
                    Fill(rt, win_, x1 - w + P(5), cy - P(13) + P(5), x1 - w + P(10), cy - P(13) + P(10), D2D1::ColorF(red.r, red.g, red.b, fg.a));
                }
                return x1 - w;
            }
            case opts::kList: {
                // "3 GAMES ›": the list itself is in the panel.
                std::wstring s = Upper(o.value()) + L"  ›";
                Text(rt, win_, s, D2D1::RectF(x1 - P(200), y0, x1, y1), val, fg);
                return x1 - TextWidth(s, val);
            }
            case opts::kInfo:
            default: {
                std::wstring s = Upper(o.value());
                Text(rt, win_, s, D2D1::RectF(x1 - P(300), y0, x1, y1), val, on_row ? fg : soft);
                return x1 - TextWidth(s, val);
            }
        }
    }

    // ---- today's schedule, across the top ------------------------------------------------------------------------
    // Each part in the warmth it puts on screen, toned down to sit in the palette.
    D2D1_COLOR_F PartColor(core::PhaseName p) const {
        int k = core::ScheduledKelvin(p, g_cfg);
        Color tint = FromTint(core::Tint(k, false));
        if (p == core::kDay) return Mix(g_pal.ink, tint, 0.35f);
        if (p == core::kEvening) return Mix(g_pal.paper_dark, tint, 0.62f);
        return Mix(g_pal.paper_dark, tint, 0.45f);
    }

    void DrawStrip(ID2D1RenderTarget* rt) {
        const float px = Px(), W = P(kW);
        const float x0 = P(kPad), x1 = W - P(kPad), y0 = P(kStripY), y1 = y0 + P(kStripH);
        core::LocalTime day0 = core::Midnight(NowLocal());
        const auto& events = TodaysEvents(day0);
        auto X = [&](double t) { return x0 + (x1 - x0) * (float)std::clamp((t - day0) / 86400.0, 0.0, 1.0); };
        Fill(rt, win_, x0, y0, x1, y1, C(g_pal.paper_dark));
        TextStyle seg{P(10), DWRITE_FONT_WEIGHT_SEMI_BOLD, P(2.5f), kCenter};
        TextStyle tick{P(10), DWRITE_FONT_WEIGHT_NORMAL, P(1.5f), kCenter};
        std::vector<std::pair<float, std::wstring>> marks;  // x, label under the band
        for (size_t i = 0; i < events.size(); i++) {
            double a = std::max(events[i].start, day0);
            double b = std::min(i + 1 < events.size() ? events[i + 1].start : day0 + 172800, day0 + 86400);
            if (b <= a) continue;
            float sx0 = std::round(X(a)), sx1 = std::round(X(b));
            core::PhaseName p = events[i].name;
            D2D1_COLOR_F fill = PartColor(p);
            Fill(rt, win_, sx0, y0, sx1, y1, fill);
            // Its name and warmth, as much as fits.
            int k = core::ScheduledKelvin(p, g_cfg);
            std::wstring name = Upper(lang::Phase(p)), warmth = Upper(lang::Kelvin(k));
            D2D1_COLOR_F text = p == core::kDay ? C(g_pal.paper) : C(g_pal.ink);
            for (const std::wstring& label : {name + L" · " + warmth, warmth, std::wstring()}) {
                if (label.empty() || TextWidth(label, seg) + P(16) > sx1 - sx0) continue;
                Text(rt, win_, label, D2D1::RectF(sx0, y0, sx1, y1), seg, text);
                break;
            }
            std::wstring go = p == core::kDay ? L"day" : p == core::kEvening ? L"evening" : L"night";
            Hitbox(sx0 / s_, kStripY, sx1 / s_, kStripY + kStripH, kStrip, 0, go);
            if (events[i].start > day0 && events[i].start < day0 + 86400) {
                std::wstring word = p == core::kDay ? (g_cfg.morning_mode == L"fixed" ? Tr(L"DAY", L"日中") : Tr(L"SUNRISE", L"日の出"))
                                    : p == core::kEvening ? (g_cfg.sunset_offset_min == 0 ? Tr(L"SUNSET", L"日の入") : Tr(L"EVENING", L"夕刻"))
                                                          : Tr(L"NIGHT", L"夜間");
                marks.push_back({sx0, word + L" " + core::FormatHM(events[i].start)});
                Fill(rt, win_, sx0, y0, sx0 + px, y1 + P(5), C(g_pal.ink));
            }
        }
        Frame(rt, win_, x0, y0, x1, y1, px, C(g_pal.ink_soft));
        // Times under the band: midnight at the ends, the switches where they are.
        float ly0 = y1 + P(5), ly1 = y1 + P(21);
        Text(rt, win_, L"00:00", D2D1::RectF(x0, ly0, x0 + P(60), ly1), {tick.size, tick.weight, tick.spacing, kLeft}, C(g_pal.ink_soft));
        Text(rt, win_, L"24:00", D2D1::RectF(x1 - P(60), ly0, x1, ly1), {tick.size, tick.weight, tick.spacing, kRight}, C(g_pal.ink_soft));
        // Each label centred on its switch, kept off the ends' times and the one
        // before it: the time alone when the words don't fit, nothing when even that doesn't.
        float last_end = x0 + P(50), right_limit = x1 - P(52);
        for (auto& [mx, label] : marks) {
            for (const std::wstring& shown : {label, label.substr(label.size() - 5)}) {
                float w = TextWidth(shown, tick);
                float a = std::max(mx - w / 2, x0 + P(52));
                if (a + w > right_limit) a = right_limit - w;
                if (a < last_end + P(8)) continue;
                Text(rt, win_, shown, D2D1::RectF(a - P(2), ly0, a + w + P(2), ly1), tick, C(g_pal.ink));
                last_end = a + w;
                break;
            }
        }
        // Now: a wedge above the band and a line through it.
        float nx = std::round(X(NowLocal()));
        Fill(rt, win_, nx - px, y0 - P(2), nx + px, y1 + P(2), C(g_pal.ink));
        Wedge(rt, nx, y0 - P(7), P(11), P(8), true, C(g_pal.ink));
    }

    // Today's switches for the band. Working them out takes a dozen time-zone
    // lookups, and the window draws up to 60 frames a second, so they're kept
    // until the day or one of the settings they depend on changes.
    const std::vector<core::Phase>& TodaysEvents(core::LocalTime day0) {
        const core::Config& c = g_cfg;
        std::wstring key = std::to_wstring(day0) + L"|" + std::to_wstring(c.latitude) + L"|" + std::to_wstring(c.longitude) +
                           L"|" + std::to_wstring(c.sunset_offset_min) + L"|" + c.morning_mode + L"|" + c.morning_time + L"|" +
                           c.night_start + L"|" + std::to_wstring(strip_tz_);
        if (key != strip_key_) {
            strip_events_ = core::Timeline(day0 + 43200, c);
            strip_key_ = key;
        }
        return strip_events_;
    }

    // ---- the side panel's little popup -------------------------------------------------------------------------
    void DrawMini(ID2D1RenderTarget* rt, opts::Preview kind, double now) {
        const float w = P(kMiniW), px = Px();
        rt->Clear(C(g_pal.paper));
        const auto& st = glitch::CurrentStyle();
        double t = now - t_preview_;
        glitch::Rng rng = glitch::RngAt(seed_, t, 21);
        auto typed = [&](const std::wstring& s, double delay, double len) { return glitch::Decode(s, glitch::TypeProgress(st, t, delay, len), rng); };
        if (kind == opts::kWarmthPreview) {
            const core::Status& status = engine::GetStatus();
            Color tint = FromTint(core::Tint(status.kelvin, status.disabled));
            Fill(rt, win_, P(12), P(13), P(17), P(18), D2D1::ColorF(tint.r, tint.g, tint.b, 1));
            Text(rt, win_, typed(Tr(L"WARMTH", L"色温度"), 0, 0.5), D2D1::RectF(P(24), P(6), P(150), P(26)), {P(10), DWRITE_FONT_WEIGHT_SEMI_BOLD, P(3)},
                 C(g_pal.ink));
            Text(rt, win_, typed(Tr(L"SCHEDULE", L"スケジュール通り"), 0.3, 0.6), D2D1::RectF(P(110), P(6), w - P(12), P(26)),
                 {P(9), DWRITE_FONT_WEIGHT_NORMAL, P(1), kRight}, C(g_pal.ink_soft));
            Fill(rt, win_, P(12), P(27), w - P(12), P(27) + px, C(g_pal.ink));
            Text(rt, win_, typed(Upper(lang::Kelvin(status.kelvin)), 0.2, 0.6), D2D1::RectF(P(12), P(30), w - P(12), P(60)),
                 {P(20), DWRITE_FONT_WEIGHT_SEMI_BOLD, P(1)}, C(g_pal.ink));
            float f = (float)(status.kelvin - core::kDarkroom) / (core::kMaxK - core::kDarkroom);
            Frame(rt, win_, P(12), P(64), w - P(12), P(72), px, C(g_pal.ink));
            Fill(rt, win_, P(14), P(66), P(14) + (w - P(28)) * std::clamp(f, 0.f, 1.f), P(70), C(g_pal.ink));
            return;
        }
        bool media = kind == opts::kMediaPreview;
        Fill(rt, win_, P(12), P(13), P(17), P(18), C(g_pal.ink));
        Text(rt, win_, typed(media ? Tr(L"PLAYING", L"再生中") : Tr(L"AUDIO", L"音量"), 0, 0.5), D2D1::RectF(P(24), P(6), P(150), P(26)),
             {P(10), DWRITE_FONT_WEIGHT_SEMI_BOLD, P(3)}, C(g_pal.ink));
        Text(rt, win_, typed(media ? L"SPOTIFY" : Tr(L"SPEAKERS", L"スピーカー"), 0.3, 0.6), D2D1::RectF(P(120), P(6), w - P(12), P(26)),
             {P(9), DWRITE_FONT_WEIGHT_NORMAL, P(1), kRight}, C(g_pal.ink_soft));
        Fill(rt, win_, P(12), P(27), w - P(12), P(27) + px, C(g_pal.ink));
        if (media) {
            float tx = g_cfg.media_art ? P(60) : P(12);
            if (g_cfg.media_art) {
                Fill(rt, win_, P(12), P(35), P(52), P(75), C(g_pal.paper_dark));
                Frame(rt, win_, P(12), P(35), P(52), P(75), px, C(g_pal.ink));
            }
            Text(rt, win_, typed(Tr(L"Song title", L"曲名"), 0.2, 0.6), D2D1::RectF(tx, P(35), w - P(12), P(55)), {P(14), DWRITE_FONT_WEIGHT_SEMI_BOLD, 0},
                 C(g_pal.ink));
            Text(rt, win_, typed(Tr(L"Artist", L"アーティスト"), 0.35, 0.5), D2D1::RectF(tx, P(54), w - P(12), P(68)), {P(10), DWRITE_FONT_WEIGHT_NORMAL, 0},
                 C(g_pal.ink_soft));
            Fill(rt, win_, tx, P(70), w - P(12), P(73), C(g_pal.paper_dark));
            Fill(rt, win_, tx, P(70), tx + (w - P(12) - tx) * 0.35f, P(73), C(g_pal.ink));
        } else {
            Text(rt, win_, typed(L"46%", 0.2, 0.6), D2D1::RectF(P(12), P(30), w - P(12), P(60)), {P(20), DWRITE_FONT_WEIGHT_SEMI_BOLD, P(1)},
                 C(g_pal.ink));
            Frame(rt, win_, P(12), P(64), w - P(12), P(72), px, C(g_pal.ink));
            Fill(rt, win_, P(14), P(66), P(14) + (w - P(28)) * 0.46f, P(70), C(g_pal.ink));
        }
    }

    void DrawPreview(ID2D1RenderTarget* rt, opts::Preview kind, float x, float y, double now) {
        int w = (int)P(kMiniW), h = (int)P(kMiniH);
        if (!mini_ || mini_gen_ != win_.generation || mini_w_ != w || mini_h_ != h) {
            if (mini_) mini_->Release();
            mini_ = nullptr;
            if (!win_.Target()) return;
            D2D1_PIXEL_FORMAT pf = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
            if (FAILED(win_.Target()->CreateCompatibleRenderTarget(D2D1::SizeF((float)w, (float)h), D2D1::SizeU(w, h), pf,
                                                                   D2D1_COMPATIBLE_RENDER_TARGET_OPTIONS_NONE, &mini_)))
                return;
            mini_->SetDpi(96.f, 96.f);
            mini_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
            mini_gen_ = win_.generation;
            mini_w_ = w;
            mini_h_ = h;
        }
        mini_->BeginDraw();
        DrawMini(mini_, kind, now);
        mini_->EndDraw();
        ID2D1Bitmap* bmp = nullptr;
        if (FAILED(mini_->GetBitmap(&bmp))) return;
        const auto& st = glitch::CurrentStyle();
        double t = now - t_preview_;
        glitch::Fx fx;
        if (t < glitch::OpenMs(st)) fx = glitch::OpenFx(st, t, seed_ ^ 0x77u, kMiniW, kMiniH);
        D2D1_COLOR_F back = C(g_pal.paper);
        if (!st.enabled && t < 200) rt->DrawBitmap(bmp, D2D1::RectF(x, y, x + w, y + h), (float)glitch::Clamp01(t / 200.0));
        else Compose(rt, win_, bmp, x, y, (float)w, (float)h, fx, s_, true, &back);
        bmp->Release();
    }

    // ---- the window ------------------------------------------------------------------------------------------------
    void DrawHeader(ID2D1RenderTarget* rt) {
        const float W = P(kW), px = Px();
        const core::Status& status = engine::GetStatus();
        Color tint = FromTint(core::Tint(status.kelvin, status.disabled));
        Fill(rt, win_, P(kPad), P(41), P(kPad + 10), P(51), D2D1::ColorF(tint.r, tint.g, tint.b, 1));
        // The logo is the same in both languages: the font setting, widely spaced.
        TextStyle logo{P(26), DWRITE_FONT_WEIGHT_MEDIUM, P(8), kLeft, false, 0, g_cfg.font.c_str()};
        Text(rt, win_, L"CANDELA", D2D1::RectF(P(60), P(26), P(330), P(66)), logo, C(g_pal.ink));
        float lw = TextWidth(L"CANDELA", logo);
        Text(rt, win_, Tr(L"SETTINGS", L"設定"), D2D1::RectF(P(60) + lw + P(10), P(32), P(520), P(66)), {P(12), DWRITE_FONT_WEIGHT_NORMAL, P(3)},
             C(g_pal.ink_soft));
        // What's on screen now, and until when; the keyboard's state when it isn't the usual.
        std::wstring stat = Upper(opts::StatusLine()), until = Upper(opts::UntilLine()), keys = engine::KeyboardStatus();
        if (keys != L"ONLINE" && !engine::Paused()) until += L"  ·  " + keys;
        float sx1 = W - P(kPad + 48);
        Text(rt, win_, stat, D2D1::RectF(P(420), P(28), sx1, P(48)), {P(12), DWRITE_FONT_WEIGHT_SEMI_BOLD, P(3), kRight}, C(g_pal.ink));
        Text(rt, win_, until, D2D1::RectF(P(300), P(47), sx1, P(64)), {P(10), DWRITE_FONT_WEIGHT_NORMAL, P(2), kRight}, C(g_pal.ink_soft));
        float cx0 = W - P(kPad + 32), cy0 = P(30), cx1 = W - P(kPad), cy1 = P(62);
        bool hot = hover_act_ == kClose;
        if (hot) Fill(rt, win_, cx0, cy0, cx1, cy1, C(g_pal.ink));
        else Frame(rt, win_, cx0, cy0, cx1, cy1, px, C(g_pal.ink_soft));
        D2D1_COLOR_F xc = C(hot ? g_pal.paper : g_pal.ink);
        Line(rt, win_, cx0 + P(11), cy0 + P(11), cx1 - P(11), cy1 - P(11), std::max(1.f, P(1.5f)), xc);
        Line(rt, win_, cx1 - P(11), cy0 + P(11), cx0 + P(11), cy1 - P(11), std::max(1.f, P(1.5f)), xc);
        Hitbox(kW - kPad - 32, 30, kW - kPad, 62, kClose, 0);
    }

    void DrawTabs(ID2D1RenderTarget* rt) {
        const float W = P(kW), px = Px();
        // Eight sections: the spacing tightens until they fit the width.
        float avail = W - 2 * P(kPad), spacing = P(2.5f), pad = P(12);
        TextStyle tab{P(12.5f), DWRITE_FONT_WEIGHT_NORMAL, spacing};
        auto total = [&](float sp, float pd) {
            float sum = 0;
            for (auto& s : secs_) {
                TextStyle ts{tab.size, DWRITE_FONT_WEIGHT_BOLD, sp};
                sum += pd + P(10) + pd * 0.75f + TextWidth(Upper(s.label), ts) + pd + P(4);
            }
            return sum - P(4);
        };
        // Below 100% (a small screen shrinks the window) P() rounds half a pixel
        // to 0, so each step is at least a whole pixel: a 0 step never ends.
        const float sp_step = std::max(1.f, P(0.5f)), sp_min = P(0.5f), pad_step = std::max(1.f, P(1));
        while (total(spacing, pad) > avail && spacing - sp_step >= sp_min) spacing -= sp_step;
        while (total(spacing, pad) > avail && pad - pad_step >= P(6)) pad -= pad_step;
        // What's left goes back into the tabs, so they span the width evenly.
        float extra = std::max(0.f, (avail - total(spacing, pad)) / std::max<size_t>(1, secs_.size()));
        float x = P(kPad);
        for (int i = 0; i < (int)secs_.size(); i++) {
            bool on = i == cat_;
            TextStyle ts{tab.size, on ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL, spacing};
            std::wstring label = Upper(secs_[i].label);
            TextStyle bold{tab.size, DWRITE_FONT_WEIGHT_BOLD, spacing};
            float tw = TextWidth(label, bold);
            float x1 = std::round(x + pad + P(10) + pad * 0.75f + tw + pad + extra);
            bool hot = !on && hover_act_ == kTab && hover_a_ == i;
            if (on) Fill(rt, win_, x, P(kTabY), x1, P(kTabY + kTabH), C(g_pal.ink));
            else if (hot) Fill(rt, win_, x, P(kTabY), x1, P(kTabY + kTabH), C(g_pal.paper_dark));
            float mx = x + pad + extra / 2;
            Fill(rt, win_, mx, P(kTabY + 16), mx + P(6), P(kTabY + 22), C(on ? g_pal.paper : hot ? g_pal.ink_soft : g_pal.paper_dark));
            Text(rt, win_, label, D2D1::RectF(mx + P(6) + pad * 0.75f, P(kTabY), x1, P(kTabY + kTabH)), ts,
                 C(on ? g_pal.paper : hot ? g_pal.ink : g_pal.ink_soft));
            Hitbox(x / s_, kTabY, x1 / s_, kTabY + kTabH, kTab, i);
            x = x1 + P(4);
        }
        Fill(rt, win_, P(kPad), P(kTabY + kTabH), W - P(kPad), P(kTabY + kTabH) + px, C(g_pal.ink));
    }

    void DrawRows(ID2D1RenderTarget* rt, double now) {
        const auto& st = glitch::CurrentStyle();
        const opts::Section& cat = Cat();
        int sel = std::clamp(sel_, 0, (int)cat.opts.size() - 1);
        for (int i = 0; i < (int)cat.opts.size(); i++) {
            const opts::Option& o = cat.opts[i];
            bool on = i == sel, enabled = o.enabled();
            float y0 = P(kBodyY + i * (kRowH + kRowGap)), y1 = y0 + P(kRowH);
            D2D1_COLOR_F bg = C(g_pal.paper_dark);
            if (on) {
                double rs = st.enabled ? glitch::Steps((now - t_sel_) / 60.0, 3) : 1.0;
                float rx1 = P(kRowX0) + (P(kRowX1) - P(kRowX0)) * (float)std::max(0.34, rs);
                Fill(rt, win_, P(kRowX0), y0, P(kRowX1), y1, C(g_pal.paper_dark));
                Fill(rt, win_, P(kRowX0), y0, rx1, y1, C(g_pal.ink, enabled ? 1.f : 0.5f));
                Wedge(rt, P(kRowX0 - 22), (y0 + y1) / 2, P(14), P(16), false, C(g_pal.ink));
                bg = C(g_pal.ink);
            } else {
                Fill(rt, win_, P(kRowX0), y0, P(kRowX1), y1, C(g_pal.paper_dark, 0.7f));
            }
            D2D1_COLOR_F fg = on ? C(g_pal.paper) : C(g_pal.ink, enabled ? 1.f : 0.4f);
            if (on && !enabled) fg.a = 0.6f;
            float cx0 = DrawControl(rt, o, y0, y1, fg, bg, on, now);
            Text(rt, win_, o.Label(), D2D1::RectF(P(kRowX0 + 16), y0, std::max(P(kRowX0 + 120), cx0 - P(16)), y1),
                 {P(15), DWRITE_FONT_WEIGHT_NORMAL, P(0.3f)}, fg);
            Hitbox(kRowX0, kBodyY + i * (kRowH + kRowGap), kRowX1, kBodyY + i * (kRowH + kRowGap) + kRowH, kRow, i);
        }
    }

    void DrawPanel(ID2D1RenderTarget* rt, double now) {
        const auto& st = glitch::CurrentStyle();
        const float px = Px();
        const opts::Section& cat = Cat();
        int sel = std::clamp(sel_, 0, (int)cat.opts.size() - 1);
        const opts::Option& o = cat.opts[sel];
        float px0 = P(kPanX0), px1 = P(kPanX1), by0 = P(kBodyY), by1 = P(kBodyY1);
        Frame(rt, win_, px0, by0, px1, by1, px, C(g_pal.ink));
        Brackets(rt, px0, by0, px1, by1, C(g_pal.ink));
        wchar_t num[64];
        swprintf(num, 64, L"%ls / %02d", Upper(cat.label).c_str(), sel + 1);
        Text(rt, win_, num, D2D1::RectF(px0 + P(22), by0 + P(18), px1 - P(22), by0 + P(36)), {P(11), DWRITE_FONT_WEIGHT_NORMAL, P(4)},
             C(g_pal.ink_soft));
        double ps = (now - t_sel_) / 220.0;
        glitch::Rng rng = glitch::RngAt(seed_, now - t_sel_, 31);
        std::wstring title = st.enabled && ps < 1 ? glitch::Decode(o.Label(), ps / 0.7, rng) : o.Label();
        TextStyle ts{P(22), DWRITE_FONT_WEIGHT_MEDIUM, 0};
        Text(rt, win_, title, D2D1::RectF(px0 + P(22), by0 + P(40), px1 - P(22), by0 + P(72)), ts, C(g_pal.ink));
        if (std::fmod(now, 1000.0) < 500) {  // the blinking block cursor
            float tw = std::min(TextWidth(title, ts), px1 - px0 - P(60));
            Fill(rt, win_, px0 + P(22) + tw + P(6), by0 + P(46), px0 + P(22) + tw + P(15), by0 + P(67), C(g_pal.ink));
        }
        Fill(rt, win_, px0 + P(22), by0 + P(82), px1 - P(22), by0 + P(82) + px, C(g_pal.paper_dark));
        std::wstring desc = o.desc;
        if (!o.enabled()) desc += Tr(L" (Not used with the current settings.)", L"報告：現在の設定では不使用。");
        desc = st.enabled && ps < 1.4 ? glitch::Decode(desc, ps / 1.3, rng) : desc;
        TextStyle body{P(14), DWRITE_FONT_WEIGHT_NORMAL, 0, kLeft, true, P(22)};
        Text(rt, win_, desc, D2D1::RectF(px0 + P(22), by0 + P(96), px1 - P(22), by0 + P(250)), body, C(g_pal.ink, 0.85f));
        float ey = by0 + P(262);
        bool err = true;
        std::wstring notice = opts::CurrentNotice(&err);
        if (!notice.empty()) {
            Fill(rt, win_, px0 + P(22), ey, px0 + P(28), ey + P(6), C(g_pal.ink));
            Text(rt, win_, Upper(notice), D2D1::RectF(px0 + P(36), ey - P(8), px1 - P(22), ey + P(40)),
                 {P(11), DWRITE_FONT_WEIGHT_NORMAL, P(1.5f), kLeft, true, P(16)}, C(g_pal.ink));
            ey += P(52);
        }
        TextStyle hint{P(10), DWRITE_FONT_WEIGHT_NORMAL, P(2)};
        float gx0 = px0 + P(22), gx1 = px1 - P(22);
        if (o.kind == opts::kSlider && o.get) {
            // A gauge with its ends.
            double f = (o.get() - o.lo) / (o.hi - o.lo);
            Frame(rt, win_, gx0, ey, gx1, ey + P(12), px, C(g_pal.ink));
            Fill(rt, win_, gx0 + P(2), ey + P(2), gx0 + P(2) + (gx1 - gx0 - P(4)) * (float)std::clamp(f, 0.0, 1.0), ey + P(10), C(g_pal.ink));
            std::wstring lo = o.kelvin ? std::to_wstring((int)o.lo) + L"K" : std::to_wstring((int)o.lo) + L"%";
            std::wstring hi = o.kelvin ? std::to_wstring((int)o.hi) + L"K" : std::to_wstring((int)o.hi) + L"%";
            Text(rt, win_, lo, D2D1::RectF(gx0, ey + P(16), gx0 + P(100), ey + P(32)), hint, C(g_pal.ink_soft));
            Text(rt, win_, hi, D2D1::RectF(gx1 - P(100), ey + P(16), gx1, ey + P(32)), {hint.size, hint.weight, hint.spacing, kRight}, C(g_pal.ink_soft));
            Text(rt, win_, Tr(L"LEFT / RIGHT OR CLICK TO ADJUST", L"← → またはクリックで調整"), D2D1::RectF(gx0, ey + P(38), gx1, ey + P(54)), hint,
                 C(g_pal.ink_soft));
        } else if (o.kind == opts::kTime || o.kind == opts::kText) {
            TextStyle lines{hint.size, hint.weight, hint.spacing, kLeft, true, P(18)};
            Text(rt, win_, o.kind == opts::kTime ? Tr(L"CLICK OR ENTER TO TYPE\nLEFT / RIGHT: 15 MINUTES", L"クリックまたはENTERで入力\n← →：15分")
                                                 : Tr(L"CLICK OR ENTER TO TYPE\nCTRL+V PASTES BOTH NUMBERS", L"クリックまたはENTERで入力\nCTRL+Vで両方を貼り付け"),
                 D2D1::RectF(gx0, ey, gx1, ey + P(40)), lines, C(g_pal.ink_soft));
        } else if (o.kind == opts::kHotkey) {
            Text(rt, win_, Tr(L"CLICK OR ENTER, THEN PRESS THE KEYS", L"クリックまたはENTERの後、キーを入力"), D2D1::RectF(gx0, ey, gx1, ey + P(16)), hint,
                 C(g_pal.ink_soft));
        } else if (o.kind == opts::kList) {
            DrawList(rt, o, ey);
        }
        if (o.swatches) {
            const Color* sw[5] = {&g_pal.paper, &g_pal.paper_dark, &g_pal.ink, &g_pal.ink_soft, &g_pal.shade};
            static const wchar_t* en[5] = {L"PAPER", L"PANEL", L"INK", L"QUIET", L"SHADE"};
            static const wchar_t* ja[5] = {L"地", L"面", L"墨", L"淡", L"影"};
            const wchar_t* const* names = Japanese() ? ja : en;
            for (int i = 0; i < 5; i++) {
                float x0 = gx0 + P(i * 62.f);
                Fill(rt, win_, x0, ey, x0 + P(52), ey + P(36), C(*sw[i]));
                Frame(rt, win_, x0, ey, x0 + P(52), ey + P(36), px, C(g_pal.ink_soft));
                Text(rt, win_, names[i], D2D1::RectF(x0, ey + P(40), x0 + P(60), ey + P(56)), {P(9), DWRITE_FONT_WEIGHT_NORMAL, P(1.5f)},
                     C(g_pal.ink_soft));
            }
        }
        if (o.preview != opts::kNoPreview && o.kind != opts::kList) {
            if (o.preview != last_preview_) {
                last_preview_ = o.preview;
                t_preview_ = now;
            }
            float mx = gx0, my = by1 - P(22) - P(kMiniH) - P(6);
            DrawPreview(rt, o.preview, mx, my, now);
            float bx1 = gx1, bx0 = bx1 - P(84), btop = by1 - P(22) - P(34);
            bool hot = hover_act_ == kReplay;
            if (hot) Fill(rt, win_, bx0, btop, bx1, btop + P(34), C(g_pal.ink));
            else Frame(rt, win_, bx0, btop, bx1, btop + P(34), px, C(g_pal.ink_soft));
            Text(rt, win_, Tr(L"REPLAY", L"再生"), D2D1::RectF(bx0, btop, bx1, btop + P(34)), {P(11), DWRITE_FONT_WEIGHT_NORMAL, P(3), kCenter},
                 C(hot ? g_pal.paper : g_pal.ink));
            Hitbox(bx0 / s_, btop / s_, bx1 / s_, (btop + P(34)) / s_, kReplay, 0);
        } else {
            last_preview_ = opts::kNoPreview;
        }
    }

    // A list of games or apps: each with a × to remove it, and the app you
    // were using to add. Fixed items (Spotify for new songs) come first, marked, without a ×.
    void DrawList(ID2D1RenderTarget* rt, const opts::Option& o, float ey) {
        const float px = Px(), px0 = P(kPanX0), px1 = P(kPanX1);
        auto list = opts::ListItems(o);
        int fixed = (int)std::min<size_t>(o.list_fixed.size(), 6);
        for (int i = 0; i < fixed; i++) {
            float y0 = ey + P(i * 40.f), y1 = y0 + P(34);
            Frame(rt, win_, px0 + P(22), y0, px1 - P(22), y1, px, C(g_pal.paper_dark));
            Text(rt, win_, o.list_fixed[i], D2D1::RectF(px0 + P(36), y0, px1 - P(64), y1), {P(13), DWRITE_FONT_WEIGHT_NORMAL, P(0.5f)},
                 C(g_pal.ink_soft));
            Text(rt, win_, Tr(L"ALWAYS", L"常時"), D2D1::RectF(px0 + P(36), y0, px1 - P(36), y1),
                 {P(10), DWRITE_FONT_WEIGHT_NORMAL, P(2), kRight}, C(g_pal.ink_soft));
        }
        ey += P(fixed * 40.f);
        int shown = std::min((int)list.size(), 6 - fixed);
        for (int i = 0; i < shown; i++) {
            float y0 = ey + P(i * 40.f), y1 = y0 + P(34);
            Fill(rt, win_, px0 + P(22), y0, px1 - P(22), y1, C(g_pal.paper_dark));
            Text(rt, win_, list[i], D2D1::RectF(px0 + P(36), y0, px1 - P(64), y1), {P(13), DWRITE_FONT_WEIGHT_NORMAL, P(0.5f)}, C(g_pal.ink));
            float bx = px1 - P(22) - P(34);
            bool hot = hover_act_ == kRemove && hover_a_ == i;
            if (hot) Fill(rt, win_, bx, y0, bx + P(34), y1, C(g_pal.ink));
            D2D1_COLOR_F xc = C(hot ? g_pal.paper : g_pal.ink_soft);
            Line(rt, win_, bx + P(12), y0 + P(12), bx + P(22), y1 - P(12), std::max(1.f, P(1.5f)), xc);
            Line(rt, win_, bx + P(22), y0 + P(12), bx + P(12), y1 - P(12), std::max(1.f, P(1.5f)), xc);
            Hitbox(bx / s_, y0 / s_, (bx + P(34)) / s_, y1 / s_, kRemove, i);
        }
        float ay = ey + P(shown * 40.f);
        if ((int)list.size() > shown) {
            Text(rt, win_,
                 Japanese() ? L"+ 他 " + std::to_wstring(list.size() - shown) + L" 件は設定ファイルに記載"
                            : L"+ " + std::to_wstring(list.size() - shown) + L" MORE IN THE SETTINGS FILE",
                 D2D1::RectF(px0 + P(22), ay, px1 - P(22), ay + P(22)), {P(11), DWRITE_FONT_WEIGHT_NORMAL, P(2)}, C(g_pal.ink_soft));
            ay += P(28);
        }
        bool can_add = !front_.empty() && !core::ListHas(g_cfg.*(o.list), front_) &&
                       !core::ListHas(core::JoinList(o.list_fixed), front_);
        std::wstring add = can_add      ? (Japanese() ? L"+ " + Upper(front_) + L" を追加" : L"+ ADD " + Upper(front_))
                           : o.list_apps ? Tr(L"+ OPEN THE APP, THEN COME BACK HERE", L"+ アプリを起動してから、再度この画面へ")
                                         : Tr(L"+ OPEN THE GAME, THEN COME BACK HERE", L"+ ゲームを起動してから、再度この画面へ");
        bool hot = can_add && hover_act_ == kAdd;
        if (hot) Fill(rt, win_, px0 + P(22), ay, px1 - P(22), ay + P(36), C(g_pal.ink));
        D2D1_COLOR_F dash = C(g_pal.ink_soft);
        for (float dx = px0 + P(22); dx < px1 - P(22); dx += P(8)) {  // dashed outline
            Fill(rt, win_, dx, ay, std::min(dx + P(4), px1 - P(22)), ay + px, dash);
            Fill(rt, win_, dx, ay + P(36) - px, std::min(dx + P(4), px1 - P(22)), ay + P(36), dash);
        }
        Text(rt, win_, add, D2D1::RectF(px0 + P(36), ay, px1 - P(22), ay + P(36)), {P(11), DWRITE_FONT_WEIGHT_NORMAL, P(2)},
             C(hot ? g_pal.paper : can_add ? g_pal.ink : g_pal.ink_soft));
        if (can_add) Hitbox(kPanX0 + 22, ay / s_, kPanX1 - 22, (ay + P(36)) / s_, kAdd, 0);
    }

    void DrawFooter(ID2D1RenderTarget* rt) {
        const float W = P(kW), px = Px();
        float fy = P(kBodyY1 + 12);
        Fill(rt, win_, P(kPad), fy, W - P(kPad), fy + px, C(g_pal.ink));
        float hx = P(kPad), hy = fy + P(26);
        auto hint = [&](const std::wstring& key, const std::wstring& what) {
            TextStyle chip{P(11), DWRITE_FONT_WEIGHT_BOLD, P(1)};
            float kw = TextWidth(key, chip) + P(14);
            Fill(rt, win_, hx, hy - P(10), hx + kw, hy + P(10), C(g_pal.ink));
            Text(rt, win_, key, D2D1::RectF(hx, hy - P(10), hx + kw, hy + P(10)), {chip.size, chip.weight, chip.spacing, kCenter}, C(g_pal.paper));
            TextStyle lab{P(11), DWRITE_FONT_WEIGHT_NORMAL, P(2)};
            float ww = TextWidth(what, lab);
            Text(rt, win_, what, D2D1::RectF(hx + kw + P(8), hy - P(10), hx + kw + P(10) + ww, hy + P(10)), lab, C(g_pal.ink));
            hx += kw + P(10) + ww + P(22);
        };
        // Something wrong (warm colors first) is said here, where it stays,
        // instead of the hints. Warm colors: a click runs the fix.
        auto problems = engine::Problems();
        if (!problems.empty()) {
            bool warm = engine::RegistryProblem();
            float cx = hx;
            hint(warm && engine::WarmRestartPending() ? Tr(L"RESTART", L"再起動") : Tr(L"WARNING", L"警告"), L"");
            float tx0 = hx - P(22) + P(2), tx1 = W - P(kPad);  // the whole width: "saved" waits
            TextStyle lab{P(11), DWRITE_FONT_WEIGHT_SEMI_BOLD, P(2)};
            Text(rt, win_, Upper(problems[0]), D2D1::RectF(tx0, hy - P(10), tx1, hy + P(10)), lab, C(g_pal.ink));
            if (warm) {
                float tw = std::min(TextWidth(Upper(problems[0]), lab), tx1 - tx0);
                if (hover_act_ == kStrip && hover_a_ == 1) Fill(rt, win_, tx0, hy + P(11), tx0 + tw, hy + P(11) + px, C(g_pal.ink));
                Hitbox(cx / s_, hy / s_ - 14, (tx0 + tw) / s_ + 4, hy / s_ + 14, kStrip, 1, L"warm");
            }
        } else {
            hint(L"HOVER", Tr(L"SELECT", L"選択"));
            hint(L"CLICK", Tr(L"CHANGE", L"決定"));
            hint(Tr(L"R-CLICK", L"右クリック"), Tr(L"BACK", L"戻す"));
            hint(L"TAB", Tr(L"NEXT SECTION", L"項目切替"));
            hint(L"ESC", Tr(L"CLOSE", L"閉じる"));
        }
        if (problems.empty())
            Text(rt, win_, Tr(L"SAVED AUTOMATICALLY", L"変更は自動保存"), D2D1::RectF(W - P(300), hy - P(10), W - P(kPad), hy + P(10)),
                 {P(11), DWRITE_FONT_WEIGHT_NORMAL, P(2), kRight}, C(g_pal.ink_soft));
    }

    void DrawWindow(ID2D1RenderTarget* rt, double now) {
        hits_.clear();
        const float W = P(kW), H = P(kH);
        const auto& st = glitch::CurrentStyle();
        rt->Clear(C(g_pal.paper));
        Frame(rt, win_, 0, 0, W, H, Px(), C(g_pal.paper_dark));
        DrawHeader(rt);
        DrawStrip(rt);
        DrawTabs(rt);
        // The body opens from a line when the tab changes.
        double pt = (now - t_tab_) / 280.0;
        float band = st.enabled && pt < 1 ? (float)glitch::EaseOut(pt / 0.5) : 1.f;
        float by0 = P(kBodyY), by1 = P(kBodyY1);
        float mid = (by0 + by1) / 2, half = std::max(P(1), (by1 - by0) / 2 * band);
        rt->PushAxisAlignedClip(D2D1::RectF(0, mid - half, W, mid + half), D2D1_ANTIALIAS_MODE_ALIASED);
        DrawRows(rt, now);
        DrawPanel(rt, now);
        rt->PopAxisAlignedClip();
        DrawFooter(rt);
    }

    // ---- frames ------------------------------------------------------------------------------------------------
    void Render(double now) {
        if (!open_) return;
        const auto& st = glitch::CurrentStyle();
        glitch::Fx fx;
        const D2D1_COLOR_F paper = C(g_pal.paper);
        const D2D1_COLOR_F* backdrop = &paper;
        double scramble = 1;  // < 1: the text settling in after a language switch
        if (closing_) {
            double t = now - t_close_;
            if (t >= glitch::CloseMs(st)) {
                open_ = closing_ = false;
                KillTimer(win_.hwnd, 1);
                win_.Hide();
                return;
            }
            fx = glitch::CloseFx(st, t, seed_, kW, kH);
            backdrop = nullptr;
        } else if (now - t_open_ < glitch::OpenMs(st)) {
            fx = glitch::OpenFx(st, now - t_open_, seed_, kW, kH);
            backdrop = nullptr;
        } else if (now - t_lang_ < LangMs(st)) {
            // Switching language: the whole window tears, flickers and breaks
            // into noise, and every text settles back in the new language.
            double t = now - t_lang_, p = t / LangMs(st);
            glitch::Rng rng = glitch::RngAt(seed_, t, 43);
            fx.tears = glitch::Tears(rng, st, 1.2 * (1 - p), 0, kH);
            fx.blocks = glitch::Blocks(rng, st, 0.9 * (1 - p), kW, kH);
            fx.alpha = glitch::Flicker(rng, st, 0.6 * (1 - p));
            scramble = glitch::EaseOut(t / (LangMs(st) * 0.85));
        } else if (st.enabled && now - t_tab_ < 280) {
            double p = (now - t_tab_) / 280.0;
            glitch::Rng rng = glitch::RngAt(seed_, now - t_tab_, 41);
            fx.tears = glitch::Tears(rng, st, 0.8 * (1 - p), kBodyY, kBodyY1);
        }
        float cw = P(kW), ch = P(kH), m = P(kMargin);
        ID2D1RenderTarget* rt = win_.Begin();
        if (!rt) return;
        if (auto* scratch = win_.Scratch((int)cw, (int)ch)) {
            scratch->BeginDraw();
            gfx::ScrambleText(scramble, seed_, now - t_lang_);
            DrawWindow(scratch, now);
            scratch->EndDraw();
            ID2D1Bitmap* bmp = nullptr;
            if (SUCCEEDED(scratch->GetBitmap(&bmp))) {
                Compose(rt, win_, bmp, m, m, cw, ch, fx, s_, true, backdrop);
                bmp->Release();
            }
        }
        gfx::ScrambleText(1, 0, 0);  // only this window's text
        RECT r;
        GetWindowRect(win_.hwnd, &r);
        win_.End(r.left, r.top, fx.alpha);
        last_blink_ = std::fmod(now, 1000.0) < 500;
        last_status_ = opts::StatusLine() + engine::KeyboardStatus();
        last_minute_ = (int)(NowLocal() / 60);
    }

    void Tick() {
        double now = NowMs();
        if (IsIconic(win_.hwnd)) return;
        bool busy = Animating(now);
        SetTimer(win_.hwnd, 1, busy ? 10 : 50, nullptr);  // idle: only the cursor blinks
        if (busy || opts::g_edit.Active() || !opts::g_recording.empty()) {
            Render(now);
            return;
        }
        int blink = std::fmod(now, 1000.0) < 500;
        if (blink != last_blink_ || opts::StatusLine() + engine::KeyboardStatus() != last_status_ || (int)(NowLocal() / 60) != last_minute_)
            Render(now);
    }

    void Select(int i, double now, bool render = true) {
        i = std::clamp(i, 0, (int)Cat().opts.size() - 1);
        if (i == sel_) return;
        opts::EndEdit(EditingOption(), true);
        opts::StopRecording();
        sel_ = i;
        t_sel_ = now;
        if (render) Render(now);
    }

    void SwitchTab(int i, double now) {
        int n = (int)secs_.size();
        i = ((i % n) + n) % n;
        if (i == cat_) return;
        opts::EndEdit(EditingOption(), true);
        opts::StopRecording();
        cat_ = i;
        sel_ = 0;
        t_tab_ = t_sel_ = t_preview_ = now;
        Render(now);
    }

    // Goes to a setting, wherever it is (a click on today's band).
    void GoTo(const std::wstring& key, double now) {
        for (int c = 0; c < (int)secs_.size(); c++)
            for (int r = 0; r < (int)secs_[c].opts.size(); r++)
                if (secs_[c].opts[r].key == key) {
                    SwitchTab(c, now);
                    Select(r, now);
                    return;
                }
    }

    // Activates the selected setting: click, Enter or Space (dir +1), right-click (-1).
    void Activate(int dir) {
        const opts::Option& o = Sel();
        if (!o.enabled()) return;
        std::wstring key = o.key, before = Upper(o.value());
        switch (o.kind) {
            case opts::kTime:
            case opts::kText:
                if (opts::g_edit.key == key) opts::EndEdit(&o, true);
                else opts::BeginEdit(o);
                break;
            case opts::kHotkey:
                if (opts::g_recording == key) opts::StopRecording();
                else opts::BeginRecording(o);
                break;
            case opts::kInfo:
            case opts::kList: break;
            case opts::kAction:
                if (dir > 0) {
                    auto run = o.change;  // a copy: running it may rebuild the option list
                    run(1);
                }
                break;
            default: {
                auto change = o.change;
                change(dir);
            }
        }
        if (!open_) return;  // Quit
        Changed(key, before);
    }

    void Changed(const std::wstring& key, const std::wstring& before) {
        Rebuild();
        double now = NowMs();
        const opts::Option* o = Find(key);
        if (o && Upper(o->value()) != before) {
            changes_.erase(std::remove_if(changes_.begin(), changes_.end(), [&](const Change& c) { return c.key == key; }), changes_.end());
            changes_.push_back({key, before, now});
        }
        if (key == L"anim" || key == L"colors" || key == L"art") t_preview_ = now;
        if (key == L"language") {
            gfx::WarmFonts();  // before the clock starts, so the glitch plays from its first frame
            t_lang_ = now = NowMs();
            changes_.clear();
        }
        Render(now);
    }

    int HitAt(float x, float y) const {
        for (int i = (int)hits_.size() - 1; i >= 0; i--) {
            const auto& h = hits_[i];
            if (x >= h.r.left && x < h.r.right && y >= h.r.top && y < h.r.bottom) return i;
        }
        return -1;
    }

    void Click(int x, int y, int dir) {
        float lx = (x - P(kMargin)) / s_, ly = (y - P(kMargin)) / s_;
        int i = HitAt(lx, ly);
        if (i < 0) return;
        Hit h = hits_[i];
        double now = NowMs();
        switch (h.act) {
            case kTab: SwitchTab(h.a, now); break;
            case kRow:
                if (h.a != sel_) Select(h.a, now);
                Activate(dir);
                break;
            case kReplay:
                t_preview_ = now;
                Render(now);
                break;
            case kRemove: {
                const opts::Option& o = Sel();
                opts::ListRemove(o, (size_t)h.a);
                Changed(o.key, Upper(o.value()));
                break;
            }
            case kAdd: {
                const opts::Option& o = Sel();
                std::wstring before = Upper(o.value());
                opts::ListAdd(o, front_);
                Changed(o.key, before);
                break;
            }
            case kStrip:
                GoTo(h.key, now);
                // The warm-colors warning in the footer: a click runs its fix
                // (unlock, or the restart question), as it says.
                if (h.key == L"warm" && open_ && Sel().key == L"warm") Activate(1);
                break;
            case kClose: PostMessageW(win_.hwnd, WM_CLOSE, 0, 0); break;
        }
    }

    bool Key(UINT vk) {
        if (vk == VK_F4 && GetKeyState(VK_MENU) < 0 && opts::g_recording.empty()) return false;  // Alt+F4 closes
        if (opts::EditKey(vk, EditingOption())) {
            Changed(Sel().key, L"");
            return true;
        }
        if (!opts::g_recording.empty()) {
            std::wstring key = opts::g_recording, before = Upper(Sel().value());
            opts::RecordKey(vk, RecordingOption());
            Changed(key, before);
            return true;
        }
        double now = NowMs();
        bool shift = GetKeyState(VK_SHIFT) < 0;
        switch (vk) {
            case VK_UP: Select(sel_ - 1, now); return true;
            case VK_DOWN: Select(sel_ + 1, now); return true;
            case VK_LEFT:
            case VK_RIGHT: {
                const opts::Option& o = Sel();
                if (o.kind == opts::kToggle || o.kind == opts::kChoice || o.kind == opts::kSlider || o.kind == opts::kStepper ||
                    o.kind == opts::kTime) {
                    if (!o.enabled()) return true;
                    std::wstring key = o.key, before = Upper(o.value());
                    auto change = o.change;
                    change(vk == VK_RIGHT ? 1 : -1);
                    if (open_) Changed(key, before);
                }
                return true;
            }
            case VK_RETURN:
            case VK_SPACE: Activate(1); return true;
            case VK_TAB: SwitchTab(cat_ + (shift ? -1 : 1), now); return true;
            case VK_PRIOR: SwitchTab(cat_ - 1, now); return true;
            case VK_NEXT: SwitchTab(cat_ + 1, now); return true;
            case VK_ESCAPE: PostMessageW(win_.hwnd, WM_CLOSE, 0, 0); return true;
        }
        return false;
    }

    static LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        auto* self = (SettingsWindow*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
        if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
        switch (msg) {
            case WM_TIMER: self->Tick(); return 0;
            case WM_NCHITTEST: {
                POINT pt = {(short)LOWORD(lp), (short)HIWORD(lp)};
                ScreenToClient(hwnd, &pt);
                float lx = (pt.x - self->P(kMargin)) / self->s_, ly = (pt.y - self->P(kMargin)) / self->s_;
                if (lx < 0 || ly < 0 || lx > kW || ly > kH) return HTTRANSPARENT;  // the shadow
                int i = self->HitAt(lx, ly);
                if (ly < kStripY - 10 && i < 0) return HTCAPTION;  // drag by the header
                return HTCLIENT;
            }
            case WM_MOUSEMOVE: {
                POINT pt;
                GetCursorPos(&pt);
                if (pt.x == self->last_mouse_.x && pt.y == self->last_mouse_.y) return 0;
                self->last_mouse_ = pt;
                float lx = ((short)LOWORD(lp) - self->P(kMargin)) / self->s_, ly = ((short)HIWORD(lp) - self->P(kMargin)) / self->s_;
                int i = self->HitAt(lx, ly);
                int act = i >= 0 ? self->hits_[i].act : -1, a = i >= 0 ? self->hits_[i].a : -1;
                int sel_before = self->sel_;
                if (i >= 0 && self->hits_[i].act == kRow && !opts::g_edit.Active() && opts::g_recording.empty())
                    self->Select(self->hits_[i].a, NowMs(), false);
                // One frame for both (a new row is selected and hovered at once).
                if (act != self->hover_act_ || a != self->hover_a_ || self->sel_ != sel_before) {
                    self->hover_act_ = act;
                    self->hover_a_ = a;
                    self->Render(NowMs());
                }
                TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&tme);
                return 0;
            }
            case WM_MOUSELEAVE:
                if (self->hover_act_ != -1) {
                    self->hover_act_ = self->hover_a_ = -1;
                    self->Render(NowMs());
                }
                self->last_mouse_ = {-1, -1};
                return 0;
            case WM_SETCURSOR:
                if (LOWORD(lp) == HTCLIENT) {
                    // A hand over what does something when clicked (not the rows: hovering them selects).
                    bool link = self->hover_act_ == kTab || self->hover_act_ == kClose || self->hover_act_ == kReplay ||
                                self->hover_act_ == kRemove || self->hover_act_ == kAdd || self->hover_act_ == kStrip;
                    SetCursor(LoadCursorW(nullptr, link ? IDC_HAND : IDC_ARROW));
                    return TRUE;
                }
                break;
            case WM_LBUTTONDOWN: self->Click((short)LOWORD(lp), (short)HIWORD(lp), +1); return 0;
            case WM_RBUTTONDOWN: self->Click((short)LOWORD(lp), (short)HIWORD(lp), -1); return 0;
            case WM_KEYDOWN:
            case WM_SYSKEYDOWN:
                if (self->Key((UINT)wp)) return 0;
                break;
            case WM_SYSCHAR: return 0;  // Alt+letter: no beep (there's no menu to open)
            case WM_CHAR:
                if (opts::EditChar((wchar_t)wp)) self->Render(NowMs());
                return 0;
            case WM_CLOSE: self->Close(true); return 0;
            case WM_DPICHANGED: {
                self->s_ = LOWORD(wp) / 96.f;
                const RECT* r = (const RECT*)lp;
                int w = (int)(self->P(kW) + 2 * self->P(kMargin)), h = (int)(self->P(kH) + 2 * self->P(kMargin));
                self->win_.Resize(w, h);
                SetWindowPos(hwnd, nullptr, r->left, r->top, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
                self->Render(NowMs());
                return 0;
            }
            case WM_ACTIVATE:
                if (LOWORD(wp) == WA_INACTIVE) {
                    opts::EndEdit(self->EditingOption(), true);
                    opts::StopRecording();
                }
                self->Render(NowMs());
                return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    struct Change {
        std::wstring key, old;
        double t;
    };

    Surface win_;
    std::vector<opts::Section> secs_;
    std::vector<Hit> hits_;
    std::vector<Change> changes_;
    int cat_ = 0, sel_ = 0, hover_act_ = -1, hover_a_ = -1;
    bool open_ = false, closing_ = false;
    double t_open_ = 0, t_close_ = 0, t_tab_ = -1e9, t_sel_ = -1e9, t_preview_ = -1e9, t_lang_ = -1e9;
    uint32_t seed_ = 1;
    float s_ = 1;
    POINT last_mouse_ = {-1, -1};
    int last_blink_ = -1, last_minute_ = -1;
    std::wstring last_status_;
    std::wstring front_;  // the app you were using before opening (for "add game")
    opts::Preview last_preview_ = opts::kNoPreview;
    std::vector<core::Phase> strip_events_;  // TodaysEvents()
    std::wstring strip_key_;
    int strip_tz_ = 0;  // bumped when the clock or time zone may have changed (opened again, Refresh)
    ID2D1BitmapRenderTarget* mini_ = nullptr;
    int mini_gen_ = -1, mini_w_ = 0, mini_h_ = 0;
};

}  // namespace

Window& Main() {
    static SettingsWindow w;
    return w;
}

}  // namespace ui
