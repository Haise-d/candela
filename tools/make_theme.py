"""Generates Candela's cursors. All original.

    python tools/make_theme.py out/

writes out/theme/cursors/<palette>/. Needs Pillow.
"""
import math
import struct
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

# The grey palette: near-black fill, light outline, grey for quieter parts.
INK = (0x18, 0x19, 0x1C)
PAPER = (0xE8, 0xE8, 0xE8)
PAPER_DARK = (0x9A, 0xA0, 0xA6)
SS = 8  # supersampling for smooth edges

# ---- cursor file formats ------------------------------------------------------------------


def _dib(img):
    """One CUR/ICO image as a 32-bit DIB (BGRA, bottom-up) plus an AND mask."""
    w, h = img.size
    header = struct.pack("<IiiHHIIiiII", 40, w, h * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    px = img.load()
    rows = []
    for y in range(h - 1, -1, -1):
        rows.append(b"".join(struct.pack("BBBB", px[x, y][2], px[x, y][1], px[x, y][0], px[x, y][3]) for x in range(w)))
    mask_stride = ((w + 31) // 32) * 4
    mask = b"\0" * (mask_stride * h)  # alpha does the work
    return header + b"".join(rows) + mask


def cur_bytes(images, hotspot):
    """A .cur with several sizes. hotspot is in 32-px units, scaled per size."""
    entries, blobs = [], []
    offset = 6 + 16 * len(images)
    for img in images:
        w, h = img.size
        blob = _dib(img)
        hx = round(hotspot[0] * w / 32)
        hy = round(hotspot[1] * h / 32)
        entries.append(struct.pack("<BBBBHHII", w % 256, h % 256, 0, 0, hx, hy, len(blob), offset))
        blobs.append(blob)
        offset += len(blob)
    return struct.pack("<HHH", 0, 2, len(images)) + b"".join(entries) + b"".join(blobs)


def ani_bytes(frames, hotspot, jiffies):
    """An animated cursor (RIFF ACON). frames: list of image lists."""
    icons = [cur_bytes(f, hotspot) for f in frames]
    n = len(icons)
    anih = struct.pack("<IIIIIIIII", 36, n, n, 0, 0, 32, 1, jiffies, 1)  # flags: AF_ICON

    def chunk(tag, data):
        pad = b"\0" if len(data) % 2 else b""
        return tag + struct.pack("<I", len(data)) + data + pad

    fram = b"fram" + b"".join(chunk(b"icon", ic) for ic in icons)
    body = b"ACON" + chunk(b"anih", anih) + chunk(b"LIST", fram)
    return b"RIFF" + struct.pack("<I", len(body)) + body


# ---- drawing --------------------------------------------------------------------------------
SIZES = (32, 48, 64)


def render(draw_fn, size):
    """draw_fn(d, u) draws on a 32-unit grid; u converts units to pixels."""
    big = Image.new("RGBA", (size * SS, size * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(big)
    u = size * SS / 32
    draw_fn(d, u)
    return big.resize((size, size), Image.LANCZOS)


def outlined_polygon(d, u, pts, width=1.6):
    """Ink shape with a paper outline, so it shows on dark and light."""
    scaled = [(x * u, y * u) for x, y in pts]
    d.polygon(scaled, fill=PAPER + (255,))
    # Inset the ink fill by shrinking toward the centroid-free way: redraw with a stroke.
    d.line(scaled + [scaled[0]], fill=PAPER + (255,), width=max(1, int(width * u)), joint="curve")
    inner = _inset(pts, width * 0.55)
    d.polygon([(x * u, y * u) for x, y in inner], fill=INK + (255,))


def _inset(pts, amount):
    """Offset a simple polygon inward by `amount` units (miter)."""
    n = len(pts)
    area = sum(pts[i][0] * pts[(i + 1) % n][1] - pts[(i + 1) % n][0] * pts[i][1] for i in range(n))
    sign = 1 if area > 0 else -1
    lines = []
    for i in range(n):
        (x0, y0), (x1, y1) = pts[i], pts[(i + 1) % n]
        dx, dy = x1 - x0, y1 - y0
        length = math.hypot(dx, dy) or 1
        nx, ny = -dy / length * sign, dx / length * sign  # inward normal
        lines.append(((x0 + nx * amount, y0 + ny * amount), (x1 + nx * amount, y1 + ny * amount)))
    out = []
    for i in range(n):
        (a0, a1), (b0, b1) = lines[i - 1], lines[i]
        out.append(_intersect(a0, a1, b0, b1))
    return out


def _intersect(p1, p2, p3, p4):
    x1, y1 = p1; x2, y2 = p2; x3, y3 = p3; x4, y4 = p4
    den = (x1 - x2) * (y3 - y4) - (y1 - y2) * (x3 - x4)
    if abs(den) < 1e-9:
        return p2
    t = ((x1 - x3) * (y3 - y4) - (y1 - y3) * (x3 - x4)) / den
    return (x1 + t * (x2 - x1), y1 + t * (y2 - y1))


def rect(d, u, x0, y0, x1, y1, color):
    d.rectangle([x0 * u, y0 * u, x1 * u - 1, y1 * u - 1], fill=color + (255,))


def outlined_rect(d, u, x0, y0, x1, y1, pad=1.0):
    rect(d, u, x0 - pad, y0 - pad, x1 + pad, y1 + pad, PAPER)
    rect(d, u, x0, y0, x1, y1, INK)


ARROW = [(1, 1), (1, 21), (6, 16.5), (9.5, 24.5), (13, 23), (9.6, 15.2), (16, 15.2)]


def arrow(d, u):
    outlined_polygon(d, u, ARROW)
    rect(d, u, 3.2, 9, 5.2, 11, PAPER)  # a small cut-out square


def link(d, u):
    arrow(d, u)
    outlined_rect(d, u, 17, 18, 24, 25)
    rect(d, u, 19, 20, 22, 23, PAPER)


def help_(d, u):
    arrow(d, u)
    font = ImageFont.truetype("DejaVuSans-Bold.ttf", int(11 * u))
    x, y = 17 * u, 12 * u
    for dx in (-u, 0, u):
        for dy in (-u, 0, u):
            d.text((x + dx, y + dy), "?", font=font, fill=PAPER + (255,))
    d.text((x, y), "?", font=font, fill=INK + (255,))


def ibeam(d, u):
    outlined_rect(d, u, 15, 5, 17, 27)
    outlined_rect(d, u, 11, 4, 21, 6, 0.9)
    outlined_rect(d, u, 11, 26, 21, 28, 0.9)
    rect(d, u, 15, 5, 17, 27, INK)


def cross(d, u):
    for x0, y0, x1, y1 in ((15, 2, 17, 12), (15, 20, 17, 30), (2, 15, 12, 17), (20, 15, 30, 17)):
        outlined_rect(d, u, x0, y0, x1, y1)
    outlined_rect(d, u, 15, 15, 17, 17, 0.8)


def double_arrow(d, u, angle):
    """A double-headed arrow through the centre, rotated `angle` degrees."""
    pts = [(16, 3), (22, 10), (18, 10), (18, 22), (22, 22), (16, 29), (10, 22), (14, 22), (14, 10), (10, 10)]
    a = math.radians(angle)
    rot = [(16 + (x - 16) * math.cos(a) - (y - 16) * math.sin(a), 16 + (x - 16) * math.sin(a) + (y - 16) * math.cos(a)) for x, y in pts]
    outlined_polygon(d, u, rot, 1.4)


def move(d, u):
    pts = [(16, 2), (21, 7), (17.5, 7), (17.5, 14.5), (25, 14.5), (25, 11), (30, 16), (25, 21), (25, 17.5),
           (17.5, 17.5), (17.5, 25), (21, 25), (16, 30), (11, 25), (14.5, 25), (14.5, 17.5), (7, 17.5), (7, 21),
           (2, 16), (7, 11), (7, 14.5), (14.5, 14.5), (14.5, 7), (11, 7)]
    outlined_polygon(d, u, pts, 1.3)


def unavailable(d, u):
    """A square frame with a slash through it."""
    rect(d, u, 5, 5, 27, 27, PAPER)
    rect(d, u, 6, 6, 26, 26, INK)
    rect(d, u, 9, 9, 23, 23, PAPER)
    d.rectangle([10 * u, 10 * u, 22 * u - 1, 22 * u - 1], fill=(0, 0, 0, 0))
    outlined_polygon(d, u, [(8.5, 10.5), (10.5, 8.5), (23.5, 21.5), (21.5, 23.5)], 0.9)


def pen(d, u):
    pts = [(3, 3), (9, 5), (27, 23), (23, 27), (5, 9)]
    outlined_polygon(d, u, pts, 1.4)
    rect(d, u, 5, 5, 7, 7, PAPER)


def up(d, u):
    pts = [(16, 2), (24, 11), (19, 11), (19, 29), (13, 29), (13, 11), (8, 11)]
    outlined_polygon(d, u, pts, 1.4)


def busy_frame(step):
    """A ring of eight squares; a bright one runs around it with a short tail."""
    def draw(d, u):
        for i in range(8):
            a = i / 8 * 2 * math.pi - math.pi / 2
            cx, cy = 16 + 9.5 * math.cos(a), 16 + 9.5 * math.sin(a)
            dist = (step - i) % 8  # 0 = the lit square, 1 = just behind it
            size = 3.0 if dist == 0 else 2.4 if dist == 1 else 1.8
            fill = INK if dist <= 1 else PAPER_DARK
            rect(d, u, cx - size - 0.9, cy - size - 0.9, cx + size + 0.9, cy + size + 0.9, PAPER)
            rect(d, u, cx - size, cy - size, cx + size, cy + size, fill)
    return draw


def working_frame(step):
    def draw(d, u):
        arrow(d, u)
        for i in range(3):
            lit = i == step % 3
            x = 18 + i * 4.5
            outlined_rect(d, u, x, 22, x + 3, 25, 0.8)
            if not lit:
                rect(d, u, x + 0.8, 22.8, x + 2.2, 24.2, PAPER)
    return draw


def make_cursors(dest):
    dest.mkdir(parents=True, exist_ok=True)
    statics = {
        "arrow": (arrow, (1, 1)),
        "link": (link, (1, 1)),
        "help": (help_, (1, 1)),
        "text": (ibeam, (16, 16)),
        "cross": (cross, (16, 16)),
        "size_ns": (lambda d, u: double_arrow(d, u, 0), (16, 16)),
        "size_we": (lambda d, u: double_arrow(d, u, 90), (16, 16)),
        "size_nwse": (lambda d, u: double_arrow(d, u, -45), (16, 16)),
        "size_nesw": (lambda d, u: double_arrow(d, u, 45), (16, 16)),
        "move": (move, (16, 16)),
        "unavailable": (unavailable, (16, 16)),
        "pen": (pen, (3, 3)),
        "up": (up, (16, 2)),
    }
    for name, (fn, hot) in statics.items():
        (dest / f"{name}.cur").write_bytes(cur_bytes([render(fn, s) for s in SIZES], hot))
    busy = [[render(busy_frame(i), s) for s in SIZES] for i in range(8)]
    (dest / "busy.ani").write_bytes(ani_bytes(busy, (16, 16), 6))
    working = [[render(working_frame(i), s) for s in SIZES] for i in range(3)]
    (dest / "working.ani").write_bytes(ani_bytes(working, (1, 1), 12))
    # A preview sheet for checking by eye.
    names = list(statics) + ["busy", "working"]
    sheet = Image.new("RGBA", (len(names) * 72, 144), (0x2E, 0x2C, 0x26, 255))
    light = Image.new("RGBA", (len(names) * 72, 72), (0xEE, 0xEE, 0xEE, 255))
    sheet.paste(light, (0, 72))
    for i, name in enumerate(names):
        if name == "busy":
            img = busy[0][2]
        elif name == "working":
            img = working[0][2]
        else:
            img = render(statics[name][0], 64)
        sheet.alpha_composite(img, (i * 72 + 4, 4))
        sheet.alpha_composite(img, (i * 72 + 4, 76))
    return sheet


# One cursor set per palette: near-black fill with a light outline, like the popups.
PALETTES = {
    "warm": ((0x1F, 0x1E, 0x1B), (0xD6, 0xD0, 0xB8), (0x8F, 0x8A, 0x78)),
    "grey": ((0x18, 0x19, 0x1C), (0xE8, 0xE8, 0xE8), (0x9A, 0xA0, 0xA6)),
}


def main(out):
    global INK, PAPER, PAPER_DARK
    out = Path(out)
    sheets = []
    for name, (ink, paper, paper_dark) in PALETTES.items():
        INK, PAPER, PAPER_DARK = ink, paper, paper_dark
        sheets.append(make_cursors(out / "theme" / "cursors" / name))
    w, h = sheets[0].size
    both = Image.new("RGBA", (w, h * len(sheets)))
    for i, sh in enumerate(sheets):
        both.paste(sh, (0, i * h))
    return both


if __name__ == "__main__":
    sheet = main(sys.argv[1] if len(sys.argv) > 1 else "out")
    if len(sys.argv) > 2:
        sheet.save(sys.argv[2])
