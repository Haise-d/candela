// config.json: one flat JSON object, written the way Python's json module writes it.
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "core.h"

namespace core {

// ---- UTF-8 ------------------------------------------------------------------------------------
std::string ToUtf8(const std::wstring& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        uint32_t c = (uint32_t)s[i];
        if (sizeof(wchar_t) == 2 && c >= 0xD800 && c < 0xDC00 && i + 1 < s.size()) {
            uint32_t lo = (uint32_t)s[i + 1];
            if (lo >= 0xDC00 && lo < 0xE000) {
                c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                i++;
            }
        }
        if (c < 0x80) out += (char)c;
        else if (c < 0x800) { out += (char)(0xC0 | (c >> 6)); out += (char)(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) {
            out += (char)(0xE0 | (c >> 12)); out += (char)(0x80 | ((c >> 6) & 0x3F)); out += (char)(0x80 | (c & 0x3F));
        } else {
            out += (char)(0xF0 | (c >> 18)); out += (char)(0x80 | ((c >> 12) & 0x3F));
            out += (char)(0x80 | ((c >> 6) & 0x3F)); out += (char)(0x80 | (c & 0x3F));
        }
    }
    return out;
}

static void PushCodepoint(std::wstring& out, uint32_t c) {
    if (sizeof(wchar_t) == 2 && c >= 0x10000) {
        c -= 0x10000;
        out += (wchar_t)(0xD800 + (c >> 10));
        out += (wchar_t)(0xDC00 + (c & 0x3FF));
    } else {
        out += (wchar_t)c;
    }
}

std::wstring FromUtf8(const std::string& s) {
    std::wstring out;
    for (size_t i = 0; i < s.size();) {
        unsigned char b = (unsigned char)s[i];
        uint32_t c;
        int extra;
        if (b < 0x80) { c = b; extra = 0; }
        else if ((b & 0xE0) == 0xC0) { c = b & 0x1F; extra = 1; }
        else if ((b & 0xF0) == 0xE0) { c = b & 0x0F; extra = 2; }
        else if ((b & 0xF8) == 0xF0) { c = b & 0x07; extra = 3; }
        else { out += (wchar_t)0xFFFD; i++; continue; }
        if (i + extra >= s.size() + (extra ? 0 : 1)) { out += (wchar_t)0xFFFD; break; }
        for (int k = 1; k <= extra; k++) c = (c << 6) | ((unsigned char)s[i + k] & 0x3F);
        PushCodepoint(out, c);
        i += 1 + extra;
    }
    return out;
}

// ---- a small JSON reader for one flat object ------------------------------------------------------
namespace {

struct Value {
    enum Kind { kNull, kBool, kNumber, kString, kOther } kind = kNull;
    bool b = false;
    double n = 0;
    std::wstring s;
    std::string raw;  // the value's exact text, to write back unknown keys unchanged
};

struct Reader {
    const std::string& t;
    size_t i = 0;
    void Ws() { while (i < t.size() && (t[i] == ' ' || t[i] == '\t' || t[i] == '\n' || t[i] == '\r')) i++; }
    bool Eat(char c) {
        Ws();
        if (i < t.size() && t[i] == c) { i++; return true; }
        return false;
    }
    bool String(std::wstring* out) {
        Ws();
        if (i >= t.size() || t[i] != '"') return false;
        i++;
        std::string bytes;
        std::wstring result;
        auto flush = [&] { result += FromUtf8(bytes); bytes.clear(); };
        while (i < t.size() && t[i] != '"') {
            if (t[i] != '\\') { bytes += t[i++]; continue; }
            if (++i >= t.size()) return false;
            char e = t[i++];
            switch (e) {
                case '"': bytes += '"'; break;
                case '\\': bytes += '\\'; break;
                case '/': bytes += '/'; break;
                case 'b': bytes += '\b'; break;
                case 'f': bytes += '\f'; break;
                case 'n': bytes += '\n'; break;
                case 'r': bytes += '\r'; break;
                case 't': bytes += '\t'; break;
                case 'u': {
                    if (i + 4 > t.size()) return false;
                    uint32_t c = (uint32_t)strtoul(t.substr(i, 4).c_str(), nullptr, 16);
                    i += 4;
                    if (c >= 0xD800 && c < 0xDC00 && i + 6 <= t.size() && t[i] == '\\' && t[i + 1] == 'u') {
                        uint32_t lo = (uint32_t)strtoul(t.substr(i + 2, 4).c_str(), nullptr, 16);
                        if (lo >= 0xDC00 && lo < 0xE000) {
                            c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                            i += 6;
                        }
                    }
                    flush();
                    PushCodepoint(result, c);
                    break;
                }
                default: return false;
            }
        }
        if (i >= t.size()) return false;
        i++;
        flush();
        *out = result;
        return true;
    }
    int depth_ = 0;  // how deeply nested the value being skipped is
    struct Depth {
        int& d;
        explicit Depth(int& x) : d(x) { d++; }
        ~Depth() { d--; }
    };
    bool Skip() {  // any value, including nested ones
        Ws();
        if (i >= t.size()) return false;
        if (t[i] == '"') { std::wstring s; return String(&s); }
        if (t[i] == '{' || t[i] == '[') {
            Depth guard(depth_);
            if (depth_ > 100) return false;  // a file of nothing but '[' must not overflow the stack
            char open = t[i], close = open == '{' ? '}' : ']';
            i++;
            if (Eat(close)) return true;
            do {
                if (open == '{') {
                    std::wstring k;
                    if (!String(&k) || !Eat(':')) return false;
                }
                if (!Skip()) return false;
            } while (Eat(','));
            return Eat(close);
        }
        size_t start = i;
        while (i < t.size() && !strchr(",}] \t\r\n", t[i])) i++;
        return i > start;
    }
    bool Val(Value* v) {
        Ws();
        size_t start = i;
        if (i < t.size() && t[i] == '"') {
            v->kind = Value::kString;
            if (!String(&v->s)) return false;
        } else if (!t.compare(i, 4, "true")) { v->kind = Value::kBool; v->b = true; i += 4; }
        else if (!t.compare(i, 5, "false")) { v->kind = Value::kBool; v->b = false; i += 5; }
        else if (!t.compare(i, 4, "null")) { v->kind = Value::kNull; i += 4; }
        else if (i < t.size() && (t[i] == '-' || (t[i] >= '0' && t[i] <= '9'))) {
            char* end = nullptr;
            v->n = strtod(t.c_str() + i, &end);
            if (end == t.c_str() + i) return false;
            // strtod also reads "-nan" and "-inf", which aren't JSON: kept as
            // they are, but never used as a number (a NaN brightness is a black screen).
            v->kind = std::isfinite(v->n) ? Value::kNumber : Value::kOther;
            i = end - t.c_str();
        } else {
            v->kind = Value::kOther;
            if (!Skip()) return false;
        }
        v->raw = t.substr(start, i - start);
        return true;
    }
};

// One settings field: how to read it from JSON and write it back.
struct Field {
    const char* name;
    enum Type { kDouble, kInt, kBool, kStr } type;
    size_t offset;
};

#define F(name, type) {#name, Field::type, offsetof(Config, name)}
const Field kFields[] = {
    F(latitude, kDouble), F(longitude, kDouble), F(day_kelvin, kInt), F(evening_kelvin, kInt),
    F(sunset_offset_min, kInt), F(night_start, kStr), F(night_darkroom, kBool), F(night_kelvin, kInt),
    F(darkroom_brightness, kDouble), F(darkroom_invert, kBool), F(darkroom_by_brightness, kBool),
    F(morning_mode, kStr), F(morning_time, kStr), F(warmth_step_k, kInt), F(show_popup, kBool),
    F(hotkey_warmer, kStr), F(hotkey_cooler, kStr), F(hotkey_dimmer, kStr),
    F(hotkey_brighter, kStr), F(hotkey_off, kStr), F(hotkey_darkroom, kStr), F(hotkey_borderless, kStr),
    F(hotkey_blackout, kStr), F(blackout_always_on_top, kBool), F(borderless_overhang_px, kInt),
    F(dim_step, kDouble), F(dim_min, kDouble), F(watchdog_ms, kInt), F(burst_ms, kInt),
    F(force_reapply_ms, kInt), F(burst_fast_ms, kInt),
    F(popup_position, kStr), F(media_popup_position, kStr), F(animation, kStr), F(games, kStr), F(popup_in_games, kBool),
    F(switcher, kBool), F(switcher_dim, kBool), F(switcher_delay_ms, kInt), F(switcher_exclusive_fullscreen, kBool),
    F(switcher_typing, kBool), F(switcher_duotone_icons, kBool), F(switcher_display, kStr),
    F(media_popup, kBool), F(volume_step, kInt), F(media_art, kBool), F(media_new_song, kBool), F(song_apps, kStr),
    F(anticheat_games, kStr), F(anticheat_step_aside, kBool),
    F(language, kStr), F(font, kStr), F(font_ja, kStr), F(palette, kStr),
    F(color_paper, kStr), F(color_paper_dark, kStr), F(color_ink, kStr), F(color_ink_soft, kStr), F(color_shade, kStr),
};
#undef F

// Python's repr() of a float: the shortest text that reads back the same.
std::string PyFloat(double v) {
    if (std::isnan(v)) return "NaN";
    if (std::isinf(v)) return v > 0 ? "Infinity" : "-Infinity";
    char buf[40];
    for (int prec = 1; prec <= 17; prec++) {
        snprintf(buf, sizeof(buf), "%.*g", prec, v);
        if (strtod(buf, nullptr) == v) break;
    }
    std::string s = buf;
    // Python writes 1e-05 style exponents for small/large values and "1.0" for whole ones.
    size_t e = s.find('e');
    if (e != std::string::npos) {
        std::string mant = s.substr(0, e), ex = s.substr(e + 1);
        int x = atoi(ex.c_str());
        if (x >= -4 && x < 16) {  // Python would use plain notation here
            snprintf(buf, sizeof(buf), "%.17f", v);
            s = buf;
            while (!s.empty() && s.back() == '0') s.pop_back();
            if (!s.empty() && s.back() == '.') s += '0';
            return s;
        }
        char sign = x < 0 ? '-' : '+';
        snprintf(buf, sizeof(buf), "%se%c%02d", mant.c_str(), sign, std::abs(x));
        return buf;
    }
    if (s.find('.') == std::string::npos) s += ".0";
    return s;
}

std::string JsonString(const std::wstring& s) {
    std::string out = "\"";
    for (size_t i = 0; i < s.size(); i++) {
        uint32_t c = (uint32_t)s[i];
        char buf[16];
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c == '\b') out += "\\b";
        else if (c == '\f') out += "\\f";
        else if (c < 0x20 || c >= 0x7F) {
            if (c >= 0x10000) {  // only on 4-byte wchar_t (Linux tests)
                c -= 0x10000;
                snprintf(buf, sizeof(buf), "\\u%04x\\u%04x", 0xD800 + (c >> 10), 0xDC00 + (c & 0x3FF));
            } else {
                snprintf(buf, sizeof(buf), "\\u%04x", c);
            }
            out += buf;
        } else out += (char)c;
    }
    return out + "\"";
}

// A number written as text, as Python's int() / float() would take it.
bool NumberText(const std::wstring& s, bool whole, double* out) {
    std::string a = ToUtf8(s);
    size_t b = a.find_first_not_of(" \t"), e = a.find_last_not_of(" \t");
    if (b == std::string::npos) return false;
    a = a.substr(b, e - b + 1);
    if (whole && a.find_first_not_of("+-0123456789") != std::string::npos) return false;
    char* end = nullptr;
    double n = strtod(a.c_str(), &end);
    if (!end || *end || !std::isfinite(n)) return false;
    *out = n;
    return true;
}

}  // namespace

