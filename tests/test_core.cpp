// Tests for src/core, run on Linux with Madrid's time zone (so daylight
// saving is exercised anywhere), plus a dump mode for comparing builds.
//
//   g++ -std=c++17 -Isrc/core tests/test_core.cpp src/core/*.cpp -o test_core && TZ=Europe/Madrid ./test_core
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <set>

#include "core.h"

using namespace core;

// The platform part: local wall-clock time at a UTC instant.
LocalTime core::LocalFromUtc(double utc) {
    double whole = std::floor(utc);
    time_t tt = (time_t)whole;
    struct tm lt;
    localtime_r(&tt, &lt);
    return Combine({lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday}, {lt.tm_hour, lt.tm_min}) + lt.tm_sec + (utc - whole);
}

static int g_failed = 0, g_checks = 0;
#define CHECK(cond)                                                             \
    do {                                                                        \
        g_checks++;                                                             \
        if (!(cond)) {                                                          \
            g_failed++;                                                         \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);              \
        }                                                                       \
    } while (0)

static LocalTime At(const char* iso) {  // "2026-09-26T13:00"
    int y, mo, d, h, mi;
    sscanf(iso, "%d-%d-%dT%d:%d", &y, &mo, &d, &h, &mi);
    return Combine({y, mo, d}, {h, mi});
}
static int Minutes(LocalTime t) { HM hm = TimeOf(t); return hm.h * 60 + hm.m; }

static void SolarTests() {
    // Reference values for Madrid from the `astral` library (as in the Python tests).
    struct { Date d; int rise, set; } cases[] = {
        {{2026, 6, 21}, 6 * 60 + 45, 21 * 60 + 48},
        {{2026, 12, 21}, 8 * 60 + 34, 17 * 60 + 51},
        {{2026, 9, 26}, 8 * 60 + 6, 20 * 60 + 5},
        {{2026, 10, 24}, 8 * 60 + 35, 19 * 60 + 21},
    };
    for (auto& c : cases) {
        LocalTime r, s;
        CHECK(LocalSunTimes(c.d, 40.4168, -3.7038, &r, &s));
        CHECK(std::abs(Minutes(r) - c.rise) <= 3);
        CHECK(std::abs(Minutes(s) - c.set) <= 3);
    }
    LocalTime r, before, after;
    LocalSunTimes({2026, 10, 24}, 40.4168, -3.7038, &r, &before);
    LocalSunTimes({2026, 10, 25}, 40.4168, -3.7038, &r, &after);
    CHECK(std::abs(Minutes(before) - Minutes(after) - 61) <= 3);  // clocks go back
}

