#include <can2gauge_Labels.h>
#include <esp_attr.h> // EXT_RAM_BSS_ATTR (0.28.2): a buffer marked with it lives in PSRAM, not internal RAM - internal RAM is kept for WiFi, Bluetooth and task stacks
#include <LittleFS.h>
#include <miniz.h> // the ESP32-S3 ROM's tinfl (raw deflate)
#include <FsUnzip.h> // 0.24.0: the label index ships zipped

#define LBL_INDEX "/lbl/index.txt"
#define LBL_CHOICE "/lbl/choice.txt"
#define LBL_PAK "/lbl/labels.pak" // every label file, deflated (tools/labels/lbl_tool.py write_pak)
#define LBL_PAK_NAME 40

// the loaded file: its text in PSRAM, tabs replaced by '\0', and per group / field pointers to name, name 2, help
static char *labelText = NULL;
static const char *(*labelEntry)[5][3] = NULL; // [256][5][3]
static SemaphoreHandle_t choiceMutex = NULL;
static SemaphoreHandle_t findMutex = NULL; // one labelsFind at a time (the K-line task and the web server both call it)

static uint8_t findLabelFiles(uint8_t addr, const char *ident, char out[][LBL_NAME_LEN], uint8_t max); // below

// makes the lookup's lock (in setup, before the K-line task and the web server start)
void setupLabels()
{
  findMutex = xSemaphoreCreateMutex();
}

void labelsPartOf(const char *ident, char *part, size_t len)
{
  size_t outLen = 0;
  for (const char *pos = ident; *pos && outLen + 1 < len && outLen < 14; pos++)
  {
    if (pos[0] == ' ' && pos[1] == ' ')
    {
      break; // the part number ends at the first double space
    }
    if (*pos == ' ' || *pos == '-')
    {
      continue;
    }
    part[outLen++] = (char)toupper((unsigned char)*pos);
  }
  part[outLen] = '\0';
}

// '?' = any character; a 9-character mask is the part number with any index; a mask's trailing '?' also match a
// shorter index (1J?920??6??? matches 1J0920826C) - tools/labels/lbl_tool.py mask_match
static bool maskMatch(const char *mask, const char *part)
{
  const size_t maskLen = strlen(mask);
  size_t partLen = strlen(part);
  if (maskLen == 9 && partLen > 9)
  {
    partLen = 9;
  }
  if (partLen > maskLen || partLen < 9)
  {
    return false;
  }
  for (size_t i = 0; i < maskLen; i++)
  {
    if (mask[i] == '?')
    {
      continue;
    }
    if (i >= partLen || mask[i] != part[i])
    {
      return false;
    }
  }
  return true;
}

static size_t readLine(File &file, char *buf, size_t len)
{
  size_t count = file.readBytesUntil('\n', buf, len - 1);
  buf[count] = '\0';
  if (count && buf[count - 1] == '\r')
  {
    buf[--count] = '\0';
  }
  return count;
}

static uint8_t splitTabs(char *line, char **fields, uint8_t max)
{
  uint8_t count = 0;
  char *pos = line;
  while (count < max)
  {
    fields[count++] = pos;
    char *tab = strchr(pos, '\t');
    if (!tab)
    {
      break;
    }
    *tab = '\0';
    pos = tab + 1;
  }
  return count;
}

// the web UI's pick for this module, if any
static bool choiceFor(const char *part, uint8_t addr, char *file, size_t len)
{
  File choices = LittleFS.open(LBL_CHOICE, "r");
  if (!choices)
  {
    return false;
  }
  char line[96];
  char *fields[3];
  bool found = false;
  while (choices.available() && !found)
  {
    if (readLine(choices, line, sizeof(line)) && splitTabs(line, fields, 3) == 3 && !strncmp(fields[0], part, 9) &&
        strlen(fields[0]) == 9 && strtoul(fields[1], NULL, 16) == addr)
    {
      copyStr(file, len, fields[2]);
      found = true;
    }
  }
  choices.close();
  return found;
}

// 0.17.4: the whole label index (/lbl/index.txt, ~170 KB), read into PSRAM the first time it's needed and kept. Read a
// byte at a time from flash it took 1.9 s on every connect, and a KWP1281 module hangs up long before that: the K-line
// page never got its first block. The index only changes with a new web UI image, which restarts the gauge.
static char *indexText = NULL;

