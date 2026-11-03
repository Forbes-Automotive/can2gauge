#!/usr/bin/env python3
"""label files -> the gauge's measuring-block labels (K-line mode, src/can2gauge_Labels.cpp).

  python tools/labels/lbl_tool.py pack [SRC...]  SRC folders, newest first (default docs/Labels) -> data/lbl/
                                                 a file name found in several folders is taken from the first one
  python tools/labels/lbl_tool.py bundle          data/lbl/<FILE>.txt files (an older pack) -> data/lbl/labels.pak
  python tools/labels/lbl_tool.py find IDENT ADDR   which label files the gauge would pick for a module, best first
  python tools/labels/lbl_tool.py show FILE       one label file out of the pack

Only the plain-text .lbl files are used. The .clb / .crd files are not plain text and are skipped. A
REDIRECT pointing at a skipped file is followed when an older label folder has that file as a plain .lbl (an older release
shipped many modules in plain text that are .clb now); otherwise it is skipped.

data/lbl/labels.pak   every label file, each raw-deflated on its own (the gauge inflates one with the ESP32 ROM's
                      tinfl; 595 small files would waste ~1.4 MB in LittleFS 4 KB blocks). Little-endian:
                        "LBLP", u16 version 1, u16 count, then count x {char name[40], u32 offset, u32 packed, u32 size}
                        sorted by name (the gauge binary-searches it), then the deflate streams.
                      A label file: one line per measuring-block field, tab separated: group, field (0 = the block's
                      title), name, second name line, help / specification (cut to 80 characters). ASCII + degree sign.
data/lbl/index.txt    how a module finds its file, tab separated: rank, mask, address (hex, "" = any), file, variant
                      rank 1  the part number with its index (037-906-023-R.lbl, REDIRECT ...,1C0-906-032-AB)
                      rank 2  a REDIRECT for a whole part number (any index)
                      rank 3  a file for the part number, split by engine code / variant (06A-906-032-AWP.lbl), or a
                              wildcard file for an address (1J0-920-xx0-17.lbl)
                      rank 5  a chassis + address file (1J-17.lbl): the part number starts with the chassis code
                      masks are part numbers without dashes, '?' = any character; a 9-character mask matches any index
The gauge takes the lowest rank; among equals it prefers a variant named in the module's identification text
("ESP" in "ABS/ESP allrad"), then the first. The web UI can pick another (/lbl/choice.txt).
"""
import os
import re
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
SRC = os.path.join(ROOT, "docs", "Labels")
DST = os.path.join(ROOT, "data", "lbl")

PN = re.compile(r"^([0-9A-Z]{3})-([0-9A-Z]{3})-([0-9A-Z]{3})(?:-(.+))?$")
CHASSIS = re.compile(r"^([0-9A-Z]{2})-([0-9A-F]{2})$")
FOLD = {"ä": "ae", "ö": "oe", "ü": "ue", "Ä": "Ae", "Ö": "Oe", "Ü": "Ue", "ß": "ss",
        "µ": "u", "²": "2", "³": "3", "…": "...", "–": "-", "—": "-", "±": "+/-",
        "‘": "'", "’": "'", "“": '"', "”": '"', "é": "e", "è": "e", " ": " "}


def read(p):
    raw = open(p, "rb").read()
    try:
        return raw.decode("utf-8")
    except UnicodeDecodeError:
        return raw.decode("cp1252", errors="replace")


def clean(s, n):
    """the panel fonts: ASCII and the degree sign"""
    s = s.replace("\\n", " | ").replace("&&", "&")  # label-file escapes: a line break in help text, a literal "&"
    s = "".join(FOLD.get(c, c) for c in s.replace("\t", " ").replace("�", "°"))
    s = "".join(c if 32 <= ord(c) < 127 or c == "°" else "?" for c in s)
    return re.sub(r"\s+", " ", s).strip()[:n]


