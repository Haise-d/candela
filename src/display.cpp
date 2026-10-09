// Setting the colors: gamma ramps on every monitor (and noticing when
// something resets them), plus Windows' full-screen color filter for Darkroom.
#include <algorithm>
#include <cmath>
#include <cstring>

#include "candela.h"

// ---- gamma ramps ------------------------------------------------------------------------------------
// The watchdog idea: after setting a ramp, read it back and remember exactly
// what the driver reports. Later, a cheap read-back that differs means
// Windows, the graphics driver or another program changed the colors, and
// ours go straight back.
//
// Two watchers share this: the main thread's timer (every watchdog_ms), and
// for a few seconds after a window switch, display change or wake-up, a
// thread of its own that checks every few milliseconds on a precise timer.
// A normal window timer only fires every 15.6 ms at best (often 31 ms), and
// only when the main thread isn't busy drawing; the fast watch doesn't wait
// for either, so a reset is undone before the screen shows it for long.
namespace gamma {

// Windows refuses ramps far from normal colors until warm colors are unlocked
// (and Windows restarted). It doesn't clamp a refused ramp, it just keeps the
// old one on screen, so a display that refuses gets the closest ramp it does
// take, on the way from normal colors to the one asked for ("limited"). Now
// and then the whole ramp is tried again, in case the refusal was only for a
// moment.
namespace {
struct Display {
    std::wstring name;
    HDC hdc;
    WORD ramp[3][256];          // what this display should show: g_ramp, or the closest it takes
    WORD expected[3][256];
    bool has_expected = false;
    bool readback_ok = true;
    bool ok = false;            // the last ramp sent was taken
    bool limited = false;       // `ramp` is only part of the way to g_ramp
    double limited_t = 1;
    double next_full_try = 0;   // when to try the whole ramp again
    int full_tries = 0;
    double last_blind_set = 0;
    int refused_in_a_row = 0;  // consecutive refusals, for backing off
    // The last fit: the ramp it was for, how far Windows let it go, and how far
    // that is in levels (which says roughly how far the next ramp can go).
    bool has_fit = false;
    WORD fit_for[3][256];
    double fit_t = 0, fit_reach = 0;
};
std::vector<Display> g_displays;
WORD g_ramp[3][256];
bool g_has_ramp = false;

// Everything above is shared with the fast watch thread.
CRITICAL_SECTION g_lock;
struct LockInit { LockInit() { InitializeCriticalSection(&g_lock); } } g_lock_init;
struct Guard {
    Guard() { EnterCriticalSection(&g_lock); }
    ~Guard() { LeaveCriticalSection(&g_lock); }
};

double Seconds() { return NowMs() / 1000.0; }

bool Set(Display& d) {
    if (!g_has_ramp) return true;
    // Stamp the attempt first: a refused attempt counts as a try, or every
    // check (the fast watch runs ~250 a second) would retry it again at once.
    d.last_blind_set = Seconds();
    if (!SetDeviceGammaRamp(d.hdc, d.ramp)) {
        d.has_expected = false;
        d.ok = false;
        if (d.refused_in_a_row < 100) d.refused_in_a_row++;
        return false;
    }
    d.ok = true;
    d.refused_in_a_row = 0;
    if (GetDeviceGammaRamp(d.hdc, d.expected)) {
        d.has_expected = true;
        d.readback_ok = true;
    } else {
        d.has_expected = false;
        d.readback_ok = false;
    }
    return true;
}

// The display refused g_ramp: put on the closest ramp it takes. Returns how
// far along the way it got (0: normal colors), or -1 if it takes nothing.
//
// Each try is a call to the driver (some wait for the next frame), so the
// same ramp refused again reuses the last answer, and a new one starts from
// a guess worked out from the last answer: usually two tries, not eight.
double FitLocked(Display& d) {
    core::Ramp from = core::IdentityRamp(), to;
    memcpy(to.ch, g_ramp, sizeof(to.ch));
    if (!memcmp(from.ch, to.ch, sizeof(to.ch))) return -1;  // normal colors refused: nothing nearer to try
    auto accepts = [&](const core::Ramp& r) { return SetDeviceGammaRamp(d.hdc, (LPVOID)r.ch) != FALSE; };
    for (int attempt = 0; attempt < 2; attempt++) {
        double t;
        bool same = d.has_fit && !memcmp(d.fit_for, g_ramp, sizeof(d.fit_for));
        if (same && attempt == 0) {
            t = d.fit_t;
        } else if (!core::Approachable(to)) {
            t = 0;  // part of the way would be a flat screen (inverted Afterglow): normal colors instead
        } else {
            double dev = core::MaxDeviation(from, to);
            double guess = d.has_fit && dev > 0 ? d.fit_reach / dev : -1;
            t = core::ClosestAccepted(from, to, accepts, guess);
            d.has_fit = true;
            memcpy(d.fit_for, g_ramp, sizeof(d.fit_for));
            d.fit_t = t;
            if (t > 0) d.fit_reach = t * dev;
        }
        core::Ramp best = core::Blend(from, to, t);
        memcpy(d.ramp, best.ch, sizeof(d.ramp));
        if (Set(d)) {
            d.limited = true;
            d.limited_t = t;
            d.full_tries = 0;
            d.next_full_try = Seconds() + 1;
            return t;
        }
        if (!same) break;  // a fresh answer refused: not even normal colors
        // The remembered answer refused (the display changed?): work it out again.
    }
    memcpy(d.ramp, g_ramp, sizeof(d.ramp));
    d.limited = false;
    d.limited_t = 1;
    return -1;
}

void CloseLocked() {
    for (auto& d : g_displays) DeleteDC(d.hdc);
    g_displays.clear();
}

std::vector<std::wstring> CheckLocked(double force_every_s) {
    std::vector<std::wstring> repaired;
    double now = Seconds();
    WORD scratch[3][256];
    for (auto& d : g_displays) {
        if (d.readback_ok && d.has_expected) {
            bool ok = GetDeviceGammaRamp(d.hdc, scratch);
            bool drifted = !ok || memcmp(scratch, d.expected, sizeof(scratch)) != 0;
            bool blind_due = force_every_s > 0 && now - d.last_blind_set >= force_every_s;
            if (drifted) {
                if (Set(d)) repaired.push_back(d.name);
            } else if (blind_due) {
                Set(d);
            }
        } else {
            // Can't read this display back, or it refused: re-send once a
            // second. After refusals, wait longer each time (1, 2, 4... up to 30 s).
            double every = std::max(force_every_s > 0 ? force_every_s : 1.0, 0.05);
            if (d.refused_in_a_row > 0)
                every = std::max(every, std::min(30.0, (double)(1 << std::min(d.refused_in_a_row - 1, 5))));
            // Refused again: the closest ramp it takes, if any.
            if (now - d.last_blind_set >= every && !Set(d)) FitLocked(d);
        }
        // Held back by Windows' limit: try the whole ramp again now and then
        // (1, 2, 4... up to 30 s apart). Once Windows takes it, it stays.
        if (d.limited && d.ok && now >= d.next_full_try) {
            if (d.full_tries < 100) d.full_tries++;
            d.next_full_try = now + std::min(30.0, (double)(1 << std::min(d.full_tries - 1, 5)));
            if (SetDeviceGammaRamp(d.hdc, g_ramp)) {
                memcpy(d.ramp, g_ramp, sizeof(d.ramp));
                d.limited = false;
                d.limited_t = 1;
                Set(d);  // the read-back the watchdog compares against
            }
        }
    }
    return repaired;
}

// ---- the fast watch ----
HANDLE g_fast_thread = nullptr, g_fast_wake = nullptr;
volatile LONG g_fast_quit = 0;
double g_fast_until = 0;  // guarded by g_lock
int g_fast_interval = 4;
HWND g_fast_notify = nullptr;
std::vector<std::wstring> g_fast_repaired;  // guarded by g_lock

DWORD WINAPI FastThread(LPVOID) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
    // A precise timer (Windows 10 1803 and later); a plain one elsewhere.
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    while (!g_fast_quit) {
        WaitForSingleObject(g_fast_wake, INFINITE);  // until a burst starts
        while (!g_fast_quit) {
            int interval;
            {
                Guard g;
                if (Seconds() > g_fast_until) break;
                auto fixed = CheckLocked(0);
                if (!fixed.empty()) {
                    g_fast_repaired.insert(g_fast_repaired.end(), fixed.begin(), fixed.end());
                    if (g_fast_notify) PostMessageW(g_fast_notify, WM_CANDELA_REPAIRED, 0, 0);
                }
                interval = g_fast_interval;
            }
            if (timer) {
                LARGE_INTEGER due;
                due.QuadPart = -(LONGLONG)interval * 10000;  // relative, 100 ns units
                if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, 100);
                else Sleep(interval);
            } else {
                Sleep(interval);
            }
        }
    }
    if (timer) CloseHandle(timer);
    return 0;
}
}  // namespace