bool labelsPrepare()
{
  if (indexText != NULL)
  {
    return true; // already in memory
  }
  // 0.24.0: the index ships zipped (/lbl/index.txt.gz, 169 KB -> 21 KB). fsReadAll() unzips it into PSRAM with a '\0'
  // after it (one read, as before), and still reads a plain index.txt from an older filesystem image.
  size_t size = 0;
  uint8_t *text = fsReadAll(LBL_INDEX, &size);
  if (text == NULL)
  {
    return false;
  }
  indexText = (char *)text; // kept for good, never freed
  return true;
}

// the lookup below shares one list of matches, so only one task does it at a time
uint8_t labelsFind(uint8_t addr, const char *ident, char out[][LBL_NAME_LEN], uint8_t max)
{
  if (findMutex == NULL)
  {
    return 0; // setupLabels() hasn't run
  }
  xSemaphoreTake(findMutex, portMAX_DELAY);
  const uint8_t count = findLabelFiles(addr, ident, out, max);
  xSemaphoreGive(findMutex);
  return count;
}

static uint8_t findLabelFiles(uint8_t addr, const char *ident, char out[][LBL_NAME_LEN], uint8_t max)
{
  char part[16];
  char identUpper[64];
  labelsPartOf(ident, part, sizeof(part));
  size_t i = 0;
  for (; ident[i] && i < sizeof(identUpper) - 1; i++)
  {
    identUpper[i] = (char)toupper((unsigned char)ident[i]);
  }
  identUpper[i] = '\0';
  uint8_t count = 0;
  char chosen[LBL_NAME_LEN];
  if (choiceFor(part, addr, chosen, sizeof(chosen)) && chosen[0] && max)
  {
    copyStr(out[count++], LBL_NAME_LEN, chosen);
  }

  if (!labelsPrepare())
  {
    return count;
  }
  struct Hit
  {
    uint8_t rank;
    int8_t score;
    char name[LBL_NAME_LEN];
  };
  EXT_RAM_BSS_ATTR static Hit hits[24]; // shared: labelsFind's lock keeps it to one task at a time
  uint8_t hitCount = 0;
  char line[128];
  char *fields[5] = {};
  const char *lineStart = indexText; // where the next line of the index starts
  while (*lineStart != '\0')
  {
    // copy one line out of the index (splitTabs cuts it up), and move lineStart on to the next one
    const char *lineEnd = strchr(lineStart, '\n');
    size_t lineLength = 0;
    if (lineEnd == NULL)
    {
      lineLength = strlen(lineStart); // the last line, with no '\n' after it
    }
    else
    {
      lineLength = lineEnd - lineStart;
    }
    const char *nextLine = lineStart + lineLength;
    if (lineEnd != NULL)
    {
      nextLine++; // past the '\n'
    }
    if (lineLength >= sizeof(line))
    {
      lineLength = sizeof(line) - 1; // an over-long line is cut short, as readLine() did
    }
    memcpy(line, lineStart, lineLength);
    line[lineLength] = '\0';
    if (lineLength > 0 && line[lineLength - 1] == '\r')
    {
      lineLength--;
      line[lineLength] = '\0'; // a Windows line end
    }
    lineStart = nextLine;
    if (lineLength == 0)
    {
      continue;
    }
    const uint8_t fieldCount = splitTabs(line, fields, 5);
    if (fieldCount < 4)
    {
      continue;
    }
    if (fields[2][0] && strtoul(fields[2], NULL, 16) != addr)
    {
      continue;
    }
    const uint8_t rank = (uint8_t)atoi(fields[0]);
    const bool ok = rank == 5 ? !strncmp(part, fields[1], strlen(fields[1])) : maskMatch(fields[1], part);
    if (!ok || hitCount >= sizeof(hits) / sizeof(hits[0]))
    {
      continue;
    }
    int8_t score = 0; // variant words (3+ letters) named in the identification: "ESP" in "ABS/ESP allrad"
    if (fieldCount > 4 && fields[4][0]) // the variant column is optional: never read last line's pointer
    {
      char variant[24];
      copyStr(variant, sizeof(variant), fields[4]);
      char *save = NULL;
      for (char *word = strtok_r(variant, "-", &save); word; word = strtok_r(NULL, "-", &save))
      {
        if (strlen(word) >= 3 && strstr(identUpper, word))
        {
          score++;
        }
      }
    }
    hits[hitCount].rank = rank;
    hits[hitCount].score = score;
    copyStr(hits[hitCount].name, LBL_NAME_LEN, fields[3]);
    hitCount++;
  }

  for (uint8_t pos = 1; pos < hitCount; pos++) // by rank, then score (stable: the index order breaks ties)
  {
    Hit hit = hits[pos];
    int slot = pos - 1;
    while (slot >= 0 && (hits[slot].rank > hit.rank || (hits[slot].rank == hit.rank && hits[slot].score < hit.score)))
    {
      hits[slot + 1] = hits[slot];
      slot--;
    }
    hits[slot + 1] = hit;
  }

  for (uint8_t hitIndex = 0; hitIndex < hitCount && count < max; hitIndex++)
  {
    bool dup = false;
    for (uint8_t outIndex = 0; outIndex < count && !dup; outIndex++)
    {
      dup = !strcmp(out[outIndex], hits[hitIndex].name);
    }
    if (!dup)
    {
      copyStr(out[count++], LBL_NAME_LEN, hits[hitIndex].name);
    }
  }
  return count;
}

