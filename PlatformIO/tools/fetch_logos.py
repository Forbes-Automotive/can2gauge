#!/usr/bin/env python3
"""
Fetch a full-colour boot-logo set for every make in the theme list into tools/logos_src/<key>.svg, then build the
gauge files with logo_convert.py:

    python tools/fetch_logos.py                 # every make (skips ones already downloaded)
    python tools/fetch_logos.py vw bmw --force  # re-fetch just these
    python tools/logo_convert.py --preview --sheet

Source: Wikimedia Commons (Special:FilePath), one ordered list of candidate files per make - the first one that
exists is used. The picks favour the colour / chrome badge versions, which read well on the gauge's black screen.
Commons rate-limits hard, so requests are spaced out and retried on HTTP 429.

Dark parts of a logo (black outlines, black or navy text drawn for a white page) disappear on black. Makes listed in
LIFT_DARK get every colour darker than LIFT_L raised to it with the hue kept (navy -> readable blue, black / grey ->
silver) before rendering; everything else keeps its own colours.

The logos are trademarks of their owners - fine for trying the look on your own gauge; check the terms before
distributing them.
"""
import argparse
import colorsys
import io
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "tools" / "logos_src"
UA = "can2gauge-logo-fetch/1.0 (personal, non-commercial gauge preview)"
COMMONS = "https://commons.wikimedia.org/wiki/Special:FilePath/{}"
ENWIKI = "https://en.wikipedia.org/wiki/Special:FilePath/{}"  # "en:" names: badges only English Wikipedia hosts
GAP_S = 3.0  # between requests (Commons answers 429 to bursts)

# logo key (include/can2gauge_themes.h) -> Commons file names, best first
LOGOS = {
    "alfa": ["en:Alfa Romeo logo.png", "Alfa Romeo logo.svg"],
    "aston": ["Aston Martin Lagonda brand logo.png", "Aston Martin logo.svg", "Aston Martin wordmark.svg"],
    "audi": ["Audi logo detail.svg", "Audi-Logo 2016.svg", "Audi logo.svg"],
    "bmw": ["BMW.svg", "BMW logo (gray).svg"],
    "chevrolet": ["Chevrolet-logo.svg", "Chevrolet simple logo.svg", "Logo Chevrolet.svg"],
    "citroen": ["Citroen 2016 logo.svg", "Citroën Logo.svg", "Citroën-Logo.svg"],
    "dodge": ["Dodge Logo 2010.svg", "Dodge logo.svg"],
    "fiat": ["FIAT logo coloured.svg", "Fiat Automobiles logo.svg", "Fiat logo.svg"],
    "ford": ["Ford logo flat.svg", "Ford Motor Company Logo.svg"],
    "honda": ["Honda.svg", "Honda auto logo.svg", "Honda Logo.svg", "Honda logo.svg"],
    "hyundai": ["Hyundai Motor Company logo.svg", "Hyundai logo.svg", "Hyundai Logo.svg"],
    "infiniti": ["Infiniti logo.svg", "Infiniti Logo.svg", "Logo of Infiniti.svg"],
    "jaguar": ["en:Jaguar-Logo.svg", "en:Jaguar logo 2021.svg", "Jaguar logo.svg"],
    "kia": ["Kia Motors 2002-2012 Logo.svg", "Kia Motors logo 2010.svg", "KIA logo3.svg"],
    "landrover": ["Land Rover 1978 logo.svg", "Land Rover logo.svg", "Land Rover 2023.svg"],
    "lexus": ["Lexus.svg", "Lexus Logo.svg"],
    "mazda": ["Mazda logo with emblem, new.svg", "Mazda logo.svg", "Mazda logo 2024.svg"],
    "mercedes": ["Mercedes-Benz Logo 2010.svg", "Mercedes-Benz logo 2.svg", "Mercedes-Benz free logo.svg"],
    "mini": ["MINI logo.svg", "Mini logo.svg"],
    "mitsubishi": ["Mitsubishi logo.svg", "Mitsubishi Motors SVG logo.svg", "Mitsubishi motors new logo.svg"],
    "nissan": ["Nissan logo 2001.svg", "Nissan logo.svg", "Nissan 2020 logo.svg"],
    "opel": ["Opel-Logo 2017.svg", "Opel Logo 2021.svg", "Opel logo.svg"],
    "peugeot": ["Peugeot Logo.svg", "Peugeot logo.svg"],
    "porsche": ["en:Porsche Logo 2024.png", "Porsche Wordmark Logo Black.svg"],
    "renault": ["Renault 2009 logo.svg", "Renault logo.svg", "Renault 2021 Text.svg"],
    "saab": ["Saab automobile badge.svg", "Saab wordmark blue.svg", "Saab wordmark grey.svg"],
    "seat": ["SEAT Logo from 2017.svg", "SEAT Logo.svg", "Seat logo.svg"],
    "skoda": ["Skoda-Auto-Logo-2011-present.svg", "Škoda Auto.svg"],
    "subaru": ["Subaru logo (transparent).svg", "Subaru (2019).svg", "Subaru logo.svg"],
    "toyota": ["Toyota logo (Red).svg", "Toyota Logo.svg", "Toyota logo.svg"],
    "vauxhall": ["Vauxhall logo 2008.svg", "Vauxhall logo.svg", "Vauxhall 1990s.svg", "Vauxhall logo 2026.svg"],
    "volvo": ["Volvo logo.svg", "Volvo-Iron-Mark-Black.svg"],
    "vw": ["Volkswagen Logo 2012.svg", "Volkswagen logo 2012.svg", "Volkswagen - Logo.svg", "Volkswagen logo 2019.svg"],
}
# makes whose logo is drawn for a white page (black / navy parts vanish on the black glass): colours darker than
# LIFT_L are raised to it with the hue kept (navy -> readable blue), greys and black go to a light silver
LIFT_DARK = {"alfa", "aston", "hyundai", "infiniti", "lexus", "mercedes", "mini", "opel", "peugeot", "saab", "seat",
             "subaru", "vauxhall"}
