#!/usr/bin/env python3
"""
nav_arrows.py - can2gauge 0.30.1: the nav card's turn arrows (src/can2gauge_Nav.cpp, GaugeUI uiNavShow).

Draws the small set of turn arrows the gauge keeps on its filesystem, like the boot logos, so the phone only has to
send a number for the turn ("roundabout, 2nd exit" = 8) and not a picture:

    data/nav/<name>.bin                                   for the gauge (fs_pack.py zips them, ~1-2 KB each)
    android/app/src/main/res/drawable-nodpi/nav_arrow_<name>.png   for the can2gauge Link app's preview card

    py tools/nav_arrows.py                  write them all
    py tools/nav_arrows.py --preview x.png  also a sheet of every arrow, to look over

The arrows are alpha masks (one byte a dot: 0 = see-through, 255 = solid), so the gauge colours them like its icons.
File format "C2GA" (can2gauge alpha), 12-byte header then the dots a row at a time:
    0  "C2GA"   4  version (1)   5  flags (0)   6  width (u16 LE)   8  height (u16 LE)   10  reserved (u16)
Roundabouts go clockwise, as in the UK (exit 1 is to the left).

The number of each arrow is its place in ARROWS below, starting at 1 - the same numbers as NAV_ARROW_* in
include/can2gauge_Nav.h and NavProtocol.kt in the app. Add new arrows at the end only.

Needs Python 3 with Pillow.
"""
import argparse
import math
import os
import struct

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAUGE_DIR = os.path.join(ROOT, "data", "nav")
APP_DIR = os.path.join(ROOT, "android", "app", "src", "main", "res", "drawable-nodpi")

SIZE = 144        # arrow size on the glass, in dots (drawn 1:1, no scaling)
SS = 4            # drawn this many times bigger, then shrunk: smooth edges
STROKE = 22       # line width of the turn arrows
HEAD_LEN = 40     # arrowhead length
HEAD_HALF = 34    # arrowhead half width

# the order IS the number the phone sends (1 = straight ...). Add at the end only.
ARROWS = ["straight", "left", "right", "slight_left", "slight_right", "uturn",
          "roundabout1", "roundabout2", "roundabout3", "roundabout4", "roundabout5", "arrive"]


def s(value):
    """arrow units (0..144) -> drawing units"""
    return value * SS


def head(draw, tip, direction, length=HEAD_LEN, half=HEAD_HALF):
    """a filled arrowhead with its point at tip, pointing along direction (dx, dy)"""
    dx, dy = direction
    n = math.hypot(dx, dy)
    dx, dy = dx / n, dy / n
    base_x, base_y = tip[0] - dx * length, tip[1] - dy * length
    left = (base_x - dy * half, base_y + dx * half)
    right = (base_x + dy * half, base_y - dx * half)
    draw.polygon([(s(tip[0]), s(tip[1])), (s(left[0]), s(left[1])), (s(right[0]), s(right[1]))], fill=255)


def arrow(draw, points, width=STROKE, length=HEAD_LEN, half=HEAD_HALF):
    """a line through points with rounded bends, and an arrowhead on the last point"""
    tip = points[-1]
    before = points[-2]
    dx, dy = tip[0] - before[0], tip[1] - before[1]
    n = math.hypot(dx, dy)
    # the line stops inside the head, so the head's point stays sharp
    end = (tip[0] - dx / n * length * 0.8, tip[1] - dy / n * length * 0.8)
    line = [(s(x), s(y)) for x, y in points[:-1] + [end]]
    draw.line(line, fill=255, width=s(width), joint="curve")
    r = s(width) / 2
    x0, y0 = line[0]
    draw.ellipse([x0 - r, y0 - r, x0 + r, y0 + r], fill=255)  # round start
    head(draw, tip, (dx, dy), length, half)


def font(size):
    for name in ("segoeuib.ttf", "arialbd.ttf", "DejaVuSans-Bold.ttf"):
        try:
            return ImageFont.truetype(name, s(size))
        except OSError:
            pass
    return ImageFont.load_default()


