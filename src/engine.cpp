// The part that runs all the time: the hidden main window, tray icon,
// hotkeys, timers, the watchdog that puts the colors back when something
// resets them, and what the keyboard hook hands over (Alt+Tab, volume and
// media keys) with the game checks that keep it out of the way.
#include "candela.h"

#include <tlhelp32.h>
#include <wtsapi32.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <deque>
#include <optional>
#include <set>

HWND g_main = nullptr;

const std::vector<HotkeyField>& HotkeyFields() {
    static const std::vector<HotkeyField> fields = {
        {HK_WARMER, &core::Config::hotkey_warmer, L"Warmer", true},
        {HK_COOLER, &core::Config::hotkey_cooler, L"Cooler", true},
        {HK_DIMMER, &core::Config::hotkey_dimmer, L"Dimmer", true},
        {HK_BRIGHTER, &core::Config::hotkey_brighter, L"Brighter", true},
        {HK_DARKROOM, &core::Config::hotkey_darkroom, L"Afterglow on / off", false},
        {HK_TOGGLE, &core::Config::hotkey_off, L"Warmth off / on", false},
        {HK_BORDERLESS, &core::Config::hotkey_borderless, L"Borderless window", false},
        {HK_BLACKOUT, &core::Config::hotkey_blackout, L"Black out other screens", false},
    };
    return fields;
}

std::wstring HotkeyName(HotkeyId id) {
    if (Japanese()) {
        switch (id) {
            case HK_WARMER: return L"暖色へ";
            case HK_COOLER: return L"寒色へ";
            case HK_DIMMER: return L"減光";
            case HK_BRIGHTER: return L"増光";
            case HK_DARKROOM: return L"残光 ON/OFF";
            case HK_TOGGLE: return L"色温度補正 ON/OFF";
            case HK_BORDERLESS: return L"ボーダーレス化";
            case HK_BLACKOUT: return L"他画面をブラックアウト";
        }
    }
    for (auto& f : HotkeyFields())
        if (f.id == id) return f.label;
    return L"";
}

static HWND g_last_user_fg = nullptr;  // the last window you used that isn't Candela's (for "Mark as a game")
HWND LastUserForeground() { return g_last_user_fg && IsWindow(g_last_user_fg) ? g_last_user_fg : nullptr; }

