#!/usr/bin/env python3
"""
Package a firmware release and (re)build the release index - shared across the
Forbes Automotive ESP32 projects (copy of _SharedTheme/tools/make_release.py).

Run from the project folder after building both images:

    pio run && pio run -t buildfs
    python tools/make_release.py [--notes "text"] [--channel stable|beta] [--no-ota] [--force]

Or, after dropping a V<version> folder into Releases/ by hand, just refresh
the index:

    python tools/make_release.py --reindex [--notes "text for the newest folder"]

What it does
  1. Finds the project's FW_VERSION (any header under include/ or src/ with
     `#define FW_VERSION "x.yy"`) and checks data/version.json carries the same
     "fs" version - the OTA page verifies the freshly flashed web UI against
     the release version, so they must match.
  2. Reads platformio.ini for the build dir (build_dir, falling back to
     .pio/build/<env>), the board (bootloader offset: 0x1000 on the classic
     ESP32, 0x0 on S2/S3/C3/C6) and the partition table (firmware/littlefs
     offsets) so the esp-web-tools manifest.json is right for this board.
  3. Copies bootloader.bin, partitions.bin, firmware.bin, littlefs.bin into
     Releases/V<version>/ and writes manifest.json there.
  4. Rebuilds Releases/releases.json - what the OTA tab's "Check for updates"
     fetches from GitHub - from EVERY Releases/V*/ folder holding both OTA
     images. Sizes are always taken fresh from the .bin files; notes / channel
     / date / ota are carried over from the existing index.

Commit Releases/ and releases.json together, and RUN THIS AGAIN after every
rebuild you copy into Releases/ - the index must describe the bins that are
pushed.
"""
import argparse
import configparser
import glob
import hashlib
import json
import os
import re
import shutil
import sys
from datetime import date, datetime

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RELEASES = os.path.join(ROOT, "Releases")
INDEX = os.path.join(RELEASES, "releases.json")
FS_VER = os.path.join(ROOT, "data", "version.json")
PIO_INI = os.path.join(ROOT, "platformio.ini")

FOLDER_RE = re.compile(r"^V(\d+(?:\.\d+)*)$", re.IGNORECASE)
IMAGES = ["bootloader.bin", "partitions.bin", "firmware.bin", "littlefs.bin"]
# otadata initial image, needed by esp-web-tools for a full USB install; lives in
# the Arduino framework, not the build dir.
FRAMEWORK_PARTS = os.path.join(os.path.expanduser("~"), ".platformio", "packages", "framework-arduinoespressif32", "tools", "partitions")
# can2gauge builds with its own packages_dir (platformio.ini): look there too
FRAMEWORK_PARTS_C2G = os.path.join("C:" + os.sep, ".platformio", "can2gauge-pio", "packages", "framework-arduinoespressif32", "tools", "partitions")


# ---- project discovery ---------------------------------------------------------
def find_fw_version():
    """(version, header_path) from the first `#define FW_VERSION "..."` found."""
    for pattern in ("include/*.h", "src/*.h", "include/**/*.h", "src/**/*.h"):
        for path in sorted(glob.glob(os.path.join(ROOT, pattern), recursive=True)):
            try:
                m = re.search(r'#define\s+(?:FW_VERSION|CAN2GAUGE_VERSION)\s+"([^"]+)"', open(path, encoding="utf-8", errors="ignore").read())  # can2gauge: CAN2GAUGE_VERSION (can2gauge_ver.h)
            except OSError:
                continue
            if m:
                return m.group(1), path
    sys.exit("no `#define FW_VERSION \"...\"` found under include/ or src/")


def read_pio():
    """(build_dir, board, partitions_csv, env_name, product) from platformio.ini."""
    cp = configparser.ConfigParser(interpolation=None, strict=False)
    cp.read(PIO_INI, encoding="utf-8")
    envs = [s for s in cp.sections() if s.startswith("env:")]
    if not envs:
        sys.exit("no [env:...] section in platformio.ini")
    # default_envs picks the env PlatformIO builds; the common [env] section
    # supplies whatever the chosen env doesn't set itself.
    env = envs[0][4:]
    if cp.has_section("platformio") and cp["platformio"].get("default_envs"):
        env = cp["platformio"]["default_envs"].split(",")[0].strip()
    sec = cp["env:" + env] if cp.has_section("env:" + env) else cp[envs[0]]
    common = cp["env"] if cp.has_section("env") else {}

    def opt(key, default=""):
        v = sec.get(key) or common.get(key) or default
        return v.strip()

    board = opt("board", "esp32dev")
    parts = opt("board_build.partitions")
    build_dir = None
    if cp.has_section("platformio") and cp["platformio"].get("build_dir"):
        build_dir = cp["platformio"]["build_dir"].strip()
        build_dir = re.sub(r"\$\{sysenv\.(\w+)\}", lambda m: os.environ.get(m.group(1), ""), build_dir)
    if not build_dir:
        build_dir = os.path.join(ROOT, ".pio", "build")
    build_dir = os.path.join(build_dir, env)
    return build_dir, board, parts, env


