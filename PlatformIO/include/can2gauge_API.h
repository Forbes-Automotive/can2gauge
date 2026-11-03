#pragma once
#include <can2gauge_defs.h>
#include <ESPAsyncWebServer.h> // AsyncWebServer / AsyncWebServerRequest in the API + used by OTA

extern AsyncWebServer webServer;
void setupWebServer(); // LittleFS static files + API routes + OTA upload
// 0.17.3: the USB bench console's way into the API: runs the handler for method ("GET" / "POST") + path with body,
// and the reply is printed on the USB serial as "@@C2G <code> <json>". false = not a route the console can use.
bool apiConsoleRequest(const char *method, const char *path, const String &body);

// 0.17.4: the filesystem's size and how much is used. LittleFS works "used" out by walking every file (9.5 MB in use:
// over 5 s), and /api/sprites did that on the web server task at every web UI load - the K-line task on the same core
// lost its byte timing and the watchdog reset the gauge. Now worked out on the house task (core 1) only after a change.
void fsUsage(size_t &total, size_t &used); // the last numbers worked out (0 until the first walk)
void fsUsageChanged();                     // a file was written or deleted: work them out again
void fsUsageTick();                        // house task: does the walk when something changed
