#include "ME7Read.h"
#include <KLine.h>

// ---------------------------------------------------------------------------
// KWP2000 service IDs and the VAG-specific bits we use (subset of NefMoto's KWP2000Constants.cs)
// ---------------------------------------------------------------------------
#define SID_START_DIAG 0x10
#define SID_READ_ECU_ID 0x1A
#define SID_SECURITY 0x27
#define SID_REQUEST_UPLOAD 0x35
#define SID_TRANSFER_DATA 0x36
#define SID_TRANSFER_EXIT 0x37
#define SID_ACCESS_TIMING 0x83
#define SID_WRITE_MEM 0x3D // 0.18.0: WriteMemoryByAddress - forges the unlock words in RAM (ramUnlock read)
#define SID_READ_MEM 0x23  // 0.18.0: ReadMemoryByAddress - reads the flash in the ramUnlock read

#define READ_MEM_CHUNK 0x3C // 0.18.0: bytes per 0x23 request in the ramUnlock read (60; tune on the bench if an ECU caps it)

#define SESSION_PROGRAMMING 0x85
#define SESSION_DEVELOPMENT 0x86
#define ID_HW_NUMBER 0x91 // vehicle manufacturer ECU hardware number
#define ID_SW_NUMBER 0x94 // system supplier ECU software number

#define REQUEST_SEED 0x01
#define SEND_KEY 0x02
#define SECURITY_OK 0x34 // VAG's "granted" byte in the key response

// ISO 14230-2 baud rate identifier: baud = 2^X * (Y + 32) * 6400 / 32, byte = XXXYYYYY
static uint8_t baudByte(uint32_t baud)
{
  uint32_t base = baud * 32u / 6400u;
  uint32_t bestExponent = 0;
  uint32_t bestPower = 1;
  double bestDist = 64.0;
  for (int exponent = 7; exponent >= 0; exponent--)
  {
    uint32_t power = 1u << exponent;
    if (power < base)
    {
      double scalar = (double)base / (double)power;
      if (scalar < 64 && scalar > 32)
      {
        double dist = scalar - floor(scalar);
        if (dist < bestDist)
        {
          bestDist = dist;
          bestExponent = exponent;
          bestPower = power;
        }
      }
    }
  }
  uint32_t yBits = (base / bestPower) - 32;
  return (uint8_t)(((bestExponent & 0x7) << 5) | (yBits & 0x1F));
}

// VAG external-RAM key transform (NefMoto SecurityAccessAction.GenerateVAGExternalRAMSendKey).
// 64-entry table, default key index 0x3F, 5 shift/xor loops.
uint32_t me7ReadKeyFromSeed(uint32_t seed, uint8_t keyIndex, uint8_t numLoops)
{
  static const uint32_t table[64] = {
      0x0A221289, 0x144890A1, 0x24212491, 0x290A0285, 0x42145091, 0x504822C1, 0x0A24C4C1, 0x14252229,
      0x24250525, 0x2510A491, 0x28488863, 0x29148885, 0x422184A5, 0x49128521, 0x50844A85, 0x620CC211,
      0x124452A9, 0x18932251, 0x2424A459, 0x29149521, 0x42352621, 0x4A512289, 0x52A48911, 0x11891475,
      0x22346523, 0x4A3118D1, 0x64497111, 0x0AE34529, 0x15398989, 0x22324A67, 0x2D12B489, 0x132A4A75,
      0x19B13469, 0x25D2C453, 0x4949349B, 0x524E9259, 0x1964CA6B, 0x24F5249B, 0x28979175, 0x352A5959,
      0x3A391749, 0x51D44EA9, 0x564A4F25, 0x6AD52649, 0x76493925, 0x25DE52C9, 0x332E9333, 0x68D64997,
      0x494947FB, 0x33749ACF, 0x5AD55B5D, 0x7F272A4F, 0x35BD5B75, 0x3F5AD55D, 0x5B5B6DAD, 0x6B5DAD6B,
      0x75B57AD5, 0x5DBAD56F, 0x6DBF6AAD, 0x75775EB5, 0x5AEDFED5, 0x6B5F7DD5, 0x6F757B6B, 0x5FBD5DBD};
  uint8_t tableIndex = keyIndex < 64 ? keyIndex : 63;
  uint32_t key = seed;
  for (uint8_t i = 0; i < numLoops; i++)
  {
    if (key >= 0x80000000u)
    {
      key <<= 1;
      key |= 1u;
      key ^= table[tableIndex];
    }
    else
    {
      key <<= 1;
    }
  }
  return key;
}

