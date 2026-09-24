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
 *   Uses built-in ESP32 core libraries: WiFi, WebServer, Preferences, Update, mbedtls, lwIP.
 *
 *   Glue design rules (see docs/PROTOCOL.md for the wire contract):
 *     - loop() must never block on the network: every socket write here is MSG_DONTWAIT.
 *       WiFiClient::write() is NOT used for WebSocket / page traffic because it retries a
 *       1 s select() up to 10 times and can stall loop() for ~10 s on a stale phone.
 *     - WebSocket frames are parsed from a persistent per-client buffer (several frames per
 *       TCP read, frames split across reads, fragmentation, ping/pong, close).
 *     - The show button is released whenever the client holding it goes away or stops
 *       refreshing BTN:1 (600 ms dead-man, same as the engine's own dead-man).
 * =========================================================================================
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <mbedtls/sha1.h>
#include <mbedtls/base64.h>
#include <lwip/sockets.h>
#include <errno.h>
#include <math.h>
#include <ctype.h>
#include <string.h>
#include <strings.h>

#include "version.h"
#include "engine_control.h"
#include "ota_update.h"
#include "web_ui.h"

// Tuning parameters (loaded from / saved to flash). Kept equal to engineGetConfig() after
// every change; the engine keeps its own validated copy.
TuningConfig config = engineDefaultConfig();

// Wi-Fi Access Point Configuration
const char* AP_SSID = "Swift-PopsAndBangs";
const char* AP_PASS = "swift123";  // Change as desired (minimum 8 characters)
IPAddress apIP(192, 168, 4, 1);
IPAddress netMsk(255, 255, 255, 0);

WebServer server(80);
WiFiServer wsServer(81);  // WebSocket server on port 81

Preferences prefs;

// ---- Timing ------------------------------------------------------------------------------
static const uint32_t TELEMETRY_PERIOD_MS     = 33;    // ~30 Hz
static const uint32_t WS_HANDSHAKE_TIMEOUT_MS = 3000;  // complete HTTP upgrade request must arrive by then
static const uint32_t WS_PING_INTERVAL_MS     = 2000;  // server ping; browsers answer with pong automatically
static const uint32_t WS_RX_TIMEOUT_MS        = 7000;  // nothing (not even a pong) for this long = dead client
static const uint32_t WS_TX_STALL_MS          = 3000;  // socket accepted no byte of our backlog for this long = dead
static const uint32_t WS_CLOSE_LINGER_MS      = 500;   // time to flush our close frame before dropping TCP
static const uint32_t BTN_DEADMAN_MS          = 600;   // BTN:1 must be refreshed within this time (PROTOCOL.md)
static const uint32_t SAVE_MAX_DEFER_MS       = 1000;  // NVS write waits for a moment without spark cut
static const uint32_t PAGE_STALL_MS           = 5000;  // HTTP page transfer without progress is aborted
static const uint32_t LOOP_GAP_GRACE_MS       = 1000;  // loop() was blocked (OTA upload...): refresh timeouts

// ---- Sizing ------------------------------------------------------------------------------
#define WS_SLOTS     3     // connections incl. one spare for a handshake in progress
#define WS_MAX_OPEN  2     // simultaneously open dashboards; a newer one replaces the least active
#define WS_RX_CAP    1536  // handshake headers, then raw frames
#define WS_TX_CAP    1024  // pending outgoing bytes per client
#define WS_MAX_MSG   512   // largest accepted message payload (SET_CFG is ~200 bytes)
#define PAGE_SLOTS   2     // concurrent dashboard page downloads

static_assert(WS_MAX_MSG + 14 <= WS_RX_CAP, "a whole frame must fit into the RX buffer");

// =========================================================================================
// TYPES (declared before any function: the .ino converter puts prototypes above the first one)
// =========================================================================================
struct JsonIn {
  const char* p;
  const char* e;
};

enum JsonKind : uint8_t { JV_NONE, JV_BOOL, JV_NUM };

enum WsState : uint8_t { WS_FREE = 0, WS_HANDSHAKE, WS_OPEN, WS_CLOSING };

struct WsConn {
  WiFiClient client;
  WsState  state = WS_FREE;
  uint32_t openedMs = 0;      // accept time (handshake timeout)
  uint32_t lastRxMs = 0;      // last byte received
  uint32_t lastPingMs = 0;
  uint32_t txProgressMs = 0;  // last time the TX backlog shrank (or became non-empty)
  uint32_t closeMs = 0;
  uint32_t btnMs = 0;         // last BTN:1
  size_t   rxLen = 0;
  size_t   txLen = 0;
  size_t   fragLen = 0;
  uint8_t  fragOpcode = 0;    // 0 = no fragmented message in progress
  bool     btnHeld = false;
  bool     saveAck = false;   // ACK:SAVED owed to this client
  uint8_t  rx[WS_RX_CAP];
  uint8_t  tx[WS_TX_CAP];
  uint8_t  frag[WS_MAX_MSG];
};

struct PageStream {
  WiFiClient client;
  bool active = false;
  char hdr[256];
  size_t hdrLen = 0, hdrPos = 0;
  const uint8_t* body = nullptr;
  size_t bodyLen = 0, bodyPos = 0;
  uint32_t lastProgressMs = 0;
};

static char g_etag[40];                 // "<FW_VERSION>-<content hash>" of INDEX_HTML
static const size_t INDEX_HTML_LEN = sizeof(INDEX_HTML) - 1;

// =========================================================================================
// NON-VOLATILE STORAGE (NVS PREFERENCES)
// =========================================================================================
// Schema history:
//   0 (no "schema" key) - firmware 1.x: same keys, no launchDrop
//   1                   - firmware 2.x: adds launchDrop
// Bump NVS_SCHEMA_VERSION whenever a key changes meaning or a default must be re-applied to
// existing installs, and migrate in loadConfigFromNVS().
#define NVS_NAMESPACE      "tuning"
#define NVS_SCHEMA_KEY     "schema"
#define NVS_SCHEMA_VERSION 1

void loadConfigFromNVS() {
  const TuningConfig d = engineDefaultConfig();
  TuningConfig c = d;
  if (prefs.begin(NVS_NAMESPACE, true)) {  // read-only; fails (-> defaults) on first boot
    uint8_t schema  = prefs.getUChar(NVS_SCHEMA_KEY, 0);
    c.armed         = prefs.getBool("armed", d.armed);
    c.launchRpm     = prefs.getInt("launchRpm", d.launchRpm);
    c.redlineRpm    = prefs.getInt("redlineRpm", d.redlineRpm);
    c.decelPops     = prefs.getBool("decelPops", d.decelPops);
    c.decelRpm      = prefs.getInt("decelRpm", d.decelRpm);
    c.cutPattern    = prefs.getInt("cutPattern", d.cutPattern);
    c.maxCutSeconds = prefs.getFloat("maxCutSec", d.maxCutSeconds);
    c.ghostCam      = prefs.getBool("ghostCam", d.ghostCam);
    c.launchDrop    = prefs.getInt("launchDrop", d.launchDrop);
    prefs.end();
    if (schema > NVS_SCHEMA_VERSION) {
      Serial.printf("[NVS] settings from newer firmware (schema %u), using known keys\n", schema);
    }
    // Migrations go here, e.g. if (schema < 2) { c.someKey = d.someKey; }
  }
  // A damaged anti-flood value must fall back to the default, never to "unlimited"
  // (engineClampConfig maps NaN/negative to 0 = unlimited). Same for an invalid pattern
  // (clamping would turn it into the most aggressive one).
  if (!isfinite(c.maxCutSeconds) || c.maxCutSeconds < 0.0f) c.maxCutSeconds = d.maxCutSeconds;
  if (c.cutPattern < 0 || c.cutPattern > 4) c.cutPattern = d.cutPattern;
  engineClampConfig(c);
  config = c;
}

// Write only keys whose stored value differs (saves flash wear on repeated SAVE_FLASH).
static bool nvsPutInt(const char* key, int32_t v) {
  if (prefs.getType(key) == PT_I32 && prefs.getInt(key, ~v) == v) return true;
  return prefs.putInt(key, v) != 0;
}
static bool nvsPutBool(const char* key, bool v) {
  if (prefs.getType(key) == PT_U8 && prefs.getUChar(key, 0xFF) == (v ? 1 : 0)) return true;
  return prefs.putBool(key, v) != 0;
}
static bool nvsPutUChar(const char* key, uint8_t v) {
  if (prefs.getType(key) == PT_U8 && prefs.getUChar(key, (uint8_t)~v) == v) return true;
  return prefs.putUChar(key, v) != 0;
}
static bool nvsPutFloat(const char* key, float v) {
  float old = NAN;
  if (prefs.getType(key) == PT_BLOB && prefs.getBytesLength(key) == sizeof(float)) {
    old = prefs.getFloat(key, NAN);
    if (memcmp(&old, &v, sizeof(float)) == 0) return true;
  }
  return prefs.putFloat(key, v) != 0;
}

bool saveConfigToNVS(const TuningConfig& c) {
  if (!prefs.begin(NVS_NAMESPACE, false)) return false;
  bool ok = true;
  ok &= nvsPutBool("armed", c.armed);
  ok &= nvsPutInt("launchRpm", c.launchRpm);
  ok &= nvsPutInt("redlineRpm", c.redlineRpm);
  ok &= nvsPutBool("decelPops", c.decelPops);
  ok &= nvsPutInt("decelRpm", c.decelRpm);
  ok &= nvsPutInt("cutPattern", c.cutPattern);
  ok &= nvsPutFloat("maxCutSec", c.maxCutSeconds);
  ok &= nvsPutBool("ghostCam", c.ghostCam);
  ok &= nvsPutInt("launchDrop", c.launchDrop);
  ok &= nvsPutUChar(NVS_SCHEMA_KEY, NVS_SCHEMA_VERSION);
  prefs.end();
  return ok;
}

// =========================================================================================
// SET_CFG PARSER - minimal JSON object reader (exact keys, typed values, whitespace tolerant)
// =========================================================================================
static void jWs(JsonIn& j) {
  while (j.p < j.e && (*j.p == ' ' || *j.p == '\t' || *j.p == '\r' || *j.p == '\n')) j.p++;
}

static bool jLit(JsonIn& j, const char* lit) {
  size_t n = strlen(lit);
  if ((size_t)(j.e - j.p) < n || memcmp(j.p, lit, n) != 0) return false;
  j.p += n;
  return true;
}

// Reads a JSON string at j.p. With out != nullptr the (ASCII) content is copied; a string
// that does not fit yields an empty result so it can never match a known key.
static bool jString(JsonIn& j, char* out, size_t cap) {
  if (j.p >= j.e || *j.p != '"') return false;
  j.p++;
  size_t o = 0;
  bool overflow = false;
  while (j.p < j.e) {
    char ch = *j.p++;
    if (ch == '"') {
      if (out && cap) out[overflow ? 0 : o] = '\0';
      return true;
    }
    if ((unsigned char)ch < 0x20) return false;
    if (ch == '\\') {
      if (j.p >= j.e) return false;
      char esc = *j.p++;
      switch (esc) {
        case '"': case '\\': case '/': ch = esc; break;
        case 'b': ch = '\b'; break;
        case 'f': ch = '\f'; break;
        case 'n': ch = '\n'; break;
        case 'r': ch = '\r'; break;
        case 't': ch = '\t'; break;
        case 'u':
          for (int i = 0; i < 4; i++) {
            if (j.p >= j.e || !isxdigit((unsigned char)*j.p)) return false;
            j.p++;
          }
          ch = '?';  // keys are plain ASCII; non-ASCII never matches
          break;
        default: return false;
      }
    }
    if (out) {
      if (o + 1 < cap) out[o++] = ch;
      else overflow = true;
    }
  }
  return false;  // unterminated
}

static bool jNumber(JsonIn& j, double& v) {
  const char* s = j.p;
  if (s < j.e && *s == '-') s++;
  if (s >= j.e || !isdigit((unsigned char)*s)) return false;
  while (s < j.e && (isdigit((unsigned char)*s) || *s == '.' || *s == 'e' || *s == 'E' || *s == '+' || *s == '-')) s++;
  size_t n = (size_t)(s - j.p);
  char buf[32];
  if (n >= sizeof(buf)) return false;
  memcpy(buf, j.p, n);
  buf[n] = '\0';
  char* end = nullptr;
  v = strtod(buf, &end);
  if (end != buf + n || !isfinite(v)) return false;
  j.p = s;
  return true;
}

// Skips any JSON value (used for unknown keys and for known keys with a wrong type).
static bool jSkip(JsonIn& j, int depth) {
  jWs(j);
  if (j.p >= j.e) return false;
  const char open = *j.p;
  if (open == '"') return jString(j, nullptr, 0);
  if (open == '{' || open == '[') {
    if (depth > 8) return false;
    const char close = (open == '{') ? '}' : ']';
    j.p++;
    jWs(j);
    if (j.p < j.e && *j.p == close) { j.p++; return true; }
    for (;;) {
      if (open == '{') {
        jWs(j);
        if (!jString(j, nullptr, 0)) return false;
        jWs(j);
        if (j.p >= j.e || *j.p != ':') return false;
        j.p++;
      }
      if (!jSkip(j, depth + 1)) return false;
      jWs(j);
      if (j.p >= j.e) return false;
      if (*j.p == ',') { j.p++; continue; }
      if (*j.p == close) { j.p++; return true; }
      return false;
    }
  }
  if (jLit(j, "true") || jLit(j, "false") || jLit(j, "null")) return true;
  double d;
  return jNumber(j, d);
}

// Reads one value; bool/number are returned, anything else is skipped (kind = JV_NONE).
static bool jValue(JsonIn& j, JsonKind& kind, bool& b, double& num) {
  jWs(j);
  kind = JV_NONE;
  if (jLit(j, "true"))  { kind = JV_BOOL; b = true;  return true; }
  if (jLit(j, "false")) { kind = JV_BOOL; b = false; return true; }
  if (j.p < j.e && (*j.p == '-' || isdigit((unsigned char)*j.p))) {
    if (!jNumber(j, num)) return false;
    kind = JV_NUM;
    return true;
  }
  return jSkip(j, 0);
}

static bool numToInt(double v, int& out) {
  if (!(v >= -1000000.0 && v <= 1000000.0)) return false;
  out = (int)lround(v);
  return true;
}

// Applies the present keys of a SET_CFG JSON object. The whole message is rejected on a
// syntax error; a known key with a wrong type / invalid value is ignored on its own.
static bool applyConfigJson(const char* s, size_t n) {
  TuningConfig c = config;
  JsonIn j = {s, s + n};
  jWs(j);
  if (j.p >= j.e || *j.p != '{') return false;
  j.p++;
  jWs(j);
  if (j.p < j.e && *j.p == '}') {
    j.p++;
  } else {
    for (;;) {
      char key[20];
      jWs(j);
      if (!jString(j, key, sizeof(key))) return false;
      jWs(j);
      if (j.p >= j.e || *j.p != ':') return false;
      j.p++;

      JsonKind kind;
      bool b = false;
      double v = 0;
      if (!jValue(j, kind, b, v)) return false;

      int iv;
      if (!strcmp(key, "armed"))          { if (kind == JV_BOOL) c.armed = b; }
      else if (!strcmp(key, "decelPops")) { if (kind == JV_BOOL) c.decelPops = b; }
      else if (!strcmp(key, "ghostCam"))  { if (kind == JV_BOOL) c.ghostCam = b; }
      else if (!strcmp(key, "launchRpm"))  { if (kind == JV_NUM && numToInt(v, iv)) c.launchRpm = iv; }
      else if (!strcmp(key, "redlineRpm")) { if (kind == JV_NUM && numToInt(v, iv)) c.redlineRpm = iv; }
      else if (!strcmp(key, "decelRpm"))   { if (kind == JV_NUM && numToInt(v, iv)) c.decelRpm = iv; }
      else if (!strcmp(key, "launchDrop")) { if (kind == JV_NUM && numToInt(v, iv)) c.launchDrop = iv; }
      else if (!strcmp(key, "cutPattern")) {
        // Only an exact pattern index; clamping 7 -> 4 would silently select "cannon".
        if (kind == JV_NUM && v == floor(v) && v >= 0 && v <= 4) c.cutPattern = (int)v;
      }
      else if (!strcmp(key, "maxCutSeconds")) {
        // 0 or >= 6.0 (slider end stop) = unlimited; negative is invalid and ignored.
        if (kind == JV_NUM && v >= 0.0) c.maxCutSeconds = (v == 0.0 || v >= 6.0) ? 0.0f : (float)v;
      }
      // unknown keys: value already skipped

      jWs(j);
      if (j.p >= j.e) return false;
      if (*j.p == ',') { j.p++; continue; }
      if (*j.p == '}') { j.p++; break; }
      return false;
    }
  }
  jWs(j);
  if (j.p != j.e) return false;  // trailing garbage

  config = c;
  engineClampConfig(config);
  engineSetConfig(config);
  config = engineGetConfig();
  return true;
}

// =========================================================================================
// LIGHTWEIGHT BUILT-IN WEBSOCKET SERVER (RFC 6455 - Zero External Library Dependencies)
// =========================================================================================
static WsConn g_ws[WS_SLOTS];
static char g_msg[WS_MAX_MSG + 1];
static volatile bool g_apStationLeft = false;
static uint32_t g_lastWsServiceMs = 0;
static bool g_savePending = false;
static uint32_t g_saveRequestMs = 0;

static const char WS_GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

static bool isTransientSendError(int e) {
  return e == EAGAIN || e == EWOULDBLOCK || e == ENOMEM || e == EINTR;
}

// ---- Show button (aggregated over clients) ---------------------------------------------
static bool wsAnyButtonHeld(uint32_t now) {
  for (int i = 0; i < WS_SLOTS; i++) {
    const WsConn& c = g_ws[i];
    if (c.state == WS_OPEN && c.btnHeld && (now - c.btnMs) <= BTN_DEADMAN_MS) return true;
  }
  return false;
}

static void wsReleaseButtonIfIdle() {
  if (!wsAnyButtonHeld(millis())) engineSetShowButton(false);
}

// ---- Connection teardown --------------------------------------------------------------
static void wsReset(WsConn& c) {
  c.client.stop();
  c.state = WS_FREE;
  c.rxLen = c.txLen = c.fragLen = 0;
  c.fragOpcode = 0;
  c.btnHeld = false;
  c.saveAck = false;
}

static void wsDrop(WsConn& c) {
  bool wasDashboard = (c.state == WS_OPEN || c.state == WS_CLOSING);
  wsReset(c);
  if (wasDashboard) wsReleaseButtonIfIdle();  // disconnect / replacement releases the button
}

// ---- Non-blocking TX ------------------------------------------------------------------
static bool wsFlush(WsConn& c, uint32_t now) {
  while (c.txLen) {
    int fd = c.client.fd();
    if (fd < 0) { wsDrop(c); return false; }
    ssize_t n = lwip_send(fd, c.tx, c.txLen, MSG_DONTWAIT);
    if (n > 0) {
      c.txLen -= (size_t)n;
      if (c.txLen) memmove(c.tx, c.tx + n, c.txLen);
      c.txProgressMs = now;
      continue;
    }
    if (n == 0 || isTransientSendError(errno)) return true;  // socket buffer full: try next loop
    wsDrop(c);
    return false;
  }
  return true;
}

// Queues header + payload as one unit. Droppable data (telemetry, pings) is skipped when
// anything is still pending, so a slow phone only ever costs us a skipped frame.
static bool wsQueue(WsConn& c, const uint8_t* a, size_t an, const uint8_t* b, size_t bn,
                    bool droppable, uint32_t now) {
  if (c.state == WS_FREE) return false;
  if (droppable && c.txLen) return false;
  if (c.txLen + an + bn > WS_TX_CAP) {
    if (droppable) return false;
    wsDrop(c);  // hopelessly backed up: the phone is gone
    return false;
  }
  if (!c.txLen) c.txProgressMs = now;
  if (an) memcpy(c.tx + c.txLen, a, an);
  c.txLen += an;
  if (bn) memcpy(c.tx + c.txLen, b, bn);
  c.txLen += bn;
  return wsFlush(c, now);
}

static bool wsSendFrame(WsConn& c, uint8_t opcode, const void* payload, size_t len,
                        bool droppable, uint32_t now) {
  uint8_t hdr[4];
  size_t hl;
  hdr[0] = 0x80 | (opcode & 0x0F);  // FIN + opcode, server frames are never masked
  if (len <= 125) {
    hdr[1] = (uint8_t)len;
    hl = 2;
  } else {
    hdr[1] = 126;
    hdr[2] = (uint8_t)(len >> 8);
    hdr[3] = (uint8_t)len;
    hl = 4;
  }
  return wsQueue(c, hdr, hl, (const uint8_t*)payload, len, droppable, now);
}

static void wsSendText(WsConn& c, const char* s, size_t n, uint32_t now) {
  wsSendFrame(c, 0x1, s, n, false, now);
}

// Sends a close frame, releases the client's button and lingers briefly to flush it.
static void wsStartClose(WsConn& c, uint16_t code, uint32_t now) {
  if (c.state != WS_OPEN) { wsDrop(c); return; }
  uint8_t p[2] = {(uint8_t)(code >> 8), (uint8_t)code};
  if (!wsSendFrame(c, 0x8, p, 2, false, now)) return;  // dropped (button already released)
  c.state = WS_CLOSING;
  c.closeMs = now;
  c.rxLen = 0;
  c.fragLen = 0;
  c.fragOpcode = 0;
  c.btnHeld = false;
  c.saveAck = false;
  wsReleaseButtonIfIdle();
}

// ---- Dashboard commands ---------------------------------------------------------------
static bool msgIs(const char* s, size_t n, const char* lit) {
  size_t ln = strlen(lit);
  return n == ln && memcmp(s, lit, ln) == 0;
}

static void wsSendConfig(WsConn& c, uint32_t now) {
  config = engineGetConfig();
  const TuningConfig& k = config;
  char buf[256];
  int n = snprintf(buf, sizeof(buf),
                   "CFG:{\"armed\":%s,\"launchRpm\":%d,\"redlineRpm\":%d,\"decelRpm\":%d,"
                   "\"decelPops\":%s,\"cutPattern\":%d,\"maxCutSeconds\":%.2f,\"ghostCam\":%s,"
                   "\"launchDrop\":%d,\"fw\":\"%s\"}",
                   k.armed ? "true" : "false", k.launchRpm, k.redlineRpm, k.decelRpm,
                   k.decelPops ? "true" : "false", k.cutPattern,
                   isfinite(k.maxCutSeconds) ? (double)k.maxCutSeconds : 0.0,
                   k.ghostCam ? "true" : "false", k.launchDrop, FW_VERSION);
  if (n > 0 && n < (int)sizeof(buf)) wsSendText(c, buf, (size_t)n, now);
}

static void wsHandleText(WsConn& c, const uint8_t* data, size_t len, uint32_t now) {
  if (len > WS_MAX_MSG) return;
  memcpy(g_msg, data, len);
  g_msg[len] = '\0';
  char* s = g_msg;
  size_t n = len;
  while (n && isspace((unsigned char)*s)) { s++; n--; }
  while (n && isspace((unsigned char)s[n - 1])) s[--n] = '\0';

  if (msgIs(s, n, "BTN:1")) {
    c.btnHeld = true;
    c.btnMs = now;
    engineSetShowButton(true);  // every refresh feeds the engine's dead-man
  } else if (msgIs(s, n, "BTN:0")) {
    c.btnHeld = false;
    wsReleaseButtonIfIdle();
  } else if (msgIs(s, n, "LAUNCH:ARM")) {
    engineLaunchArm();
  } else if (msgIs(s, n, "LAUNCH:DISARM")) {
    engineLaunchDisarm();
  } else if (msgIs(s, n, "GET_CONFIG")) {
    wsSendConfig(c, now);
  } else if (n >= 8 && memcmp(s, "SET_CFG:", 8) == 0) {
    if (!applyConfigJson(s + 8, n - 8)) Serial.println("[WS] SET_CFG ignored: malformed JSON");
  } else if (msgIs(s, n, "SAVE_FLASH")) {
    c.saveAck = true;
    if (!g_savePending) {
      g_savePending = true;
      g_saveRequestMs = now;
    }
  }
  // anything else: ignored
}

// ---- Frame parser ---------------------------------------------------------------------
static void wsOnFrame(WsConn& c, bool fin, uint8_t op, uint8_t* pay, size_t len, uint32_t now) {
  if (op & 0x08) {  // control frame
    if (!fin || len > 125) { wsStartClose(c, 1002, now); return; }
    if (op == 0x8) {  // close -> echo the status code, then close TCP
      if (len == 1) { wsStartClose(c, 1002, now); return; }
      uint16_t code = (len >= 2) ? (uint16_t)((pay[0] << 8) | pay[1]) : 1000;
      wsStartClose(c, code, now);
    } else if (op == 0x9) {  // ping -> pong with the same payload
      wsSendFrame(c, 0xA, pay, len, false, now);
    } else if (op == 0xA) {
      // pong: lastRxMs was already refreshed by the read
    } else {
      wsStartClose(c, 1002, now);  // reserved control opcode
    }
    return;
  }

  if (op == 0x1 || op == 0x2) {  // new text / binary message
    if (c.fragOpcode) { wsStartClose(c, 1002, now); return; }
    if (fin) {
      if (op == 0x1) wsHandleText(c, pay, len, now);
      return;
    }
    c.fragOpcode = op;
    c.fragLen = 0;
  } else if (op == 0x0) {  // continuation
    if (!c.fragOpcode) { wsStartClose(c, 1002, now); return; }
  } else {
    wsStartClose(c, 1002, now);  // reserved data opcode
    return;
  }

  if (c.fragLen + len > WS_MAX_MSG) { wsStartClose(c, 1009, now); return; }
  memcpy(c.frag + c.fragLen, pay, len);
  c.fragLen += len;
  if (fin) {
    uint8_t whole = c.fragOpcode;
    size_t wholeLen = c.fragLen;
    c.fragOpcode = 0;
    c.fragLen = 0;
    if (whole == 0x1) wsHandleText(c, c.frag, wholeLen, now);
  }
}

static void wsParse(WsConn& c, uint32_t now) {
  size_t off = 0;
  while (c.state == WS_OPEN) {
    size_t avail = c.rxLen - off;
    if (avail < 2) break;
    uint8_t* p = c.rx + off;
    const bool fin = (p[0] & 0x80) != 0;
    const uint8_t rsv = p[0] & 0x70;
    const uint8_t op = p[0] & 0x0F;
    const bool masked = (p[1] & 0x80) != 0;
    size_t plen = p[1] & 0x7F;
    size_t hlen = 2;
    if (rsv) { wsStartClose(c, 1002, now); break; }     // no extensions negotiated
    if (!masked) { wsStartClose(c, 1002, now); break; }  // client frames must be masked
    if (plen == 127) { wsStartClose(c, 1009, now); break; }  // 64-bit length: never needed here
    if (plen == 126) {
      if (avail < 4) break;
      plen = ((size_t)p[2] << 8) | p[3];
      hlen = 4;
    }
    if (plen > WS_MAX_MSG) { wsStartClose(c, 1009, now); break; }
    hlen += 4;  // masking key
    if (avail < hlen + plen) break;  // wait for the rest of the frame
    const uint8_t* mask = p + hlen - 4;
    uint8_t* pay = p + hlen;
    for (size_t i = 0; i < plen; i++) pay[i] ^= mask[i & 3];
    off += hlen + plen;
    wsOnFrame(c, fin, op, pay, plen, now);
  }
  if (c.state != WS_OPEN) return;  // closing / dropped: buffer already discarded
  if (off) {
    c.rxLen -= off;
    if (c.rxLen) memmove(c.rx, c.rx + off, c.rxLen);
  }
}

// ---- Handshake (non-blocking: headers accumulate across loop iterations) ---------------
static void trimSpan(const char*& s, size_t& n) {
  while (n && (*s == ' ' || *s == '\t')) { s++; n--; }
  while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) n--;
}

