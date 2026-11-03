#include <can2gauge_RAM.h>
#include <esp_heap_caps.h> // heap_caps_malloc_prefer, the failed-request hook
#include <esp_system.h>    // esp_reset_reason
#include <esp_attr.h>      // RTC_NOINIT_ATTR
#include <esp_rom_sys.h>   // esp_rom_printf (prints without asking for memory)

// ---------------------------------------------------------------------------
// JSON in PSRAM
// ---------------------------------------------------------------------------
// ArduinoJson asks for its memory through an "Allocator": a small class with three functions (take, give back, resize)
// that the library calls. It has to be a class because that's the shape the library expects - "override" marks each
// function as our version of the library's. Each one asks for PSRAM first and falls back to internal RAM.
struct PsramJsonAllocator : ArduinoJson::Allocator
{
  void *allocate(size_t size) override
  {
    return heap_caps_malloc_prefer(size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_DEFAULT);
  }
  void deallocate(void *pointer) override
  {
    heap_caps_free(pointer); // works for either kind of memory
  }
  void *reallocate(void *pointer, size_t newSize) override
  {
    return heap_caps_realloc_prefer(pointer, newSize, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_DEFAULT);
  }
};

static PsramJsonAllocator psramJson; // the one allocator every JsonDocument in the API shares (it holds nothing itself)

ArduinoJson::Allocator *psramJsonAllocator()
{
  return &psramJson;
}

// ---------------------------------------------------------------------------
// the note of failed memory requests
// ---------------------------------------------------------------------------
#define RAM_NOTE_MAGIC 0xC26A4D01 // marks the kept note as real (after a power-on that memory holds rubbish)

// kept through a reset (not a power cut): RTC memory that start-up doesn't clear
RTC_NOINIT_ATTR static uint32_t keptMagic;  // RAM_NOTE_MAGIC when keptNote is real
RTC_NOINIT_ATTR static RamFailNote keptNote; // this boot's note, copied here as it changes

static RamFailNote nowNote = {};      // this boot's failed requests
static RamFailNote lastBootNote = {};  // the boot before this one's

// called by ESP-IDF, in the task that asked, whenever a memory request can't be met. It must not ask for memory itself.
static void ramFailHook(size_t size, uint32_t caps, const char *functionName)
{
  nowNote.count++;
  nowNote.size = size;
  nowNote.caps = caps;
  nowNote.uptimeS = millis() / 1000;
  const char *taskName = pcTaskGetName(NULL); // the task that asked
  copyStr(nowNote.task, sizeof(nowNote.task), taskName);
  keptNote = nowNote; // kept in case this ends in a reset
  keptMagic = RAM_NOTE_MAGIC;
  esp_rom_printf("RAM: request for %u bytes (caps 0x%x, %s) failed in task %s\n", (unsigned)size, (unsigned)caps,
                 functionName ? functionName : "?", nowNote.task);
}

void setupRam()
{
  DEBUG("Setting up the RAM note...");
  // the note the last boot left, unless this is a power-on (the RTC memory is rubbish then)
  if (keptMagic == RAM_NOTE_MAGIC && esp_reset_reason() != ESP_RST_POWERON && keptNote.count > 0)
  {
    lastBootNote = keptNote;
    lastBootNote.task[sizeof(lastBootNote.task) - 1] = '\0'; // in case it was half written
    DEBUG("Last boot: memory ran out %u times, the last %u bytes (caps 0x%x) in task %s at %u s",
          (unsigned)lastBootNote.count, (unsigned)lastBootNote.size, (unsigned)lastBootNote.caps, lastBootNote.task,
          (unsigned)lastBootNote.uptimeS);
  }
  // start this boot's note empty
  memset(&keptNote, 0, sizeof(keptNote));
  keptMagic = RAM_NOTE_MAGIC;
  heap_caps_register_failed_alloc_callback(ramFailHook);
  DEBUG("Set up the RAM note!");
}

const RamFailNote &ramFailNow()
{
  return nowNote;
}

const RamFailNote &ramFailLastBoot()
{
  return lastBootNote;
}
