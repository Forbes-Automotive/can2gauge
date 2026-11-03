#pragma once
#include <Arduino.h>

// ---------------------------------------------------------------------------
// FsUnzip (0.24.0) - files that ship zipped on the gauge's LittleFS.
//
// The filesystem image is built from data/ by tools/fs_pack.py, which zips the big files so they fit with room to
// spare (the image was 97.5 % full): the make logos (/logos/<key>.bin.gz), the label index
// (/lbl/index.txt.gz), the web UI's scripts and styles (served zipped as they are), and each sprite pack's frames
// (C2GS version 2, include/c2gs.h). The unzipping is the ESP32-S3 ROM's own tinfl (raw deflate), so it costs no
// flash; its ~11 KB working area is borrowed from PSRAM.
// ---------------------------------------------------------------------------

// true if the file is on the gauge as it is, or zipped (path + ".gz")
bool fsExists(const char *path);

// the whole file read into PSRAM - unzipped when only path + ".gz" is there - with a '\0' after it (so a text file
// can be used as a string). size gets its length. NULL if it isn't there, is damaged or doesn't fit in memory.
// Free it with heap_caps_free().
uint8_t *fsReadAll(const char *path, size_t *size);

// the unzipper's working area (~11 KB), for fsUnzipRaw. A sprite keeps one while it is open, so a frame doesn't borrow
// and give back 11 KB every time. In PSRAM (0.24.7 tried internal RAM: it starved the web UI). Free it with
// heap_caps_free(). NULL if there's no memory.
void *fsUnzipWorkArea();

// unzips one block of raw deflate (no gzip / zlib header) from in into out. true when exactly outLen bytes came out.
bool fsUnzipRaw(void *workArea, const uint8_t *in, size_t inLen, uint8_t *out, size_t outLen);
