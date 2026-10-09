// Settings file, log, clocks, colors and small helpers.
#include "candela.h"

#include <shlobj.h>
#include <shellscalingapi.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cwctype>

core::Config g_cfg;
HINSTANCE g_inst = nullptr;
Palette g_pal = {Hex(0x1F1E1B), Hex(0x2C2A25), Hex(0xD6D0B8), Hex(0x8F8A78), Hex(0x000000)};
std::atomic<bool> g_fg_exclusive{false};
static std::map<std::string, std::string> g_unknown;  // keys from a newer/older version, kept as they are
static bool g_dirty = false;       // changed in Candela and not written yet
static bool g_read_failed = false; // the file couldn't be understood: never overwrite it by itself
static FILETIME g_file_time = {};  // when config.json was last written, as we last read or wrote it
static bool g_first_run = false;   // there was no config.json when Candela started

bool FirstRun() { return g_first_run; }

static FILETIME FileTime() {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(ConfigPath().c_str(), GetFileExInfoStandard, &a)) return {};
    return a.ftLastWriteTime;
}

static std::wstring RoamingDir(const wchar_t* name) {
    PWSTR path = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &path))) {
        dir = std::wstring(path) + L"\\" + name;
        CoTaskMemFree(path);
    } else {
        dir = std::wstring(L".\\") + name;
    }
    return dir;
}

std::wstring AppDir() {
    static std::wstring dir;
    if (!dir.empty()) return dir;
    dir = RoamingDir(L"Candela");
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::wstring ExePath() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)std::size(buf));
    return std::wstring(buf, n);
}

std::wstring ExeDir() {
    std::wstring p = ExePath();
    size_t slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : p.substr(0, slash);
}

std::wstring ConfigPath() { return AppDir() + L"\\config.json"; }

static bool ReadFile(const std::wstring& path, std::string* out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size;
    bool ok = GetFileSizeEx(f, &size) && size.QuadPart < (1 << 20);
    if (ok) {
        out->resize((size_t)size.QuadPart);
        DWORD got = 0;
        ok = ::ReadFile(f, out->data(), (DWORD)out->size(), &got, nullptr) && got == out->size();
    }
    CloseHandle(f);
    if (ok && out->size() >= 3 && (unsigned char)(*out)[0] == 0xEF) out->erase(0, 3);  // UTF-8 BOM
    return ok;
}

// ---- colors -------------------------------------------------------------------------------------
// The built-in palettes: paper, paper_dark, ink, ink_soft, shade.
static const struct {
    const wchar_t* name;
    uint32_t c[5];
} kPalettes[] = {
    {L"warm", {0x1F1E1B, 0x2C2A25, 0xD6D0B8, 0x8F8A78, 0x000000}},  // beige on warm charcoal
    {L"grey", {0x18191C, 0x2C2F34, 0xE8E8E8, 0x9AA0A6, 0x000000}},  // neutral
};

const std::vector<std::wstring>& PaletteNames() {
    static const std::vector<std::wstring> names = {L"warm", L"grey"};
    return names;
}

static bool ParseHex(std::wstring s, uint32_t* out) {
    while (!s.empty() && iswspace(s.front())) s.erase(s.begin());
    while (!s.empty() && iswspace(s.back())) s.pop_back();
    if (!s.empty() && s[0] == L'#') s.erase(0, 1);
    if (s.size() != 6) return false;
    wchar_t* end = nullptr;
    unsigned long v = wcstoul(s.c_str(), &end, 16);
    if (!end || *end) return false;
    *out = (uint32_t)v;
    return true;
}

