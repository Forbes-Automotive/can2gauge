// ME7 fast logging (the B7 RAM routine): writes our routine into the ECU's RAM, hooks it, polls it and un-hooks it. See
// can2gauge_ME7Fast.h for the whole story and the risk note. The C166 routines are our own clean-room code, assembled
// offline and held below as HANDLER_V0 / HANDLER_V1. The ME7 logger (lib/ME7Logger) installs it when Fast is armed,
// and the K-line task's fast-path bench check uses it too.
#include <can2gauge_ME7Fast.h>
#include <KLine.h>

// ===== Settings for this file =====
#define HANDLER_OFFSET 0xC6         // the handler sits here inside the blob (the 198-byte table is 0..0xC5)
#define ME7FAST_CONFIG_OFFSET 0x110 // the config block sits here (0x387B10); fixed, with headroom past the handler

// the biggest blob: the config at its fixed offset, its count word, and up to ME7FAST_MAX_VARS entries of 4 bytes
#define ME7FAST_BLOB_MAX (ME7FAST_CONFIG_OFFSET + 2 + ME7FAST_MAX_VARS * 4) // bytes

// ===== Variables for this file =====
static uint32_t fastPointer = ME7FAST_POINTER; // the ECU's mapping-table base pointer we hook (0xE228, EXTP; see the header)
static Me7FastStatus fastStatus;               // where the install / hook stands (zero-filled at boot: M7F_IDLE, haveSaved false)

// ===== The C166 routines (our own, assembled offline) =====
// v0 TEST LINK, assembled with the Ghidra C166 module (tools/me7fast/b7_handler_v0.asm): writes "C2G!" to the response
// buffer (R12+0..3) and returns length 4 in RL4, then rets.
//   mov r5,r12 / (movb RL4,#c ; movb [r5],RL4 ; add r5,#1) x4 / movb RL4,#4 / rets
static const uint8_t HANDLER_V0[] = {
    0xF0, 0x5C,             // mov r5,r12          ; cursor = buffer base
    0xE7, 0xF8, 0x43, 0x00, // movb RL4,#0x43 'C'
    0xB9, 0x85,             // movb [r5],RL4       ; buffer[0]
    0x08, 0x51,             // add r5,#0x1
    0xE7, 0xF8, 0x32, 0x00, // movb RL4,#0x32 '2'
    0xB9, 0x85,             // movb [r5],RL4       ; buffer[1]
    0x08, 0x51,             // add r5,#0x1
    0xE7, 0xF8, 0x47, 0x00, // movb RL4,#0x47 'G'
    0xB9, 0x85,             // movb [r5],RL4       ; buffer[2]
    0x08, 0x51,             // add r5,#0x1
    0xE7, 0xF8, 0x21, 0x00, // movb RL4,#0x21 '!'
    0xB9, 0x85,             // movb [r5],RL4       ; buffer[3]
    0xE1, 0x48,             // movb RL4,#0x4       ; response length = 4
    0xDB, 0x00              // rets
};