static bool ciEquals(const char* a, size_t an, const char* b) {
  size_t bn = strlen(b);
  return an == bn && strncasecmp(a, b, an) == 0;
}

static bool tokenListHas(const char* v, size_t n, const char* tok) {
  size_t i = 0;
  while (i <= n) {
    size_t j = i;
    while (j < n && v[j] != ',') j++;
    const char* t = v + i;
    size_t tn = j - i;
    trimSpan(t, tn);
    if (ciEquals(t, tn, tok)) return true;
    i = j + 1;
  }
  return false;
}

// Host part of "host[:port]" (Host header) - stops at ':' or '/'.
static void hostPart(const char* s, size_t n, const char*& h, size_t& hn) {
  h = s;
  hn = 0;
  while (hn < n && s[hn] != ':' && s[hn] != '/') hn++;
}

static bool isIPv4Literal(const char* h, size_t n) {
  int dots = 0, digits = 0;
  for (size_t i = 0; i < n; i++) {
    if (h[i] == '.') {
      if (!digits) return false;
      dots++;
      digits = 0;
    } else if (isdigit((unsigned char)h[i])) {
      if (++digits > 3) return false;
    } else {
      return false;
    }
  }
  return dots == 3 && digits > 0;
}

// Cross-site protection: a web page on the phone (with mobile data on) must not be able to
// open ws://192.168.4.1:81 and cut the ignition. Browsers always send Origin; the dashboard's
// origin host equals the Host it connects to. Only IP literals / .local names are accepted,
// which also defeats DNS rebinding. No Origin = not a browser (tools) -> allowed.
static bool originAllowed(const char* origin, size_t on, const char* host, size_t hn) {
  if (!origin) return true;
  static const char kHttp[] = "http://";
  if (on < sizeof(kHttp) - 1 || strncasecmp(origin, kHttp, sizeof(kHttp) - 1) != 0) return false;
  const char* oh;
  size_t ohn;
  hostPart(origin + sizeof(kHttp) - 1, on - (sizeof(kHttp) - 1), oh, ohn);
  if (!host) return false;
  const char* hh;
  size_t hhn;
  hostPart(host, hn, hh, hhn);
  if (ohn == 0 || ohn != hhn || strncasecmp(oh, hh, ohn) != 0) return false;
  bool local = (ohn > 6 && strncasecmp(oh + ohn - 6, ".local", 6) == 0);
  return isIPv4Literal(oh, ohn) || local;
}

