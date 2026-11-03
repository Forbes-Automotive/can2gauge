#pragma once
#include <can2gauge_defs.h>

// ---------------------------------------------------------------------------
// USB bench console (0.17.3). A PC script (tools/diag/kline_bench.py) talks to the gauge over the USB cable, one
// command per line, so the K-line can be tested on the bench without WiFi. Every reply is one line starting with
// CONSOLE_MARK ("@@C2G <code> <json>"); everything else on the serial is the normal debug output.
//
//   PING                       -> @@C2G 200 {"version":..., "uptime":...}
//   GET <path>                 -> the same JSON the web UI gets (apiConsoleRequest: status, settings, kline/block,
//   POST <path> <json>            me7/vars, me7read/..., files, oil, service, signals, settings and action posts)
//   FILEGET <path>             -> @@C2G 200 {"size":n} then "@@B64 <base64>" lines, then @@C2G 200 {"size","crc32"}
//   FILEPUT <path>             -> starts a new file (replaces one already there)
//   FILEDATA <base64>          -> appends to it; each line answered @@C2G 200 {"bytes":total so far}
//   FILEEND                    -> closes it: @@C2G 200 {"size","crc32"}
//   (0.17.4) FILEGET /api/me7read/bin sends the ECU flash read held in memory the same way.
//   (0.17.4) FILEDEL <path>    -> removes a file in /me7/ or /logs/ (the folders FILEPUT may write to)
//
// 0.17.4: keep each command line under ~250 characters. The USB serial takes in 256 bytes at a time and the console
// task (low priority) can be slow to empty it, so a longer line loses its middle ("bad base64"). The script sends
// 144 file bytes per FILEDATA line and waits for each reply.
// ---------------------------------------------------------------------------

void setupConsole(); // starts the console task (does nothing with enableConsole 0)
