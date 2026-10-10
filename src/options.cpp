#include "options.h"
#include "render.h"

#include <algorithm>
#include <cmath>
#include <cwctype>

namespace opts {

Editor g_edit;
std::wstring g_recording;
static std::wstring g_notice;
static bool g_notice_error = false;
static double g_notice_at = -1e9, g_quit_armed = -1e9;

void Changed() { engine::Wake(); }

void Notice(const std::wstring& text, bool error) {
    g_notice = text;
    g_notice_error = error;
    g_notice_at = NowMs();
}

std::wstring CurrentNotice(bool* error) {
    if (NowMs() - g_notice_at > 6000) return L"";
    if (error) *error = g_notice_error;
    return g_notice;
}

bool QuitArmed() { return NowMs() - g_quit_armed < 2500; }

std::wstring StatusLine() {
    if (engine::Paused()) return Tr(L"Paused", L"一時停止中");
    const core::Status& st = engine::GetStatus();
    std::wstring s = lang::Phase(st.phase.name) + L" · ";
    if (st.disabled) return s + Tr(L"off", L"OFF");
    // Windows wouldn't take what was asked for: what's really on screen.
    int k = st.kelvin;
    double b = st.brightness;
    bool limited = engine::Limited(&k, &b);
    s += (limited && k > core::kDarkroom ? L"~" : L"") + lang::Kelvin(k);
    if (b < 0.995) s += L" · " + std::to_wstring((int)std::nearbyint(b * 100)) + L"%";
    if (limited) s += Tr(L" · limited", L" · 制限中");  // the footer says why
    return s;
}

std::wstring UntilLine() {
    if (engine::Paused()) return Tr(L"normal colors until resumed", L"再開まで通常の色");
    const core::Status& st = engine::GetStatus();
    std::wstring until = lang::Until(st);
    if (Japanese()) {
        if (st.disabled) return until.empty() ? L"OFF" : until + L" OFF";
        if (st.overridden) return until + L" 調整中（予定：" + lang::Kelvin(st.scheduled) + L"）";
        return until;
    }
    if (st.disabled) return L"off " + until;
    if (st.overridden) return L"adjusted " + until + L" (schedule: " + core::Describe(st.scheduled) + L")";
    return until;
}

// ---- builders ---------------------------------------------------------------------------------------
namespace {

Option Toggle(std::wstring key, std::wstring label, std::wstring desc, bool* flag, std::function<void()> after = nullptr) {
    Option o;
    o.key = key;
    o.label = label;
    o.desc = desc;
    o.kind = kToggle;
    o.value = [flag] { return lang::OnOff(*flag); };
    o.on = [flag] { return *flag; };
    o.change = [flag, after](int) {
        *flag = !*flag;
        Changed();
        if (after) after();
    };
    return o;
}

Option ChoiceOf(std::wstring key, std::wstring label, std::wstring desc, std::vector<Choice> choices, std::wstring* field,
                std::function<void()> after = nullptr) {
    Option o;
    o.key = key;
    o.label = label;
    o.desc = desc;
    o.kind = kChoice;
    o.choices = choices;
    o.current = [field] { return *field; };
    o.pick = [field, after](const std::wstring& v) {
        if (*field == v) return;
        *field = v;
        Changed();
        if (after) after();
    };
    o.value = [field, choices] {
        for (auto& c : choices)
            if (c.value == *field) return c.label;
        return *field;
    };
    o.change = [o](int dir) {
        int n = (int)o.choices.size(), i = 0;
        for (int k = 0; k < n; k++)
            if (o.choices[k].value == o.current()) i = k;
        o.pick(o.choices[((i + dir) % n + n) % n].value);
    };
    return o;
}

// A number stepped through a few values (stops at the first and last).
Option Steps(std::wstring key, std::wstring label, std::wstring desc, std::vector<int> values, int* field,
             std::function<std::wstring(int)> show) {
    Option o;
    o.key = key;
    o.label = label;
    o.desc = desc;
    o.kind = kStepper;
    o.value = [field, show] { return show(*field); };
    o.change = [field, values](int dir) {
        int n = (int)values.size(), i = -1;
        for (int k = 0; k < n; k++)
            if (values[k] == *field) i = k;
        if (i < 0) {  // a value typed into the file: go to its neighbour
            i = 0;
            while (i < n - 1 && values[i] < *field) i++;
            if (dir > 0 && values[i] > *field) i--;
        }
        *field = values[std::clamp(i + dir, 0, n - 1)];
        Changed();
    };
    return o;
}

Option Kelvin(std::wstring key, std::wstring label, std::wstring desc, int* field) {
    Option o;
    o.key = key;
    o.label = label;
    o.desc = desc;
    o.kind = kSlider;
    o.lo = core::kMinK;
    o.hi = core::kMaxK;
    o.step = 100;
    o.kelvin = true;
    o.get = [field] { return (double)*field; };
    o.set = [field](double v) {
        int k = (int)std::clamp(std::nearbyint(v / 100) * 100, (double)core::kMinK, (double)core::kMaxK);
        if (k == *field) return;
        *field = k;
        Changed();
    };
    o.value = [field] { return std::to_wstring(*field) + L"K"; };
    o.change = [o](int dir) { o.set(o.get() + dir * o.step); };
    return o;
}

Option TimeOf(std::wstring key, std::wstring label, std::wstring desc, std::wstring* field) {
    Option o;
    o.key = key;
    o.label = label;
    o.desc = desc;
    o.kind = kTime;
    o.value = [field] { return *field; };
    o.text = [field] { return *field; };
    o.commit = [field](const std::wstring& t, std::wstring* err) {
        auto hm = core::ParseHM(t);
        if (!hm) {
            *err = Tr(L"Times are written like 22:00.", L"警告：時刻の形式が不正。例：22:00");
            return false;
        }
        wchar_t buf[8];
        swprintf(buf, 8, L"%02d:%02d", hm->h, hm->m);
        *field = buf;
        Changed();
        return true;
    };
    o.change = [field](int dir) {  // 15 minutes at a time
        auto hm = core::ParseHM(*field);
        int m = hm ? hm->h * 60 + hm->m : 0;
        m = ((m / 15 * 15 + (dir > 0 ? 15 : (m % 15 ? 0 : -15))) % 1440 + 1440) % 1440;
        wchar_t buf[8];
        swprintf(buf, 8, L"%02d:%02d", m / 60, m % 60);
        *field = buf;
        Changed();
    };
    return o;
}

Option Action(std::wstring key, std::wstring label, std::wstring desc, std::function<std::wstring()> value,
              std::function<void()> run) {
    Option o;
    o.key = key;
    o.label = label;
    o.desc = desc;
    o.kind = kAction;
    o.value = value;
    o.change = [run](int dir) {
        if (dir > 0) run();
    };
    return o;
}

Option Info(std::wstring key, std::wstring label, std::wstring desc, std::function<std::wstring()> value) {
    Option o;
    o.key = key;
    o.label = label;
    o.desc = desc;
    o.kind = kInfo;
    o.value = value;
    o.change = [](int) {};
    return o;
}

Option List(std::wstring key, std::wstring label, std::wstring desc, std::wstring core::Config::*field) {
    Option o;
    o.key = key;
    o.label = label;
    o.desc = desc;
    o.kind = kList;
    o.list = field;
    o.value = [field] {
        size_t n = core::SplitList(g_cfg.*field).size();
        if (Japanese()) return std::to_wstring(n) + L" 件";
        return n == 0 ? std::wstring(L"None") : std::to_wstring(n) + (n == 1 ? L" game" : L" games");
    };
    o.change = [](int) {};
    return o;
}

// The apps whose new songs show: Spotify always, plus the list.
Option SongApps() {
    Option o = List(L"songapps", Tr(L"New song apps", L"曲を通知するアプリ"),
                    Tr(L"Only songs from these apps show the card by themselves; a YouTube video in a browser doesn't, "
                       L"unless you add the browser. Spotify is always in. To add an app, play something in it, then "
                       L"open these settings.",
                       L"報告：これらのアプリの曲のみ自動で通知。ブラウザを追加しない限り、YouTubeの動画は通知しない。"
                       L"Spotifyは常に対象。追加するには、アプリで再生してからこの設定画面を開く。"),
                    &core::Config::song_apps);
    o.list_fixed = media::AlwaysSongApps();
    o.list_apps = true;
    o.value = [] {
        size_t n = core::SplitList(g_cfg.song_apps).size();
        if (Japanese()) return n ? L"SPOTIFY ＋" + std::to_wstring(n) + L"件" : std::wstring(L"SPOTIFYのみ");
        return n ? L"Spotify + " + std::to_wstring(n) : std::wstring(L"Spotify only");
    };
    o.enabled = [] { return g_cfg.media_popup && g_cfg.media_new_song; };
    return o;
}

std::wstring Coord(double v) {
    wchar_t buf[32];
    swprintf(buf, 32, L"%.4f", v);
    return buf;
}

// "48.8566" or a pasted "48.8566, 2.3522" (as Google Maps copies it).
bool ParseCoords(const std::wstring& t, double* a, double* b, bool* pair) {
    std::wstring s = t;
    // "41,38" (a decimal comma) is one number; "41.38, 2.17" or "41,38; 2,17" are two.
    size_t semi = s.find(L';');
    if (semi != std::wstring::npos) {
        for (auto& c : s) {
            if (c == L',') c = L'.';
        }
        s[semi] = L',';
    } else {
        size_t first = s.find(L','), count = std::count(s.begin(), s.end(), L',');
        bool pair_like = count == 1 && (s.find(L", ") != std::wstring::npos ||
                                        (s.find(L'.') < first && s.find(L'.', first) != std::wstring::npos));
        if (count == 1 && !pair_like) s[first] = L'.';
        else if (count == 3) {  // "41,38, 2,17"
            size_t sep = s.find(L", ");
            if (sep == std::wstring::npos) return false;
            for (size_t i = 0; i < s.size(); i++)
                if (s[i] == L',' && i != sep) s[i] = L'.';
        }
    }
    size_t comma = s.find(L',');
    auto num = [](std::wstring x, double* out) {
        while (!x.empty() && iswspace(x.front())) x.erase(x.begin());
        while (!x.empty() && (iswspace(x.back()) || x.back() == L'°')) x.pop_back();
        if (x.empty()) return false;
        wchar_t* end = nullptr;
        *out = wcstod(x.c_str(), &end);
        return end && *end == 0 && std::isfinite(*out);
    };
    *pair = comma != std::wstring::npos;
    if (!*pair) return num(s, a);
    return num(s.substr(0, comma), a) && num(s.substr(comma + 1), b);
}

// A fade's length: "Instant", "0.4 s", "20 s", "5 min".
std::wstring Duration(int ms) {
    if (ms <= 0) return Tr(L"Instant", L"即時");
    std::wstring sec = Tr(L" s", L"秒"), min = Tr(L" min", L"分");
    if (ms < 60000 && ms % 1000) {
        wchar_t buf[16];
        swprintf(buf, 16, L"%.1f", ms / 1000.0);
        return buf + sec;
    }
    int s = ms / 1000;
    if (s < 60) return std::to_wstring(s) + sec;
    if (s % 60 == 0) return std::to_wstring(s / 60) + min;
    return std::to_wstring(s / 60) + min + L" " + std::to_wstring(s % 60) + sec;
}

// A small title over the rows after it.
Option Heading(std::wstring label) {
    Option o;
    o.key = L"heading:" + label;
    o.label = label;
    o.kind = kHeading;
    o.value = [] { return L""; };
    o.change = [](int) {};
    return o;
}

Option Coordinate(bool latitude) {
    Option o;
    o.key = latitude ? L"lat" : L"lon";
    o.label = latitude ? Tr(L"Latitude", L"緯度") : Tr(L"Longitude", L"経度");
    o.desc = Tr(L"Your town, for sunrise and sunset. In Google Maps, right-click your town: the first number is latitude, the "
                L"second longitude. You can paste both at once into either field. Nothing leaves your PC.",
                L"報告：日の出・日の入の算出に使用。Googleマップで地点を右クリック：最初の数値が緯度、次が経度。"
                L"両方をまとめて、どちらの欄にも貼り付け可能。通信は一切不要。");
    o.kind = kText;
    double* field = latitude ? &g_cfg.latitude : &g_cfg.longitude;
    o.value = [field] { return Coord(*field); };
    o.text = [field] { return Coord(*field); };
    o.commit = [latitude, field](const std::wstring& t, std::wstring* err) {
        double a = 0, b = 0;
        bool pair = false;
        if (!ParseCoords(t, &a, &b, &pair)) {
            *err = Tr(L"That isn't a number.", L"警告：数値として認識不可。");
            return false;
        }
        if (pair) {
            if (!(a >= -90 && a <= 90 && b >= -180 && b <= 180)) {
                *err = Tr(L"Latitude must be -90..90 and longitude -180..180.", L"警告：緯度は−90〜90、経度は−180〜180の範囲で入力。");
                return false;
            }
            g_cfg.latitude = a;
            g_cfg.longitude = b;
        } else {
            if (latitude ? !(a >= -90 && a <= 90) : !(a >= -180 && a <= 180)) {
                *err = latitude ? Tr(L"Latitude must be between -90 and 90.", L"警告：緯度は−90〜90の範囲で入力。")
                                : Tr(L"Longitude must be between -180 and 180.", L"警告：経度は−180〜180の範囲で入力。");
                return false;
            }
            *field = a;
        }
        Changed();
        return true;
    };
    o.change = [](int) {};
    return o;
}

// How a Japanese font is named in the Font row.
std::wstring JaFontName(const std::wstring& f) {
    if (f == L"rodin") return L"ロダン";
    if (f == L"Yu Gothic UI") return L"游ゴシック UI";
    if (f == L"Meiryo UI") return L"メイリオ UI";
    if (f == L"BIZ UDPGothic") return L"BIZ UDPゴシック";
    return f;
}

}  // namespace

// ---- the list ------------------------------------------------------------------------------------------
// Labels and descriptions come in pairs, English and Japanese. The Japanese
// reads like a terse system log: 報告： (report), 推奨： (recommended),
// 警告： (warning), and 当機 when Candela speaks of itself.
std::vector<Section> Build() {
    std::vector<Section> out;
    core::Config& c = g_cfg;

    // ---- the schedule, and where you are ----
    Section sc{L"schedule", Tr(L"Schedule", L"スケジュール"), {}};
    sc.opts.push_back(Kelvin(L"day", Tr(L"Day", L"日中"),
                             Tr(L"The screen's color during the day. 6500K is untouched; lower is warmer.",
                                L"報告：日中の画面色温度。6500Kは無補正。数値が低いほど暖色。"),
                             &c.day_kelvin));
    sc.opts.push_back(Kelvin(L"evening", Tr(L"From sunset", L"日没以降"),
                             Tr(L"The color from sunset (plus the offset below) until night starts.",
                                L"報告：日没（下記の補正を含む）から夜間開始までの色温度。"),
                             &c.evening_kelvin));
    {
        Option o;
        o.key = L"offset";
        o.label = Tr(L"Sunset offset", L"日没補正");
        o.desc = Tr(L"Moves the evening change away from sunset: -30 switches half an hour before it.",
                    L"報告：夕刻への切替を日没の前後に移動。例：−30で日没の30分前に切替。");
        o.kind = kStepper;
        o.value = [] {
            int v = g_cfg.sunset_offset_min;
            std::wstring unit = Tr(L" min", L"分");
            return v == 0 ? L"0" + unit : (v > 0 ? L"+" : L"−") + std::to_wstring(std::abs(v)) + unit;
        };
        o.change = [](int dir) {
            g_cfg.sunset_offset_min = std::clamp(g_cfg.sunset_offset_min + dir * 5, -180, 180);
            Changed();
        };
        sc.opts.push_back(o);
    }
    sc.opts.push_back(TimeOf(L"night", Tr(L"Night starts at", L"夜間開始"),
                             Tr(L"Night starts at this time on the clock, whatever the sun does. Type a time, or step it by "
                                L"15 minutes. After midnight works too (00:30).",
                                L"報告：夜間は太陽の位置に関係なく、この時刻に開始。時刻の直接入力、または15分単位の調整が可能。"
                                L"深夜0時以降も指定可能（例：00:30）。"),
                             &c.night_start));
    sc.opts.push_back(Toggle(L"nightdr", Tr(L"Night uses Afterglow", L"夜間は残光"),
                             Tr(L"Afterglow at night: the whole screen turns red, by brightness, the gentlest light for a dark room. "
                                L"Off: night uses the temperature below.",
                                L"報告：夜間は残光。画面全体を輝度に応じた赤一色に変換し、暗い部屋で最も目に優しい光に。"
                                L"OFF時は下記の色温度を使用。"),
                             &c.night_darkroom));
    {
        Option o = Kelvin(L"nightk", Tr(L"Night", L"夜間"),
                          Tr(L"The night's color when it doesn't use Afterglow.", L"報告：残光不使用時の夜間色温度。"),
                          &c.night_kelvin);
        o.enabled = [] { return !g_cfg.night_darkroom; };
        sc.opts.push_back(o);
    }
    sc.opts.push_back(ChoiceOf(L"morning", Tr(L"Day starts", L"日中開始"),
                               Tr(L"When the day color comes back: at sunrise, or at a time on the clock.",
                                  L"報告：日中の色温度に戻る時点。日の出、または指定時刻から選択。"),
                               {{L"sunrise", Tr(L"Sunrise", L"日の出")}, {L"fixed", Tr(L"Fixed time", L"指定時刻")}},
                               &c.morning_mode));
    {
        Option o = TimeOf(L"morningt", Tr(L"Day starts at", L"日中開始時刻"),
                          Tr(L"The time the day color comes back, when the day starts at a fixed time.",
                             L"報告：日中開始を「指定時刻」とした場合の開始時刻。"),
                          &c.morning_time);
        o.enabled = [] { return g_cfg.morning_mode == L"fixed"; };
        sc.opts.push_back(o);
    }
    sc.opts.push_back(Coordinate(true));
    sc.opts.push_back(Coordinate(false));
    out.push_back(sc);

    // ---- warmth: fades, then Afterglow (red by brightness; "darkroom" in the code and the settings file) ----
    Section dr{L"warmth", Tr(L"Warmth", L"色温度"), {}};
    dr.opts.push_back(Toggle(L"fade", Tr(L"Fade color changes", L"色の切替をフェード"),
                             Tr(L"Color changes come on gradually instead of all at once. Scheduled changes (sunset, night, "
                                L"morning) and the ones you make (hotkeys, the tray menu, these settings) each take their own "
                                L"time, set below.",
                                L"報告：色の切替を瞬時ではなく徐々に実行。スケジュールによる切替（日没・夜間・朝）と"
                                L"手動の操作（ホットキー・トレイメニュー・本設定）で、それぞれ下記の所要時間を適用。"),
                             &c.fade));
    {
        Option o = Steps(L"fadesched", Tr(L"Scheduled fade", L"スケジュール切替の所要時間"),
                         Tr(L"How long a scheduled change takes: sunset, night and morning. A long fade, like 30 minutes, "
                            L"drifts in the way the light outside does.",
                            L"報告：スケジュールによる切替（日没・夜間・朝）の所要時間。"
                            L"30分など長めに設定すると、外の光のように緩やかに変化。"),
                         {5, 10, 20, 30, 60, 120, 300, 600, 900, 1800, 3600}, &c.fade_seconds,
                         [](int v) { return Duration(v * 1000); });
        o.enabled = [] { return g_cfg.fade; };
        dr.opts.push_back(o);
    }
    {
        Option o = Steps(L"fadehk", Tr(L"Hotkey fade", L"ホットキー操作の所要時間"),
                         Tr(L"How long a change you make takes: Warmer, Cooler, Dimmer, Brighter, Afterglow and Warmth off, "
                            L"from a hotkey, the tray menu or these settings. Instant leaves them as they were.",
                            L"報告：手動操作による切替の所要時間。対象：暖色・寒色・減光・増光・残光・色温度補正の停止"
                            L"（ホットキー、トレイメニュー、本設定から）。「即時」で従来通り瞬時に切替。"),
                         {0, 200, 400, 700, 1000, 1500, 2000, 3000, 5000}, &c.fade_hotkey_ms, Duration);
        o.enabled = [] { return g_cfg.fade; };
        dr.opts.push_back(o);
    }
    dr.opts.push_back(Heading(Tr(L"Afterglow", L"残光")));
    {
        Option o = Action(L"drnow", Tr(L"Afterglow now", L"残光を起動"),
                          Tr(L"Turns Afterglow on right away, until the next scheduled change. Press again to leave it.",
                             L"報告：次のスケジュール切替まで、残光を即時起動。再度実行で解除。"),
                          [] {
                              bool on = engine::GetStatus().Darkroom();
                              return on ? Tr(L"Leave", L"解除") : Tr(L"Start", L"起動");
                          },
                          [] { engine::Hotkey(HK_DARKROOM, true); });
        o.accent = true;
        o.enabled = [] { return !engine::Paused(); };
        dr.opts.push_back(o);
    }
    {
        Option o;
        o.key = L"drbright";
        o.label = Tr(L"Brightness", L"輝度");
        o.desc = Tr(L"Afterglow's brightness. Lower is gentler on the eyes in a dark room.",
                    L"報告：残光の輝度。推奨：暗所では低めの設定。");
        o.kind = kSlider;
        o.lo = 10;
        o.hi = 100;
        o.step = 5;
        o.get = [] { return g_cfg.darkroom_brightness * 100; };
        o.set = [](double v) {
            double b = std::clamp(std::nearbyint(v / 5) * 5, 10.0, 100.0) / 100;
            if (std::fabs(b - g_cfg.darkroom_brightness) < 1e-6) return;
            g_cfg.darkroom_brightness = b;
            Changed();
        };
        o.value = [] { return std::to_wstring((int)std::nearbyint(g_cfg.darkroom_brightness * 100)) + L"%"; };
        o.change = [o](int dir) { o.set(o.get() + dir * o.step); };
        dr.opts.push_back(o);
    }
    dr.opts.push_back(Toggle(L"readable", Tr(L"Keep all text readable", L"文字の判読性を維持"),
                             Tr(L"Turns each pixel's brightness into red, so green and blue text stays readable. Off keeps only "
                                L"the red channel (dark text on green or blue can vanish).",
                                L"報告：各ピクセルの輝度を赤に変換し、緑・青の文字も判読可能な状態を維持。"
                                L"OFF時は赤チャンネルのみ残存（緑・青地の暗い文字は消失の恐れあり）。推奨：ON。"),
                             &c.darkroom_by_brightness));
    dr.opts.push_back(Toggle(L"invert", Tr(L"Invert", L"反転"),
                             Tr(L"For a real photographic darkroom: white turns black and black turns red. It makes dark-mode apps "
                                L"brighter, so it's off by default.",
                                L"報告：写真用の暗室向け。白は黒に、黒は赤に変換。"
                                L"ダークモードのアプリが明るくなるため、初期設定はOFF。"),
                             &c.darkroom_invert));
    out.push_back(dr);

    // ---- hotkeys ----
    Section hk{L"hotkeys", Tr(L"Hotkeys", L"ホットキー"), {}};
    for (auto& f : HotkeyFields()) {
        Option o;
        o.key = L"hk" + std::to_wstring(f.id);
        o.label = HotkeyName(f.id);
        o.kind = kHotkey;
        o.hotkey = f.field;
        auto field = f.field;
        o.value = [field] { return lang::Keys(g_cfg.*field); };
        o.change = [](int) {};
        o.desc = Tr(L"Click, then press the new combination (with Ctrl, Alt or Win, so a plain key is never taken away from "
                    L"other programs). Esc cancels, Backspace removes the hotkey.",
                    L"報告：クリック後、新しいキーの組み合わせを入力。Ctrl・Alt・Winのいずれかが必須"
                    L"（単独キーで他のアプリの操作を妨げないため）。Escで中止、Backspaceで削除。");
        if (f.id == HK_TOGGLE)
            o.desc = Tr(L"Turns the warmth off (for a film, or a photo that needs true colors) until the next scheduled change, "
                        L"or back on. ",
                        L"報告：次のスケジュール切替まで色温度補正を停止（映画や写真の確認用）、または再開。") + o.desc;
        hk.opts.push_back(o);
    }
    hk.opts.push_back(Steps(L"step", Tr(L"Warmer / cooler step", L"色温度の変化幅"),
                            Tr(L"How far one press of Warmer or Cooler moves. Adjustments last until the next scheduled change; "
                               L"dimming lasts until morning.",
                               L"報告：暖色／寒色キー1回あたりの変化量。調整は次のスケジュール切替まで、減光は翌朝まで持続。"),
                            {50, 100, 200, 250, 500, 1000}, &c.warmth_step_k, [](int v) { return std::to_wstring(v) + L"K"; }));
    hk.opts.push_back(Toggle(L"cover", Tr(L"Blackout covers every window", L"ブラックアウトを最前面に固定"),
                             Tr(L"Off: blacked-out screens act like a black desktop background, and windows you bring there "
                                L"show on top. On: the black covers everything.",
                                L"報告：OFF時、ブラックアウトした画面は黒い壁紙として機能し、移動したウィンドウは手前に表示。"
                                L"ON時は全てを黒で覆う。"),
                             &c.blackout_always_on_top));
    out.push_back(hk);

    // ---- the switcher ----
    Section sw{L"switcher", Tr(L"Switcher", L"スイッチャー"), {}};
    sw.opts.push_back(Toggle(L"sw", Tr(L"Alt+Tab switcher", L"Alt+Tab スイッチャー"),
                             Tr(L"Replaces Windows' Alt+Tab with a list of your windows and a live preview of the selected one. "
                                L"Click × on a row (or press Delete) to close that window.",
                                L"報告：WindowsのAlt+Tabを当機のウィンドウ一覧に置換。選択中のウィンドウをリアルタイムでプレビュー表示。"
                                L"行の×（またはDeleteキー）でそのウィンドウを閉じる。"),
                             &c.switcher, [] { engine::KeyboardSettingsChanged(); }));
    {
        Option o = Toggle(L"dim", Tr(L"Dim the screen behind", L"背景を暗転"),
                          Tr(L"Darkens everything behind the switcher while it is open.", L"報告：スイッチャー表示中、背後の画面全体を暗くする。"),
                          &c.switcher_dim);
        o.enabled = [] { return g_cfg.switcher; };
        sw.opts.push_back(o);
    }
    {
        // The screens connected now, plus the one in the file if it's unplugged, so it shows as picked.
        std::vector<Choice> screens = {{L"active", Tr(L"Active window's", L"使用中の画面")},
                                       {L"primary", Tr(L"Primary", L"メイン画面")}};
        bool known = c.switcher_display == L"active" || c.switcher_display == L"primary";
        for (auto& d : Displays()) {
            screens.push_back({d.device, d.label});
            known = known || d.device == c.switcher_display;
        }
        if (!known) screens.push_back({c.switcher_display, c.switcher_display + Tr(L" (not connected)", L"（未接続）")});
        Option o = ChoiceOf(L"swscreen", Tr(L"Screen", L"表示する画面"),
                            Tr(L"Where the switcher opens: on the screen of the window you're using, on the primary screen, or "
                               L"always on the screen you pick here. A picked screen that isn't connected falls back to the "
                               L"primary one.",
                               L"報告：スイッチャーの表示先。使用中のウィンドウの画面、メイン画面、またはここで選択した画面に固定。"
                               L"選択した画面が未接続の場合はメイン画面に表示。"),
                            screens, &c.switcher_display);
        o.enabled = [] { return g_cfg.switcher; };
        sw.opts.push_back(o);
    }
    {
        Option o = Steps(L"delay", Tr(L"Quick-switch delay", L"即時切替の猶予"),
                         Tr(L"An Alt+Tab released sooner than this switches straight away without showing anything, like "
                            L"Windows does.",
                            L"報告：この時間より早くAlt+Tabを離した場合、何も表示せずに即座に切替。Windowsと同じ動作。"),
                         {0, 40, 80, 120, 160, 250}, &c.switcher_delay_ms,
                         [](int v) { return v ? std::to_wstring(v) + L" ms" : Tr(L"None", L"なし"); });
        o.enabled = [] { return g_cfg.switcher; };
        sw.opts.push_back(o);
    }
    {
        Option o = Toggle(L"typing", Tr(L"Window names type in", L"ウィンドウ名のタイプ演出"),
                          Tr(L"When the switcher opens, every window name types itself in, all at the same time.",
                             L"報告：スイッチャー表示時、全てのウィンドウ名を同時にタイプ入力の演出で表示。"),
                          &c.switcher_typing);
        o.enabled = [] { return g_cfg.switcher; };
        sw.opts.push_back(o);
    }
    {
        static std::wstring icons;
        icons = c.switcher_duotone_icons ? L"palette" : L"own";
        Option o = ChoiceOf(L"icons", Tr(L"App icons", L"アプリアイコン"),
                            Tr(L"Each app's icon in its own colors, or recolored to Candela's palette.",
                               L"報告：各アプリのアイコンを固有色で表示、または当機のパレットで再着色。"),
                            {{L"own", Tr(L"Own colors", L"固有色")}, {L"palette", Tr(L"Palette", L"パレット")}}, &icons,
                            [] { g_cfg.switcher_duotone_icons = icons == L"palette"; });
        o.enabled = [] { return g_cfg.switcher; };
        sw.opts.push_back(o);
    }
    {
        static std::wstring excl;
        excl = c.switcher_exclusive_fullscreen ? L"candela" : L"windows";
        Option o = ChoiceOf(L"excl", Tr(L"Exclusive fullscreen games", L"排他的フルスクリーンのゲーム"),
                            Tr(L"Nothing can be drawn over a game in exclusive fullscreen, so by default Windows handles Alt+Tab "
                               L"there. Borderless games always get Candela's switcher.",
                               L"報告：排他的フルスクリーンのゲーム上には描画不可。初期設定ではWindowsがAlt+Tabを処理。"
                               L"ボーダーレスのゲームでは常に当機のスイッチャーを使用。"),
                            {{L"windows", L"Windows"}, {L"candela", L"Candela"}}, &excl, [] {
                                g_cfg.switcher_exclusive_fullscreen = excl == L"candela";
                                engine::KeyboardSettingsChanged();
                            });
        o.enabled = [] { return g_cfg.switcher; };
        sw.opts.push_back(o);
    }
    out.push_back(sw);

    // ---- popups: the warmth one and the volume / media one, each in its own corner ----
    Section pp{L"popups", Tr(L"Popups", L"通知"), {}};
    {
        Option o = Toggle(L"showpopup", Tr(L"Warmth popup", L"色温度の通知"),
                          Tr(L"When you use a hotkey, shows the new value and how long it lasts. It never takes focus.",
                             L"報告：ホットキー使用時、変更後の値と持続期間を表示。フォーカスは奪わない。"),
                          &c.show_popup);
        o.preview = kWarmthPreview;
        pp.opts.push_back(o);
    }
    {
        Option o = Toggle(L"media", Tr(L"Volume and media popup", L"音量・メディア通知"),
                          Tr(L"Volume and media keys show the volume, or the song with its cover and where it's at, instead "
                             L"of Windows' flyout. The volume also shows when it changes any other way: a headset's "
                             L"buttons, a volume knob, Windows' slider.",
                             L"報告：音量・メディアキー使用時、Windowsのフライアウトの代わりに音量、"
                             L"または再生中の曲とカバー、再生位置を表示。ヘッドセットのボタン、音量ノブ、"
                             L"Windowsのスライダーなど、他の方法で音量が変わった際も表示。"),
                          &c.media_popup, [] { engine::KeyboardSettingsChanged(); });
        o.preview = kVolumePreview;
        pp.opts.push_back(o);
    }
    {
        Option o = Toggle(L"newsong", Tr(L"New song popup", L"曲の切替を通知"),
                          Tr(L"When the next song starts by itself in one of the apps below (Spotify always), the song card "
                             L"shows its title, artist and cover. Not over the games in your list.",
                             L"報告：下記のアプリ（Spotifyは常に対象）で次の曲が自動で再生開始した際、"
                             L"曲名・アーティスト・カバーを表示。リスト内のゲーム上には非表示。"),
                          &c.media_new_song, [] { engine::KeyboardSettingsChanged(); });
        o.enabled = [] { return g_cfg.media_popup; };
        o.preview = kMediaPreview;
        pp.opts.push_back(o);
    }
    pp.opts.push_back(SongApps());
    {
        Option o = Toggle(L"art", Tr(L"Album cover", L"アルバムカバー"),
                          Tr(L"The song's cover on the left of the card, when the app shares one.",
                             L"報告：アプリが提供する場合、曲のカバーをカードの左側に表示。"),
                          &c.media_art);
        o.enabled = [] { return g_cfg.media_popup; };
        o.preview = kMediaPreview;
        pp.opts.push_back(o);
    }
    {
        Option o = Steps(L"volstep", Tr(L"Volume step", L"音量の変化幅"),
                         Tr(L"How much one press of a volume key changes the volume. Windows uses 2%.",
                            L"報告：音量キー1回あたりの変化量。Windowsの標準は2%。"),
                         {1, 2, 4, 5, 10}, &c.volume_step, [](int v) { return std::to_wstring(v) + L"%"; });
        o.enabled = [] { return g_cfg.media_popup; };
        o.preview = kVolumePreview;
        pp.opts.push_back(o);
    }
    std::vector<Choice> corners = {{L"top-left", Tr(L"Top left", L"左上")},       {L"top-center", Tr(L"Top centre", L"中央上")},
                                   {L"top-right", Tr(L"Top right", L"右上")},     {L"bottom-left", Tr(L"Bottom left", L"左下")},
                                   {L"bottom-center", Tr(L"Bottom centre", L"中央下")}, {L"bottom-right", Tr(L"Bottom right", L"右下")}};
    {
        Option o = ChoiceOf(L"position", Tr(L"Warmth position", L"色温度の表示位置"),
                            Tr(L"Where the warmth popup appears, on the screen you are using.",
                               L"報告：使用中の画面における色温度の通知の表示位置。"),
                            corners, &c.popup_position, [] { engine::PreviewPopup(true); });
        o.preview = kWarmthPreview;
        pp.opts.push_back(o);
    }
    {
        // "Same as warmth" first, so existing setups (one shared corner) stay as they were.
        std::vector<Choice> media_corners = corners;
        media_corners.insert(media_corners.begin(), {L"same", Tr(L"Same as warmth", L"色温度と同じ")});
        Option o = ChoiceOf(L"mediapos", Tr(L"Volume position", L"音量の表示位置"),
                            Tr(L"Where the volume and media popup appears. In another corner than the warmth popup, both can "
                               L"show at once; in the same one, a new popup replaces the other.",
                               L"報告：音量・メディア通知の表示位置。色温度の通知と別の位置なら同時に表示可能。"
                               L"同じ位置の場合、新しい通知がもう一方を置換。"),
                            media_corners, &c.media_popup_position, [] { media::Preview(); });
        o.enabled = [] { return g_cfg.media_popup; };
        o.preview = kVolumePreview;
        pp.opts.push_back(o);
    }
    {
        Option o = Action(L"prevwarm", Tr(L"Try the warmth popup", L"色温度の通知を表示"),
                          Tr(L"Shows the warmth popup once, as a hotkey would.", L"報告：ホットキー使用時と同じ通知を一度だけ表示。"),
                          [] { return Tr(L"Show", L"表示"); }, [] { engine::PreviewPopup(true); });
        o.preview = kWarmthPreview;
        pp.opts.push_back(o);
    }
    {
        Option o = Action(L"prevvol", Tr(L"Try the volume popup", L"音量の通知を表示"),
                          Tr(L"Shows the volume popup once, as a volume key would, without changing the volume.",
                             L"報告：音量キー使用時と同じ通知を一度だけ表示。音量は変更しない。"),
                          [] { return Tr(L"Show", L"表示"); }, [] { media::Preview(); });
        o.preview = kVolumePreview;
        pp.opts.push_back(o);
    }
    out.push_back(pp);

    // ---- games ----
    Section gm{L"games", Tr(L"Games", L"ゲーム"), {}};
    gm.opts.push_back(List(L"games", Tr(L"No popups over", L"通知を出さないゲーム"),
                           Tr(L"While one of these is in front (or anything in exclusive fullscreen), the keys still work but "
                              L"no popup is drawn over the game: that can cost a frame or knock it out of fullscreen.",
                              L"報告：これらのゲームが前面にある間（排他的フルスクリーンも同様）、キーは有効のまま通知を非表示。"
                              L"警告：ゲーム上への描画はフレーム落ちやフルスクリーン解除の原因となる。"),
                           &core::Config::games));
    gm.opts.push_back(Toggle(L"ingames", Tr(L"Popups over games anyway", L"ゲーム中も通知を表示"),
                             Tr(L"On: popups show over the games above and exclusive fullscreen too.",
                                L"報告：ON時、上記のゲームや排他的フルスクリーン上にも通知を表示。"),
                             &c.popup_in_games));
    gm.opts.push_back(Toggle(L"release", Tr(L"Let go for anti-cheat", L"アンチチート対策の待機"),
                             Tr(L"While a game from the next list is running, even minimized to the tray, Candela stops watching "
                                L"the keyboard entirely: Windows' own Alt+Tab and volume flyout are back until it closes. "
                                L"Hotkeys and the warmth keep working.",
                                L"報告：次のリストのゲームが実行中の間（トレイへの最小化を含む）、当機はキーボードの監視を完全に停止。"
                                L"ゲーム終了まで、Windows標準のAlt+Tabと音量フライアウトが復帰。ホットキーと色温度は継続。"),
                             &c.anticheat_step_aside, [] { engine::KeyboardSettingsChanged(); }));
    {
        Option o = List(L"releases", Tr(L"Anti-cheat games", L"待機対象のゲーム"),
                        Tr(L"Games whose anti-cheat is touchy about other software watching the keyboard. If in doubt, add it.",
                           L"報告：キーボードを監視するソフトウェアに敏感なアンチチートを持つゲーム。推奨：判断に迷う場合は追加。"),
                        &core::Config::anticheat_games);
        o.enabled = [] { return g_cfg.anticheat_step_aside; };
        gm.opts.push_back(o);
    }
    out.push_back(gm);

    // ---- style ----
    Section st{L"style", Tr(L"Style", L"外観"), {}};
    {
        Option o = ChoiceOf(L"anim", Tr(L"Animation", L"演出"),
                            Tr(L"How much everything glitches: the popups, the switcher, this window and screens you black out. "
                               L"Full: heavier tearing, noise and flicker. Subtle: the same moves, faster and lighter. Off: a "
                               L"plain fade.",
                               L"報告：通知、スイッチャー、この画面、ブラックアウトの演出。フル：強い断裂・ノイズ・明滅。"
                               L"控えめ：同じ動作を短く軽く。OFF：単純なフェード。"),
                            {{L"full", Tr(L"Full", L"フル")}, {L"subtle", Tr(L"Subtle", L"控えめ")}, {L"off", Tr(L"Off", L"OFF")}},
                            &c.animation);
        o.preview = kWarmthPreview;
        st.opts.push_back(o);
    }
    {
        std::vector<Choice> palettes = {{L"warm", Tr(L"Warm", L"暖色")}, {L"grey", Tr(L"Grey", L"グレー")}};
        if (c.palette == L"custom") palettes.push_back({L"custom", Tr(L"Custom", L"カスタム")});
        Option o = ChoiceOf(L"colors", Tr(L"Colors", L"配色"),
                            Tr(L"Warm: beige text on warm charcoal. Grey: neutral. The cursors follow when they're "
                               L"applied. The settings file takes five colors of your own (palette \"custom\").",
                               L"報告：暖色：温かみのある黒地にベージュの文字。グレー：ニュートラルグレー。"
                               L"カーソルは適用時に追従。設定ファイルで独自の5色も指定可能（palette \"custom\"）。"),
                            palettes, &c.palette, [] {
                                ApplyPalette();
                                engine::StyleChanged();  // the cursors follow too (theme::Follow)
                            });
        o.swatches = true;
        st.opts.push_back(o);
    }
    if (!Japanese()) {
        std::vector<Choice> fonts = {{L"Segoe UI", L"Segoe UI"}, {L"Bahnschrift", L"Bahnschrift"}, {L"Consolas", L"Consolas"}};
        bool known = false;
        for (auto& f : fonts) known = known || f.value == c.font;
        if (!known) fonts.insert(fonts.begin(), {c.font, c.font});
        st.opts.push_back(ChoiceOf(L"font", L"Font",
                                   L"The font of every panel. Any font installed on your PC works (in the settings file); these "
                                   L"ship with Windows.",
                                   fonts, &c.font, [] { engine::StyleChanged(); }));
    } else {
        // Japanese has its own font (font_ja), so the English one stays as it was.
        std::vector<Choice> fonts;
        if (gfx::RodinInstalled()) fonts.push_back({L"rodin", JaFontName(L"rodin")});
        for (const wchar_t* f : {L"Yu Gothic UI", L"Meiryo UI", L"BIZ UDPGothic"})
            if (gfx::FontInstalled(f)) fonts.push_back({f, JaFontName(f)});
        static std::wstring ja;
        ja = gfx::JapaneseFont();
        bool known = false;
        for (auto& f : fonts) known = known || f.value == ja;
        if (!known) fonts.insert(fonts.begin(), {ja, JaFontName(ja)});
        st.opts.push_back(ChoiceOf(L"fontja", L"フォント",
                                   L"報告：全パネルの日本語フォント。ロダンはインストール時のみ選択可能。"
                                   L"その他はWindows標準搭載。",
                                   fonts, &ja, [] {
                                       g_cfg.font_ja = ja;
                                       engine::StyleChanged();
                                   }));
    }
    {
        Option o;
        o.key = L"theme";
        o.label = Tr(L"Cursors and dark mode", L"カーソルとダークモード");
        o.desc = Tr(L"Candela's cursors, in these colors, plus Windows' dark mode. Your own settings are saved first, and turning "
                    L"this off puts exactly those back. Wallpaper, accent color and sounds are left alone.",
                    L"報告：当機のカーソル（現在の配色）とWindowsのダークモードを適用。適用前の設定を保存し、"
                    L"OFF時に完全に復元。壁紙・アクセントカラー・サウンドは変更しない。");
        o.kind = kToggle;
        bool on = theme::Applied();
        o.on = [on] { return on; };
        o.value = [on] { return lang::OnOff(on); };
        o.change = [on](int) {
            if (on) theme::Restore();
            else if (!theme::Apply())
                Notice(Tr(L"The theme folder isn't next to candela.exe.", L"警告：candela.exe と同じ場所に theme フォルダーがない。"));
        };
        st.opts.push_back(o);
    }
    out.push_back(st);

    // ---- system ----
    Section sy{L"system", Tr(L"System", L"システム"), {}};
    // Both languages in the label, so whoever switched by accident can find the way back.
    sy.opts.push_back(ChoiceOf(L"language", Tr(L"Language · 言語", L"言語 · LANGUAGE"),
                               Tr(L"Candela's language: these settings, the popups, the switcher, the blackout and the tray menu.",
                                  L"報告：当機の表示言語。この設定画面、通知、スイッチャー、ブラックアウト、トレイメニューに適用。"),
                               {{L"en", L"English"}, {L"ja", L"日本語"}}, &c.language, [] { engine::StyleChanged(); }));
    {
        Option o;
        o.key = L"startup";
        o.label = Tr(L"Start with Windows", L"Windows起動時に実行");
        bool on = sys::StartupEnabled(), elsewhere = sys::StartupPointsElsewhere();
        o.desc = Tr(L"Starts Candela when you sign in, as admin, so the switcher also works over admin windows (Task Manager, "
                    L"installers). Windows asks for permission when you change this.",
                    L"報告：サインイン時に当機を管理者として起動し、管理者ウィンドウ上でもスイッチャーを動作可能にする。"
                    L"変更時にWindowsが許可を確認。");
        if (elsewhere)
            o.desc += Tr(L" Right now Windows starts a candela.exe in another folder; turning this on switches it to this one.",
                         L"警告：現在の起動対象は別フォルダーのcandela.exe。ONでこのcandela.exeに切替。");
        else if (on && sys::StartupNeedsUpdate())
            o.desc += Tr(L" Older setup: it starts as admin from a folder any program can change. Turn this off and on to fix it.",
                         L"警告：旧版の設定により、変更可能なフォルダーから管理者として起動中。OFF→ONで修正。");
        o.kind = kToggle;
        o.on = [on] { return on; };
        o.value = [on] { return lang::OnOff(on); };
        o.change = [on](int) {
            if (!sys::SetStartup(!on)) Notice(Tr(L"Windows didn't allow changing that.", L"警告：Windowsにより変更を拒否。"));
        };
        sy.opts.push_back(o);
    }
    {
        Option o;
        o.key = L"startmenu";
        o.label = Tr(L"In the Start menu", L"スタートメニューに登録");
        o.desc = Tr(L"Adds Candela to the Start menu, so you can search for it and pin it. Opening it while Candela runs opens "
                    L"these settings.",
                    L"報告：スタートメニューに当機を追加し、検索とピン留めを可能にする。実行中に開くとこの設定画面を表示。");
        o.kind = kToggle;
        bool on = sys::StartMenuExists();  // read once per rebuild, not every frame
        o.on = [on] { return on; };
        o.value = [on] { return lang::OnOff(on); };
        o.change = [on](int) {
            std::wstring err;
            if (!sys::SetStartMenu(!on, &err)) Notice(Tr(err.c_str(), L"警告：スタートメニューを変更できず。"));
        };
        sy.opts.push_back(o);
    }
    {
        // Locked: "Allow". Allowed but Windows hasn't restarted since: "Restart".
        bool pending = engine::WarmRestartPending();
        Option o = Action(
            L"warm",
            pending ? Tr(L"Restart to unlock warm colors", L"再起動して暖色域を有効化") : Tr(L"Allow warm colors", L"暖色域の許可"),
            pending ? Tr(L"Warm colors are unlocked, but Windows only uses them after a restart (Restart: Shut down and "
                         L"signing out don't count). Until then Candela goes no warmer than about 3400K, nor much dimmer than "
                         L"half. Save your work first: this restarts the PC.",
                         L"報告：暖色域は解除済み。ただしWindowsは再起動後に使用（シャットダウンやサインアウトは不可）。"
                         L"再起動まで当機は約3400K・輝度約50%が上限。推奨：作業を保存してから再起動。")
                    : Tr(L"Windows refuses anything warmer than about 3400K, or dimmer than about half, until one setting is "
                         L"changed (the same one f.lux uses); meanwhile Candela puts on the closest it allows. Two steps, "
                         L"once: click Allow (Windows asks for admin), then restart your PC.",
                         L"警告：Windowsは約3400K未満の色温度と約50%未満の減光を拒否中（当機は許可範囲内で最も近い色を適用）。"
                         L"解除にはf.luxと同じ設定を1つ変更。手順（初回のみ）：「許可」を実行（管理者の確認あり）、次にPCを再起動。"),
            [pending] { return pending ? Tr(L"Restart", L"再起動") : Tr(L"Allow", L"許可"); },
            [pending] {
                if (pending) sys::AskRestartForWarmColors(false);
                else sys::UnlockWarmColors();
                engine::Wake();
            });
        o.visible = [] { return engine::RegistryProblem(); };
        sy.opts.push_back(o);
    }
    {
        Option o;
        o.key = L"pause";
        o.label = Tr(L"Pause everything", L"全機能を一時停止");
        o.desc = Tr(L"Normal colors, and Candela lets go of the keyboard (Windows' own Alt+Tab and volume flyout), until you "
                    L"turn it back on here or in the tray menu.",
                    L"報告：再開するまで画面の色を通常に戻し、キーボードの監視も停止（Windows標準のAlt+Tabと音量表示）。"
                    L"再開はここ、またはトレイメニューから。");
        o.kind = kToggle;
        o.on = [] { return engine::Paused(); };
        o.value = [] { return lang::OnOff(engine::Paused()); };
        o.change = [](int) { engine::SetPaused(!engine::Paused()); };
        sy.opts.push_back(o);
    }
    sy.opts.push_back(Info(L"resets", Tr(L"Color resets fixed today", L"本日の色リセット修復"),
                           Tr(L"How often something (a game, the graphics driver) reset the screen's colors today, and Candela "
                              L"put them back. The log says which app was in front.",
                              L"報告：本日、ゲームやグラフィックドライバーが画面の色をリセットし、当機が復元した回数。"
                              L"前面にあったアプリはログに記録。") +
                               (engine::ResetsToday() ? Tr(L" Last: ", L" 最終：") + engine::LastReset() : L""),
                           [] { return std::to_wstring(engine::ResetsToday()); }));
    sy.opts.push_back(Action(L"file", Tr(L"Settings file", L"設定ファイル"),
                             Tr(L"Everything on these pages as a text file, plus a few rarely needed ones (the watchdog timing, "
                                L"borderless overhang, your own colors).",
                                L"報告：全設定をテキストファイルとして開く。稀に使用する項目（監視間隔、ボーダーレスのはみ出し量、"
                                L"独自の配色など）も含む。"),
                             [] { return Tr(L"Open", L"開く"); },
                             [] {
                                 // Written first only if there's something to write. A file
                                 // with a mistake stays as it is, so it can be fixed (saving
                                 // here would replace it with the settings Candela fell back to).
                                 if (ConfigDirty() || GetFileAttributesW(ConfigPath().c_str()) == INVALID_FILE_ATTRIBUTES)
                                     SaveConfigNow();
                                 sys::OpenFile(ConfigPath());
                             }));
    sy.opts.push_back(Action(L"log", Tr(L"Log folder", L"ログフォルダー"),
                             Tr(L"Opens the folder with candela.log: color resets caught, games stepped aside for, anything "
                                L"Windows refused, and crashes.",
                                L"報告：candela.logのあるフォルダーを開く。色リセットの修復、ゲームのための待機、"
                                L"Windowsに拒否された操作、クラッシュを記録。"),
                             [] { return Tr(L"Open", L"開く"); }, [] { sys::OpenFolder(AppDir()); }));
    {
        Option o = Action(L"quit", Tr(L"Quit Candela", L"当機を終了"),
                          Tr(L"Stops Candela and puts normal colors back. Click twice.",
                             L"警告：当機を停止し、画面の色を通常に復元。実行には2回のクリックが必要。"),
                          [] { return QuitArmed() ? Tr(L"Click again", L"再度クリック") : Tr(L"Quit", L"終了"); },
                          [] {
                              if (QuitArmed()) engine::Quit();
                              else g_quit_armed = NowMs();
                          });
        o.danger = true;
        sy.opts.push_back(o);
    }
    out.push_back(sy);

    // Hidden options stay out of the lists entirely.
    for (auto& s : out) s.opts.erase(std::remove_if(s.opts.begin(), s.opts.end(), [](const Option& o) { return !o.visible(); }), s.opts.end());
    return out;
}

// ---- the game lists --------------------------------------------------------------------------------
std::vector<std::wstring> ListItems(const Option& o) { return o.list ? core::SplitList(g_cfg.*(o.list)) : std::vector<std::wstring>(); }

void ListRemove(const Option& o, size_t index) {
    auto items = ListItems(o);
    if (index >= items.size()) return;
    Log(L"Removed %ls from %ls", items[index].c_str(), o.key.c_str());
    items.erase(items.begin() + (long)index);
    g_cfg.*(o.list) = core::JoinList(items);
    Changed();
    engine::KeyboardSettingsChanged();
}

void ListAdd(const Option& o, const std::wstring& exe) {
    if (!o.list || exe.empty() || core::ListHas(g_cfg.*(o.list), exe)) return;
    auto items = ListItems(o);
    items.push_back(exe);
    g_cfg.*(o.list) = core::JoinList(items);
    Log(L"Added %ls to %ls", exe.c_str(), o.key.c_str());
    Changed();
    engine::KeyboardSettingsChanged();
}

// ---- typing -------------------------------------------------------------------------------------------------
void BeginEdit(const Option& o) {
    StopRecording();
    g_edit.key = o.key;
    g_edit.buffer = o.text ? o.text() : L"";
}

bool EditChar(wchar_t c) {
    if (!g_edit.Active()) return false;
    if (c >= 0x20 && c != 0x7F && g_edit.buffer.size() < 40) g_edit.buffer += c;
    return true;
}

void EndEdit(const Option* o, bool keep) {
    if (!g_edit.Active()) return;
    if (keep && o && o->commit) {
        std::wstring err;
        if (!o->commit(g_edit.buffer, &err)) {
            Notice(err);
            g_edit.key.clear();
            return;
        }
    }
    g_edit.key.clear();
}

bool EditKey(UINT vk, const Option* o) {
    if (!g_edit.Active()) return false;
    if (vk == VK_RETURN || vk == VK_TAB) EndEdit(o, true);
    else if (vk == VK_ESCAPE) EndEdit(o, false);
    else if (vk == VK_BACK) {
        if (!g_edit.buffer.empty()) g_edit.buffer.pop_back();
    } else if (vk == 'V' && GetKeyState(VK_CONTROL) < 0) {
        if (OpenClipboard(nullptr)) {
            if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
                if (auto* p = (const wchar_t*)GlobalLock(h)) {
                    g_edit.buffer = std::wstring(p).substr(0, 60);
                    GlobalUnlock(h);
                }
            }
            CloseClipboard();
        }
    }
    return true;  // every key belongs to the field while typing
}