void ApplyPalette() {
    for (auto& p : kPalettes)
        if (g_cfg.palette == p.name) {
            g_pal = {Hex(p.c[0]), Hex(p.c[1]), Hex(p.c[2]), Hex(p.c[3]), Hex(p.c[4])};
            return;
        }
    // custom: the five colors from the file, each falling back to the warm one's
    const std::wstring* text[5] = {&g_cfg.color_paper, &g_cfg.color_paper_dark, &g_cfg.color_ink, &g_cfg.color_ink_soft,
                                   &g_cfg.color_shade};
    Color* dst[5] = {&g_pal.paper, &g_pal.paper_dark, &g_pal.ink, &g_pal.ink_soft, &g_pal.shade};
    for (int i = 0; i < 5; i++) {
        uint32_t v = kPalettes[0].c[i];
        ParseHex(*text[i], &v);
        *dst[i] = Hex(v);
    }
}

COLORREF ToColorRef(const Color& c) { return RGB((int)(c.r * 255 + .5f), (int)(c.g * 255 + .5f), (int)(c.b * 255 + .5f)); }

// ---- the settings file -------------------------------------------------------------------------------------
// Sanity limits, so a hand-edited file can't make Candela misbehave.
static void Clamp(core::Config& c) {
    auto clampi = [](int& v, int lo, int hi) { v = v < lo ? lo : v > hi ? hi : v; };
    clampi(c.day_kelvin, core::kMinK, core::kMaxK);
    clampi(c.evening_kelvin, core::kMinK, core::kMaxK);
    clampi(c.night_kelvin, core::kMinK, core::kMaxK);
    clampi(c.sunset_offset_min, -240, 240);
    clampi(c.warmth_step_k, 10, 2000);
    clampi(c.borderless_overhang_px, 0, 8);
    clampi(c.watchdog_ms, 20, 5000);
    clampi(c.burst_ms, 10, 1000);
    clampi(c.force_reapply_ms, 0, 60000);
    clampi(c.burst_fast_ms, 0, 50);
    clampi(c.switcher_delay_ms, 0, 1000);
    clampi(c.volume_step, 1, 25);
    // The same limits Dim() always used, applied here too, so nothing odd (a NaN) reaches it.
    if (!(c.dim_step > 0)) c.dim_step = 0.1;
    c.dim_step = std::clamp(c.dim_step, 0.01, 0.5);
    if (!(c.dim_min == c.dim_min)) c.dim_min = 0.1;  // NaN
    c.dim_min = std::clamp(c.dim_min, 0.05, 1.0);
    if (!(c.darkroom_brightness >= 0.05 && c.darkroom_brightness <= 1)) c.darkroom_brightness = c.darkroom_brightness > 1 ? 1 : 0.05;
    if (!(c.latitude >= -90 && c.latitude <= 90)) c.latitude = core::kPlaceholderLat;
    if (!(c.longitude >= -180 && c.longitude <= 180)) c.longitude = core::kPlaceholderLon;
    if (c.animation != L"off" && c.animation != L"subtle") c.animation = L"full";
    if (c.language != L"ja") c.language = L"en";
    if (c.font.empty()) c.font = L"Segoe UI";
    if (c.palette != L"grey" && c.palette != L"custom") c.palette = L"warm";
    if (!core::IsPopupCorner(c.popup_position)) c.popup_position = L"bottom-right";
    if (!core::IsPopupCorner(c.media_popup_position)) c.media_popup_position = L"same";
    if (c.switcher_display.empty()) c.switcher_display = L"active";
}

