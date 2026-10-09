// Two window helpers: black out the other screens, and make a window
// borderless over its whole screen.
#include "candela.h"

// ---- black out other screens -------------------------------------------------------------------------
// Like a fullscreen black picture on the other monitors, one hotkey away.
// The black never takes focus (a video on your screen keeps playing), isn't
// in the taskbar or Alt+Tab, and swallows clicks harmlessly.
//
// By default it acts like a black desktop background rather than a lid: it
// covers every window on those screens when it appears, but any window you
// then bring there (Alt+Tab, the taskbar, dragging it over) shows on top,
// because Windows raises whatever you activate and the black can never be
// activated. The taskbar on those screens is covered by a thin strip that
// does stay on top. With blackout_always_on_top the black covers everything.
//
// Getting "above every window, but not always on top" right: Windows only
// lets a program lift its own window to the top of the normal windows when
// it's allowed to take the foreground, and a hotkey doesn't always grant
// that. So the black is first shown always-on-top (which Windows never
// refuses), then lowered to sit directly under the window you're using.
// Lowering a window is never refused either, and "under the window in front"
// is above everything else.
namespace blackout {

namespace {
struct Black {
    HWND hwnd;
    bool screen;  // a whole screen (false: a taskbar strip, which always stays on top)
};
std::vector<Black> g_windows;
bool g_cover_all = false;
const wchar_t* kClass = L"CandelaBlackout";

LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;  // clicking the black must not steal focus
    if (msg == WM_ERASEBKGND) {
        // Black, or the color the animation is passing through (the sunset's
        // sky), which blackfx keeps in the window's user data.
        RECT rc;
        GetClientRect(h, &rc);
        HBRUSH b = (HBRUSH)GetStockObject(DC_BRUSH);  // a stock brush in this color: nothing to create or delete per paint
        SetDCBrushColor((HDC)wp, (COLORREF)GetWindowLongPtrW(h, GWLP_USERDATA));
        FillRect((HDC)wp, &rc, b);
        return 1;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

bool Topmost(HWND h) { return (GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0; }

// `animated`: it starts see-through, and blackfx brings the black in.
HWND Black(const RECT& r, bool screen, bool animated) {
    DWORD ex = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST | (animated ? WS_EX_LAYERED : 0);
    int w = r.right - r.left, h = r.bottom - r.top;
    HWND hwnd = CreateWindowExW(ex, kClass, L"Candela blackout", WS_POPUP, r.left, r.top, w, h, nullptr, nullptr, g_inst, nullptr);
    if (!hwnd) {
        Log(L"Could not create a blackout window (error %lu)", GetLastError());
        return nullptr;
    }
    if (animated) SetLayeredWindowAttributes(hwnd, 0, 0, LWA_ALPHA);
    if (!SetWindowPos(hwnd, HWND_TOPMOST, r.left, r.top, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW))
        Log(L"Blackout: Windows refused to show it (error %lu)", GetLastError());
    g_windows.push_back({hwnd, screen});
    return hwnd;
}

// Is `h` a window you'd be working in (not the desktop, taskbar or Candela)?
bool IsUserWindow(HWND h) {
    if (!h || !IsWindowVisible(h)) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId()) return false;
    wchar_t cls[64] = {};
    GetClassNameW(h, cls, 64);
    for (auto* shell : {L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd"})
        if (!wcscmp(cls, shell)) return false;
    return true;
}

// Lowers the whole-screen blacks from always-on-top to just under `front`.
void SinkUnder(HWND front) {
    if (g_cover_all) return;
    for (auto& b : g_windows) {
        if (!b.screen || !Topmost(b.hwnd)) continue;
        bool ok;
        if (IsUserWindow(front) && !Topmost(front)) {
            ok = SetWindowPos(b.hwnd, front, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        } else {
            // Nothing usable in front (the desktop, the tray menu): top of the normal windows.
            ok = SetWindowPos(b.hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        if (!ok || Topmost(b.hwnd))
            Log(L"Blackout: couldn't lower the black under the window in front (error %lu); it stays on top until you "
                L"switch windows",
                GetLastError());
    }
}

// Is `a` above `b` in the z-order?
bool IsAbove(HWND a, HWND b) {
    int guard = 0;
    for (HWND h = GetWindow(a, GW_HWNDNEXT); h && guard < 5000; h = GetWindow(h, GW_HWNDNEXT), guard++)
        if (h == b) return true;
    return false;
}

BOOL CALLBACK Collect(HMONITOR mon, HDC, LPRECT r, LPARAM lp) {
    ((std::vector<std::pair<HMONITOR, RECT>>*)lp)->push_back({mon, *r});
    return TRUE;
}
}  // namespace

bool Init() {
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = Proc;
    wc.hInstance = g_inst;
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kClass;
    return RegisterClassExW(&wc) != 0;
}

bool Active() { return !g_windows.empty(); }

bool Owns(HWND h) {
    for (auto& b : g_windows)
        if (b.hwnd == h) return true;
    return false;
}

void Clear() {
    blackfx::StopAll();
    for (auto& b : g_windows) DestroyWindow(b.hwnd);
    if (!g_windows.empty()) Log(L"Blackout off");
    g_windows.clear();
}

int Toggle(HMONITOR keep, bool always_on_top) {
    if (!g_windows.empty()) {
        Clear();
        return 0;
    }
    g_cover_all = always_on_top;
    std::vector<std::pair<HMONITOR, RECT>> mons;
    EnumDisplayMonitors(nullptr, nullptr, Collect, (LPARAM)&mons);
    const bool animated = blackfx::Wanted();
    struct Screen { HMONITOR mon; RECT r; std::vector<HWND> blacks; };
    std::vector<Screen> screens;
    for (auto& [mon, r] : mons) {
        if (mon == keep) continue;
        Screen sc{mon, r, {}};
        auto add = [&](const RECT& part, bool whole) {
            if (HWND h = Black(part, whole, animated)) sc.blacks.push_back(h);
        };
        add(r, true);
        if (!always_on_top) {
            // The parts of the screen outside its work area: where its taskbar is.
            MONITORINFO mi = {sizeof(mi)};
            if (GetMonitorInfoW(mon, &mi)) {
                const RECT& w = mi.rcWork;
                if (w.bottom < r.bottom) add({r.left, w.bottom, r.right, r.bottom}, false);
                if (w.top > r.top) add({r.left, r.top, r.right, w.top}, false);
                if (w.left > r.left) add({r.left, r.top, w.left, r.bottom}, false);
                if (w.right < r.right) add({w.right, r.top, r.right, r.bottom}, false);
            }
        }
        if (!sc.blacks.empty()) screens.push_back(sc);  // a screen whose black couldn't be made isn't counted
    }
    SinkUnder(GetForegroundWindow());
    // Each screen plays the animation once, as its black comes in.
    if (animated)
        for (auto& sc : screens)
            if (!sc.blacks.empty()) blackfx::Start(sc.mon, sc.r, sc.blacks);
    Log(L"Blacked out %d other screen(s) (%ls%ls)", (int)screens.size(), always_on_top ? L"always on top" : L"windows can go on top",
        animated ? L", animated" : L"");
    int count = (int)screens.size();
    return count;
}

void OnForeground(HWND fg) {
    // A window you switch to must show above the black. Windows raises it by
    // itself; this is the safety net for when it doesn't (and for a black
    // that couldn't be lowered when it appeared). Only a black that is
    // actually above that window moves, and only to just under it, so
    // windows you brought over before stay uncovered.
    if (g_windows.empty() || Owns(fg) || !IsUserWindow(fg)) return;
    // A window brought to a screen that is still animating: the stars (or
    // the panel) mustn't draw over it, so that screen goes plain black now.
    blackfx::Finish(MonitorFromWindow(fg, MONITOR_DEFAULTTONULL));
    SinkUnder(fg);
    if (g_cover_all) return;
    RECT fr;
    if (!GetWindowRect(fg, &fr)) return;
    for (auto& b : g_windows) {
        RECT br, overlap;
        if (!b.screen || !GetWindowRect(b.hwnd, &br) || !IntersectRect(&overlap, &br, &fr)) continue;
        if (IsAbove(b.hwnd, fg) && !SetWindowPos(b.hwnd, fg, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE))
            Log(L"Blackout: couldn't move the black under the window you switched to (error %lu)", GetLastError());
    }
}

}  // namespace blackout

// ---- borderless ------------------------------------------------------------------------------------------
// For apps whose own fullscreen flashes the screen. The window is made a
// pixel bigger than the screen on every side (configurable), which hides its
// title bar and the taskbar without Windows treating it as true fullscreen.
// Pressing the hotkey again, or quitting Candela, puts it back exactly as it was.
namespace borderless {

namespace {
struct Saved { LONG_PTR style, exstyle; WINDOWPLACEMENT placement; RECT rect; };  // rect: its normal size, on screen
std::map<HWND, Saved> g_saved;
const LONG_PTR kRemoveStyle = WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
const LONG_PTR kRemoveEx = WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE;

bool SetStyles(HWND h, LONG_PTR style, LONG_PTR ex) {
    // SetWindowLongPtr returns the old value, which can be 0: failure shows in the last error.
    SetLastError(0);
    if (!SetWindowLongPtrW(h, GWL_STYLE, style) && GetLastError()) return false;
    SetLastError(0);
    if (!SetWindowLongPtrW(h, GWL_EXSTYLE, ex) && GetLastError()) return false;
    return true;
}

std::wstring Describe(HWND h) {
    wchar_t cls[64] = {};
    GetClassNameW(h, cls, 64);
    RECT r = {};
    GetWindowRect(h, &r);
    wchar_t buf[256];
    swprintf(buf, 256, L"%ls [%ls] %ldx%ld at %ld,%ld", ProcessName(h).c_str(), cls, r.right - r.left, r.bottom - r.top,
             r.left, r.top);
    return buf;
}

void Restore(HWND h) {
    auto it = g_saved.find(h);
    if (it == g_saved.end()) return;
    Saved s = it->second;
    g_saved.erase(it);
    SetStyles(h, s.style, s.exstyle);
    // Back to its size first (some apps ignore a placement set by another
    // program), then the placement, which also brings back "maximized".
    const RECT& r = s.rect;
    SetWindowPos(h, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
                 SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE);
    SetWindowPlacement(h, &s.placement);
    if (s.placement.showCmd == SW_SHOWMAXIMIZED && !IsZoomed(h)) ShowWindow(h, SW_SHOWMAXIMIZED);
    Log(L"Borderless off: now %ls", Describe(h).c_str());
}

void ForgetClosed() {
    for (auto it = g_saved.begin(); it != g_saved.end();) it = IsWindow(it->first) ? std::next(it) : g_saved.erase(it);
}

// What Candela wants a borderless window to look like: for the check afterwards.
struct Want { LONG_PTR style, exstyle; RECT rect; int tries; };
std::map<HWND, Want> g_want;

bool Apply(HWND h, const Want& w) {
    if (!SetStyles(h, w.style, w.exstyle)) return false;
    // No z-order change and no activation: the window is already the one in
    // front, and asking for either can make Windows refuse the whole call.
    return SetWindowPos(h, nullptr, w.rect.left, w.rect.top, w.rect.right - w.rect.left, w.rect.bottom - w.rect.top,
                        SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW) != 0;
}

bool Matches(HWND h, const Want& w) {
    RECT r = {};
    GetWindowRect(h, &r);
    LONG_PTR style = GetWindowLongPtrW(h, GWL_STYLE);
    return EqualRect(&r, &w.rect) && (style & kRemoveStyle) == 0;
}

// Some apps put their own frame or size back a moment later. Check twice,
// and put ours back once more if that happened. One timer for all windows:
// toggling several quickly mustn't start a chain of checks for each.
UINT_PTR g_recheck = 0;

void CALLBACK Recheck(HWND, UINT, UINT_PTR id, DWORD) {
    KillTimer(nullptr, id);
    g_recheck = 0;
    for (auto it = g_want.begin(); it != g_want.end();) {
        HWND h = it->first;
        Want& w = it->second;
        if (!IsWindow(h) || !g_saved.count(h)) {
            it = g_want.erase(it);
            continue;
        }
        if (!Matches(h, w)) {
            Log(L"Borderless: the app changed it back (now %ls); applying again", Describe(h).c_str());
            if (!Apply(h, w)) Log(L"Borderless: Windows refused again (error %lu)", GetLastError());
        }
        if (++w.tries >= 2) it = g_want.erase(it);
        else ++it;
    }
    if (!g_want.empty()) g_recheck = SetTimer(nullptr, 0, 400, Recheck);
}
}  // namespace

bool Toggle(HWND h, int overhang, bool* on, std::wstring* error) {
    ForgetClosed();
    if (!h || !IsWindow(h)) {
        *error = Tr(L"no window in front", L"前面にウィンドウなし");
        return false;
    }
    if (g_saved.count(h)) {
        g_want.erase(h);
        Restore(h);
        *on = false;
        return true;
    }
    wchar_t cls[256] = {};
    GetClassNameW(h, cls, 256);
    for (auto* not_app : {L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd"})
        if (!wcscmp(cls, not_app)) {
            *error = Tr(L"not an app window", L"アプリのウィンドウではない");
            Log(L"Borderless: %ls is not an app window", Describe(h).c_str());
            return false;
        }
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId()) {
        *error = Tr(L"that's Candela itself", L"当機自身は対象外");
        return false;
    }
    Log(L"Borderless: making %ls borderless", Describe(h).c_str());
    Saved s;
    s.style = GetWindowLongPtrW(h, GWL_STYLE);
    s.exstyle = GetWindowLongPtrW(h, GWL_EXSTYLE);
    s.placement = {sizeof(s.placement)};
    GetWindowPlacement(h, &s.placement);
    MONITORINFO mi = {sizeof(mi)};
    if (!GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi)) {
        *error = Tr(L"can't find its screen", L"画面を特定できず");
        return false;
    }
    if (IsZoomed(h)) ShowWindow(h, SW_RESTORE);
    GetWindowRect(h, &s.rect);  // after restoring: its normal size
    const RECT& r = mi.rcMonitor;
    int o = std::max(0, overhang);
    Want w{s.style & ~kRemoveStyle, s.exstyle & ~kRemoveEx, {r.left - o, r.top - o, r.right + o, r.bottom + o}, 0};
    SetLastError(0);
    if (!Apply(h, w)) {
        DWORD err = GetLastError();
        Log(L"Borderless: Windows refused (error %lu)", err);
        SetStyles(h, s.style, s.exstyle);  // don't leave it half done
        SetWindowPos(h, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        SetWindowPlacement(h, &s.placement);
        *error = err == ERROR_ACCESS_DENIED ? Tr(L"Windows refused (admin app?)", L"Windowsが拒否（管理者権限？）") : Tr(L"Windows refused", L"Windowsが拒否");
        return false;
    }
    g_saved[h] = s;
    g_want[h] = w;
    if (!g_recheck) g_recheck = SetTimer(nullptr, 0, 250, Recheck);
    Log(L"Borderless on: now %ls", Describe(h).c_str());
    *on = true;
    return true;
}

void RestoreAll() {
    std::vector<HWND> all;
    for (auto& [h, s] : g_saved) all.push_back(h);
    for (HWND h : all)
        if (IsWindow(h)) Restore(h);
    g_saved.clear();
}

}  // namespace borderless
