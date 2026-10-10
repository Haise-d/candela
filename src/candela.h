// Candela for Windows: screen warmth (f.lux-style schedule, Darkroom,
// blackout), plus an Alt+Tab switcher, volume and media popup and cursors,
// all in one glitch-animated look. Shared declarations.
//
// Everything runs on the main thread (one message loop: the engine, the
// popups, the switcher and the settings window) except the keyboard hook
// (its own thread, keyboard.cpp), the media worker (media.cpp) and the
// gamma fast-watch (display.cpp).
#pragma once

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// _WIN32_WINNT / WINVER (Windows 10+) are set by build.sh.

#include <windows.h>
#include <dwmapi.h>
#include <d2d1.h>
#include <dwrite.h>
#include <dwrite_1.h>
#include <wincodec.h>
#include <shellapi.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "core/core.h"

// Windows 11 DWM attributes (older MinGW headers lack them).
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif
#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif
#ifndef DWMWA_TEXT_COLOR
#define DWMWA_TEXT_COLOR 36
#endif
#ifndef DWMWA_COLOR_DEFAULT
#define DWMWA_COLOR_DEFAULT 0xFFFFFFFF
#endif

#define CANDELA_MUTEX L"Local\\CandelaSingleInstance"
#define CANDELA_MAIN_CLASS L"CandelaMain"
#define CANDELA_NAME L"Candela"               // message boxes
#define CANDELA_VERSION L"1.1.0"

// Messages posted to the main window.
enum : UINT {
    WM_CANDELA_TRAY = WM_APP + 1,  // lParam: mouse message (WM_LBUTTONUP opens the settings)
    WM_CANDELA_OPEN_SETTINGS,      // candela.exe started again: open the settings
    WM_CANDELA_REPAIRED,           // the fast watch put the colors back
    WM_CANDELA_SWITCH,             // wParam: +1 / -1 (Alt+Tab / Alt+Shift+Tab)
    WM_CANDELA_SW_KEY,             // wParam: virtual key while the switcher is open
    WM_CANDELA_SW_COMMIT,          // Alt released
    WM_CANDELA_MEDIA,              // wParam: media / volume virtual key
    WM_CANDELA_MEDIA_INFO,         // lParam: media::Info* (media.cpp), the main thread deletes it
    WM_CANDELA_DO,                 // wParam: a HotkeyId (candela.exe --do ...)
    WM_CANDELA_QUIT,               // candela.exe --reset: quit (WM_CLOSE can't reach a Candela running as admin)
    WM_CANDELA_VOLUME,             // wParam: kVolumeChanged (by something other than Candela) or kDeviceChanged
};
enum : WPARAM { kVolumeChanged = 0, kDeviceChanged = 1 };

// Tag on input Candela injects itself, so its keyboard hook ignores it.
constexpr ULONG_PTR CANDELA_INJECTED = 0x43414E44;  // "CAND"

// ---- app.cpp: settings file, log, helpers ----------------------------------------------------
extern core::Config g_cfg;
extern HINSTANCE g_inst;
extern HWND g_main;                    // the hidden main window (engine.cpp)
std::wstring AppDir();                 // %APPDATA%\Candela
std::wstring ExePath();
std::wstring ExeDir();                 // the folder candela.exe is in (theme\ lives there)
std::wstring ConfigPath();             // %APPDATA%\Candela\config.json
bool LoadConfig(bool reload = false);  // reload: a hand edit with a mistake changes nothing
bool FirstRun();                       // no config.json existed at start-up
void SaveConfigSoon();                 // after a short pause (dragging a slider saves once)
void SaveConfigNow();
void MarkConfigDirty();
bool ConfigDirty();                    // changed in Candela, not saved yet
bool ConfigFileChanged();              // config.json was edited by hand since we read or wrote it
void Log(const wchar_t* fmt, ...);
double NowMs();                        // high-resolution clock
extern double g_fake_now;              // >= 0: frozen clock, only for --dump
core::LocalTime NowLocal();
extern core::LocalTime g_fake_local;   // >= 0: frozen wall clock, only for --dump
float DpiScaleForMonitor(HMONITOR mon);
HMONITOR ForegroundMonitor();
// The screens connected now, for settings that pick one: Windows' name for it
// ("\\.\DISPLAY2"), and a label for people ("DELL U2720Q · 2560×1440"), primary first.
struct DisplayInfo {
    std::wstring device, label;
    bool primary = false;
};
std::vector<DisplayInfo> Displays();
HMONITOR MonitorByDevice(const std::wstring& device);  // nullptr if it isn't connected
MONITORINFO MonitorInfoOf(HMONITOR mon);  // never left empty: the primary screen's size if Windows can't say
extern std::atomic<bool> g_fg_exclusive;  // the window in front is in exclusive fullscreen (read by the hook)
bool ExclusiveFullscreenNow();
bool GameInFront();                    // a game from the list, or anything in exclusive fullscreen, is in front
bool IsGame(const std::wstring& exe);  // in g_cfg.games
bool AddGame(const std::wstring& exe); // false if already there
std::wstring ProcessName(HWND hwnd);   // "chrome.exe", "pid 123" or "?" (for the log)
std::wstring ProcessExeName(HWND hwnd);  // "chrome.exe", or "" if unknown
std::wstring WindowTitle(HWND hwnd);
HWND LastUserForeground();             // the last window you used that isn't Candela's or the shell