void Close() {
    Guard g;
    CloseLocked();
}

void RefreshDisplays() {
    Guard g;
    CloseLocked();
    DISPLAY_DEVICEW dev = {sizeof(dev)};
    std::wstring names;
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dev, 0); i++) {
        if (dev.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) {
            HDC hdc = CreateDCW(dev.DeviceName, nullptr, nullptr, nullptr);
            if (hdc) {
                Display d;
                d.name = dev.DeviceName;
                d.hdc = hdc;
                memcpy(d.ramp, g_ramp, sizeof(d.ramp));
                g_displays.push_back(d);
                names += (names.empty() ? L"" : L", ") + d.name;
            } else {
                Log(L"Could not open display %ls", dev.DeviceName);
            }
        }
        dev = {sizeof(dev)};
    }
    Log(L"Displays: %ls", names.empty() ? L"none" : names.c_str());
}

ApplyResult Apply(const core::Ramp& ramp) {
    Guard g;
    memcpy(g_ramp, ramp.ch, sizeof(g_ramp));
    g_has_ramp = true;
    ApplyResult res;
    for (auto& d : g_displays) {
        memcpy(d.ramp, g_ramp, sizeof(d.ramp));
        d.limited = false;
        d.limited_t = 1;
        if (Set(d)) {
            res.full++;
            continue;
        }
        double t = FitLocked(d);
        if (t < 0) {
            res.refused.push_back(d.name);
        } else {
            res.limited.push_back(d.name);
            res.t = std::min(res.t, t);
        }
    }
    return res;
}

