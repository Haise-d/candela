// The core logic: sun, colors, schedule, hotkey text, settings file.
// Nothing in src/core touches Windows, so it builds and is tested on any
// machine (tests/). The one thing it needs from the system is the local
// clock's offset at a given moment: LocalFromUtc(), in time_win.cpp on
// Windows and in the tests on Linux.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace core {

// ---- time -----------------------------------------------------------------------------------
// Local wall-clock time as seconds since 1970-01-01 00:00 *on the local clock*
// (no zone attached), like Python's naive datetime. Fractions are kept.
using LocalTime = double;

struct Date {
    int y, m, d;
    bool operator==(const Date& o) const { return y == o.y && m == o.m && d == o.d; }
};
struct HM {
    int h, m;
    bool operator==(const HM& o) const { return h == o.h && m == o.m; }
    bool operator<(const HM& o) const { return h * 60 + m < o.h * 60 + o.m; }
};

int64_t DaysFromCivil(int y, int m, int d);
Date CivilFromDays(int64_t days);
Date DateOf(LocalTime t);
Date AddDays(Date d, int n);
LocalTime Combine(Date d, HM t);
LocalTime Midnight(LocalTime t);
HM TimeOf(LocalTime t);                     // hour and minute
std::wstring FormatHM(LocalTime t);         // "22:00"
int Weekday(Date d);                        // 0 = Monday
std::optional<HM> ParseHM(const std::wstring& text);

// Provided by the platform: the local wall-clock time at a UTC instant
// (seconds since the Unix epoch), with that moment's daylight saving.
LocalTime LocalFromUtc(double utc_seconds);

// ---- sun ------------------------------------------------------------------------------------
// Sunrise and sunset of local date `d`, as local wall-clock times. false in
// polar day or night.
bool SunTimesUtc(Date d, double lat, double lon, double* rise_utc, double* set_utc);
bool LocalSunTimes(Date d, double lat, double lon, LocalTime* rise, LocalTime* set);

// ---- color ------------------------------------------------------------------------------------
constexpr int kDarkroom = 800;  // special: pure red (by brightness), not a real color
constexpr int kMinK = 900;
constexpr int kMaxK = 6500;
constexpr int kNeutralK = 6500;

struct Rgb { double r, g, b; };
Rgb Multipliers(double kelvin);
struct Ramp { uint16_t ch[3][256]; };
Ramp BuildRamp(int kelvin, double darkroom_brightness = 1.0, bool invert = false, double brightness = 1.0);
Ramp IdentityRamp();
std::wstring Describe(int kelvin);          // "2000K" or "Afterglow"

// ---- when Windows won't take a ramp -----------------------------------------------------------
// Out of the box Windows refuses ramps far from normal colors (warmer than
// about 3400K, or dimmed a lot). It doesn't clamp them: the refused ramp just
// isn't used. So Candela puts on the closest one it does take, somewhere on
// the way from normal colors to what was asked for.
Ramp Blend(const Ramp& from, const Ramp& to, double t);  // t = 0: from, 1: to
// The largest t in [0, 1) whose Blend(from, to, t) `accepts` takes, to 1/256
// (0 if none does). `to` itself is assumed refused already.
//
// `guess` (0..1, or < 0 for none) is where to look first, from an earlier
// fit: when it's right, two tries settle it instead of eight. Each try puts a
// ramp on screen if it's taken, so the caller puts the answer on screen
// itself afterwards.
double ClosestAccepted(const Ramp& from, const Ramp& to, const std::function<bool(const Ramp&)>& accepts, double guess = -1);
// How far `to` strays from `from` at most (16-bit levels): what Windows' limit
// is about. A ramp Windows took part of the way to one target tells, through
// this, roughly how far it goes toward the next one.
double MaxDeviation(const Ramp& from, const Ramp& to);
// Whether stopping part of the way toward `to` still shows a usable picture.
// Not for a ramp that turns a channel upside down (Afterglow's invert): half
// way there every level of that channel is the same, a flat screen.
bool Approachable(const Ramp& to);
// Roughly what a ramp shows: the color temperature its white is nearest to
// (900..6500, in 100s) and its brightness (the top of its strongest channel).
int ApproxKelvin(const Ramp& r);
double ApproxBrightness(const Ramp& r);
// The color that stands for a temperature in icons, bars and the sky strip.
struct Rgb8 { uint8_t r, g, b; };
Rgb8 Tint(int kelvin, bool disabled);