bool ParseConfig(const std::string& json, Config* cfg, std::map<std::string, std::string>* unknown) {
    Reader r{json};
    if (!r.Eat('{')) return false;
    if (r.Eat('}')) return true;
    do {
        std::wstring wkey;
        Value v;
        if (!r.String(&wkey) || !r.Eat(':') || !r.Val(&v)) return false;
        std::string key = ToUtf8(wkey);
        const Field* f = nullptr;
        for (auto& k : kFields)
            if (key == k.name) f = &k;
        if (!f) {
            if (unknown) (*unknown)[key] = v.raw;
            continue;
        }
        char* base = (char*)cfg + f->offset;
        // A value of the wrong type is ignored (the default stays).
        switch (f->type) {
            case Field::kDouble: {
                double n;
                if (v.kind == Value::kNumber) *(double*)base = v.n;
                else if (v.kind == Value::kBool) *(double*)base = v.b ? 1 : 0;
                else if (v.kind == Value::kString && NumberText(v.s, false, &n)) *(double*)base = n;  // "40.5"
                break;
            }
            case Field::kInt: {
                double n;
                if (v.kind == Value::kNumber && std::isfinite(v.n) && std::fabs(v.n) < 2e9) *(int*)base = (int)v.n;
                else if (v.kind == Value::kBool) *(int*)base = v.b ? 1 : 0;
                else if (v.kind == Value::kString && NumberText(v.s, true, &n) && std::fabs(n) < 2e9) *(int*)base = (int)n;  // "2000"
                break;
            }
            case Field::kBool:
                if (v.kind == Value::kBool) *(bool*)base = v.b;
                else if (v.kind == Value::kNumber) *(bool*)base = v.n != 0;
                break;
            case Field::kStr:
                if (v.kind == Value::kString) *(std::wstring*)base = v.s;
                else if (v.kind == Value::kNumber) *(std::wstring*)base = FromUtf8(v.raw);  // Python's str(123)
                break;
        }
    } while (r.Eat(','));
    return r.Eat('}');
}

