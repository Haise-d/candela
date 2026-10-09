// Start with Windows, the Start menu shortcut, the registry setting for warm
// colors, and dark menus.
//
// The registry is touched for warm colors only: f.lux's GdiIcmGammaRange value
// (only when you click the button, with Windows asking for admin), and two
// volatile keys that remember a restart is still needed: one in HKLM, written
// in the same admin step, which only a restart removes (signing out or a Fast
// Startup "Shut down" don't), and one in HKCU shared with Candela mini (gone
// at sign-out).
// Everything goes through Windows' own APIs: no other program is started.
// The cursors and dark mode are theme.cpp's.
#include "candela.h"

#include <shlobj.h>
#include <shobjidl.h>
#include <taskschd.h>

#include <cwctype>

namespace sys {

static const wchar_t* kIcmKey = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\ICM";
// Volatile: exists from "Allow" until Windows really restarts.
static const wchar_t* kPendingMachineKey = L"SOFTWARE\\Candela warm colors pending";

bool IsElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION e = {};
    DWORD size = 0;
    bool elevated = GetTokenInformation(token, TokenElevation, &e, sizeof(e), &size) && e.TokenIsElevated;
    CloseHandle(token);
    return elevated;
}

static std::wstring XmlEscape(const std::wstring& s) {
    std::wstring out;
    for (wchar_t c : s) {
        switch (c) {
            case L'&': out += L"&amp;"; break;
            case L'<': out += L"&lt;"; break;
            case L'>': out += L"&gt;"; break;
            case L'"': out += L"&quot;"; break;
            default: out += c;
        }
    }
    return out;
}

static std::wstring Env(const wchar_t* name) {
    wchar_t buf[512];
    DWORD n = GetEnvironmentVariableW(name, buf, 512);
    return n && n < 512 ? std::wstring(buf, n) : L"";
}

static bool SamePath(const std::wstring& a, const std::wstring& b) { return !a.empty() && _wcsicmp(a.c_str(), b.c_str()) == 0; }

// Set once Candela is quitting (WM_QUIT seen while waiting): no more waiting
// and no more message handling, the main loop takes over.
static bool g_quitting = false;

// Waits up to `ms` for `h` (or just that long, if h is null) without freezing
// the app: Windows messages (tray, hotkeys, timers, painting) keep being
// handled while it waits. True if `h` was signaled.
static bool PumpWait(HANDLE h, DWORD ms) {
    if (g_quitting) return h && WaitForSingleObject(h, 0) == WAIT_OBJECT_0;
    ULONGLONG end = GetTickCount64() + ms;
    for (;;) {
        ULONGLONG now = GetTickCount64();
        if (now >= end) return false;
        DWORD r = MsgWaitForMultipleObjects(h ? 1 : 0, h ? &h : nullptr, FALSE, (DWORD)(end - now), QS_ALLINPUT);
        if (h && r == WAIT_OBJECT_0) return true;
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            if (m.message == WM_QUIT) {
                // Quit from the tray while waiting. Let the main loop see it too;
                // without the flag the next wait would find it again at once and
                // spin until its time ran out.
                g_quitting = true;
                PostQuitMessage((int)m.wParam);
                return h && WaitForSingleObject(h, 0) == WAIT_OBJECT_0;
            }
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
}

static std::wstring XmlUnescape(std::wstring s) {
    static const std::pair<const wchar_t*, wchar_t> kEntities[] = {
        {L"&lt;", L'<'}, {L"&gt;", L'>'}, {L"&quot;", L'"'}, {L"&apos;", L'\''}, {L"&amp;", L'&'}};
    for (size_t i = s.find(L'&'); i != std::wstring::npos; i = s.find(L'&', i + 1))
        for (auto& [entity, c] : kEntities)
            if (s.compare(i, wcslen(entity), entity) == 0) {
                s.replace(i, wcslen(entity), 1, c);
                break;
            }
    return s;
}

// The text of the first <tag>...</tag> after `from` in a task's XML, unescaped.
static std::wstring XmlElement(const std::wstring& text, const wchar_t* tag, size_t from = 0) {
    std::wstring open = std::wstring(L"<") + tag + L">", close = std::wstring(L"</") + tag + L">";
    size_t a = text.find(open, from);
    if (a == std::wstring::npos) return L"";
    a += open.size();
    size_t b = text.find(close, a);
    return b == std::wstring::npos ? L"" : XmlUnescape(text.substr(a, b - a));
}

// Where a task's <Command> points, from the task's XML ("" if it can't be read).
// Unescaped: a folder like "C:\Tools & Games" is written "&amp;" there.
static std::wstring CommandOf(const std::wstring& text) {
    std::wstring command = XmlElement(text, L"Command");
    while (!command.empty() && (command.front() == L'"' || iswspace(command.front()))) command.erase(command.begin());
    while (!command.empty() && (command.back() == L'"' || iswspace(command.back()))) command.pop_back();
    return command;
}

static bool UnderFolder(const std::wstring& path, const std::wstring& folder) {
    if (folder.empty() || path.size() <= folder.size()) return false;
    size_t n = folder.size();
    if (_wcsnicmp(path.c_str(), folder.c_str(), n) != 0) return false;
    return path[n] == L'\\' || folder.back() == L'\\';
}

// Whether only administrators can change the files in this folder (Program
// Files, Windows). A folder like C:\Tools, or anything under the user's
// profile, can be changed by any program running as the user: a startup task
// that runs candela.exe as admin from there would run whatever replaced it.
//
// The folders come from Windows' known-folder list, not from environment
// variables: any program running as the user can set its own %ProgramFiles%
// (HKCU\Environment) and so make its own folder look protected.
static bool InProtectedFolder(const std::wstring& path) {
    for (const KNOWNFOLDERID& id : {FOLDERID_ProgramFiles, FOLDERID_ProgramFilesX86, FOLDERID_Windows}) {
        PWSTR folder = nullptr;
        bool under = false;
        if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, nullptr, &folder)) && folder) under = UnderFolder(path, folder);
        CoTaskMemFree(folder);
        if (under) return true;
    }
    return false;
}