bool LoadConfig(bool reload) {
    core::Config c;
    std::string text;
    std::map<std::string, std::string> unknown;
    FILETIME time = FileTime();
    bool bad = false;
    if (!reload && GetFileAttributesW(ConfigPath().c_str()) == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND) {
        g_first_run = true;
    }
    bool read = ReadFile(ConfigPath(), &text);
    static int unreadable = 0;  // reloads in a row that couldn't read the file
    if (!read && reload) {
        // Can't be read right now: an editor holding it while it saves, it grew
        // past 1 MB, or it was deleted. Keep everything as it is (not the
        // defaults). A lock is usually gone by the next check; after a few
        // tries, wait until the file changes again.
        bool gone = GetFileAttributesW(ConfigPath().c_str()) == INVALID_FILE_ATTRIBUTES;
        if (gone || ++unreadable >= 5) {
            g_file_time = time;
            unreadable = 0;
            Log(gone ? L"config.json is gone; keeping the current settings (they're written again on the next change)"
                     : L"config.json can't be read; keeping the current settings until it changes again");
        }
        return false;
    }
    unreadable = 0;
    if (read) {
        if (!core::ParseConfig(text, &c, &unknown)) {
            bad = true;
            CopyFileW(ConfigPath().c_str(), (ConfigPath() + L".bad").c_str(), FALSE);
            if (reload) {
                // Edited by hand while Candela runs, with a mistake: keep everything
                // as it is until the file is fixed, rather than half of it turning
                // into defaults (the colors and hotkeys would change under you).
                g_file_time = time;
                g_read_failed = true;
                Log(L"config.json has a mistake (a copy is in config.json.bad); keeping the current settings until it's fixed");
                return false;
            }
            // Starting up: use what could be read, defaults for the rest, and
            // don't write over the file unless you change a setting.
            Log(L"config.json couldn't be read (a typo?); using the defaults for what's missing. A copy is in config.json.bad");
        }
    }
    Clamp(c);
    // Settings older versions had and this one dropped: not carried along forever.
    for (const char* gone : {"app_icon", "icon_entry_hidden"}) unknown.erase(gone);
    g_cfg = c;
    g_unknown = unknown;
    g_read_failed = bad;
    g_file_time = time;
    g_dirty = false;
    ApplyPalette();
    return !bad;
}

void MarkConfigDirty() { g_dirty = true; }
bool ConfigDirty() { return g_dirty; }

bool ConfigFileChanged() {
    FILETIME t = FileTime();
    return CompareFileTime(&t, &g_file_time) != 0;
}

void SaveConfigNow() {
    // Only a write that worked clears these: a failed one stays "not saved yet"
    // and is tried again (engine.cpp, and at exit).
    bool was_read_failed = g_read_failed;
    g_dirty = false;
    g_read_failed = false;
    std::string json = core::SerializeConfig(g_cfg, g_unknown);
    std::wstring path = ConfigPath(), tmp = path + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        Log(L"Couldn't write the settings (error %lu)", GetLastError());
        g_dirty = true;
        g_read_failed = was_read_failed;
        return;
    }
    DWORD written = 0;
    bool ok = WriteFile(f, json.data(), (DWORD)json.size(), &written, nullptr) && written == json.size();
    CloseHandle(f);
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        Log(L"Couldn't save the settings (error %lu)", GetLastError());
        DeleteFileW(tmp.c_str());
        g_dirty = true;
        g_read_failed = was_read_failed;
    }
    g_file_time = FileTime();
}

void Log(const wchar_t* fmt, ...) {
    static std::wstring path = AppDir() + L"\\candela.log";
    wchar_t msg[1024];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf(msg, std::size(msg) - 1, fmt, args);
    va_end(args);
    msg[std::size(msg) - 1] = 0;
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t line[1200];
    int n = _snwprintf(line, std::size(line) - 1, L"%04d-%02d-%02d %02d:%02d:%02d,%03d %5lu %ls\r\n", t.wYear, t.wMonth,
                       t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentThreadId(), msg);
    if (n < 0) n = (int)std::size(line) - 1;
    line[std::size(line) - 1] = 0;
    // Kept small: one old copy.
    WIN32_FILE_ATTRIBUTE_DATA attr;
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attr) && attr.nFileSizeLow > 256000)
        MoveFileExW(path.c_str(), (path + L".1").c_str(), MOVEFILE_REPLACE_EXISTING);
    HANDLE f = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    std::string utf8 = core::ToUtf8(line);
    DWORD written;
    WriteFile(f, utf8.data(), (DWORD)utf8.size(), &written, nullptr);
    CloseHandle(f);
}

// ---- clocks -----------------------------------------------------------------------------------------
double g_fake_now = -1;
core::LocalTime g_fake_local = -1;