std::string SerializeConfig(const Config& cfg, const std::map<std::string, std::string>& unknown) {
    std::string out = "{";
    bool first = true;
    auto item = [&](const std::string& key, const std::string& value) {
        out += first ? "\n" : ",\n";
        first = false;
        out += "  " + JsonString(FromUtf8(key)) + ": " + value;
    };
    for (auto& f : kFields) {
        const char* base = (const char*)&cfg + f.offset;
        switch (f.type) {
            case Field::kDouble: item(f.name, PyFloat(*(const double*)base)); break;
            case Field::kInt: item(f.name, std::to_string(*(const int*)base)); break;
            case Field::kBool: item(f.name, *(const bool*)base ? "true" : "false"); break;
            case Field::kStr: item(f.name, JsonString(*(const std::wstring*)base)); break;
        }
    }
    for (auto& [key, raw] : unknown) item(key, raw);
    return out + "\n}";
}

// ---- popup corners ----------------------------------------------------------------------------------
bool IsPopupCorner(const std::wstring& s) {
    for (auto* c : {L"top-left", L"top-center", L"top-right", L"bottom-left", L"bottom-center", L"bottom-right"})
        if (s == c) return true;
    return false;
}

std::wstring MediaCorner(const Config& cfg) {
    return IsPopupCorner(cfg.media_popup_position) ? cfg.media_popup_position : cfg.popup_position;
}

