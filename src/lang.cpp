// Candela in Japanese: the texts that are built from the schedule's state
// (phases, temperatures, "until ..."), shared by the settings window, the
// popup and the tray. Everything else sits next to its English with Tr().
//
// The Japanese reads like a terse system log: kanji for what the system
// reports (日中, 夕刻, 夜間, 輝度), katakana for names (スイッチャー), ON / OFF
// in capitals, and "報告：" / "警告：" in front of what it tells you.
#include "candela.h"

#include <cmath>
#include <cwctype>

bool Japanese() { return g_cfg.language == L"ja"; }

namespace lang {

static std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

std::wstring Phase(core::PhaseName p) {
    if (!Japanese()) return core::Label(p);
    return p == core::kDay ? L"日中" : p == core::kEvening ? L"夕刻" : L"夜間";
}

std::wstring Kelvin(int kelvin) {
    if (Japanese() && kelvin <= core::kDarkroom) return L"残光";
    return core::Describe(kelvin);
}

std::wstring Until(const core::Status& st) {
    if (!st.next) return L"";
    if (Japanese()) return Phase(st.next->name) + L" " + core::FormatHM(st.next->start) + L" まで";
    return L"until " + Lower(core::Label(st.next->name)) + L" " + core::FormatHM(st.next->start);
}

std::wstring Summary(const core::Status& st) {
    if (!Japanese()) return st.Summary();
    std::wstring now;
    if (st.disabled) {
        now = L"OFF";
    } else {
        now = Kelvin(st.kelvin) + (st.overridden ? L"（調整中）" : L"");
        if (st.brightness < 0.999) now += L"、輝度 " + std::to_wstring((int)std::nearbyint(st.brightness * 100)) + L"%";
    }
    std::wstring text = Phase(st.phase.name) + L"：" + now;
    if (st.next) text += L" ｜ " + Phase(st.next->name) + L" " + core::FormatHM(st.next->start) + L"〜";
    return text;
}

std::wstring Keys(const std::wstring& hotkey) {
    if (Japanese() && hotkey.empty()) return L"なし";
    return core::DisplayHotkey(hotkey);
}

std::wstring OnOff(bool on) {
    if (Japanese()) return on ? L"ON" : L"OFF";
    return on ? L"On" : L"Off";
}

}  // namespace lang
