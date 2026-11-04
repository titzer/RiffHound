#!/usr/bin/env python3
"""Draw the Tuner app icon and write a macOS .iconset directory.

  make-icon.py ICONSET_DIR [--png out.png]

The art is generated, not loaded: a dark rounded tile (Apple's 824/1024
inset) with a cents-meter arc, an in-tune needle, and a strip of piano keys.
"""
import argparse, math, os
from PIL import Image, ImageDraw

BG_TOP = (0x1c, 0x22, 0x2c)
BG_BOT = (0x0f, 0x13, 0x19)
LINE   = (0x3b, 0x47, 0x58)
GOOD   = (0x86, 0xef, 0xac)
ACCENT = (0x7d, 0xd3, 0xfc)
WHITE  = (0xc8, 0xd4, 0xe0)
BLACK  = (0x1c, 0x24, 0x35)

def draw(size=1024, ss=4):
    S = size * ss
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    tile = 824 * ss
    off = (S - tile) // 2
    r = int(tile * 0.225)

    # Vertical gradient clipped to the rounded tile.
    grad = Image.new("RGBA", (S, S))
    gd = ImageDraw.Draw(grad)
    for y in range(off, off + tile):
        f = (y - off) / tile
        c = tuple(int(BG_TOP[i] + (BG_BOT[i] - BG_TOP[i]) * f) for i in range(3)) + (255,)
        gd.line([(off, y), (off + tile, y)], fill=c)
    mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(mask).rounded_rectangle([off, off, off + tile - 1, off + tile - 1], r, fill=255)
    im.paste(grad, (0, 0), mask)
    d = ImageDraw.Draw(im)

    # Meter arc: centre below the middle, ticks fanning over 100 degrees.
    cx, cy = S / 2, off + tile * 0.66
    R = tile * 0.40
    span = math.radians(50)
    w = int(10 * ss)
    for i in range(-10, 11):
        a = -math.pi / 2 + span * i / 10
        major = i % 5 == 0
        r0 = R * (0.80 if major else 0.87)
        col = GOOD if i == 0 else LINE
        width = w * (2 if i == 0 else (1.4 if major else 1))
        d.line([(cx + r0 * math.cos(a), cy + r0 * math.sin(a)),
                (cx + R * math.cos(a), cy + R * math.sin(a))], fill=col, width=int(width))
    # Green in-tune band on the arc.
    bb = [cx - R * 1.06, cy - R * 1.06, cx + R * 1.06, cy + R * 1.06]
    d.arc(bb, -90 - 7, -90 + 7, fill=GOOD, width=int(14 * ss))
    # Needle, slightly sharp of centre for life.
    a = -math.pi / 2 + math.radians(3)
    d.line([(cx, cy), (cx + R * 0.98 * math.cos(a), cy + R * 0.98 * math.sin(a))], fill=ACCENT, width=int(16 * ss))
    hub = 30 * ss
    d.ellipse([cx - hub, cy - hub, cx + hub, cy + hub], fill=ACCENT)

    # Piano strip along the bottom of the tile.
    ky0, ky1 = off + tile * 0.76, off + tile * 0.90
    kx0, kx1 = off + tile * 0.12, off + tile * 0.88
    n = 7
    kw = (kx1 - kx0) / n
    gap = 5 * ss
    for k in range(n):
        x = kx0 + k * kw
        d.rounded_rectangle([x + gap / 2, ky0, x + kw - gap / 2, ky1], int(8 * ss),
                            fill=GOOD if k == 5 else WHITE)
    for k in (1, 2, 4, 5, 6):   # black keys between C-D, D-E, F-G, G-A, A-B
        x = kx0 + k * kw
        bw = kw * 0.55
        d.rectangle([x - bw / 2, ky0, x + bw / 2, ky0 + (ky1 - ky0) * 0.6], fill=BLACK)

    return im.resize((size, size), Image.LANCZOS)

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("iconset")
    ap.add_argument("--png")
    args = ap.parse_args()
    big = draw()
    os.makedirs(args.iconset, exist_ok=True)
    for s in (16, 32, 128, 256, 512):
        big.resize((s, s), Image.LANCZOS).save(os.path.join(args.iconset, f"icon_{s}x{s}.png"))
        big.resize((s * 2, s * 2), Image.LANCZOS).save(os.path.join(args.iconset, f"icon_{s}x{s}@2x.png"))
    if args.png:
        big.save(args.png)

if __name__ == "__main__":
    main()