// ---- Task Scheduler, through its own COM interface ---------------------------------------
// The root folder of Task Scheduler, where Candela's task lives.
class Scheduler {
public:
    Scheduler() {
        if (FAILED(CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_ITaskService, (void**)&svc_))) return;
        VARIANT none;
        VariantInit(&none);
        if (FAILED(svc_->Connect(none, none, none, none))) return;
        BSTR root = SysAllocString(L"\\");
        svc_->GetFolder(root, &root_);
        SysFreeString(root);
    }
    ~Scheduler() {
        if (root_) root_->Release();
        if (svc_) svc_->Release();
    }
    bool Ok() const { return root_ != nullptr; }

    // The task's definition as XML, or "" if there's no such task (or it can't be read).
    std::wstring Xml(const wchar_t* name) {
        std::wstring out;
        if (!root_) return out;
        IRegisteredTask* task = nullptr;
        BSTR n = SysAllocString(name);
        if (SUCCEEDED(root_->GetTask(n, &task)) && task) {
            BSTR xml = nullptr;
            if (SUCCEEDED(task->get_Xml(&xml)) && xml) out.assign(xml, SysStringLen(xml));
            if (xml) SysFreeString(xml);
            task->Release();
        }
        SysFreeString(n);
        return out;
    }

    bool Exists(const wchar_t* name) {
        if (!root_) return false;
        IRegisteredTask* task = nullptr;
        BSTR n = SysAllocString(name);
        bool found = SUCCEEDED(root_->GetTask(n, &task)) && task;
        if (task) task->Release();
        SysFreeString(n);
        return found;
    }

    HRESULT Register(const wchar_t* name, const std::wstring& xml) {
        if (!root_) return E_FAIL;
        BSTR n = SysAllocString(name), x = SysAllocString(xml.c_str());
        VARIANT none;
        VariantInit(&none);
        IRegisteredTask* task = nullptr;
        HRESULT hr = root_->RegisterTask(n, x, TASK_CREATE_OR_UPDATE, none, none, TASK_LOGON_INTERACTIVE_TOKEN, none, &task);
        if (task) task->Release();
        SysFreeString(n);
        SysFreeString(x);
        return hr;
    }

    // Removes a task; one that isn't there counts as removed.
    HRESULT Delete(const wchar_t* name) {
        if (!root_) return E_FAIL;
        if (!Exists(name)) return S_OK;
        BSTR n = SysAllocString(name);
        HRESULT hr = root_->DeleteTask(n, 0);
        SysFreeString(n);
        return hr;
    }