std::vector<std::wstring> CheckAndRepair(double force_every_s) {
    Guard g;
    return CheckLocked(force_every_s);
}

bool AnyRefusing() {
    Guard g;
    for (auto& d : g_displays)
        if (d.refused_in_a_row > 0) return true;
    return false;
}

State GetState() {
    Guard g;
    State s;
    for (auto& d : g_displays) {
        if (!d.ok) {
            s.refused++;
        } else if (d.limited) {
            s.limited++;
            s.t = std::min(s.t, d.limited_t);
        } else {
            s.full++;
        }
    }
    return s;
}

void FastWatch(double seconds, int interval_ms, HWND notify) {
    if (interval_ms <= 0) return;
    {
        Guard g;
        g_fast_until = std::max(g_fast_until, Seconds() + seconds);
        g_fast_interval = std::clamp(interval_ms, 1, 50);
        g_fast_notify = notify;
    }
    if (!g_fast_thread) {
        InterlockedExchange(&g_fast_quit, 0);
        g_fast_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        g_fast_thread = CreateThread(nullptr, 0, FastThread, nullptr, 0, nullptr);
        if (!g_fast_thread) Log(L"The fast color check couldn't start (error %lu)", GetLastError());
    }
    if (g_fast_wake) SetEvent(g_fast_wake);
}

std::vector<std::wstring> TakeFastRepairs() {
    Guard g;
    std::vector<std::wstring> out;
    out.swap(g_fast_repaired);
    return out;
}

void StopFastWatch() {
    if (!g_fast_thread) return;
    InterlockedExchange(&g_fast_quit, 1);
    SetEvent(g_fast_wake);
    WaitForSingleObject(g_fast_thread, 2000);
    CloseHandle(g_fast_thread);
    CloseHandle(g_fast_wake);
    g_fast_thread = g_fast_wake = nullptr;
}

}  // namespace gamma