struct Color { float r, g, b, a; };
inline Color Hex(uint32_t v, float a = 1.f) {
    return {((v >> 16) & 0xFF) / 255.f, ((v >> 8) & 0xFF) / 255.f, (v & 0xFF) / 255.f, a};
}
inline Color FromTint(core::Rgb8 t) { return {t.r / 255.f, t.g / 255.f, t.b / 255.f, 1.f}; }
COLORREF ToColorRef(const Color& c);
// The five colors everything is drawn in, from g_cfg.palette.
struct Palette { Color paper, paper_dark, ink, ink_soft, shade; };
extern Palette g_pal;
void ApplyPalette();                   // g_cfg.palette (and the custom colors) -> g_pal
// The built-in palettes, in the order the settings cycle through them.
const std::vector<std::wstring>& PaletteNames();  // "warm", "grey"

// ---- lang.cpp: English and Japanese -------------------------------------------------------------
// Tr() picks the string for the current language, so each text sits next to
// its translation where it's used.
bool Japanese();
inline std::wstring Tr(const wchar_t* en, const wchar_t* ja) { return Japanese() ? ja : en; }
namespace lang {
std::wstring Phase(core::PhaseName p);             // "Evening" / 夕刻
std::wstring Kelvin(int kelvin);                   // "2000K", "Afterglow" / 残光
std::wstring Until(const core::Status& st);        // "until night 22:00" / 夜間 22:00 まで ("" when nothing's next)
std::wstring Summary(const core::Status& st);      // the tray tooltip's line
std::wstring Keys(const std::wstring& hotkey);     // "Alt+Shift+PgDn", "none" / なし
std::wstring OnOff(bool on);                       // "On" / ON
}  // namespace lang

// ---- system.cpp ---------------------------------------------------------------------------------
namespace sys {
// Start with Windows: a scheduled task that starts Candela as admin at sign-in,
// so the switcher also works over admin windows (Windows keeps normal apps
// away from those). Windows asks for admin once when it's turned on or off.
bool StartupEnabled();            // cached; Refresh() asks Task Scheduler again
bool StartupPointsElsewhere();    // the task starts a candela.exe in another folder
bool SetStartup(bool on);
void RefreshStartup();
// A task from an older Candela that still starts Candela as admin from a folder any
// program can change. UpdateOldStartup() registers it again under today's rule
// when Candela is admin (as that task starts it), else only logs it.
bool StartupNeedsUpdate();
void UpdateOldStartup();
bool StartMenuExists();           // Candela.lnk exists and points at this candela.exe
bool SetStartMenu(bool on, std::wstring* error);
bool WarmColorsAllowed();         // GdiIcmGammaRange = 256
bool RequestWarmColors();         // asks for admin
// Windows only uses that setting after a restart; until then warm colors are
// still refused. The prompts below say so in plain words, and the settings,
// the tray tip and the tray menu keep saying it until the restart.
bool WarmColorsRestartPending();  // allowed, but Windows hasn't restarted since
bool WarmColorsWork();            // a warm color got through since start (whatever the registry says)
void NoteWarmColorWorked();       // the engine saw a warm color go through
void NoteWarmColorRefused();      // ...or refused while allowed
bool UnlockWarmColors();          // Allow (Windows asks for admin), then offers to restart. True if allowed.
bool AskRestartForWarmColors(bool just_allowed);  // "restart now?"; true if Windows is restarting
void OfferWarmColorsAtFirstStart();               // the first-start question, if warm colors are locked
// What needs admin, done in candela.exe started again as admin
// (candela.exe --admin <what> [arg]): startup-on <user>, startup-off,
// warm-colors. Returns the exit code (0: done).
int RunAdmin(const std::wstring& what, const std::wstring& arg);
bool IsElevated();
void EnableDarkMenus();
void OpenFolder(const std::wstring& path);
void OpenFile(const std::wstring& path);
}  // namespace sys

