// Glitch building blocks shared by the switcher and the media popup.
// The ideas: a thin line that snaps open into a panel,
// text that types in through random glyphs, slices that tear sideways, noise
// blocks, and a quick flicker. Pure maths; render.cpp draws the results.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace glitch {

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 0x632BE59BD9B4E019ull) {}
    uint64_t next() {  // splitmix64
        uint64_t z = (s += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    double uniform() { return (next() >> 11) * (1.0 / 9007199254740992.0); }
    int range(int lo, int hi) { return hi <= lo ? lo : lo + (int)(next() % (uint64_t)(hi - lo)); }  // [lo, hi)
    bool chance(double p) { return uniform() < p; }
};

// Glitches re-roll every `step_ms`, like dropped frames, not every paint.
Rng RngAt(uint32_t seed, double t_ms, int salt, double step_ms = 33.0);

struct Style {
    double line_ms, expand_ms, decode_ms, change_ms;
    double break_ms, collapse_ms, shrink_ms;
    float tear_px;   // biggest sideways jump (unscaled px)
    int tears, blocks;
    double flicker;  // chance per step that the panel blinks dim
    bool enabled;    // false = "off": appear, fade out
};
const Style& CurrentStyle();  // from g_cfg.animation

struct Tear { float y0, y1, dx; };
enum BlockKind { kInk, kPaper, kPaperDark, kVoid };
struct Block { float x0, y0, x1, y1; BlockKind kind; };

// What a panel looks like right now, apart from its content.
struct Fx {
    float line = -1;      // >= 0: only a thin line this wide (0..1)
    float open = 1;       // share of the height showing, centred
    bool edges = false;   // bright top/bottom edges while opening/closing
    float alpha = 1;
    std::vector<Tear> tears;
    std::vector<Block> blocks;
    bool operator==(const Fx& o) const;
};

double Clamp01(double p);
double EaseOut(double p);
double EaseIn(double p);
double Steps(double p, int n);

std::wstring Decode(const std::wstring& text, double p, Rng& rng, bool* typing = nullptr,
                    bool from_right = false, int lead = 3);
std::wstring Morph(const std::wstring& from, const std::wstring& to, double p, Rng& rng,
                   bool pulse = false, bool from_right = false);
std::wstring Corrupt(const std::wstring& text, double amount, Rng& rng);

std::vector<Tear> Tears(Rng& rng, const Style& s, double intensity, float y0, float y1);
std::vector<Block> Blocks(Rng& rng, const Style& s, double intensity, float w, float h);
float Flicker(Rng& rng, const Style& s, double intensity);

// Standard open / close timelines. `t` is ms since the phase started.
// Open returns the Fx plus how far the text should be typed (0..1 per field
// with a stagger applied by the caller). Done when t >= OpenMs().
double OpenMs(const Style& s);
double CloseMs(const Style& s);
Fx OpenFx(const Style& s, double t, uint32_t seed, float w, float h);
Fx CloseFx(const Style& s, double t, uint32_t seed, float w, float h);
double TypeProgress(const Style& s, double t, double delay_share, double length_share);

}  // namespace glitch
