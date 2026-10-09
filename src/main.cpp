// Candela: screen warmth for Windows, with an Alt+Tab switcher and a volume
// and media popup to match.
//
//   candela.exe              run it (tray icon)
//   candela.exe --settings   run it and open the settings (the Start menu shortcut does this)
//   candela.exe --schedule   show the next few days' switch times
//   candela.exe --reset      put normal colors back, then exit
//   candela.exe --do <what>  the same as pressing a hotkey, in the running Candela
#include "options.h"
#include "render.h"

#include <shobjidl.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>

// If Candela ever crashes, the log says where, so it can be found and fixed.
static LONG WINAPI OnCrash(EXCEPTION_POINTERS* ep) {
    // Whatever happened, don't leave the screen red: that first, logging after.
    gamma::Apply(core::IdentityRamp());
    void* addr = ep->ExceptionRecord->ExceptionAddress;
    HMODULE mod = nullptr;
    wchar_t name[MAX_PATH] = L"?";
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)addr, &mod))
        GetModuleFileNameW(mod, name, MAX_PATH);
    const wchar_t* base = wcsrchr(name, L'\\');
    Log(L"CRASH: exception 0x%08lx at %ls+0x%llx (thread %lu)", ep->ExceptionRecord->ExceptionCode, base ? base + 1 : name,
        (unsigned long long)((char*)addr - (char*)mod), GetCurrentThreadId());
    return EXCEPTION_CONTINUE_SEARCH;
}

static void OnAbort(int) {
    gamma::Apply(core::IdentityRamp());
    Log(L"CRASH: aborted");
}
static void OnTerminate() {
    OnAbort(0);
    std::abort();
}

static std::wstring ScheduleText() {
    const core::Config& cfg = g_cfg;
    core::LocalTime now = NowLocal();
    wchar_t line[256];
    swprintf(line, 256, L"Location %.4f, %.4f\n", cfg.latitude, cfg.longitude);
    std::wstring out = line;
    std::vector<core::Phase> seen;
    static const wchar_t* days[] = {L"Mon", L"Tue", L"Wed", L"Thu", L"Fri", L"Sat", L"Sun"};
    static const wchar_t* months[] = {L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun", L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec"};
    for (int offset = 0; offset < 4; offset++)
        for (auto& p : core::Timeline(now + offset * 86400.0, cfg)) {
            if (p.start < now - 12 * 3600.0) continue;
            bool dup = false;
            for (auto& s : seen) dup = dup || s.Same(p);
            if (dup) continue;
            seen.push_back(p);
            core::Date d = core::DateOf(p.start);
            swprintf(line, 256, L"%ls %02d %ls %ls  %-8ls %ls\n", days[core::Weekday(d)], d.d, months[d.m - 1],
                     core::FormatHM(p.start).c_str(), core::Label(p.name), core::Describe(core::ScheduledKelvin(p.name, cfg)).c_str());
            out += line;
        }
    return out;
}

// Prints to the console Candela was started from, or shows a message box.
static void Say(const std::wstring& text, const wchar_t* title) {
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
        std::string utf8 = "\n" + core::ToUtf8(text);
        DWORD written;
        WriteFile(out, utf8.data(), (DWORD)utf8.size(), &written, nullptr);
        FreeConsole();
    } else {
        MessageBoxW(nullptr, text.c_str(), title, MB_OK);
    }
}

static void DumpFrames(const std::wstring& dir);

// ---- the guard --------------------------------------------------------------------------------------
// Ended from outside (Task Manager's End task), Candela gets no chance to put
// the colors back, and the ramp it set stays on screen. So a second, windowless
// candela.exe waits for it to end, and if it didn't end by quitting (exit code
// 0), puts normal colors back. Darkroom's color filter needs nothing: Windows
// removes it with the process that set it.
//
// The guard is started through a go-between that exits at once, so it isn't
// Candela's child: Task Manager groups an app's children under it, and End
// task on the group would end the guard too. Candela's own process handle is
// handed down by inheritance (the same value in every process), so the guard
// never has to look it up by its id.
static bool StartProcess(const std::wstring& args) {
    std::wstring cmd = L"\"" + ExePath() + L"\" " + args;
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(ExePath().c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

static void StartGuard() {
    HANDLE self = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &self,
                         SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, TRUE, 0)) {
        Log(L"Guard not started (error %lu): ending Candela from Task Manager would leave its colors on", GetLastError());
        return;
    }
    if (!StartProcess(L"--guard-start " + std::to_wstring((unsigned long long)(uintptr_t)self)))
        Log(L"Guard not started (error %lu): ending Candela from Task Manager would leave its colors on", GetLastError());
    CloseHandle(self);  // the copies the go-between and the guard inherited stay open
}