static void ColorTests() {
    Ramp n = BuildRamp(6500);
    CHECK(!memcmp(n.ch[0], n.ch[1], sizeof(n.ch[0])));
    CHECK(n.ch[0][255] == 65535);
    for (int k : {1200, 2000, 3400, 4800}) {
        Rgb m = Multipliers(k);
        CHECK(m.r == 1.0);
        CHECK(m.g > m.b);
    }
    CHECK(Multipliers(2000).g < Multipliers(4800).g);
    Ramp d = BuildRamp(kDarkroom);
    CHECK(d.ch[0][0] == 0 && d.ch[0][255] == 65535);
    bool zero = true;
    for (int i = 0; i < 256; i++) zero = zero && d.ch[1][i] == 0 && d.ch[2][i] == 0;
    CHECK(zero);
    Ramp inv = BuildRamp(kDarkroom, 1.0, true);
    CHECK(inv.ch[0][0] == 65535 && inv.ch[0][255] == 0);
    Ramp full = BuildRamp(2000), half = BuildRamp(2000, 1.0, false, 0.5);
    for (int c = 0; c < 3; c++) CHECK(std::abs(half.ch[c][255] - full.ch[c][255] * 0.5) <= 1);
    CHECK(BuildRamp(kDarkroom, 1.0, false, 0.5).ch[0][255] == 32768);
    // Fading below 1200K.
    double g1 = Multipliers(1200).g, g2 = Multipliers(1100).g, g3 = Multipliers(1000).g, g4 = Multipliers(900).g;
    CHECK(g1 > g2 && g2 > g3 && g3 > g4 && g4 > 0);
    std::wstring keys;
    for (int k : {6500, 1200, 1100, 1000, 900, 800}) keys += EffectFor(k, true).key + L" ";
    CHECK(keys == L"identity identity blend0.25 blend0.5 blend0.75 darkroom ");
    CHECK(EffectFor(900, false).key == L"identity");
    // The Darkroom filter: white stays bright red, green text stays visible.
    auto apply = [](const Matrix& m, double r, double g, double b, int j) {
        double v[5] = {r, g, b, 1, 1}, s = 0;
        for (int i = 0; i < 5; i++) s += v[i] * m.m[i * 5 + j];
        return std::min(1.0, std::max(0.0, s));
    };
    Matrix red = BrightnessToRed();
    CHECK(std::abs(apply(red, 1, 1, 1, 0) - 1.0) < 1e-5);
    CHECK(apply(red, 0.3, 0.85, 0.4, 0) > 0.6);
    CHECK(apply(red, 0.3, 0.85, 0.4, 1) == 0 && apply(red, 0.3, 0.85, 0.4, 2) == 0);
    CHECK(Describe(800) == L"Afterglow" && Describe(2000) == L"2000K");

    // Fades: the filter part of the way, and the curve.
    Matrix id = IdentityMatrix();
    CHECK(!memcmp(BlendMatrix(id, red, 0).m, id.m, sizeof(id.m)));
    CHECK(!memcmp(BlendMatrix(id, red, 1).m, red.m, sizeof(red.m)));
    CHECK(!memcmp(BlendMatrix(id, red, 7).m, red.m, sizeof(red.m)));  // clamped
    Matrix midway = BlendMatrix(id, red, 0.5);
    CHECK(std::abs(midway.m[0] - (1 + 0.299f) / 2) < 1e-6 && std::abs(midway.m[6] - 0.5) < 1e-6);
    // Half way to Afterglow, green text still shows (as green and as red).
    CHECK(apply(midway, 0.3, 0.85, 0.4, 1) > 0.4 && apply(midway, 0.3, 0.85, 0.4, 0) > 0.4);
    CHECK(FadeCurve(0) == 0 && FadeCurve(1) == 1 && FadeCurve(0.5) == 0.5);
    // Inverted Afterglow doesn't fade (half way is a flat screen); the rest does.
    CHECK(!Approachable(BuildRamp(kDarkroom, 1.0, true)) && Approachable(BuildRamp(kDarkroom)) &&
          Approachable(BuildRamp(2000, 1.0, false, 0.3)) && Approachable(IdentityRamp()));
    CHECK(FadeCurve(-1) == 0 && FadeCurve(2) == 1);
    CHECK(FadeCurve(0.1) < 0.1 && FadeCurve(0.9) > 0.9);  // eased at both ends
    double prev = 0;
    bool rising = true;
    for (int i = 1; i <= 100; i++) {
        double v = FadeCurve(i / 100.0);
        rising = rising && v >= prev;
        prev = v;
    }
    CHECK(rising);
}

static int K(const Config& cfg, const char* at, Controller* ctl = nullptr) {
    Controller local;
    return (ctl ? ctl : &local)->Get(At(at), cfg).kelvin;
}

// The expected times below are Madrid's; the built-in default location is a
// placeholder, so the tests set it themselves.
static Config MadridConfig() {
    Config c;
    c.latitude = 40.4168;
    c.longitude = -3.7038;
    return c;
}

