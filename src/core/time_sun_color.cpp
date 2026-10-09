// Calendar maths, sunrise/sunset, color temperature and Darkroom's filter.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwctype>

#include "core.h"

namespace core {

// ---- calendar -------------------------------------------------------------------------------
// Howard Hinnant's days-from-civil algorithms (proleptic Gregorian).
int64_t DaysFromCivil(int y, int m, int d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

Date CivilFromDays(int64_t z) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t y = (int64_t)yoe + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp < 10 ? mp + 3 : mp - 9;
    return {(int)(y + (m <= 2)), (int)m, (int)d};
}

static int64_t DayNumber(LocalTime t) { return (int64_t)std::floor(t / 86400.0); }

Date DateOf(LocalTime t) { return CivilFromDays(DayNumber(t)); }
Date AddDays(Date d, int n) { return CivilFromDays(DaysFromCivil(d.y, d.m, d.d) + n); }
LocalTime Combine(Date d, HM t) { return (double)DaysFromCivil(d.y, d.m, d.d) * 86400.0 + t.h * 3600.0 + t.m * 60.0; }
LocalTime Midnight(LocalTime t) { return (double)DayNumber(t) * 86400.0; }

HM TimeOf(LocalTime t) {
    int64_t s = (int64_t)std::floor(t) - DayNumber(t) * 86400;
    return {(int)(s / 3600), (int)(s / 60 % 60)};
}

std::wstring FormatHM(LocalTime t) {
    HM hm = TimeOf(t);
    wchar_t buf[8];
    swprintf(buf, 8, L"%02d:%02d", hm.h, hm.m);
    return buf;
}

int Weekday(Date d) {
    int64_t days = DaysFromCivil(d.y, d.m, d.d);
    return (int)(((days + 3) % 7 + 7) % 7);  // 1970-01-01 was a Thursday
}

std::optional<HM> ParseHM(const std::wstring& text) {
    // "22:00", "7:05", " 22:00 ". Like Python's time(int(hh), int(mm)).
    std::wstring s = text;
    while (!s.empty() && iswspace(s.front())) s.erase(s.begin());
    while (!s.empty() && iswspace(s.back())) s.pop_back();
    size_t colon = s.find(L':');
    if (colon == std::wstring::npos || s.find(L':', colon + 1) != std::wstring::npos) return std::nullopt;
    auto num = [](const std::wstring& p, int* out) {
        std::wstring q = p;
        while (!q.empty() && iswspace(q.front())) q.erase(q.begin());
        while (!q.empty() && iswspace(q.back())) q.pop_back();
        if (q.empty() || q.size() > 4) return false;
        size_t i = (q[0] == L'+') ? 1 : 0;
        if (i >= q.size()) return false;
        int v = 0;
        for (; i < q.size(); i++) {
            if (q[i] < L'0' || q[i] > L'9') return false;
            v = v * 10 + (q[i] - L'0');
        }
        *out = v;
        return true;
    };
    int h, m;
    if (!num(s.substr(0, colon), &h) || !num(s.substr(colon + 1), &m)) return std::nullopt;
    if (h > 23 || m > 59) return std::nullopt;
    return HM{h, m};
}

// ---- sun --------------------------------------------------------------------------------------
// The standard "sunrise equation", as in NOAA's calculator (simplified).
// About a minute of accuracy; no network, no time zone database.
static constexpr double kJ2000 = 2451545.0;
static constexpr double kUnixEpochJd = 2440587.5;
static constexpr double kPi = 3.14159265358979323846;
static double Rad(double deg) { return deg * kPi / 180.0; }
static double Deg(double rad) { return rad * 180.0 / kPi; }
static double PyMod(double a, double b) {  // Python's %: never negative for b > 0
    double r = std::fmod(a, b);
    return r < 0 ? r + b : r;
}

bool SunTimesUtc(Date d, double lat, double lon, double* rise_utc, double* set_utc) {
    double jd_noon = (double)DaysFromCivil(d.y, d.m, d.d) + 0.5 + kUnixEpochJd;
    double n = std::nearbyint(jd_noon - kJ2000);
    double j_star = n - lon / 360.0;
    double m = Rad(PyMod(357.5291 + 0.98560028 * j_star, 360.0));
    double c = 1.9148 * std::sin(m) + 0.02 * std::sin(2 * m) + 0.0003 * std::sin(3 * m);
    double lam = Rad(PyMod(Deg(m) + c + 180.0 + 102.9372, 360.0));
    double j_transit = kJ2000 + j_star + 0.0053 * std::sin(m) - 0.0069 * std::sin(2 * lam);
    double sin_decl = std::sin(lam) * std::sin(Rad(23.4397));
    double cos_decl = std::cos(std::asin(sin_decl));
    double phi = Rad(lat);
    // -0.833 degrees: refraction and the size of the sun's disc.
    double cos_w0 = (std::sin(Rad(-0.833)) - std::sin(phi) * sin_decl) / (std::cos(phi) * cos_decl);
    if (!(cos_w0 >= -1.0 && cos_w0 <= 1.0)) return false;
    double w0 = Deg(std::acos(cos_w0));
    *rise_utc = (j_transit - w0 / 360.0 - kUnixEpochJd) * 86400.0;
    *set_utc = (j_transit + w0 / 360.0 - kUnixEpochJd) * 86400.0;
    return true;
}

bool LocalSunTimes(Date d, double lat, double lon, LocalTime* rise, LocalTime* set) {
    double r, s;
    if (!SunTimesUtc(d, lat, lon, &r, &s)) return false;
    // Rounded to the microsecond, like Python's datetime.
    r = std::nearbyint(r * 1e6) / 1e6;
    s = std::nearbyint(s * 1e6) / 1e6;
    *rise = LocalFromUtc(r);
    *set = LocalFromUtc(s);
    return true;
}

// ---- color --------------------------------------------------------------------------------------
// The color of a black body (Krystek's approximation of the Planckian locus,
// 1000K-15000K), in linear sRGB, normalised so 6500K is exactly white.
static Rgb BlackbodyLinear(double t) {
    double u = (0.860117757 + 1.54118254e-4 * t + 1.28641212e-7 * t * t) / (1 + 8.42420235e-4 * t + 7.08145163e-7 * t * t);
    double v = (0.317398726 + 4.22806245e-5 * t + 4.20481691e-8 * t * t) / (1 - 2.89741816e-5 * t + 1.61456053e-7 * t * t);
    double x = 3 * u / (2 * u - 8 * v + 4);
    double y = 2 * v / (2 * u - 8 * v + 4);
    double X = x / y, Y = 1.0, Z = (1 - x - y) / y;
    return {3.2406 * X - 1.5372 * Y - 0.4986 * Z, -0.9689 * X + 1.8758 * Y + 0.0415 * Z,
            0.0557 * X - 0.2040 * Y + 1.0570 * Z};
}

Rgb Multipliers(double kelvin) {
    if (kelvin < 1000) {
        // The approximation stops at 1000K. Below it green and blue fade out,
        // so 800K (Darkroom) is pure red, as in f.lux.
        Rgb c = Multipliers(1000);
        double f = std::max(0.0, (kelvin - kDarkroom) / (1000.0 - kDarkroom));
        return {c.r, c.g * f, c.b * f};
    }
    static const Rgb white = BlackbodyLinear(kNeutralK);
    Rgb lin = BlackbodyLinear(std::min(kelvin, 15000.0));
    double r = std::max(0.0, lin.r / white.r), g = std::max(0.0, lin.g / white.g), b = std::max(0.0, lin.b / white.b);
    double top = std::max({r, g, b});
    r /= top; g /= top; b /= top;
    // The ramp works on gamma-encoded levels, so encode the linear light ratio.
    return {std::pow(r, 1 / 2.2), std::pow(g, 1 / 2.2), std::pow(b, 1 / 2.2)};
}

static uint16_t Level(double v) { return (uint16_t)std::clamp(std::nearbyint(v), 0.0, 65535.0); }

Ramp BuildRamp(int kelvin, double darkroom_brightness, bool invert, double brightness) {
    Ramp out = {};
    brightness = std::clamp(brightness, 0.0, 1.0);
    if (kelvin <= kDarkroom) {
        // Pure red: green and blue off, so dark stays dark. Inverted: f.lux's
        // photographic darkroom (white turns black).
        double b = std::clamp(darkroom_brightness, 0.0, 1.0) * brightness;
        for (int i = 0; i < 256; i++) {
            double v = i / 255.0;
            out.ch[0][i] = Level((invert ? 1 - v : v) * b * 65535);
        }
        return out;
    }
    Rgb m = kelvin >= kNeutralK ? Rgb{1, 1, 1} : Multipliers(kelvin);
    const double mult[3] = {m.r, m.g, m.b};
    for (int c = 0; c < 3; c++)
        for (int i = 0; i < 256; i++) out.ch[c][i] = Level(i / 255.0 * mult[c] * brightness * 65535);
    return out;
}

Ramp IdentityRamp() { return BuildRamp(kNeutralK); }

std::wstring Describe(int kelvin) {
    if (kelvin <= kDarkroom) return L"Afterglow";
    return std::to_wstring(kelvin) + L"K";
}

// ---- when Windows won't take a ramp --------------------------------------------------------------
Ramp Blend(const Ramp& from, const Ramp& to, double t) {
    t = std::clamp(t, 0.0, 1.0);
    Ramp out = {};
    for (int c = 0; c < 3; c++)
        for (int i = 0; i < 256; i++) out.ch[c][i] = Level(from.ch[c][i] + (to.ch[c][i] - (double)from.ch[c][i]) * t);
    return out;
}

double ClosestAccepted(const Ramp& from, const Ramp& to, const std::function<bool(const Ramp&)>& accepts, double guess) {
    // Windows checks how far each level strays from normal, so a ramp further
    // along the way is never taken when a nearer one wasn't: halving works.
    // Everything is counted in 256ths, so the answer is the same whichever
    // way it's found.
    int lo = 0, hi = 256;  // lo: taken (or nothing yet), hi: refused
    auto take = [&](int n) { return accepts(Blend(from, to, n / 256.0)); };
    if (guess > 0 && guess < 1) {
        // The step guessed, then the one past it (or before it): when the
        // guess is right or one off, that's the answer.
        int g = std::clamp((int)std::lround(guess * 256), 1, 255);
        if (take(g)) {
            lo = g;
            if (g + 1 >= hi || !take(g + 1)) return lo / 256.0;
            lo = g + 1;
        } else {
            hi = g;
            if (g - 1 <= lo) return lo / 256.0;
            if (take(g - 1)) return (g - 1) / 256.0;
            hi = g - 1;
        }
    }
    while (hi - lo > 1) {
        int mid = (lo + hi) / 2;
        if (take(mid)) lo = mid;
        else hi = mid;
    }
    return lo / 256.0;
}

double MaxDeviation(const Ramp& from, const Ramp& to) {
    double most = 0;
    for (int c = 0; c < 3; c++)
        for (int i = 0; i < 256; i++) most = std::max(most, std::fabs((double)to.ch[c][i] - from.ch[c][i]));
    return most;
}

bool Approachable(const Ramp& to) {
    for (int c = 0; c < 3; c++)
        if (to.ch[c][255] < to.ch[c][0]) return false;
    return true;
}

static void TopRatios(const Ramp& r, double* top, double* g, double* b) {
    *top = std::max({(double)r.ch[0][255], (double)r.ch[1][255], (double)r.ch[2][255]});
    *g = *top > 0 ? r.ch[1][255] / *top : 1;
    *b = *top > 0 ? r.ch[2][255] / *top : 1;
}

int ApproxKelvin(const Ramp& r) {
    double top, g, b;
    TopRatios(r, &top, &g, &b);
    int best = kNeutralK;
    double best_err = 1e9;
    for (int k = kMinK; k <= kMaxK; k += 100) {
        Rgb m = Multipliers(k);
        double err = std::fabs(m.g - g) + std::fabs(m.b - b);
        if (err < best_err) {
            best_err = err;
            best = k;
        }
    }
    return best;
}

double ApproxBrightness(const Ramp& r) {
    double top, g, b;
    TopRatios(r, &top, &g, &b);
    return top / 65535.0;
}

Rgb8 Tint(int kelvin, bool disabled) {
    if (disabled) return {150, 150, 150};
    if (kelvin <= kDarkroom) return {235, 45, 32};
    Rgb m = Multipliers(kelvin);
    // Exaggerated a little, so day and evening are easy to tell apart at 16px.
    return {255, (uint8_t)(int)(255 * std::pow(m.g, 1.6)), (uint8_t)(int)(255 * std::pow(m.b, 2.2))};
}

// ---- Darkroom's filter ------------------------------------------------------------------------------
// Brightness weights (Rec. 601, as in Microsoft's grayscale example).
static constexpr float kWr = 0.299f, kWg = 0.587f, kWb = 0.114f;
static constexpr int kBlendStartK = 1200;

Matrix IdentityMatrix() {
    Matrix m = {};
    for (int i = 0; i < 5; i++) m.m[i * 5 + i] = 1;
    return m;
}

Matrix BrightnessToRed() {
    // Output red = brightness; green and blue = 0.
    Matrix m = {};
    m.m[0 * 5 + 0] = kWr;
    m.m[1 * 5 + 0] = kWg;
    m.m[2 * 5 + 0] = kWb;
    m.m[3 * 5 + 3] = 1;
    m.m[4 * 5 + 4] = 1;
    return m;
}

static Matrix TowardGray(float t) {
    // Mix of normal colors (t = 0) and plain brightness (t = 1).
    const float w[3] = {kWr, kWg, kWb};
    Matrix m = {};
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) m.m[i * 5 + j] = t * w[i];
        m.m[i * 5 + i] += 1 - t;
    }
    m.m[3 * 5 + 3] = 1;
    m.m[4 * 5 + 4] = 1;
    return m;
}

Effect EffectFor(int kelvin, bool by_brightness) {
    // Below 1200K the screen fades toward Darkroom's look, so 1100K, 1000K
    // and 900K are steps on the way rather than a sudden switch.
    if (!by_brightness || kelvin >= kBlendStartK) return {L"identity", 0.0, IdentityMatrix()};
    if (kelvin <= kDarkroom) return {L"darkroom", 1.0, BrightnessToRed()};
    double t = std::nearbyint((double)(kBlendStartK - kelvin) / (kBlendStartK - kDarkroom) * 1000.0) / 1000.0;
    wchar_t buf[32];
    swprintf(buf, 32, L"blend%.3f", t);
    std::wstring key = buf;
    while (key.back() == L'0') key.pop_back();  // "blend0.250" -> "blend0.25", like Python
    if (key.back() == L'.') key += L'0';
    return {key, t, TowardGray((float)t)};
}

}  // namespace core