double NowMs() {
    if (g_fake_now >= 0) return g_fake_now;
    static LARGE_INTEGER freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)freq.QuadPart;
}

static core::LocalTime FromSystemTime(const SYSTEMTIME& t) {
    return core::Combine({t.wYear, t.wMonth, t.wDay}, {t.wHour, t.wMinute}) + t.wSecond + t.wMilliseconds / 1000.0;
}

core::LocalTime NowLocal() {
    if (g_fake_local >= 0) return g_fake_local;
    SYSTEMTIME t;
    GetLocalTime(&t);
    return FromSystemTime(t);
}

// The wall clock at a UTC moment, with the daylight saving rules for that
// date (Windows' own time zone settings).
core::LocalTime core::LocalFromUtc(double utc) {
    double whole = std::floor(utc);
    ULARGE_INTEGER ft;
    ft.QuadPart = (ULONGLONG)((whole + 11644473600.0) * 10000000.0);
    FILETIME f = {ft.LowPart, ft.HighPart};
    SYSTEMTIME st, local;
    FileTimeToSystemTime(&f, &st);
    // The Ex version uses the rules for that particular year; looked up at run
    // time because Wine (used for testing) doesn't have it.
    using ExFn = BOOL(WINAPI*)(const DYNAMIC_TIME_ZONE_INFORMATION*, const SYSTEMTIME*, LPSYSTEMTIME);
    static ExFn ex = (ExFn)(void*)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SystemTimeToTzSpecificLocalTimeEx");
    DYNAMIC_TIME_ZONE_INFORMATION tz;
    bool ok = ex && GetDynamicTimeZoneInformation(&tz) != TIME_ZONE_ID_INVALID && ex(&tz, &st, &local);
    if (!ok && !SystemTimeToTzSpecificLocalTime(nullptr, &st, &local)) local = st;
    local.wMilliseconds = 0;
    return FromSystemTime(local) + (utc - whole);
}

// ---- screens and windows ------------------------------------------------------------------------------
MONITORINFO MonitorInfoOf(HMONITOR mon) {
    MONITORINFO mi = {sizeof(mi)};
    if (mon && GetMonitorInfoW(mon, &mi)) return mi;
    mi = {sizeof(mi)};
    mi.rcMonitor = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &mi.rcWork, 0)) mi.rcWork = mi.rcMonitor;
    mi.dwFlags = MONITORINFOF_PRIMARY;
    return mi;
}

float DpiScaleForMonitor(HMONITOR mon) {
    UINT x = 96, y = 96;
    if (mon && SUCCEEDED(GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &x, &y))) return x / 96.f;
    return GetDpiForSystem() / 96.f;
}

// The monitor's own name ("DELL U2720Q") for a screen like \\.\DISPLAY2, from
// the display configuration; "" if Windows doesn't say (a projector, Wine).
static std::wstring MonitorName(const std::wstring& device) {
    UINT32 npaths = 0, nmodes = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &npaths, &nmodes) != ERROR_SUCCESS) return L"";
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(npaths);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(nmodes);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &npaths, paths.data(), &nmodes, modes.data(), nullptr) != ERROR_SUCCESS)
        return L"";
    for (UINT32 i = 0; i < npaths; i++) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME src = {};
        src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        src.header.size = sizeof(src);
        src.header.adapterId = paths[i].sourceInfo.adapterId;
        src.header.id = paths[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS || _wcsicmp(src.viewGdiDeviceName, device.c_str())) continue;
        DISPLAYCONFIG_TARGET_DEVICE_NAME target = {};
        target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        target.header.size = sizeof(target);
        target.header.adapterId = paths[i].targetInfo.adapterId;
        target.header.id = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&target.header) == ERROR_SUCCESS) return target.monitorFriendlyDeviceName;
    }
    return L"";
}