static void wsQueueRaw(WsConn& c, const char* s, size_t n, uint32_t now) {
  wsQueue(c, (const uint8_t*)s, n, nullptr, 0, false, now);
}

static void wsRejectHandshake(WsConn& c, const char* status, const char* extraHeaders, uint32_t now) {
  char resp[200];
  int n = snprintf(resp, sizeof(resp),
                   "HTTP/1.1 %s\r\nConnection: close\r\nContent-Length: 0\r\n%s\r\n",
                   status, extraHeaders);
  c.rxLen = 0;
  if (n > 0 && n < (int)sizeof(resp)) wsQueueRaw(c, resp, (size_t)n, now);
  if (c.state == WS_FREE) return;
  c.state = WS_CLOSING;  // linger to flush the response, then drop
  c.closeMs = now;
}

static void wsEnforceMaxOpen(const WsConn& keep) {
  int open = 0, oldest = -1;
  for (int i = 0; i < WS_SLOTS; i++) {
    const WsConn& o = g_ws[i];
    if (&o == &keep || o.state != WS_OPEN) continue;
    open++;
    if (oldest < 0 || (int32_t)(o.lastRxMs - g_ws[oldest].lastRxMs) < 0) oldest = i;
  }
  if (open >= WS_MAX_OPEN && oldest >= 0) {
    Serial.println("[WS] replacing least active dashboard");
    wsDrop(g_ws[oldest]);
  }
}