// unzips one label file (raw deflate) into out; returns the bytes unzipped, or 0 if it failed.
// 0.17.4: tinfl_decompress_mem_to_mem() kept its ~11 KB working area on the stack of whichever task called it - more
// than the web server's or the K-line task's 8 KB - so opening a label file reset the gauge (K-line tab, K-line mode,
// a gauge page with a K-line source). Here the working area is borrowed from PSRAM instead.
static size_t unzipLabelFile(char *out, size_t outSize, const uint8_t *in, size_t inSize)
{
  tinfl_decompressor *unzipper = (tinfl_decompressor *)ps_malloc(sizeof(tinfl_decompressor)); // the ~11 KB working area
  if (unzipper == NULL)
  {
    return 0;
  }
  tinfl_init(unzipper); // start from the beginning of a new file
  size_t inBytes = inSize;   // in: bytes we give it, out: bytes it used
  size_t outBytes = outSize; // in: room we give it, out: bytes it wrote
  const tinfl_status status = tinfl_decompress(unzipper, in, &inBytes, (mz_uint8 *)out, (mz_uint8 *)out, &outBytes,
                                               TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF); // the whole file fits in out
  free(unzipper);
  if (status != TINFL_STATUS_DONE)
  {
    return 0;
  }
  return outBytes;
}

// one label file out of the pack: its text (malloc'd in PSRAM, NUL-terminated) or NULL. The table is sorted by name.
char *labelsReadFile(const char *file, size_t *len)
{
  if (!file || !file[0] || strlen(file) >= LBL_PAK_NAME)
  {
    return NULL;
  }
  char want[LBL_PAK_NAME];
  size_t wantLen = 0;
  for (; file[wantLen] && wantLen < sizeof(want) - 1; wantLen++)
  {
    want[wantLen] = (char)toupper((unsigned char)file[wantLen]);
  }
  want[wantLen] = '\0';
  File pak = LittleFS.open(LBL_PAK, "r");
  if (!pak)
  {
    return NULL;
  }
  uint8_t head[8];
  struct __attribute__((packed)) Entry
  {
    char name[LBL_PAK_NAME];
    uint32_t offset, packed, size;
  } entry;
  bool found = false;
  if (pak.read(head, sizeof(head)) == sizeof(head) && !memcmp(head, "LBLP", 4) && (head[4] | head[5] << 8) == 1)
  {
    int lo = 0;
    int hi = (int)(head[6] | head[7] << 8) - 1;
    while (lo <= hi && !found)
    {
      const int mid = (lo + hi) / 2;
      if (!pak.seek(sizeof(head) + mid * sizeof(Entry)) || pak.read((uint8_t *)&entry, sizeof(entry)) != sizeof(entry))
      {
        break;
      }
      entry.name[LBL_PAK_NAME - 1] = '\0';
      const int compare = strcmp(want, entry.name);
      if (!compare)
      {
        found = true;
      }
      else if (compare < 0)
      {
        hi = mid - 1;
      }
      else
      {
        lo = mid + 1;
      }
    }
  }
  char *text = NULL;
  uint8_t *packed = NULL;
  if (found && entry.size < 512 * 1024 && entry.offset + entry.packed <= pak.size())
  {
    packed = (uint8_t *)ps_malloc(entry.packed);
    text = (char *)ps_malloc(entry.size + 1);
    if (packed && text && pak.seek(entry.offset) && pak.read(packed, entry.packed) == entry.packed &&
        unzipLabelFile(text, entry.size, packed, entry.packed) == entry.size)
    {
      text[entry.size] = '\0';
      if (len)
      {
        *len = entry.size;
      }
    }
    else
    {
      free(text);
      text = NULL;
    }
  }
  free(packed);
  pak.close();
  return text;
}

