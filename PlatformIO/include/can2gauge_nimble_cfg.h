#pragma once

// Bluetooth (NimBLE) settings for can2gauge, read at the very start of every file (platformio.ini: -include).
//
// Why this file exists: the Arduino framework comes ready-built with its own settings file (sdkconfig.h), which
// NimBLE-Arduino reads first, and it only fills in a value the framework hasn't set. The framework sets these for
// a general-purpose board: 3 connections, every role, and ALL of NimBLE's buffers in internal RAM. Putting the same
// names in platformio.ini's build_flags did nothing, because the framework's file is read after them and wins.
// 2026-10-14: Bluetooth took 59 KB of internal RAM and left the gauge 16 KB.
//
// So this reads the framework's file first (it only ever loads once), then replaces the Bluetooth values with ours.
// Only NimBLE-Arduino (compiled with the project) reads these names; nothing else changes.
#include "sdkconfig.h"

// NimBLE's buffers (messages, data to and from the radio, events) in PSRAM instead of internal RAM
#undef CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_INTERNAL
#define CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL 1

// two connections: the OBD adapter (the gauge connects out to it) and, since 0.30.0, the can2gauge Link phone app (it
// connects in to the gauge for the nav link). The framework allowed 3, and sizes the buffers and the radio controller
// for them. Until 0.30.0 this was 1.
#undef CONFIG_BT_NIMBLE_MAX_CONNECTIONS
#define CONFIG_BT_NIMBLE_MAX_CONNECTIONS 2

// Roles: the gauge scans (observer) and connects out to the OBD adapter (central). 0.30.0: it also advertises
// (broadcaster) and takes the phone app's connection (peripheral) for the nav link, so those two roles - switched off
// here until 0.30.0 - are back on as the framework sets them. They only cost RAM while Bluetooth is running, and
// Bluetooth only starts when the OBD adapter or the nav link is switched on.

// The framework's file also has each setting under an old name (CONFIG_NIMBLE_...). NimBLE's nimconfig_rename.h
// sees an old name set and the new one at 0, takes that as "only the old name was set", and redefines the new name as
// empty - which breaks the build ("operator '&&' has no right operand"). So the old names for what we change go too.
#undef CONFIG_NIMBLE_MEM_ALLOC_MODE_INTERNAL

// the Bluetooth host task on core 0, so LVGL keeps core 1 to itself
#undef CONFIG_BT_NIMBLE_PINNED_TO_CORE
#define CONFIG_BT_NIMBLE_PINNED_TO_CORE 0

// the Bluetooth host task's stack (internal RAM): 5 KB in the framework, and it had 2.7 KB it never used
#undef CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE
#define CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE 4096

// 0.24.6: the radio controller's own sizes. It is ready-built, but NimBLE-Arduino hands it these numbers when it starts
// it (BT_CONTROLLER_INIT_CONFIG_DEFAULT), and it takes internal RAM for each:
// - activities at once: 6 in the framework. The gauge scans, advertises (nav link) and holds two connections (the
//   adapter and the phone): 4. Was 3 until 0.30.0 (scan + one connection + a spare).
#undef CONFIG_BT_CTRL_BLE_MAX_ACT
#define CONFIG_BT_CTRL_BLE_MAX_ACT 4
#undef CONFIG_BT_CTRL_BLE_MAX_ACT_EFF
#define CONFIG_BT_CTRL_BLE_MAX_ACT_EFF 4
// - devices remembered while scanning, so each is reported once: 100 in the framework; a scan finds a handful
#undef CONFIG_BT_CTRL_SCAN_DUPL_CACHE_SIZE
#define CONFIG_BT_CTRL_SCAN_DUPL_CACHE_SIZE 20