def guess_product():
    """Project name = first ancestor folder that isn't a version folder
    ("V1.05 - OTA"), "PlatformIO" or "firmware"."""
    p = ROOT
    for _ in range(4):
        name = os.path.basename(p)
        if not re.match(r"^V\d", name, re.IGNORECASE) and name.lower() not in ("platformio", "firmware"):
            return name
        p = os.path.dirname(p)
    return os.path.basename(ROOT)


def chip_family(board):
    b = board.lower()
    for tag, fam in (("c6", "ESP32-C6"), ("c3", "ESP32-C3"), ("s3", "ESP32-S3"), ("s2", "ESP32-S2"), ("h2", "ESP32-H2")):
        if tag in b:
            return fam
    return "ESP32"


def resolve_partitions_csv(name):
    """A project-relative CSV, else the framework's (default.csv etc.)."""
    if not name:
        name = "default.csv"
    for cand in (os.path.join(ROOT, name), os.path.join(FRAMEWORK_PARTS, name)):
        if os.path.isfile(cand):
            return cand
    return None


def partition_offsets(csv_path):
    """(app_offset, fs_offset, otadata_offset) from the partition CSV; defaults if unreadable."""
    app_off, fs_off, ota_off = 0x10000, None, 0xE000
    try:
        for line in open(csv_path, encoding="utf-8"):
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            cols = [c.strip() for c in line.split(",")]
            if len(cols) < 5:
                continue
            name, ptype, subtype, offset = cols[0], cols[1].lower(), cols[2].lower(), cols[3]
            off = int(offset, 0) if offset else None
            if ptype == "app" and off is not None and (subtype in ("factory", "ota_0")):
                app_off = min(app_off, off) if subtype == "factory" else (off if app_off == 0x10000 else app_off)
            if ptype == "data" and subtype in ("spiffs", "littlefs") and off is not None:
                fs_off = off
            if ptype == "data" and subtype == "ota" and off is not None:
                ota_off = off
    except (OSError, TypeError):
        pass
    return app_off, fs_off, ota_off


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def vkey(v):
    return tuple(int(x) for x in re.findall(r"\d+", v))


def part_info(folder, name):
    p = os.path.join(folder, name)
    return {"path": os.path.basename(folder) + "/" + name, "size": os.path.getsize(p), "sha256": sha256(p)}


def scan_release_folders():
    found = {}
    if not os.path.isdir(RELEASES):
        return found
    for name in os.listdir(RELEASES):
        m = FOLDER_RE.match(name)
        folder = os.path.join(RELEASES, name)
        if not m or not os.path.isdir(folder):
            continue
        if not (os.path.isfile(os.path.join(folder, "firmware.bin")) and os.path.isfile(os.path.join(folder, "littlefs.bin"))):
            print(f"  skipping {name}: no firmware.bin + littlefs.bin")
            continue
        found[m.group(1)] = folder
    return found


