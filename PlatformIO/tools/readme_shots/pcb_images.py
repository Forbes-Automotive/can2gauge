#!/usr/bin/env python3
"""
README images of the board: the routed PCB and the schematic (the thumbnails EasyEDA Pro keeps inside the .epro2),
and a pinout card for the three rear plugs.

    py tools/readme_shots/pcb_images.py                       newest pcb/*.epro2
    py tools/readme_shots/pcb_images.py pcb/<file>.epro2

Needs Pillow. Output: images/pcb-top.png, images/pcb-schematic.png, images/pcb-connectors.png
"""
import glob
import io
import os
import sys
import zipfile

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
OUT = os.path.join(ROOT, "images")
sys.path.insert(0, HERE)
from glass_shots import font, BG, TEXT, MUTED, ACCENT, LINE  # noqa: E402  (same house palette / fonts)


def thumbnails(epro2):
    """the board render (mostly red/black) and the schematic sheet (mostly white), told apart by brightness"""
    with zipfile.ZipFile(epro2) as z:
        ims = [Image.open(io.BytesIO(z.read(n))).convert("RGB") for n in z.namelist()
               if n.startswith("IMAGE/") and n.lower().endswith((".webp", ".png", ".jpg"))]
    ims.sort(key=lambda im: sum(im.resize((32, 32)).convert("L").getdata()))
    return ims[0], ims[-1]


def trim(im, bg, pad=24, tol=30):
    """crop to the content that differs from the background colour"""
    diff = Image.new("L", im.size)
    px, dp = im.load(), diff.load()
    w, h = im.size
    for y in range(0, h, 2):
        for x in range(0, w, 2):
            r, g, b = px[x, y]
            if abs(r - bg[0]) + abs(g - bg[1]) + abs(b - bg[2]) > tol:
                dp[x, y] = 255
    box = diff.getbbox()
    if not box:
        return im
    return im.crop((max(0, box[0] - pad), max(0, box[1] - pad), min(w, box[2] + pad), min(h, box[3] + pad)))


PLUGS = [
    ("J_BUS1 / J_BUS2", "JST PA 4-pin  (B04B-PASK)  - wired straight through, so gauges daisy-chain",
     [("1", "12 V", "switched / ignition feed", (255, 92, 92)),
      ("2", "GND", "ground", (150, 160, 170)),
      ("3", "CAN-H", "car CAN or the gauge chain", (255, 176, 46)),
      ("4", "CAN-L", "120R switched in the web UI", (36, 209, 196))]),
    ("J_OUT", "JST PA 3-pin  (B03B-PASK)",
     [("1", "OUT", "12 V high-side OR ground low-side, 1.5 A", (63, 208, 127)),
      ("2", "K-LINE", "OBD pin 7 (KWP1281 / KWP2000 / ME7)", (180, 140, 255)),
      ("3", "DASH LIGHTS", "58d / 58 - PWM or dimmed 12 V", (255, 210, 90))]),
]


def connectors():
    W = 1400
    rowh = 74
    H = 140 + sum(150 + rowh * len(p[2]) for p in PLUGS) + 40
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    for x in range(0, W, 48):
        d.line((x, 0, x, H), fill=(18, 25, 32))
    for y in range(0, H, 48):
        d.line((0, y, W, y), fill=(18, 25, 32))
    f1, f2, f3, f4, f5 = font(44, True), font(15), font(26, True), font(22, True), font(19)
    x = 48
    for part, col in (("can", TEXT), ("2", ACCENT), ("gauge", TEXT), ("  rear plugs", MUTED)):
        d.text((x, 30), part, font=f1, fill=col)
        x += d.textlength(part, font=f1)
    d.text((48, 86), "PIN NUMBERS AS ON THE SCHEMATIC - CHECK THE PIN 1 MARK ON THE HEADER  -  JST PA, 2.0 MM, 3 A PER PIN, ONE CRIMP FOR ALL THREE", font=f2, fill=MUTED)
    d.line((48, 112, W - 48, 112), fill=ACCENT, width=2)
    y = 140
    for name, sub, pins in PLUGS:
        d.text((48, y), name, font=f3, fill=ACCENT)
        d.text((48, y + 36), sub, font=f2, fill=MUTED)
        # the header body, pins in a row
        bx, by = 48, y + 70
        bw = 70 * len(pins) + 30
        d.rounded_rectangle((bx, by, bx + bw, by + 64), 10, fill=(232, 228, 214), outline=(120, 116, 104), width=2)
        d.rectangle((bx + bw / 2 - 30, by - 10, bx + bw / 2 + 30, by), fill=(232, 228, 214), outline=(120, 116, 104), width=2)  # latch
        for i, (n, sig, _, col) in enumerate(pins):
            cx = bx + 50 + i * 70
            d.rectangle((cx - 16, by + 16, cx + 16, by + 48), fill=(40, 40, 40))
            d.ellipse((cx - 9, by + 23, cx + 9, by + 41), fill=col)
            d.text((cx - 5, by + 70), n, font=f5, fill=TEXT)
        # the table
        tx = bx + bw + 60
        for i, (n, sig, note, col) in enumerate(pins):
            ry = y + 70 + i * rowh
            d.rounded_rectangle((tx, ry, W - 48, ry + rowh - 12), 10, fill=(19, 26, 34), outline=LINE)
            d.ellipse((tx + 18, ry + 20, tx + 40, ry + 42), fill=col)
            d.text((tx + 58, ry + 16), n, font=f4, fill=MUTED)
            d.text((tx + 92, ry + 16), sig, font=f4, fill=TEXT)
            d.text((tx + 290, ry + 19), note, font=f5, fill=MUTED)
        y += 150 + rowh * len(pins)
    img.save(os.path.join(OUT, "pcb-connectors.png"), optimize=True)
    print("  pcb-connectors.png %dx%d" % img.size)


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else max(glob.glob(os.path.join(ROOT, "pcb", "*.epro2")), key=os.path.getmtime)
    os.makedirs(OUT, exist_ok=True)
    print("from %s" % os.path.relpath(src, ROOT))
    board, sch = thumbnails(src)
    trim(board, (0, 0, 0)).save(os.path.join(OUT, "pcb-top.png"), optimize=True)
    print("  pcb-top.png")
    trim(sch, (255, 255, 255), tol=60).save(os.path.join(OUT, "pcb-schematic.png"), optimize=True)
    print("  pcb-schematic.png")
    connectors()


if __name__ == "__main__":
    main()
