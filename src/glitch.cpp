#include "glitch.h"

#include "candela.h"

#include <algorithm>
#include <cmath>

namespace glitch {

static const wchar_t kGlyphs[] = L"0123456789ABCDEFHKXZ#%&*+=/<>?!";
static constexpr int kGlyphCount = (int)(sizeof(kGlyphs) / sizeof(kGlyphs[0])) - 1;

// In Japanese, the scramble is Japanese too, and each glyph as wide as the
// character it stands in for, so a line doesn't jump about while it decodes:
// full-width katakana, kanji numerals and symbols for kana and kanji,
// half-width katakana and digits for Latin letters and numbers.
static const wchar_t kWide[] = L"アイウエオカキクケコサシスセソタチツテトナニヌネノハヒフヘホマミムメモヤユヨラリルレロワヲン"
                               L"零壱弐参肆伍陸漆捌玖亜＃％＆＊＋＝／？！■□◆◇※";
static const wchar_t kNarrow[] = L"ｱｲｳｴｵｶｷｸｹｺｻｼｽｾｿﾀﾁﾂﾃﾄﾅﾆﾇﾈﾉﾊﾋﾌﾍﾎﾏﾐﾑﾒﾓﾔﾕﾖﾗﾘﾙﾚﾛﾜｦﾝ0123456789#%&*+=/<>?!";
static constexpr int kWideCount = (int)(sizeof(kWide) / sizeof(kWide[0])) - 1;
static constexpr int kNarrowCount = (int)(sizeof(kNarrow) / sizeof(kNarrow[0])) - 1;

// Full-width: CJK punctuation, kana, kanji, hangul, full-width forms.
static bool Wide(wchar_t c) {
    return (c >= 0x2E80 && c <= 0x9FFF) || (c >= 0xAC00 && c <= 0xD7A3) || (c >= 0xF900 && c <= 0xFAFF) ||
           (c >= 0xFE30 && c <= 0xFE4F) || (c >= 0xFF00 && c <= 0xFF60) || (c >= 0xFFE0 && c <= 0xFFE6);
}

static bool Blank(wchar_t c) { return c == L' ' || c == 0x3000; }

// A random glyph standing in for `like`.
static wchar_t Glyph(Rng& rng, wchar_t like) {
    if (!Japanese()) return kGlyphs[rng.range(0, kGlyphCount)];
    return Wide(like) ? kWide[rng.range(0, kWideCount)] : kNarrow[rng.range(0, kNarrowCount)];
}


Rng RngAt(uint32_t seed, double t_ms, int salt, double step_ms) {
    uint64_t step = (uint64_t)std::max(0.0, std::floor(t_ms / step_ms));
    return Rng(((uint64_t)seed << 20) ^ (step * 1000003ull) ^ ((uint64_t)salt << 50));
}

// Timings in ms. "subtle" never slows repeated key presses down; "full" is
// the slower, heavier version.
static const Style kOff = {0, 0, 0, 0, 0, 0, 180, 0, 0, 0, 0, false};
static const Style kSubtle = {40, 80, 160, 150, 70, 90, 60, 6, 2, 2, 0.0, true};
static const Style kFull = {90, 150, 340, 260, 150, 120, 90, 14, 5, 7, 0.3, true};

const Style& CurrentStyle() {
    if (g_cfg.animation == L"off") return kOff;
    if (g_cfg.animation == L"subtle") return kSubtle;
    return kFull;
}

bool Fx::operator==(const Fx& o) const {
    if (line != o.line || open != o.open || edges != o.edges || alpha != o.alpha) return false;
    if (tears.size() != o.tears.size() || blocks.size() != o.blocks.size()) return false;
    for (size_t i = 0; i < tears.size(); i++)
        if (tears[i].y0 != o.tears[i].y0 || tears[i].y1 != o.tears[i].y1 || tears[i].dx != o.tears[i].dx) return false;
    for (size_t i = 0; i < blocks.size(); i++)
        if (blocks[i].x0 != o.blocks[i].x0 || blocks[i].y0 != o.blocks[i].y0 || blocks[i].kind != o.blocks[i].kind)
            return false;
    return true;
}

double Clamp01(double p) { return p < 0 ? 0 : p > 1 ? 1 : p; }
double EaseOut(double p) { p = Clamp01(p); return 1 - (1 - p) * (1 - p) * (1 - p); }
double EaseIn(double p) { p = Clamp01(p); return p * p; }
double Steps(double p, int n) { p = Clamp01(p); return p >= 1 ? 1 : std::floor(p * n) / n; }

std::wstring Decode(const std::wstring& text, double p, Rng& rng, bool* typing, bool from_right, int lead) {
    if (from_right) {
        std::wstring rev(text.rbegin(), text.rend());
        std::wstring out = Decode(rev, p, rng, typing, false, lead);
        return std::wstring(out.rbegin(), out.rend());
    }
    size_t n = text.size();
    size_t done = p >= 1 ? n : (size_t)(Clamp01(p) * n);
    std::wstring out = text.substr(0, done);
    for (size_t i = done; i < n && i < done + (size_t)lead; i++)
        out += Blank(text[i]) ? text[i] : Glyph(rng, text[i]);
    if (typing) *typing = done < n;
    return out;
}

std::wstring Morph(const std::wstring& from, const std::wstring& to, double p, Rng& rng, bool pulse, bool from_right) {
    if (from_right) {
        std::wstring a(from.rbegin(), from.rend()), b(to.rbegin(), to.rend());
        std::wstring out = Morph(a, b, p, rng, pulse, false);
        return std::wstring(out.rbegin(), out.rend());
    }
    bool same = from == to;
    if (p >= 1 || (same && !pulse)) return to;
    std::wstring out;
    size_t n = std::max<size_t>(to.size(), 1);
    for (size_t i = 0; i < to.size(); i++) {
        double settle_at = 0.2 + 0.8 * (double)i / (double)n;
        bool keep = !same && i < from.size() && from[i] == to[i];
        out += (keep || p >= settle_at || Blank(to[i])) ? to[i] : Glyph(rng, to[i]);
    }
    return out;
}

std::wstring Corrupt(const std::wstring& text, double amount, Rng& rng) {
    std::wstring out = text;
    for (auto& ch : out)
        if (!Blank(ch) && rng.uniform() < amount) ch = Glyph(rng, ch);
    return out;
}

std::vector<Tear> Tears(Rng& rng, const Style& s, double intensity, float y0, float y1) {
    std::vector<Tear> out;
    if (!s.enabled || intensity <= 0 || y1 - y0 < 4) return out;
    int count = (int)std::lround(s.tears * intensity * (0.35 + 0.65 * rng.uniform()));
    static const float heights[] = {2, 3, 4, 6, 8, 12};
    for (int i = 0; i < count; i++) {
        float h = heights[rng.range(0, 6)];
        float top = y0 + (float)rng.range(0, std::max(1, (int)(y1 - y0 - h)));
        float dx = (float)std::lround((rng.uniform() * 2 - 1) * s.tear_px * intensity);
        if (dx == 0) continue;
        if (std::fabs(dx) < 2) dx = dx > 0 ? 2.f : -2.f;
        out.push_back({top, std::min(y1, top + h), dx});
    }
    std::sort(out.begin(), out.end(), [](const Tear& a, const Tear& b) { return a.y0 < b.y0; });
    return out;
}

std::vector<Block> Blocks(Rng& rng, const Style& s, double intensity, float w, float h) {
    std::vector<Block> out;
    if (!s.enabled || intensity <= 0) return out;
    int count = (int)std::lround(s.blocks * intensity * rng.uniform());
    static const float widths[] = {4, 6, 10, 16, 24, 40};
    static const float heights[] = {2, 2, 3, 4, 6};
    static const BlockKind kinds[] = {kInk, kInk, kPaperDark, kPaper, kVoid, kVoid};
    for (int i = 0; i < count; i++) {
        float bw = widths[rng.range(0, 6)], bh = heights[rng.range(0, 5)];
        float x = (float)rng.range(0, std::max(1, (int)(w - bw)));
        float y = (float)rng.range(0, std::max(1, (int)(h - bh)));
        out.push_back({x, y, x + bw, y + bh, kinds[rng.range(0, 6)]});
    }
    return out;
}

float Flicker(Rng& rng, const Style& s, double intensity) {
    return rng.uniform() < s.flicker * intensity ? 0.45f : 1.f;
}

double OpenMs(const Style& s) {
    if (!s.enabled) return 0;
    return std::max(s.line_ms + s.expand_ms, s.line_ms + s.expand_ms * 0.4 + s.decode_ms);
}

double CloseMs(const Style& s) { return s.enabled ? s.break_ms + s.collapse_ms + s.shrink_ms : s.shrink_ms; }

Fx OpenFx(const Style& s, double t, uint32_t seed, float w, float h) {
    Fx fx;
    if (!s.enabled) return fx;
    Rng rng = RngAt(seed, t, 1);
    if (t < s.line_ms) {
        fx.line = (float)EaseOut(t / s.line_ms);
        fx.alpha = Flicker(rng, s, 1);
        return fx;
    }
    double opened = s.line_ms + s.expand_ms, end = OpenMs(s);
    double grow = (t - s.line_ms) / s.expand_ms;
    fx.open = (float)EaseOut(grow);
    fx.edges = grow < 1;
    double intensity = t < opened ? 1.0 : 0.6 * Clamp01(1 - (t - opened) / std::max(1.0, end - opened));
    fx.alpha = t < opened ? Flicker(rng, s, intensity) : 1.f;
    fx.tears = Tears(rng, s, intensity, 0, h);
    fx.blocks = Blocks(rng, s, intensity * 0.8, w, h);
    return fx;
}

Fx CloseFx(const Style& s, double t, uint32_t seed, float w, float h) {
    Fx fx;
    if (!s.enabled) {
        fx.alpha = (float)Clamp01(1 - t / s.shrink_ms);
        return fx;
    }
    Rng rng = RngAt(seed, t, 4);
    if (t >= s.break_ms + s.collapse_ms) {
        double p = (t - s.break_ms - s.collapse_ms) / s.shrink_ms;
        fx.line = (float)(1 - EaseIn(p));
        fx.alpha = (float)(1 - 0.5 * Clamp01(p));
        return fx;
    }
    double p_break = Clamp01(t / s.break_ms);
    if (t >= s.break_ms) {
        fx.open = (float)(1 - EaseIn((t - s.break_ms) / s.collapse_ms));
        fx.edges = true;
    }
    double intensity = 0.5 + 0.5 * p_break;
    fx.alpha = Flicker(rng, s, intensity);
    fx.tears = Tears(rng, s, intensity, 0, h);
    fx.blocks = Blocks(rng, s, intensity, w, h);
    return fx;
}

double TypeProgress(const Style& s, double t, double delay_share, double length_share) {
    if (!s.enabled) return 1;
    double start = s.line_ms + s.expand_ms * 0.4 + s.decode_ms * delay_share;
    return (t - start) / (s.decode_ms * length_share);
}

}  // namespace glitch
