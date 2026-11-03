#include "FsUnzip.h"
#include <LittleFS.h>
#include <esp_heap_caps.h>
#include <miniz.h> // the ESP32-S3 ROM's tinfl (raw deflate)

// gzip header flag bits (RFC 1952): what optional parts come after the fixed 10 bytes
#define GZIP_FLAG_HEADER_CRC 0x02 // a 2-byte header checksum
#define GZIP_FLAG_EXTRA 0x04      // a length, then that many extra bytes
#define GZIP_FLAG_NAME 0x08       // the original file name, ending in '\0'
#define GZIP_FLAG_COMMENT 0x10    // a comment, ending in '\0'
#define GZIP_TRAILER_BYTES 8      // CRC32 then the unzipped length, at the very end

bool fsExists(const char *path)
{
  if (LittleFS.exists(path))
  {
    return true;
  }
  String zipped = String(path) + ".gz";
  return LittleFS.exists(zipped);
}

// in PSRAM. 0.24.8: 0.24.7 tried internal RAM while a sprite played; with the sprite's decoder task and a phone on the
// web UI as well, internal RAM fell to 4 KB and the web page wouldn't load. Not worth it.
void *fsUnzipWorkArea()
{
  return heap_caps_malloc(sizeof(tinfl_decompressor), MALLOC_CAP_SPIRAM);
}

bool fsUnzipRaw(void *workArea, const uint8_t *in, size_t inLen, uint8_t *out, size_t outLen)
{
  if (!workArea || !in || !out)
  {
    return false;
  }
  tinfl_decompressor *unzipper = (tinfl_decompressor *)workArea; // the working area handed in, used as the unzipper
  tinfl_init(unzipper); // start from the beginning of a new block
  size_t inBytes = inLen;   // in: bytes we give it, out: bytes it used
  size_t outBytes = outLen; // in: room we give it, out: bytes it wrote
  const tinfl_status status = tinfl_decompress(unzipper, in, &inBytes, out, out, &outBytes,
                                               TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF); // all of it fits in out
  return status == TINFL_STATUS_DONE && outBytes == outLen;
}

// a file read as it is, into PSRAM with a '\0' after it
static uint8_t *readPlain(File &file, size_t *size)
{
  const size_t length = file.size();
  uint8_t *data = (uint8_t *)heap_caps_malloc(length + 1, MALLOC_CAP_SPIRAM);
  if (!data)
  {
    return NULL;
  }
  if (file.read(data, length) != length)
  {
    heap_caps_free(data);
    return NULL;
  }
  data[length] = '\0';
  *size = length;
  return data;
}

// a .gz file: read it, step over its header, unzip the deflate block into PSRAM (its length is in the last 4 bytes)
static uint8_t *readZipped(File &file, size_t *size)
{
  const size_t zipLength = file.size();
  if (zipLength < 10 + GZIP_TRAILER_BYTES)
  {
    return NULL;
  }
  uint8_t *zipped = (uint8_t *)heap_caps_malloc(zipLength, MALLOC_CAP_SPIRAM);
  if (!zipped)
  {
    return NULL;
  }
  uint8_t *data = NULL;
  void *workArea = NULL;
  bool ok = file.read(zipped, zipLength) == zipLength;
  // byte 0-1 the gzip mark 1F 8B, byte 2 method 8 (deflate), byte 3 the flags, then 6 bytes we don't need
  ok = ok && zipped[0] == 0x1F && zipped[1] == 0x8B && zipped[2] == 8;
  size_t start = 10;
  if (ok)
  {
    const uint8_t flags = zipped[3];
    if (flags & GZIP_FLAG_EXTRA)
    {
      start = start + 2 + (zipped[start] | (zipped[start + 1] << 8));
    }
    if (flags & GZIP_FLAG_NAME)
    {
      while (start < zipLength && zipped[start] != 0)
      {
        start++;
      }
      start++; // and the '\0'
    }
    if (flags & GZIP_FLAG_COMMENT)
    {
      while (start < zipLength && zipped[start] != 0)
      {
        start++;
      }
      start++;
    }
    if (flags & GZIP_FLAG_HEADER_CRC)
    {
      start = start + 2;
    }
    ok = start + GZIP_TRAILER_BYTES <= zipLength;
  }
  size_t length = 0;
  if (ok)
  {
    const uint8_t *trailer = zipped + zipLength - 4; // the unzipped length, little-endian
    length = trailer[0] | (trailer[1] << 8) | ((uint32_t)trailer[2] << 16) | ((uint32_t)trailer[3] << 24);
    data = (uint8_t *)heap_caps_malloc(length + 1, MALLOC_CAP_SPIRAM);
    workArea = fsUnzipWorkArea();
    ok = data && workArea &&
        fsUnzipRaw(workArea, zipped + start, zipLength - start - GZIP_TRAILER_BYTES, data, length);
  }
  if (workArea)
  {
    heap_caps_free(workArea);
  }
  heap_caps_free(zipped);
  if (!ok)
  {
    if (data)
    {
      heap_caps_free(data);
    }
    return NULL;
  }
  data[length] = '\0';
  *size = length;
  return data;
}

uint8_t *fsReadAll(const char *path, size_t *size)
{
  *size = 0;
  if (LittleFS.exists(path)) // the plain file wins (a user's own upload, or an older filesystem image)
  {
    File file = LittleFS.open(path, "r");
    if (!file)
    {
      return NULL;
    }
    uint8_t *data = readPlain(file, size);
    file.close();
    return data;
  }
  String zippedPath = String(path) + ".gz";
  if (!LittleFS.exists(zippedPath))
  {
    return NULL;
  }
  File file = LittleFS.open(zippedPath, "r");
  if (!file)
  {
    return NULL;
  }
  uint8_t *data = readZipped(file, size);
  file.close();
  return data;
}
