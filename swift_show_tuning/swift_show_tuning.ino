/*
 * =========================================================================================
 *   SUZUKI SWIFT 1.3 8V (2000 - G13BA) - SHOW TUNING CONTROLLER
 *   ESP32 Firmware: Wi-Fi Pops & Bangs, 2-Step Launch Control & Rev Limiter
 * =========================================================================================
 *   Source layout:
 *     swift_show_tuning.ino  - glue: Wi-Fi AP, web server, WebSocket protocol, NVS settings
 *     engine_control.*       - tach sensing, RPM, spark-cut patterns, launch control (real-time)
 *     ota_update.*           - firmware updates (browser upload, GitHub online update)
 *     web_ui.h               - embedded mobile dashboard (HTML/CSS/JS)
 *     version.h              - firmware version (compared against version.json on GitHub)
 *
 *   100% SELF-CONTAINED: ZERO external library dependencies!
 *   Uses built-in ESP32 core libraries: WiFi, WebServer, Preferences, Update, mbedtls.
 * =========================================================================================
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <mbedtls/sha1.h>
#include <mbedtls/base64.h>

#include "version.h"
#include "engine_control.h"
#include "ota_update.h"
#include "web_ui.h"

// Tuning parameters (loaded from / saved to flash; the engine keeps its own validated copy)
TuningConfig config = engineDefaultConfig();

unsigned long lastTelemetryMillis = 0;

// Wi-Fi Access Point Configuration
const char* AP_SSID = "Swift-PopsAndBangs";
const char* AP_PASS = "swift123";  // Change as desired (minimum 8 characters)
IPAddress apIP(192, 168, 4, 1);
IPAddress netMsk(255, 255, 255, 0);

WebServer server(80);
WiFiServer wsServer(81);  // WebSocket server on port 81
WiFiClient wsClient;
bool wsConnected = false;

Preferences prefs;

void saveConfigToNVS();

// =========================================================================================
// LIGHTWEIGHT BUILT-IN WEBSOCKET SERVER (RFC 6455 - Zero External Library Dependencies)
// =========================================================================================
void handleWebSocketHandshake(WiFiClient& client, String req) {
  int keyIndex = req.indexOf("Sec-WebSocket-Key");
  if (keyIndex == -1) return;
  int colonIndex = req.indexOf(":", keyIndex);
  if (colonIndex == -1) return;
  int keyEnd = req.indexOf("\r\n", colonIndex);
  if (keyEnd == -1) keyEnd = req.indexOf("\n", colonIndex);
  if (keyEnd == -1) return;
  String key = req.substring(colonIndex + 1, keyEnd);
  key.trim();

  // RFC6455 Magic GUID
  String acceptKey = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

  // SHA1 Hash
  unsigned char sha1Result[20];
  mbedtls_sha1_context ctx;
  mbedtls_sha1_init(&ctx);
  mbedtls_sha1_starts(&ctx);
  mbedtls_sha1_update(&ctx, (const unsigned char*)acceptKey.c_str(), acceptKey.length());
  mbedtls_sha1_finish(&ctx, sha1Result);
  mbedtls_sha1_free(&ctx);

  // Base64 Encode
  unsigned char base64Result[32];
  size_t base64Len = 0;
  mbedtls_base64_encode(base64Result, sizeof(base64Result), &base64Len, sha1Result, 20);
  base64Result[base64Len] = '\0';

  String response = "HTTP/1.1 101 Switching Protocols\r\n"
                    "Upgrade: websocket\r\n"
                    "Connection: Upgrade\r\n"
                    "Sec-WebSocket-Accept: " + String((char*)base64Result) + "\r\n\r\n";
  client.print(response);
  client.flush();
  wsConnected = true;
}

void sendWsTextMessage(const String& msg) {
  if (!wsConnected || !wsClient || !wsClient.connected()) return;

  size_t len = msg.length();
  uint8_t header[10];
  size_t headerLen = 0;

  header[0] = 0x81; // FIN + Text frame
  if (len <= 125) {
    header[1] = len;
    headerLen = 2;
  } else if (len <= 65535) {
    header[1] = 126;
    header[2] = (len >> 8) & 0xFF;
    header[3] = len & 0xFF;
    headerLen = 4;
  }
  wsClient.write(header, headerLen);
  wsClient.print(msg);
}

void processIncomingWsFrame(uint8_t* buf, size_t len) {
  if (len < 6) return;
  uint8_t opcode = buf[0] & 0x0F;
  if (opcode == 0x08) { // Connection Close
    wsConnected = false;
    wsClient.stop();
    return;
  }
  if (opcode != 0x01) return; // Only process text frames

  bool isMasked = (buf[1] & 0x80) != 0;
  uint64_t payloadLen = buf[1] & 0x7F;
  size_t maskOffset = 2;

  if (payloadLen == 126) {
    payloadLen = ((uint64_t)buf[2] << 8) | buf[3];
    maskOffset = 4;
  }

  uint8_t maskKey[4] = {0,0,0,0};
  if (isMasked) {
    for (int i = 0; i < 4; i++) maskKey[i] = buf[maskOffset + i];
    maskOffset += 4;
  }

  String text = "";
  for (size_t i = 0; i < payloadLen && (maskOffset + i) < len; i++) {
    char c = buf[maskOffset + i] ^ maskKey[i % 4];
    text += c;
  }

  // Handle Commands from Web Dashboard
  if (text == "BTN:1") {
    engineSetShowButton(true);
  } else if (text == "BTN:0") {
    engineSetShowButton(false);
  } else if (text == "LAUNCH:ARM") {
    engineLaunchArm();
  } else if (text == "LAUNCH:DISARM") {
    engineLaunchDisarm();
  } else if (text == "GET_CONFIG") {
    String cfgJson = "CFG:{\"armed\":" + String(config.armed ? "true" : "false") +
                     ",\"launchRpm\":" + String(config.launchRpm) +
                     ",\"redlineRpm\":" + String(config.redlineRpm) +
                     ",\"decelRpm\":" + String(config.decelRpm) +
                     ",\"decelPops\":" + String(config.decelPops ? "true" : "false") +
                     ",\"cutPattern\":" + String(config.cutPattern) +
                     ",\"maxCutSeconds\":" + String(config.maxCutSeconds, 1) +
                     ",\"ghostCam\":" + String(config.ghostCam ? "true" : "false") +
                     ",\"launchDrop\":" + String(config.launchDrop) +
                     ",\"fw\":\"" FW_VERSION "\"}";
    sendWsTextMessage(cfgJson);
  } else if (text.startsWith("SET_CFG:")) {
    String json = text.substring(8);
    // Parse lightweight key-values
    if (json.indexOf("\"armed\":true") != -1) config.armed = true;
    if (json.indexOf("\"armed\":false") != -1) config.armed = false;
    if (json.indexOf("\"decelPops\":true") != -1) config.decelPops = true;
    if (json.indexOf("\"decelPops\":false") != -1) config.decelPops = false;
    if (json.indexOf("\"ghostCam\":true") != -1) config.ghostCam = true;
    if (json.indexOf("\"ghostCam\":false") != -1) config.ghostCam = false;

    int idxLaunch = json.indexOf("\"launchRpm\":");
    if (idxLaunch != -1) config.launchRpm = json.substring(idxLaunch + 12).toInt();

    int idxRed = json.indexOf("\"redlineRpm\":");
    if (idxRed != -1) config.redlineRpm = json.substring(idxRed + 13).toInt();

    int idxDecel = json.indexOf("\"decelRpm\":");
    if (idxDecel != -1) config.decelRpm = json.substring(idxDecel + 11).toInt();

    int idxPat = json.indexOf("\"cutPattern\":");
    if (idxPat != -1) config.cutPattern = json.substring(idxPat + 13).toInt();

    int idxTimeout = json.indexOf("\"maxCutSeconds\":");
    if (idxTimeout != -1) {
      float tVal = json.substring(idxTimeout + 16).toFloat();
      if (tVal >= 6.0f) tVal = 0.0f;
      config.maxCutSeconds = tVal;
    }

    int idxDrop = json.indexOf("\"launchDrop\":");
    if (idxDrop != -1) config.launchDrop = json.substring(idxDrop + 13).toInt();

    engineClampConfig(config);
    engineSetConfig(config);
  } else if (text == "SAVE_FLASH") {
    saveConfigToNVS();
    sendWsTextMessage("ACK:SAVED");
  }
}

// =========================================================================================
// NON-VOLATILE STORAGE (NVS PREFERENCES)
// =========================================================================================
void loadConfigFromNVS() {
  TuningConfig d = engineDefaultConfig();
  prefs.begin("tuning", false); // Initialize namespace (creates if not exists)
  config.armed         = prefs.getBool("armed", d.armed);
  config.launchRpm     = prefs.getInt("launchRpm", d.launchRpm);
  config.redlineRpm    = prefs.getInt("redlineRpm", d.redlineRpm);
  config.decelPops     = prefs.getBool("decelPops", d.decelPops);
  config.decelRpm      = prefs.getInt("decelRpm", d.decelRpm);
  config.cutPattern    = prefs.getInt("cutPattern", d.cutPattern);
  config.maxCutSeconds = prefs.getFloat("maxCutSec", d.maxCutSeconds);
  config.ghostCam      = prefs.getBool("ghostCam", d.ghostCam);
  config.launchDrop    = prefs.getInt("launchDrop", d.launchDrop);
  prefs.end();
  engineClampConfig(config);
}

void saveConfigToNVS() {
  prefs.begin("tuning", false); // Read-write mode
  prefs.putBool("armed", config.armed);
  prefs.putInt("launchRpm", config.launchRpm);
  prefs.putInt("redlineRpm", config.redlineRpm);
  prefs.putBool("decelPops", config.decelPops);
  prefs.putInt("decelRpm", config.decelRpm);
  prefs.putInt("cutPattern", config.cutPattern);
  prefs.putFloat("maxCutSec", config.maxCutSeconds);
  prefs.putBool("ghostCam", config.ghostCam);
  prefs.putInt("launchDrop", config.launchDrop);
  prefs.end();
}

// =========================================================================================
// SETUP
// =========================================================================================
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== Suzuki Swift 1.3 8V Show Tuning v" FW_VERSION " Initializing ===");

  // Load persistent tuning configuration from Flash, then start the engine core
  loadConfigFromNVS();
  engineBegin(config);

  // Start Standalone Wi-Fi SoftAP
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(apIP, apIP, netMsk);
  WiFi.softAP(AP_SSID, AP_PASS);

  Serial.print("Wi-Fi AP Started: ");
  Serial.println(AP_SSID);
  Serial.print("Web Dashboard IP: http://");
  Serial.println(WiFi.softAPIP());

  // Setup Web Server to serve embedded HTML reliably in 1460-byte TCP MSS chunks
  server.on("/", HTTP_GET, []() {
    server.setContentLength(sizeof(INDEX_HTML) - 1);
    server.send(200, "text/html", "");
    WiFiClient client = server.client();
    const char* ptr = INDEX_HTML;
    size_t remaining = sizeof(INDEX_HTML) - 1;
    while (remaining > 0 && client.connected()) {
      size_t toSend = (remaining > 1460) ? 1460 : remaining;
      size_t written = client.write((const uint8_t*)ptr, toSend);
      if (written > 0) {
        ptr += written;
        remaining -= written;
      }
      delay(1);
    }
  });

  // Firmware update + device info routes
  otaBegin(server);

  server.onNotFound([]() {
    // Return empty 204 No Content for /favicon.ico and other browser prefetch probes
    server.send(204, "text/plain", "");
  });

  server.begin();

  // Setup WebSocket Server on port 81
  wsServer.begin();
  wsServer.setNoDelay(true);

  Serial.println("System Ready. Fail-safe active.");
}

// =========================================================================================
// MAIN LOOP
// =========================================================================================
void loop() {
  unsigned long currentMillis = millis();

  // 1. Engine control step (RPM, launch state machine, spark cut)
  engineSetInhibit(otaIsBusy());
  engineUpdate();

  // 2. Firmware update state machine
  otaLoop();

  // 3. Handle Web Server Requests
  server.handleClient();

  // 4. Handle WebSocket Client Connections & Frames
  if (wsServer.hasClient()) {
    WiFiClient newClient = wsServer.available();
    if (wsClient && wsClient != newClient) {
      wsClient.stop();
    }
    wsClient = newClient;
    wsConnected = false;
  }

  if (wsClient && wsClient.connected()) {
    if (!wsConnected && wsClient.available()) {
      // Read until double CRLF with timeout so all headers arrive
      String req = "";
      unsigned long waitStart = millis();
      while (wsClient.connected() && (millis() - waitStart < 300)) {
        while (wsClient.available()) {
          char c = (char)wsClient.read();
          req += c;
          waitStart = millis();
        }
        if (req.indexOf("\r\n\r\n") != -1) break;
        delay(2);
      }
      if (req.indexOf("Sec-WebSocket-Key") != -1) {
        handleWebSocketHandshake(wsClient, req);
      }
    } else if (wsConnected && wsClient.available()) {
      uint8_t buffer[256];
      size_t bytesRead = wsClient.read(buffer, sizeof(buffer));
      if (bytesRead > 0) {
        processIncomingWsFrame(buffer, bytesRead);
      }
    }
  } else {
    wsConnected = false;
  }

  // 5. Broadcast Telemetry to Mobile App at ~30 FPS (every 33ms)
  //    Format: T:<rpm>,<cut 0|1>,<launchState 0-3>,<launchLeftMs>,<cutReason 0-8>
  if (wsConnected && (currentMillis - lastTelemetryMillis >= 33)) {
    lastTelemetryMillis = currentMillis;
    EngineTelemetry t = engineGetTelemetry();
    String telemetry = "T:" + String(t.rpm) + "," + String(t.cutActive ? "1" : "0") + "," +
                       String(t.launchState) + "," + String(t.launchLeftMs) + "," + String(t.reason);
    sendWsTextMessage(telemetry);
  }
}