def rebuild_index(product, meta_for, index_path=INDEX):
    old = {}
    if os.path.isfile(index_path):
        try:
            for r in json.load(open(index_path, encoding="utf-8")).get("releases", []):
                if r.get("version"):
                    old[r["version"]] = r
        except Exception as e:  # noqa: BLE001
            print(f"  existing {index_path} unreadable ({e}) - rebuilding from scratch")

    releases = []
    for version, folder in scan_release_folders().items():
        meta = meta_for(version, old.get(version))
        releases.append({
            "version": version,
            "channel": meta.get("channel", "stable"),
            "date": meta.get("date") or datetime.fromtimestamp(os.path.getmtime(os.path.join(folder, "firmware.bin"))).date().isoformat(),
            "ota": bool(meta.get("ota", True)),
            "notes": meta.get("notes", ""),
            "firmware": part_info(folder, "firmware.bin"),
            "filesystem": part_info(folder, "littlefs.bin"),
        })
    if not releases:
        sys.exit(f"no release folders with firmware.bin + littlefs.bin under {RELEASES}")
    releases.sort(key=lambda r: vkey(r["version"]), reverse=True)
    stable = [r for r in releases if r["ota"] and r["channel"] == "stable"]
    index = {"product": product, "latest": stable[0]["version"] if stable else releases[0]["version"], "releases": releases}
    os.makedirs(os.path.dirname(os.path.abspath(index_path)), exist_ok=True)
    with open(index_path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(index, f, indent=2)
        f.write("\n")
    return index


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--notes", default=None, help="short release notes shown in the update picker")
    ap.add_argument("--channel", default=None, choices=["stable", "beta"], help="default: stable")
    ap.add_argument("--no-ota", action="store_true", help="list the release but do not offer it for OTA")
    ap.add_argument("--force", action="store_true", help="overwrite an existing Releases/V<version> folder")
    ap.add_argument("--reindex", action="store_true", help="only rebuild releases.json from the existing folders")
    ap.add_argument("--product", default=None, help="product name for the index/manifest (default: project folder name)")
    ap.add_argument("--index", default=INDEX, help="releases.json to write (default: Releases/releases.json)")
    ap.add_argument("--build", default=None, help="PlatformIO build dir holding the .bin files (default: from platformio.ini)")
    args = ap.parse_args()

    build_dir, board, parts_csv, env = read_pio()
    if args.build:
        build_dir = args.build
    product = args.product or guess_product()
    target = None

    if not args.reindex:
        version, ver_h = find_fw_version()
        try:
            fs_version = json.load(open(FS_VER, encoding="utf-8")).get("fs")
        except Exception as e:  # noqa: BLE001
            sys.exit(f"cannot read {FS_VER}: {e} - data/version.json must hold {{\"fs\":\"{version}\"}}")
        if fs_version != version:
            sys.exit(f"data/version.json says fs={fs_version!r} but FW_VERSION is {version!r} ({ver_h}) - fix and rebuild the filesystem")

        for name in IMAGES:
            if not os.path.isfile(os.path.join(build_dir, name)):
                sys.exit(f"missing {name} in {build_dir} - run 'pio run' and 'pio run -t buildfs' first")

        dest = os.path.join(RELEASES, "V" + version)
        if os.path.isdir(dest) and not args.force:
            sys.exit(f"{dest} already exists - use --force to overwrite")
        os.makedirs(dest, exist_ok=True)
        for name in IMAGES:
            shutil.copy2(os.path.join(build_dir, name), os.path.join(dest, name))
        boot_app0 = None
        for cand in (os.path.join(build_dir, "boot_app0.bin"), os.path.join(FRAMEWORK_PARTS, "boot_app0.bin"), os.path.join(FRAMEWORK_PARTS_C2G, "boot_app0.bin")):
            if os.path.isfile(cand):
                shutil.copy2(cand, os.path.join(dest, "boot_app0.bin"))
                boot_app0 = cand
                break

        fam = chip_family(board)
        csv_path = resolve_partitions_csv(parts_csv)
        app_off, fs_off, ota_off = partition_offsets(csv_path) if csv_path else (0x10000, None, 0xE000)
        boot_off = 0x1000 if fam == "ESP32" else 0x0
        parts = [("bootloader.bin", boot_off), ("partitions.bin", 0x8000)]
        if boot_app0:
            parts.append(("boot_app0.bin", ota_off))
        parts.append(("firmware.bin", app_off))
        if fs_off is not None:
            parts.append(("littlefs.bin", fs_off))
        manifest = {"name": product, "version": version, "builds": [{"chipFamily": fam, "parts": [{"path": n, "offset": off} for n, off in parts]}]}
        with open(os.path.join(dest, "manifest.json"), "w", encoding="utf-8", newline="\n") as f:
            json.dump(manifest, f, indent=2)
            f.write("\n")

        print(f"packaged V{version} ({fam}, env {env}, partitions {os.path.basename(csv_path) if csv_path else 'default'}) -> {dest}")
        for name in IMAGES + (["boot_app0.bin"] if boot_app0 else []):
            print(f"  {name:<16}{os.path.getsize(os.path.join(dest, name)):>9} bytes")
        target = version
    else:
        known = set()
        if os.path.isfile(args.index):
            try:
                known = {r.get("version") for r in json.load(open(args.index, encoding="utf-8")).get("releases", [])}
            except Exception:  # noqa: BLE001
                pass
        new = [v for v in scan_release_folders() if v not in known]
        if new:
            target = max(new, key=vkey)

    def meta_for(version, old_entry):
        meta = dict(old_entry or {})
        if version == target:
            meta["date"] = date.today().isoformat() if old_entry is None else meta.get("date", date.today().isoformat())
            if args.notes is not None:
                meta["notes"] = args.notes
            if args.channel is not None:
                meta["channel"] = args.channel
            if args.no_ota:
                meta["ota"] = False
        return meta

    print("rebuilding release index" + (" (--reindex)" if args.reindex else ""))
    index = rebuild_index(product, meta_for, args.index)
    for r in index["releases"]:
        flag = "" if r["ota"] else "  [no OTA]"
        print(f"  V{r['version']:<8} {r['channel']:<6} {r['date']}  fw {r['firmware']['size']:>8}  fs {r['filesystem']['size']:>7}{flag}  {r['notes'][:60]}")
    print(f"wrote {args.index} (latest = {index['latest']})")


if __name__ == "__main__":
    main()