static void wsTryHandshake(WsConn& c, uint32_t now) {
  const char* req = (const char*)c.rx;
  size_t end = 0;
  for (size_t i = 3; i < c.rxLen; i++) {
    if (req[i - 3] == '\r' && req[i - 2] == '\n' && req[i - 1] == '\r' && req[i] == '\n') {
      end = i + 1;
      break;
    }
  }
  if (!end) {
    if (c.rxLen >= WS_RX_CAP) wsRejectHandshake(c, "431 Request Header Fields Too Large", "", now);
    return;  // wait for more (the handshake timeout is checked by the service loop)
  }

  const char* lineEnd = (const char*)memchr(req, '\r', end);
  size_t rl = (size_t)(lineEnd - req);
  bool requestOk = rl >= 14 && memcmp(req, "GET ", 4) == 0;
  if (requestOk) {
    const char* ver = req + rl - 8;  // "HTTP/1.x"
    requestOk = memcmp(ver, "HTTP/1.", 7) == 0 && ver[7] >= '1' && ver[7] <= '9';
  }

  bool upgradeOk = false, connectionOk = false, versionOk = false;
  const char* key = nullptr;
  size_t keyLen = 0;
  const char* origin = nullptr;
  size_t originLen = 0;
  const char* host = nullptr;
  size_t hostLen = 0;

  size_t pos = rl + 2;
  while (pos + 2 <= end) {
    const char* line = req + pos;
    const char* cr = (const char*)memchr(line, '\r', end - pos);
    if (!cr) break;
    size_t ln = (size_t)(cr - line);
    pos += ln + 2;
    if (ln == 0) break;  // blank line
    const char* colon = (const char*)memchr(line, ':', ln);
    if (!colon) continue;
    const char* name = line;
    size_t nn = (size_t)(colon - line);
    trimSpan(name, nn);
    const char* val = colon + 1;
    size_t vn = (size_t)((line + ln) - val);
    trimSpan(val, vn);
    if (ciEquals(name, nn, "Upgrade")) upgradeOk = tokenListHas(val, vn, "websocket");
    else if (ciEquals(name, nn, "Connection")) connectionOk = tokenListHas(val, vn, "upgrade");
    else if (ciEquals(name, nn, "Sec-WebSocket-Version")) versionOk = (vn == 2 && memcmp(val, "13", 2) == 0);
    else if (ciEquals(name, nn, "Sec-WebSocket-Key")) { key = val; keyLen = vn; }
    else if (ciEquals(name, nn, "Origin")) { origin = val; originLen = vn; }
    else if (ciEquals(name, nn, "Host")) { host = val; hostLen = vn; }
  }

  // The key must be base64 of exactly 16 bytes (24 characters).
  bool keyOk = false;
  if (key && keyLen == 24) {
    unsigned char raw[20];
    size_t rawLen = 0;
    keyOk = mbedtls_base64_decode(raw, sizeof(raw), &rawLen, (const unsigned char*)key, keyLen) == 0 &&
            rawLen == 16;
  }

  if (!requestOk || !upgradeOk || !connectionOk || !keyOk) {
    wsRejectHandshake(c, "400 Bad Request", "", now);
    return;
  }
  if (!versionOk) {
    wsRejectHandshake(c, "426 Upgrade Required", "Sec-WebSocket-Version: 13\r\n", now);
    return;
  }
  if (!originAllowed(origin, originLen, host, hostLen)) {
    Serial.println("[WS] handshake refused: foreign Origin");
    wsRejectHandshake(c, "403 Forbidden", "", now);
    return;
  }

  // Sec-WebSocket-Accept = base64(SHA1(key + GUID))
  char cat[24 + sizeof(WS_GUID)];
  memcpy(cat, key, 24);
  memcpy(cat + 24, WS_GUID, sizeof(WS_GUID) - 1);
  unsigned char sha[20];
  mbedtls_sha1_ret((const unsigned char*)cat, 24 + sizeof(WS_GUID) - 1, sha);
  unsigned char acc[32];
  size_t accLen = 0;
  if (mbedtls_base64_encode(acc, sizeof(acc) - 1, &accLen, sha, sizeof(sha)) != 0) {
    wsRejectHandshake(c, "500 Internal Server Error", "", now);
    return;
  }
  acc[accLen] = '\0';

  char resp[160];
  int n = snprintf(resp, sizeof(resp),
                   "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                   "Sec-WebSocket-Accept: %s\r\n\r\n",
                   (const char*)acc);

  // Anything after the headers already belongs to the frame stream.
  c.rxLen -= end;
  if (c.rxLen) memmove(c.rx, c.rx + end, c.rxLen);

  wsQueueRaw(c, resp, (size_t)n, now);
  if (c.state == WS_FREE) return;
  wsEnforceMaxOpen(c);
  c.state = WS_OPEN;
  c.lastRxMs = now;
  c.lastPingMs = now;
  c.btnHeld = false;
  c.fragOpcode = 0;
  c.fragLen = 0;
  Serial.println("[WS] dashboard connected");
}