// ---------------------------------------------------------------------------
// module state
// ---------------------------------------------------------------------------
static Me7ReadStatus readStatus;
static Me7ReadParams readParams;
static volatile bool cancelFlag = false;
// 0.17.4: the read goes into PSRAM, not straight into a file: the gauge's filesystem has ~330 KB free with the web UI
// image on it, so a 512 KB / 1 MB dump was cut short without a word. Allocated at the first read and kept (never
// freed, so a download that's still running can't be left reading freed memory); the app saves it if there's room.
static uint8_t *dumpBuffer = NULL;  // the flash, byte for byte, at its offset from flashBase
static uint32_t dumpLength = 0;     // bytes filled so far
static bool dumpComplete = false;   // true once a whole read has finished (and until the next one starts)
static uint32_t startMs = 0;
static uint32_t blockBase = 0;      // start of the 64 KB block being read
static uint32_t blockDone = 0;      // bytes of the current block already written
static uint8_t maxBlockSize = 0;    // TransferData block size the ECU reported
static const uint32_t READ_BLOCK = 0x10000; // 64 KB per RequestUpload, like NefMoto's per-sector reads
static bool use23Read = false;      // 0.18.0: true in a ramUnlock read - read the flash with 0x23, not RequestUpload

static void updateProgressTiming(); // defined below; called mid-stream so the bar moves within a 64 KB block

static void setMessage(const char *message)
{
  strncpy(readStatus.message, message, sizeof(readStatus.message) - 1);
  readStatus.message[sizeof(readStatus.message) - 1] = 0;
}

static void fail(const char *message)
{
  readStatus.state = M7R_ERROR;
  setMessage(message);
}

const char *me7ReadStateName(me7ReadState_t state)
{
  static const char *names[] = {"idle", "init", "ram unlock", "session", "security", "programming", "mirror check", "reading", "verify", "done", "error"};
  return names[state <= M7R_ERROR ? state : M7R_ERROR];
}

// ---------------------------------------------------------------------------
// KWP2000 helpers layered on the KLine transport
// ---------------------------------------------------------------------------
// starts a diagnostic session, asking for a new baud rate with it (baud 0 = no change). baudChanged = true when the ECU
// took the new rate (it switches after its answer). 0.17.4: if it refused the rate and took the plain request, the
// caller used to switch to the new rate anyway and lost the ECU.
static bool startSession(uint8_t type, uint32_t baud, bool &baudChanged)
{
  uint8_t resp[16];
  uint8_t respLen;
  baudChanged = false;
  if (baud && baud != 10400)
  {
    uint8_t req[3] = {SID_START_DIAG, type, baudByte(baud)};
    if (kwp2000Request(req, 3, resp, respLen, 2000, sizeof(resp)))
    {
      baudChanged = true;
      return true;
    }
    // ECU refused the baud change: retry with no baud byte, stay at the current rate
  }
  uint8_t req[2] = {SID_START_DIAG, type};
  return kwp2000Request(req, 2, resp, respLen, 2000, sizeof(resp));
}

// 0.17.4: back to the ECU's default timing (83 01), as NefMoto does before its first login. Best effort.
static void defaultTiming()
{
  uint8_t resp[16];
  uint8_t respLen;
  uint8_t req[2] = {SID_ACCESS_TIMING, 0x01}; // 0x01 = set to defaults
  kwp2000Request(req, 2, resp, respLen, 400, sizeof(resp));
}

// ask for the ECU's timing limits then set them, so P2max is large enough for a full transfer block
static void negotiateTiming()
{
  // 0.17.2: done by the K-line library now. This sent "83 00" padded with four zero bytes and set only four of the five
  // values (P4min was missing): the answer to 83 00 is [C3][00][P2min][P2max][P3min][P3max][P4min].
  // Nothing is set if the ECU won't give its limits (NULL); our byte gap goes to 0 for speed, as before.
  kwp2000AccessTiming(NULL, 0);
}