private:
    ITaskService* svc_ = nullptr;
    ITaskFolder* root_ = nullptr;
};


// ---- what needs admin -------------------------------------------------------------------
// Anything that needs admin runs in candela.exe itself, started again as
// admin with --admin <what> (Windows asks once), or right here if Candela
// already is admin. No other programs are run.
static std::wstring StartupTaskXml(const std::wstring& user, bool admin) {
    return L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n"
           L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n"
           L"  <RegistrationInfo><Description>Candela: screen warmth, an Alt+Tab switcher and a media popup</Description></RegistrationInfo>\r\n"
           L"  <Triggers><LogonTrigger><Enabled>true</Enabled><UserId>" + XmlEscape(user) + L"</UserId></LogonTrigger></Triggers>\r\n"
           L"  <Principals><Principal id=\"Author\"><UserId>" + XmlEscape(user) +
           L"</UserId><LogonType>InteractiveToken</LogonType><RunLevel>" + (admin ? L"HighestAvailable" : L"LeastPrivilege") +
           L"</RunLevel></Principal></Principals>\r\n"
           L"  <Settings>\r\n"
           L"    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>\r\n"
           L"    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>\r\n"
           L"    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>\r\n"
           L"    <AllowHardTerminate>true</AllowHardTerminate>\r\n"
           L"    <StartWhenAvailable>false</StartWhenAvailable>\r\n"
           L"    <RunOnlyIfNetworkAvailable>false</RunOnlyIfNetworkAvailable>\r\n"
           L"    <IdleSettings><StopOnIdleEnd>false</StopOnIdleEnd><RestartOnIdle>false</RestartOnIdle></IdleSettings>\r\n"
           L"    <AllowStartOnDemand>true</AllowStartOnDemand>\r\n"
           L"    <Enabled>true</Enabled>\r\n"
           L"    <Hidden>false</Hidden>\r\n"
           L"    <RunOnlyIfIdle>false</RunOnlyIfIdle>\r\n"
           L"    <ExecutionTimeLimit>PT0S</ExecutionTimeLimit>\r\n"
           L"    <Priority>4</Priority>\r\n"
           L"  </Settings>\r\n"
           L"  <Actions Context=\"Author\"><Exec><Command>" + XmlEscape(ExePath()) + L"</Command></Exec></Actions>\r\n"
           L"</Task>\r\n";
}

