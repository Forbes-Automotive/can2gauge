#include <can2gauge_Drag.h>
#include <Preferences.h> // NVS: the scoreboard is kept under its own name, next to (not in) the settings

// ---------------------------------------------------------------------------
// Drag timer scoreboard (0.38.0), see can2gauge_Drag.h. The screen adds runs from the LVGL task and the web server reads
// and clears them from its own, so every change to the board happens under boardMux, and the NVS write is done from a
// copy afterwards.
// ---------------------------------------------------------------------------

#define DRAG_MAGIC 0xD7A6  // marks a saved board as this one
#define DRAG_VERSION 1     // the saved board's layout (a different one starts empty)

// the whole board, as saved
struct DragBoard
{
  uint16_t magic;                            // DRAG_MAGIC
  uint8_t version;                           // DRAG_VERSION
  uint8_t count[DRAG_KINDS];                 // times on the board, each kind
  uint32_t runs[DRAG_KINDS];                 // runs finished, each kind (numbers the runs)
  DragRun best[DRAG_KINDS][DRAG_SCORES];     // fastest first
};

static DragBoard board;                                      // the scoreboard
static portMUX_TYPE boardMux = portMUX_INITIALIZER_UNLOCKED; // guards it (the LVGL task and the web server use it)

// writes a copy of the board to NVS (called after each change, outside boardMux: NVS writes take a few ms)
static void saveBoard()
{
  DragBoard copy;
  portENTER_CRITICAL(&boardMux);
  copy = board;
  portEXIT_CRITICAL(&boardMux);
  Preferences prefs; // its own Preferences, so it never closes the settings' one (can2gauge_EEP.cpp)
  prefs.begin("c2gdrag", false);
  prefs.putBytes("board", &copy, sizeof(copy));
  prefs.end();
}

void setupDrag()
{
  DEBUG("Setting up the drag timer...");
  memset(&board, 0, sizeof(board));
  Preferences prefs;
  prefs.begin("c2gdrag", false);
  if (prefs.getBytesLength("board") == sizeof(DragBoard))
  {
    prefs.getBytes("board", &board, sizeof(board));
  }
  prefs.end();
  if (board.magic != DRAG_MAGIC || board.version != DRAG_VERSION)
  {
    memset(&board, 0, sizeof(board)); // nothing saved yet (or another layout): an empty board
    board.magic = DRAG_MAGIC;
    board.version = DRAG_VERSION;
  }
  for (uint8_t kind = 0; kind < DRAG_KINDS; kind++)
  {
    if (board.count[kind] > DRAG_SCORES)
    {
      board.count[kind] = DRAG_SCORES; // never more than fit
    }
  }
  DEBUG("Set up the drag timer! (%u + %u times on the board)", board.count[DRAG_KIND_KMH], board.count[DRAG_KIND_MPH]);
}

uint8_t dragAddRun(uint8_t kind, uint32_t timeMs)
{
  if (kind >= DRAG_KINDS)
  {
    return 0;
  }
  uint8_t place = 0;
  portENTER_CRITICAL(&boardMux);
  board.runs[kind]++;
  // where it goes: after every time that is as quick or quicker
  uint8_t slot = 0;
  while (slot < board.count[kind] && board.best[kind][slot].timeMs <= timeMs)
  {
    slot++;
  }
  if (slot < DRAG_SCORES)
  {
    // move the slower ones down one (the slowest falls off a full board)
    uint8_t last = board.count[kind];
    if (last >= DRAG_SCORES)
    {
      last = DRAG_SCORES - 1;
    }
    for (uint8_t i = last; i > slot; i--)
    {
      board.best[kind][i] = board.best[kind][i - 1];
    }
    board.best[kind][slot].timeMs = timeMs;
    board.best[kind][slot].runNumber = board.runs[kind];
    if (board.count[kind] < DRAG_SCORES)
    {
      board.count[kind]++;
    }
    place = slot + 1;
  }
  portEXIT_CRITICAL(&boardMux);
  saveBoard(); // the run count changed even when the time didn't make the board
  return place;
}

uint8_t dragScoreCount(uint8_t kind)
{
  if (kind >= DRAG_KINDS)
  {
    return 0;
  }
  return board.count[kind];
}

DragRun dragScore(uint8_t kind, uint8_t place)
{
  DragRun run;
  run.timeMs = 0;
  run.runNumber = 0;
  if (kind >= DRAG_KINDS || place >= DRAG_SCORES)
  {
    return run;
  }
  portENTER_CRITICAL(&boardMux);
  if (place < board.count[kind])
  {
    run = board.best[kind][place];
  }
  portEXIT_CRITICAL(&boardMux);
  return run;
}

uint32_t dragRunCount(uint8_t kind)
{
  if (kind >= DRAG_KINDS)
  {
    return 0;
  }
  return board.runs[kind];
}

void dragClear(uint8_t kind)
{
  if (kind >= DRAG_KINDS)
  {
    return;
  }
  portENTER_CRITICAL(&boardMux);
  board.count[kind] = 0;
  portEXIT_CRITICAL(&boardMux);
  saveBoard();
}

void dragDelete(uint8_t kind, uint8_t place)
{
  if (kind >= DRAG_KINDS)
  {
    return;
  }
  portENTER_CRITICAL(&boardMux);
  if (place < board.count[kind])
  {
    // move the slower ones up one into its place
    for (uint8_t i = place; i + 1 < board.count[kind]; i++)
    {
      board.best[kind][i] = board.best[kind][i + 1];
    }
    board.count[kind]--;
  }
  portEXIT_CRITICAL(&boardMux);
  saveBoard();
}
