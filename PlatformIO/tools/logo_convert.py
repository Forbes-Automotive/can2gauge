#!/usr/bin/env python3
"""
can2gauge make-logo converter: SVG / PNG / JPG -> "C2GL" .bin for the gauge's boot screen (boot logo option 5).

    python tools/logo_convert.py                        # every file in tools/logos_src/ -> data/logos/<key>.bin
    python tools/logo_convert.py vw.svg --scale 75      # one file, key from the file name
    python tools/logo_convert.py --preview              # also writes tools/logos_preview/<key>.png (round mask)
    python tools/logo_convert.py can2gauge.svg --exact  # drawn 1:1, no trim / fit (the can2gauge logo's dial must stay put)

Source files are named by logo key (include/can2gauge_themes.h, logos[]): alfa, aston, audi, bmw, chevrolet, citroen,
dodge, fiat, ford, honda, hyundai, infiniti, jaguar, kia, landrover, lexus, mazda, mercedes, mini, mitsubishi, nissan,
opel, peugeot, porsche, renault, saab, seat, skoda, subaru, toyota, vauxhall, volvo, vw. Plus forbes, the Forbes
Automotive logo for boot logo option 1 (source: J:/My Drive/Forbes Automotive/BRANDING/svg's, recoloured white).

The logo is fitted into a square of --scale % of the 480 px glass (default 80), centred, drawn on black (the screen
background; transparency becomes black), converted to RGB565 and RLE-packed. `pio run -t uploadfs` then puts
data/logos/ on the gauge, or upload single files from the web UI (Settings > Boot logos does the same conversion
in the browser).

File format (little-endian), read by loadLogo() in lib/GaugeUI/src/GaugeUI.cpp:
    0  "C2GL"
    4  u8  version = 1
    5  u8  flags: bit0 = RLE
    6  u16 width, u16 height (<= 480), u16 reserved = 0
    12 u32 data length
    16 data: RGB565 pixels, or RLE packets: c < 0x80 -> c + 1 literal pixels follow;
            c >= 0x80 -> the next pixel repeated (c & 0x7F) + 2 times

Needs Pillow (pip install pillow). SVG is rendered with cairosvg if it's installed, otherwise with headless Chrome /
Edge / Chromium (found automatically, or set CHROME=<path to the browser>).
"""
import argparse
import io
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

SIZE = 480
GLASS_R = SIZE / 2 * 0.88  # no logo pixel further than this from the centre (clear of the round glass edge)
ROOT = Path(__file__).resolve().parent.parent
KEYS = {"alfa", "aston", "audi", "bmw", "chevrolet", "citroen", "dodge", "fiat", "ford", "honda", "hyundai", "infiniti",
        "jaguar", "kia", "landrover", "lexus", "mazda", "mercedes", "mini", "mitsubishi", "nissan", "opel", "peugeot",
        "porsche", "renault", "saab", "seat", "skoda", "subaru", "toyota", "vauxhall", "volvo", "vw",
        "forbes", "can2gauge"}  # forbes = boot logo option 1 (Forbes Automotive), can2gauge = option 6 (tools/logo_can2gauge.py)


def pixels(img: Image.Image):
    """flat pixel sequence (getdata() is deprecated from Pillow 12, removed in 14)"""
    return img.get_flattened_data() if hasattr(img, "get_flattened_data") else img.getdata()


def find_browser():
    names = [os.environ.get("CHROME"),
             r"C:\Program Files\Google\Chrome\Application\chrome.exe",
             r"C:\Program Files (x86)\Google\Chrome\Application\chrome.exe",
             r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
             r"C:\Program Files\Microsoft\Edge\Application\msedge.exe",
             "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
             shutil.which("google-chrome"), shutil.which("chromium"), shutil.which("chromium-browser"),
             shutil.which("chrome"), shutil.which("msedge")]
    return next((n for n in names if n and Path(n).exists()), None)