static bool securityAccess()
{
  uint8_t resp[16];
  uint8_t respLen;
  uint8_t reqSeed[2] = {SID_SECURITY, REQUEST_SEED};
  if (!kwp2000Request(reqSeed, 2, resp, respLen, 500, sizeof(resp)))
  {
    return false;
  }
  if (respLen < 2 || resp[0] != (SID_SECURITY + 0x40) || resp[1] != REQUEST_SEED)
  {
    return false;
  }
  if (respLen < 6) // seed is 4 bytes; a shorter reply means already unlocked
  {
    return true;
  }
  // 0.17.4: an all-zero seed still gets its key (NefMoto: the second login, inside the programming session, gets a
  // zero seed and must still send the key). The key of seed 0 is 0. Before, a zero seed counted as "unlocked".
  uint32_t seed = ((uint32_t)resp[2] << 24) | ((uint32_t)resp[3] << 16) | ((uint32_t)resp[4] << 8) | resp[5];
  uint32_t key = me7ReadKeyFromSeed(seed);
  uint8_t sendKey[6] = {SID_SECURITY, SEND_KEY, (uint8_t)(key >> 24), (uint8_t)(key >> 16), (uint8_t)(key >> 8), (uint8_t)key};
  if (!kwp2000Request(sendKey, 6, resp, respLen, 500, sizeof(resp)))
  {
    return false;
  }
  return respLen >= 2 && resp[0] == (SID_SECURITY + 0x40) && resp[1] == SECURITY_OK;
}

