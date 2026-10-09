// What the screen should look like right now.
//
// There is no stored day/evening/night mode that could fall out of step
// (f.lux's "stuck at sunset warmth" bug). Every tick the phase is worked out
// again from the clock and the sun, and your adjustments are remembered
// together with the phase they were made in, so they expire by themselves as
// soon as the next phase begins.
#include <algorithm>
#include <cmath>
#include <cwctype>

#include "core.h"

namespace core {

const wchar_t* Label(PhaseName p) { return p == kDay ? L"Day" : p == kEvening ? L"Evening" : L"Night"; }

int64_t Phase::Minute() const { return (int64_t)std::floor(start / 60.0); }

static HM ParseOr(const std::wstring& text, HM fallback) {
    auto t = ParseHM(text);
    return t ? *t : fallback;
}

static void EventsFor(Date d, const Config& cfg, std::vector<Phase>* out) {
    LocalTime sunrise, sunset;
    if (!LocalSunTimes(d, cfg.latitude, cfg.longitude, &sunrise, &sunset)) {  // polar day or night
        sunrise = Combine(d, {7, 0});
        sunset = Combine(d, {19, 0});
    }
    LocalTime morning = cfg.morning_mode == L"fixed" ? Combine(d, ParseOr(cfg.morning_time, {8, 0})) : sunrise;
    LocalTime evening = sunset + cfg.sunset_offset_min * 60.0;
    HM night_t = ParseOr(cfg.night_start, {22, 0});
    // A night start like 00:30 belongs to the early hours of the next day.
    Date night_day = night_t.h >= 12 ? d : AddDays(d, 1);
    LocalTime night = Combine(night_day, night_t);
    evening = std::min(evening, night);  // the night start always wins over a late sunset
    out->push_back({kDay, morning});
    out->push_back({kEvening, evening});
    out->push_back({kNight, night});
}

std::vector<Phase> Timeline(LocalTime now, const Config& cfg) {
    std::vector<Phase> events;
    Date today = DateOf(now);
    for (int offset = -1; offset <= 1; offset++) EventsFor(AddDays(today, offset), cfg, &events);
    std::stable_sort(events.begin(), events.end(), [](const Phase& a, const Phase& b) {
        if (a.start != b.start) return a.start < b.start;
        return a.name < b.name;
    });
    return events;
}

static void PickCurrentAndNext(const std::vector<Phase>& events, LocalTime now, Phase* current, std::optional<Phase>* next) {
    *current = events.front();
    next->reset();
    for (auto& p : events)
        if (p.start <= now) *current = p;
    for (auto& p : events)
        if (p.start > now) {
            *next = p;
            break;
        }
}

void CurrentAndNext(LocalTime now, const Config& cfg, Phase* current, std::optional<Phase>* next) {
    PickCurrentAndNext(Timeline(now, cfg), now, current, next);
}

const std::vector<Phase>& Controller::CachedTimeline(LocalTime now, const Config& cfg) {
    Date today = DateOf(now);
    TimelineCache& c = cache_;
    if (!(c.valid && c.date == today && c.latitude == cfg.latitude && c.longitude == cfg.longitude &&
          c.sunset_offset_min == cfg.sunset_offset_min && c.morning_mode == cfg.morning_mode &&
          c.morning_time == cfg.morning_time && c.night_start == cfg.night_start)) {
        c.events = Timeline(now, cfg);
        c.date = today;
        c.latitude = cfg.latitude;
        c.longitude = cfg.longitude;
        c.sunset_offset_min = cfg.sunset_offset_min;
        c.morning_mode = cfg.morning_mode;
        c.morning_time = cfg.morning_time;
        c.night_start = cfg.night_start;
        c.valid = true;
    }
    return c.events;
}

int ScheduledKelvin(PhaseName p, const Config& cfg) {
    int k;
    if (p == kDay) k = cfg.day_kelvin;
    else if (p == kEvening) k = cfg.evening_kelvin;
    else {
        if (cfg.night_darkroom) return kDarkroom;
        k = cfg.night_kelvin;
    }
    return std::clamp(k, kMinK, kMaxK);
}

std::wstring Status::Summary() const {
    std::wstring now;
    if (disabled) {
        now = L"Off";
    } else {
        now = Describe(kelvin) + (overridden ? L" (adjusted)" : L"");
        if (brightness < 0.999) now += L", " + std::to_wstring((int)std::nearbyint(brightness * 100)) + L"% brightness";
    }
    std::wstring text = std::wstring(Label(phase.name)) + L": " + now;
    if (next) text += std::wstring(L" | ") + Label(next->name) + L" at " + FormatHM(next->start);
    return text;
}

LocalTime Controller::LastMorning(LocalTime now, const Config& cfg) {
    std::optional<LocalTime> last;
    for (auto& p : CachedTimeline(now, cfg))
        if (p.name == kDay && p.start <= now) last = p.start;
    return last ? *last : Midnight(now);
}

Status Controller::Get(LocalTime now, const Config& cfg) {
    Status st;
    PickCurrentAndNext(CachedTimeline(now, cfg), now, &st.phase, &st.next);
    st.scheduled = ScheduledKelvin(st.phase.name, cfg);
    int kelvin = st.scheduled;
    if (override_ && override_->first.Same(st.phase)) {
        kelvin = override_->second;
        st.overridden = true;
    } else {
        override_.reset();
    }
    st.disabled = disabled_for_ && disabled_for_->Same(st.phase);
    if (!st.disabled) disabled_for_.reset();
    double brightness = 1.0;
    if (dim_ && dim_->first == LastMorning(now, cfg)) brightness = dim_->second;
    else dim_.reset();
    st.kelvin = st.disabled ? kNeutralK : kelvin;
    st.brightness = st.disabled ? 1.0 : brightness;
    return st;
}

Status Controller::Dim(LocalTime now, const Config& cfg, bool darker) {
    disabled_for_.reset();  // dimming a switched-off screen would do nothing visible
    Status st = Get(now, cfg);
    double step = cfg.dim_step > 0 ? cfg.dim_step : 0.1;
    double floor = std::min(std::max(cfg.dim_min, 0.05), 1.0);
    double level = darker ? st.brightness - step : st.brightness + step;
    level = std::nearbyint(std::max(floor, std::min(level, 1.0)) * 1000.0) / 1000.0;
    if (level >= 0.999) dim_.reset();
    else dim_ = std::make_pair(LastMorning(now, cfg), level);
    return Get(now, cfg);
}

Status Controller::Nudge(LocalTime now, const Config& cfg, bool warmer) {
    Status st = Get(now, cfg);
    if (st.disabled) {
        disabled_for_.reset();
        st = Get(now, cfg);
    }
    int k = st.kelvin, step = std::max(10, cfg.warmth_step_k), next;
    if (warmer) next = k <= kMinK ? kDarkroom : std::max(k - step, kMinK);
    else next = k <= kDarkroom ? kMinK : std::min(k + step, kMaxK);
    if (next == st.scheduled) override_.reset();
    else override_ = std::make_pair(st.phase, next);
    return Get(now, cfg);
}

Status Controller::ToggleDarkroom(LocalTime now, const Config& cfg) {
    // Darkroom on demand until the next scheduled change. Pressing again
    // leaves it: back to the schedule if it was your choice, or to 1200K if
    // the schedule itself says Darkroom.
    Status st = Get(now, cfg);
    disabled_for_.reset();
    if (st.Darkroom()) {
        if (st.overridden) override_.reset();
        else override_ = std::make_pair(st.phase, 1200);
    } else {
        if (st.scheduled <= kDarkroom) override_.reset();
        else override_ = std::make_pair(st.phase, kDarkroom);
    }
    return Get(now, cfg);
}

Status Controller::ToggleDisabled(LocalTime now, const Config& cfg) {
    Status st = Get(now, cfg);
    if (st.disabled) disabled_for_.reset();
    else disabled_for_ = st.phase;
    return Get(now, cfg);
}

void Controller::Reset() {
    override_.reset();
    disabled_for_.reset();
    dim_.reset();
}

// ---- hotkeys ----------------------------------------------------------------------------------------
namespace {
struct Key { std::wstring name, lower; unsigned vk; };
// Built once (thread-safe static), not on every parse or format.
const std::vector<Key>& Keys() {
    static const std::vector<Key> keys = [] {
        std::vector<Key> k;
        auto add = [&](const std::wstring& name, unsigned vk) {
            std::wstring low = name;
            for (auto& c : low) c = (wchar_t)towlower(c);
            k.push_back({name, low, vk});
        };
        for (auto& [name, vk] : std::vector<std::pair<const wchar_t*, unsigned>>{
                 {L"PageUp", 0x21}, {L"PageDown", 0x22}, {L"End", 0x23}, {L"Home", 0x24}, {L"Left", 0x25},
                 {L"Up", 0x26}, {L"Right", 0x27}, {L"Down", 0x28}, {L"Insert", 0x2D}, {L"Delete", 0x2E},
                 {L"Space", 0x20}, {L"Pause", 0x13}, {L"Backspace", 0x08}, {L"Minus", 0xBD}, {L"Plus", 0xBB},
                 {L"Comma", 0xBC}, {L"Period", 0xBE}})
            add(name, vk);
        for (wchar_t c = L'A'; c <= L'Z'; c++) add(std::wstring(1, c), (unsigned)c);
        for (int d = 0; d < 10; d++) add(std::to_wstring(d), 0x30u + d);
        for (int n = 1; n <= 24; n++) add(L"F" + std::to_wstring(n), 0x6Fu + n);
        for (int d = 0; d < 10; d++) add(L"Num" + std::to_wstring(d), 0x60u + d);
        return k;
    }();
    return keys;
}

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

std::wstring Strip(const std::wstring& s) {
    size_t a = 0, b = s.size();
    while (a < b && iswspace(s[a])) a++;
    while (b > a && iswspace(s[b - 1])) b--;
    return s.substr(a, b - a);
}
}  // namespace

bool ParseHotkey(const std::wstring& raw, std::optional<Hotkey>* out, std::wstring* error) {
    out->reset();
    std::wstring text = Strip(raw);
    if (text.empty()) return true;
    unsigned mods = 0;
    std::optional<unsigned> vk;
    size_t start = 0;
    const auto& keys = Keys();
    while (start <= text.size()) {
        size_t plus = text.find(L'+', start);
        if (plus == std::wstring::npos) plus = text.size();
        std::wstring part = Strip(text.substr(start, plus - start));
        start = plus + 1;
        if (part.empty()) continue;
        std::wstring low = Lower(part);
        if (low == L"ctrl" || low == L"control") mods |= kModCtrl;
        else if (low == L"alt") mods |= kModAlt;
        else if (low == L"shift") mods |= kModShift;
        else if (low == L"win") mods |= kModWin;
        else {
            std::optional<unsigned> found;
            if (low == L"pgup") found = 0x21;
            else if (low == L"pgdn") found = 0x22;
            else if (low == L"del") found = 0x2E;
            else if (low == L"ins") found = 0x2D;
            for (auto& k : keys)
                if (!found && k.lower == low) found = k.vk;
            if (found && !vk) vk = found;
            else {
                *error = L"Don't understand '" + part + L"' in '" + text + L"'";
                return false;
            }
        }
    }
    if (!vk) {
        *error = L"'" + text + L"' has no key, only modifiers";
        return false;
    }
    if (!(mods & (kModCtrl | kModAlt | kModWin))) {
        *error = L"'" + text + L"' needs Ctrl, Alt or Win, or it would block that key everywhere";
        return false;
    }
    *out = Hotkey{mods, *vk};
    return true;
}

std::wstring FormatHotkey(unsigned mods, unsigned vk) {
    std::wstring s;
    if (mods & kModCtrl) s += L"Ctrl+";
    if (mods & kModAlt) s += L"Alt+";
    if (mods & kModShift) s += L"Shift+";
    if (mods & kModWin) s += L"Win+";
    const std::wstring* name = nullptr;
    for (auto& k : Keys())
        if (k.vk == vk) name = &k.name;  // the last name for a key wins, as in Python
    if (name) return s + *name;
    wchar_t buf[8];
    swprintf(buf, 8, L"0x%02X", vk);
    return s + buf;
}

std::wstring DisplayHotkey(const std::wstring& text) {
    if (text.empty()) return L"none";
    std::wstring s = text;
    auto replace = [&](const std::wstring& a, const std::wstring& b) {
        for (size_t i = s.find(a); i != std::wstring::npos; i = s.find(a, i + b.size())) s.replace(i, a.size(), b);
    };
    replace(L"PageDown", L"PgDn");
    replace(L"PageUp", L"PgUp");
    replace(L"Delete", L"Del");
    return s;
}

bool IsModifierVk(unsigned vk) {
    static const unsigned mods[] = {0x10, 0x11, 0x12, 0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0x5B, 0x5C};
    for (unsigned m : mods)
        if (m == vk) return true;
    return false;
}

}  // namespace core