static HANDLE HandleArg(const wchar_t* text) { return (HANDLE)(uintptr_t)wcstoull(text, nullptr, 10); }

// The go-between: starts the guard with the handle it inherited, and exits.
static int GuardStart(const wchar_t* handle) { return StartProcess(L"--guard " + std::wstring(handle)) ? 0 : 1; }

static int Guard(HANDLE candela) {
    SetPriorityClass(GetCurrentProcess(), IDLE_PRIORITY_CLASS);
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);  // it only waits: give the memory back
    if (WaitForSingleObject(candela, INFINITE) != WAIT_OBJECT_0) return 1;
    DWORD code = 0;
    if (!GetExitCodeProcess(candela, &code) || code == 0) return 0;  // quit normally: it put the colors back itself
    SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS);
    gamma::RefreshDisplays();
    gamma::Apply(core::IdentityRamp());
    gamma::Close();
    Log(L"Candela was ended from outside (exit code %lu); the guard put normal colors back", code);
    return 0;
}

static bool NoTownYet() { return g_cfg.latitude == core::kPlaceholderLat && g_cfg.longitude == core::kPlaceholderLon; }

// Where the settings open the first time: on the location while it's
// Greenwich's (so the sun times are yours), else where they'd open anyway.
static void FirstSettings() {
    if (NoTownYet()) settings::OpenOn(L"lat");
    else settings::Open();
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    g_inst = inst;
    SetUnhandledExceptionFilter(OnCrash);
    signal(SIGABRT, OnAbort);
    std::set_terminate(OnTerminate);
    int argc = 0;
    LPWSTR* argv_raw = CommandLineToArgvW(GetCommandLineW(), &argc);
    // Freed on every way out of here, early returns included.
    std::unique_ptr<LPWSTR, decltype(&LocalFree)> argv_owner(argv_raw, LocalFree);
    LPWSTR* argv = argv_raw;
    if (!argv) argc = 0;
    auto has = [&](const wchar_t* flag) {
        for (int i = 1; i < argc; i++)
            if (!wcscmp(argv[i], flag)) return true;
        return false;
    };
    // candela.exe --admin <what>: Candela started again as admin, for the one
    // thing that needs it (Windows asked first). Does it and exits.
    // The guard and its go-between (see StartGuard).
    if (argc == 3 && !wcscmp(argv[1], L"--guard-start")) return GuardStart(argv[2]);
    if (argc == 3 && !wcscmp(argv[1], L"--guard")) return Guard(HandleArg(argv[2]));
    for (int i = 1; i + 1 < argc; i++)
        if (!wcscmp(argv[i], L"--admin")) {
            CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            int code = sys::RunAdmin(argv[i + 1], i + 2 < argc ? argv[i + 2] : L"");
            CoUninitialize();
            return code;
        }
    std::wstring dump_dir;
    for (int i = 1; i + 1 < argc; i++)
        if (!wcscmp(argv[i], L"--dump")) dump_dir = argv[i + 1];

    // candela.exe --do blackout: the same as pressing that hotkey (for scripts,
    // macro keys, and testing where hotkeys can't be pressed).
    int action = 0;
    for (int i = 1; i < argc; i++)
        if (!wcscmp(argv[i], L"--do")) {
            if (i + 1 >= argc) {  // --do with nothing after it
                Say(L"--do needs an action: warmer, cooler, off, afterglow, dimmer, brighter, borderless, blackout.\n", L"Candela");
                return 1;
            }
            static const std::pair<const wchar_t*, HotkeyId> names[] = {
                {L"warmer", HK_WARMER},   {L"cooler", HK_COOLER},         {L"off", HK_TOGGLE},
                {L"afterglow", HK_DARKROOM}, {L"darkroom", HK_DARKROOM}, {L"dimmer", HK_DIMMER},       {L"brighter", HK_BRIGHTER},
                {L"borderless", HK_BORDERLESS}, {L"blackout", HK_BLACKOUT}};
            for (auto& [name, id] : names)
                if (!_wcsicmp(argv[i + 1], name)) action = id;
            if (!action) {
                Say(L"Unknown action. Use one of: warmer, cooler, off, afterglow, dimmer, brighter, borderless, blackout.\n",
                    L"Candela");
                return 1;
            }
        }

    // --reset works even when Candela is running (or hung): a running one is
    // closed first, or it would put its colors straight back.
    if (has(L"--reset")) {
        bool was_running = false, closed = true;
        if (HWND other = FindWindowW(CANDELA_MAIN_CLASS, nullptr)) {
            was_running = true;
            DWORD pid = 0;
            GetWindowThreadProcessId(other, &pid);
            // Waiting on a process is allowed even when it runs as admin; ending
            // one isn't, so the two are asked for separately.
            HANDLE wait = pid ? OpenProcess(SYNCHRONIZE, FALSE, pid) : nullptr;
            // Its own quit message: Windows lets that one through to a Candela
            // running as admin (WM_CLOSE from here would be dropped).
            PostMessageW(other, WM_CANDELA_QUIT, 0, 0);
            PostMessageW(other, WM_CLOSE, 0, 0);  // and the old way, for a Candela from before that message
            if (wait) closed = WaitForSingleObject(wait, 4000) == WAIT_OBJECT_0;
            else {
                for (int i = 0; i < 80 && IsWindow(other); i++) Sleep(50);
                closed = !IsWindow(other);
            }
            if (!closed) {  // hung: end it (Windows removes its color filter itself)
                if (HANDLE kill = pid ? OpenProcess(PROCESS_TERMINATE, FALSE, pid) : nullptr) {
                    if (TerminateProcess(kill, 1)) closed = !wait || WaitForSingleObject(wait, 2000) == WAIT_OBJECT_0;
                    CloseHandle(kill);
                }
            }
            if (wait) CloseHandle(wait);
        }
        gamma::RefreshDisplays();
        gamma::Apply(core::IdentityRamp());
        gamma::Close();
        Say(!was_running ? L"Normal colors restored.\n"
            : closed     ? L"Candela was running: it was closed, and normal colors are back.\n"
                         : L"Candela is running as admin and didn't close, so it may put its colors back. Quit it from its "
                           L"tray icon, or run this again from an admin prompt.\n",
            L"Candela");
        return closed ? 0 : 1;
    }

    // Read-only, so it works whether or not Candela is already running.
    if (has(L"--schedule") && dump_dir.empty()) {
        LoadConfig();
        Say(ScheduleText(), L"Candela schedule");
        return 0;
    }

    HANDLE mutex = CreateMutexW(nullptr, TRUE, CANDELA_MUTEX);
    DWORD err = GetLastError();
    // "Access denied" means a Candela running as admin owns it: also already running.
    if (err == ERROR_ALREADY_EXISTS || (!mutex && err == ERROR_ACCESS_DENIED)) {
        // Already running: open its settings, or do what --do asked.
        if (HWND other = FindWindowW(CANDELA_MAIN_CLASS, nullptr)) {
            AllowSetForegroundWindow(ASFW_ANY);
            if (action) PostMessageW(other, WM_CANDELA_DO, action, 0);
            else PostMessageW(other, WM_CANDELA_OPEN_SETTINGS, 0, 0);
        }
        return 0;
    }
    if (action) {
        Say(L"Candela isn't running.\n", L"Candela");
        return 1;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (dump_dir.empty()) {
        StartGuard();
        LoadConfig();
    } else {
        g_cfg = core::Config();  // --dump: the defaults, whatever this PC has, in Madrid
        g_cfg.latitude = 40.4168;
        g_cfg.longitude = -3.7038;
        g_cfg.games = L"eldenring.exe, cs2.exe";
        ApplyPalette();
        if (has(L"--ja")) g_cfg.language = L"ja";
        if (has(L"--grey")) {
            g_cfg.palette = L"grey";
            ApplyPalette();
        }
    }
    // Without this, Windows may group Candela with other apps in the taskbar.
    SetCurrentProcessExplicitAppUserModelID(L"Candela.ScreenWarmth");
    sys::EnableDarkMenus();
    if (!gfx::Init()) {
        MessageBoxW(nullptr, L"Direct2D isn't available, so Candela can't draw its windows.", L"Candela", MB_ICONERROR);
        return 1;
    }
    if (dump_dir.empty() && FirstRun()) {
        SaveConfigNow();  // so the first-start welcome happens once
        engine::SkipWarmNotice();  // its question says that, not a notification as well
    }
    if (!engine::Init()) {
        MessageBoxW(nullptr, L"Candela couldn't start (see the log).", L"Candela", MB_ICONERROR);
        return 1;
    }
    Log(L"Candela %ls started%ls", CANDELA_VERSION, sys::IsElevated() ? L" (as admin)" : L"");
    // An older Candela's startup task still runs it as admin from an unprotected
    // folder: that task is what started this one as admin, so fix it now. (Not
    // elevated: nothing to look at at start-up; the settings show it.)
    if (dump_dir.empty() && sys::IsElevated()) sys::UpdateOldStartup();
    if (!dump_dir.empty()) {
        DumpFrames(dump_dir);
    } else if (FirstRun()) {
        // First of all: until this is done (and Windows restarted), Windows
        // keeps the screen at about 3400K or cooler, whatever the settings say.
        sys::OfferWarmColorsAtFirstStart();
        // Quit from the tray while Windows' admin question was up: just leave.
        if (!engine::ShuttingDown()) {
            engine::Wake();
            FirstSettings();
        }
    } else if (NoTownYet()) {
        // Still no town: say, started again after the restart the first start asked for.
        FirstSettings();
    } else if (has(L"--settings")) {
        settings::Open();
    }
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    settings::Shutdown();
    engine::Shutdown();
    gfx::Shutdown();
    Log(L"Candela stopped");
    CoUninitialize();
    if (mutex) CloseHandle(mutex);
    return 0;
}

// Developer screenshots: candela.exe --dump <folder> [--ja] [--grey] runs
// the popups, the switcher, the blackout and the settings window on a frozen
// clock and saves every frame as a PNG, then quits.
static void DumpFrames(const std::wstring& dir) {
    wchar_t name[96];
    g_fake_now = 100000;
    g_fake_local = core::Combine({2026, 9, 29}, {21, 13});
    // The warmth popup (popup_*): open, a repeat press, then brightness.
    {
        double start = g_fake_now;
        for (int i = 0; i < 200; i++) {
            if (i == 0) popup::Show({Tr(L"Warmth", L"色温度"), L"2000K", Tr(L"until night 22:00", L"夜間 22:00 まで"), 0.2f, true, FromTint(core::Tint(2000, false))});
            if (i == 75) popup::Show({Tr(L"Warmth", L"色温度"), L"1900K", Tr(L"until night 22:00", L"夜間 22:00 まで"), 0.186f, true, FromTint(core::Tint(1900, false))});
            if (i == 100) popup::Show({Tr(L"Brightness", L"輝度"), L"90%", Tr(L"until morning", L"翌朝まで"), 0.9f, false, {}});
            popup::DebugTick();
            swprintf(name, 96, L"\\popup_%04d.png", i);
            popup::DebugSave(dir + name);
            g_fake_now = start + 16.0 * (i + 1);
        }
        g_fake_now += 5000;
        popup::DebugTick();
    }
    // The switcher (switcher_*): open, move twice, close that window (Delete), back once, then cancel.
    switcher::OnSwitch(1);
    for (int i = 0; i < 140; i++) {
        if (i == 55 || i == 80) switcher::OnSwitch(1);
        if (i == 88) switcher::OnKey(VK_DELETE);
        if (i == 95) switcher::OnSwitch(-1);
        if (i == 110) switcher::OnKey(VK_ESCAPE);
        switcher::DebugTick();
        swprintf(name, 96, L"\\switcher_%04d.png", i);
        switcher::DebugSave(dir + name);
        g_fake_now += 16;
    }
    g_fake_now += 2000;
    switcher::DebugTick();
    // The volume and media popup (media_*): volume, volume again, then music.
    {
        g_fake_now += 5000;
        double start = g_fake_now;
        std::vector<std::pair<int, int>> presses = {{0, 0}, {40, 1}, {170, 2}, {230, 3}};
        for (int i = 0; i < 330; i++) {
            for (auto& [at, kind] : presses)
                if (i == at) media::DebugShow(kind);
            media::DebugTick();
            swprintf(name, 96, L"\\media_%04d.png", i);
            media::DebugSave(dir + name);
            g_fake_now = start + 16.0 * (i + 1);
        }
        g_fake_now += 5000;
        media::DebugTick();
    }
    blackfx::DebugDump(dir);
    g_fake_now += 5000;
    settings::DebugDump(dir);
    g_fake_now = -1;
    g_fake_local = -1;
    engine::Quit();
}
