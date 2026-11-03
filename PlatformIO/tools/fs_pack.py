# fs_pack.py - can2gauge 0.24.0. A PlatformIO "pre" script (platformio.ini extra_scripts): when the filesystem image
# is built (Build / Upload Filesystem Image, pio run -t buildfs / uploadfs / uploadfsota), it makes a packed copy of
# data/ in the build folder and builds the image from that instead. data/ itself stays plain, so the files are edited
# as before and the simulator serves them as they are.
#
# What gets zipped (the image was 97.5 % full; now about half):
#   the web UI's scripts and styles (top folder *.js, *.css)  -> name.gz   the web server sends them zipped as they are
#                                                                          (the browser unzips); index.html stays plain:
#                                                                          the shared WiFi module serves it as it is
#                                                                          (0.38.0: its indentation is taken out, 131 ->
#                                                                          109 kB); app.js is kept plain for the OTA
#                                                                          module's check AND zipped beside it (0.38.0):
#                                                                          the web server sends the .gz (257 -> 79 kB)
#   the make logos (logos/*.bin)                              -> .bin.gz   unzipped into memory as the boot logo loads
#   the nav card's turn arrows (nav/*.bin, 0.30.1)            -> .bin.gz   unzipped into memory when a turn needs one
#   the label index (lbl/index.txt)                      -> .txt.gz   unzipped into memory the first time it's used
#   the DID library (vdid/*.json, 0.34.0)                      -> .json.gz  the web server sends them zipped as they are
#   the fault code descriptions (dtc/sae.json, 0.38.0)          -> .json.gz  the same
#   the car pictures (cars/*.svg + index.json, 0.35.0)          -> .gz       the same
#   the sprite packs (sprites/*.c2gs)                         -> C2GS version 2, every frame zipped on its own
#                                                                          (tools/c2gs_pack.py); unzipped frame by frame
# Everything else is copied as it is. The gauge reads a plain file first and the .gz when there isn't one
# (lib/FsUnzip), so an older image still works.
Import("env")

import gzip
import os
import shutil
import sys

FS_TARGETS = {"buildfs", "uploadfs", "uploadfsota"}
WEB_ZIP = (".js", ".css")  # top-folder files zipped for the web server
# kept plain: the shared OTA module (src/ota_manager.cpp, left as it is) decides the web UI is on the gauge by finding
# /index.html AND /app.js - a zipped app.js made it serve its recovery page instead (0.24.0, first flash)
# 0.38.0: so app.js goes in twice, plain and zipped: the web server (ESPAsyncWebServer's serveStatic) tries name.gz
# first, so the browser gets the zipped one, and the OTA module still finds /app.js
WEB_KEEP_PLAIN = ("app.js",)


def zip_file(source, target):
    """gzip, most packing, no time stamp inside (the same data/ always gives the same image)"""
    data = open(source, "rb").read()
    open(target, "wb").write(gzip.compress(data, 9, mtime=0))


def strip_indent(source, target):
    """0.38.0: index.html without the spaces at the start of each line (the browser doesn't need them; there is no
    <pre> or <textarea> in it whose text they belong to)"""
    text = open(source, encoding="utf-8").read()
    lines = text.split("\n")
    stripped = []
    for line in lines:
        stripped.append(line.strip())
    open(target, "w", encoding="utf-8", newline="\n").write("\n".join(stripped))


def build_packed_data(data_dir, packed_dir, tools_dir):
    sys.path.insert(0, tools_dir)
    from c2gs_pack import pack_c2gs  # the sprite repacker sits next to this script

    if os.path.isdir(packed_dir):
        shutil.rmtree(packed_dir)
    plain_bytes = 0
    packed_bytes = 0
    for folder, _, files in os.walk(data_dir):
        relative = os.path.relpath(folder, data_dir)
        top = relative == "."
        out_folder = os.path.join(packed_dir, relative)
        os.makedirs(out_folder, exist_ok=True)
        for name in files:
            source = os.path.join(folder, name)
            lower = name.lower()
            subfolder = relative.replace("\\", "/")
            if top and lower.endswith(WEB_ZIP) and lower not in WEB_KEEP_PLAIN:
                target = os.path.join(out_folder, name + ".gz")
                zip_file(source, target)
            elif top and lower in WEB_KEEP_PLAIN:
                # 0.38.0: the plain copy for the OTA module's check, and the zipped one the browser is sent
                shutil.copyfile(source, os.path.join(out_folder, name))
                target = os.path.join(out_folder, name + ".gz")
                zip_file(source, target)
                packed_bytes += os.path.getsize(os.path.join(out_folder, name))
            elif top and lower == "index.html":
                target = os.path.join(out_folder, name)
                strip_indent(source, target)  # 0.38.0
            elif subfolder in ("logos", "nav") and lower.endswith(".bin"):  # 0.30.1: + the nav card's turn arrows
                target = os.path.join(out_folder, name + ".gz")
                zip_file(source, target)
            elif (subfolder in ("vdid", "dtc") and lower.endswith(".json")) or (subfolder == "cars" and lower.endswith((".svg", ".json"))):
                # 0.34.0: the DID library; 0.35.0: the car pictures; 0.38.0: the fault code descriptions - all sent
                # zipped to the web UI
                target = os.path.join(out_folder, name + ".gz")
                zip_file(source, target)
            elif subfolder == "lbl" and lower == "index.txt":
                target = os.path.join(out_folder, name + ".gz")
                zip_file(source, target)
            elif subfolder == "sprites" and lower.endswith(".c2gs"):
                target = os.path.join(out_folder, name)
                open(target, "wb").write(pack_c2gs(open(source, "rb").read()))
            else:
                target = os.path.join(out_folder, name)
                shutil.copyfile(source, target)
            plain_bytes += os.path.getsize(source)
            packed_bytes += os.path.getsize(target)
    write_manifest(packed_dir)
    print("fs_pack: data/ %d kB -> %d kB packed (%s)" % (plain_bytes // 1024, packed_bytes // 1024, packed_dir))


MANIFEST = "fs_manifest.txt"


def write_manifest(packed_dir):
    """0.25.0: the list of the image's own files, one "/path" a line (src/can2gauge_ChainUpdate.cpp). A chain update
    sends only these to the other gauges, and deletes those an older list had that the new one doesn't - each gauge's
    own files (.ecu, logs, uploads) are on neither list, so they're left alone."""
    paths = []
    for folder, _, files in os.walk(packed_dir):
        for name in files:
            relative = os.path.relpath(os.path.join(folder, name), packed_dir).replace("\\", "/")
            if relative != MANIFEST:
                paths.append("/" + relative)
    paths.sort()
    paths.append("/" + MANIFEST)  # the list itself, last
    with open(os.path.join(packed_dir, MANIFEST), "w", newline="\n") as manifest:
        manifest.write("\n".join(paths) + "\n")
    print("fs_pack: %s lists %d files" % (MANIFEST, len(paths)))


if FS_TARGETS & set(COMMAND_LINE_TARGETS):  # only when a filesystem image is being built
    data_dir = env.subst("$PROJECT_DATA_DIR")
    packed_dir = os.path.join(env.subst("$BUILD_DIR"), "fsdata")
    tools_dir = os.path.join(env.subst("$PROJECT_DIR"), "tools")
    build_packed_data(data_dir, packed_dir, tools_dir)
    env.Replace(PROJECT_DATA_DIR=packed_dir)  # the image builder reads the packed copy