// ---- recording ---------------------------------------------------------------------------------------------------
void BeginRecording(const Option& o) {
    EndEdit(nullptr, false);
    g_recording = o.key;
    engine::SuspendHotkeys(true);  // so the old hotkeys don't swallow the keys
}

void StopRecording() {
    if (g_recording.empty()) return;
    g_recording.clear();
    engine::SuspendHotkeys(false);
}

bool RecordKey(UINT vk, const Option* o) {
    if (g_recording.empty()) return false;
    if (!o || !o->hotkey) {
        StopRecording();
        return true;
    }
    if (core::IsModifierVk(vk)) return true;  // wait for the real key
    auto down = [](int k) { return GetKeyState(k) < 0; };
    unsigned mods = (down(VK_CONTROL) ? core::kModCtrl : 0) | (down(VK_MENU) ? core::kModAlt : 0) |
                    (down(VK_SHIFT) ? core::kModShift : 0) | (down(VK_LWIN) || down(VK_RWIN) ? core::kModWin : 0);
    if (vk == VK_ESCAPE && !mods) {
        StopRecording();
        return true;
    }
    if (vk == VK_BACK && !mods) {
        g_cfg.*(o->hotkey) = L"";
        StopRecording();
        Changed();
        Notice(Tr(L"Hotkey removed.", L"報告：ホットキーを削除。"), false);
        return true;
    }
    std::wstring text = core::FormatHotkey(mods, vk), err;
    std::optional<core::Hotkey> parsed;
    if (!core::ParseHotkey(text, &parsed, &err)) {
        StopRecording();
        if (text.find(L"0x") != std::wstring::npos) Notice(Tr(L"That key can't be used for a hotkey.", L"警告：そのキーはホットキーに使用不可。"));
        else if (Japanese()) Notice(err.find(L"needs Ctrl") != std::wstring::npos ? L"警告：Ctrl・Alt・Winのいずれかが必要。"
                                                                                   : L"警告：そのキーの組み合わせは使用不可。");
        else Notice(err);
        return true;
    }
    for (auto& f : HotkeyFields()) {
        if (f.field == o->hotkey) continue;
        std::optional<core::Hotkey> other;
        std::wstring e;
        if (core::ParseHotkey(g_cfg.*(f.field), &other, &e) && other && other->mods == parsed->mods && other->vk == parsed->vk) {
            StopRecording();
            Notice(Japanese() ? L"警告：" + core::DisplayHotkey(text) + L" は「" + HotkeyName(f.id) + L"」に使用中。"
                              : core::DisplayHotkey(text) + L" is already used for " + f.label + L".");
            return true;
        }
    }
    g_cfg.*(o->hotkey) = text;
    StopRecording();
    Changed();
    return true;
}

}  // namespace opts