int RunAdmin(const std::wstring& what, const std::wstring& arg) {
    Scheduler tasks;
    if (what == L"startup-on") {
        // As admin only when the exe sits where only admins can change it.
        bool admin = InProtectedFolder(ExePath());
        if (!admin) Log(L"Startup task: %ls isn't in a folder only admins can change, so it will start without admin rights", ExePath().c_str());
        HRESULT hr = tasks.Register(L"Candela", StartupTaskXml(arg.empty() ? Env(L"USERDOMAIN") + L"\\" + Env(L"USERNAME") : arg, admin));
        Log(L"Startup task: %ls (0x%08lx)", SUCCEEDED(hr) ? L"registered" : L"not registered", (unsigned long)hr);
        return SUCCEEDED(hr) ? 0 : 1;
    }
    if (what == L"startup-off") {
        HRESULT hr = tasks.Delete(L"Candela");
        Log(L"Startup task: %ls (0x%08lx)", SUCCEEDED(hr) ? L"removed" : L"not removed", (unsigned long)hr);
        return SUCCEEDED(hr) ? 0 : 1;
    }
    if (what == L"warm-colors") {
        // The same single value f.lux's "expand range" sets.
        HKEY key;
        LSTATUS r = RegCreateKeyExW(HKEY_LOCAL_MACHINE, kIcmKey, 0, nullptr, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &key, nullptr);
        if (r == ERROR_SUCCESS) {
            DWORD v = 256;
            r = RegSetValueExW(key, L"GdiIcmGammaRange", 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
            RegCloseKey(key);
        }
        if (r == ERROR_SUCCESS) {
            // Until the restart: kept in memory only, so nothing is left behind.
            HKEY mark;
            LSTATUS m = RegCreateKeyExW(HKEY_LOCAL_MACHINE, kPendingMachineKey, 0, nullptr, REG_OPTION_VOLATILE,
                                        KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &mark, nullptr);
            if (m == ERROR_SUCCESS) RegCloseKey(mark);
            else Log(L"Warm colors: couldn't note that a restart is needed (%ld)", (long)m);
        }
        Log(L"Warm colors: %ls (%ld)", r == ERROR_SUCCESS ? L"allowed" : L"not allowed", (long)r);
        return r == ERROR_SUCCESS ? 0 : 1;
    }
    return 2;
}

// How an admin step went.
enum class Admin {
    kDone,
    kDeclined,  // No to Windows' admin question
    kFailed,    // Windows or the step itself failed (the log says why)
    kNoAnswer,  // the question is still up after 2 minutes (a Yes later still does it)
    kBusy,      // another admin question is already up
    kQuitting,  // Candela is quitting
};

// Runs RunAdmin(what) as admin: here if Candela is admin, else in a second
// candela.exe started as admin (Windows asks).
static Admin AsAdmin(const std::wstring& what, const std::wstring& arg = L"") {
    if (IsElevated()) return RunAdmin(what, arg) == 0 ? Admin::kDone : Admin::kFailed;
    static bool busy = false;  // the prompt is up: messages are handled meanwhile, so a second click lands here
    if (busy) return Admin::kBusy;
    std::wstring params = L"--admin " + what + (arg.empty() ? L"" : L" \"" + arg + L"\"");
    std::wstring exe = ExePath();
    SHELLEXECUTEINFOW sei = {sizeof(sei)};
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NO_CONSOLE;
    sei.lpVerb = L"runas";
    sei.lpFile = exe.c_str();
    sei.lpParameters = params.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei) || !sei.hProcess) {
        DWORD err = GetLastError();
        if (err == ERROR_CANCELLED) {
            Log(L"Admin step '%ls': declined", what.c_str());
            return Admin::kDeclined;
        }
        Log(L"Admin step '%ls': Windows couldn't start it (error %lu)", what.c_str(), err);
        return Admin::kFailed;
    }
    busy = true;
    DWORD code = 1;
    // The prompt can sit there for a while; wait up to two minutes, but keep the
    // tray, hotkeys, Alt+Tab and the color watchdog alive meanwhile.
    bool finished = PumpWait(sei.hProcess, 120000);
    busy = false;
    if (finished) GetExitCodeProcess(sei.hProcess, &code);
    else if (g_quitting) Log(L"Admin step '%ls': Candela is quitting, not waiting for it (it finishes by itself)", what.c_str());
    else Log(L"Admin step '%ls': no answer after 2 minutes", what.c_str());
    CloseHandle(sei.hProcess);
    if (finished) {
        if (code != 0) Log(L"Admin step '%ls': failed (exit code %lu)", what.c_str(), code);
        return code == 0 ? Admin::kDone : Admin::kFailed;
    }
    return g_quitting ? Admin::kQuitting : Admin::kNoAnswer;
}

// ---- start with Windows -------------------------------------------------------------------
// A scheduled task rather than the Run key, because only a task can start
// Candela as admin without asking every time.
static int g_startup = -1;            // -1: not asked yet
static bool g_startup_elsewhere = false;
static bool g_startup_unsafe = false;  // an older Candela's task: as admin, from a folder anyone can change
static std::wstring g_startup_user;    // whom the task is for

void RefreshStartup() {
    g_startup_elsewhere = g_startup_unsafe = false;
    g_startup_user.clear();
    std::wstring text = Scheduler().Xml(L"Candela");
    if (text.empty()) {
        g_startup = 0;
        return;
    }
    // Its <Command> says which candela.exe starts.
    std::wstring command = CommandOf(text);
    bool here = command.empty() || SamePath(command, ExePath());  // unreadable: assume it's this one
    g_startup = here ? 1 : 0;
    g_startup_elsewhere = !here;
    // Older Candelas registered the task to start as admin wherever candela.exe
    // was. Now that only happens from a protected folder, but a task
    // registered back then stays as it was until it's registered again.
    g_startup_unsafe = here && XmlElement(text, L"RunLevel") == L"HighestAvailable" && !InProtectedFolder(ExePath());
    size_t principals = text.find(L"<Principals>");
    if (principals != std::wstring::npos) g_startup_user = XmlElement(text, L"UserId", principals);
}

