// The settings window (settings_window.cpp), and where windows go.
#include "settings_ui.h"

#include <algorithm>
#include <cmath>

namespace settings {

void Open() {
    ui::Main().Open(nullptr);
}

void OpenOn(const std::wstring& section) {
    ui::Main().ShowSection(section);
    Open();
}

bool IsOwnWindow(HWND h) { return ui::Main().Owns(h); }

void Refresh() { ui::Main().Refresh(); }

void DebugDump(const std::wstring& dir) { ui::Main().DebugDump(dir); }

void Shutdown() { ui::Main().Shutdown(); }

}  // namespace settings

namespace ui {

Placement Place(float w, float h, float margin, const POINT* center) {
    POINT pt;
    if (center) pt = *center;
    else GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = MonitorInfoOf(mon);
    const RECT& r = mi.rcWork;
    float s = DpiScaleForMonitor(mon);
    // Shrink to fit a small screen (a laptop at 150%, say).
    float fit = std::min((r.right - r.left) / (w + 2 * margin), (r.bottom - r.top) / (h + 2 * margin));
    s = std::max(0.5f, std::min(s, fit));
    int pw = (int)(std::round(w * s) + 2 * std::round(margin * s)), ph = (int)(std::round(h * s) + 2 * std::round(margin * s));
    int x = center ? pt.x - pw / 2 : (r.left + r.right - pw) / 2;
    int y = center ? pt.y - ph / 2 : (r.top + r.bottom - ph) / 2;
    x = std::clamp(x, (int)r.left - (int)std::round(margin * s), std::max((int)r.left, (int)r.right - pw + (int)std::round(margin * s)));
    y = std::clamp(y, (int)r.top - (int)std::round(margin * s), std::max((int)r.top, (int)r.bottom - ph + (int)std::round(margin * s)));
    return {x, y, pw, ph, s};
}

void SetIcons(HWND h) {
    HICON big = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_SHARED);
    HICON small = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                    GetSystemMetrics(SM_CYSMICON), LR_SHARED);
    SendMessageW(h, WM_SETICON, ICON_BIG, (LPARAM)big);
    SendMessageW(h, WM_SETICON, ICON_SMALL, (LPARAM)small);
}

}  // namespace ui
