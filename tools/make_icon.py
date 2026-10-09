"""Generates Candela's app icon, res/candela.ico. Needs Pillow.

    python tools/make_icon.py [preview.png]

The drawing is the tray icon's (src/icons.cpp): a sun setting behind two
bars, cut square, here in a sunset gradient with paper-colored bars on a
dark tile.
"""
import sys
from pathlib import Path

from PIL import Image

PAPER = (0x1F, 0x1E, 0x1B)       # the tile: the panels' background
INK = (0xD6, 0xD0, 0xB8)         # the bars: the panels' text
SUN_TOP = (255, 190, 92)         # the sunset, top to horizon
SUN_BOTTOM = (255, 110, 56)
SIZES = (16, 20, 24, 32, 40, 48, 64, 128, 256)

# The tray icon's geometry (0..1, y down), on a 16-unit grid.
SUN_CX, SUN_CY, SUN_R, SUN_BOTTOM_Y = 0.5, 0.625, 0.4375, 0.5625
BARS = ((0.0625, 0.9375, 0.625, 0.75), (0.25, 0.75, 0.8125, 0.9375))
SUN_TOP_Y = SUN_CY - SUN_R


def shape(u, v):
    """What's at (u, v) of the tray drawing: 'sun', 'bar' or None."""
    dx, dy = abs(u - SUN_CX), abs(v - SUN_CY)
    if v <= SUN_BOTTOM_Y and dx <= SUN_R and dy <= SUN_R and dx + dy <= SUN_R * 1.41421356:
        return "sun"
    for x0, x1, y0, y1 in BARS:
        if x0 <= u <= x1 and y0 <= v <= y1:
            return "bar"
    return None


def lerp(a, b, p):
    return tuple(round(a[i] + (b[i] - a[i]) * p) for i in range(3))


def render(size, ss=6):
    # The drawing sits in the tile's middle 72%, nudged up so it looks centred
    # (the sun's top is lighter than the bars' bottom).
    scale, nudge = 0.72, -0.01
    img = Image.new("RGBA", (size, size))
    px = img.load()
    for y in range(size):
        for x in range(size):
            acc = [0, 0, 0]
            for sy in range(ss):
                for sx in range(ss):
                    u = (x + (sx + 0.5) / ss) / size
                    v = (y + (sy + 0.5) / ss) / size
                    su = 0.5 + (u - 0.5) / scale
                    sv = 0.5 + (v - 0.5 - nudge) / scale
                    what = shape(su, sv)
                    if what == "sun":
                        c = lerp(SUN_TOP, SUN_BOTTOM, (sv - SUN_TOP_Y) / (SUN_BOTTOM_Y - SUN_TOP_Y))
                    elif what == "bar":
                        c = INK
                    else:
                        c = PAPER
                    for i in range(3):
                        acc[i] += c[i]
            n = ss * ss
            px[x, y] = (acc[0] // n, acc[1] // n, acc[2] // n, 255)
    return img


def main():
    root = Path(__file__).resolve().parent.parent
    images = [render(s) for s in SIZES]
    out = root / "res" / "candela.ico"
    images[-1].save(out, format="ICO", sizes=[(s, s) for s in SIZES], append_images=images[:-1])
    print("wrote", out)
    if len(sys.argv) > 1:
        sheet = Image.new("RGBA", (sum(SIZES) + 10 * len(SIZES), 256), (240, 240, 240, 255))
        x = 0
        for img in images:
            sheet.paste(img, (x, 0))
            x += img.size[0] + 10
        sheet.save(sys.argv[1])


if __name__ == "__main__":
    main()