// ---- Accept / service -----------------------------------------------------------------
static void wsAccept(uint32_t now) {
  for (int guard = 0; guard < WS_SLOTS && wsServer.hasClient(); guard++) {
    WiFiClient nc = wsServer.available();
    if (!nc) break;
    int idx = -1;
    for (int i = 0; i < WS_SLOTS && idx < 0; i++) if (g_ws[i].state == WS_FREE) idx = i;
    for (int i = 0; i < WS_SLOTS && idx < 0; i++) if (g_ws[i].state == WS_CLOSING) idx = i;
    for (int i = 0; i < WS_SLOTS; i++) {  // oldest pending handshake
      if (g_ws[i].state != WS_HANDSHAKE) continue;
      if (idx >= 0 && g_ws[idx].state != WS_HANDSHAKE) continue;
      if (idx < 0 || (int32_t)(g_ws[i].openedMs - g_ws[idx].openedMs) < 0) idx = i;
    }
    if (idx < 0) {  // cannot happen with WS_SLOTS > WS_MAX_OPEN, but never block
      nc.stop();
      continue;
    }
    WsConn& c = g_ws[idx];
    if (c.state != WS_FREE) wsDrop(c);
    c.client = nc;
    c.state = WS_HANDSHAKE;
    c.openedMs = now;
    c.lastRxMs = now;
    c.txProgressMs = now;
    c.rxLen = c.txLen = c.fragLen = 0;
    c.fragOpcode = 0;
    c.btnHeld = false;
    c.saveAck = false;
  }
}