// ---- lists ------------------------------------------------------------------------------------------
static std::wstring Trim(const std::wstring& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == L' ' || s[a] == L'\t')) a++;
    while (b > a && (s[b - 1] == L' ' || s[b - 1] == L'\t')) b--;
    return s.substr(a, b - a);
}

static bool SameNoCase(const std::wstring& a, const std::wstring& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
        wchar_t x = a[i], y = b[i];
        if (x >= L'A' && x <= L'Z') x += 32;
        if (y >= L'A' && y <= L'Z') y += 32;
        if (x != y) return false;
    }
    return true;
}

std::vector<std::wstring> SplitList(const std::wstring& s) {
    std::vector<std::wstring> out;
    std::wstring item;
    for (wchar_t c : s + L",") {
        if (c == L',' || c == L';') {
            std::wstring t = Trim(item);
            bool dup = false;
            for (auto& o : out) dup = dup || SameNoCase(o, t);
            if (!t.empty() && !dup) out.push_back(t);
            item.clear();
        } else {
            item += c;
        }
    }
    return out;
}

std::wstring JoinList(const std::vector<std::wstring>& v) {
    std::wstring s;
    for (auto& x : v) s += (s.empty() ? L"" : L", ") + x;
    return s;
}

bool ListHas(const std::wstring& list, const std::wstring& item) {
    for (auto& x : SplitList(list))
        if (SameNoCase(x, item)) return true;
    return false;
}

}  // namespace core