static void ScheduleTests() {
    Config cfg = MadridConfig();
    CHECK(K(cfg, "2026-09-26T13:00") == 4800);
    CHECK(K(cfg, "2026-09-26T20:30") == 2000);
    CHECK(K(cfg, "2026-09-26T22:00") == kDarkroom);
    CHECK(K(cfg, "2026-09-27T03:00") == kDarkroom);
    CHECK(K(cfg, "2026-09-27T07:30") == kDarkroom);
    CHECK(K(cfg, "2026-09-27T09:00") == 4800);
    {
        Config c = cfg;
        c.longitude = -8.72;
        c.night_start = L"21:45";
        CHECK(K(c, "2026-06-21T21:50") == kDarkroom);
    }
    {
        Config c = cfg;
        c.night_start = L"00:30";
        CHECK(K(c, "2026-09-26T23:30") == 2000);
        CHECK(K(c, "2026-09-27T00:10") == 2000);
        CHECK(K(c, "2026-09-27T00:40") == kDarkroom);
    }
    {
        Config c = cfg;
        c.morning_mode = L"fixed";
        c.morning_time = L"07:00";
        CHECK(K(c, "2026-12-21T07:30") == 4800);
    }
    CHECK(K(cfg, "2026-10-25T02:30") == kDarkroom);
    CHECK(K(cfg, "2026-10-25T19:30") == 2000);
    {  // a nudge expires at the next phase
        Controller ctl;
        Status st = ctl.Nudge(At("2026-09-26T20:30"), cfg, true);
        CHECK(st.kelvin == 1900 && st.overridden);
        CHECK(K(cfg, "2026-09-26T21:00", &ctl) == 1900);
        CHECK(K(cfg, "2026-09-26T22:00", &ctl) == kDarkroom);
        CHECK(K(cfg, "2026-09-27T20:30", &ctl) == 2000);
    }
    {  // the ladder around Darkroom
        Controller ctl;
        LocalTime now = At("2026-09-27T01:00");
        std::vector<int> up, down;
        for (int i = 0; i < 5; i++) up.push_back(ctl.Nudge(now, cfg, false).kelvin);
        for (int i = 0; i < 5; i++) down.push_back(ctl.Nudge(now, cfg, true).kelvin);
        CHECK((up == std::vector<int>{900, 1000, 1100, 1200, 1300}));
        CHECK((down == std::vector<int>{1200, 1100, 1000, 900, kDarkroom}));
        CHECK(!ctl.Get(now, cfg).overridden);
    }
    {
        Controller ctl;
        Config c = cfg;
        c.warmth_step_k = 1000;
        CHECK(ctl.Nudge(At("2026-09-27T15:00"), c, true).kelvin == 3800);
    }
    {  // off expires
        Controller ctl;
        Status st = ctl.ToggleDisabled(At("2026-09-26T15:00"), cfg);
        CHECK(st.disabled && st.kelvin == 6500);
        CHECK(K(cfg, "2026-09-26T20:30", &ctl) == 2000);
    }
    {  // Darkroom on demand
        Controller ctl;
        LocalTime day = At("2026-09-27T15:00");
        Status st = ctl.ToggleDarkroom(day, cfg);
        CHECK(st.kelvin == kDarkroom && st.Darkroom());
        CHECK(ctl.ToggleDarkroom(day, cfg).kelvin == 4800);
        ctl.ToggleDarkroom(day, cfg);
        CHECK(K(cfg, "2026-09-27T20:30", &ctl) == 2000);
    }
    {
        Controller ctl;
        LocalTime night = At("2026-09-27T23:00");
        CHECK(ctl.ToggleDarkroom(night, cfg).kelvin == 1200);
        CHECK(ctl.ToggleDarkroom(night, cfg).kelvin == kDarkroom);
    }
    {
        Controller ctl;
        LocalTime day = At("2026-09-27T15:00");
        ctl.ToggleDisabled(day, cfg);
        Status st = ctl.ToggleDarkroom(day, cfg);
        CHECK(!st.disabled && st.kelvin == kDarkroom);
    }
    {  // dimming
        Controller ctl;
        LocalTime now = At("2026-09-27T23:00");
        CHECK(std::abs(ctl.Dim(now, cfg, true).brightness - 0.9) < 1e-9);
        Status st;
        for (int i = 0; i < 20; i++) st = ctl.Dim(now, cfg, true);
        CHECK(std::abs(st.brightness - 0.1) < 1e-9);
        for (int i = 0; i < 20; i++) st = ctl.Dim(now, cfg, false);
        CHECK(st.brightness == 1.0);
    }
    {
        Controller ctl;
        ctl.Dim(At("2026-09-27T20:30"), cfg, true);
        CHECK(std::abs(ctl.Get(At("2026-09-27T23:00"), cfg).brightness - 0.9) < 1e-9);
        CHECK(std::abs(ctl.Get(At("2026-09-28T03:00"), cfg).brightness - 0.9) < 1e-9);
        CHECK(ctl.Get(At("2026-09-28T09:00"), cfg).brightness == 1.0);
    }
    {
        Controller ctl;
        LocalTime now = At("2026-09-27T15:00");
        ctl.Dim(now, cfg, true);
        CHECK(ctl.ToggleDisabled(now, cfg).brightness == 1.0);
        Status st = ctl.Dim(now, cfg, true);
        CHECK(!st.disabled && std::abs(st.brightness - 0.8) < 1e-9);
    }
    {
        Controller ctl;
        Config c = cfg;
        LocalTime now = At("2026-09-26T20:30");
        CHECK(ctl.Get(now, c).kelvin == 2000);
        c.evening_kelvin = 2400;
        CHECK(ctl.Get(now, c).kelvin == 2400);
    }
}