// ---- Darkroom's color filter ----------------------------------------------------------------
// Matrices for Windows' full-screen color effect: m[i*5+j] = how input
// channel i (R, G, B, A, offset) feeds output channel j.
struct Matrix { float m[25]; };
Matrix IdentityMatrix();
Matrix BrightnessToRed();
struct Effect {
    std::wstring key;  // "identity", "darkroom" or "blend0.25"
    double strength;   // 0 = identity .. 1 = darkroom
    Matrix matrix;
};
Effect EffectFor(int kelvin, bool by_brightness);
// For fades: part of the way from one filter to another (t = 0: from, 1: to).
Matrix BlendMatrix(const Matrix& from, const Matrix& to, double t);
// How far along a fade looks at a point in its time (0..1 -> 0..1): eased
// at both ends, so it neither starts nor stops with a jolt.
double FadeCurve(double progress);

// ---- settings ---------------------------------------------------------------------------------
// Where a new install starts (Greenwich) until you set your own town; on the
// first run Candela opens its settings on the Location page for that.
constexpr double kPlaceholderLat = 51.4779;
constexpr double kPlaceholderLon = 0.0;

struct Config {
    double latitude = kPlaceholderLat;
    double longitude = kPlaceholderLon;
    int day_kelvin = 4800;
    int evening_kelvin = 2000;
    int sunset_offset_min = 0;  // negative = switch before sunset
    std::wstring night_start = L"22:00";
    bool night_darkroom = true;
    int night_kelvin = 1200;    // only when night_darkroom is false
    double darkroom_brightness = 1.0;
    bool darkroom_invert = false;
    bool darkroom_by_brightness = true;
    std::wstring morning_mode = L"sunrise";  // "sunrise" or "fixed"
    std::wstring morning_time = L"08:00";
    int warmth_step_k = 100;
    // Fades: color changes come on gradually rather than at once. Scheduled
    // changes (sunset, night, morning) take fade_seconds; the ones you make
    // (hotkeys, the tray menu, the settings) take fade_hotkey_ms (0 = instant).
    bool fade = false;
    int fade_seconds = 20;
    int fade_hotkey_ms = 400;
    bool show_popup = true;
    std::wstring hotkey_warmer = L"Alt+Shift+PageDown";
    std::wstring hotkey_cooler = L"Alt+Shift+PageUp";
    std::wstring hotkey_dimmer = L"Alt+PageDown";
    std::wstring hotkey_brighter = L"Alt+PageUp";
    std::wstring hotkey_off = L"Alt+Delete";
    std::wstring hotkey_darkroom = L"Alt+Shift+Delete";
    std::wstring hotkey_borderless = L"Ctrl+Alt+B";
    std::wstring hotkey_blackout = L"Ctrl+Alt+N";
    bool blackout_always_on_top = false;
    int borderless_overhang_px = 1;
    double dim_step = 0.1;
    double dim_min = 0.1;
    int watchdog_ms = 100;
    int burst_ms = 15;
    int force_reapply_ms = 0;
    int burst_fast_ms = 4;   // the fast check after window switches (0 = off, main-thread timer only)

    // ---- popups (the warmth popup and the volume / media popup share these) ----
    std::wstring popup_position = L"bottom-right";        // the warmth popup's corner
    std::wstring media_popup_position = L"same";          // the volume / media popup's: "same" follows popup_position
    std::wstring animation = L"full";  // off / subtle / full: popups, switcher, settings, blackout
    // No popup while one of these is in front, or any exclusive-fullscreen app.
    std::wstring games = L"";  // e.g. "game.exe, other.exe"; added from the tray menu or the settings
    bool popup_in_games = false;

    // ---- Alt+Tab switcher ----
    bool switcher = true;
    bool switcher_dim = true;              // darken the screen behind it
    int switcher_delay_ms = 80;            // a quicker Alt+Tab switches without showing anything
    bool switcher_exclusive_fullscreen = false;  // false: Windows handles Alt+Tab over exclusive-fullscreen games
    bool switcher_typing = true;           // window names type in when it opens
    bool switcher_duotone_icons = false;   // app icons recolored to the palette
    // Which screen it opens on: "active" (the window in front's), "primary",
    // or one screen by its Windows name ("\\.\DISPLAY2"; the primary if it's unplugged).
    std::wstring switcher_display = L"active";

    // ---- volume and media keys ----
    bool media_popup = true;
    int volume_step = 2;                   // percent per press (Windows: 2)
    bool media_art = true;                 // the album cover on the song card
    bool media_new_song = true;            // a new song starting by itself shows the song card
    std::wstring song_apps = L"";          // ...in these apps (.exe names, comma separated), besides Spotify

    // ---- anti-cheat: let go of the keyboard while one of these runs ----
    std::wstring anticheat_games = L"";    // comma separated .exe names
    bool anticheat_step_aside = true;

