#!/usr/bin/env python3
"""
c2gs_pack.py - zips a C2GS sprite pack's frames: version 1 -> version 2 (include/c2gs.h, can2gauge 0.24.0).

Each frame's planes are zipped on their own (raw deflate), so the gauge unzips one frame at a time, just before it
decodes it - the pack is never unpacked as a whole. Rev goes from 5.9 MB to 2.8 MB. Frames that share data in the
pack still share it. The header, tables and palettes are kept as they are; only the version (2), the C2GS_DEFLATE
flag and each frame's table entry change (len[3] = the zipped size).

    py tools/c2gs_pack.py rev.c2gs rev_zipped.c2gs       a pack to upload from the web UI (firmware 0.24.0 or later)

tools/fs_pack.py calls pack_c2gs() for every pack in data/sprites when the filesystem image is built, so the packs
in data/ stay version 1 (the simulator reads those).
"""
import struct
import sys
import zlib

HEADER_BYTES = 32
CLIP_BYTES = 32
STATE_BYTES = 24
TEXT_BYTES = 32
FRAME_BYTES = 20      # offset + len[4], all uint32
C2GS_DEFLATE = 0x08   # header flag: frames zipped
ZIPPED_VERSION = 2


def deflate_raw(data):
    """raw deflate (no zlib / gzip header), the most packing - what the gauge's ROM tinfl unzips"""
    packer = zlib.compressobj(9, zlib.DEFLATED, -15)
    return packer.compress(data) + packer.flush()


def pack_c2gs(data):
    """a version 1 pack (bytes) -> the same pack with its frames zipped (bytes). A version 2 pack comes back as it is."""
    if data[:4] != b"C2GS":
        raise ValueError("not a C2GS sprite pack")
    version = data[4]
    flags = data[5]
    if version == ZIPPED_VERSION:
        return data  # already zipped
    if version != 1:
        raise ValueError("unknown C2GS version %d" % version)
    frame_count = struct.unpack_from("<H", data, 10)[0]
    state_count = data[12]
    text_count = data[13]
    clip_count = data[14]
    frames_at = HEADER_BYTES + clip_count * CLIP_BYTES + state_count * STATE_BYTES + text_count * TEXT_BYTES

    frames = []
    for i in range(frame_count):
        offset, len0, len1, len2, len3 = struct.unpack_from("<5I", data, frames_at + i * FRAME_BYTES)
        if len3:
            raise ValueError("frame %d uses plane 3 - version 2 needs it free for the zipped size" % i)
        frames.append((offset, len0, len1, len2))
    first_frame_data = min(frame[0] for frame in frames)

    # the palettes (layered packs) must sit before the frame data, so their offsets stay right
    for clip in range(clip_count):
        clip_at = HEADER_BYTES + clip * CLIP_BYTES
        pal_count = struct.unpack_from("<H", data, clip_at + 12)[0]
        pal_offset = struct.unpack_from("<I", data, clip_at + 16)[0]
        if pal_count and pal_offset + pal_count * 4 > first_frame_data:
            raise ValueError("clip %d's palette is after the frame data - can't repack this pack" % clip)

    out = bytearray(data[:first_frame_data])  # header, tables, palettes, unchanged
    out[4] = ZIPPED_VERSION
    out[5] = flags | C2GS_DEFLATE
    zipped_at = {}  # original frame offset -> (new offset, zipped size): shared frames stay shared
    for i, (offset, len0, len1, len2) in enumerate(frames):
        if offset not in zipped_at:
            planes = data[offset: offset + len0 + len1 + len2]
            zipped = deflate_raw(planes)
            zipped_at[offset] = (len(out), len(zipped))
            out += zipped
        new_offset, zipped_len = zipped_at[offset]
        struct.pack_into("<5I", out, frames_at + i * FRAME_BYTES, new_offset, len0, len1, len2, zipped_len)
    return bytes(out)


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: c2gs_pack.py in.c2gs out.c2gs")
    data = open(sys.argv[1], "rb").read()
    packed = pack_c2gs(data)
    open(sys.argv[2], "wb").write(packed)
    print("%s: %d kB -> %d kB" % (sys.argv[1], len(data) // 1024, len(packed) // 1024))


if __name__ == "__main__":
    main()