static void HotkeyTests() {
    Config cfg;
    std::optional<Hotkey> hk;
    std::wstring err;
    for (auto* t : {&cfg.hotkey_warmer, &cfg.hotkey_cooler, &cfg.hotkey_dimmer, &cfg.hotkey_brighter, &cfg.hotkey_off,
                    &cfg.hotkey_darkroom}) {
        CHECK(ParseHotkey(*t, &hk, &err) && hk);
    }
    CHECK(ParseHotkey(cfg.hotkey_off, &hk, &err) && hk->mods == kModAlt && hk->vk == 0x2E);
    CHECK(ParseHotkey(L"ctrl+alt+pgdn", &hk, &err) && FormatHotkey(hk->mods, hk->vk) == L"Ctrl+Alt+PageDown");
    CHECK(ParseHotkey(L"Alt+F9", &hk, &err) && hk->mods == kModAlt && hk->vk == 0x78);
    CHECK(ParseHotkey(L"", &hk, &err) && !hk);
    for (auto* bad : {L"Shift+Delete", L"PageDown", L"Alt+Shift", L"Alt+Banana", L"Alt+A+B"}) CHECK(!ParseHotkey(bad, &hk, &err));
    CHECK(DisplayHotkey(L"Alt+Shift+PageDown") == L"Alt+Shift+PgDn");
    CHECK(DisplayHotkey(L"") == L"none");
}

static void ConfigTests() {
    Config c;
    std::map<std::string, std::string> unknown;
    std::string json = R"({"latitude": 41.38, "day_kelvin": 5000.9, "night_darkroom": false, "night_start": "23:15",
        "hotkey_off": "Alt+é", "future_thing": {"a": [1, 2]}, "watchdog_ms": "oops"})";
    CHECK(ParseConfig(json, &c, &unknown));
    CHECK(c.latitude == 41.38 && c.day_kelvin == 5000 && !c.night_darkroom && c.night_start == L"23:15");
    CHECK(c.hotkey_off == L"Alt+é");
    CHECK(c.watchdog_ms == 100);  // wrong type: ignored
    CHECK(unknown.count("future_thing") && unknown["future_thing"] == R"({"a": [1, 2]})");
    std::string out = SerializeConfig(c, unknown);
    Config back;
    std::map<std::string, std::string> unknown2;
    CHECK(ParseConfig(out, &back, &unknown2));
    CHECK(back.latitude == 41.38 && back.hotkey_off == L"Alt+é" && unknown2 == unknown);
    CHECK(out.find("\"darkroom_brightness\": 1.0") != std::string::npos);
    CHECK(out.find("\"dim_step\": 0.1") != std::string::npos);
    CHECK(out.find("\\u00e9") != std::string::npos);
    CHECK(ParseHM(L"7:05") && *ParseHM(L"7:05") == (HM{7, 5}));
    CHECK(!ParseHM(L"25:00") && !ParseHM(L"ab") && !ParseHM(L"12"));
}

// Many moments across a year, several settings, printed for comparing two
// builds line for line (test_core --dump).
static void Dump() {
    std::vector<Config> cfgs(4, MadridConfig());
    cfgs[1].night_start = L"00:30";
    cfgs[1].sunset_offset_min = -30;
    cfgs[2].morning_mode = L"fixed";
    cfgs[2].morning_time = L"06:45";
    cfgs[2].night_darkroom = false;
    cfgs[2].night_kelvin = 1500;
    cfgs[3].latitude = 43.36;  // Vigo-ish, late sunsets
    cfgs[3].longitude = -8.72;
    cfgs[3].night_start = L"21:45";
    for (size_t ci = 0; ci < cfgs.size(); ci++) {
        for (int day = 0; day < 365; day += 3) {
            Date d = AddDays({2026, 1, 1}, day);
            for (int minute = 7; minute < 1440; minute += 97) {
                LocalTime t = Combine(d, {minute / 60, minute % 60});
                Controller ctl;
                Status st = ctl.Get(t, cfgs[ci]);
                printf("%zu %04d-%02d-%02d %02d:%02d %s %d %s\n", ci, d.y, d.m, d.d, minute / 60, minute % 60,
                       ToUtf8(Label(st.phase.name)).c_str(), st.kelvin,
                       st.next ? (ToUtf8(Label(st.next->name)) + "@" + ToUtf8(FormatHM(st.next->start))).c_str() : "-");
            }
        }
    }
    for (int k = 800; k <= 6500; k += 100) {
        Ramp r = BuildRamp(k, 0.8, false, 0.7);
        printf("ramp %d %u %u %u %u\n", k, r.ch[0][128], r.ch[1][200], r.ch[2][255], r.ch[1][17]);
        Rgb8 t = Tint(k, false);
        printf("tint %d %d %d %d\n", k, t.r, t.g, t.b);
    }
}