// theme.cpp: Candela's cursors plus Windows' dark mode, with backup and undo.
namespace theme {
bool Apply();                     // false: the theme folder isn't next to candela.exe
bool Restore();
bool Applied();
void Follow();                    // on, and the palette changed between grey and warm: the matching set
}  // namespace theme

// ---- display.cpp (gamma, colorfx) / windows.cpp (blackout, borderless) ------------------------
namespace gamma {
void RefreshDisplays();
// Windows refuses ramps far from normal colors until warm colors are
// unlocked (and Windows restarted). A display that refuses gets the closest
// ramp it does take ("limited"), and the whole one is tried again now and then.
struct ApplyResult {
    int full = 0;                       // displays that took the whole ramp
    std::vector<std::wstring> limited;  // ...only part of the way: the closest one they take is on
    std::vector<std::wstring> refused;  // ...nothing, not even normal colors
    double t = 1;                       // the least of the way a limited display got (0: normal colors)
};
ApplyResult Apply(const core::Ramp& ramp);
struct State { int full = 0, limited = 0, refused = 0; double t = 1; };  // as above, right now
State GetState();
std::vector<std::wstring> CheckAndRepair(double force_every_s = 0);
bool AnyRefusing();  // a display refused the last ramp and hasn't taken it since (it's retried, slower each time)
void Close();
// A few seconds of very frequent checks on a thread of their own, with a
// precise timer, for the moments resets usually happen (leaving a
// fullscreen game). Repairs are reported to `notify` as WM_CANDELA_REPAIRED.
void FastWatch(double seconds, int interval_ms, HWND notify);
std::vector<std::wstring> TakeFastRepairs();  // displays the fast watch repaired since last asked
void StopFastWatch();
}  // namespace gamma

namespace colorfx {
bool Init();
bool Available();
bool Pending();  // Windows refused the last filter; it's retried until it takes
bool Set(const core::Matrix& m, bool identity);
bool CheckAndRepair();
void Close();
}  // namespace colorfx

namespace blackout {
bool Init();
bool Active();
int Toggle(HMONITOR keep, bool always_on_top);  // screens blacked out (0 = off or only one)
void Clear();
bool Owns(HWND h);
void OnForeground(HWND fg);                     // a window was switched to: it goes above the black
}  // namespace blackout

// blackfx.cpp: the glitch a screen plays as it's blacked out (from the
// Animation setting).
namespace blackfx {
bool Wanted();  // the animation isn't Off
// `blacks`: that screen's black windows, made layered and see-through; the
// animation brings them in and leaves them plain black.
void Start(HMONITOR mon, const RECT& screen, const std::vector<HWND>& blacks);
void Finish(HMONITOR mon);  // a window came to that screen: plain black now
void StopAll();             // the blackout is going away
void DebugDump(const std::wstring& dir);
}  // namespace blackfx

namespace borderless {
// Returns true if the window is now borderless, false if put back; throws
// nothing: on failure `error` is set (short, fits the popup).
bool Toggle(HWND hwnd, int overhang, bool* on, std::wstring* error);
void RestoreAll();
}  // namespace borderless

// ---- icons.cpp ------------------------------------------------------------------------------------
// The tray icon, drawn in code in the current warmth's tint.
HICON MakeTrayIcon(int size, int kelvin, bool disabled);

// ---- engine.cpp -------------------------------------------------------------------------------------
enum HotkeyId { HK_WARMER = 1, HK_COOLER, HK_TOGGLE, HK_DARKROOM, HK_DIMMER, HK_BRIGHTER, HK_BORDERLESS, HK_BLACKOUT };
struct HotkeyField { HotkeyId id; std::wstring core::Config::*field; const wchar_t* label; bool repeats; };
const std::vector<HotkeyField>& HotkeyFields();
std::wstring HotkeyName(HotkeyId id);  // the field's label, in the current language