def draw_arrow(name):
    img = Image.new("L", (s(SIZE), s(SIZE)), 0)
    d = ImageDraw.Draw(img)
    if name == "straight":
        arrow(d, [(72, 138), (72, 6)])
    elif name in ("left", "right"):
        # up from the bottom, then a square turn
        if name == "right":
            arrow(d, [(48, 138), (48, 64), (138, 64)])
        else:
            arrow(d, [(96, 138), (96, 64), (6, 64)])
    elif name in ("slight_left", "slight_right"):
        if name == "slight_right":
            arrow(d, [(56, 138), (56, 84), (126, 14)])
        else:
            arrow(d, [(88, 138), (88, 84), (18, 14)])
    elif name == "uturn":
        # up, over the top to the right and back down (a UK U-turn goes round to the right)
        cx, cy, r = 74, 54, 30
        points = [(cx - r, 138)]
        for step in range(0, 19):
            angle = math.pi - math.pi * step / 18  # 180 -> 0 degrees, over the top
            points.append((cx + r * math.cos(angle), cy - r * math.sin(angle)))
        points.append((cx + r, 128))
        arrow(d, points, length=36, half=30)
    elif name.startswith("roundabout"):
        exit_number = int(name[-1])
        cx, cy, r = 72, 74, 30
        ring = s(10)
        d.ellipse([s(cx - r) - ring / 2, s(cy - r) - ring / 2, s(cx + r) + ring / 2, s(cy + r) + ring / 2],
                  outline=255, width=ring)
        d.line([(s(cx), s(140)), (s(cx), s(cy + r))], fill=255, width=s(16))  # the way in, from the bottom
        # where each exit leaves the ring, clockwise from the way in: 1 left, 2 ahead, 3 right, 4 and 5 further round
        # (both down-right: the number in the ring tells them apart, and further round would meet the way in)
        angle = math.radians({1: 180, 2: 90, 3: 0, 4: -40, 5: -40}[exit_number])
        start = (cx + r * math.cos(angle), cy - r * math.sin(angle))
        tip = (cx + 72 * math.cos(angle), cy - 72 * math.sin(angle))
        arrow(d, [start, tip], width=14, length=28, half=22)
        f = font(34)
        d.text((s(cx), s(cy)), str(exit_number), fill=255, font=f, anchor="mm")  # the exit number in the ring
    elif name == "arrive":
        # a chequered flag on a pole
        d.rectangle([s(30), s(10), s(40), s(138)], fill=255)
        x0, y0, x1, y1 = 40, 12, 132, 78
        cols, rows = 4, 3
        for row in range(rows):
            for col in range(cols):
                if (row + col) % 2 == 0:
                    d.rectangle([s(x0 + (x1 - x0) * col / cols), s(y0 + (y1 - y0) * row / rows),
                                 s(x0 + (x1 - x0) * (col + 1) / cols), s(y0 + (y1 - y0) * (row + 1) / rows)], fill=255)
        d.rectangle([s(x0), s(y0), s(x1), s(y1)], outline=255, width=s(5))
    else:
        raise ValueError(name)
    return img.resize((SIZE, SIZE), Image.LANCZOS)


def c2ga(img):
    return b"C2GA" + struct.pack("<BBHHH", 1, 0, img.width, img.height, 0) + img.tobytes()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--preview", help="also write a sheet of every arrow to this PNG")
    args = parser.parse_args()
    os.makedirs(GAUGE_DIR, exist_ok=True)
    have_app = os.path.isdir(os.path.dirname(APP_DIR))
    if have_app:
        os.makedirs(APP_DIR, exist_ok=True)
    images = []
    for number, name in enumerate(ARROWS, start=1):
        img = draw_arrow(name)
        images.append(img)
        with open(os.path.join(GAUGE_DIR, name + ".bin"), "wb") as f:
            f.write(c2ga(img))
        if have_app:
            white = Image.new("LA", img.size, 255)
            white.putalpha(img)
            white.save(os.path.join(APP_DIR, "nav_arrow_%s.png" % name))
        print("%2d  %s" % (number, name))
    if args.preview:
        sheet = Image.new("RGB", (len(images) * (SIZE + 8) + 8, SIZE + 16), (30, 142, 62))
        for i, img in enumerate(images):
            sheet.paste((255, 255, 255), (8 + i * (SIZE + 8), 8), img)
        sheet.save(args.preview)
        print("preview", args.preview)


if __name__ == "__main__":
    main()