std::vector<DisplayInfo> Displays() {
    std::vector<std::pair<HMONITOR, MONITORINFOEXW>> mons;
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR m, HDC, LPRECT, LPARAM lp) -> BOOL {
            MONITORINFOEXW mi = {};
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(m, &mi)) ((std::vector<std::pair<HMONITOR, MONITORINFOEXW>>*)lp)->push_back({m, mi});
            return TRUE;
        },
        (LPARAM)&mons);
    std::vector<DisplayInfo> out;
    for (auto& [mon, mi] : mons) {
        DisplayInfo d;
        d.device = mi.szDevice;
        d.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
        std::wstring name = MonitorName(d.device);
        if (name.empty()) {  // "Display 2", from \\.\DISPLAY2
            size_t digits = d.device.find_last_not_of(L"0123456789");
            name = Tr(L"Display ", L"ディスプレイ ") + (digits == std::wstring::npos ? d.device : d.device.substr(digits + 1));
        }
        wchar_t size[48];
        swprintf(size, 48, L" · %ld×%ld", mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top);
        d.label = name + size;
        out.push_back(d);
    }
    std::stable_sort(out.begin(), out.end(), [](const DisplayInfo& a, const DisplayInfo& b) { return a.primary > b.primary; });
    return out;
}

HMONITOR MonitorByDevice(const std::wstring& device) {
    struct Search {
        const std::wstring* want;
        HMONITOR found;
    } search{&device, nullptr};
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR m, HDC, LPRECT, LPARAM lp) -> BOOL {
            auto* s = (Search*)lp;
            MONITORINFOEXW mi = {};
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(m, &mi) && !_wcsicmp(mi.szDevice, s->want->c_str())) {
                s->found = m;
                return FALSE;
            }
            return TRUE;
        },
        (LPARAM)&search);
    return search.found;
}

HMONITOR ForegroundMonitor() {
    HWND fg = GetForegroundWindow();
    if (fg) return MonitorFromWindow(fg, MONITOR_DEFAULTTOPRIMARY);
    POINT pt;
    GetCursorPos(&pt);
    return MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
}

std::wstring ProcessExeName(HWND hwnd) {
    if (!hwnd) return L"";
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) return L"";
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return L"";
    wchar_t buf[MAX_PATH * 2];
    DWORD size = (DWORD)std::size(buf);
    std::wstring name;
    if (QueryFullProcessImageNameW(h, 0, buf, &size)) {
        name.assign(buf, size);
        size_t slash = name.find_last_of(L"\\/");
        if (slash != std::wstring::npos) name = name.substr(slash + 1);
    }
    CloseHandle(h);
    return name;
}

std::wstring ProcessName(HWND hwnd) {
    if (!hwnd) return L"?";
    std::wstring name = ProcessExeName(hwnd);
    if (!name.empty()) return name;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    return L"pid " + std::to_wstring(pid);
}

std::wstring WindowTitle(HWND hwnd) {
    // InternalGetWindowText reads the title without sending a message, so a
    // hung app can't freeze the switcher.
    wchar_t buf[512];
    int n = InternalGetWindowText(hwnd, buf, (int)std::size(buf));
    return std::wstring(buf, n > 0 ? n : 0);
}

// ---- games ------------------------------------------------------------------------------------------
bool IsGame(const std::wstring& exe) { return !exe.empty() && core::ListHas(g_cfg.games, exe); }

bool AddGame(const std::wstring& exe) {
    if (exe.empty() || IsGame(exe)) return false;
    auto list = core::SplitList(g_cfg.games);
    list.push_back(exe);
    g_cfg.games = core::JoinList(list);
    Log(L"Marked %ls as a game", exe.c_str());
    return true;
}

bool ExclusiveFullscreenNow() {
    QUERY_USER_NOTIFICATION_STATE state;
    return SUCCEEDED(SHQueryUserNotificationState(&state)) && state == QUNS_RUNNING_D3D_FULL_SCREEN;
}

bool GameInFront() {
    // Exclusive fullscreen: anything drawn on top can knock the game out of it.
    if (ExclusiveFullscreenNow()) return true;
    HWND fg = GetForegroundWindow();
    return fg && IsGame(ProcessExeName(fg));
}
