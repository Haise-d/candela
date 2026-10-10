// Every setting, once: the settings window draws from this list, so the
// window, the settings file and the tray menu always agree.
#pragma once
#include "candela.h"

namespace opts {

enum Kind {
    kToggle,   // on / off: an ON | OFF pair
    kChoice,   // one of a few: ‹ VALUE › with a pip per choice
    kSlider,   // a number with a range (temperature, percent): a block gauge
    kStepper,  // a number stepped with ‹ and ›
    kTime,     // HH:MM, typed or stepped
    kText,     // typed (latitude, longitude): a field
    kHotkey,   // recorded by pressing it: keycaps
    kAction,   // a button
    kInfo,     // text only
    kList,     // a list of .exe names, edited in the side panel
    kHeading,  // a small title over the rows after it on the same page; never selected
};

// What the side panel's little popup shows for an option.
enum Preview { kNoPreview, kWarmthPreview, kVolumePreview, kMediaPreview };

struct Choice {
    std::wstring value, label;
};

struct Option {
    std::wstring key, label;
    std::wstring Label() const { return label; }
    std::wstring desc;  // a few sentences (the side panel)
    Kind kind = kInfo;
    std::function<std::wstring()> value;               // what it shows now
    std::function<void(int)> change;                   // +1 / -1: next / previous, flip, step, press
    std::function<bool()> enabled = [] { return true; };
    std::function<bool()> visible = [] { return true; };
    std::function<bool()> on;                          // kToggle: whether it's on
    // kChoice
    std::vector<Choice> choices;
    std::function<std::wstring()> current;
    std::function<void(const std::wstring&)> pick;
    // kSlider
    double lo = 0, hi = 1, step = 1;
    std::function<double()> get;
    std::function<void(double)> set;
    bool kelvin = false;  // the gauge is a temperature: its blocks carry the tint
    // kTime, kText: the text to edit, and taking it back (false + error if it doesn't fit)
    std::function<std::wstring()> text;
    std::function<bool(const std::wstring&, std::wstring*)> commit;
    // kHotkey
    std::wstring core::Config::*hotkey = nullptr;
    // kList: which list (g_cfg.games, g_cfg.anticheat_games or g_cfg.song_apps)
    std::wstring core::Config::*list = nullptr;
    std::vector<std::wstring> list_fixed;  // shown first, always in, no ×
    bool list_apps = false;                // its items are apps, not games (the wording)
    // looks
    bool accent = false;    // Darkroom's button carries the tint
    bool danger = false;    // Quit
    bool swatches = false;  // the palette's five colors in the side panel
    Preview preview = kNoPreview;
};

struct Section {
    std::wstring key, label;
    std::vector<Option> opts;
};

// Built fresh whenever something changed (labels and values depend on the settings).
std::vector<Section> Build();

// Something changed: apply it now and save soon.
void Changed();

// ---- typing into a field (latitude, longitude, times) ------------------------------------------
struct Editor {
    std::wstring key;     // option being edited, or empty
    std::wstring buffer;
    bool Active() const { return !key.empty(); }
};
extern Editor g_edit;
void BeginEdit(const Option& o);
bool EditChar(wchar_t c);      // WM_CHAR: true if taken
bool EditKey(UINT vk, const Option* o);  // Enter, Esc, Backspace: true if taken
void EndEdit(const Option* o, bool keep);

// ---- recording a hotkey ------------------------------------------------------------------------
extern std::wstring g_recording;  // option key being recorded, or empty
void BeginRecording(const Option& o);
// A key went down while recording. true if taken (the window shouldn't act on it).
bool RecordKey(UINT vk, const Option* o);
void StopRecording();

// ---- the game lists ----------------------------------------------------------------------------
std::vector<std::wstring> ListItems(const Option& o);
void ListRemove(const Option& o, size_t index);
void ListAdd(const Option& o, const std::wstring& exe);

// A short message after something didn't work (bad latitude, hotkey taken...).
void Notice(const std::wstring& text, bool error = true);
std::wstring CurrentNotice(bool* error);  // empty when none (they fade after a few seconds)

// Header lines.
std::wstring StatusLine();   // "Evening · 2000K"
std::wstring UntilLine();    // "until night 22:00", "adjusted until night 22:00"

// Quit asks for a second click.
bool QuitArmed();

}  // namespace opts
