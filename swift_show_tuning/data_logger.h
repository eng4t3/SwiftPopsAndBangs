#pragma once
// =========================================================================================
// DATA LOGGER (v2.1) - 25 Hz samples, 120 s RAM ring, flash drive log + event captures
// =========================================================================================
// Contract: docs/PROTOCOL.md, section "Data logger" (routes, CSV columns, capture types,
// write policy). The binary flash format is documented at the top of data_logger.cpp and
// decoded / validated by tools/log/swiftlog.py (keep both in sync).
//
// The logger only READS engine state. Whatever goes wrong in here (no partition, no heap,
// flash errors) degrades the logger - RAM-only or no captures - never the engine.
// =========================================================================================
#include <WebServer.h>

// Call once in setup(), after otaBegin(): finds the `spiffs` data partition and scans the log
// (~0.1-0.3 s), allocates the RAM ring (120 s, reduced only if the free heap would otherwise
// drop below 90 KB), starts the sampler task (core 1, priority 2) and registers the
// /api/log/* routes on the dashboard web server.
void loggerBegin(WebServer& server);

// Call every loop() iteration: pumps the CSV / JSON downloads with non-blocking socket writes.
void loggerLoop();
