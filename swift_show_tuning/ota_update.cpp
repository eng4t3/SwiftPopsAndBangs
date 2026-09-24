// =========================================================================================
// FIRMWARE UPDATE (OTA) - see ota_update.h
// =========================================================================================
#include "ota_update.h"
#include <Update.h>
#include "engine_control.h"
#include "version.h"

static WebServer* srv = nullptr;
static bool busy = false;

bool otaIsBusy() { return busy; }

void otaLoop() {}

void otaBegin(WebServer& server) {
  srv = &server;

  // Fallback direct browser upload form at /update
  server.on("/update", HTTP_GET, []() {
    srv->send(200, "text/html",
      "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<title>Swift OTA Frissítés</title>"
      "<style>body{background:#0a0e17;color:#00f0ff;font-family:sans-serif;padding:24px;text-align:center;}"
      "input,button{padding:12px;margin:10px;border-radius:8px;border:none;font-size:16px;}"
      "input[type=file]{background:#162032;color:#fff;}"
      "input[type=submit]{background:#00ff88;color:#000;font-weight:bold;cursor:pointer;}"
      "</style></head><body>"
      "<h2>📡 Swift Show Tuning - Firmware OTA</h2>"
      "<p>Válaszd ki a lefordított .bin fájlt a frissítéshez:</p>"
      "<form method='POST' action='/update' enctype='multipart/form-data'>"
      "<input type='file' name='update' accept='.bin'><br>"
      "<input type='submit' value='Feltöltés és Telepítés'>"
      "</form></body></html>");
  });

  // OTA Binary Upload and Flashing Handler
  server.on("/update", HTTP_POST, []() {
    srv->sendHeader("Connection", "close");
    if (Update.hasError()) {
      busy = false;
      engineSetInhibit(false);
      srv->send(500, "text/plain", "OTA HIBA: " + String(Update.errorString()));
    } else {
      srv->send(200, "text/plain", "SIKERES FRISSITES! A vezerlo ujraindul...");
      delay(500);
      ESP.restart();
    }
  }, []() {
    HTTPUpload& upload = srv->upload();
    if (upload.status == UPLOAD_FILE_START) {
      // Safety: Disable spark cut so ignition is NEVER clamped during firmware flashing!
      busy = true;
      engineSetInhibit(true);
      digitalWrite(PIN_STATUS_LED, HIGH); // Solid status LED indicates flashing in progress
      Serial.printf("[OTA] Frissítés megkezdése: %s\n", upload.filename.c_str());
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_END) {
      if (Update.end(true)) { // true = set total size to current progress
        Serial.printf("[OTA] Sikeres feltöltés! Méret: %u bájt. Újraindítás...\n", upload.totalSize);
      } else {
        Update.printError(Serial);
      }
      digitalWrite(PIN_STATUS_LED, LOW);
    } else if (upload.status == UPLOAD_FILE_ABORTED) {
      Update.end();
      busy = false;
      engineSetInhibit(false);
      Serial.println("[OTA] Frissítés megszakítva!");
      digitalWrite(PIN_STATUS_LED, LOW);
    }
  });

  // Firmware / device info for the dashboard
  server.on("/api/info", HTTP_GET, []() {
    String j = "{\"fw\":\"" FW_VERSION "\",\"code\":" + String(FW_VERSION_CODE) +
               ",\"heap\":" + String(ESP.getFreeHeap()) + "}";
    srv->send(200, "application/json", j);
  });
}