// RequestUpload for [addr,size]; on success maxBlockSize holds the ECU's TransferData block length
static bool requestUpload(uint32_t addr, uint32_t size)
{
  uint8_t resp[16];
  uint8_t respLen;
  uint8_t req[7] = {SID_REQUEST_UPLOAD,
                    (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr,
                    0x00, // data format: uncompressed, unencrypted
                    (uint8_t)(size >> 16), (uint8_t)(size >> 8)};
  req[6] = (uint8_t)size;
  if (!kwp2000Request(req, 7, resp, respLen, 800, sizeof(resp)))
  {
    return false;
  }
  if (respLen < 1 || resp[0] != (SID_REQUEST_UPLOAD + 0x40))
  {
    return false;
  }
  maxBlockSize = resp[1] ? resp[1] : 0x7C; // 124 default if the ECU reports 0
  return true;
}

// one TransferData (0x36) request -> up to maxBlockSize payload bytes. Returns count, -1 on error.
static int transferData(uint8_t *out, uint16_t capacity)
{
  uint8_t resp[260];
  uint8_t respLen;
  uint8_t req[1] = {SID_TRANSFER_DATA};
  if (!kwp2000Request(req, 1, resp, respLen, 1500, sizeof(resp)))
  {
    return -1;
  }
  if (respLen < 1 || resp[0] != (SID_TRANSFER_DATA + 0x40))
  {
    return -1;
  }
  uint8_t count = respLen - 1;
  if (count > capacity)
  {
    count = capacity;
  }
  memcpy(out, resp + 1, count);
  return count;
}

static bool transferExit()
{
  uint8_t resp[16];
  uint8_t respLen;
  uint8_t req[1] = {SID_TRANSFER_EXIT};
  return kwp2000Request(req, 1, resp, respLen, 800, sizeof(resp));
}

// 0.18.0: WriteMemoryByAddress (0x3D) - write len bytes to a 24-bit address. Returns true when the ECU accepts it
// (positive answer 0x7D). Used to forge the unlock words in RAM; the ECU allows this with no security.
static bool writeMem(uint32_t addr, const uint8_t *data, uint8_t len)
{
  uint8_t resp[16];
  uint8_t respLen;
  uint8_t req[5 + 8];
  req[0] = SID_WRITE_MEM;
  req[1] = (uint8_t)(addr >> 16); // address high byte
  req[2] = (uint8_t)(addr >> 8);  // address middle byte
  req[3] = (uint8_t)addr;         // address low byte
  req[4] = len;                   // how many bytes follow
  for (uint8_t i = 0; i < len && i < 8; i++)
  {
    req[5 + i] = data[i];
  }
  if (!kwp2000Request(req, 5 + len, resp, respLen, 800, sizeof(resp)))
  {
    return false;
  }
  return respLen >= 1 && resp[0] == (SID_WRITE_MEM + 0x40);
}

// 0.18.0: ReadMemoryByAddress (0x23) - read len bytes from a 24-bit address into out. Returns the byte count, or -1
// on a failure / negative answer. The ECU's 0x23 serves only certain address regions (RAM, XRAM, and the flash
// window once the permit word is set); outside them it answers 7F 23 xx.
static int readMem(uint32_t addr, uint8_t len, uint8_t *out, uint8_t capacity)
{
  uint8_t resp[128];
  uint8_t respLen;
  uint8_t req[5] = {SID_READ_MEM, (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr, len};
  if (!kwp2000Request(req, 5, resp, respLen, 800, sizeof(resp)))
  {
    return -1;
  }
  if (respLen < 1 || resp[0] != (SID_READ_MEM + 0x40))
  {
    return -1;
  }
  uint8_t count = respLen - 1; // the first byte is the 0x63 positive-answer id
  if (count > capacity)
  {
    count = capacity;
  }
  memcpy(out, resp + 1, count);
  return count;
}

static void readEcuId()
{
  uint8_t resp[64];
  uint8_t respLen;
  readStatus.ecuId[0] = 0;
  for (uint8_t option : {(uint8_t)ID_HW_NUMBER, (uint8_t)ID_SW_NUMBER})
  {
    uint8_t req[2] = {SID_READ_ECU_ID, option};
    if (kwp2000Request(req, 2, resp, respLen, 500, sizeof(resp)) && respLen > 1)
    {
      size_t pos = strlen(readStatus.ecuId);
      if (pos && pos < sizeof(readStatus.ecuId) - 1)
      {
        readStatus.ecuId[pos++] = ' ';
      }
      for (uint8_t byteIndex = 1; byteIndex < respLen && pos < sizeof(readStatus.ecuId) - 1; byteIndex++)
      {
        char character = (char)resp[byteIndex];
        readStatus.ecuId[pos++] = (character >= 32 && character < 127) ? character : ' ';
      }
      readStatus.ecuId[pos] = 0;
    }
  }
}

// read one contiguous chunk [addr,size]: into sampleOut (the first sampleLen bytes - the mirror check and the verify),
// or with sampleOut NULL into the dump at blockBase. Returns bytes transferred, or -1 on failure.
static int32_t readChunk(uint32_t addr, uint32_t size, uint8_t *sampleOut, uint32_t sampleLen)
{
  if (!requestUpload(addr, size))
  {
    return -1;
  }
  uint8_t buf[256];
  uint32_t got = 0;
  while (got < size)
  {
    int count = transferData(buf, sizeof(buf));
    if (count <= 0)
    {
      return -1;
    }
    if ((uint32_t)count > size - got)
    {
      count = size - got;
    }
    if (dumpBuffer != NULL && sampleOut == NULL)
    {
      memcpy(dumpBuffer + blockBase + got, buf, count); // into the dump at its place in the flash
      dumpLength = blockBase + got + count;
    }
    if (sampleOut && got < sampleLen)
    {
      uint32_t take = sampleLen - got;
      if (take > (uint32_t)count)
      {
        take = count;
      }
      memcpy(sampleOut + got, buf, take);
    }
    got += count;
    blockDone = got;
    if (sampleOut == NULL)
    {
      updateProgressTiming(); // live progress while a real (file) block streams
    }
  }
  transferExit();
  return got;
}

// 0.18.0: the same job as readChunk() but with ReadMemoryByAddress (0x23) instead of RequestUpload - used by the
// ramUnlock read. Reads [addr,size] in READ_MEM_CHUNK pieces straight into the dump (sampleOut NULL) or into a
// sample buffer (the mirror check / verify). Returns bytes read, or -1 on failure.
static int32_t readChunk23(uint32_t addr, uint32_t size, uint8_t *sampleOut, uint32_t sampleLen)
{
  uint8_t buf[READ_MEM_CHUNK];
  uint32_t got = 0;
  while (got < size)
  {
    uint32_t want = size - got;
    if (want > READ_MEM_CHUNK)
    {
      want = READ_MEM_CHUNK;
    }
    int count = readMem(addr + got, (uint8_t)want, buf, sizeof(buf));
    if (count <= 0)
    {
      return -1;
    }
    if ((uint32_t)count > want)
    {
      count = want;
    }
    if (dumpBuffer != NULL && sampleOut == NULL)
    {
      memcpy(dumpBuffer + blockBase + got, buf, count); // into the dump at its place in the flash
      dumpLength = blockBase + got + count;
    }
    if (sampleOut && got < sampleLen)
    {
      uint32_t take = sampleLen - got;
      if (take > (uint32_t)count)
      {
        take = count;
      }
      memcpy(sampleOut + got, buf, take);
    }
    got += count;
    blockDone = got;
    if (sampleOut == NULL)
    {
      updateProgressTiming();
    }
  }
  return got;
}

// 0.18.0: read [addr,size] with whichever transport this read is using - 0x23 for a ramUnlock read, else the
// RequestUpload path. Lets the mirror check, the main read and the verify stay the same for both.
static int32_t readAny(uint32_t addr, uint32_t size, uint8_t *sampleOut, uint32_t sampleLen)
{
  if (use23Read)
  {
    return readChunk23(addr, size, sampleOut, sampleLen);
  }
  return readChunk(addr, size, sampleOut, sampleLen);
}

// ---------------------------------------------------------------------------
// public control
// ---------------------------------------------------------------------------
bool me7ReadBusy()
{
  return readStatus.state != M7R_IDLE && readStatus.state != M7R_DONE && readStatus.state != M7R_ERROR;
}

const Me7ReadStatus &me7ReadStatusGet()
{
  return readStatus;
}

void me7ReadCancel()
{
  cancelFlag = true;
}

bool me7ReadStart(const Me7ReadParams &params)
{
  if (me7ReadBusy())
  {
    return false;
  }
  readParams = params;
  cancelFlag = false;
  memset(&readStatus, 0, sizeof(readStatus));
  readStatus.state = M7R_INIT;
  readStatus.bytesTotal = readParams.flashSize;
  readStatus.baud = readParams.baud;
  setMessage("starting");
  startMs = millis();
  dumpLength = 0;
  dumpComplete = false;
  use23Read = false; // 0.18.0: set true once a ramUnlock read has forged the unlock words
  return true;
}

const uint8_t *me7ReadDump(uint32_t &length)
{
  if (!dumpComplete || dumpBuffer == NULL)
  {
    length = 0;
    return NULL;
  }
  length = dumpLength;
  return dumpBuffer;
}

// ---------------------------------------------------------------------------
// the state machine, pumped from the K-line task
// ---------------------------------------------------------------------------
static void updateProgressTiming()
{
  readStatus.elapsedMs = millis() - startMs;
  readStatus.bytesRead = blockBase + blockDone;
  if (readStatus.bytesTotal)
  {
    readStatus.percent = (uint8_t)((uint64_t)readStatus.bytesRead * 100 / readStatus.bytesTotal);
  }
  if (readStatus.bytesRead > 4096 && readStatus.elapsedMs > 500)
  {
    float bps = (float)readStatus.bytesRead / readStatus.elapsedMs;
    readStatus.etaMs = bps > 0 ? (uint32_t)((readStatus.bytesTotal - readStatus.bytesRead) / bps) : 0;
  }
}

bool me7ReadPump()
{
  if (cancelFlag && me7ReadBusy())
  {
    fail("cancelled");
    kwp2000Disconnect();
    return false;
  }
  switch (readStatus.state)
  {
  case M7R_INIT:
  {
    setMessage("connecting to ECU");
    klSetBaud(10400);
    uint8_t keyBytes[2];
    if (readParams.address == 0x01 && !readParams.fastInit)
    {
      // 0.17.4: NefMoto's slow-init handoff (KWP2000Interface: KWP1281RunShortSession). An ME7 answers 0x01 with
      // KWP1281: run that session properly (identification blocks, end block), wait 1.5 s "to give the ECU time to
      // switch to KWP2000", then wake 0x11 (plain byte) and talk with no address bytes (NefMoto's address mode None).
      char kwp1281Id[48];
      if (kwp1281Connect(0x01, kwp1281Id, sizeof(kwp1281Id)))
      {
        kwp1281Disconnect(); // the end-communication block
        delay(1500);
        if (kwp2000Connect(0x11, false, keyBytes))
        {
          kwp2000SetAddressBytes(false);
          readEcuId();
          readStatus.state = readParams.ramUnlock ? M7R_RAMUNLOCK : M7R_SESSION;
          return true;
        }
      }
    }
    if (!kwp2000Connect(readParams.address, readParams.fastInit, keyBytes))
    {
      // one automatic retry the other way: ME7.5 typically needs slow init
      if (!kwp2000Connect(readParams.address, !readParams.fastInit, keyBytes))
      {
        // 0.17.4: an ME7.5 answers 0x01 with KWP1281; its KWP2000 is a slow init to 0x11, then it answers as 0x10
        if (readParams.address == 0x01 && kwp2000Connect(0x11, false, keyBytes))
        {
          readParams.target = 0x10;
        }
        else
        {
          fail("no answer from ECU (try the other init / ignition on)");
          return false;
        }
      }
    }
    if (readParams.target != 0)
    {
      kwp2000SetTarget(readParams.target); // 0.17.4: talk to a different address than the one woken (ME7: 0x11 -> 0x10)
    }
    readEcuId();
    readStatus.state = readParams.ramUnlock ? M7R_RAMUNLOCK : M7R_SESSION;
    return true;
  }
  case M7R_RAMUNLOCK:
  {
    // 0.18.0: read a locked ECU with 0x23 instead of the normal RequestUpload path (which needs the 0x27 login
    // these ECUs refuse). An REA of 16 images (docs/me7_ramroutine_re.md 6e.3) showed the flash window (0x800000+)
    // is gated by an INTEGRITY word at permitAddr, not a login: on a stock, checksum-valid ECU that word already
    // holds the value the gate wants, so the flash reads straight away with NO forge and nothing is written to the
    // ECU at all. So try the plain read first, and forge only if the flash won't read - forcing a wrong permit
    // value would otherwise BREAK a read that would have worked (it overwrites the value that was already correct).
    setMessage("opening flash read");
    defaultTiming();
    use23Read = true; // this path reads the flash with 0x23, not RequestUpload

    uint8_t sample[1];
    if (readMem(readParams.flashBase, 1, sample, sizeof(sample)) == 1)
    {
      setMessage("flash readable - reading with 0x23"); // stock ECU: no forge needed, nothing written
    }
    else
    {
      // Not readable as-is (e.g. a modified flash whose permit word no longer matches). Forge the unlock words in
      // RAM (WriteMemoryByAddress 0x3D, allowed with no security; clears at ignition-off): set the security-granted
      // bit, write the permit value the user supplied for this ECU, then try the flash once more.
      uint8_t word[2];
      if (readMem(readParams.grantAddr, 2, word, sizeof(word)) == 2)
      {
        uint16_t status = (uint16_t)(word[0] | (word[1] << 8));
        status |= readParams.grantMask;
        uint8_t granted[2] = {(uint8_t)status, (uint8_t)(status >> 8)};
        writeMem(readParams.grantAddr, granted, 2);
      }
      uint8_t permit[2] = {(uint8_t)readParams.permitValue, (uint8_t)(readParams.permitValue >> 8)};
      writeMem(readParams.permitAddr, permit, 2);
      if (readMem(readParams.flashBase, 1, sample, sizeof(sample)) != 1)
      {
        fail("flash not readable - set this ECU's own Permit value (see docs) or check flashBase");
        kwp2000Disconnect();
        return false;
      }
      setMessage("RAM unlock (forced permit) - reading with 0x23");
    }

    readStatus.baud = 10400; // the read runs in the default session at 10400; raising baud is a later job
    readStatus.state = readParams.mirrorCheck ? M7R_MIRRORCHECK : M7R_READING;
    blockBase = 0;
    blockDone = 0;
    return true;
  }
  case M7R_SESSION:
  {
    // 0.17.4: NefMoto's order for a flash read (ReadExternalFlashOperation): default timing, log in while still in
    // the default session, then the programming session with the baud change, the timing limits, and log in again
    // (M7R_SECURITY). The old order (10 86, log in, 10 85) got 7F 27 10 on the bench ME7.5: no login after 10 86,
    // and 10 85 without a login first gets no answer at all.
    setMessage("security access");
    defaultTiming();
    if (!securityAccess())
    {
      fail("security access denied");
      kwp2000Disconnect();
      return false;
    }
    setMessage("programming session");
    bool baudChanged = false;
    if (!startSession(SESSION_PROGRAMMING, readParams.baud, baudChanged))
    {
      fail("ECU refused the programming session");
      kwp2000Disconnect();
      return false;
    }
    if (baudChanged)
    {
      delay(30); // the ECU switches after its positive answer
      klSetBaud(readParams.baud);
      delay(40);
    }
    else
    {
      readStatus.baud = 10400;
    }
    negotiateTiming();
    if (!kwp2000TesterPresent()) // confirm the link still talks at the new rate
    {
      klSetBaud(10400);
      readStatus.baud = 10400;
      setMessage("stayed at 10400 baud");
    }
    readStatus.state = M7R_SECURITY;
    return true;
  }
  case M7R_SECURITY:
  {
    setMessage("security access (programming session)");
    if (!securityAccess()) // the second login: a zero seed, but the key must still go
    {
      fail("security access denied in the programming session");
      kwp2000Disconnect();
      return false;
    }
    readStatus.state = M7R_PROGRAMMING;
    return true;
  }
  case M7R_PROGRAMMING:
  {
    // 0.17.4: already in the programming session (M7R_SESSION) - nothing to send here any more
    readStatus.state = readParams.mirrorCheck ? M7R_MIRRORCHECK : M7R_READING;
    blockBase = 0;
    blockDone = 0;
    return true;
  }
  case M7R_MIRRORCHECK:
  {
    // read 64 B at the flash base and the same 64 B one flashSize/2 higher; if they match the chip is
    // a 512 KB part mirrored into the 1 MB window (NefMoto MirroredFlashLayoutHelper), so read only 512 KB.
    setMessage("checking flash size");
    uint8_t lowSample[64], highSample[64];
    uint32_t half = readParams.flashSize / 2;
    if (readAny(readParams.flashBase, sizeof(lowSample), lowSample, sizeof(lowSample)) != (int32_t)sizeof(lowSample) ||
        readAny(readParams.flashBase + half, sizeof(highSample), highSample, sizeof(highSample)) != (int32_t)sizeof(highSample))
    {
      // upload not allowed here = the usual "read disabled" case; say so clearly
      if (kwp2000LastNrc() == 0x11 || kwp2000LastNrc() == 0x10)
      {
        fail("ECU refused RequestUpload - reading may be disabled on this software");
      }
      else
      {
        fail("could not read flash (RequestUpload failed)");
      }
      kwp2000Disconnect();
      return false;
    }
    if (memcmp(lowSample, highSample, sizeof(lowSample)) == 0)
    {
      readStatus.bytesTotal = half; // 512 KB
      setMessage("512 KB chip detected");
    }
    readStatus.state = M7R_READING;
    blockBase = 0;
    blockDone = 0;
    return true;
  }
  case M7R_READING:
  {
    if (dumpBuffer == NULL)
    {
      // the first read since start-up: room for the biggest chip (1 MB) in PSRAM, kept for every read after this
      dumpBuffer = (uint8_t *)ps_malloc(0x100000);
      if (dumpBuffer == NULL)
      {
        fail("not enough memory for the read");
        kwp2000Disconnect();
        return false;
      }
    }
    if (readStatus.bytesTotal > 0x100000)
    {
      fail("flash size over 1 MB");
      kwp2000Disconnect();
      return false;
    }
    uint32_t remaining = readStatus.bytesTotal - blockBase;
    uint32_t size = remaining < READ_BLOCK ? remaining : READ_BLOCK;
    blockDone = 0;
    snprintf(readStatus.message, sizeof(readStatus.message), "reading 0x%06lX",
             (unsigned long)(readParams.flashBase + blockBase));
    int32_t got = readAny(readParams.flashBase + blockBase, size, NULL, 0);
    if (got != (int32_t)size)
    {
      fail("transfer failed mid-read");
      kwp2000Disconnect();
      return false;
    }
    blockBase += size;
    blockDone = 0; // fully counted in blockBase now; avoid double-counting in updateProgressTiming()
    updateProgressTiming();
    if (blockBase >= readStatus.bytesTotal)
    {
      readStatus.state = readParams.verify ? M7R_VERIFY : M7R_DONE;
    }
    return true;
  }
  case M7R_VERIFY:
  {
    setMessage("verifying");
    uint8_t again[64];
    if (readAny(readParams.flashBase, sizeof(again), again, sizeof(again)) != (int32_t)sizeof(again))
    {
      setMessage("read OK (verify read failed)"); // the dump is complete; only the confirmation step failed
    }
    else if (memcmp(again, dumpBuffer, sizeof(again)) == 0) // the start of the flash again, against the dump
    {
      setMessage("read complete, verified");
    }
    else
    {
      setMessage("read complete (verify mismatch)");
    }
    readStatus.state = M7R_DONE;
    return true;
  }
  case M7R_DONE:
    dumpComplete = true;
    readStatus.percent = 100;
    updateProgressTiming();
    kwp2000Disconnect();
    return false;
  case M7R_ERROR:
  default:
    return false;
  }
}