static void wsServiceSlot(WsConn& c, uint32_t now) {
  if (c.state == WS_FREE) return;
  if (!c.client.connected()) { wsDrop(c); return; }

  // RX: drain what the socket has (bounded), parse after every read
  for (int iter = 0; iter < 4 && c.state != WS_FREE; iter++) {
    int avail = c.client.available();
    if (avail <= 0) break;
    if (c.state == WS_CLOSING) {  // discard
      uint8_t junk[64];
      if (c.client.read(junk, sizeof(junk)) <= 0) break;
      continue;
    }
    size_t room = WS_RX_CAP - c.rxLen;
    if (!room) break;
    int n = c.client.read(c.rx + c.rxLen, room);
    if (n <= 0) break;
    c.rxLen += (size_t)n;
    c.lastRxMs = now;
    if (c.state == WS_HANDSHAKE) wsTryHandshake(c, now);
    if (c.state == WS_OPEN) wsParse(c, now);
  }
  if (c.state == WS_FREE) return;

  if (c.state == WS_HANDSHAKE && (now - c.openedMs) > WS_HANDSHAKE_TIMEOUT_MS) { wsDrop(c); return; }

  if (!wsFlush(c, now)) return;
  if (c.txLen && (now - c.txProgressMs) > WS_TX_STALL_MS) {
    Serial.println("[WS] client stalled, dropping");
    wsDrop(c);
    return;
  }

  if (c.state == WS_CLOSING) {
    if (!c.txLen || (now - c.closeMs) > WS_CLOSE_LINGER_MS) wsDrop(c);
    return;
  }

  if (c.state == WS_OPEN) {
    if ((now - c.lastRxMs) > WS_RX_TIMEOUT_MS) {
      Serial.println("[WS] client silent (no pong), dropping");
      wsDrop(c);
      return;
    }
    if ((now - c.lastPingMs) >= WS_PING_INTERVAL_MS) {
      c.lastPingMs = now;
      wsSendFrame(c, 0x9, nullptr, 0, true, now);
      if (c.state == WS_FREE) return;
    }
    if (c.btnHeld && (now - c.btnMs) > BTN_DEADMAN_MS) {  // BTN:1 refresh stopped
      c.btnHeld = false;
      wsReleaseButtonIfIdle();
    }
  }
}