LIFT_L = 0.55
SILVER_L = 0.82


def lift_rgb(r: int, g: int, b: int):
    h, l, s = colorsys.rgb_to_hls(r / 255, g / 255, b / 255)
    if s < 0.15 and l < SILVER_L:  # black / grey -> silver
        l = SILVER_L
    elif l < LIFT_L:
        l = LIFT_L
    else:
        return r, g, b
    return tuple(round(c * 255) for c in colorsys.hls_to_rgb(h, l, s))


def fetch(name: str) -> bytes:
    base = ENWIKI if name.startswith("en:") else COMMONS
    url = base.format(urllib.parse.quote(name.removeprefix("en:").replace(" ", "_")))
    for attempt in range(5):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": UA})
            with urllib.request.urlopen(req, timeout=30) as r:
                return r.read()
        except urllib.error.HTTPError as e:
            if e.code == 429:
                time.sleep(10 * (attempt + 1))
                continue
            raise
    raise RuntimeError("rate limited")


def lift_dark(svg: str) -> str:
    """every fill / stroke / gradient colour through lift_rgb, plus a light default fill for shapes with none
    (an SVG's default fill is black)"""
    def repl(m):
        h = m.group(2)
        if len(h) == 3:
            h = "".join(c * 2 for c in h)
        return m.group(1) + "%02X%02X%02X" % lift_rgb(*(int(h[i:i + 2], 16) for i in (0, 2, 4)))
    svg = re.sub(r"((?:fill|stroke|stop-color)\s*[:=]\s*[\"']?#)([0-9a-fA-F]{6}|[0-9a-fA-F]{3})\b", repl, svg)
    silver = "#%02X%02X%02X" % lift_rgb(0, 0, 0)
    svg = re.sub(r"((?:fill|stroke|stop-color)\s*[:=]\s*[\"']?)(black|currentColor)\b", lambda m: m.group(1) + silver, svg)
    return re.sub(r"<svg\b(?![^>]*\bfill=)", f'<svg fill="{silver}"', svg, count=1)


def lift_png(data: bytes) -> bytes:
    from PIL import Image  # only needed for raster logos that need lifting
    img = Image.open(io.BytesIO(data)).convert("RGBA")
    px = img.load()
    for y in range(img.height):
        for x in range(img.width):
            r, g, b, a = px[x, y]
            if a:
                px[x, y] = (*lift_rgb(r, g, b), a)
    out = io.BytesIO()
    img.save(out, "PNG")
    return out.getvalue()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("keys", nargs="*", help="only these logo keys (default: all 33)")
    ap.add_argument("--force", action="store_true", help="download again even if tools/logos_src has the file")
    a = ap.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    failed = []
    for key, names in LOGOS.items():
        if a.keys and key not in a.keys:
            continue
        if not a.force and any((OUT / f"{key}{ext}").exists() for ext in (".svg", ".png")):
            print(f"  {key:11s} (have it)")
            continue
        for name in names:
            time.sleep(GAP_S)
            try:
                data = fetch(name)
            except Exception:
                continue
            ext = Path(name).suffix.lower()
            if ext == ".svg":
                text = data.decode("utf-8", "replace")
                if "<svg" not in text[:4000]:
                    continue
                if key in LIFT_DARK:
                    text = lift_dark(text)
                data = text.encode("utf-8")
            elif key in LIFT_DARK:
                data = lift_png(data)
            for old in OUT.glob(f"{key}.*"):
                old.unlink()
            (OUT / f"{key}{ext}").write_bytes(data)
            print(f"  {key:11s} <- {name}")
            break
        else:
            failed.append(key)
            print(f"  {key:11s} NOT FOUND (tried {len(names)} names)")
    if failed:
        sys.exit("missing: " + ", ".join(failed))


if __name__ == "__main__":
    main()