bool labelsLoad(const char *file)
{
  free(labelText);
  labelText = NULL;
  if (!labelEntry)
  {
    labelEntry = (const char *(*)[5][3])ps_calloc(256, sizeof(*labelEntry));
  }
  if (!labelEntry)
  {
    return false;
  }
  memset(labelEntry, 0, 256 * sizeof(*labelEntry));
  if (!file || !file[0])
  {
    return false;
  }
  labelText = labelsReadFile(file, NULL);
  if (!labelText)
  {
    return false;
  }
  for (char *line = labelText; line && *line;)
  {
    char *next = strchr(line, '\n');
    if (next)
    {
      *next++ = '\0';
    }
    char *fields[5] = {};
    const uint8_t count = splitTabs(line, fields, 5);
    const int group = atoi(fields[0]);
    const int field = count > 1 ? atoi(fields[1]) : -1;
    if (count >= 3 && group >= 0 && group < 256 && field >= 0 && field < 5)
    {
      labelEntry[group][field][0] = fields[2];
      labelEntry[group][field][1] = count > 3 ? fields[3] : "";
      labelEntry[group][field][2] = count > 4 ? fields[4] : "";
    }
    line = next;
  }
  return true;
}

bool labelsField(uint8_t group, uint8_t field, char *name, size_t nameLen, char *spec, size_t specLen)
{
  name[0] = '\0';
  if (spec && specLen)
  {
    spec[0] = '\0';
  }
  if (!labelText || !labelEntry || field > 4 || !labelEntry[group][field][0])
  {
    return false;
  }
  const char *name1 = labelEntry[group][field][0];
  const char *name2 = labelEntry[group][field][1];
  snprintf(name, nameLen, (name1[0] && name2[0]) ? "%s %s" : "%s%s", name1, name2);
  if (spec && specLen)
  {
    copyStr(spec, specLen, labelEntry[group][field][2]);
  }
  return true;
}

void labelsRemember(uint8_t addr, const char *file, const char *ident)
{
  if (!choiceMutex)
  {
    choiceMutex = xSemaphoreCreateMutex();
  }
  char newLine[128];
  snprintf(newLine, sizeof(newLine), "%02X\t%s\t%s", addr, file ? file : "", ident ? ident : "");
  for (char *character = newLine; *character; character++)
  {
    if (*character == '\n' || *character == '\r')
    {
      *character = ' ';
    }
  }
  xSemaphoreTake(choiceMutex, portMAX_DELAY);
  String keep;
  bool same = false;
  File known = LittleFS.open("/lbl/known.txt", "r");
  if (known)
  {
    char line[128];
    char key[4];
    snprintf(key, sizeof(key), "%02X\t", addr);
    while (known.available())
    {
      if (!readLine(known, line, sizeof(line)))
      {
        continue;
      }
      if (!strncmp(line, key, 3))
      {
        same = !strcmp(line, newLine);
      }
      else
      {
        keep += String(line) + "\n";
      }
    }
    known.close();
  }
  if (!same)
  {
    keep += String(newLine) + "\n";
    known = LittleFS.open("/lbl/known.txt", "w");
    if (known)
    {
      known.print(keep);
      known.close();
    }
  }
  xSemaphoreGive(choiceMutex);
}

bool labelsSetChoice(const char *ident, uint8_t addr, const char *file)
{
  if (!choiceMutex)
  {
    choiceMutex = xSemaphoreCreateMutex();
  }
  char part[16];
  labelsPartOf(ident, part, sizeof(part));
  if (strlen(part) < 9)
  {
    return false;
  }
  part[9] = '\0';
  xSemaphoreTake(choiceMutex, portMAX_DELAY);
  // rewrite the file without this module's line, then add the new pick
  String keep;
  File choices = LittleFS.open(LBL_CHOICE, "r");
  if (choices)
  {
    char line[96];
    while (choices.available())
    {
      if (!readLine(choices, line, sizeof(line)))
      {
        continue;
      }
      char key[16];
      snprintf(key, sizeof(key), "%s\t%02X\t", part, addr);
      if (strncmp(line, key, strlen(key)))
      {
        keep += String(line) + "\n";
      }
    }
    choices.close();
  }
  if (file && file[0])
  {
    char addLine[80];
    snprintf(addLine, sizeof(addLine), "%s\t%02X\t%s\n", part, addr, file);
    keep += addLine;
  }
  choices = LittleFS.open(LBL_CHOICE, "w");
  const bool ok = choices && choices.print(keep) == keep.length();
  if (choices)
  {
    choices.close();
  }
  xSemaphoreGive(choiceMutex);
  return ok;
}
