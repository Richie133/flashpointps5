#!/usr/bin/env python3
# Flashpoint PS5 - draws sce_sys/icon0.png (512x512): the app's mark, a play
# triangle with a bolt through it, over a Flash-style timeline strip.
# SPDX-License-Identifier: GPL-3.0-or-later
#
# usage: python3 tools/make-icon.py [sce_sys/icon0.png]
# The same mark is drawn in the app by draw_mark() in src/fp/app.cpp.

import math
import sys

from PIL import Image, ImageDraw, ImageFilter

OUT = sys.argv[1] if len(sys.argv) > 1 else "sce_sys/icon0.png"
SIZE = 512
SS = 4  # supersampling
W = SIZE * SS

TRIANGLE = [(0.25, 0.14), (0.86, 0.50), (0.25, 0.86)]
BOLT = [(0.555, 0.20), (0.355, 0.545), (0.505, 0.545), (0.43, 0.80), (0.665, 0.44), (0.515, 0.44), (0.60, 0.20)]


def rounded_triangle(points, radius, steps=12):
    out = []
    for i in range(3):
        p0, p1, p2 = points[i - 1], points[i], points[(i + 1) % 3]

        def unit(a, b):
            dx, dy = b[0] - a[0], b[1] - a[1]
            length = math.hypot(dx, dy)
            return dx / length, dy / length

        u1, u2 = unit(p1, p0), unit(p1, p2)
        a = (p1[0] + u1[0] * radius, p1[1] + u1[1] * radius)
        b = (p1[0] + u2[0] * radius, p1[1] + u2[1] * radius)
        for k in range(steps + 1):
            t = k / steps
            out.append(((1 - t) ** 2 * a[0] + 2 * (1 - t) * t * p1[0] + t * t * b[0],
                        (1 - t) ** 2 * a[1] + 2 * (1 - t) * t * p1[1] + t * t * b[1]))
    return out


def main():
    image = Image.new("RGBA", (W, W))
    draw = ImageDraw.Draw(image)
    # deep slate, a little lighter at the top
    for y in range(W):
        t = y / W
        draw.line([(0, y), (W, y)], fill=(int(40 - 16 * t), int(44 - 16 * t), int(60 - 18 * t), 255))

    # the timeline strip
    top = int(W * 0.80)
    draw.rectangle([0, top, W, W], fill=(29, 32, 43, 255))
    cells = 12
    for i in range(cells + 1):
        x = int(W * i / cells)
        draw.line([(x, top), (x, W)], fill=(70, 77, 94, 255), width=6)
    draw.line([(0, top), (W, top)], fill=(75, 82, 99, 255), width=8)
    for i in range(0, cells, 3):
        cx = int(W * (i + 0.5) / cells)
        draw.ellipse([cx - 18, top + 42, cx + 18, top + 78], fill=(243, 244, 247, 255))

    # the mark, with a soft lavender glow
    scale_y = 0.92
    tri = [(x * W, y * W * scale_y) for x, y in rounded_triangle(TRIANGLE, 0.07)]
    glow = Image.new("RGBA", (W, W))
    ImageDraw.Draw(glow).polygon(tri, fill=(184, 178, 242, 140))
    glow = glow.filter(ImageFilter.GaussianBlur(W * 0.045))
    image = Image.alpha_composite(image, glow)
    draw = ImageDraw.Draw(image)
    draw.polygon(tri, fill=(243, 244, 247, 255))
    draw.polygon([(x * W, y * W * scale_y) for x, y in BOLT], fill=(221, 4, 40, 255))

    # the playhead
    px = int(W * 3.5 / cells)
    draw.rectangle([px - 9, top - 26, px + 9, W], fill=(221, 4, 40, 255))
    draw.polygon([(px - 32, top - 26), (px + 32, top - 26), (px, top + 12)], fill=(221, 4, 40, 255))

    image.resize((SIZE, SIZE), Image.LANCZOS).convert("RGB").save(OUT, optimize=True)
    print(f"{OUT}: {SIZE}x{SIZE}")


if __name__ == "__main__":
    main()
