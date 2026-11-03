#pragma once
#include <can2gauge_defs.h>

// ---------------------------------------------------------------------------
// Measuring-block labels for K-line mode, from the label files (tools/labels/lbl_tool.py pack -> data/lbl/):
//   /lbl/index.txt   rank, part-number mask, address, file, variant - how a module finds its file
//   /lbl/<FILE>.txt  group, field (0 = title), name, second name, help / specification
//   /lbl/choice.txt  the web UI's pick per module: part number (9 characters), address, file ("" = auto)
// The lookup follows the label files' own rules: the part number with its index, REDIRECTs, files split by engine code / variant, wildcard
// files per address, then chassis + address files (1J-17). Used from the K-line task only (labelsSetChoice: any task).
// ---------------------------------------------------------------------------
#define LBL_NAME_LEN 40
#define LBL_MAX_CANDS 6

void setupLabels(); // the lookup's lock: call in setup, before setupKLine and the web server
void labelsPartOf(const char *ident, char *part, size_t len); // "06A906032HN  1.8L R4/5VT ..." -> "06A906032HN"
// 0.17.4: reads /lbl/index.txt into PSRAM (once; later calls return at once). The K-line task calls it before it wakes
// a module, so finding the label file after the module answers takes milliseconds, not seconds. false = no index
bool labelsPrepare();
// the files that fit a module, best first (the web UI's pick first); returns the count
uint8_t labelsFind(uint8_t addr, const char *ident, char out[][LBL_NAME_LEN], uint8_t max);
bool labelsLoad(const char *file); // "" or a missing file unloads
// one label file out of /lbl/labels.pak (deflated, tools/labels/lbl_tool.py): its text, malloc'd (free() it), or NULL
char *labelsReadFile(const char *file, size_t *len);
// a field's name ("Coolant Temperature") and specification from the loaded file; field 0 = the block's title
bool labelsField(uint8_t group, uint8_t field, char *name, size_t nameLen, char *spec, size_t specLen);
bool labelsSetChoice(const char *ident, uint8_t addr, const char *file); // "" = back to automatic
// /lbl/known.txt: the label file and identification last seen per module address (the web UI's page editor names
// blocks and fields from it); rewritten only when something changed
void labelsRemember(uint8_t addr, const char *file, const char *ident);