def blocks(text):
    out = []
    for line in text.splitlines():
        m = re.match(r"^\s*(\d{1,3}),(\d),(.*)$", line)
        if not m or int(m.group(1)) > 255 or int(m.group(2)) > 4:
            continue
        rest = m.group(3).split(";")[0] if not m.group(3).lstrip().startswith(";") else ""
        parts = [p for p in rest.split(",", 2)] + ["", ""]
        name, name2, help_ = clean(parts[0], 39), clean(parts[1], 39), clean(parts[2], 80)
        if name or name2:
            out.append("%d\t%s\t%s\t%s\t%s" % (int(m.group(1)), m.group(2), name, name2, help_))
    return out


PAK = "labels.pak"
PAK_NAME = 40


def write_pak(texts, dst=DST):
    """{FILE: text} -> data/lbl/labels.pak"""
    names = sorted(texts)
    head = struct.pack("<4sHH", b"LBLP", 1, len(names))
    table, body = bytearray(), bytearray()
    at = len(head) + len(names) * (PAK_NAME + 12)
    for n in names:
        raw = texts[n].encode("utf-8")
        c = zlib.compressobj(9, zlib.DEFLATED, -15)  # raw deflate: what tinfl_decompress_mem_to_mem takes with flags 0
        packed = c.compress(raw) + c.flush()
        if len(n.encode()) >= PAK_NAME:
            raise SystemExit("label file name too long: " + n)
        table += struct.pack("<%dsIII" % PAK_NAME, n.encode().ljust(PAK_NAME, b"\0"), at + len(body), len(packed), len(raw))
        body += packed
    with open(os.path.join(dst, PAK), "wb") as f:
        f.write(head + bytes(table) + bytes(body))
    return len(head) + len(table) + len(body)


def read_label(name, dst=DST):
    """one label file's text from the pack (None if it isn't there) - the simulator reads them this way"""
    p = os.path.join(dst, PAK)
    if not name or not os.path.isfile(p):
        return None
    data = open(p, "rb").read()
    magic, ver, count = struct.unpack_from("<4sHH", data, 0)
    for i in range(count):
        n, off, packed, size = struct.unpack_from("<%dsIII" % PAK_NAME, data, 8 + i * (PAK_NAME + 12))
        if n.rstrip(b"\0").decode() == name.upper():
            return zlib.decompress(data[off:off + packed], -15).decode("utf-8")
    return None


