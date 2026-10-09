// The tray icon, drawn in code: Candela's sun setting behind two bars, cut
// square (a half octagon over flat bars), tinted with
// what's on screen (pale for day, orange for evening, red for Darkroom, grey
// when off). The app icon (res/candela.ico, tools/make_icon.py) is the same
// drawing on a dark tile.
#include <algorithm>
#include <cmath>

#include "candela.h"

namespace {
// Geometry in a 0..1 square (y down), on a 16-unit grid so 16 px stays crisp.
constexpr double kSunCx = 0.5, kSunCy = 0.625, kSunR = 0.4375, kSunBottom = 0.5625;
constexpr double kBars[2][4] = {{0.0625, 0.9375, 0.625, 0.75}, {0.25, 0.75, 0.8125, 0.9375}};

bool Inside(double x, double y) {
    double dx = std::fabs(x - kSunCx), dy = std::fabs(y - kSunCy);
    if (y <= kSunBottom && dx <= kSunR && dy <= kSunR && dx + dy <= kSunR * 1.41421356) return true;
    for (auto& b : kBars)
        if (x >= b[0] && x <= b[1] && y >= b[2] && y <= b[3]) return true;
    return false;
}
}  // namespace

HICON MakeTrayIcon(int size, int kelvin, bool disabled) {
    core::Rgb8 t = core::Tint(kelvin, disabled);
    BITMAPV5HEADER bi = {};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = size;
    bi.bV5Height = -size;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!color) return nullptr;
    const int n = size <= 64 ? 4 : 2;  // supersampling for smooth edges
    auto* px = (uint32_t*)bits;
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            int hits = 0;
            for (int sy = 0; sy < n; sy++)
                for (int sx = 0; sx < n; sx++)
                    hits += Inside((x + (sx + 0.5) / n) / size, (y + (sy + 0.5) / n) / size);
            uint32_t a = hits * 255 / (n * n);
            // Premultiplied is not wanted here: icons use straight alpha.
            px[y * size + x] = hits ? (a << 24) | (t.r << 16) | (t.g << 8) | t.b : 0;
        }
    // An all-zero mask (CreateBitmap without bits leaves it undefined): the
    // alpha channel decides, wherever Windows still looks at the mask.
    std::vector<BYTE> zeros((size_t)((size + 15) / 16 * 2) * size, 0);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, zeros.data());
    ICONINFO ii = {TRUE, 0, 0, mask, color};
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    return icon;
}