bool StartupNeedsUpdate() {
    if (g_startup < 0) RefreshStartup();
    return g_startup == 1 && g_startup_unsafe;
}

void UpdateOldStartup() {
    if (!StartupNeedsUpdate()) return;
    if (!IsElevated()) {
        // Changing a task that runs as admin takes admin. That task started this
        // Candela as admin at sign-in, so the next sign-in fixes it by itself.
        Log(L"Start with Windows: the task is from an older Candela and starts it as admin from %ls, which any program can "
            L"change; it's updated at the next sign-in (or turn Start with Windows off and on)", ExeDir().c_str());
        return;
    }
    std::wstring user = g_startup_user.empty() ? Env(L"USERDOMAIN") + L"\\" + Env(L"USERNAME") : g_startup_user;
    int code = RunAdmin(L"startup-on", user);  // registers it again under today's rule
    RefreshStartup();
    Log(L"Start with Windows: updated the older task so it no longer starts Candela as admin from here (%ls)",
        code == 0 && !g_startup_unsafe ? L"done" : L"failed");
}

bool StartupEnabled() {
    if (g_startup < 0) RefreshStartup();
    return g_startup == 1;
}

bool StartupPointsElsewhere() {
    if (g_startup < 0) RefreshStartup();
    return g_startup_elsewhere;
}

bool SetStartup(bool on) {
    // The task is for you, even if someone else's admin account says yes to the prompt.
    bool done = (on ? AsAdmin(L"startup-on", Env(L"USERDOMAIN") + L"\\" + Env(L"USERNAME")) : AsAdmin(L"startup-off")) == Admin::kDone;
    RefreshStartup();
    Log(L"Start with Windows %ls: %ls", on ? L"on" : L"off", done ? L"done" : L"not done (declined or refused)");
    return done;
}

// ---- Start menu -------------------------------------------------------------------------------------
static std::wstring ShortcutPath(const wchar_t* name) {
    PWSTR programs = nullptr;
    std::wstring path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &programs))) {
        path = std::wstring(programs) + L"\\" + name + L".lnk";
        CoTaskMemFree(programs);
    }
    return path;
}

static std::wstring ShortcutTarget(const std::wstring& lnk) {
    IShellLinkW* link = nullptr;
    IPersistFile* file = nullptr;
    std::wstring target;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void**)&link)) &&
        SUCCEEDED(link->QueryInterface(IID_IPersistFile, (void**)&file)) && SUCCEEDED(file->Load(lnk.c_str(), STGM_READ))) {
        wchar_t buf[MAX_PATH * 2] = {};
        if (SUCCEEDED(link->GetPath(buf, (int)std::size(buf), nullptr, SLGP_RAWPATH))) target = buf;
    }
    if (file) file->Release();
    if (link) link->Release();
    return target;
}

// Reading the shortcut loads it through COM. The settings ask on every rebuild
// (each click, each hotkey press while they're open), so the answer is kept
// for a few seconds, and dropped whenever Candela changes the shortcut.
static int g_start_menu = -1;  // -1: not known
static ULONGLONG g_start_menu_at = 0;

bool StartMenuExists() {
    ULONGLONG now = GetTickCount64();
    if (g_start_menu >= 0 && now - g_start_menu_at < 5000) return g_start_menu == 1;
    std::wstring lnk = ShortcutPath(L"Candela");
    bool exists = !lnk.empty() && GetFileAttributesW(lnk.c_str()) != INVALID_FILE_ATTRIBUTES &&
                  SamePath(ShortcutTarget(lnk), ExePath());
    g_start_menu = exists ? 1 : 0;
    g_start_menu_at = now;
    return exists;
}

