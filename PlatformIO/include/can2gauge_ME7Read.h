#pragma once
#include <ME7Read.h>

// Firmware glue for the ME7 flash read (see src/can2gauge_ME7Read.cpp and lib/ME7Read).
// The web API calls me7ReadRequest(); the K-line task calls me7ReadService() when one is pending.

// ===== Asking for a read (web API) =====
bool me7ReadRequest(const Me7ReadParams &params); // queue a read; false if one is already running
bool me7ReadPending();                           // a read has been queued but not started
const char *me7ReadLastFile();                   // the .bin the last read was saved to, "" if none / only in memory

// ===== Running it (K-line task) =====
void me7ReadService(); // run a queued read to completion (call from the K-line task)