// Candela's own: the .exe lists, and its settings surviving a save.
static void ListTests() {
    CHECK(SplitList(L" a.exe, b.exe;;c.exe ,A.EXE") == (std::vector<std::wstring>{L"a.exe", L"b.exe", L"c.exe"}));
    CHECK(JoinList({L"a.exe", L"b.exe"}) == L"a.exe, b.exe");
    CHECK(ListHas(L"Game.exe, other.exe", L"game.EXE") && !ListHas(L"game.exe", L"gam.exe") && !ListHas(L"", L"x"));

    Config c;
    CHECK(c.switcher_display == L"active");
    c.games = L"shared.exe, elden.exe";
    c.anticheat_games = L"valorant.exe";
    c.switcher = false;
    c.switcher_delay_ms = 120;
    c.switcher_display = L"\\\\.\\DISPLAY2";
    c.font = L"Bahnschrift";
    c.palette = L"custom";
    c.color_ink = L"FFEECC";
    std::map<std::string, std::string> none, back_unknown;
    Config back;
    CHECK(ParseConfig(SerializeConfig(c, none), &back, &back_unknown));
    CHECK(back.games == c.games && back.anticheat_games == c.anticheat_games && back.palette == L"custom" &&
          back.color_ink == L"FFEECC" && back.switcher_delay_ms == 120 && !back.switcher && back.font == L"Bahnschrift");
    CHECK(back.switcher_display == L"\\\\.\\DISPLAY2");
    CHECK(back_unknown.empty());

    // Settings older versions wrote and this one dropped are kept as unknown keys, not misread.
    Config old;
    std::map<std::string, std::string> old_unknown;
    CHECK(ParseConfig(R"({"app_icon": "x", "icon_entry_hidden": true, "day_kelvin": 5000})", &old, &old_unknown));
    CHECK(old.day_kelvin == 5000 && old_unknown.size() == 2);

    // Fades: off by default (an older file reads as before), and kept on a save.
    CHECK(!old.fade && old.fade_seconds == 20 && old.fade_hotkey_ms == 400);
    Config f;
    f.fade = true;
    f.fade_seconds = 1800;
    f.fade_hotkey_ms = 0;
    Config fback;
    CHECK(ParseConfig(SerializeConfig(f, none), &fback, nullptr));
    CHECK(fback.fade && fback.fade_seconds == 1800 && fback.fade_hotkey_ms == 0);
}

// The two popups' corners and the new-song popup.
static void PopupCornerTests() {
    Config c;
    CHECK(c.media_popup_position == L"same" && c.media_new_song);
    CHECK(MediaCorner(c) == L"bottom-right");  // follows the warmth popup by default
    c.popup_position = L"top-left";
    CHECK(MediaCorner(c) == L"top-left");
    c.media_popup_position = L"bottom-center";
    CHECK(MediaCorner(c) == L"bottom-center" && c.popup_position == L"top-left");
    c.media_popup_position = L"middle";  // a typo in the file: back to following
    CHECK(MediaCorner(c) == L"top-left");
    CHECK(IsPopupCorner(L"top-center") && !IsPopupCorner(L"same") && !IsPopupCorner(L""));

    // An older config.json (one shared corner) reads as before.
    Config old;
    CHECK(ParseConfig(R"({"popup_position": "top-right", "media_popup": true})", &old, nullptr));
    CHECK(MediaCorner(old) == L"top-right" && old.media_new_song);

    // And the new keys survive a save.
    c.media_popup_position = L"top-center";
    c.media_new_song = false;
    CHECK(c.song_apps.empty());
    c.song_apps = L"foobar2000.exe, chrome.exe";
    std::map<std::string, std::string> none;
    Config back;
    CHECK(ParseConfig(SerializeConfig(c, none), &back, nullptr));
    CHECK(back.media_popup_position == L"top-center" && !back.media_new_song && back.popup_position == L"top-left" &&
          back.song_apps == L"foobar2000.exe, chrome.exe");
}

