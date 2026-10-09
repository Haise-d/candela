// Candela's cursors plus Windows' dark mode, applied with one click and
// undone with one click (the cursors are made by tools/make_theme.py and live
// in theme\cursors next to candela.exe).
#include "candela.h"

#include <shlobj.h>

#include <cstdio>

namespace theme {

static bool WriteUtf16(const std::wstring& path, const std::wstring& text) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written;
    WORD bom = 0xFEFF;
    bool ok = WriteFile(f, &bom, 2, &written, nullptr) &&
              WriteFile(f, text.data(), (DWORD)(text.size() * sizeof(wchar_t)), &written, nullptr);
    CloseHandle(f);
    return ok;
}

// ---- theme -----------------------------------------------------------------------------------------
// Cursors and dark mode, set directly in the registry: the same values
// Settings and Control Panel change. Wallpaper and accent color are left
// alone. Before changing anything Candela saves the current values in
// %APPDATA%\Candela\theme_backup.ini, and "Undo" writes exactly those back.
static const wchar_t* kCursors[][2] = {
    {L"Arrow", L"arrow.cur"},     {L"Help", L"help.cur"},        {L"AppStarting", L"working.ani"},
    {L"Wait", L"busy.ani"},       {L"Crosshair", L"cross.cur"},  {L"IBeam", L"text.cur"},
    {L"NWPen", L"pen.cur"},       {L"No", L"unavailable.cur"},   {L"SizeNS", L"size_ns.cur"},
    {L"SizeWE", L"size_we.cur"},  {L"SizeNWSE", L"size_nwse.cur"}, {L"SizeNESW", L"size_nesw.cur"},
    {L"SizeAll", L"move.cur"},    {L"UpArrow", L"up.cur"},       {L"Hand", L"link.cur"},
};
static const wchar_t* kCursorKey = L"Control Panel\\Cursors";
static const wchar_t* kModeKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";


// One registry value: whether it existed, its type and its data.
struct Saved {
    bool exists = false;
    DWORD type = REG_SZ;
    std::wstring text;
    DWORD number = 0;
    std::vector<BYTE> raw;  // a value of some other kind (binary, multi-string...): kept byte for byte
};

static bool IsTextType(DWORD t) { return t == REG_SZ || t == REG_EXPAND_SZ; }
// What gets written back as raw bytes: anything that isn't plain text or a 4-byte number.
static bool IsRaw(const Saved& v) { return !IsTextType(v.type) && (v.type != REG_DWORD || !v.raw.empty()); }

static std::wstring ToHex(const std::vector<BYTE>& b) {
    static const wchar_t* d = L"0123456789abcdef";
    std::wstring s;
    for (BYTE x : b) { s += d[x >> 4]; s += d[x & 15]; }
    return s;
}
static std::vector<BYTE> FromHex(const std::wstring& s) {
    std::vector<BYTE> b;
    auto nib = [](wchar_t c) { return c >= L'0' && c <= L'9' ? c - L'0' : c >= L'a' && c <= L'f' ? c - L'a' + 10 : c >= L'A' && c <= L'F' ? c - L'A' + 10 : 0; };
    for (size_t i = 0; i + 1 < s.size(); i += 2) b.push_back((BYTE)(nib(s[i]) * 16 + nib(s[i + 1])));
    return b;
}

static Saved RegRead(const std::wstring& key, const wchar_t* name) {
    Saved v;
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS) return v;
    DWORD size = 0, type = 0;
    if (RegQueryValueExW(k, name, nullptr, &type, nullptr, &size) == ERROR_SUCCESS) {
        std::vector<BYTE> buf(size + 2 * sizeof(wchar_t), 0);  // zero padding: text always ends
        DWORD got = size;
        if (RegQueryValueExW(k, name, nullptr, &type, buf.data(), &got) == ERROR_SUCCESS) {
            v.exists = true;
            v.type = type;
            if (type == REG_DWORD && got == 4) v.number = *(DWORD*)buf.data();
            else if (IsTextType(type)) v.text = (const wchar_t*)buf.data();
            else v.raw.assign(buf.begin(), buf.begin() + got);  // not ours to understand: kept as it is
        }
    }
    RegCloseKey(k);
    return v;
}

static void RegWrite(const std::wstring& key, const wchar_t* name, const Saved& v) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) !=
        ERROR_SUCCESS)
        return;
    if (!v.exists) {
        RegDeleteValueW(k, name);
    } else if (IsRaw(v)) {
        RegSetValueExW(k, name, 0, v.type, v.raw.data(), (DWORD)v.raw.size());
    } else if (v.type == REG_DWORD) {
        RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE*)&v.number, sizeof(DWORD));
    } else {
        RegSetValueExW(k, name, 0, v.type, (const BYTE*)v.text.c_str(), (DWORD)((v.text.size() + 1) * sizeof(wchar_t)));
    }
    RegCloseKey(k);
}

static Saved Text(const std::wstring& s, DWORD type = REG_SZ) { Saved v; v.exists = true; v.type = type; v.text = s; return v; }
static Saved Number(DWORD n) { Saved v; v.exists = true; v.type = REG_DWORD; v.number = n; return v; }

