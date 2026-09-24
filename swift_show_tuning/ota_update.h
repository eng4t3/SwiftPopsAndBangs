#pragma once
// =========================================================================================
// FIRMWARE UPDATE (OTA) - browser upload, GitHub online update, version info API
// =========================================================================================
#include <WebServer.h>

// Registers all update / info HTTP routes on the dashboard web server (port 80).
void otaBegin(WebServer& server);

// Call every loop() iteration (drives the non-blocking online-update state machine).
void otaLoop();

// true while firmware is being downloaded / written. The engine is inhibited meanwhile.
bool otaIsBusy();