def svg_via_browser(path: Path, px: int) -> Image.Image:
    """Screenshot the SVG on black in a headless browser (own throwaway profile, so a running browser is untouched)."""
    browser = find_browser()
    if not browser:
        sys.exit("SVG needs cairosvg (pip install cairosvg) or Chrome / Edge (set CHROME=<path>)")
    with tempfile.TemporaryDirectory() as tmp:
        html, png = Path(tmp) / "logo.html", Path(tmp) / "logo.png"
        html.write_text(f'<html><body style="margin:0;background:#000;overflow:hidden">'
                        f'<img src="{path.resolve().as_uri()}" style="display:block;width:{px}px;height:{px}px;'
                        f'object-fit:contain"></body></html>', encoding="utf-8")
        subprocess.run([browser, "--headless=new", "--disable-gpu", "--hide-scrollbars", "--no-first-run",
                        "--force-device-scale-factor=1", f"--user-data-dir={tmp}", f"--window-size={px},{px}",
                        f"--screenshot={png}", html.as_uri()], check=True, capture_output=True, timeout=90)
        return Image.open(png).convert("RGBA")


def load(path: Path, px: int) -> Image.Image:
    if path.suffix.lower() == ".svg":
        try:
            import cairosvg
        except ImportError:  # usual on Windows (cairo DLL) - the browser renders SVG just as well
            return svg_via_browser(path, px)
        png = cairosvg.svg2png(url=str(path), output_width=px)
        return Image.open(io.BytesIO(png)).convert("RGBA")
    return Image.open(path).convert("RGBA")


def content_mask(img: Image.Image) -> Image.Image:
    """Logo pixels: by alpha for transparent sources, by brightness for ones drawn on black."""
    alpha = img.getchannel("A")
    if alpha.getextrema()[0] < 255:
        return alpha.point(lambda v: 255 if v > 16 else 0)
    return img.convert("L").point(lambda v: 255 if v > 12 else 0)


def fit_scale(img: Image.Image, box: int) -> float:
    """Largest scale that keeps the logo inside a box x box square AND every logo pixel inside the round glass.
    Measured on the real outline, so a round badge can fill the box while a square one shrinks to keep its corners."""
    m = img.copy()
    m.thumbnail((200, 200))
    w, h = m.size
    cx, cy = (w - 1) / 2, (h - 1) / 2
    r2 = 0.0
    for i, v in enumerate(pixels(content_mask(m))):
        if v:
            x, y = i % w, i // w
            r2 = max(r2, (x - cx) ** 2 + (y - cy) ** 2)
    rmax = (r2 ** 0.5 + 0.7) * img.width / w
    return min(box / img.width, box / img.height, GLASS_R / rmax if rmax else 99)


def render_exact(path: Path) -> Image.Image:
    """--exact: the source is already laid out for the 480 px glass (tools/logo_can2gauge.py) - drawn 1:1, no trim / fit"""
    img = load(path, SIZE).resize((SIZE, SIZE), Image.LANCZOS)
    canvas = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 255))
    canvas.alpha_composite(img)
    return canvas.convert("RGB")


