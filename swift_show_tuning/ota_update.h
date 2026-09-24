#pragma once
// =========================================================================================
// FIRMWARE UPDATE (OTA) - browser upload, GitHub online update, version info API
// =========================================================================================
// HTTP routes (port 80, registered by otaBegin):
//   GET  /update              minimal no-JS upload page
//   POST /update[?md5=<hex>]  multipart upload (field "update"), 200 -> reboot
//   GET  /api/info            firmware + update settings (never the Wi-Fi password)
//   GET  /api/ota/status      online-update state machine
//   POST /api/ota/config      ssid, pass, repo, branch, autoCheck (form-urlencoded)
//   POST /api/ota/check       connect to Wi-Fi, read version.json
//   POST /api/ota/install     connect, read version.json, download, flash, reboot
//   POST /api/ota/cancel      abort (unless already finalising the flash)
// See docs/PROTOCOL.md for the exact JSON shapes.
// =========================================================================================
#include <WebServer.h>

// Registers all update / info HTTP routes on the dashboard web server (port 80).
// Call once in setup(), after the Wi-Fi AP is up.
void otaBegin(WebServer& server);

// Call every loop() iteration (starts the optional boot-time update check).
// The online update itself runs in its own FreeRTOS task, so loop() keeps serving.
void otaLoop();

// true while firmware is being downloaded / written. The engine is inhibited meanwhile.
bool otaIsBusy();