namespace engine {

namespace {

enum : UINT_PTR { TIMER_TICK = 1, TIMER_BURST, TIMER_FORCE, TIMER_SAVE, TIMER_GAMES, TIMER_FADE };
constexpr int kFadeSteps = 400;  // a fade's ramp steps at most (each one too small to see)
constexpr int kFadeFilterSteps = 100;  // ...and Windows' color filter's (fewer: each one redraws the screen)
constexpr double kBurstSeconds = 3.0;
constexpr int kSurelyWarmK = 3000;  // a color this warm only gets through once warm colors are unlocked
enum : UINT {
    MENU_TOGGLE = 1, MENU_RESET, MENU_SETTINGS, MENU_LOG, MENU_QUIT, MENU_DARKROOM, MENU_FULL, MENU_BLACKOUT, MENU_GAME,
    MENU_SWITCHER, MENU_MEDIA, MENU_ANIM_FULL, MENU_ANIM_SUBTLE, MENU_ANIM_OFF, MENU_STEP_ASIDE, MENU_PAUSE, MENU_WARM,
};

struct Applied {
    int kelvin;
    double brightness;
    std::wstring fx;
    bool darkroom;
    double dr_brightness;
    bool dr_invert;
    bool operator==(const Applied& o) const {
        return kelvin == o.kelvin && brightness == o.brightness && fx == o.fx && darkroom == o.darkroom &&
               (!darkroom || (dr_brightness == o.dr_brightness && dr_invert == o.dr_invert));
    }
};

core::Controller g_ctl;
core::Status g_status;
std::optional<Applied> g_applied;
std::wstring g_filter_on = L"identity";
double g_filter_strength = 0;
std::map<std::wstring, std::wstring> g_problems;
bool g_restart_pending = false;  // warm colors allowed, Windows not restarted since (system.cpp)
// Windows refused the colors asked for: the closest it allows is on (display.cpp).
struct Shown {
    bool limited = false;
    int kelvin = core::kNeutralK;  // roughly what's on screen
    double brightness = 1;
};
Shown g_shown;
core::Ramp g_want_ramp = core::IdentityRamp();  // the ramp asked for last
// What was last put on screen, a fade's step included: where the next fade starts.
core::Ramp g_cur_ramp = core::IdentityRamp();
core::Matrix g_cur_matrix = core::IdentityMatrix();
// A fade under way: the ramp and the color filter going from where they were
// to the new colors, a step at a time (TIMER_FADE). Its last step puts the new
// colors on the usual way, with everything that checks them.
struct Fade {
    bool active = false;
    core::Ramp from, to;
    core::Matrix mfrom, mto;
    bool filter = false;     // the color filter takes part (it differs at the two ends)
    int filter_step = -1;    // the filter's step last sent
    core::Status st;         // what it fades to
    core::Effect fx;
    double start = 0, seconds = 0;
};
Fade g_fade;
// Why the colors are being put on: it decides how long they fade.
enum class Why { kUser, kSchedule, kForce };
// Warm-color notifications already shown this run: each one once, not every
// time a warm color is refused again (each evening, each warmth off / on).
std::set<std::wstring> g_noticed;
int g_resets_today = 0;
core::Date g_reset_day{0, 0, 0};
std::wstring g_last_reset;
std::deque<double> g_recent_resets;
bool g_warned_fight = false;
double g_reset_logged_at = -1e9;  // resets are logged at most once per 5 s
int g_reset_unlogged = 0;
double g_burst_until = 0;
std::wstring g_foreground = L"?";
HICON g_icon = nullptr;
int g_icon_kelvin = -1;
bool g_icon_disabled = false;
std::wstring g_tip;
std::map<int, std::wstring> g_registered;
bool g_suspended = false, g_shut = false, g_blackout_ok = false;
std::optional<std::wstring> g_last_failure;
UINT g_taskbar_created = 0;
std::vector<HWINEVENTHOOK> g_hooks;
int g_file_checks = 0;
bool g_paused = false;
std::wstring g_step_aside;  // the running anti-cheat game the keyboard was let go for, or ""

double Seconds() { return NowMs() / 1000.0; }

std::wstring HotkeyText(HotkeyId id) {
    for (auto& f : HotkeyFields())
        if (f.id == id) return g_cfg.*(f.field);
    return L"";
}

std::wstring HotkeyLabel(HotkeyId id) {
    std::wstring t = HotkeyText(id);
    return t.empty() ? L"" : L"\t" + core::DisplayHotkey(t);
}

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

std::wstring Upper(std::wstring s) {
    CharUpperBuffW(s.data(), (DWORD)s.size());
    return s;
}

// ---- tray ---------------------------------------------------------------------------------------------
NOTIFYICONDATAW Nid(UINT flags) {
    NOTIFYICONDATAW nid = {sizeof(nid)};
    nid.hWnd = g_main;
    nid.uID = 1;
    nid.uFlags = flags;
    nid.uCallbackMessage = WM_CANDELA_TRAY;
    return nid;
}

// What Windows held the screen to, short: "~3400K", "~3700K, 70%", "~50%",
// or "" (nothing held back, or Afterglow, which has no temperature to show).
std::wstring LimitText(const core::Status& st) {
    if (!g_shown.limited || st.disabled || st.kelvin <= core::kDarkroom) return L"";
    std::wstring s;
    if (g_shown.kelvin > st.kelvin + 50) s = L"~" + lang::Kelvin(g_shown.kelvin);
    if (g_shown.brightness > st.brightness + 0.02)
        s += (s.empty() ? L"~" : L", ") + std::to_wstring((int)std::nearbyint(g_shown.brightness * 100)) + L"%";
    return s;
}

// The tooltip holds 127 characters and the end is what gets cut, so what's
// wrong comes right after the summary.
std::wstring TrayTip(const core::Status& st) {
    if (g_paused) return Tr(L"Candela (paused)", L"Candela（一時停止中）");
    std::wstring tip = L"Candela - " + lang::Summary(st);
    std::wstring held = LimitText(st);
    bool registry = g_problems.count(L"registry") > 0;
    if (g_shown.limited && registry) {
        if (held.empty())
            tip += L"\n" + (g_restart_pending ? Tr(L"Limited by Windows until you restart your PC", L"PC再起動まではWindowsが制限")
                                             : Tr(L"Limited by Windows: right-click to unlock", L"Windowsが制限中：右クリックで解除"));
        else if (g_restart_pending)
            tip += L"\n" + (Japanese() ? L"PC再起動まで" + held + L"で停止" : L"Held at " + held + L" until you restart your PC");
        else
            tip += L"\n" + (Japanese() ? L"Windowsが" + held + L"で制限中：右クリックで解除"
                                       : L"Windows limit: " + held + L". Right-click to unlock");
    } else if (registry) {
        tip += L"\n" + (g_restart_pending ? Tr(L"Restart your PC to unlock warm colors", L"PCを再起動して暖色域を有効化")
                                         : Tr(L"Warm colors locked: right-click to unlock", L"暖色域が制限中：右クリックで解除"));
    } else if (g_shown.limited) {
        tip += L"\n" + (held.empty()  ? Tr(L"Limited by Windows", L"Windowsが制限中")
                        : Japanese() ? L"Windowsが" + held + L"で制限中"
                                     : L"Windows holds it at " + held);
    }
    if (!g_step_aside.empty())
        tip += Japanese() ? L"\n" + g_step_aside + L" 実行中：キーボード監視を停止" : L"\nKeyboard let go while " + g_step_aside + L" runs";
    return tip;
}

// The icon takes the color that's really on screen.
int IconKelvin(const core::Status& st) {
    return g_shown.limited && !st.disabled && st.kelvin > core::kDarkroom ? g_shown.kelvin : st.kelvin;
}

void UpdateTray(const core::Status& st, bool force = false) {
    std::wstring tip = TrayTip(st);
    int kelvin = IconKelvin(st);
    bool icon_changed = force || kelvin != g_icon_kelvin || st.disabled != g_icon_disabled;
    if (!icon_changed && tip == g_tip) return;
    NOTIFYICONDATAW nid = Nid(NIF_TIP | NIF_MESSAGE);
    HICON fresh = nullptr;
    if (icon_changed) {
        int size = GetSystemMetricsForDpi(SM_CXSMICON, GetDpiForSystem());
        if (!size) size = 16;
        fresh = MakeTrayIcon(size, kelvin, st.disabled);
        if (fresh) {
            nid.uFlags |= NIF_ICON;
            nid.hIcon = fresh;
        }
    }
    wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
    if (fresh) {
        if (g_icon) DestroyIcon(g_icon);
        g_icon = fresh;
        g_icon_kelvin = kelvin;
        g_icon_disabled = st.disabled;
    }
    g_tip = tip;
}

void AddTrayIcon() {
    NOTIFYICONDATAW nid = Nid(NIF_MESSAGE | NIF_TIP);
    wcsncpy_s(nid.szTip, L"Candela", _TRUNCATE);
    if (!Shell_NotifyIconW(NIM_ADD, &nid)) Log(L"Taskbar not ready yet; the icon will appear when it is");
    g_tip.clear();
    UpdateTray(g_status, true);
}

void Balloon(const std::wstring& title, const std::wstring& text, bool warning = true) {
    NOTIFYICONDATAW nid = Nid(NIF_INFO);
    wcsncpy_s(nid.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(nid.szInfo, text.c_str(), _TRUNCATE);
    nid.dwInfoFlags = warning ? NIIF_WARNING : (NIIF_NONE | NIIF_NOSOUND);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

// ---- hotkeys --------------------------------------------------------------------------------------------
void RegisterHotkeys() {
    for (auto& f : HotkeyFields()) {
        UnregisterHotKey(g_main, f.id);
        g_problems.erase(L"hotkey" + std::to_wstring(f.id));
    }
    g_registered.clear();
    for (auto& f : HotkeyFields()) g_registered[f.id] = g_cfg.*(f.field);
    if (g_suspended) return;
    for (auto& f : HotkeyFields()) {
        std::wstring text = g_cfg.*(f.field), error;
        std::optional<core::Hotkey> hk;
        std::wstring key = L"hotkey" + std::to_wstring(f.id);
        if (!core::ParseHotkey(text, &hk, &error)) {
            g_problems[key] = error;
            continue;
        }
        if (!hk) continue;
        UINT mods = hk->mods | (f.repeats ? 0 : MOD_NOREPEAT);
        if (!RegisterHotKey(g_main, f.id, mods, hk->vk)) {
            g_problems[key] = Japanese() ? L"警告：" + core::DisplayHotkey(text) + L" は他のアプリが使用中（f.luxが実行中？）。"
                                         : core::DisplayHotkey(text) + L" is taken by another program (is f.lux still running?).";
            Log(L"Could not register hotkey %ls", text.c_str());
        }
    }
}

bool HotkeysChanged() {
    for (auto& f : HotkeyFields())
        if (g_registered[f.id] != g_cfg.*(f.field)) return true;
    return false;
}

// ---- the keyboard hook and games ----------------------------------------------------------------------------------
// A running game that should have Candela's keyboard hook out of its way
// entirely (minimized to the tray counts: its anti-cheat still runs).
std::wstring AntiCheatGameRunning() {
    if (!g_cfg.anticheat_step_aside || g_cfg.anticheat_games.empty()) return L"";
    auto names = core::SplitList(g_cfg.anticheat_games);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return L"";
    PROCESSENTRY32W pe = {sizeof(pe)};
    std::wstring found;
    for (BOOL ok = Process32FirstW(snap, &pe); ok && found.empty(); ok = Process32NextW(snap, &pe))
        for (auto& name : names)
            if (!_wcsicmp(pe.szExeFile, name.c_str())) found = name;
    CloseHandle(snap);
    return found;
}

void UpdateKeyboard() {
    keyboard::switcher_on = g_cfg.switcher;
    keyboard::media_on = g_cfg.media_popup;
    keyboard::exclusive_on = g_cfg.switcher_exclusive_fullscreen;
    keyboard::SetEnabled(!g_paused && g_step_aside.empty() && (g_cfg.switcher || g_cfg.media_popup));
    media::SettingsChanged();
}

void CheckGames() {
    g_fg_exclusive = ExclusiveFullscreenNow();
    std::wstring game = AntiCheatGameRunning();
    if (game == g_step_aside) return;
    g_step_aside = game;
    if (!game.empty()) Log(L"%ls is running: keyboard let go until it closes", game.c_str());
    else Log(L"Keyboard back");
    UpdateKeyboard();
    g_tip.clear();
    UpdateTray(g_status);
    settings::Refresh();
}

// ---- the core loop --------------------------------------------------------------------------------------------
void SetTickTimer() {
    SetTimer(g_main, TIMER_TICK, std::max(20, g_cfg.watchdog_ms), nullptr);
    if (g_cfg.force_reapply_ms > 0) SetTimer(g_main, TIMER_FORCE, std::max(50, g_cfg.force_reapply_ms), nullptr);
    else KillTimer(g_main, TIMER_FORCE);
}

void StartBurst() {
    g_burst_until = Seconds() + kBurstSeconds;
    SetTimer(g_main, TIMER_BURST, std::max(10, g_cfg.burst_ms), nullptr);
    gamma::FastWatch(kBurstSeconds, g_cfg.burst_fast_ms, g_main);
}

// Warm colors: locked by Windows, or allowed but waiting for a restart. Either
// way it's the first thing the settings footer, the tray tip and the tray menu say.
void CheckRegistry() {
    std::wstring before = g_problems.count(L"registry") ? g_problems[L"registry"] : L"";
    g_restart_pending = sys::WarmColorsRestartPending();
    if (!sys::WarmColorsAllowed() && !sys::WarmColorsWork())
        g_problems[L"registry"] = Tr(L"Warm colors are locked by Windows: about 3400K at most. Click here to unlock.",
                                     L"Windowsが暖色域を制限中：約3400Kが上限。ここをクリックして解除。");
    else if (g_restart_pending)
        g_problems[L"registry"] = Tr(L"Restart your PC to finish unlocking warm colors (about 3400K at most until then). Click here.",
                                     L"PCを再起動して暖色域の解除を完了（再起動まで約3400Kが上限）。ここをクリック。");
    else
        g_problems.erase(L"registry");
    std::wstring after = g_problems.count(L"registry") ? g_problems[L"registry"] : L"";
    if (after != before) {
        g_tip.clear();
        settings::Refresh();
    }
}

std::wstring Join(const std::vector<std::wstring>& v) {
    std::wstring s;
    for (auto& x : v) s += (s.empty() ? L"" : L", ") + x;
    return s;
}

void OnResetCaught(const std::vector<std::wstring>& names) {
    core::Date today = core::DateOf(NowLocal());
    if (!(today == g_reset_day)) {
        g_reset_day = today;
        g_resets_today = 0;
    }
    g_resets_today++;
    wchar_t secs[8];
    swprintf(secs, 8, L":%02d", (int)std::fmod(NowLocal(), 60.0));
    g_last_reset = core::FormatHM(NowLocal()) + secs + L" while " + g_foreground + L" was active";
    double now = Seconds();
    if (now - g_reset_logged_at >= 5) {
        if (g_reset_unlogged > 0)
            Log(L"Colors were reset %d more times in the last few seconds (not logged one by one)", g_reset_unlogged);
        Log(L"Colors were reset on %ls; restored (foreground: %ls)", Join(names).c_str(), g_foreground.c_str());
        g_reset_logged_at = now;
        g_reset_unlogged = 0;
    } else {
        g_reset_unlogged++;
    }
    StartBurst();
    g_recent_resets.push_back(now);
    while (!g_recent_resets.empty() && now - g_recent_resets.front() > 60) g_recent_resets.pop_front();
    if (g_recent_resets.size() > 40 && !g_warned_fight) {
        g_warned_fight = true;
        Balloon(Tr(L"Something keeps resetting the screen colors", L"警告：画面の色が繰り返しリセットされている"),
                Tr(L"Candela keeps restoring them. If f.lux or Night Light is on, turn it off; otherwise the log shows "
                   L"which app was in front.",
                   L"報告：当機が復元を継続中。推奨：f.luxや夜間モードが有効なら無効化。原因のアプリはログで確認可能。"));
    }
}

// Roughly what's on screen, from display.cpp's state: the ramp asked for, or
// the part of the way to it that Windows allowed. True if that changed.
bool UpdateShown() {
    gamma::State s = gamma::GetState();
    Shown now;
    if (s.limited > 0) {
        core::Ramp ramp = core::Blend(core::IdentityRamp(), g_want_ramp, s.t);
        now.limited = true;
        now.kelvin = core::ApproxKelvin(ramp);
        now.brightness = core::ApproxBrightness(ramp);
    }
    bool changed = now.limited != g_shown.limited || now.kelvin != g_shown.kelvin ||
                   std::fabs(now.brightness - g_shown.brightness) > 0.005;
    g_shown = now;
    return changed;
}

// Proof that warm colors are unlocked: a display took, in full, a color well
// below the ~3400K Windows allows by default. Not counted when no display took
// anything (none attached, a screen switched off, quitting).
void NoteIfWarmWorked(const core::Status& st, int displays_full) {
    if (g_fade.active) return;  // a fade's step isn't what the status says is on screen
    if (displays_full > 0 && !st.disabled && st.kelvin <= kSurelyWarmK && !sys::WarmColorsWork()) {
        sys::NoteWarmColorWorked();
        Log(L"Warm colors work");
        CheckRegistry();
    }
}

// Windows refused a change, and a later retry (display.cpp keeps trying,
// slower each time) went through: the warning goes, so the settings don't
// keep saying it was refused, and a new refusal gets its balloon again.
void ClearAcceptedRefusals() {
    bool changed = false;
    bool was_limited = g_shown.limited;
    if (!g_fade.active && UpdateShown()) {  // mid-fade the step on screen isn't the colors asked for
        g_tip.clear();
        changed = true;
        if (was_limited && !g_shown.limited) {
            g_problems.erase(L"limited");
            Log(L"Windows took the whole ramp after all");
            gamma::State s = gamma::GetState();
            if (s.limited == 0 && s.refused == 0) NoteIfWarmWorked(g_status, s.full);
        }
    }
    if (g_problems.count(L"blocked") && !gamma::AnyRefusing()) {
        g_problems.erase(L"blocked");
        g_last_failure.reset();
        Log(L"Windows accepted the colors after all");
        changed = true;
    }
    if (g_problems.count(L"colorfx-refused") && colorfx::Available() && !colorfx::Pending()) {
        g_problems.erase(L"colorfx-refused");
        changed = true;
    }
    if (changed) settings::Refresh();
}

void CheckAll() {
    auto repaired = gamma::CheckAndRepair();
    if (colorfx::CheckAndRepair()) repaired.push_back(L"color filter");
    if (!repaired.empty()) OnResetCaught(repaired);
    ClearAcceptedRefusals();
}

// Windows didn't take the ramp everywhere. Limited (it took only part of the
// way): its range limit, so warm colors are locked or Windows hasn't restarted
// since they were unlocked; the closest it allows is on. Refused (not even
// normal colors): something else. Each warm-color notification is shown once
// per run; the settings footer, the tray tip and the tray menu keep saying it.
void OnApplyFailed(const gamma::ApplyResult& res) {
    if (!res.limited.empty()) {
        bool allowed = sys::WarmColorsAllowed();
        // Held back although it's allowed: most likely Windows hasn't restarted
        // since it was (system.cpp decides). Dimming counts too: the same limit.
        if (allowed) sys::NoteWarmColorRefused();
        CheckRegistry();
        std::wstring msg, key;
        if (!allowed) {
            msg = Tr(L"Windows is blocking warm colors (and deep dimming), so Candela put on the closest it allows. "
                     L"Right-click the tray icon, choose Unlock warm colors, then restart your PC.",
                     L"報告：Windowsが暖色域（と深い減光）を制限中。当機は許可範囲内で最も近い色を適用。"
                     L"推奨：トレイアイコンを右クリックし「暖色域を解除」を実行後、PCを再起動。");
            key = L"blocked-registry";
        } else if (g_restart_pending) {
            msg = Tr(L"Windows is still limiting the colors: it hasn't restarted since warm colors were unlocked. Restart your PC.",
                     L"報告：暖色域の解除後に未再起動のため、Windowsが制限を継続中。推奨：PCを再起動。");
            key = L"blocked-restart";
        } else {
            msg = Japanese() ? L"報告：" + Join(res.limited) + L" でWindowsが色を制限。当機は許可範囲内で最も近い色を適用。"
                             : L"Windows limited the colors on " + Join(res.limited) + L", so Candela put on the closest it allows.";
            g_problems[L"limited"] = msg;
            key = L"limited";
        }
        if (!g_noticed.count(key)) {
            g_noticed.insert(key);
            Log(L"%ls", msg.c_str());
            Balloon(Tr(L"Candela couldn't change the colors fully", L"警告：画面の色を完全には変更できず"), msg);
        }
    }
    if (!res.refused.empty()) {
        std::wstring msg = Japanese() ? L"報告：" + Join(res.refused) + L" で色の変更が拒否された。"
                                      : L"Windows refused the color change on " + Join(res.refused) + L".";
        g_problems[L"blocked"] = msg;
        if (g_last_failure != msg) {
            g_last_failure = msg;
            Log(L"%ls", msg.c_str());
            Balloon(Tr(L"Candela couldn't change the colors", L"警告：画面の色を変更できず"), msg);
        }
    }
}

core::Status CurrentStatus() {
    core::Status st = g_ctl.Get(NowLocal(), g_cfg);
    if (g_paused) {  // normal colors until resumed, shown as "off"
        st.disabled = true;
        st.kelvin = core::kNeutralK;
        st.brightness = 1;
        st.overridden = false;
    }
    return st;
}

// The new colors, all at once, and everything that checks how they went.
void ApplyNow(const core::Status& st, const core::Ramp& ramp, const core::Effect& fx, bool force) {
    // Forced (display change, wake-up): put the filter back only if it's in
    // use. Re-sending "no filter" makes Windows redraw the screen for
    // nothing, which shows as a flash right when a game leaves fullscreen.
    bool fx_changes = fx.key != g_filter_on || (force && fx.key != L"identity");
    // Going warmer: filter first, then the ramp. Going cooler: the reverse.
    // Either way no frame in between drops green or blue text.
    bool filter_first = fx.strength >= g_filter_strength;
    bool fx_ok = true;
    if (fx_changes && filter_first) fx_ok = colorfx::Set(fx.matrix, fx.key == L"identity");
    auto res = gamma::Apply(ramp);
    g_want_ramp = ramp;
    g_cur_ramp = ramp;
    if (fx_changes && !filter_first) fx_ok = colorfx::Set(fx.matrix, fx.key == L"identity");
    if (fx_changes) g_cur_matrix = fx.matrix;
    g_filter_on = fx.key;
    g_filter_strength = fx.strength;
    if (fx.key != L"identity" && !colorfx::Available())
        g_problems[L"colorfx"] = Tr(L"Windows' color filter isn't available, so Afterglow is red-only.",
                                    L"警告：Windowsのカラーフィルターが使用不可。残光は赤チャンネルのみ。");
    else if (!fx_ok)  // the color filter keeps trying again by itself, slowly (cleared in ClearAcceptedRefusals)
        g_problems[L"colorfx-refused"] = Tr(L"Windows refused the color filter (is Magnifier or a color filter on?).",
                                            L"警告：Windowsがカラーフィルターを拒否（拡大鏡やカラーフィルターが有効？）。");
    else if (fx_changes) {
        g_problems.erase(L"colorfx");
        g_problems.erase(L"colorfx-refused");
    }
    Log(L"Applied %ls (%ls)", core::Describe(st.kelvin).c_str(), st.Summary().c_str());
    if (UpdateShown()) g_tip.clear();
    if (g_shown.limited)
        Log(L"Windows allowed only part of it: about %ls at %d%%", core::Describe(g_shown.kelvin).c_str(),
            (int)std::nearbyint(g_shown.brightness * 100));
    if (res.limited.empty()) g_problems.erase(L"limited");
    if (res.refused.empty()) {
        g_problems.erase(L"blocked");
        g_last_failure.reset();
    }
    if (!res.limited.empty() || !res.refused.empty()) OnApplyFailed(res);
    // Well below the ~3400K Windows allows by default, and every display
    // took it: warm colors are on, whatever else says otherwise.
    if (res.limited.empty() && res.refused.empty()) NoteIfWarmWorked(st, res.full);
}

// ---- fades --------------------------------------------------------------------------------------------
// How long a change fades in: scheduled ones and the ones you make each have
// their own time. Forced re-applies (a display change, waking up) never fade.
double FadeSeconds(Why why) {
    if (!g_cfg.fade || why == Why::kForce) return 0;
    return why == Why::kSchedule ? g_cfg.fade_seconds : g_cfg.fade_hotkey_ms / 1000.0;
}

void StopFade() {
    if (!g_fade.active) return;
    g_fade.active = false;
    KillTimer(g_main, TIMER_FADE);
}

// The fade's end: the new colors go on the usual way, with every check.
void FinishFade(bool force = false) {
    if (!g_fade.active) return;
    StopFade();
    ApplyNow(g_fade.st, g_fade.to, g_fade.fx, force);
    UpdateTray(g_status);
}

// One step of the fade under way. Mid-way, how Windows took each step isn't
// reported (no balloons, no log); the last step's result is.
void FadeStep(bool force = false) {
    if (!g_fade.active) return;
    double p = (Seconds() - g_fade.start) / g_fade.seconds;
    if (!(p < 1)) {
        FinishFade(force);
        return;
    }
    double t = core::FadeCurve(p);
    if (g_fade.filter) {
        // Fewer steps for the filter than for the ramp: each one makes Windows redraw.
        int step = (int)std::nearbyint(t * kFadeFilterSteps);
        if (step != g_fade.filter_step || force) {
            g_fade.filter_step = step;
            g_cur_matrix = core::BlendMatrix(g_fade.mfrom, g_fade.mto, (double)step / kFadeFilterSteps);
            colorfx::Set(g_cur_matrix, false);
            g_filter_on = L"fading";  // so the last step sends the real filter, whichever it is
        }
    }
    g_cur_ramp = core::Blend(g_fade.from, g_fade.to, t);
    g_want_ramp = g_cur_ramp;
    auto res = gamma::Apply(g_cur_ramp);
    // Windows held this step back (warm colors still locked) or refused it:
    // the fade has gone as far as Windows lets it. Finish now, so the closest
    // ramp Windows takes is worked out once rather than again on every step
    // (each try is a call to the driver, and a taken try shows on screen).
    if (!res.limited.empty() || !res.refused.empty()) FinishFade(force);
}

// From what's on screen now (a fade's step too, so a new change mid-fade
// carries on smoothly from there) to the new colors.
void StartFade(const core::Status& st, const core::Ramp& ramp, const core::Effect& fx, double seconds) {
    // Held back by Windows (warm colors locked): start from what's really on
    // screen, the part of the way to the last colors that Windows took.
    gamma::State shown = gamma::GetState();
    g_fade.from = !g_fade.active && shown.limited > 0 ? core::Blend(core::IdentityRamp(), g_cur_ramp, shown.t) : g_cur_ramp;
    g_fade.to = ramp;
    g_fade.mfrom = g_cur_matrix;
    g_fade.mto = fx.matrix;
    // The same filter at both ends (dimming in Afterglow, say): left alone,
    // rather than sent again on every step for nothing.
    g_fade.filter = colorfx::Available() && memcmp(g_fade.mfrom.m, g_fade.mto.m, sizeof(g_fade.mto.m)) != 0;
    g_fade.filter_step = -1;
    g_fade.st = st;
    g_fade.fx = fx;
    g_fade.start = Seconds();
    g_fade.seconds = seconds;
    g_fade.active = true;
    if (seconds >= 5) Log(L"Fading to %ls over %d s", core::Describe(st.kelvin).c_str(), (int)std::nearbyint(seconds));
    SetTimer(g_main, TIMER_FADE, std::clamp((int)(seconds * 1000 / kFadeSteps), 16, 1000), nullptr);
}

void Tick(Why why = Why::kUser) {
    if (g_shut) return;  // quitting: normal colors stay
    bool force = why == Why::kForce;
    core::Status st = CurrentStatus();
    g_status = st;
    bool darkroom = st.kelvin <= core::kDarkroom;
    core::Effect fx = core::EffectFor(st.kelvin, g_cfg.darkroom_by_brightness);
    Applied want{st.kelvin, st.brightness, fx.key, darkroom, g_cfg.darkroom_brightness, g_cfg.darkroom_invert};
    if (!g_applied || !(want == *g_applied)) {
        core::Ramp ramp = core::BuildRamp(st.kelvin, g_cfg.darkroom_brightness, g_cfg.darkroom_invert, st.brightness);
        double seconds = g_applied ? FadeSeconds(why) : 0;  // the first colors at start-up go straight on
        // To or from inverted Afterglow, half way would be a flat screen: straight on.
        if (!core::Approachable(ramp) || !core::Approachable(g_cur_ramp)) seconds = 0;
        if (seconds > 0) {
            StartFade(st, ramp, fx, seconds);
        } else {
            StopFade();
            ApplyNow(st, ramp, fx, force);
        }
        g_applied = want;
    } else if (force) {
        // A display change or waking up, nothing new to show: put the colors
        // back as they are, a fade under way included.
        if (g_fade.active) FadeStep(true);
        else ApplyNow(st, core::BuildRamp(st.kelvin, g_cfg.darkroom_brightness, g_cfg.darkroom_invert, st.brightness), fx, true);
    } else {
        CheckAll();
    }
    UpdateTray(st);
}

// ---- hotkey actions and the popup --------------------------------------------------------------------------
void ShowPopup(HotkeyId id) {
    const core::Status& st = g_status;
    std::wstring until = lang::Until(st);
    std::wstring on_schedule = Tr(L"schedule", L"スケジュール通り");
    popup::Content c;
    float warmth = (float)(st.kelvin - core::kDarkroom) / (core::kMaxK - core::kDarkroom);
    Color tint = FromTint(core::Tint(st.kelvin, st.disabled));
    auto warm = [&](const std::wstring& title, const std::wstring& value, const std::wstring& note) {
        c = {title, value, note, warmth, true, tint};
    };
    // Windows held the screen back (warm colors locked): the note says how far it really went.
    std::wstring held = LimitText(st);
    std::wstring limit_note = held.empty() ? L"" : Tr(L"Windows limit ", L"制限 ") + held;
    if (!held.empty()) tint = FromTint(core::Tint(g_shown.kelvin, false));
    if (g_paused) {
        c = {L"Candela", Tr(L"Paused", L"一時停止中"), Tr(L"resume from the tray", L"トレイから再開")};
    } else if (id == HK_DIMMER || id == HK_BRIGHTER) {
        bool dimmed = st.brightness < 0.999;
        c = {Tr(L"Brightness", L"輝度"), std::to_wstring((int)std::nearbyint(st.brightness * 100)) + L"%",
             !limit_note.empty() ? limit_note : dimmed ? Tr(L"until morning", L"翌朝まで") : L"", (float)st.brightness, false, {}};
    } else if (st.disabled) {
        c = {Tr(L"Warmth", L"色温度"), lang::OnOff(false), until};
    } else if (id == HK_TOGGLE) {
        warm(Tr(L"Warmth", L"色温度"), lang::OnOff(true) + L" · " + lang::Kelvin(st.kelvin), limit_note);
    } else if (id == HK_DARKROOM) {
        if (st.Darkroom()) c = {Tr(L"Afterglow", L"残光"), lang::OnOff(true), st.overridden ? until : on_schedule};
        else warm(Tr(L"Afterglow", L"残光"), lang::OnOff(false), Tr(L"now ", L"現在 ") + (held.empty() ? lang::Kelvin(st.kelvin) : held));
    } else {
        warm(Tr(L"Warmth", L"色温度"), lang::Kelvin(st.kelvin),
             !limit_note.empty() ? limit_note : st.overridden ? until : on_schedule);
    }
    popup::Show(c);
}

void ToggleBlackout(bool show) {
    if (!g_blackout_ok) return;
    bool was_on = blackout::Active();
    int count = blackout::Toggle(MonitorFromWindow(GetForegroundWindow(), MONITOR_DEFAULTTOPRIMARY), g_cfg.blackout_always_on_top);
    if (!(show && g_cfg.show_popup)) return;
    std::wstring title = Tr(L"Other screens", L"他画面");
    if (was_on) popup::Show({title, Tr(L"Back", L"復帰"), L""});
    else if (count)
        popup::Show({title, Tr(L"Black", L"ブラックアウト"),
                     std::to_wstring(count) + (Japanese() ? L"画面" : count > 1 ? L" screens" : L" screen")});
    else popup::Show({title, Tr(L"Only one", L"対象なし"), Tr(L"no other screen found", L"他の画面を検出できず")});
}

void ToggleBorderless() {
    HWND fg = GetForegroundWindow();
    std::wstring name = ProcessName(fg);
    if (name.size() > 4 && !_wcsicmp(name.c_str() + name.size() - 4, L".exe")) name.resize(name.size() - 4);
    bool on = false;
    std::wstring error;
    if (!borderless::Toggle(fg, g_cfg.borderless_overhang_px, &on, &error)) {
        Log(L"Borderless not possible: %ls", error.c_str());
        if (g_cfg.show_popup) popup::Show({Tr(L"Borderless", L"ボーダーレス"), Tr(L"Not here", L"不可"), error});
        return;
    }
    if (g_cfg.show_popup) popup::Show({Tr(L"Borderless", L"ボーダーレス"), lang::OnOff(on), name});
    StartBurst();  // watch closely in case the switch resets the colors
}

// ---- the menu --------------------------------------------------------------------------------------------
void ShowMenu() {
    const core::Status& st = g_status;
    HMENU m = CreatePopupMenu(), anim = CreatePopupMenu();
    auto check = [](bool on) { return (UINT)(MF_STRING | (on ? MF_CHECKED : MF_UNCHECKED)); };
    std::wstring held = LimitText(st);
    std::wstring summary = g_paused ? Tr(L"Paused", L"一時停止中")
                                    : lang::Summary(st) + (held.empty() ? L"" : Tr(L" (Windows: ", L"（Windows：") + held + Tr(L")", L"）"));
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, summary.c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    // Warm colors locked, or waiting for a restart: the fix, right at the top.
    if (g_problems.count(L"registry")) {
        std::wstring fix = g_restart_pending ? Tr(L"Restart to unlock warm colors...", L"再起動して暖色域を有効化...")
                                             : Tr(L"Unlock warm colors...", L"暖色域を解除...");
        AppendMenuW(m, MF_STRING, MENU_WARM, fix.c_str());
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    }
    // Warmth.
    UINT warmth = g_paused ? MF_GRAYED : 0;
    std::wstring label;
    if (st.disabled) label = Tr(L"Turn warmth back on", L"色温度補正を再開") + HotkeyLabel(HK_TOGGLE);
    else if (st.next && Japanese())
        label = lang::Phase(st.next->name) + L"（" + core::FormatHM(st.next->start) + L"）まで停止" + HotkeyLabel(HK_TOGGLE);
    else if (st.next)
        label = L"Warmth off until " + Lower(core::Label(st.next->name)) + L" (" + core::FormatHM(st.next->start) + L")" +
                HotkeyLabel(HK_TOGGLE);
    else label = Tr(L"Warmth off", L"色温度補正を停止") + HotkeyLabel(HK_TOGGLE);
    AppendMenuW(m, MF_STRING | warmth, MENU_TOGGLE, label.c_str());
    AppendMenuW(m, MF_STRING | warmth | (st.Darkroom() ? MF_CHECKED : 0), MENU_DARKROOM,
                (Tr(L"Afterglow", L"残光") + HotkeyLabel(HK_DARKROOM)).c_str());
    if (g_blackout_ok)
        AppendMenuW(m, MF_STRING | (blackout::Active() ? MF_CHECKED : 0), MENU_BLACKOUT,
                    (Tr(L"Black out other screens", L"他画面をブラックアウト") + HotkeyLabel(HK_BLACKOUT)).c_str());
    if (st.brightness < 0.999)
        AppendMenuW(m, MF_STRING, MENU_FULL,
                    (Tr(L"Full brightness (now ", L"輝度を最大に（現在 ") + std::to_wstring((int)std::nearbyint(st.brightness * 100)) +
                     Tr(L"%)", L"%）"))
                        .c_str());
    if (st.overridden) AppendMenuW(m, MF_STRING, MENU_RESET, Tr(L"Back to schedule", L"スケジュールに戻す").c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    // The keyboard's parts.
    AppendMenuW(m, check(g_cfg.switcher), MENU_SWITCHER, Tr(L"Alt+Tab switcher", L"Alt+Tab スイッチャー").c_str());
    AppendMenuW(m, check(g_cfg.media_popup), MENU_MEDIA, Tr(L"Volume and media popup", L"音量・メディア通知").c_str());
    AppendMenuW(anim, check(g_cfg.animation == L"full"), MENU_ANIM_FULL, Tr(L"Full", L"フル").c_str());
    AppendMenuW(anim, check(g_cfg.animation == L"subtle"), MENU_ANIM_SUBTLE, Tr(L"Subtle", L"控えめ").c_str());
    AppendMenuW(anim, check(g_cfg.animation == L"off"), MENU_ANIM_OFF, L"Off");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)anim, Tr(L"Animation", L"演出").c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    // Games.
    HWND last = LastUserForeground();
    std::wstring game = last ? ProcessExeName(last) : L"";
    std::wstring mark = game.empty() ? Tr(L"Mark the app in front as a game", L"前面のアプリをゲームとして登録")
                                     : Japanese() ? game + L" の上に通知を出さない" : L"No popups over " + game;
    AppendMenuW(m, MF_STRING | (game.empty() ? MF_GRAYED : 0) | (IsGame(game) ? MF_CHECKED | MF_GRAYED : 0), MENU_GAME, mark.c_str());
    if (!g_cfg.anticheat_games.empty()) {
        std::wstring step = Tr(L"Let go of the keyboard for anti-cheat games", L"アンチチート対策のゲーム実行中は待機");
        if (!g_step_aside.empty()) step += Japanese() ? L"（" + g_step_aside + L" 実行中）" : L" (" + g_step_aside + L" running)";
        AppendMenuW(m, check(g_cfg.anticheat_step_aside), MENU_STEP_ASIDE, step.c_str());
    }
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, check(g_paused), MENU_PAUSE, Tr(L"Pause everything", L"全機能を一時停止").c_str());
    AppendMenuW(m, MF_STRING | MF_DEFAULT, MENU_SETTINGS, Tr(L"Settings...", L"設定...").c_str());
    AppendMenuW(m, MF_STRING, MENU_LOG, Tr(L"Open log folder", L"ログフォルダーを開く").c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, MENU_QUIT, Tr(L"Quit", L"終了").c_str());
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_main);  // without this the menu doesn't close when you click elsewhere
    UINT cmd = TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, g_main, nullptr);
    PostMessageW(g_main, WM_NULL, 0, 0);
    DestroyMenu(m);  // and the submenu with it
    auto set_anim = [](const wchar_t* v) {
        g_cfg.animation = v;
        SaveConfigSoon();
    };
    switch (cmd) {
        case MENU_TOGGLE: Hotkey(HK_TOGGLE, false); break;
        case MENU_DARKROOM: Hotkey(HK_DARKROOM, false); break;
        case MENU_BLACKOUT: ToggleBlackoutAtCursor(); break;
        case MENU_FULL: FullBrightness(); break;
        case MENU_RESET: BackToSchedule(); break;
        case MENU_SWITCHER: g_cfg.switcher = !g_cfg.switcher; KeyboardSettingsChanged(); SaveConfigSoon(); break;
        case MENU_MEDIA: g_cfg.media_popup = !g_cfg.media_popup; KeyboardSettingsChanged(); SaveConfigSoon(); break;
        case MENU_ANIM_FULL: set_anim(L"full"); break;
        case MENU_ANIM_SUBTLE: set_anim(L"subtle"); break;
        case MENU_ANIM_OFF: set_anim(L"off"); break;
        case MENU_GAME:
            if (AddGame(game)) SaveConfigSoon();
            break;
        case MENU_STEP_ASIDE:
            g_cfg.anticheat_step_aside = !g_cfg.anticheat_step_aside;
            KeyboardSettingsChanged();
            SaveConfigSoon();
            break;
        case MENU_PAUSE: SetPaused(!g_paused); break;
        case MENU_WARM:
            if (g_restart_pending) sys::AskRestartForWarmColors(false);
            else sys::UnlockWarmColors();
            Wake();
            break;
        case MENU_SETTINGS: settings::Open(); break;
        case MENU_LOG: sys::OpenFolder(AppDir()); break;
        case MENU_QUIT: Quit(); break;
    }
    if (cmd && cmd != MENU_QUIT) settings::Refresh();
}

// ---- window -------------------------------------------------------------------------------------------------
// A window you'd be using: not Candela's, not the taskbar, tray or desktop
// (right-clicking the tray icon makes the taskbar the window in front).
bool IsUserWindow(HWND h) {
    if (!h) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId()) return false;
    wchar_t cls[64] = {};
    if (!GetClassNameW(h, cls, 64)) return false;
    for (auto* shell : {L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd", L"NotifyIconOverflowWindow",
                        L"TopLevelWindowForOverflowXamlIsland", L"Progman", L"WorkerW", L"Windows.UI.Core.CoreWindow",
                        L"XamlExplorerHostIslandWindow"})
        if (!wcscmp(cls, shell)) return false;
    return true;
}