bool SetStartMenu(bool on, std::wstring* error) {
    g_start_menu = -1;  // whatever happens here, ask again next time
    std::wstring lnk = ShortcutPath(L"Candela");
    if (lnk.empty()) {
        *error = L"Couldn't find the Start menu folder.";
        return false;
    }
    if (!on) {
        // Only Candela's own shortcut: one that points somewhere else isn't ours to delete.
        if (GetFileAttributesW(lnk.c_str()) == INVALID_FILE_ATTRIBUTES || StartMenuExists()) DeleteFileW(lnk.c_str());
        return true;
    }
    IShellLinkW* link = nullptr;
    IPersistFile* file = nullptr;
    std::wstring exe = ExePath();
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void**)&link);
    if (SUCCEEDED(hr)) {
        link->SetPath(exe.c_str());
        link->SetArguments(L"--settings");
        link->SetWorkingDirectory(ExeDir().c_str());
        link->SetIconLocation(exe.c_str(), 0);  // candela.exe's own icon
        link->SetDescription(L"Candela: screen warmth");
        hr = link->QueryInterface(IID_IPersistFile, (void**)&file);
    }
    if (SUCCEEDED(hr)) hr = file->Save(lnk.c_str(), TRUE);
    if (file) file->Release();
    if (link) link->Release();
    g_start_menu = -1;
    if (FAILED(hr)) {
        *error = L"Windows couldn't create the shortcut.";
        Log(L"Start menu shortcut failed (0x%08lx)", (unsigned long)hr);
        return false;
    }
    SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW, lnk.c_str(), nullptr);  // so Explorer drops its cached icon
    Log(L"Start menu shortcut created at %ls", lnk.c_str());
    return true;
}

// ---- warm colors ------------------------------------------------------------------------------------------
bool WarmColorsAllowed() {
    DWORD value = 0, size = sizeof(value);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, kIcmKey, L"GdiIcmGammaRange", RRF_RT_REG_DWORD | RRF_SUBKEY_WOW6464KEY, nullptr,
                     &value, &size) == ERROR_SUCCESS)
        return value >= 256;
    wchar_t text[32] = {};  // some tools write it as text
    size = sizeof(text);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, kIcmKey, L"GdiIcmGammaRange", RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, text,
                     &size) == ERROR_SUCCESS)
        return _wtoi(text) >= 256;
    return false;
}

bool RequestWarmColors() {
    // Takes effect after a restart (see below).
    return AsAdmin(L"warm-colors") == Admin::kDone;
}

// ---- warm colors: the restart after allowing them -------------------------------------------------------
// Windows only uses GdiIcmGammaRange after a restart, so between "Allow" and
// the next restart warm colors are still refused. That gap is where people got
// lost, so it's tracked here and said out loud until it's over.
//
// Allowed from here: the admin step also creates a volatile key in HKLM, which
// only a real restart removes (not signing out, not a Fast Startup "Shut
// down", which are exactly what doesn't count). Older versions of Candela and
// Candela mini noted it only in a volatile HKCU key with the time; that one is
// still read and written (so they see it too), but it's gone at sign-out.
// Allowed some other way since this boot (f.lux, regedit): counts once Windows
// actually refuses a warm color. Whichever way, one warm color getting through
// ends it.
static const wchar_t* kPendingKey = L"Software\\Candela warm colors";
static bool g_warm_worked = false;   // a warm color got through since start
static bool g_warm_refused = false;  // a warm color was refused while allowed

static uint64_t AsU64(const FILETIME& ft) { return ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime; }

static uint64_t NowFileTime() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return AsU64(ft);
}

// When Windows last really started (the tick count runs on through sleep).
static uint64_t BootFileTime() { return NowFileTime() - GetTickCount64() * 10000ull; }

static uint64_t IcmWrittenAt() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kIcmKey, 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) return 0;
    FILETIME ft = {};
    LSTATUS r = RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &ft);
    RegCloseKey(key);
    return r == ERROR_SUCCESS ? AsU64(ft) : 0;
}

static void NoteAllowedHere() {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kPendingKey, 0, nullptr, REG_OPTION_VOLATILE, KEY_SET_VALUE, nullptr, &key, nullptr) !=
        ERROR_SUCCESS)
        return;
    uint64_t now = NowFileTime();
    RegSetValueExW(key, L"AllowedAt", 0, REG_QWORD, (const BYTE*)&now, sizeof(now));
    RegCloseKey(key);
}

static uint64_t AllowedHereAt() {
    uint64_t at = 0;
    DWORD size = sizeof(at);
    if (RegGetValueW(HKEY_CURRENT_USER, kPendingKey, L"AllowedAt", RRF_RT_REG_QWORD, nullptr, &at, &size) != ERROR_SUCCESS) return 0;
    return at;
}

