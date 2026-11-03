# obdex_to_c2g.py - can2gauge 0.38.0. Builds the gauge's fault code descriptions (data/dtc/sae.json) from OBDex.
#
# OBDex (https://github.com/foerbsnavi/OBDex) is an open list of every generic OBD-II fault code of SAE J2012 /
# ISO 15031-6 (P0, P2, P3, U0, U3, B0, C0: about 9,500 codes). Licence of the data: CC0 1.0 (public domain, no
# attribution needed; the README credits it anyway). Only each code's short English title is kept ("P0300" ->
# "Random/Multiple Cylinder Misfire Detected"): OBDex's longer descriptions, causes and repair notes are left out,
# to keep the file small and because the titles are the part that follows the standard's own names.
# Maker-specific codes (P1xxx, B1xxx, C1xxx, U1xxx) aren't in it: those show with no description.
#
# Use:
#   python tools/dtc/obdex_to_c2g.py data/dtc            (downloads OBDex's generic.min.json, about 21 MB)
#   python tools/dtc/obdex_to_c2g.py data/dtc <file>     (or reads a copy already downloaded)
#
# Output: data/dtc/sae.json, {"P0300": "Random/Multiple Cylinder Misfire Detected", ...}. The filesystem image zips
# it (tools/fs_pack.py, about 70 kB); the web UI loads it the first time it shows a fault code.

import json
import os
import sys
import urllib.request

OBDEX_URL = "https://foerbsnavi.github.io/OBDex/generic.min.json"


def load_obdex(local_file):
    """the OBDex list: from the file given, else downloaded"""
    if local_file:
        with open(local_file, encoding="utf-8") as source:
            return json.load(source)
    print("downloading " + OBDEX_URL)
    with urllib.request.urlopen(OBDEX_URL) as answer:
        return json.loads(answer.read().decode("utf-8"))


def main():
    if len(sys.argv) < 2:
        print("use: obdex_to_c2g.py <output folder> [obdex generic.json]")
        sys.exit(1)
    out_folder = sys.argv[1]
    local_file = ""
    if len(sys.argv) > 2:
        local_file = sys.argv[2]
    entries = load_obdex(local_file)
    titles = {}
    for entry in entries:
        code = str(entry.get("code", "")).strip().upper()
        title = entry.get("title", {})
        english = ""
        if isinstance(title, dict):
            english = str(title.get("en", "")).strip()
        if len(code) == 5 and english:
            titles[code] = english
    os.makedirs(out_folder, exist_ok=True)
    out_path = os.path.join(out_folder, "sae.json")
    # sorted, no spaces: the same OBDex always gives the same file
    text = json.dumps(dict(sorted(titles.items())), separators=(",", ":"), ensure_ascii=False)
    with open(out_path, "w", encoding="utf-8", newline="\n") as out_file:
        out_file.write(text)
    print("%d codes -> %s (%d kB)" % (len(titles), out_path, len(text.encode("utf-8")) // 1024))


main()