void CALLBACK OnWinEvent(HWINEVENTHOOK, DWORD event, HWND, LONG, LONG, DWORD, DWORD) {
    // First the colors: this is the moment a game leaving fullscreen resets them.
    CheckAll();
    StartBurst();  // resets often land a few frames after a window switch
    HWND fg = GetForegroundWindow();
    g_foreground = ProcessName(fg);
    if (event == EVENT_SYSTEM_FOREGROUND) {
        blackout::OnForeground(fg);
        if (IsUserWindow(fg)) g_last_user_fg = fg;
        g_fg_exclusive = ExclusiveFullscreenNow();
    }
}

void DoShutdown() {
    if (g_shut) return;
    g_shut = true;
    KillTimer(g_main, TIMER_SAVE);
    if (ConfigDirty()) SaveConfigNow();  // only what you changed; hand edits are never overwritten
    keyboard::Stop();
    StopFade();
    gamma::StopFastWatch();
    colorfx::Close();
    gamma::Apply(core::IdentityRamp());
    gamma::Close();
    Log(L"Restored normal colors");
    blackout::Clear();
    borderless::RestoreAll();
    for (auto h : g_hooks) UnhookWinEvent(h);
    g_hooks.clear();
    for (auto& f : HotkeyFields()) UnregisterHotKey(g_main, f.id);
    NOTIFYICONDATAW nid = Nid(0);
    Shell_NotifyIconW(NIM_DELETE, &nid);
    if (g_icon) DestroyIcon(g_icon);
    g_icon = nullptr;
}

LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_TIMER:
            // After WM_ENDSESSION the window lives on until Windows ends the
            // process; nothing may put colors back or touch the keyboard then.
            if (g_shut) return 0;
            if (wp == TIMER_SAVE) {
                KillTimer(h, TIMER_SAVE);
                SaveConfigNow();
                return 0;
            }
            if (wp == TIMER_GAMES) {
                CheckGames();
                return 0;
            }
            if (wp == TIMER_FADE) {
                bool fading = g_fade.active;
                FadeStep();
                if (fading && !g_fade.active) settings::Refresh();  // done: the settings show how it went
                return 0;
            }
            if (wp == TIMER_BURST && Seconds() > g_burst_until) KillTimer(h, TIMER_BURST);
            if (wp == TIMER_FORCE) {
                gamma::CheckAndRepair(0.001);
                return 0;
            }
            if (wp == TIMER_TICK && g_file_checks % 100 == 99 && ConfigDirty()) SaveConfigNow();  // a save that failed: try again
            // Every 10 s or so: warm colors may have been allowed meanwhile (an
            // admin question answered late, f.lux, Candela mini, regedit).
            if (wp == TIMER_TICK && g_file_checks % 100 == 49) CheckRegistry();
            if (wp == TIMER_TICK && ++g_file_checks % 20 == 0 && !ConfigDirty() && ConfigFileChanged()) {
                // config.json was edited by hand (Settings file -> Open): use it.
                // (Logged once it worked: a file still locked by its editor is
                // looked at again on every check until it can be read.)
                // The palette's own colors too: with palette "custom", editing a
                // color leaves the palette's name as it was.
                auto look = [] {
                    return g_cfg.palette + L"|" + g_cfg.language + L"|" + g_cfg.font + L"|" + g_cfg.font_ja + L"|" +
                           g_cfg.color_paper + g_cfg.color_paper_dark + g_cfg.color_ink + g_cfg.color_ink_soft + g_cfg.color_shade;
                };
                std::wstring looked = look();
                if (LoadConfig(true)) {  // false: a mistake or unreadable; the settings stay as they were
                    Log(L"config.json changed; reloaded it");
                    SetTickTimer();
                    if (HotkeysChanged()) RegisterHotkeys();
                    KeyboardSettingsChanged();
                    if (look() != looked) StyleChanged();
                    if (!g_cfg.fade) FinishFade();  // fades just turned off: the one under way ends now
                    Tick(Why::kUser);  // a hand edit counts as a change you made
                    settings::Refresh();
                }
            }
            Tick(Why::kSchedule);
            return 0;
        case WM_QUERYENDSESSION: return TRUE;
        case WM_ENDSESSION:
            // Signing out or shutting down: Windows ends Candela without WM_CLOSE.
            if (wp) {
                DoShutdown();
                // Asked to close for an update or install (Restart Manager), not a
                // sign-out: exit for real rather than linger with everything off.
                if (lp & ENDSESSION_CLOSEAPP) PostMessageW(h, WM_CLOSE, 0, 0);
            }
            return 0;
        case WM_CANDELA_QUIT: Quit(); return 0;
        case WM_HOTKEY: Hotkey((HotkeyId)wp); return 0;  // (both do nothing once shut down)
        case WM_CANDELA_DO: Hotkey((HotkeyId)wp); return 0;
        case WM_CANDELA_TRAY:
            if (LOWORD(lp) == WM_LBUTTONUP) settings::Open();
            else if (LOWORD(lp) == WM_RBUTTONUP) ShowMenu();
            return 0;
        case WM_CANDELA_OPEN_SETTINGS: settings::Open(); return 0;
        case WM_CANDELA_REPAIRED: {
            auto names = gamma::TakeFastRepairs();
            if (!names.empty()) OnResetCaught(names);
            return 0;
        }
        case WM_CANDELA_SWITCH: switcher::OnSwitch((int)(INT_PTR)wp); return 0;
        case WM_CANDELA_SW_KEY: switcher::OnKey((UINT)wp); return 0;
        case WM_CANDELA_SW_COMMIT: switcher::OnCommit(); return 0;
        case WM_CANDELA_MEDIA: media::OnKey((UINT)wp); return 0;
        case WM_CANDELA_MEDIA_INFO: media::OnInfo(lp); return 0;
        case WM_CANDELA_VOLUME: media::OnVolume(wp); return 0;
        case WM_TIMECHANGE:  // the clock or the time zone was changed: the day's sun times may be different now
            // After WM_ENDSESSION (or a quit while a question was up), none of
            // these may put colors back.
            if (g_shut) return 0;
            g_ctl.ForgetTimeline();
            Tick(Why::kSchedule);
            settings::Refresh();
            return 0;
        case WM_DISPLAYCHANGE:
            if (g_shut) return 0;
            Log(L"Display configuration changed");
            blackout::Clear();  // screens moved; don't leave black windows in odd places
            gamma::RefreshDisplays();
            Tick(Why::kForce);
            StartBurst();
            return 0;
        case WM_POWERBROADCAST:
            if (!g_shut && (wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND)) {
                Log(L"Resumed from sleep");
                keyboard::Reinstall();
                gamma::RefreshDisplays();
                Tick(Why::kForce);
                StartBurst();
            }
            return TRUE;
        case WM_WTSSESSION_CHANGE:
            // Unlocking, or coming back to this session after switching users or
            // a remote desktop: the hook may have been dropped meanwhile.
            if (!g_shut && (wp == WTS_SESSION_UNLOCK || wp == WTS_CONSOLE_CONNECT || wp == WTS_REMOTE_CONNECT)) {
                keyboard::Reinstall();
                Tick(Why::kForce);
                StartBurst();
            }
            return 0;
        case WM_CLOSE:
            DoShutdown();  // while the window still exists, so the tray icon goes away
            DestroyWindow(h);
            return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    if (msg == g_taskbar_created && msg) {  // Explorer restarted
        AddTrayIcon();
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

bool Init() {
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = Proc;
    wc.hInstance = g_inst;
    wc.lpszClassName = CANDELA_MAIN_CLASS;
    if (!RegisterClassExW(&wc)) return false;
    // A real (never shown) top-level window, not message-only: those don't get
    // display-change, power or "taskbar restarted" broadcasts.
    g_main = CreateWindowExW(WS_EX_TOOLWINDOW, CANDELA_MAIN_CLASS, L"Candela", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, g_inst, nullptr);
    if (!g_main) return false;
    g_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    // Candela may run as admin (Start with Windows): let a normal candela.exe
    // (the Start menu shortcut, --do) and Explorer reach it.
    for (UINT m : {g_taskbar_created, (UINT)WM_CANDELA_TRAY, (UINT)WM_CANDELA_OPEN_SETTINGS, (UINT)WM_CANDELA_DO,
                   (UINT)WM_CANDELA_QUIT})
        ChangeWindowMessageFilterEx(g_main, m, MSGFLT_ALLOW, nullptr);
    gamma::RefreshDisplays();
    colorfx::Init();
    g_blackout_ok = blackout::Init();
    RegisterHotkeys();
    popup::Init();
    if (!switcher::Init()) Log(L"The switcher could not start (error %lu)", GetLastError());
    media::Init();
    // A game leaving fullscreen usually minimizes or loses the foreground: both
    // are when the driver resets the colors.
    for (DWORD ev : {EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND})
        if (HWINEVENTHOOK hook = SetWinEventHook(ev, ev, nullptr, OnWinEvent, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS))
            g_hooks.push_back(hook);
    WTSRegisterSessionNotification(g_main, NOTIFY_FOR_THIS_SESSION);
    g_status = CurrentStatus();
    if (IsUserWindow(GetForegroundWindow())) g_last_user_fg = GetForegroundWindow();
    AddTrayIcon();
    CheckRegistry();
    Tick(Why::kForce);
    SetTickTimer();
    keyboard::Start();
    CheckGames();
    UpdateKeyboard();
    SetTimer(g_main, TIMER_GAMES, 2000, nullptr);
    if (!g_step_aside.empty())
        Balloon(Tr(L"Candela is keeping off the keyboard", L"報告：当機はキーボード監視を停止中"),
                Japanese() ? g_step_aside + L" が実行中のため、終了までAlt+Tabと音量キーをWindowsに委任。"
                           : g_step_aside + L" is running, so Alt+Tab and the volume keys are Windows' until it closes.",
                false);
    return true;
}

void Shutdown() {
    DoShutdown();
    popup::Shutdown();
    media::Shutdown();
    switcher::Shutdown();
}

HWND Window() { return g_main; }
const core::Status& GetStatus() { return g_status; }

std::vector<std::wstring> Problems() {
    std::vector<std::wstring> out;
    if (g_problems.count(L"registry")) out.push_back(g_problems[L"registry"]);
    for (auto& [key, text] : g_problems)
        if (key != L"registry") out.push_back(text);
    return out;
}
bool RegistryProblem() { return g_problems.count(L"registry") > 0; }
bool WarmRestartPending() { return g_restart_pending; }

bool Limited(int* kelvin, double* brightness) {
    if (!g_shown.limited || g_paused) return false;
    if (kelvin) *kelvin = g_status.kelvin <= core::kDarkroom ? g_status.kelvin : g_shown.kelvin;
    if (brightness) *brightness = g_shown.brightness;
    return true;
}

std::wstring LimitNote() { return LimitText(g_status); }
void SkipWarmNotice() { g_noticed.insert(L"blocked-registry"); }
bool ShuttingDown() { return g_shut; }
int ResetsToday() { return g_resets_today; }
std::wstring LastReset() { return g_last_reset; }

void Wake() {
    if (g_shut) return;  // quit while a question was up: nothing more to apply
    media::SettingsChanged();  // the album-cover setting, read by the media worker
    SetTickTimer();
    CheckRegistry();
    if (HotkeysChanged()) RegisterHotkeys();
    if (!g_cfg.fade) FinishFade();  // fades just turned off: the one under way ends now
    Tick();
    SaveConfigSoon();
}

void Hotkey(HotkeyId id, bool show) {
    if (g_shut) return;
    core::LocalTime now = NowLocal();
    if (id == HK_BORDERLESS) {
        ToggleBorderless();
        return;
    }
    if (id == HK_BLACKOUT) {
        ToggleBlackout(show);
        return;
    }
    if (g_paused) {  // warmth stays off until resumed; say so rather than change nothing silently
        if (show && g_cfg.show_popup) ShowPopup(id);
        return;
    }
    switch (id) {
        case HK_WARMER: g_ctl.Nudge(now, g_cfg, true); break;
        case HK_COOLER: g_ctl.Nudge(now, g_cfg, false); break;
        case HK_TOGGLE: g_ctl.ToggleDisabled(now, g_cfg); break;
        case HK_DARKROOM: g_ctl.ToggleDarkroom(now, g_cfg); break;
        case HK_DIMMER: g_ctl.Dim(now, g_cfg, true); break;
        case HK_BRIGHTER: g_ctl.Dim(now, g_cfg, false); break;
        default: break;
    }
    Tick();
    if (show && g_cfg.show_popup) ShowPopup(id);
    settings::Refresh();
}

void SuspendHotkeys(bool on) {
    g_suspended = on;
    RegisterHotkeys();
}

void PreviewPopup(bool force) {
    if (!force && !g_cfg.show_popup) return;
    Tick();
    ShowPopup(HK_WARMER);
}

bool BlackoutActive() { return blackout::Active(); }

void ToggleBlackoutAtCursor() {
    if (!g_blackout_ok) return;
    if (blackout::Active()) {
        blackout::Clear();
        return;
    }
    // The menu or settings took focus, so "the screen you're using" = where the pointer is.
    POINT pt;
    GetCursorPos(&pt);
    blackout::Toggle(MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY), g_cfg.blackout_always_on_top);
}