// The Controller keeps the day's timeline between ticks. One Controller ticking
// through days, DST changes and settings changes must always agree with a
// fresh one (which can't have anything cached).
static bool SameStatus(const Status& a, const Status& b) {
    bool next = a.next.has_value() == b.next.has_value() && (!a.next || a.next->Same(*b.next));
    return a.kelvin == b.kelvin && a.phase.Same(b.phase) && next && a.scheduled == b.scheduled;
}

static void TimelineCacheTests() {
    Config base = MadridConfig();
    std::vector<Config> cfgs(5, base);
    cfgs[1].night_start = L"00:30";
    cfgs[2].sunset_offset_min = -45;
    cfgs[3].morning_mode = L"fixed";
    cfgs[3].morning_time = L"06:15";
    cfgs[4].latitude = 60.17;  // Helsinki: very different sun times
    cfgs[4].longitude = 24.94;
    int mismatches = 0, steps = 0;
    {  // ticking every 7 minutes for a month around the October DST change, one config
        Controller ctl;
        LocalTime t = At("2026-10-10T00:03");
        for (; t < At("2026-11-05T00:00"); t += 7 * 60, steps++) {
            Controller fresh;
            if (!SameStatus(ctl.Get(t, base), fresh.Get(t, base))) mismatches++;
        }
    }
    {  // switching settings between ticks (as the settings window does)
        Controller ctl;
        LocalTime t = At("2026-03-20T00:10");
        for (int i = 0; t < At("2026-04-05T00:00"); t += 41 * 60, i++, steps++) {
            const Config& c = cfgs[(i / 3) % cfgs.size()];
            Controller fresh;
            if (!SameStatus(ctl.Get(t, c), fresh.Get(t, c))) mismatches++;
        }
    }
    {  // going back in time (the clock was changed) after ForgetTimeline
        Controller ctl;
        ctl.Get(At("2026-09-27T12:00"), base);
        ctl.ForgetTimeline();
        Controller fresh;
        CHECK(SameStatus(ctl.Get(At("2026-09-20T21:00"), base), fresh.Get(At("2026-09-20T21:00"), base)));
        // Even without ForgetTimeline a different date rebuilds it.
        CHECK(SameStatus(ctl.Get(At("2026-09-24T21:00"), base), fresh.Get(At("2026-09-24T21:00"), base)));
    }
    CHECK(steps > 5000);
    CHECK(mismatches == 0);
    if (mismatches) printf("  timeline cache: %d of %d ticks differ\n", mismatches, steps);
}

// Values a hand-edited file may hold that must never become numbers.
static void ConfigNumberTests() {
    Config c;
    CHECK(ParseConfig(R"({"dim_min": -nan, "dim_step": -inf, "latitude": -Infinity, "day_kelvin": 5000})", &c, nullptr));
    CHECK(c.dim_min == 0.1 && c.dim_step == 0.1 && c.latitude == kPlaceholderLat && c.day_kelvin == 5000);
}

// A stand-in for Windows' default limit, as ReactOS reimplements it: every
// level within 128 (of 256) of normal. Windows' own check isn't documented, but
// this one puts the limit at about 3300K, matching the ~3400K people see.
static bool DefaultLimit(const Ramp& r) {
    for (int c = 0; c < 3; c++)
        for (int i = 0; i < 256; i++) {
            int v = r.ch[c][i] / 256;
            if (v < i - 128 || v > i + 128) return false;
        }
    return true;
}