// ---- the color filter --------------------------------------------------------------------------------
// A gamma ramp can only scale red, green and blue separately: it can't turn
// green or blue light into red, so "pure red" by ramp alone blacks out text
// drawn in green or blue. This filter goes first and turns every pixel into
// its brightness, as red (like f.lux's Darkroom), so everything keeps its
// contrast. It's Windows' Magnification API, the mechanism behind Windows'
// own color filters: no admin rights, and Windows removes it by itself when
// Candela exits or crashes. With it on, a driver resetting the ramp leaves the
// screen red rather than flashing full color.
namespace colorfx {

namespace {
struct MagEffect { float transform[25]; };
using InitFn = BOOL(WINAPI*)();
using SetFn = BOOL(WINAPI*)(MagEffect*);
InitFn g_init = nullptr, g_uninit = nullptr;
SetFn g_set = nullptr, g_get = nullptr;
bool g_available = false, g_on = false, g_verifiable = true;
MagEffect g_want;
// Windows refused the filter: keep what we want, try again later (1, 2, 4...
// up to 30 s apart) rather than on every check.
bool g_pending = false;
int g_refused = 0;
double g_retry_at = 0;

double Now() { return NowMs() / 1000.0; }

void Refused() {
    if (g_refused < 100) g_refused++;
    g_retry_at = Now() + std::min(30.0, (double)(1 << std::min(g_refused - 1, 5)));
}

bool IsApplied() {
    MagEffect cur;
    if (!g_get(&cur)) return true;  // can't tell; assume fine rather than re-applying forever
    for (int i = 0; i < 25; i++)
        if (std::fabs(cur.transform[i] - g_want.transform[i]) >= 1e-4f) return false;
    return true;
}
}  // namespace

bool Init() {
    // Wine's magnification.dll aborts on MagInitialize (only matters for testing on Linux).
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    bool wine = ntdll && GetProcAddress(ntdll, "wine_get_version");
    HMODULE mag = wine ? nullptr : LoadLibraryExW(L"magnification.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (mag) {
        g_init = (InitFn)(void*)GetProcAddress(mag, "MagInitialize");
        g_uninit = (InitFn)(void*)GetProcAddress(mag, "MagUninitialize");
        g_set = (SetFn)(void*)GetProcAddress(mag, "MagSetFullscreenColorEffect");
        g_get = (SetFn)(void*)GetProcAddress(mag, "MagGetFullscreenColorEffect");
    }
    g_available = g_init && g_uninit && g_set && g_get && g_init();
    core::Matrix id = core::IdentityMatrix();
    memcpy(g_want.transform, id.m, sizeof(id.m));
    if (!g_available) Log(L"Windows color filter unavailable; Afterglow will use red only");
    return g_available;
}

bool Available() { return g_available; }
bool Pending() { return g_pending; }

bool Set(const core::Matrix& m, bool identity) {
    if (!g_available) return false;
    MagEffect e;
    memcpy(e.transform, m.m, sizeof(m.m));
    g_want = e;  // what we want, even if Windows says no right now
    g_on = !identity;
    if (!g_set(&e)) {
        if (g_refused == 0) Log(L"Windows refused the color filter (is Windows Magnifier or a color filter on?)");
        g_pending = true;
        Refused();
        return false;
    }
    g_pending = false;
    g_refused = 0;
    return true;
}

bool CheckAndRepair() {
    if (!(g_available && (g_on || g_pending) && g_verifiable)) return false;
    if (g_pending || !IsApplied()) {
        if (Now() < g_retry_at) return false;
        MagEffect e = g_want;
        if (!g_set(&e)) {
            Refused();
            return false;
        }
        g_refused = 0;
        if (g_pending) {  // the filter we couldn't set earlier is on now: not a reset by someone else
            g_pending = false;
            Log(L"Windows accepted the color filter after all");
            return false;
        }
        if (!IsApplied()) {
            g_verifiable = false;
            Log(L"Windows doesn't report the color filter back; no longer checking it");
            return false;
        }
        return true;
    }
    return false;
}

void Close() {
    if (!g_available) return;
    core::Matrix id = core::IdentityMatrix();
    MagEffect e;
    memcpy(e.transform, id.m, sizeof(id.m));
    g_set(&e);
    g_uninit();
    g_available = false;
}

}  // namespace colorfx