void BackToSchedule() {
    g_ctl.Reset();
    Tick();
    settings::Refresh();
}

void FullBrightness() {
    g_ctl.FullBrightness();
    Tick();
    settings::Refresh();
}

bool Paused() { return g_paused; }

void SetPaused(bool on) {
    if (g_paused == on) return;
    g_paused = on;
    Log(on ? L"Paused" : L"Resumed");
    UpdateKeyboard();
    g_tip.clear();
    Tick();
    settings::Refresh();
}

std::wstring SteppingAsideFor() { return g_step_aside; }

std::wstring KeyboardStatus() {
    if (g_paused) return Tr(L"PAUSED", L"一時停止中");
    if (!g_step_aside.empty()) return Japanese() ? L"待機 · " + Upper(g_step_aside) + L" 実行中" : L"OFF WHILE " + Upper(g_step_aside) + L" RUNS";
    if (!g_cfg.switcher && !g_cfg.media_popup) return Tr(L"KEYS OFF", L"キー操作 OFF");
    return L"ONLINE";
}

void KeyboardSettingsChanged() {
    CheckGames();
    UpdateKeyboard();
    g_tip.clear();
    UpdateTray(g_status);
}

void StyleChanged() {
    theme::Follow();  // grey or warm cursors, with the palette (also after a hand edit of config.json)
    CheckRegistry();  // its text is in the language
    switcher::StyleChanged();
    popup::Hide();
    media::Hide();
    g_tip.clear();
    UpdateTray(g_status, true);
}

void Quit() { PostMessageW(g_main, WM_CLOSE, 0, 0); }

}  // namespace engine

void SaveConfigSoon() {
    MarkConfigDirty();
    if (HWND h = engine::Window()) SetTimer(h, 4 /* TIMER_SAVE */, 500, nullptr);
    else SaveConfigNow();
}
