// The keyboard hook: sees every key press before any app, decides in
// microseconds whether Candela takes it (Alt+Tab, media and volume keys), and
// hands the work to the main thread. It runs on its own high-priority thread
// so a busy main thread can never slow typing down: Windows silently drops a
// hook that answers too slowly.
#include "candela.h"

namespace keyboard {

std::atomic<bool> switcher_on{true}, media_on{true}, switcher_active{false};
std::atomic<bool> exclusive_on{false};  // g_cfg.switcher_exclusive_fullscreen, readable from this thread

// Off until the engine says (after it has looked for an anti-cheat game), so the
// hook is never up, even for a moment, while one is already running.
static std::atomic<bool> g_enabled{false};
static HHOOK g_hook = nullptr;
static DWORD g_thread_id = 0;
static HANDLE g_thread = nullptr;
static HANDLE g_ready = nullptr;

enum : UINT { TM_INSTALL = WM_APP + 50, TM_REMOVE, TM_REINSTALL };

static bool IsMediaKey(DWORD vk) {
    switch (vk) {
        case VK_VOLUME_MUTE: case VK_VOLUME_DOWN: case VK_VOLUME_UP:
        case VK_MEDIA_NEXT_TRACK: case VK_MEDIA_PREV_TRACK: case VK_MEDIA_STOP: case VK_MEDIA_PLAY_PAUSE:
            return true;
    }
    return false;
}

// Releasing Alt on its own makes many apps open their menu bar. A harmless
// unassigned key between Alt's press and release stops that (Windows' own
// Alt+Tab does the equivalent).
static void MaskAlt() {
    INPUT in[2] = {};
    for (auto& i : in) {
        i.type = INPUT_KEYBOARD;
        i.ki.wVk = 0xE8;  // unassigned
        i.ki.dwExtraInfo = CANDELA_INJECTED;
    }
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
}

static bool Down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

static LRESULT CALLBACK Proc(int code, WPARAM wp, LPARAM lp) {
    if (code != HC_ACTION) return CallNextHookEx(g_hook, code, wp, lp);
    auto* k = (KBDLLHOOKSTRUCT*)lp;
    if (k->dwExtraInfo == CANDELA_INJECTED) return CallNextHookEx(g_hook, code, wp, lp);
    const bool down = wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN;
    const DWORD vk = k->vkCode;

    if (vk == VK_LMENU || vk == VK_RMENU || vk == VK_MENU) {
        if (!down && switcher_active.exchange(false)) PostMessageW(g_main, WM_CANDELA_SW_COMMIT, 0, 0);
        return CallNextHookEx(g_hook, code, wp, lp);
    }

    if (switcher_on) {
        // Alt is held if Windows says so, if Tab arrives as a "system" key
        // (which only happens with Alt down), or if Alt reads as down now.
        const bool alt = (k->flags & LLKHF_ALTDOWN) || wp == WM_SYSKEYDOWN || wp == WM_SYSKEYUP || Down(VK_MENU);
        if (vk == VK_TAB && alt && !Down(VK_CONTROL) && !Down(VK_LWIN) && !Down(VK_RWIN)) {
            const bool active = switcher_active.load();
            // Exclusive-fullscreen games: nothing can draw over them, so let
            // Windows switch (unless the settings say otherwise).
            if (!active && g_fg_exclusive && !exclusive_on) return CallNextHookEx(g_hook, code, wp, lp);
            if (down) {
                if (!active) {
                    switcher_active = true;
                    MaskAlt();
                }
                PostMessageW(g_main, WM_CANDELA_SWITCH, Down(VK_SHIFT) ? (WPARAM)-1 : (WPARAM)1, 0);
            }
            return 1;  // Windows never sees this Tab
        }
        if (switcher_active) {
            switch (vk) {
                case VK_ESCAPE: case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_RETURN:
                case VK_HOME: case VK_END: case VK_DELETE:
                    if (down) {
                        if (vk == VK_ESCAPE) switcher_active = false;
                        PostMessageW(g_main, WM_CANDELA_SW_KEY, vk, 0);
                    }
                    return 1;
            }
        }
    }

    if (media_on && IsMediaKey(vk)) {
        if (down) PostMessageW(g_main, WM_CANDELA_MEDIA, vk, 0);
        return 1;  // so Windows' own volume flyout never appears
    }
    return CallNextHookEx(g_hook, code, wp, lp);
}

static void Install() {
    if (!g_hook) {
        g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, Proc, GetModuleHandleW(nullptr), 0);
        if (!g_hook) Log(L"Keyboard hook failed (error %lu)", GetLastError());
        else Log(L"Keyboard hook on");
    }
}

static void Remove() {
    if (g_hook) {
        UnhookWindowsHookEx(g_hook);
        g_hook = nullptr;
        Log(L"Keyboard hook off");
    }
    if (switcher_active.exchange(false)) PostMessageW(g_main, WM_CANDELA_SW_KEY, VK_ESCAPE, 0);
}

static DWORD WINAPI Thread(LPVOID) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    MSG msg;
    PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE);  // make the message queue
    SetEvent(g_ready);
    if (g_enabled) Install();
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == TM_INSTALL) Install();
        else if (msg.message == TM_REMOVE) Remove();
        else if (msg.message == TM_REINSTALL) {
            if (g_enabled) {
                Remove();
                Install();
            }
        }
        else DispatchMessageW(&msg);
    }
    Remove();
    return 0;
}

void Start() {
    g_ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_thread = CreateThread(nullptr, 0, Thread, nullptr, 0, &g_thread_id);
    if (g_thread) WaitForSingleObject(g_ready, 2000);
}

void Stop() {
    if (g_thread) {
        PostThreadMessageW(g_thread_id, WM_QUIT, 0, 0);
        WaitForSingleObject(g_thread, 2000);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
    if (g_ready) CloseHandle(g_ready);
    g_ready = nullptr;
}

void SetEnabled(bool on) {
    if (g_enabled.exchange(on) == on) return;
    PostThreadMessageW(g_thread_id, on ? TM_INSTALL : TM_REMOVE, 0, 0);
}

// Windows drops a keyboard hook without telling anyone if it ever answers too
// slowly (a sleep, a lock screen, a stalled PC can do that). Putting it back
// after those is cheap and always safe.
void Reinstall() {
    if (g_enabled && g_thread_id) PostThreadMessageW(g_thread_id, TM_REINSTALL, 0, 0);
}

bool Enabled() { return g_enabled; }

}  // namespace keyboard