namespace engine {
bool Init();
void Shutdown();
HWND Window();
const core::Status& GetStatus();
std::vector<std::wstring> Problems();  // for people, in order of importance
bool RegistryProblem();                 // warm colors locked, or waiting for a restart
bool WarmRestartPending();              // ...waiting for a restart (cached)
// Windows refused the colors asked for, so the closest it allows is on:
// roughly what that is. False when what's on screen is what was asked for.
bool Limited(int* kelvin, double* brightness);
std::wstring LimitNote();               // "~3300K" / "~3300K, 50%" / "" : what Windows holds the screen at
void SkipWarmNotice();                  // before Init on a first start: its own question says it, not a notification too
bool ShuttingDown();                    // quitting: nothing may put colors back any more
int ResetsToday();
std::wstring LastReset();
void Wake();                            // settings changed: apply now
void Hotkey(HotkeyId id, bool popup = true);
void SuspendHotkeys(bool on);           // the settings window is recording a new hotkey
void PreviewPopup(bool force = false);  // force: even with the warmth popup turned off (the Preview button)
bool BlackoutActive();
void ToggleBlackoutAtCursor();
void BackToSchedule();
void FullBrightness();
// Pause: normal colors and the keyboard let go, until resumed.
bool Paused();
void SetPaused(bool on);
std::wstring SteppingAsideFor();        // the anti-cheat game Candela let go of the keyboard for, or ""
std::wstring KeyboardStatus();          // "ONLINE", "PAUSED", "OFF WHILE GAME.EXE RUNS"
void KeyboardSettingsChanged();         // switcher / media / anti-cheat settings changed
void StyleChanged();                    // colors, font or language changed: repaint what caches them
void Quit();
}  // namespace engine

// ---- popup.cpp: the warmth popup (what a hotkey did) ------------------------------------------------
namespace popup {
struct Content {
    std::wstring title, value, note;
    float bar = -1;           // 0..1, or < 0 for none
    bool tinted = false;      // the header square in `tint` (a temperature)
    Color tint{1, 1, 1, 1};
};
bool Init();
void Show(const Content& c);  // the volume popup shares its corner, so it goes away
void Hide();
bool IsOwnWindow(HWND h);
void DebugTick();
bool DebugSave(const std::wstring& png);
void Shutdown();
}  // namespace popup

// ---- keyboard.cpp: the low-level keyboard hook (Alt+Tab, volume and media keys) ------------------
namespace keyboard {
void Start();
void Stop();
void SetEnabled(bool on);         // off while paused or an anti-cheat game runs
void Reinstall();                 // after sleep or unlock: Windows may have dropped the hook silently
bool Enabled();
extern std::atomic<bool> switcher_on, media_on, switcher_active;
extern std::atomic<bool> exclusive_on;  // Candela handles Alt+Tab even in exclusive fullscreen
}  // namespace keyboard

// ---- switcher.cpp / media.cpp --------------------------------------------------------------------
namespace switcher {
bool Init();
void StyleChanged();
void OnSwitch(int dir);
void DebugTick();
bool DebugSave(const std::wstring& png);
void OnKey(UINT vk);
void OnCommit();
void Cancel();
bool IsOwnWindow(HWND hwnd);
void Shutdown();
}  // namespace switcher

namespace media {
bool Init();
void OnKey(UINT vk);
void OnInfo(LPARAM info);
void DebugShow(int kind);
void Preview();                   // the volume popup once, as it is (the settings' Preview)
void OnVolume(WPARAM what);       // WM_CANDELA_VOLUME: the volume changed elsewhere, or the output device did
void SettingsChanged();           // popup settings, pause or anti-cheat changed: start or stop looking for new songs
std::vector<std::wstring> AlwaysSongApps();  // the apps whose new songs always show (Spotify), besides g_cfg.song_apps
void Hide();                      // the warmth popup took the corner
void DebugTick();
bool DebugSave(const std::wstring& png);
bool IsOwnWindow(HWND hwnd);
void Shutdown();
}  // namespace media

// ---- the settings window ---------------------------------------------------------------------------------------
namespace settings {
void Open();
void OpenOn(const std::wstring& key);  // a section ("system") or a setting ("lat") to start on
bool IsOwnWindow(HWND h);
void Refresh();               // the status changed: redraw if open
void DebugDump(const std::wstring& dir);
void Shutdown();
}  // namespace settings