// v1 FAST, driven by its config block. Reads the config at a FIXED spot (0x387B10 = blob offset ME7FAST_CONFIG_OFFSET)
// - [N word][N x {page16, off16}] - and for each entry reads the 2-byte value at page:off and adds it to the
// response, in order. Returns N*2 in RL4. Assembled with the keyhana C166 module (docs §9.5).
// Each value is read as TWO BYTE reads (movb RL4,[r2+] / movb RH4,[r2]), not one word read: a C166 word read must be
// at an even address, our byte-sized variables sit at ODD addresses (e.g. IntakeAirTemp 0x380A2F), and a word read
// there traps the CPU (the crash on the bench). Byte reads can be at any address.
static const uint8_t HANDLER_V1[] = {
    0x88, 0x60, 0x88, 0x70, 0x88, 0x80, // push r6/r7/r8
    0xF0, 0x5C,                         // mov r5,r12          ; cursor = buffer base
    0xE6, 0xF7, 0xE1, 0x00,             // mov r7,#0xE1        ; config page
    0xE6, 0xF6, 0x10, 0x3B,             // mov r6,#0x3B10      ; config offset (physical 0x387B10)
    0xDC, 0x47, 0xA8, 0x86, 0x08, 0x62, // extp r7,#1 ; mov r8,[r6] (N) ; add r6,#2
    0xDC, 0x47, 0xA8, 0x36, 0x08, 0x62, // LOOP: extp r7,#1 ; mov r3,[r6] (var page) ; add r6,#2
    0xDC, 0x47, 0xA8, 0x26, 0x08, 0x62, // extp r7,#1 ; mov r2,[r6] (var off) ; add r6,#2
    0xDC, 0x53, 0x99, 0x82, 0xA9, 0x92, // extp r3,#2 ; movb RL4,[r2+] (low) ; movb RH4,[r2] (high) - byte reads
    0xB8, 0x45, 0x08, 0x52,             // mov [r5],r4 ; add r5,#2
    0x28, 0x81, 0x3D, 0xF3,             // sub r8,#1 ; jmpr cc_NE,LOOP
    0x20, 0x5C, 0xF0, 0x45,             // sub r5,r12 ; mov r4,r5  (length -> RL4)
    0x98, 0x80, 0x98, 0x70, 0x98, 0x60, // pop r8/r7/r6
    0xDB, 0x00                          // rets
};

// ===== Small helpers =====
// the status line the web UI / bench log shows
static void setMessage(const char *message)
{
  strncpy(fastStatus.message, message, sizeof(fastStatus.message) - 1);
  fastStatus.message[sizeof(fastStatus.message) - 1] = 0;
}

// something went wrong: the error state, with the reason
static void fail(const char *message)
{
  fastStatus.state = M7F_ERROR;
  setMessage(message);
}

// ===== Functions in this file =====
static uint16_t buildBlob(uint8_t *buf, const uint32_t *varAddrs, uint8_t varCount); // the blob: pointer table, handler (+ config)
static bool fastWriteMem(uint32_t addr, const uint8_t *data, uint8_t len);           // 0x3D WriteMemoryByAddress
static int fastReadMem(uint32_t addr, uint8_t len, uint8_t *out, uint8_t cap);       // 0x23 ReadMemoryByAddress

// Writes the routine blob into the ECU's RAM, checks it landed, saves the ECU's pointer and repoints it at the blob.
// varCount 0 = the v0 Test link marker, more = v1 Fast reading those variables. true = installed + hooked.
bool me7FastInstall(const uint32_t *varAddrs, uint8_t varCount)
{
  if (varCount > ME7FAST_MAX_VARS)
  {
    varCount = ME7FAST_MAX_VARS; // clamp to the config bound (the reply can't exceed the ECU buffer anyway)
  }
  memset(&fastStatus, 0, sizeof(fastStatus));
  setMessage("installing");

  // 0) the pointer we hook is always ME7FAST_POINTER (0xE228, the offset word; the page word is at 0xE22A), whatever
  //    the ECU's bootrom version. Bench 8N (2026-10-22): a linear 24-bit address written at 0xE228 froze it (every
  //    service stopped answering), and a hook at 0xE226 did nothing (see the header).
  fastPointer = ME7FAST_POINTER;

  // 1) build and write the blob into RAM in ME7FAST_CHUNK-byte chunks (small enough that each 0x3D write fits the
  //    60-byte KWP2000 request the K-line layer can frame)
  uint8_t blob[ME7FAST_BLOB_MAX];
  const uint16_t blobLen = buildBlob(blob, varAddrs, varCount);
  for (uint16_t offset = 0; offset < blobLen; offset += ME7FAST_CHUNK)
  {
    uint16_t chunk = blobLen - offset;
    if (chunk > ME7FAST_CHUNK)
    {
      chunk = ME7FAST_CHUNK;
    }
    if (!fastWriteMem(ME7FAST_SCRATCH + offset, blob + offset, (uint8_t)chunk))
    {
      fail("blob write refused (3D) - RAM write not allowed on this ECU?");
      return false;
    }
  }

  // 2) read the handler's start back and compare: make sure the blob really landed before we hook anything
  const uint8_t *handler = HANDLER_V0;
  if (varCount != 0)
  {
    handler = HANDLER_V1;
  }
  uint8_t check[4];
  if (fastReadMem(ME7FAST_SCRATCH + HANDLER_OFFSET, 4, check, sizeof(check)) != 4 ||
      memcmp(check, handler, 4) != 0)
  {
    fail("blob verify failed - RAM did not read back the same");
    return false;
  }
  fastStatus.state = M7F_UPLOADED;

  // 3) save the ECU's original mapping-table pointer so we can put it back
  if (fastReadMem(fastPointer, 4, fastStatus.pointerSaved, 4) != 4)
  {
    fail("could not read the mapping-table pointer");
    return false;
  }
  fastStatus.haveSaved = true;

  // 4) hook: repoint the mapping-table base at our blob. The dispatch reads this pointer with EXTP, so it is an
  //    {offset word, page word} pair, NOT a linear address: offset = addr's low 14 bits, page = addr >> 14.
  //    For the blob at 0x387A00: page 0xE1, offset 0x3A00. (docs §9.4)
  const uint16_t blobOffset = (uint16_t)(ME7FAST_SCRATCH & 0x3FFF); // low 14 bits (the two high bits are ignored)
  const uint16_t blobPage = (uint16_t)(ME7FAST_SCRATCH >> 14);      // EXTP page number
  uint8_t newPointer[4] = {
      (uint8_t)blobOffset, (uint8_t)(blobOffset >> 8), // offset word at 0xE228
      (uint8_t)blobPage, (uint8_t)(blobPage >> 8)};    // page word at 0xE22A
  if (!fastWriteMem(fastPointer, newPointer, 4))
  {
    fail("could not write the mapping-table pointer (hook)");
    return false;
  }
  fastStatus.state = M7F_HOOKED;
  // the status says where it hooked and which routine went in, for the bench log
  const char *handlerName = "Fast";
  if (varCount == 0)
  {
    handlerName = "Test link";
  }
  char hookMessage[48];
  snprintf(hookMessage, sizeof(hookMessage), "installed + hooked @0x%04lX (%s)", (unsigned long)fastPointer,
           handlerName);
  setMessage(hookMessage);
  return true;
}