    // ---- looks ----
    std::wstring language = L"en";         // "en" or "ja"
    std::wstring font = L"Segoe UI";
    std::wstring font_ja = L"";            // "" = Rodin if installed, else Yu Gothic UI; "rodin" or a family name
    std::wstring palette = L"warm";        // warm / grey / custom (the five colors below)
    std::wstring color_paper = L"1F1E1B", color_paper_dark = L"2C2A25", color_ink = L"D6D0B8",
                 color_ink_soft = L"8F8A78", color_shade = L"000000";
};

// The popups' corners: top-left, top-center, top-right, bottom-left,
// bottom-center, bottom-right.
bool IsPopupCorner(const std::wstring& s);
// Where the volume / media popup goes: its own corner, or the warmth popup's.
std::wstring MediaCorner(const Config& cfg);

// config.json: a flat JSON object; keys this version doesn't know are kept
// when saving.
bool ParseConfig(const std::string& json, Config* cfg, std::map<std::string, std::string>* unknown);
std::string SerializeConfig(const Config& cfg, const std::map<std::string, std::string>& unknown);

// Comma separated .exe lists ("a.exe, b.exe"): split (trimmed, empty items
// dropped) and joined back.
std::vector<std::wstring> SplitList(const std::wstring& s);
std::wstring JoinList(const std::vector<std::wstring>& v);
bool ListHas(const std::wstring& list, const std::wstring& item);  // case-insensitive (ASCII)

std::string ToUtf8(const std::wstring& s);
std::wstring FromUtf8(const std::string& s);

// ---- schedule -------------------------------------------------------------------------------------
enum PhaseName { kDay = 0, kEvening = 1, kNight = 2 };
const wchar_t* Label(PhaseName p);  // "Day", "Evening", "Night"

struct Phase {
    PhaseName name;
    LocalTime start;
    int64_t Minute() const;  // start, to the minute (identifies the phase)
    bool Same(const Phase& o) const { return name == o.name && Minute() == o.Minute(); }
};

std::vector<Phase> Timeline(LocalTime now, const Config& cfg);  // yesterday to tomorrow, in order
void CurrentAndNext(LocalTime now, const Config& cfg, Phase* current, std::optional<Phase>* next);
int ScheduledKelvin(PhaseName p, const Config& cfg);

struct Status {
    int kelvin = kNeutralK;   // on screen (6500 when off)
    Phase phase{kDay, 0};
    std::optional<Phase> next;
    int scheduled = kNeutralK;
    bool overridden = false;
    bool disabled = false;
    double brightness = 1.0;
    bool Darkroom() const { return !disabled && kelvin <= kDarkroom; }
    std::wstring Summary() const;
};

// The only state: short-lived adjustments. Warmth, Darkroom on demand and
// "off" last until the next scheduled change; dimming until the next morning.
class Controller {
public:
    Status Get(LocalTime now, const Config& cfg);
    Status Nudge(LocalTime now, const Config& cfg, bool warmer);
    Status Dim(LocalTime now, const Config& cfg, bool darker);
    Status ToggleDarkroom(LocalTime now, const Config& cfg);
    Status ToggleDisabled(LocalTime now, const Config& cfg);
    void FullBrightness() { dim_.reset(); }
    void Reset();
    void ForgetTimeline() { cache_.valid = false; }  // the clock or time zone changed

private:
    // The day's sunrise/sunset events, worked out once per day and settings
    // rather than on every tick (each one costs several time-zone calls).
    struct TimelineCache {
        bool valid = false;
        Date date{0, 0, 0};
        double latitude = 0, longitude = 0, sunset_offset_min = 0;
        std::wstring morning_mode, morning_time, night_start;
        std::vector<Phase> events;
    };
    const std::vector<Phase>& CachedTimeline(LocalTime now, const Config& cfg);
    LocalTime LastMorning(LocalTime now, const Config& cfg);
    TimelineCache cache_;
    std::optional<std::pair<Phase, int>> override_;
    std::optional<Phase> disabled_for_;
    std::optional<std::pair<LocalTime, double>> dim_;  // (morning it was set after, level)
};

// ---- hotkeys --------------------------------------------------------------------------------------
constexpr unsigned kModAlt = 0x1, kModCtrl = 0x2, kModShift = 0x4, kModWin = 0x8;
struct Hotkey { unsigned mods; unsigned vk; };
// "Alt+Shift+PageDown" -> mods and key. Empty text: ok, no hotkey (nullopt).
// Bad text: returns false with a message for people.
bool ParseHotkey(const std::wstring& text, std::optional<Hotkey>* out, std::wstring* error);
std::wstring FormatHotkey(unsigned mods, unsigned vk);
std::wstring DisplayHotkey(const std::wstring& text);  // PageDown -> PgDn
bool IsModifierVk(unsigned vk);

}  // namespace core