static void wsDropAll() {
  for (int i = 0; i < WS_SLOTS; i++) if (g_ws[i].state != WS_FREE) wsDrop(g_ws[i]);
}

static void onWiFiEvent(arduino_event_id_t event) {
  if (event == ARDUINO_EVENT_WIFI_AP_STADISCONNECTED) g_apStationLeft = true;  // event task: flag only
}

static void wsService(uint32_t now) {
  // loop() was blocked for a long time (e.g. OTA upload): don't mistake that for dead clients
  if ((now - g_lastWsServiceMs) > LOOP_GAP_GRACE_MS) {
    for (int i = 0; i < WS_SLOTS; i++) {
      WsConn& c = g_ws[i];
      c.lastRxMs = now;
      c.openedMs = now;
      c.txProgressMs = now;
      c.closeMs = now;
    }
  }
  g_lastWsServiceMs = now;

  if (g_apStationLeft) {
    g_apStationLeft = false;
    if (WiFi.softAPgetStationNum() == 0) wsDropAll();  // last phone left the AP
  }

  wsAccept(now);
  for (int i = 0; i < WS_SLOTS; i++) wsServiceSlot(g_ws[i], now);
}

// ---- Telemetry ------------------------------------------------------------------------
static uint32_t g_lastTelemetryMs = 0;

static void wsTelemetry(uint32_t now) {
  if ((now - g_lastTelemetryMs) < TELEMETRY_PERIOD_MS) return;
  g_lastTelemetryMs += TELEMETRY_PERIOD_MS;
  if ((now - g_lastTelemetryMs) >= TELEMETRY_PERIOD_MS) g_lastTelemetryMs = now;  // no catch-up bursts

  bool any = false;
  for (int i = 0; i < WS_SLOTS; i++) any |= (g_ws[i].state == WS_OPEN);
  if (!any) return;

  // Format: T:<rpm>,<cut 0|1>,<launchState 0-3>,<launchLeftMs>,<cutReason 0-8>
  EngineTelemetry t = engineGetTelemetry();
  char buf[64];
  int n = snprintf(buf, sizeof(buf), "T:%d,%u,%u,%lu,%u", t.rpm, t.cutActive ? 1u : 0u,
                   (unsigned)t.launchState, (unsigned long)t.launchLeftMs, (unsigned)t.reason);
  if (n <= 0 || n >= (int)sizeof(buf)) return;
  for (int i = 0; i < WS_SLOTS; i++) {
    if (g_ws[i].state == WS_OPEN) wsSendFrame(g_ws[i], 0x1, buf, (size_t)n, true, now);
  }
}

// ---- Deferred flash save --------------------------------------------------------------
// An NVS write stalls the CPU (flash cache off) for a few ms up to a sector erase. Do it while
// the engine is not cutting so a stall can never freeze the spark-cut output in the ON state.
static void serviceSave(uint32_t now) {
  if (!g_savePending) return;
  if (engineGetTelemetry().cutActive && (now - g_saveRequestMs) < SAVE_MAX_DEFER_MS) return;
  g_savePending = false;
  config = engineGetConfig();
  bool ok = saveConfigToNVS(config);
  if (!ok) Serial.println("[NVS] save FAILED");
  uint32_t t = millis();
  for (int i = 0; i < WS_SLOTS; i++) {
    WsConn& c = g_ws[i];
    if (c.state != WS_OPEN || !c.saveAck) continue;
    c.saveAck = false;
    if (ok) wsSendText(c, "ACK:SAVED", 9, t);
  }
}

// =========================================================================================
// HTTP "/" - dashboard page, streamed without blocking loop()
// =========================================================================================
static PageStream g_pages[PAGE_SLOTS];