// Every value the theme touches (undo also restores anything an older
// version's backup recorded, like sounds), as (key, value name). nullptr = the key's default value.
static std::vector<std::pair<std::wstring, const wchar_t*>> Touched() {
    std::vector<std::pair<std::wstring, const wchar_t*>> out;
    out.push_back({kCursorKey, nullptr});
    for (auto& c : kCursors) out.push_back({kCursorKey, c[0]});
    out.push_back({kModeKey, L"AppsUseLightTheme"});
    out.push_back({kModeKey, L"SystemUsesLightTheme"});
    return out;
}

static std::wstring BackupPath() { return AppDir() + L"\\theme_backup.ini"; }

static void SaveBackup() {
    std::wstring path = BackupPath();
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return;  // keep the first, original one
    WriteUtf16(path, L"; Your settings before Candela's theme. Turning off \"Cursors and dark mode\" restores them.\r\n");
    int i = 0;
    for (auto& [key, name] : Touched()) {
        Saved v = RegRead(key, name);
        wchar_t sec[16];
        swprintf(sec, 16, L"v%d", i++);
        WritePrivateProfileStringW(sec, L"key", key.c_str(), path.c_str());
        WritePrivateProfileStringW(sec, L"name", name ? name : L"", path.c_str());
        WritePrivateProfileStringW(sec, L"exists", v.exists ? L"1" : L"0", path.c_str());
        WritePrivateProfileStringW(sec, L"type", std::to_wstring(v.type).c_str(), path.c_str());
        WritePrivateProfileStringW(sec, L"number", std::to_wstring(v.number).c_str(), path.c_str());
        WritePrivateProfileStringW(sec, L"text", (L"\"" + v.text + L"\"").c_str(), path.c_str());
        if (IsRaw(v)) WritePrivateProfileStringW(sec, L"raw", ToHex(v.raw).c_str(), path.c_str());
    }
}

// Telling every window can take a while (each one that's slow to answer holds
// it up for its 200 ms), so it's done on a thread of its own and the tray, the
// hotkeys and the color watchdog carry on meanwhile.
static DWORD WINAPI AnnounceThread(LPVOID) {
    SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, SPIF_SENDCHANGE);
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"ImmersiveColorSet", SMTO_ABORTIFHUNG, 200, nullptr);
    return 0;
}

static void Announce() {
    HANDLE t = CreateThread(nullptr, 0, AnnounceThread, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
}

bool Apply() {
    // The cursor set that matches the current colors.
    std::wstring set = g_cfg.palette == L"grey" ? L"grey" : L"warm";
    std::wstring dir = ExeDir() + L"\\theme";
    if (GetFileAttributesW((dir + L"\\cursors\\" + set + L"\\arrow.cur").c_str()) == INVALID_FILE_ATTRIBUTES) {
        Log(L"Theme files not found in %ls", dir.c_str());
        return false;
    }
    SaveBackup();
    RegWrite(kCursorKey, nullptr, Text(L"Candela"));
    for (auto& c : kCursors) RegWrite(kCursorKey, c[0], Text(dir + L"\\cursors\\" + set + L"\\" + c[1], REG_EXPAND_SZ));
    RegWrite(kModeKey, L"AppsUseLightTheme", Number(0));
    RegWrite(kModeKey, L"SystemUsesLightTheme", Number(0));
    Announce();
    Log(L"Theme applied (backup in %ls)", BackupPath().c_str());
    return true;
}

bool Restore() {
    std::wstring path = BackupPath();
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return false;  // nothing to undo
    for (int i = 0;; i++) {
        wchar_t sec[16], key[512], name[128], text[2048];
        swprintf(sec, 16, L"v%d", i);
        if (!GetPrivateProfileStringW(sec, L"key", L"", key, 512, path.c_str())) break;
        GetPrivateProfileStringW(sec, L"name", L"", name, 128, path.c_str());
        GetPrivateProfileStringW(sec, L"text", L"", text, 2048, path.c_str());
        Saved v;
        v.exists = GetPrivateProfileIntW(sec, L"exists", 0, path.c_str()) != 0;
        v.type = GetPrivateProfileIntW(sec, L"type", REG_SZ, path.c_str());
        v.number = GetPrivateProfileIntW(sec, L"number", 0, path.c_str());
        v.text = text;
        static std::vector<wchar_t> hex(32768);
        if (GetPrivateProfileStringW(sec, L"raw", L"", hex.data(), (DWORD)hex.size(), path.c_str()) > 0)
            v.raw = FromHex(hex.data());
        RegWrite(key, name[0] ? name : nullptr, v);
    }
    Announce();
    DeleteFileW(path.c_str());
    Log(L"Theme undone");
    return true;
}

bool Applied() { return GetFileAttributesW(BackupPath().c_str()) != INVALID_FILE_ATTRIBUTES; }

void Follow() {
    if (!Applied()) return;
    std::wstring set = g_cfg.palette == L"grey" ? L"grey" : L"warm";
    std::wstring ours = ExeDir() + L"\\theme\\cursors\\", want = ours + set + L"\\arrow.cur";
    Saved arrow = RegRead(kCursorKey, L"Arrow");
    if (!arrow.exists || !_wcsicmp(arrow.text.c_str(), want.c_str())) return;
    // Only over Candela's own cursors: ones changed in Control Panel since stay.
    if (_wcsnicmp(arrow.text.c_str(), ours.c_str(), ours.size()) != 0) return;
    if (Apply()) Log(L"Black cursors: now the %ls set, to match the palette", set.c_str());
}


}  // namespace theme