def render(path: Path, scale: int) -> Image.Image:
    box = SIZE * scale // 100
    img = load(path, max(2 * box, 800))  # well above the target, Lanczos down for clean edges
    bbox = content_mask(img).getbbox()  # trim margins so --scale means the logo itself
    if bbox:
        img = img.crop(bbox)
    k = fit_scale(img, box)
    img = img.resize((max(1, round(img.width * k)), max(1, round(img.height * k))), Image.LANCZOS)
    canvas = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 255))
    canvas.alpha_composite(img, ((SIZE - img.width) // 2, (SIZE - img.height) // 2))
    return canvas.convert("RGB")


def rgb565(img: Image.Image) -> list:
    return [((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3) for r, g, b in pixels(img)]


def rle(px: list) -> bytes:
    out = bytearray()
    i, n = 0, len(px)
    while i < n:
        run = 1
        while i + run < n and px[i + run] == px[i] and run < 129:
            run += 1
        if run >= 2:
            out += bytes((0x80 | (run - 2), px[i] & 0xFF, px[i] >> 8))
            i += run
            continue
        start = i
        while i < n and i - start < 128 and not (i + 1 < n and px[i + 1] == px[i]):
            i += 1
        out.append(i - start - 1)
        for p in px[start:i]:
            out += bytes((p & 0xFF, p >> 8))
    return bytes(out)


def encode(img: Image.Image) -> bytes:
    px = rgb565(img)
    packed = rle(px)
    raw = struct.pack("<%dH" % len(px), *px)
    use_rle = len(packed) < len(raw)
    data = packed if use_rle else raw
    return b"C2GL" + struct.pack("<BBHHHI", 1, 1 if use_rle else 0, img.width, img.height, 0, len(data)) + data


def preview(img: Image.Image, path: Path):
    mask = Image.new("L", img.size, 0)
    ImageDraw.Draw(mask).ellipse((0, 0, SIZE - 1, SIZE - 1), fill=255)
    out = Image.new("RGB", img.size, (40, 40, 40))
    out.paste(img, (0, 0), mask)
    path.parent.mkdir(parents=True, exist_ok=True)
    out.save(path)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="*", type=Path, help="source images (default: everything in tools/logos_src)")
    ap.add_argument("--scale", type=int, default=80, help="logo box as %% of the glass width (40-100, default 80)")
    ap.add_argument("--out", type=Path, default=ROOT / "data" / "logos", help="output folder (default data/logos)")
    ap.add_argument("--exact", action="store_true", help="draw the source 1:1 on the 480 px glass, no trim / fit (the can2gauge logo)")
    ap.add_argument("--preview", action="store_true", help="also write a round-masked PNG preview per logo")
    ap.add_argument("--sheet", action="store_true", help="also write tools/logos_preview/_sheet.png with every logo")
    a = ap.parse_args()
    files = a.files or sorted(p for p in (ROOT / "tools" / "logos_src").glob("*")
                              if p.suffix.lower() in (".svg", ".png", ".jpg", ".jpeg", ".webp"))
    if not files:
        sys.exit("no source images (put <key>.svg / <key>.png in tools/logos_src/)")
    a.out.mkdir(parents=True, exist_ok=True)
    done = []
    for f in files:
        key = f.stem.lower()
        if key not in KEYS:
            print(f"  skip {f.name}: '{key}' is not a logo key")
            continue
        if a.exact:
            img = render_exact(f)
        else:
            img = render(f, max(40, min(100, a.scale)))
        data = encode(img)
        (a.out / f"{key}.bin").write_bytes(data)
        if a.preview:  # outside data/ so uploadfs doesn't put it on the gauge
            preview(img, ROOT / "tools" / "logos_preview" / f"{key}.png")
        done.append((key, img))
        print(f"  {f.name:24s} -> {key}.bin  {len(data) / 1024:6.1f} kB")
    if a.sheet and done:
        sheet(done, ROOT / "tools" / "logos_preview" / "_sheet.png")


def sheet(items, path: Path, cell: int = 240, cols: int = 6):
    """every logo as it sits on the round glass, labelled, on one image"""
    rows = (len(items) + cols - 1) // cols
    out = Image.new("RGB", (cols * cell, rows * (cell + 26)), (40, 40, 40))
    mask = Image.new("L", (cell, cell), 0)
    ImageDraw.Draw(mask).ellipse((0, 0, cell - 1, cell - 1), fill=255)
    try:
        font = ImageFont.load_default(size=18)
    except TypeError:  # Pillow < 10.1
        font = ImageFont.load_default()
    draw = ImageDraw.Draw(out)
    for i, (key, img) in enumerate(items):
        x, y = (i % cols) * cell, (i // cols) * (cell + 26)
        out.paste(img.resize((cell, cell), Image.LANCZOS), (x, y), mask)
        draw.text((x + cell // 2, y + cell + 3), key, fill=(220, 220, 220), font=font, anchor="ma")
    path.parent.mkdir(parents=True, exist_ok=True)
    out.save(path)
    print(f"  sheet -> {path}")


if __name__ == "__main__":
    main()