def bundle(dst=DST):
    """an older data/lbl (one .txt per label file) -> labels.pak; the .txt files go"""
    texts = {}
    for f in os.listdir(dst):
        if f.endswith(".txt") and f not in ("index.txt", "choice.txt", "known.txt"):
            texts[f[:-4].upper()] = open(os.path.join(dst, f), encoding="utf-8").read()
    if not texts:
        raise SystemExit("no label .txt files in " + dst)
    size = write_pak(texts, dst)
    for n in texts:
        os.remove(os.path.join(dst, n + ".txt"))
    print("bundle: %d label files -> %s (%d kB)" % (len(texts), PAK, size // 1024))


def mask_of(base_parts, suffix=""):
    return ("".join(base_parts) + suffix).upper().replace("X", "?").replace("-", "")


def pack(srcs=(SRC,)):
    os.makedirs(DST, exist_ok=True)
    for f in os.listdir(DST):
        os.remove(os.path.join(DST, f))
    lbl = {}
    for src in srcs:  # newest first: the first folder with a file name wins
        for f in os.listdir(src):
            if f.lower().endswith(".lbl"):
                lbl.setdefault(os.path.splitext(f)[0].upper(), os.path.join(src, f))
    have, texts, index = set(), {}, []
    for name, path in sorted(lbl.items()):
        b = blocks(read(path))
        if b:
            texts[name] = "\n".join(b) + "\n"
            have.add(name)
    size = write_pak(texts)
    for name, path in sorted(lbl.items()):
        m = PN.match(name)
        if m and name in have:
            base, suffix = m.groups()[:3], m.group(4) or ""
            toks = suffix.split("-") if suffix else []
            addr = toks.pop() if toks and re.fullmatch(r"[0-9A-F]{2}", toks[-1]) and "X" in "".join(base) else ""
            variant = "-".join(toks)
            if variant and re.fullmatch(r"[0-9A-Z]{1,2}", variant) and not addr:
                index.append((1, mask_of(base, variant), "", name, ""))  # the index letters: 037-906-023-R
            index.append((3, mask_of(base), addr, name, variant))
        c = CHASSIS.match(name)
        if c and name in have:
            index.append((5, c.group(1), c.group(2), name, ""))
        # REDIRECT,TARGET.LBL,mask[,mask...] ; comment
        for line in read(path).splitlines():
            if not line.startswith("REDIRECT"):
                continue
            f = [x.strip() for x in line.split(";")[0].split(",")]
            if len(f) < 3:
                continue
            tgt = os.path.splitext(f[1])[0].upper()
            if tgt not in have:
                continue  # not plain text (.CLB) with no plain .lbl of that name, or no measuring blocks
            for tok in f[2:]:
                tok = tok.upper()
                if not tok:
                    continue
                if "-" not in tok and len(tok) <= 3 and m:  # an index relative to this file's part number
                    mk = mask_of(m.groups()[:3], tok)
                else:
                    mk = tok.replace("-", "").replace("X", "?")
                if not re.fullmatch(r"[0-9A-Z?]{9,14}", mk):
                    continue
                index.append((1 if len(mk) > 9 else 2, mk, "", tgt, ""))
    seen, rows = set(), []
    for r in sorted(index, key=lambda r: (r[0], r[1], r[3])):
        if (r[1], r[2], r[3]) not in seen:
            seen.add((r[1], r[2], r[3]))
            rows.append("%d\t%s\t%s\t%s\t%s" % r)
    with open(os.path.join(DST, "index.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(rows) + "\n")
    size += os.path.getsize(os.path.join(DST, "index.txt"))
    print("pack: %d label files with measuring blocks, %d index rows -> data/lbl/ (%d kB)" % (len(have), len(rows), size // 1024))


# ---- the gauge's lookup (src/can2gauge_Labels.cpp lblFind), for checking ----------------------------------------------
def part_of(ident):
    """'06A906032HN  1.8L R4/5VT ...' -> '06A906032HN' (the part number ends at the first double space)"""
    return re.split(r"\s{2,}", ident.strip())[0].replace(" ", "").replace("-", "").upper()[:14]


def mask_match(mask, part):
    """'?' = any character; a 9-character mask is the part number with any index, and a mask's trailing '?' also
    match a shorter index (1J?920??6??? matches 1J0920826C)"""
    if len(mask) == 9:
        part = part[:9]
    if len(part) > len(mask) or len(part) < 9:
        return False
    return all(m == "?" or (i < len(part) and m == part[i]) for i, m in enumerate(mask)) and         all(m == "?" for m in mask[len(part):])


def find(ident, addr, index_path=os.path.join(DST, "index.txt")):
    part, desc = part_of(ident), ident.upper()
    found = []
    for line in open(index_path, encoding="utf-8"):
        rank, mask, a, name, variant = line.rstrip("\n").split("\t")
        if a and int(a, 16) != addr:
            continue
        ok = part.startswith(mask) if rank == "5" else mask_match(mask, part)
        if ok:
            score = sum(1 for t in variant.split("-") if len(t) >= 3 and t in desc)
            found.append((int(rank), -score, name))
    out, seen = [], set()
    for r in sorted(found, key=lambda x: (x[0], x[1])):
        if r[2] not in seen:
            seen.add(r[2])
            out.append(r[2])
    return out


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if cmd == "pack":
        pack(sys.argv[2:] or [SRC])
    elif cmd == "bundle":
        bundle()
    elif cmd == "find" and len(sys.argv) > 3:
        print(find(sys.argv[2], int(sys.argv[3], 16)))
    elif cmd == "show" and len(sys.argv) > 2:
        print(read_label(sys.argv[2]) or "not in the pack")
    else:
        print(__doc__)
        sys.exit(1)