static bool MarkedUntilRestart() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kPendingMachineKey, 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS)
        return false;
    RegCloseKey(key);
    return true;
}

bool WarmColorsWork() { return g_warm_worked; }

bool WarmColorsRestartPending() {
    if (g_warm_worked || !WarmColorsAllowed()) return false;
    uint64_t boot = BootFileTime();
    return MarkedUntilRestart() || AllowedHereAt() > boot || (g_warm_refused && IcmWrittenAt() > boot);
}

void NoteWarmColorWorked() {
    if (g_warm_worked) return;
    g_warm_worked = true;
    // It's over. The HKLM key needs admin to delete; it goes at the restart,
    // and until then g_warm_worked says it's over.
    RegDeleteKeyW(HKEY_CURRENT_USER, kPendingKey);
}

void NoteWarmColorRefused() { g_warm_refused = true; }

// A yes / no box. Never two at once (each runs its own message loop, so a
// second click could otherwise open another).
static bool g_asking = false;
static bool AskYesNo(const std::wstring& text, bool default_no = false) {
    if (g_asking) return false;
    g_asking = true;
    int answer = MessageBoxW(nullptr, text.c_str(), CANDELA_NAME, MB_YESNO | MB_ICONWARNING | MB_SETFOREGROUND | MB_TOPMOST | (default_no ? MB_DEFBUTTON2 : 0));
    g_asking = false;
    return answer == IDYES;
}

// Like Start → Power → Restart: not forced, so an app with unsaved work can
// still ask first.
static bool RestartWindows() {
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        TOKEN_PRIVILEGES tp = {};
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        if (LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &tp.Privileges[0].Luid))
            AdjustTokenPrivileges(token, FALSE, &tp, 0, nullptr, nullptr);
        CloseHandle(token);
    }
    bool ok = ExitWindowsEx(EWX_REBOOT, SHTDN_REASON_MAJOR_APPLICATION | SHTDN_REASON_MINOR_RECONFIG | SHTDN_REASON_FLAG_PLANNED);
    Log(L"Warm colors: restarting Windows %ls (%lu)", ok ? L"started" : L"failed", ok ? 0ul : GetLastError());
    return ok;
}