// Un-hook: writes the saved original pointer back. Safe to call when not hooked. true = it wrote it.
bool me7FastUnhook()
{
  if (!fastStatus.haveSaved)
  {
    return false; // nothing to put back
  }
  const bool ok = fastWriteMem(fastPointer, fastStatus.pointerSaved, 4);
  if (ok)
  {
    fastStatus.state = M7F_IDLE;
    setMessage("un-hooked (original pointer restored)");
  }
  return ok;
}

// one sample: sends 0xB7 and copies the routine's reply into out (up to cap bytes). The byte count, or -1 on failure.
int me7FastPoll(uint8_t *out, uint8_t cap)
{
  uint8_t resp[160];
  uint8_t respLen;
  uint8_t req[1] = {0xB7};
  // 200 ms, not 800: the routine answers on the ECU's own ~10 ms tick, and this poll runs every sample - a long
  // timeout on an ECU that never answers B7 would block the K-line task long enough to trip the task watchdog.
  if (!kwp2000Request(req, 1, resp, respLen, 200, sizeof(resp)))
  {
    return -1;
  }
  // resp is the ECU-framed reply (v0: our "C2G!" block; v1: the values in order), handed back raw for the caller to
  // read
  uint8_t count = respLen;
  if (count > cap)
  {
    count = cap;
  }
  memcpy(out, resp, count);
  return count;
}

// where the install / hook stands, with its last message (the web UI and the bench log read it)
const Me7FastStatus &me7FastStatusGet()
{
  return fastStatus;
}

