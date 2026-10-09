// The Alt+Tab switcher: a list of windows on the left (each with a × that
// closes it) and a live preview of the selected one on the right.
//
// The preview is drawn by Windows itself (DWM thumbnails), so it is live and
// costs Candela almost nothing. For the glitch, the same window is registered as
// several thumbnails, each showing one horizontal band, and the bands are
// pushed sideways for a few frames.
#include "switcher_state.h"

#include <algorithm>
#include <cmath>

namespace switcher {

using namespace gfx;

namespace detail {
std::vector<Item> g_items;
int g_sel = 0, g_prev_sel = 0;
Phase g_phase = kHidden;
double g_t_shown = 0, g_t_sel = -1e9, g_t_close = 0;
uint32_t g_seed = 1;
HWND g_dim = nullptr;
}  // namespace detail

namespace {

using namespace detail;

// Layout, in unscaled px. The surface is bigger than the panel by kMargin on
// every side, room for the shadow and for slices to tear into.
constexpr float kW = 860, kMargin = 28, kRowH = 36, kListX0 = 44, kListX1 = 404, kBodyY = 68;
constexpr float kPrevX0 = 428, kPrevY0 = 68, kPrevX1 = 832, kPrevY1 = 304;  // preview frame
constexpr float kHostInset = 4;
constexpr float kCloseW = 28;  // the × at the right end of each row: its click target's width
constexpr int kThumbs = 8;

Surface g_panel;
HWND g_host = nullptr;
HBRUSH g_host_brush = nullptr, g_dim_brush = nullptr;
ID2D1PathGeometry* g_cursor_geo = nullptr;

int g_first = 0;
double g_t_start = 0;
float g_s = 1;
RECT g_mon = {}, g_work = {};
int g_px = 0, g_py = 0;
float g_h = 470;  // panel height (unscaled)
int g_rows_max = 10;  // rows shown before the list scrolls; set to fit the screen
bool g_dirty = true;
int g_last_minute = -1;
POINT g_mouse = {};  // where the cursor was; Windows sends a "move" when a window appears under a still cursor
int g_hover_close = -1;      // the row whose × the pointer is on, or -1
bool g_tracking = false;     // asked Windows for WM_MOUSELEAVE
// After a window is asked to close, the list is looked at again every so
// often for a few seconds: what closed leaves, a "save changes?" it opened comes in.
bool g_watching = false;
double g_watch_until = 0, g_last_refresh = 0;
constexpr double kWatchMs = 5000, kRefreshMs = 120;

struct Thumbs {
    HWND src = nullptr;
    HTHUMBNAIL th[kThumbs] = {};
    SIZE size = {};
    RECT crop = {};  // the visible part of the window (without its invisible resize borders)
    bool ok = false;
} g_th;

float P(float v) { return std::round(v * g_s); }

// ---- which windows belong in Alt+Tab ------------------------------------------------
bool IsCloaked(HWND h) {
    DWORD cloaked = 0;
    return SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked;
}

bool IsAltTabWindow(HWND h) {
    if (!IsWindowVisible(h) || IsOwnWindow(h) || media::IsOwnWindow(h) || popup::IsOwnWindow(h) || h == g_main) return false;
    LONG ex = GetWindowLongW(h, GWL_EXSTYLE);
    bool app = (ex & WS_EX_APPWINDOW) != 0;
    if (!app && (ex & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE))) return false;
    if (IsCloaked(h)) return false;  // other virtual desktops, suspended Store apps
    if (!app) {
        // Raymond Chen's rule: the window must be the last active popup of its root owner.
        HWND walk = GetAncestor(h, GA_ROOTOWNER), next;
        while ((next = GetLastActivePopup(walk)) != walk) {
            if (IsWindowVisible(next)) break;
            walk = next;
        }
        if (walk != h) return false;
    }
    wchar_t cls[64];
    GetClassNameW(h, cls, 64);
    static const wchar_t* skip[] = {L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd", L"Progman", L"WorkerW",
                                    L"Windows.UI.Core.CoreWindow"};
    for (auto* s : skip)
        if (!wcscmp(cls, s)) return false;
    return !WindowTitle(h).empty();  // (GetWindowTextLength would message a possibly hung app)
}

HICON WindowIcon(HWND h, bool big) {
    DWORD_PTR r = 0;
    const UINT kinds[] = {big ? (UINT)ICON_BIG : (UINT)ICON_SMALL2, big ? (UINT)ICON_SMALL2 : (UINT)ICON_BIG};
    for (UINT kind : kinds)
        if (SendMessageTimeoutW(h, WM_GETICON, kind, 0, SMTO_ABORTIFHUNG, 15, &r) && r) return (HICON)r;
    if ((r = GetClassLongPtrW(h, big ? GCLP_HICON : GCLP_HICONSM))) return (HICON)r;
    if ((r = GetClassLongPtrW(h, big ? GCLP_HICONSM : GCLP_HICON))) return (HICON)r;
    return nullptr;
}

std::wstring Upper(std::wstring s) {
    CharUpperBuffW(s.data(), (DWORD)s.size());
    return s;
}

std::wstring AppLabel(const Item& it) { return Upper(AppName(it)); }

void ReleaseItems() {
    for (auto& it : g_items) {
        if (it.bmp) it.bmp->Release();
    }
    g_items.clear();
}

bool g_big_icons = false;  // 18px icons need the big one above 125%

// The screen the switcher opens on (Switcher -> Screen): the one with the
// window in front, the primary one, or one picked by name (the primary while
// that one isn't connected).
HMONITOR SwitcherMonitor() {
    const std::wstring& want = g_cfg.switcher_display;
    if (want == L"primary") return MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    if (!want.empty() && want != L"active") {
        if (HMONITOR m = MonitorByDevice(want)) return m;
        return MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    }
    return ForegroundMonitor();
}

// The windows Alt+Tab lists, front to back. Icons are asked for when a row is
// first drawn, not here: asking means a message to each app (up to 15 ms
// each for a slow one), and a quick Alt+Tab that never shows the list
// shouldn't wait for sixty of them.
std::vector<Item> Collect() {
    std::vector<Item> out;
    EnumWindows(
        [](HWND h, LPARAM lp) -> BOOL {
            auto* list = (std::vector<Item>*)lp;
            if (IsAltTabWindow(h)) list->push_back({h, WindowTitle(h), ProcessExeName(h), nullptr});
            return list->size() < 60;  // nobody scrolls that far; keep it quick
        },
        (LPARAM)&out);
    return out;
}

void Enumerate() {
    ReleaseItems();
    g_big_icons = DpiScaleForMonitor(SwitcherMonitor()) >= 1.25f;
    g_items = Collect();
    // --dump: no real windows to show, so a few made-up ones.
    if (g_fake_now >= 0 && g_items.empty()) {
        const wchar_t* fake[][2] = {{L"Amber Hours - Spotify", L"Spotify.exe"},
                                    {L"Candela - README.md - Visual Studio Code", L"Code.exe"},
                                    {L"Sunrise and sunset times - Mozilla Firefox", L"firefox.exe"},
                                    {L"Downloads", L"explorer.exe"},
                                    {L"Task Manager", L"Taskmgr.exe"},
                                    {L"Discord", L"Discord.exe"},
                                    {L"config.json - Notepad", L"notepad.exe"}};
        for (auto& f : fake) g_items.push_back({nullptr, f[0], f[1], nullptr});
    }
}

// ---- live preview (DWM thumbnails) --------------------------------------------------
void ClearThumbs() {
    for (auto& t : g_th.th)
        if (t) {
            DwmUnregisterThumbnail(t);
            t = nullptr;
        }
    g_th.src = nullptr;
    g_th.ok = false;
}

void SetThumbSource(HWND src) {
    if (src == g_th.src) return;
    ClearThumbs();
    g_th.src = src;
    if (!src || !g_host) return;
    for (auto& t : g_th.th)
        if (FAILED(DwmRegisterThumbnail(g_host, src, &t))) t = nullptr;
    g_th.ok = g_th.th[0] && SUCCEEDED(DwmQueryThumbnailSourceSize(g_th.th[0], &g_th.size)) && g_th.size.cx > 40 &&
              g_th.size.cy > 40;
    if (IsIconic(src) && (g_th.size.cx < 200 || g_th.size.cy < 120)) g_th.ok = false;  // nothing worth showing
    // Windows 10/11 windows are wider than they look: invisible resize
    // borders. Crop them so the preview is just the window.
    g_th.crop = {0, 0, g_th.size.cx, g_th.size.cy};
    RECT wr, vis;
    if (g_th.ok && !IsIconic(src) && GetWindowRect(src, &wr) &&
        SUCCEEDED(DwmGetWindowAttribute(src, DWMWA_EXTENDED_FRAME_BOUNDS, &vis, sizeof(vis))) &&
        wr.right - wr.left == g_th.size.cx && wr.bottom - wr.top == g_th.size.cy) {
        RECT c = {std::max(0L, vis.left - wr.left), std::max(0L, vis.top - wr.top),
                  g_th.size.cx - std::max(0L, wr.right - vis.right), g_th.size.cy - std::max(0L, wr.bottom - vis.bottom)};
        if (c.right - c.left > 40 && c.bottom - c.top > 40) g_th.crop = c;
    }
}

RECT HostRect() {  // in screen coordinates
    float ox = g_px + P(kMargin), oy = g_py + P(kMargin);
    return {(LONG)(ox + P(kPrevX0 + kHostInset)), (LONG)(oy + P(kPrevY0 + kHostInset)),
            (LONG)(ox + P(kPrevX1 - kHostInset)), (LONG)(oy + P(kPrevY1 - kHostInset))};
}

void UpdateThumbs(double intensity, double now, bool visible) {
    if (!g_host) return;
    if (!visible || !g_th.ok) {
        // No picture: hide the host too, so the panel's "MINIMIZED" / "NO PREVIEW" shows.
        for (auto& t : g_th.th)
            if (t) {
                DWM_THUMBNAIL_PROPERTIES p = {};
                p.dwFlags = DWM_TNP_VISIBLE;
                p.fVisible = FALSE;
                DwmUpdateThumbnailProperties(t, &p);
            }
        ShowWindow(g_host, SW_HIDE);
        return;
    }
    RECT hr = HostRect();
    int hw = hr.right - hr.left, hh = hr.bottom - hr.top;
    if (!IsWindowVisible(g_host))
        SetWindowPos(g_host, HWND_TOPMOST, hr.left, hr.top, hw, hh, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    // Fit the window into the frame, never enlarged (small windows stay sharp).
    const RECT& cr = g_th.crop;
    int cw = cr.right - cr.left, ch = cr.bottom - cr.top;
    double scale = std::min({(double)hw / cw, (double)hh / ch, 1.0});
    int dw = (int)std::lround(cw * scale), dh = (int)std::lround(ch * scale);
    int dx0 = (hw - dw) / 2, dy0 = (hh - dh) / 2;
    auto set = [](HTHUMBNAIL t, bool vis, RECT dst, RECT src, BYTE opacity) {
        if (!t) return;
        DWM_THUMBNAIL_PROPERTIES p = {};
        p.dwFlags = DWM_TNP_VISIBLE | DWM_TNP_RECTDESTINATION | DWM_TNP_RECTSOURCE | DWM_TNP_OPACITY |
                    DWM_TNP_SOURCECLIENTAREAONLY;
        p.fVisible = vis;
        p.rcDestination = dst;
        p.rcSource = src;
        p.opacity = opacity;
        p.fSourceClientAreaOnly = FALSE;
        DwmUpdateThumbnailProperties(t, &p);
    };
    const RECT full_src = cr;
    const RECT full_dst = {dx0, dy0, dx0 + dw, dy0 + dh};
    if (intensity <= 0 || !glitch::CurrentStyle().enabled) {
        set(g_th.th[0], true, full_dst, full_src, 255);
        for (int i = 1; i < kThumbs; i++) set(g_th.th[i], false, full_dst, full_src, 255);
        return;
    }
    // Cut the picture into bands at random heights; some bands jump sideways.
    glitch::Rng rng = glitch::RngAt(g_seed, now, 7, 40);
    int cuts[kThumbs + 1];
    cuts[0] = 0;
    cuts[kThumbs] = dh;
    for (int i = 1; i < kThumbs; i++) cuts[i] = rng.range(1, std::max(2, dh));
    std::sort(cuts + 1, cuts + kThumbs);
    BYTE opacity = rng.chance(0.3 * intensity) ? 110 : 255;
    float tear = glitch::CurrentStyle().tear_px * 2.2f * g_s;
    for (int i = 0; i < kThumbs; i++) {
        int y0 = cuts[i], y1 = cuts[i + 1];
        if (y1 <= y0) {
            set(g_th.th[i], false, full_dst, full_src, 255);
            continue;
        }
        int shift = rng.chance(0.45) ? (int)std::lround((rng.uniform() * 2 - 1) * tear * intensity) : 0;
        RECT dst = {dx0 + shift, dy0 + y0, dx0 + dw + shift, dy0 + y1};
        RECT src = {cr.left, cr.top + (LONG)std::lround(y0 / scale), cr.right, cr.top + (LONG)std::lround(y1 / scale)};
        if (src.bottom <= src.top) src.bottom = src.top + 1;
        set(g_th.th[i], true, dst, src, opacity);
    }
}

// ---- drawing ------------------------------------------------------------------------------
void EnsureCursorGeometry() {
    if (g_cursor_geo || !d2d) return;
    ID2D1GeometrySink* sink = nullptr;
    if (FAILED(d2d->CreatePathGeometry(&g_cursor_geo)) || FAILED(g_cursor_geo->Open(&sink))) return;
    // A right-pointing wedge with a notch, drawn in a 14 x 16 box.
    sink->BeginFigure(D2D1::Point2F(0, 0), D2D1_FIGURE_BEGIN_FILLED);
    D2D1_POINT_2F pts[] = {{14, 8}, {0, 16}, {4, 8}};
    sink->AddLines(pts, 3);
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();
    sink->Release();
}

void DrawCursor(ID2D1RenderTarget* rt, float x, float y, D2D1_COLOR_F c) {
    EnsureCursorGeometry();
    if (!g_cursor_geo) return;
    rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    rt->SetTransform(D2D1::Matrix3x2F::Scale(g_s, g_s) * D2D1::Matrix3x2F::Translation(x, y));
    rt->FillGeometry(g_cursor_geo, g_panel.Brush(c));
    rt->SetTransform(D2D1::Matrix3x2F::Identity());
}

void CornerBrackets(ID2D1RenderTarget* rt, float x0, float y0, float x1, float y1, D2D1_COLOR_F c) {
    float L = P(10), T = std::max(1.f, P(2)), o = P(4);
    x0 -= o; y0 -= o; x1 += o; y1 += o;
    Fill(rt, g_panel, x0, y0, x0 + L, y0 + T, c); Fill(rt, g_panel, x0, y0, x0 + T, y0 + L, c);
    Fill(rt, g_panel, x1 - L, y0, x1, y0 + T, c); Fill(rt, g_panel, x1 - T, y0, x1, y0 + L, c);
    Fill(rt, g_panel, x0, y1 - T, x0 + L, y1, c); Fill(rt, g_panel, x0, y1 - L, x0 + T, y1, c);
    Fill(rt, g_panel, x1 - L, y1 - T, x1, y1, c); Fill(rt, g_panel, x1 - T, y1 - L, x1, y1, c);
}

// Key hint in the footer: a small inked chip with the key, then what it does.
float KeyHint(ID2D1RenderTarget* rt, float x, float cy, const std::wstring& key, const std::wstring& label) {
    TextStyle chip{P(11), DWRITE_FONT_WEIGHT_SEMI_BOLD, P(1.5f)};
    TextStyle lab{P(12), DWRITE_FONT_WEIGHT_NORMAL, P(2)};
    float kw = TextWidth(key, chip) + P(12);
    Fill(rt, g_panel, x, cy - P(9), x + kw, cy + P(9), C(g_pal.ink));
    Text(rt, g_panel, key, D2D1::RectF(x, cy - P(9), x + kw, cy + P(9)), {chip.size, chip.weight, chip.spacing, kCenter},
         C(g_pal.paper));
    float lw = TextWidth(label, lab);
    Text(rt, g_panel, label, D2D1::RectF(x + kw + P(8), cy - P(10), x + kw + P(10) + lw, cy + P(10)), lab,
         C(g_pal.ink));
    return x + kw + P(10) + lw + P(22);
}

// The × at a row's right end. Quiet until the pointer is on it; then it gets
// a chip of its own, like the key hints.
void DrawClose(ID2D1RenderTarget* rt, float y0, float y1, bool sel, bool hover, bool closing) {
    float cx = P(kListX1 - kCloseW / 2 - 2), cy = std::round((y0 + y1) / 2), half = P(4.5f);
    const Color& on = sel ? g_pal.paper : g_pal.ink;    // the row's text color
    const Color& off = sel ? g_pal.ink : g_pal.paper;   // the row's background
    D2D1_COLOR_F mark = C(sel ? g_pal.paper : g_pal.ink_soft, closing ? 0.45f : 1.f);
    if (hover && !closing) {
        float b = P(10);
        Fill(rt, g_panel, cx - b, cy - b, cx + b, cy + b, C(on));
        mark = C(off);
    }
    float w = std::max(1.f, P(1.6f));
    Line(rt, g_panel, cx - half, cy - half, cx + half, cy + half, w, mark);
    Line(rt, g_panel, cx - half, cy + half, cx + half, cy - half, w, mark);
}

struct Texts {
    std::wstring header, count, exe, title;
    std::vector<std::wstring> rows;
    bool cursor_typing = false;
};

// What the text says right now: typing in while opening, scrambling into
// the new selection, breaking up while closing.
Texts CurrentTexts(double now) {
    const auto& st = glitch::CurrentStyle();
    Texts tx;
    int n = (int)g_items.size();
    tx.header = Tr(L"WINDOWS", L"ウィンドウ");
    wchar_t count[32];
    swprintf(count, 32, L"%02d / %02d", n ? g_sel + 1 : 0, n);
    tx.count = count;
    if (n) {
        tx.exe = AppLabel(g_items[g_sel]);
        tx.title = g_items[g_sel].title;
    } else {
        tx.exe = L"-";
        tx.title = Tr(L"No other windows are open.", L"報告：他に開いているウィンドウなし。");
    }
    int rows = std::min(n, g_rows_max);
    for (int r = 0; r < rows; r++) tx.rows.push_back(g_items[g_first + r].title);
    if (!st.enabled) return tx;

    double t = now - g_t_shown;
    if (g_phase == kClosing) {
        double p = glitch::Clamp01((now - g_t_close) / std::max(1.0, st.break_ms));
        glitch::Rng rng = glitch::RngAt(g_seed, now - g_t_close, 30);
        double amount = 0.15 + 0.85 * p;
        tx.header = glitch::Corrupt(tx.header, amount, rng);
        tx.count = glitch::Corrupt(tx.count, amount, rng);
        tx.exe = glitch::Corrupt(tx.exe, amount, rng);
        tx.title = glitch::Corrupt(tx.title, amount, rng);
        for (auto& r : tx.rows) r = glitch::Corrupt(r, amount, rng);
        return tx;
    }
    if (t < glitch::OpenMs(st) + 1) {
        glitch::Rng rng = glitch::RngAt(g_seed, t, 20);
        bool typing = false;
        tx.header = glitch::Decode(tx.header, glitch::TypeProgress(st, t, 0.0, 0.4), rng, &typing);
        tx.count = glitch::Decode(tx.count, glitch::TypeProgress(st, t, 0.2, 0.5), rng, nullptr, true);
        // All rows type in together, so no name glitches more than another.
        if (g_cfg.switcher_typing) {
            double pr = glitch::TypeProgress(st, t, 0.1, 0.45);
            for (auto& row : tx.rows) row = glitch::Decode(row, pr, rng, nullptr, false, 2);
        }
        tx.exe = glitch::Decode(tx.exe, glitch::TypeProgress(st, t, 0.3, 0.5), rng);
        tx.title = glitch::Decode(tx.title, glitch::TypeProgress(st, t, 0.35, 0.6), rng, &tx.cursor_typing);
        return tx;
    }
    double ps = (now - g_t_sel) / st.change_ms;
    if (ps < 1 && g_prev_sel < n && g_prev_sel >= 0) {
        glitch::Rng rng = glitch::RngAt(g_seed, now - g_t_sel, 40);
        const Item& old = g_items[g_prev_sel];
        tx.exe = glitch::Morph(AppLabel(old), tx.exe, ps * 1.3, rng);
        tx.title = glitch::Morph(old.title, tx.title, ps, rng);
        tx.cursor_typing = true;
    }
    return tx;
}

void DrawContent(ID2D1RenderTarget* rt, double now) {
    const float W = P(kW), H = P(g_h), px = std::max(1.f, P(1));
    const int n = (int)g_items.size();
    Texts tx = CurrentTexts(now);
    rt->Clear(C(g_pal.paper));

    // Header: a small square, the title in spaced capitals, the count, a rule.
    Fill(rt, g_panel, P(28), P(24), P(36), P(32), C(g_pal.ink));
    Text(rt, g_panel, tx.header, D2D1::RectF(P(46), P(10), P(500), P(46)),
         {P(21), DWRITE_FONT_WEIGHT_SEMI_BOLD, P(4)}, C(g_pal.ink));
    Text(rt, g_panel, tx.count, D2D1::RectF(W - P(260), P(10), W - P(28), P(46)),
         {P(13), DWRITE_FONT_WEIGHT_NORMAL, P(2), kRight}, C(g_pal.ink_soft));
    Fill(rt, g_panel, P(28), P(52), W - P(28), P(52) + px, C(g_pal.ink));
    Fill(rt, g_panel, W - P(40), P(50), W - P(28), P(55), C(g_pal.ink));

    // The list.
    const auto& st = glitch::CurrentStyle();
    int rows = (int)tx.rows.size();
    for (int r = 0; r < rows; r++) {
        int idx = g_first + r;
        Item& it = g_items[idx];
        float y0 = P(kBodyY + r * kRowH), y1 = P(kBodyY + r * kRowH + kRowH - 4);
        bool sel = idx == g_sel;
        if (sel) {
            // The highlight snaps in over three quick steps when it moves.
            double reveal = st.enabled ? glitch::Steps((now - g_t_sel) / 60.0, 3) : 1.0;
            float x1 = P(kListX0) + (P(kListX1) - P(kListX0)) * (float)std::max(0.34, reveal);
            Fill(rt, g_panel, P(kListX0), y0, P(kListX1), y1, C(g_pal.paper_dark, 0.6f));
            Fill(rt, g_panel, P(kListX0), y0, x1, y1, C(g_pal.ink));
            DrawCursor(rt, P(22), (y0 + y1) / 2 - P(8), C(g_pal.ink));
        } else {
            Fill(rt, g_panel, P(kListX0), y0, P(kListX1), y1, C(g_pal.paper_dark, 0.6f));
        }
        // Icon: duotone, on a paper chip when the row is inked.
        float isz = P(18), ix = P(kListX0 + 10), iy = (y0 + y1) / 2 - isz / 2;
        if (sel) Fill(rt, g_panel, ix - P(3), iy - P(3), ix + isz + P(3), iy + isz + P(3), C(g_pal.paper));
        if (it.bmp && it.bmp_gen != g_panel.generation) {
            it.bmp->Release();
            it.bmp = nullptr;
        }
        if (!it.bmp && it.icon && it.bmp_gen != -2) {
            it.bmp = IconBitmap(g_panel.Target(), it.icon, (int)isz, g_cfg.switcher_duotone_icons);
            it.bmp_gen = it.bmp ? g_panel.generation : -2;
        }
        if (it.bmp) {
            rt->DrawBitmap(it.bmp, D2D1::RectF(ix, iy, ix + isz, iy + isz));
        } else {
            Frame(rt, g_panel, ix + P(2), iy + P(2), ix + isz - P(2), iy + isz - P(2), std::max(1.f, P(2)),
                  C(sel ? g_pal.ink : g_pal.ink_soft));
        }
        // A window asked to close stays, faded, until it's really gone (it may ask to save first).
        float fade = it.closing ? 0.45f : 1.f;
        Text(rt, g_panel, tx.rows[r], D2D1::RectF(P(kListX0 + 40), y0, P(kListX1 - kCloseW - 8), y1),
             {P(15), DWRITE_FONT_WEIGHT_NORMAL, P(0.5f)}, C(sel ? g_pal.paper : g_pal.ink, fade));
        DrawClose(rt, y0, y1, sel, idx == g_hover_close, it.closing);
    }
    if (n == 0) {
        float y0 = P(kBodyY), y1 = P(kBodyY + kRowH - 4);
        Fill(rt, g_panel, P(kListX0), y0, P(kListX1), y1, C(g_pal.paper_dark, 0.6f));
        Text(rt, g_panel, Tr(L"NO WINDOWS", L"ウィンドウなし"), D2D1::RectF(P(kListX0 + 14), y0, P(kListX1), y1),
             {P(13), DWRITE_FONT_WEIGHT_NORMAL, P(3)}, C(g_pal.ink_soft));
    }
    if (n > g_rows_max) {  // scroll position
        float top = P(kBodyY), bot = P(kBodyY + g_rows_max * kRowH - 4);
        Fill(rt, g_panel, P(kListX1 + 8), top, P(kListX1 + 8) + px, bot, C(g_pal.ink_soft));
        float t0 = top + (bot - top) * g_first / n, t1 = top + (bot - top) * (g_first + g_rows_max) / n;
        Fill(rt, g_panel, P(kListX1 + 6), t0, P(kListX1 + 11), t1, C(g_pal.ink));
    }

    // The preview frame. The live picture itself is a separate window on top.
    Fill(rt, g_panel, P(kPrevX0), P(kPrevY0), P(kPrevX1), P(kPrevY1), C(g_pal.paper_dark));
    Frame(rt, g_panel, P(kPrevX0), P(kPrevY0), P(kPrevX1), P(kPrevY1), px, C(g_pal.ink));
    CornerBrackets(rt, P(kPrevX0), P(kPrevY0), P(kPrevX1), P(kPrevY1), C(g_pal.ink));
    if (!g_th.ok) {
        std::wstring msg = n == 0                            ? Tr(L"NO SIGNAL", L"信号なし")
                           : IsIconic(g_items[g_sel].hwnd) ? Tr(L"MINIMIZED", L"最小化中")
                                                           : Tr(L"NO PREVIEW", L"プレビュー不可");
        Text(rt, g_panel, msg, D2D1::RectF(P(kPrevX0), P(kPrevY0), P(kPrevX1), P(kPrevY1)),
             {P(13), DWRITE_FONT_WEIGHT_NORMAL, P(5), kCenter}, C(g_pal.ink_soft));
    }

    // Caption: the app in spaced capitals, then the full window title.
    Fill(rt, g_panel, P(kPrevX0), P(312), P(kPrevX1), P(312) + px, C(g_pal.ink_soft));
    Text(rt, g_panel, tx.exe, D2D1::RectF(P(kPrevX0), P(318), P(kPrevX1), P(338)),
         {P(12), DWRITE_FONT_WEIGHT_SEMI_BOLD, P(3)}, C(g_pal.ink_soft));
    TextStyle title_style{P(15), DWRITE_FONT_WEIGHT_NORMAL, 0, kLeft, true};
    Text(rt, g_panel, tx.title, D2D1::RectF(P(kPrevX0), P(342), P(kPrevX1), P(388)), title_style, C(g_pal.ink));
    if (tx.cursor_typing) {
        // The block cursor that leads typing text, after the first line's end.
        TextStyle one = title_style;
        one.wrap = false;
        float tw = std::min(TextWidth(tx.title, one), P(kPrevX1 - kPrevX0 - 14));
        Fill(rt, g_panel, P(kPrevX0) + tw + P(3), P(345), P(kPrevX0) + tw + P(11), P(361), C(g_pal.ink));
    }

    // Footer: key hints and the time.
    float fy = H - P(46);
    Fill(rt, g_panel, P(28), fy, W - P(28), fy + px, C(g_pal.ink));
    float x = P(28), cy = fy + P(24);
    x = KeyHint(rt, x, cy, L"TAB", Tr(L"NEXT", L"次へ"));
    x = KeyHint(rt, x, cy, L"SHIFT+TAB", Tr(L"BACK", L"戻る"));
    x = KeyHint(rt, x, cy, L"ESC", Tr(L"CANCEL", L"キャンセル"));
    x = KeyHint(rt, x, cy, L"DEL", Tr(L"CLOSE", L"閉じる"));
    SYSTEMTIME lt;
    GetLocalTime(&lt);
    wchar_t clock[16];
    swprintf(clock, 16, L"%02d:%02d", lt.wHour, lt.wMinute);
    Text(rt, g_panel, clock, D2D1::RectF(W - P(200), fy, W - P(28), fy + P(46)),
         {P(13), DWRITE_FONT_WEIGHT_NORMAL, P(3), kRight}, C(g_pal.ink_soft));
    g_last_minute = lt.wMinute;
}

// ---- lifecycle ----------------------------------------------------------------------------------
// As many rows as fit in ~85% of the screen's height, so the list rarely scrolls.
void FitRowsToScreen() {
    HMONITOR mon = SwitcherMonitor();
    MONITORINFO mi = MonitorInfoOf(mon);
    float scale = DpiScaleForMonitor(mon);
    float usable = (mi.rcWork.bottom - mi.rcWork.top) / scale * 0.85f - (kBodyY + 14 + 46);
    g_rows_max = std::clamp((int)(usable / kRowH), 6, 40);
}

void KeepSelectionVisible() {
    int n = (int)g_items.size();
    if (n <= g_rows_max) {
        g_first = 0;
        return;
    }
    if (g_sel < g_first + 1) g_first = std::max(0, g_sel - 1);
    if (g_sel > g_first + g_rows_max - 2) g_first = std::min(n - g_rows_max, g_sel - g_rows_max + 2);
}

void Show(double now) {
    HMONITOR mon = SwitcherMonitor();
    MONITORINFO mi = MonitorInfoOf(mon);
    g_mon = mi.rcMonitor;
    g_work = mi.rcWork;
    g_s = DpiScaleForMonitor(mon);
    int rows = std::clamp((int)g_items.size(), 1, g_rows_max);
    float body = std::max(rows * kRowH, 332.f);
    g_h = kBodyY + body + 14 + 46;
    int sw = (int)(P(kW) + 2 * P(kMargin)), sh = (int)(P(g_h) + 2 * P(kMargin));
    g_panel.Resize(sw, sh);
    g_px = (g_work.left + g_work.right) / 2 - sw / 2;
    g_py = (g_work.top + g_work.bottom) / 2 - sh / 2;
    g_t_shown = now;
    g_seed = (uint32_t)(now * 7919.0) ^ 0xA5A5u;
    g_phase = kOpen;
    g_dirty = true;
    Log(L"Switcher open: %d windows, %.0f%% scale", (int)g_items.size(), g_s * 100);
    GetCursorPos(&g_mouse);
    if (!g_items.empty()) SetThumbSource(g_items[g_sel].hwnd);
    // Shown even without dimming, then all but invisible (Render), so a click
    // outside the panel still lands on it and cancels.
    if (g_dim) {
        SetLayeredWindowAttributes(g_dim, 0, 0, LWA_ALPHA);
        SetWindowPos(g_dim, HWND_TOPMOST, g_mon.left, g_mon.top, g_mon.right - g_mon.left, g_mon.bottom - g_mon.top,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
}

// The icons of the rows about to be drawn, asked for before drawing starts:
// waiting on another app's answer lets messages sent to Candela run, and none
// of them should land in the middle of a frame.
void FetchVisibleIcons() {
    for (int r = 0; r < g_rows_max; r++) {
        size_t i = (size_t)(g_first + r);
        if (i >= g_items.size()) break;  // checked each time: the list must not change meanwhile, but if it did...
        if (g_items[i].icon_asked) continue;
        g_items[i].icon_asked = true;
        HWND h = g_items[i].hwnd;
        HICON icon = h ? WindowIcon(h, g_big_icons) : nullptr;
        if (i < g_items.size() && g_items[i].hwnd == h) g_items[i].icon = icon;
    }
}

void Render(double now) {
    const auto& st = glitch::CurrentStyle();
    const float cw = P(kW), ch = P(g_h), ox = P(kMargin), oy = P(kMargin);
    if (g_phase != kClosing) FetchVisibleIcons();
    glitch::Fx fx;
    float dim = 1;
    bool preview = false;
    double preview_glitch = 0;
    double t = now - g_t_shown;
    if (g_phase == kClosing) {
        double tc = now - g_t_close;
        if (tc >= glitch::CloseMs(st)) {
            HideAll();
            return;
        }
        fx = glitch::CloseFx(st, tc, g_seed, kW, g_h);
        dim = (float)(1 - glitch::Clamp01(tc / glitch::CloseMs(st)));
    } else {
        double open_ms = glitch::OpenMs(st);
        if (t < open_ms) fx = glitch::OpenFx(st, t, g_seed, kW, g_h);
        dim = st.enabled ? (float)glitch::Clamp01(t / std::max(1.0, st.line_ms + st.expand_ms)) : 1.f;
        // The preview appears once the panel is open, glitching in.
        double shown_at = st.enabled ? st.line_ms + st.expand_ms : 0;
        preview = t >= shown_at;
        preview_glitch = st.enabled ? std::max(0.0, 1 - (t - shown_at) / 220.0) : 0;
        double ps = now - g_t_sel;
        // Moving the selection glitches only the preview (and the caption
        // scrambles into the new name): the list itself stays calm.
        if (st.enabled && ps < 180) preview_glitch = std::max(preview_glitch, 0.8 * (1 - ps / 180.0));
    }
    if (g_dim) {
        // Not dimming: 1 of 255, which nobody sees but still takes the click.
        BYTE a = g_cfg.switcher_dim ? (BYTE)std::lround(dim * 0.45 * 255) : (BYTE)(g_phase == kClosing ? 0 : 1);
        SetLayeredWindowAttributes(g_dim, 0, a, LWA_ALPHA);
    }

    ID2D1RenderTarget* rt = g_panel.Begin();
    if (!rt) return;
    ID2D1BitmapRenderTarget* scratch = g_panel.Scratch((int)cw, (int)ch);
    if (scratch) {
        scratch->BeginDraw();
        DrawContent(scratch, now);
        scratch->EndDraw();
        ID2D1Bitmap* content = nullptr;
        if (SUCCEEDED(scratch->GetBitmap(&content))) {
            Compose(rt, g_panel, content, ox, oy, cw, ch, fx, g_s);
            content->Release();
        }
    }
    g_panel.End(g_px, g_py, fx.alpha);
    UpdateThumbs(preview_glitch, now, preview && g_phase == kOpen && !g_items.empty());
    g_dirty = false;
}

bool Animating(double now) {
    const auto& st = glitch::CurrentStyle();
    if (g_phase == kClosing) return true;
    return now - g_t_shown < glitch::OpenMs(st) + 260 || now - g_t_sel < 260;
}

void RefreshItems(double now);

// When Alt went up while the switcher is open, or -1. The keyboard hook
// normally sees Alt go up and switches; if that's missed (Windows dropped the
// hook for a moment, a secure desktop took the key), the switcher would stay
// open and keep taking Enter, Delete and the arrows from every app.
double g_alt_up_since = -1;

void Tick() {
    double now = NowMs();
    if ((g_phase == kPending || g_phase == kOpen) && g_fake_now < 0) {
        if (GetAsyncKeyState(VK_MENU) & 0x8000) {
            g_alt_up_since = -1;
        } else if (g_alt_up_since < 0) {
            g_alt_up_since = now;
        } else if (now - g_alt_up_since > 400) {
            Log(L"Switcher: Alt is up but its release wasn't seen; switching");
            g_alt_up_since = -1;
            OnCommit();
            return;
        }
    }
    if (g_phase == kPending) {
        if (now - g_t_start >= g_cfg.switcher_delay_ms) {
            Show(now);
            Render(now);
        }
        return;
    }
    if (g_phase == kHidden) return;
    if (g_watching && g_phase == kOpen) {
        if (now >= g_watch_until) {
            // Still here after a while: it asked something, or refused. Not fading any more.
            g_watching = false;
            for (auto& it : g_items) it.closing = false;
            g_dirty = true;
        } else if (now - g_last_refresh >= kRefreshMs) {
            RefreshItems(now);
        }
    }
    SYSTEMTIME lt;
    GetLocalTime(&lt);
    if (lt.wMinute != g_last_minute) g_dirty = true;
    // One last draw when the animation stops, so its final state (no typing
    // cursor, no scramble) is what stays on screen.
    static bool was_animating = false;
    bool animating = Animating(now);
    if (g_dirty || animating || was_animating) Render(now);
    was_animating = animating;
}


// Bring a window to the front. Windows only lets the app you're using do
// that, so if the plain call is refused, Candela uses the two standard ways
// switchers get permission.
void Activate(HWND h) {
    if (!IsWindow(h)) return;
    if (IsIconic(h)) ShowWindowAsync(h, SW_RESTORE);
    if (SetForegroundWindow(h) && GetForegroundWindow() == h) return;
    INPUT in = {};
    in.type = INPUT_MOUSE;
    in.mi.dwExtraInfo = CANDELA_INJECTED;
    SendInput(1, &in, sizeof(in));
    if (SetForegroundWindow(h) && GetForegroundWindow() == h) return;
    HWND fg = GetForegroundWindow();
    DWORD fg_thread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0, me = GetCurrentThreadId();
    if (fg_thread && fg_thread != me) AttachThreadInput(me, fg_thread, TRUE);
    BringWindowToTop(h);
    SetForegroundWindow(h);
    if (fg_thread && fg_thread != me) AttachThreadInput(me, fg_thread, FALSE);
    if (GetForegroundWindow() != h) Log(L"Could not bring %ls to the front", WindowTitle(h).c_str());
}

void StartClose(double now) {
    g_phase = kClosing;
    g_t_close = now;
    UpdateThumbs(0, now, false);
    Render(now);
}

int RowAt(int x, int y) {  // surface pixel -> row index, or -1
    float px = (x - P(kMargin)) / g_s, py = (y - P(kMargin)) / g_s;
    if (px < kListX0 || px > kListX1 || py < kBodyY) return -1;
    int r = (int)((py - kBodyY) / kRowH);
    if (r < 0 || r >= std::min((int)g_items.size(), g_rows_max)) return -1;
    return g_first + r;
}

int CloseAt(int x, int y) {  // surface pixel -> the row whose × it's on, or -1
    int r = RowAt(x, y);
    if (r < 0) return -1;
    float px = (x - P(kMargin)) / g_s;
    return px >= kListX1 - kCloseW - 4 ? r : -1;
}

void SetHoverClose(int row) {
    if (row == g_hover_close) return;
    g_hover_close = row;
    g_dirty = true;  // drawn on the next tick
}

// The × the pointer is on right now (after the rows moved under a still pointer).
void UpdateHoverFromCursor() {
    POINT pt;
    if (!g_panel.hwnd || !GetCursorPos(&pt) || !ScreenToClient(g_panel.hwnd, &pt)) return;
    SetHoverClose(CloseAt(pt.x, pt.y));
}

void AfterListChanged(HWND selected, int old_sel) {
    int n = (int)g_items.size();
    g_sel = std::min(old_sel, std::max(0, n - 1));
    for (int i = 0; selected && i < n; i++)
        if (g_items[i].hwnd == selected) g_sel = i;
    g_prev_sel = g_sel;  // nothing to scramble from: the old row may be gone
    g_first = std::min(g_first, std::max(0, n - g_rows_max));
    KeepSelectionVisible();
    SetThumbSource(n ? g_items[g_sel].hwnd : nullptr);
    g_dirty = true;
    UpdateHoverFromCursor();
}

// The list again, while windows are closing. The windows still there keep
// their icons and the selection stays on the same window when it can.
void RefreshItems(double now) {
    g_last_refresh = now;
    if (g_fake_now >= 0) return;  // --dump's made-up windows aren't real ones to look for
    std::vector<Item> fresh = Collect();
    bool same = fresh.size() == g_items.size();
    for (size_t i = 0; same && i < fresh.size(); i++) same = fresh[i].hwnd == g_items[i].hwnd;
    if (same) return;
    HWND selected = g_items.empty() ? nullptr : g_items[g_sel].hwnd;
    for (auto& f : fresh)
        for (auto& old : g_items)
            if (old.hwnd == f.hwnd) {
                f.icon = old.icon;
                f.icon_asked = old.icon_asked;
                f.bmp = old.bmp;
                f.bmp_gen = old.bmp_gen;
                f.closing = old.closing;
                old.bmp = nullptr;  // it belongs to the new list now
                break;
            }
    ReleaseItems();  // the bitmaps of the windows that left
    g_items = std::move(fresh);
    AfterListChanged(selected, g_sel);
}

// Asks a window to close, as the taskbar's "Close window" does (so an app
// with unsaved work can still ask first). It stays in the list, faded, until
// it's gone.
void CloseItem(int idx, double now) {
    if (g_phase != kOpen || idx < 0 || idx >= (int)g_items.size() || g_items[idx].closing) return;
    Item& it = g_items[idx];
    if (!it.hwnd) {  // --dump's made-up windows just leave
        if (it.bmp) it.bmp->Release();
        g_items.erase(g_items.begin() + idx);
        AfterListChanged(nullptr, g_sel > idx ? g_sel - 1 : g_sel);
        return;
    }
    if (!PostMessageW(it.hwnd, WM_SYSCOMMAND, SC_CLOSE, 0)) {
        DWORD err = GetLastError();
        Log(L"Switcher: couldn't ask %ls to close (error %lu%ls)", ProcessName(it.hwnd).c_str(), err,
            err == ERROR_ACCESS_DENIED ? L"; it runs as admin and Candela doesn't" : L"");
        return;
    }
    Log(L"Switcher: asked %ls to close", ProcessName(it.hwnd).c_str());
    it.closing = true;
    g_watching = true;
    g_watch_until = now + kWatchMs;
    g_last_refresh = now;  // the first look comes a moment later
    g_dirty = true;
}

}  // namespace

namespace detail {

void HideAll() {
    KillTimer(g_panel.hwnd, 1);
    ClearThumbs();
    if (g_host) ShowWindow(g_host, SW_HIDE);
    if (g_dim) ShowWindow(g_dim, SW_HIDE);
    g_panel.Hide();
    ReleaseItems();
    g_phase = kHidden;
    g_hover_close = -1;
    g_watching = false;
}

void Select(int idx, double now) {
    int n = (int)g_items.size();
    if (!n) return;
    idx = ((idx % n) + n) % n;
    if (idx == g_sel) return;
    g_prev_sel = g_sel;
    g_sel = idx;
    g_t_sel = now;
    KeepSelectionVisible();
    if (g_phase == kOpen) {
        SetThumbSource(g_items[g_sel].hwnd);
        g_dirty = true;
        Render(now);
    }
}

std::wstring AppName(const Item& it) {
    std::wstring e = it.exe;
    if (e.size() > 4 && !_wcsicmp(e.c_str() + e.size() - 4, L".exe")) e.resize(e.size() - 4);
    if (!_wcsicmp(e.c_str(), L"ApplicationFrameHost")) e = L"App";
    return e.empty() ? Tr(L"Window", L"ウィンドウ") : e;
}

}  // namespace detail

namespace {

LRESULT CALLBACK PanelProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_TIMER:
            Tick();
            return 0;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_MOUSEMOVE: {
            if (!g_tracking) {
                TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, h, 0};
                g_tracking = TrackMouseEvent(&tme);
            }
            POINT pt;
            GetCursorPos(&pt);
            if (pt.x == g_mouse.x && pt.y == g_mouse.y) return 0;  // didn't really move
            g_mouse = pt;
            if (g_phase == kOpen) {
                int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
                int r = RowAt(x, y);
                if (r >= 0) Select(r, NowMs());
                SetHoverClose(CloseAt(x, y));
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            g_tracking = false;
            SetHoverClose(-1);
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                SetCursor(LoadCursorW(nullptr, g_hover_close >= 0 ? IDC_HAND : IDC_ARROW));
                return TRUE;
            }
            break;
        case WM_LBUTTONUP:
            if (g_phase == kOpen) {
                int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
                int c = CloseAt(x, y);
                if (c >= 0) {  // the ×: close that window, the switcher stays open
                    CloseItem(c, NowMs());
                    return 0;
                }
                int r = RowAt(x, y);
                if (r >= 0) Select(r, NowMs());
                OnCommit();
            }
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

LRESULT CALLBACK DimProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN) {
        keyboard::switcher_active = false;
        Cancel();
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

LRESULT CALLBACK HostProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (msg == WM_LBUTTONUP) {
        OnCommit();
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

HWND MakePlainWindow(const wchar_t* cls, WNDPROC proc, HBRUSH bg, DWORD ex, HWND owner) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = proc;
    wc.hInstance = g_inst;
    wc.lpszClassName = cls;
    wc.hbrBackground = bg;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);
    return CreateWindowExW(ex | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, cls, L"Candela", WS_POPUP, 0, 0, 1, 1,
                           owner, nullptr, g_inst, nullptr);
}

}  // namespace

bool Init() {
    g_dim_brush = CreateSolidBrush(ToColorRef(g_pal.shade));
    g_host_brush = CreateSolidBrush(ToColorRef(g_pal.paper_dark));
    g_dim = MakePlainWindow(L"CandelaDim", DimProc, g_dim_brush, WS_EX_LAYERED, nullptr);
    if (!g_panel.Create(L"CandelaSwitcher", PanelProc, nullptr)) return false;
    g_host = MakePlainWindow(L"CandelaPreview", HostProc, g_host_brush, 0, g_panel.hwnd);
    return g_panel.hwnd != nullptr;
}

void OnSwitch(int dir) {
    double now = NowMs();
    if (g_phase == kHidden || g_phase == kClosing) {
        if (g_phase == kClosing) HideAll();
        Enumerate();
        FitRowsToScreen();
        int n = (int)g_items.size();
        g_sel = n > 1 ? (dir > 0 ? 1 : n - 1) : 0;
        g_prev_sel = g_sel;
        g_t_sel = -1e9;
        g_first = 0;
        KeepSelectionVisible();
        g_t_start = now;
        g_alt_up_since = -1;
        g_phase = kPending;
        SetTimer(g_panel.hwnd, 1, 10, nullptr);
        if (g_cfg.switcher_delay_ms <= 0) {
            Show(now);
            Render(now);
        }
        return;
    }
    bool was_pending = g_phase == kPending;
    Select(g_sel + dir, now);
    if (was_pending && g_phase == kPending) g_prev_sel = g_sel;  // no scramble for moves nobody saw
}

void OnKey(UINT vk) {
    if (g_phase == kHidden || g_phase == kClosing) return;
    double now = NowMs();
    switch (vk) {
        case VK_ESCAPE: Cancel(); break;
        case VK_RETURN: keyboard::switcher_active = false; OnCommit(); break;
        case VK_LEFT: case VK_UP: Select(g_sel - 1, now); break;
        case VK_RIGHT: case VK_DOWN: Select(g_sel + 1, now); break;
        case VK_HOME: Select(0, now); break;
        case VK_END: Select((int)g_items.size() - 1, now); break;
        case VK_DELETE: CloseItem(g_sel, now); break;
    }
}

void OnCommit() {
    if (g_phase == kHidden || g_phase == kClosing) return;
    keyboard::switcher_active = false;
    HWND target = g_items.empty() ? nullptr : g_items[g_sel].hwnd;
    bool pending = g_phase == kPending;
    if (pending) HideAll();  // quick Alt+Tab: nothing was shown, just switch
    if (target) Activate(target);
    if (!pending) {
        StartClose(NowMs());
    }
}

void Cancel() {
    if (g_phase == kHidden || g_phase == kClosing) return;
    keyboard::switcher_active = false;
    if (g_phase == kPending) {
        HideAll();
        return;
    }
    StartClose(NowMs());
}

bool IsOwnWindow(HWND h) { return h && (h == g_panel.hwnd || h == g_dim || h == g_host); }

void DebugTick() { Tick(); }
bool DebugSave(const std::wstring& png) { return g_panel.Visible() && g_panel.SavePng(png); }

void StyleChanged() {
    // The dimmed screen and the preview's background are window brushes.
    HBRUSH dim = CreateSolidBrush(ToColorRef(g_pal.shade)), host = CreateSolidBrush(ToColorRef(g_pal.paper_dark));
    if (g_dim) SetClassLongPtrW(g_dim, GCLP_HBRBACKGROUND, (LONG_PTR)dim);
    if (g_host) SetClassLongPtrW(g_host, GCLP_HBRBACKGROUND, (LONG_PTR)host);
    if (g_dim_brush) DeleteObject(g_dim_brush);
    if (g_host_brush) DeleteObject(g_host_brush);
    g_dim_brush = dim;
    g_host_brush = host;
}

void Shutdown() {
    HideAll();
    if (g_cursor_geo) g_cursor_geo->Release();
    g_cursor_geo = nullptr;
    g_panel.Destroy();
    if (g_host) DestroyWindow(g_host);
    if (g_dim) DestroyWindow(g_dim);
    g_host = g_dim = nullptr;
    if (g_host_brush) DeleteObject(g_host_brush);
    if (g_dim_brush) DeleteObject(g_dim_brush);
}

}  // namespace switcher