static void LimitTests() {
    Ramp id = IdentityRamp(), warm = BuildRamp(2000);
    CHECK(!memcmp(Blend(id, warm, 0).ch, id.ch, sizeof(id.ch)));
    CHECK(!memcmp(Blend(id, warm, 1).ch, warm.ch, sizeof(warm.ch)));
    for (int k : {900, 1500, 2000, 3400, 4800, 6500}) CHECK(ApproxKelvin(BuildRamp(k)) == k);
    CHECK(std::fabs(ApproxBrightness(BuildRamp(6500, 1, false, 0.5)) - 0.5) < 0.01);
    CHECK(std::fabs(ApproxBrightness(BuildRamp(2000, 1, false, 0.3)) - 0.3) < 0.01);

    // 2000K is refused; the closest taken is about 3300-3600K, and a step further isn't taken.
    CHECK(!DefaultLimit(warm));
    Ramp last_taken = id;
    double t = ClosestAccepted(id, warm, [&](const Ramp& r) {
        bool ok = DefaultLimit(r);
        if (ok) last_taken = r;
        return ok;
    });
    CHECK(t > 0.3 && t < 0.9);
    CHECK(DefaultLimit(Blend(id, warm, t)));
    CHECK(!DefaultLimit(Blend(id, warm, t + 2.0 / 256)));
    CHECK(!memcmp(last_taken.ch, Blend(id, warm, t).ch, sizeof(id.ch)));  // what's on screen is the answer
    int k = ApproxKelvin(Blend(id, warm, t));
    CHECK(k >= 3200 && k <= 3700);

    // Dimming alone: held at about half.
    Ramp dim = BuildRamp(6500, 1, false, 0.2);
    CHECK(!DefaultLimit(dim));
    double td = ClosestAccepted(id, dim, DefaultLimit);
    double b = ApproxBrightness(Blend(id, dim, td));
    CHECK(b > 0.45 && b < 0.56);
    CHECK(ApproxKelvin(Blend(id, dim, td)) == 6500);

    // Afterglow (red only): part of the way, never nothing.
    Ramp red = BuildRamp(kDarkroom);
    double tr = ClosestAccepted(id, red, DefaultLimit);
    CHECK(tr > 0.3 && tr < 0.7);

    // Nothing taken at all: 0.
    CHECK(ClosestAccepted(id, warm, [](const Ramp&) { return false; }) == 0);
    // The largest taken step, exactly: one more is refused.
    CHECK(!DefaultLimit(Blend(id, warm, t + 1.0 / 256)));

    // A guess from an earlier fit gives the same answer, in two tries when it's
    // right and never more than a search without it would take plus two.
    double reach = t * MaxDeviation(id, warm);
    int worst = 0, guessed_right = 0, cases = 0;
    for (int kk = 900; kk <= 3200; kk += 100)
        for (double bb : {1.0, 0.8, 0.5, 0.3}) {
            Ramp to = BuildRamp(kk, 1, false, bb);
            if (DefaultLimit(to)) continue;
            cases++;
            int plain_calls = 0, guess_calls = 0;
            double plain = ClosestAccepted(id, to, [&](const Ramp& r) { plain_calls++; return DefaultLimit(r); });
            double fast = ClosestAccepted(id, to, [&](const Ramp& r) { guess_calls++; return DefaultLimit(r); },
                                          reach / MaxDeviation(id, to));
            CHECK(plain == fast);
            worst = std::max(worst, guess_calls - plain_calls);
            if (guess_calls <= 2) guessed_right++;
        }
    CHECK(worst <= 2);
    CHECK(guessed_right * 2 >= cases);  // most of the time
    // A useless guess still finds it.
    CHECK(ClosestAccepted(id, warm, DefaultLimit, 0.999) == t);
    CHECK(ClosestAccepted(id, warm, DefaultLimit, 0.001) == t);

    // Inverted Afterglow: half way, red is flat (no picture), so it's not approached.
    CHECK(Approachable(warm) && Approachable(dim) && Approachable(red));
    Ramp inverted = BuildRamp(kDarkroom, 1, true);
    CHECK(!Approachable(inverted));
    Ramp flat = Blend(id, inverted, 0.5);
    CHECK(std::abs((int)flat.ch[0][255] - (int)flat.ch[0][0]) < 1000);
}

int main(int argc, char** argv) {
    if (!getenv("TZ")) {
        setenv("TZ", "Europe/Madrid", 1);
        tzset();
    }
    if (argc > 1 && !strcmp(argv[1], "--dump")) {
        Dump();
        return 0;
    }
    SolarTests();
    ColorTests();
    ScheduleTests();
    HotkeyTests();
    ConfigTests();
    ListTests();
    PopupCornerTests();
    TimelineCacheTests();
    ConfigNumberTests();
    LimitTests();
    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