// Builds the blob in buf and returns its length. First a 198-byte table of the 4-byte far pointer 0x00387AC6 (the
// handler's address, little-endian "C6 7A 38 00" over and over) so whatever sub-index the ECU works out lands on our
// one handler. Then the handler: the v0 marker (Test link) when varCount is 0, else v1 Fast followed by its config
// block [N word][N x {page16, off16}] built from each variable's 24-bit address.
static uint16_t buildBlob(uint8_t *buf, const uint32_t *varAddrs, uint8_t varCount)
{
  const uint32_t handlerAddr = ME7FAST_SCRATCH + HANDLER_OFFSET; // 0x387AC6
  const uint8_t pattern[4] = {
      (uint8_t)handlerAddr,         // code address, low byte
      (uint8_t)(handlerAddr >> 8),  // code address, high byte
      (uint8_t)(handlerAddr >> 16), // code segment
      0x00};                        // always 0
  for (uint16_t i = 0; i < HANDLER_OFFSET; i++)
  {
    buf[i] = pattern[i & 3]; // i & 3 = the remainder of i / 4: the 4 bytes over and over
  }
  if (varCount == 0)
  {
    memcpy(buf + HANDLER_OFFSET, HANDLER_V0, sizeof(HANDLER_V0)); // Test link marker
    return HANDLER_OFFSET + (uint16_t)sizeof(HANDLER_V0);
  }
  // v1: the Fast handler, then its config block at the FIXED offset the handler reads (ME7FAST_CONFIG_OFFSET =
  // 0x387B10). The gap between the handler's end and the config is zeroed so nothing stray sits there.
  memcpy(buf + HANDLER_OFFSET, HANDLER_V1, sizeof(HANDLER_V1));
  for (uint16_t i = HANDLER_OFFSET + (uint16_t)sizeof(HANDLER_V1); i < ME7FAST_CONFIG_OFFSET; i++)
  {
    buf[i] = 0x00;
  }
  uint16_t pos = ME7FAST_CONFIG_OFFSET;
  buf[pos++] = varCount; // N, low byte
  buf[pos++] = 0x00;     // N, high byte (count never exceeds 255)
  for (uint8_t varIndex = 0; varIndex < varCount; varIndex++)
  {
    const uint16_t page = (uint16_t)(varAddrs[varIndex] >> 14);         // EXTP page = address / 0x4000
    const uint16_t pageOffset = (uint16_t)(varAddrs[varIndex] & 0x3FFF); // 14-bit offset within that page
    buf[pos++] = (uint8_t)page;              // page, low byte
    buf[pos++] = (uint8_t)(page >> 8);       // page, high byte
    buf[pos++] = (uint8_t)pageOffset;        // offset, low byte
    buf[pos++] = (uint8_t)(pageOffset >> 8); // offset, high byte
  }
  return pos;
}

// WriteMemoryByAddress (0x3D): writes len bytes to a 24-bit address. true = the ECU accepted (positive answer 0x7D).
static bool fastWriteMem(uint32_t addr, const uint8_t *data, uint8_t len)
{
  uint8_t resp[16];
  uint8_t respLen;
  uint8_t req[5 + ME7FAST_CHUNK];
  req[0] = 0x3D;                  // WriteMemoryByAddress
  req[1] = (uint8_t)(addr >> 16); // address high byte
  req[2] = (uint8_t)(addr >> 8);  // address middle byte
  req[3] = (uint8_t)addr;         // address low byte
  req[4] = len;                   // how many bytes follow
  for (uint8_t i = 0; i < len; i++)
  {
    req[5 + i] = data[i];
  }
  if (!kwp2000Request(req, 5 + len, resp, respLen, 800, sizeof(resp)))
  {
    return false;
  }
  return respLen >= 1 && resp[0] == 0x7D;
}

// ReadMemoryByAddress (0x23): reads len bytes from a 24-bit address into out (up to cap). The byte count, or -1.
static int fastReadMem(uint32_t addr, uint8_t len, uint8_t *out, uint8_t cap)
{
  uint8_t resp[160];
  uint8_t respLen;
  uint8_t req[5] = {0x23,                  // ReadMemoryByAddress
                    (uint8_t)(addr >> 16), // address high byte
                    (uint8_t)(addr >> 8),  // address middle byte
                    (uint8_t)addr,         // address low byte
                    len};                  // how many bytes to read
  if (!kwp2000Request(req, 5, resp, respLen, 800, sizeof(resp)))
  {
    return -1;
  }
  if (respLen < 1 || resp[0] != 0x63)
  {
    return -1;
  }
  uint8_t count = respLen - 1; // first byte is the 0x63 positive-answer id
  if (count > cap)
  {
    count = cap;
  }
  memcpy(out, resp + 1, count);
  return count;
}