bool AskRestartForWarmColors(bool just_allowed) {
    const std::wstring name = CANDELA_NAME;
    std::wstring text;
    if (Japanese()) {
        text = just_allowed ? L"報告：解除完了。次にPCを再起動。\n\nWindowsは再起動後に新しい設定を使用。再起動まで当機は約3400Kが上限（深い減光も不可）。\n\n"
                            : L"報告：暖色域は解除済み。ただしWindowsは再起動後に使用。再起動まで当機は約3400Kが上限（深い減光も不可）。\n\n";
        text += L"今すぐ再起動する？先に作業を保存すること。";
        if (!StartupEnabled()) text += L"\n再起動後、当機を再度起動すること。";
        if (just_allowed) text += L"\n（いいえ：後で手動で再起動。シャットダウンやサインアウトは不可。当機が通知を継続。）";
    } else {
        text = just_allowed ? L"Done. Now restart your PC.\n\nWindows only uses the new setting after a restart. Until you restart, " + name +
                                  L" can't go warmer than about 3400K, or dim much below half.\n\n"
                            : L"Warm colors are unlocked, but Windows only uses them after a restart. Until you restart, " + name +
                                  L" can't go warmer than about 3400K, or dim much below half.\n\n";
        text += L"Restart now? Save your work first.";
        if (!StartupEnabled()) text += L"\nAfter the restart, start " + name + L" again.";
        if (just_allowed)
            text += L"\n(No: restart later yourself, with Restart: Shut down and signing out don't count. " + name + L" will keep reminding you.)";
    }
    if (!AskYesNo(text, true)) {  // No by default: Enter alone mustn't restart the PC
        Log(L"Warm colors: restart later");
        return false;
    }
    if (RestartWindows()) return true;
    MessageBoxW(nullptr,
                Tr(L"Windows didn't restart. Restart from the Start menu: Power, then Restart.",
                   L"警告：再起動できず。スタートメニューの電源から「再起動」を実行。")
                    .c_str(),
                CANDELA_NAME, MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
    return false;
}

// Says why warm colors are still locked after "Unlock" didn't go through.
static void TellStillLocked(Admin why) {
    std::wstring text;
    if (why == Admin::kDeclined)
        text = Tr(L"Warm colors are still locked: Windows' admin question was answered No.\n\n"
                  L"To try again, right-click the sun icon in the tray and choose Unlock warm colors.",
                  L"報告：管理者の確認で「いいえ」が選択されたため、暖色域は制限中のまま。\n\n"
                  L"再試行：トレイの太陽アイコンを右クリックし「暖色域を解除」を実行。");
    else if (why == Admin::kFailed)
        text = Tr(L"Windows didn't allow unlocking warm colors (the log says why). They're still locked.",
                  L"警告：Windowsが暖色域の解除を拒否（詳細はログ）。暖色域は制限中のまま。");
    else if (why == Admin::kNoAnswer)
        text = Tr(L"Windows' admin question hasn't been answered yet. If you answer Yes to it later, restart your PC "
                  L"afterwards; Candela will remind you.",
                  L"報告：管理者の確認に未応答。後で「はい」を選択した場合は、その後PCを再起動すること。当機が通知を継続。");
    else
        return;  // busy or quitting: nothing to say
    if (g_asking) return;
    g_asking = true;
    MessageBoxW(nullptr, text.c_str(), CANDELA_NAME, MB_OK | MB_ICONWARNING | MB_SETFOREGROUND | MB_TOPMOST);
    g_asking = false;
}

bool UnlockWarmColors() {
    if (g_asking) return false;
    Admin r = AsAdmin(L"warm-colors");
    if (r != Admin::kDone) {
        if (!g_quitting) TellStillLocked(r);
        return false;
    }
    NoteAllowedHere();
    if (!g_quitting) AskRestartForWarmColors(true);
    return true;
}

void OfferWarmColorsAtFirstStart() {
    // Already allowed, or a warm color already got through (some setups don't
    // limit it at all): nothing to unlock.
    if (WarmColorsAllowed() || WarmColorsWork()) return;
    const std::wstring name = CANDELA_NAME;
    std::wstring text =
        Japanese() ? L"警告：Windowsが暖色域を制限中。\n\n現状、当機は約3400K未満の色温度を使用不可。\n\n"
                     L"解除手順（初回のみ）：\n1. ここで「はい」、Windowsの確認でも「はい」。\n2. PCを再起動。\n\n"
                     L"今すぐ暖色域を解除する？"
                   : L"Windows is blocking warm colors.\n\nRight now " + name +
                         L" can't make your screen warmer than about 3400K, so it never gets properly orange.\n\n"
                         L"Unlocking takes 2 steps, once:\n1. Click Yes, then Yes again in Windows' box.\n"
                         L"2. Restart your PC.\n\nUnlock warm colors now?";
    if (AskYesNo(text)) UnlockWarmColors();
    else Log(L"Warm colors: not now (first start)");
}

// ---- looks --------------------------------------------------------------------------------------------------
void EnableDarkMenus() {
    // SetPreferredAppMode (uxtheme ordinal 135, undocumented, used by Notepad++
    // and Windows Terminal): the tray menu follows Windows' dark mode.
    HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ux) return;
    using SetMode = int(WINAPI*)(int);
    using Flush = void(WINAPI*)();
    auto set = (SetMode)(void*)GetProcAddress(ux, MAKEINTRESOURCEA(135));
    auto flush = (Flush)(void*)GetProcAddress(ux, MAKEINTRESOURCEA(136));
    if (set) set(1);  // follow the Windows app theme
    if (flush) flush();
}

void OpenFolder(const std::wstring& path) { ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }
void OpenFile(const std::wstring& path) {
    std::wstring quoted = L"\"" + path + L"\"";  // a path with spaces is one argument
    // Notepad by its full path: a bare "notepad.exe" is searched for in
    // candela.exe's own folder first, and that folder may be one anyone can write to.
    wchar_t sys[MAX_PATH];
    UINT n = GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring notepad = n && n < MAX_PATH ? std::wstring(sys, n) + L"\\notepad.exe" : L"notepad.exe";
    ShellExecuteW(nullptr, L"open", notepad.c_str(), quoted.c_str(), nullptr, SW_SHOWNORMAL);
}

}  // namespace sys