static void pageClose(PageStream& p) {
  p.client.stop();  // lwIP still delivers everything already queued, then FIN
  p.active = false;
}

static void pagePump(PageStream& p, uint32_t now) {
  if (!p.active) return;
  int fd = p.client.fd();
  if (fd < 0) { pageClose(p); return; }
  for (int i = 0; i < 4; i++) {
    const uint8_t* src;
    size_t rem;
    if (p.hdrPos < p.hdrLen) {
      src = (const uint8_t*)p.hdr + p.hdrPos;
      rem = p.hdrLen - p.hdrPos;
    } else if (p.bodyPos < p.bodyLen) {
      src = p.body + p.bodyPos;
      rem = p.bodyLen - p.bodyPos;
    } else {
      pageClose(p);  // all bytes handed to TCP
      return;
    }
    if (rem > 4096) rem = 4096;
    ssize_t n = lwip_send(fd, src, rem, MSG_DONTWAIT);
    if (n > 0) {
      if (p.hdrPos < p.hdrLen) p.hdrPos += (size_t)n;
      else p.bodyPos += (size_t)n;
      p.lastProgressMs = now;
      continue;
    }
    if (n < 0 && !isTransientSendError(errno)) { pageClose(p); return; }
    break;  // socket buffer full
  }
  if ((now - p.lastProgressMs) > PAGE_STALL_MS) pageClose(p);
}

static void computeEtag() {
  uint32_t h = 2166136261u;  // FNV-1a over the page: a changed page always gets a new tag
  const uint8_t* p = (const uint8_t*)INDEX_HTML;
  for (size_t i = 0; i < INDEX_HTML_LEN; i++) {
    h ^= p[i];
    h *= 16777619u;
  }
  snprintf(g_etag, sizeof(g_etag), "\"%s-%08lx\"", FW_VERSION, (unsigned long)h);
}

static void handleRoot() {
  bool notModified = false;
  if (server.hasHeader("If-None-Match")) {
    String inm = server.header("If-None-Match");
    notModified = (inm.indexOf(g_etag) >= 0) || inm == "*";
  }
  WiFiClient c = server.client();
  if (!c) return;

  int idx = -1;
  for (int i = 0; i < PAGE_SLOTS && idx < 0; i++) if (!g_pages[i].active) idx = i;
  if (idx < 0) {  // all busy: abort the one that has been slowest to make progress
    idx = 0;
    for (int i = 1; i < PAGE_SLOTS; i++) {
      if ((int32_t)(g_pages[i].lastProgressMs - g_pages[idx].lastProgressMs) < 0) idx = i;
    }
    pageClose(g_pages[idx]);
  }
  PageStream& p = g_pages[idx];
  int n;
  if (notModified) {
    n = snprintf(p.hdr, sizeof(p.hdr),
                 "HTTP/1.1 304 Not Modified\r\nCache-Control: no-cache\r\nETag: %s\r\n"
                 "Connection: close\r\n\r\n",
                 g_etag);
    p.body = nullptr;
    p.bodyLen = 0;
  } else {
    n = snprintf(p.hdr, sizeof(p.hdr),
                 "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %u\r\n"
                 "Cache-Control: no-cache\r\nETag: %s\r\nConnection: close\r\n\r\n",
                 (unsigned)INDEX_HTML_LEN, g_etag);
    p.body = (const uint8_t*)INDEX_HTML;
    p.bodyLen = INDEX_HTML_LEN;
  }
  if (n <= 0 || n >= (int)sizeof(p.hdr)) return;
  p.client = c;
  p.hdrLen = (size_t)n;
  p.hdrPos = 0;
  p.bodyPos = 0;
  p.active = true;
  p.lastProgressMs = millis();
  pagePump(p, p.lastProgressMs);  // send what fits right away; loop() pumps the rest
}

// =========================================================================================
// SETUP
// =========================================================================================
void setup() {
  Serial.begin(115200);

  // Engine first: drives the spark-cut output to its safe (LOW) state as early as possible.
  loadConfigFromNVS();
  engineBegin(config);
  config = engineGetConfig();

  Serial.println("\n=== Suzuki Swift 1.3 8V Show Tuning v" FW_VERSION " Initializing ===");
  computeEtag();

  // Start Standalone Wi-Fi SoftAP
  WiFi.onEvent(onWiFiEvent, ARDUINO_EVENT_WIFI_AP_STADISCONNECTED);
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(apIP, apIP, netMsk);
  if (!WiFi.softAP(AP_SSID, AP_PASS)) Serial.println("Wi-Fi AP start FAILED");

  Serial.print("Wi-Fi AP Started: ");
  Serial.println(AP_SSID);
  Serial.print("Web Dashboard IP: http://");
  Serial.println(WiFi.softAPIP());

  // The only collectHeaders() call (last call wins): "If-None-Match" for the ETag check on "/",
  // "Origin" for the cross-site POST check in ota_update.cpp.
  static const char* kHeaderKeys[] = {"If-None-Match", "Origin"};
  server.collectHeaders(kHeaderKeys, 2);

  server.on("/", HTTP_GET, handleRoot);

  // Firmware update + device info routes
  otaBegin(server);

  server.onNotFound([]() {
    // Return empty 204 No Content for /favicon.ico and other browser prefetch probes
    server.send(204, "text/plain", "");
  });

  server.begin();

  // Setup WebSocket Server on port 81 (setNoDelay must follow begin(), which resets it)
  wsServer.begin();
  wsServer.setNoDelay(true);

  uint32_t now = millis();
  g_lastWsServiceMs = now;
  g_lastTelemetryMs = now;
  Serial.println("System Ready. Fail-safe active.");
}

// =========================================================================================
// MAIN LOOP
// =========================================================================================
void loop() {
  // 1. Engine control step (RPM, launch state machine, spark cut)
  engineSetInhibit(otaIsBusy());
  engineUpdate();

  // 2. Firmware update state machine
  otaLoop();

  // 3. HTTP requests (page transfers continue non-blocking below)
  server.handleClient();

  uint32_t now = millis();
  for (int i = 0; i < PAGE_SLOTS; i++) pagePump(g_pages[i], now);

  // 4. WebSocket connections, frames, keepalive
  wsService(now);

  // 5. Telemetry to the dashboards at ~30 Hz (frames are skipped, never queued, on a slow link)
  wsTelemetry(now);

  // 6. Pending SAVE_FLASH
  serviceSave(now);
}
