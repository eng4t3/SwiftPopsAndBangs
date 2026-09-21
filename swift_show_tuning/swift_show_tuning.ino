/*
 * =========================================================================================
 *   SUZUKI SWIFT 1.3 8V (2000 - G13BA) - SHOW TUNING CONTROLLER
 *   ESP32 Firmware: Wi-Fi Pops & Bangs, 2-Step Launch Control & Rev Limiter
 * =========================================================================================
 *   Features:
 *     - Optical isolated Tachometer RPM sensing (2 pulses per revolution)
 *     - Fail-safe low-voltage ignition trigger (IGt) clamp pull-down
 *     - Self-contained Wi-Fi Access Point ("Swift-ShowTuning") with embedded Mobile Web App
 *     - Real-time WebSockets telemetry (30 FPS analog & digital tachometer)
 *     - Instant virtual 2-Step touch button (sub-5ms latency)
 *     - Configurable limits: Launch RPM, Redline RPM, Overrun Decel Cut, Safety Timeout
 *     - Cut patterns: "Hard Cut" (Bee*R style), "Flame Spitter", "Gunfire Crackle"
 *     - Anti-flood safety cutouts & minimum RPM interlock (never stalls below 2000 RPM)
 *     - Settings persist in ESP32 Flash (Preferences / NVS)
 *     - 100% SELF-CONTAINED: ZERO external library dependencies!
 *       Uses built-in ESP32 core libraries: WiFi, WebServer, Preferences, mbedtls.
 * =========================================================================================
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <Update.h>
#include <mbedtls/sha1.h>
#include <mbedtls/base64.h>

// =========================================================================================
// PIN DEFINITIONS & HARDWARE CONSTANTS
// =========================================================================================
#define PIN_TACH_IN       18  // Tachometer pulse input from PC817 optocoupler (Interrupt)
#define PIN_SPARK_CUT     19  // Ignition clamp trigger to PC817 + 2N2222 transistor (Output)
#define PIN_STATUS_LED     2  // Onboard blue LED indicator
#define PIN_LAUNCH_SW     23  // Physical Clutch switch / Handbrake / Steering button input (Active LOW with internal PULLUP)

// Engine specs: 4-cylinder, 4-stroke G13BA = 2 ignition pulses per engine crankshaft revolution
#define PULSES_PER_REV     2  

// Safety limits
#define MIN_CUT_RPM     2000  // Engine stall prevention: Never cut spark below this RPM
#define HARD_MAX_RPM    8000  // Failsafe upper ceiling

// =========================================================================================
// TUNING PARAMETERS (Loaded from / Saved to Flash Preferences)
// =========================================================================================
struct TuningConfig {
  bool  armed;              // Master system armed toggle
  int   launchRpm;          // 2-Step stationary launch limit (default 3800 RPM)
  int   redlineRpm;         // Main redline rev limiter (default 6200 RPM)
  bool  decelPops;          // Overrun decel pop mode enabled
  int   decelRpm;           // Minimum RPM for decel pops to trigger (default 3200 RPM)
  int   cutPattern;         // 0 = Hard Cut, 1 = Flame Spitter, 2 = Gunfire Crackle, 3 = AK-47
  float maxCutSeconds;      // Anti-flood safety limit (default 3.0s, 0.0 = unlimited)
  bool  ghostCam;           // Ghost Cam / Lumpy V8 idle simulation
};

TuningConfig config = {
  .armed         = true,
  .launchRpm     = 3800,
  .redlineRpm    = 6200,
  .decelPops     = true,
  .decelRpm      = 3200,
  .cutPattern    = 1,      // Pattern 1 (Flame Spitter) - optimal for overrun flames & pops
  .maxCutSeconds = 3.0f,
  .ghostCam      = false
};

// Runtime dynamic state
volatile unsigned long lastPulseMicros = 0;
volatile unsigned long pulseIntervalSum = 0;
volatile unsigned int  pulseIntervalCount = 0;
volatile unsigned long lastValidInterval = 0;
volatile unsigned long pulseHistory[4] = {35000, 35000, 35000, 35000};
volatile uint8_t       pulseHistIdx = 0;

int  currentRpm = 0;
bool virtualTwoStepActive = false;
volatile bool isSparkCutActive = false;
unsigned long cutEngagedTimestamp = 0;
unsigned long lastRpmCalcMillis = 0;
unsigned long lastTelemetryMillis = 0;
unsigned int  cutCycleCounter = 0;

// Hands-Free Smart Launch State Machine
enum LaunchState {
  LAUNCH_OFF = 0,      // Normal driving
  LAUNCH_ARMED = 1,    // Ready for throttle (10s arming window)
  LAUNCH_HOLDING = 2,  // On limiter at launchRpm (cutting spark)
  LAUNCH_FIRED = 3     // Clutch drop detected, launched!
};
LaunchState launchState = LAUNCH_OFF;
unsigned long launchArmTimestamp = 0;
unsigned long launchHoldStart = 0;
int peakLaunchRpm = 0;

// Automatic Overrun Decel Tracking
int  previousRpm = 0;
int  rpmDelta = 0;
unsigned long decelBurstStart = 0;
bool decelBurstActive = false;
int  decelDropFrames = 0;            // Consecutive 40ms frames of real decel
unsigned long lastRpmRiseMillis = 0; // Timestamp of when engine was last actively accelerating

// Ghost Cam / V8 Lumpy Idle State
volatile unsigned int ghostCamPulseCounter = 0;
unsigned long ghostCutStartMicros = 0;
bool ghostCutInProgress = false;
unsigned int ghostCycleId = 0;

// Pattern 4: Fireball Cannon State
unsigned long cannonCutStart = 0;
bool cannonInCutPhase = false;

// Wi-Fi Access Point Configuration
const char* AP_SSID = "Swift-ShowTuning";
const char* AP_PASS = "swift138v";  // Change as desired (minimum 8 characters)
IPAddress apIP(192, 168, 4, 1);
IPAddress netMsk(255, 255, 255, 0);

WebServer server(80);
WiFiServer wsServer(81);  // WebSocket server on port 81
WiFiClient wsClient;
bool wsConnected = false;

Preferences prefs;

// =========================================================================================
// EMBEDDED MOBILE WEB DASHBOARD (HTML5, CSS3, SVG Tachometer, Vanilla JS)
// =========================================================================================
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="hu">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0, user-scalable=no, maximum-scale=1.0">
  <title>Suzuki Swift 1.3 8V - Show Tuning Vezérlő</title>
  <link rel="preconnect" href="https://fonts.googleapis.com">
  <link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
  <link href="https://fonts.googleapis.com/css2?family=Orbitron:wght@600;800;900&family=Rajdhani:wght@600;700;800&display=swap" rel="stylesheet">
  <style>
    :root {
      --bg-gradient: radial-gradient(circle at 50% 10%, #151d2c 0%, #07090e 75%);
      --panel-bg: rgba(16, 22, 34, 0.85);
      --panel-border: rgba(255, 255, 255, 0.08);
      --neon-cyan: #00f0ff;
      --neon-amber: #ffaa00;
      --neon-red: #ff2247;
      --neon-red-glow: rgba(255, 34, 71, 0.5);
      --neon-green: #00ff88;
      --text-main: #f0f4fc;
      --text-muted: #7e8da6;
    }

    * {
      box-sizing: border-box;
      margin: 0;
      padding: 0;
      user-select: none;
      -webkit-user-select: none;
      font-family: 'Rajdhani', -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
    }

    body {
      background: #07090e;
      background-image: var(--bg-gradient);
      color: var(--text-main);
      display: flex;
      flex-direction: column;
      align-items: center;
      min-height: 100vh;
      padding: 12px 14px 28px;
      overflow-x: hidden;
    }

    /* Cockpit Top Bar */
    .header-bar {
      width: 100%;
      max-width: 440px;
      display: flex;
      justify-content: space-between;
      align-items: center;
      padding: 10px 14px;
      background: var(--panel-bg);
      backdrop-filter: blur(16px);
      -webkit-backdrop-filter: blur(16px);
      border: 1px solid var(--panel-border);
      border-radius: 16px;
      margin-bottom: 12px;
      box-shadow: 0 4px 20px rgba(0,0,0,0.4);
    }

    .brand-wrap {
      display: flex;
      flex-direction: column;
    }

    .brand-title {
      font-family: 'Orbitron', sans-serif;
      font-size: 0.95rem;
      font-weight: 900;
      letter-spacing: 1.5px;
      color: #fff;
      display: flex;
      align-items: center;
      gap: 6px;
    }

    .brand-badge {
      background: linear-gradient(135deg, #ff2247 0%, #aa0720 100%);
      color: #fff;
      font-size: 0.62rem;
      padding: 2px 7px;
      border-radius: 4px;
      font-weight: 800;
      letter-spacing: 0.5px;
    }

    .brand-sub {
      font-size: 0.7rem;
      color: var(--text-muted);
      font-weight: 700;
      letter-spacing: 1.5px;
    }

    .header-actions {
      display: flex;
      align-items: center;
      gap: 8px;
    }

    .info-btn {
      width: 32px;
      height: 32px;
      border-radius: 50%;
      background: rgba(0, 240, 255, 0.12);
      border: 1px solid rgba(0, 240, 255, 0.45);
      color: var(--neon-cyan);
      font-family: 'Orbitron', sans-serif;
      font-size: 0.95rem;
      font-weight: 900;
      cursor: pointer;
      display: flex;
      justify-content: center;
      align-items: center;
      transition: all 0.2s ease;
      box-shadow: 0 0 10px rgba(0, 240, 255, 0.2);
      padding: 0;
      line-height: 1;
      -webkit-tap-highlight-color: transparent;
    }

    .info-btn:hover, .info-btn:active {
      background: var(--neon-cyan);
      color: #07090e;
      box-shadow: 0 0 18px rgba(0, 240, 255, 0.75);
      transform: scale(1.08);
    }

    .status-pill {
      display: flex;
      align-items: center;
      gap: 6px;
      font-family: 'Orbitron', sans-serif;
      font-size: 0.65rem;
      font-weight: 800;
      letter-spacing: 1px;
      padding: 5px 12px;
      border-radius: 20px;
      background: rgba(0, 0, 0, 0.5);
      border: 1px solid var(--panel-border);
      color: var(--text-muted);
      transition: all 0.25s ease;
    }

    .status-pill.connected {
      color: var(--neon-green);
      border-color: rgba(0, 255, 136, 0.4);
      background: rgba(0, 255, 136, 0.1);
      box-shadow: 0 0 12px rgba(0, 255, 136, 0.2);
    }

    .status-pill.spark-cut {
      color: #fff;
      background: var(--neon-red);
      border-color: #fff;
      box-shadow: 0 0 20px var(--neon-red-glow);
      animation: alertFlash 0.12s infinite alternate;
    }

    .status-dot {
      width: 7px;
      height: 7px;
      border-radius: 50%;
      background: currentColor;
      box-shadow: 0 0 6px currentColor;
    }

    @keyframes alertFlash {
      from { transform: scale(0.96); opacity: 0.85; }
      to { transform: scale(1.04); opacity: 1; }
    }

    /* Gauge Cluster Card */
    .cluster-card {
      width: 100%;
      max-width: 440px;
      background: var(--panel-bg);
      backdrop-filter: blur(20px);
      -webkit-backdrop-filter: blur(20px);
      border: 1px solid var(--panel-border);
      border-radius: 24px;
      padding: 12px 10px 16px;
      display: flex;
      flex-direction: column;
      align-items: center;
      position: relative;
      box-shadow: 0 18px 45px rgba(0, 0, 0, 0.7), inset 0 1px 0 rgba(255,255,255,0.06);
      margin-bottom: 14px;
    }

    .gauge-wrapper {
      width: 320px;
      height: 240px;
      position: relative;
      display: flex;
      justify-content: center;
      align-items: center;
    }

    .tach-svg {
      width: 100%;
      height: 100%;
      overflow: visible;
    }

    /* Dedicated Digital Display Bar Underneath Gauge */
    .cluster-digital-bar {
      width: 92%;
      background: rgba(8, 12, 18, 0.92);
      border: 1px solid rgba(0, 240, 255, 0.25);
      border-radius: 16px;
      padding: 8px 18px;
      display: flex;
      justify-content: space-between;
      align-items: center;
      margin-top: -8px;
      box-shadow: 0 6px 20px rgba(0, 0, 0, 0.6), inset 0 0 12px rgba(0, 240, 255, 0.05);
      position: relative;
      z-index: 5;
    }

    .digital-left {
      display: flex;
      flex-direction: column;
      align-items: flex-start;
    }

    .digital-rpm {
      font-family: 'Rajdhani', sans-serif;
      font-size: 2.9rem;
      font-weight: 800;
      line-height: 0.88;
      letter-spacing: -1px;
      color: #fff;
      text-shadow: 0 0 12px rgba(0, 240, 255, 0.5);
      font-variant-numeric: tabular-nums;
      transition: color 0.15s;
    }

    .digital-rpm.rev-warning {
      color: var(--neon-amber);
      text-shadow: 0 0 18px rgba(255, 170, 0, 0.6);
    }

    .digital-rpm.rev-danger {
      color: var(--neon-red);
      text-shadow: 0 0 22px rgba(255, 34, 71, 0.8);
    }

    .unit-tag {
      font-family: 'Orbitron', sans-serif;
      font-size: 0.62rem;
      font-weight: 700;
      letter-spacing: 2px;
      color: var(--text-muted);
      margin-top: 4px;
    }

    .digital-right {
      display: flex;
      flex-direction: column;
      align-items: flex-end;
      gap: 4px;
    }

    .peak-tag {
      font-family: 'Orbitron', sans-serif;
      font-size: 0.68rem;
      font-weight: 800;
      color: var(--neon-amber);
      background: rgba(255, 170, 0, 0.12);
      border: 1px solid rgba(255, 170, 0, 0.3);
      border-radius: 6px;
      padding: 3px 8px;
      letter-spacing: 1px;
      cursor: pointer;
    }

    .mode-badge-mini {
      font-size: 0.65rem;
      font-weight: 700;
      color: var(--text-muted);
      letter-spacing: 1px;
    }

    .spark-alert-banner {
      position: absolute;
      top: 10px;
      background: rgba(255, 34, 71, 0.2);
      border: 1px solid var(--neon-red);
      color: #fff;
      font-family: 'Orbitron', sans-serif;
      font-size: 0.7rem;
      font-weight: 900;
      letter-spacing: 2.5px;
      padding: 4px 18px;
      border-radius: 12px;
      text-transform: uppercase;
      box-shadow: 0 0 16px var(--neon-red-glow);
      opacity: 0;
      transform: translateY(-5px);
      transition: all 0.15s ease;
      pointer-events: none;
      z-index: 10;
    }

    .spark-alert-banner.active {
      opacity: 1;
      transform: translateY(0);
    }

    /* 2-Step Launch / Fire Button */
    .button-section {
      width: 100%;
      max-width: 440px;
      margin-bottom: 14px;
    }

    .launch-mode-selector {
      display: flex;
      gap: 8px;
      margin-bottom: 8px;
      width: 100%;
    }

    .mode-pill {
      flex: 1;
      padding: 9px 8px;
      border-radius: 12px;
      background: rgba(14, 20, 32, 0.85);
      border: 1px solid var(--panel-border);
      color: var(--text-muted);
      font-family: 'Orbitron', sans-serif;
      font-size: 0.68rem;
      font-weight: 800;
      letter-spacing: 0.5px;
      cursor: pointer;
      display: flex;
      justify-content: center;
      align-items: center;
      transition: all 0.2s ease;
      -webkit-tap-highlight-color: transparent;
    }

    .mode-pill.active {
      background: rgba(0, 240, 255, 0.15);
      border-color: var(--neon-cyan);
      color: var(--neon-cyan);
      box-shadow: 0 0 14px rgba(0, 240, 255, 0.3);
    }

    .fire-btn {
      width: 100%;
      height: 90px;
      background: linear-gradient(180deg, #2a0b12 0%, #130407 100%);
      border: 2px solid var(--neon-red);
      border-radius: 20px;
      display: flex;
      flex-direction: column;
      justify-content: center;
      align-items: center;
      gap: 4px;
      cursor: pointer;
      position: relative;
      overflow: hidden;
      box-shadow: 0 8px 28px rgba(255, 34, 71, 0.3), inset 0 1px 0 rgba(255, 255, 255, 0.15);
      transition: transform 0.05s ease, box-shadow 0.1s ease, background 0.1s ease;
      -webkit-tap-highlight-color: transparent;
    }

    .fire-btn::before {
      content: '';
      position: absolute;
      top: 0; left: 0; right: 0; bottom: 0;
      background: repeating-linear-gradient(45deg, rgba(255,255,255,0.02), rgba(255,255,255,0.02) 8px, transparent 8px, transparent 16px);
      pointer-events: none;
    }

    .fire-btn-title {
      font-family: 'Orbitron', sans-serif;
      font-size: 1.25rem;
      font-weight: 900;
      letter-spacing: 1.5px;
      color: #fff;
      display: flex;
      align-items: center;
      gap: 8px;
      text-shadow: 0 0 12px var(--neon-red-glow);
      text-align: center;
    }

    .fire-btn-sub {
      font-size: 0.72rem;
      font-weight: 700;
      letter-spacing: 1px;
      color: var(--neon-red);
      text-transform: uppercase;
      text-align: center;
    }

    .fire-btn:active, .fire-btn.pressed {
      background: var(--neon-red);
      transform: translateY(3px) scale(0.985);
      box-shadow: 0 0 45px rgba(255, 34, 71, 0.95), inset 0 0 15px rgba(0, 0, 0, 0.4);
    }

    .fire-btn:active .fire-btn-title, .fire-btn.pressed .fire-btn-title {
      color: #fff;
      text-shadow: 0 0 15px #fff;
    }

    .fire-btn:active .fire-btn-sub, .fire-btn.pressed .fire-btn-sub {
      color: #fff;
    }

    /* Hands-Free Button States */
    .fire-btn.armed {
      background: linear-gradient(180deg, #332505 0%, #171102 100%);
      border-color: var(--neon-amber);
      box-shadow: 0 0 30px rgba(255, 170, 0, 0.6), inset 0 0 15px rgba(255, 170, 0, 0.2);
      animation: pulseAmber 1.2s infinite ease-in-out;
    }

    .fire-btn.armed .fire-btn-title {
      color: #fff;
      text-shadow: 0 0 14px var(--neon-amber);
    }

    .fire-btn.armed .fire-btn-sub {
      color: var(--neon-amber);
    }

    .fire-btn.holding {
      background: var(--neon-red);
      border-color: #fff;
      box-shadow: 0 0 50px rgba(255, 34, 71, 0.95), inset 0 0 20px rgba(0,0,0,0.5);
      animation: alertFlash 0.12s infinite alternate;
    }

    .fire-btn.holding .fire-btn-title {
      color: #fff;
      text-shadow: 0 0 18px #fff;
    }

    .fire-btn.holding .fire-btn-sub {
      color: #ffe0e6;
    }

    .fire-btn.fired {
      background: linear-gradient(180deg, #053315 0%, #02170a 100%);
      border-color: var(--neon-green);
      box-shadow: 0 0 35px rgba(0, 255, 136, 0.8);
    }

    .fire-btn.fired .fire-btn-title {
      color: #fff;
      text-shadow: 0 0 15px var(--neon-green);
    }

    .fire-btn.fired .fire-btn-sub {
      color: var(--neon-green);
    }

    @keyframes pulseAmber {
      0%, 100% { transform: scale(1); box-shadow: 0 0 20px rgba(255, 170, 0, 0.4); }
      50% { transform: scale(1.02); box-shadow: 0 0 35px rgba(255, 170, 0, 0.85); }
    }

    /* Tuning Controls Card */
    .controls-card {
      width: 100%;
      max-width: 440px;
      background: var(--panel-bg);
      backdrop-filter: blur(20px);
      -webkit-backdrop-filter: blur(20px);
      border: 1px solid var(--panel-border);
      border-radius: 24px;
      padding: 20px 16px;
      display: flex;
      flex-direction: column;
      gap: 18px;
      box-shadow: 0 15px 35px rgba(0, 0, 0, 0.5);
      margin-bottom: 20px;
    }

    .control-row {
      display: flex;
      justify-content: space-between;
      align-items: center;
    }

    .control-label-group {
      display: flex;
      flex-direction: column;
    }

    .control-label {
      font-size: 1.05rem;
      font-weight: 700;
      color: #fff;
      letter-spacing: 0.5px;
    }

    .control-sub {
      font-size: 0.74rem;
      color: var(--text-muted);
      font-weight: 600;
    }

    .control-val-pill {
      font-family: 'Rajdhani', sans-serif;
      font-size: 1.15rem;
      font-weight: 800;
      padding: 2px 10px;
      border-radius: 8px;
      background: rgba(0, 0, 0, 0.55);
      border: 1px solid var(--panel-border);
      color: var(--neon-amber);
      font-variant-numeric: tabular-nums;
    }

    /* Master Toggle Switch */
    .switch-wrap {
      position: relative;
      display: inline-block;
      width: 58px;
      height: 32px;
    }

    .switch-wrap input {
      opacity: 0;
      width: 0;
      height: 0;
    }

    .switch-slider {
      position: absolute;
      cursor: pointer;
      top: 0; left: 0; right: 0; bottom: 0;
      background-color: #1a2233;
      border: 1px solid var(--panel-border);
      transition: .25s ease;
      border-radius: 32px;
    }

    .switch-slider::before {
      position: absolute;
      content: "";
      height: 24px;
      width: 24px;
      left: 3px;
      bottom: 3px;
      background-color: #8899af;
      transition: .25s ease;
      border-radius: 50%;
      box-shadow: 0 2px 6px rgba(0,0,0,0.5);
    }

    input:checked + .switch-slider {
      background-color: var(--neon-red);
      border-color: var(--neon-red);
      box-shadow: 0 0 15px var(--neon-red-glow);
    }

    input:checked + .switch-slider::before {
      transform: translateX(26px);
      background-color: #fff;
    }

    /* Motorsport Sliders */
    .slider-block {
      display: flex;
      flex-direction: column;
      gap: 8px;
    }

    input[type=range] {
      -webkit-appearance: none;
      width: 100%;
      height: 8px;
      border-radius: 6px;
      background: #141b2a;
      outline: none;
      border: 1px solid rgba(255,255,255,0.06);
    }

    input[type=range]::-webkit-slider-thumb {
      -webkit-appearance: none;
      appearance: none;
      width: 26px;
      height: 26px;
      border-radius: 50%;
      background: linear-gradient(135deg, #ff416c 0%, #ff2247 100%);
      cursor: pointer;
      box-shadow: 0 0 14px rgba(255, 34, 71, 0.7);
      border: 2px solid #fff;
    }

    /* Pattern Cards */
    .pattern-grid {
      display: grid;
      grid-template-columns: repeat(2, 1fr);
      gap: 8px;
      width: 100%;
    }

    .pattern-card {
      background: rgba(14, 18, 28, 0.8);
      border: 1px solid var(--panel-border);
      border-radius: 12px;
      padding: 10px 8px;
      display: flex;
      flex-direction: column;
      align-items: center;
      gap: 3px;
      cursor: pointer;
      transition: all 0.2s ease;
      text-align: center;
    }

    .pattern-card.active {
      background: linear-gradient(180deg, #2b1016 0%, #19090d 100%);
      border-color: var(--neon-red);
      box-shadow: 0 0 16px rgba(255, 34, 71, 0.35);
    }

    .pattern-card.span-2 {
      grid-column: span 2;
    }

    .pattern-icon {
      font-size: 1.15rem;
    }

    .pattern-title {
      font-family: 'Orbitron', sans-serif;
      font-size: 0.68rem;
      font-weight: 800;
      color: #fff;
      letter-spacing: 0.5px;
    }

    .pattern-desc {
      font-size: 0.65rem;
      color: var(--text-muted);
      line-height: 1.1;
    }

    /* Slider Step Adjustment Group */
    .val-adjust-group {
      display: flex;
      align-items: center;
      gap: 6px;
    }

    .step-btn {
      background: rgba(255, 255, 255, 0.08);
      border: 1px solid var(--panel-border);
      color: #fff;
      font-family: 'Rajdhani', sans-serif;
      font-size: 1.1rem;
      font-weight: 800;
      width: 30px;
      height: 30px;
      border-radius: 8px;
      cursor: pointer;
      display: flex;
      justify-content: center;
      align-items: center;
      transition: all 0.15s ease;
      user-select: none;
      -webkit-user-select: none;
    }

    .step-btn:active {
      background: var(--neon-cyan);
      color: #000;
      transform: scale(0.92);
    }

    /* Action Buttons */
    .save-flash-btn {
      width: 100%;
      padding: 14px;
      background: linear-gradient(180deg, #1c2538 0%, #131926 100%);
      border: 1px solid rgba(255,255,255,0.1);
      border-radius: 14px;
      color: #fff;
      font-family: 'Orbitron', sans-serif;
      font-size: 0.88rem;
      font-weight: 800;
      letter-spacing: 1.5px;
      text-transform: uppercase;
      cursor: pointer;
      display: flex;
      justify-content: center;
      align-items: center;
      gap: 8px;
      box-shadow: 0 4px 15px rgba(0,0,0,0.4);
      transition: all 0.2s ease;
    }

    .save-flash-btn:active {
      transform: scale(0.98);
      background: #25314a;
      border-color: var(--neon-cyan);
    }

    .restore-btn {
      width: 100%;
      padding: 12px;
      margin-top: 10px;
      background: rgba(255, 170, 0, 0.08);
      border: 1px solid rgba(255, 170, 0, 0.35);
      border-radius: 14px;
      color: var(--neon-amber);
      font-family: 'Orbitron', sans-serif;
      font-size: 0.78rem;
      font-weight: 800;
      letter-spacing: 1px;
      text-transform: uppercase;
      cursor: pointer;
      display: flex;
      justify-content: center;
      align-items: center;
      gap: 8px;
      transition: all 0.2s ease;
    }

    .restore-btn:active {
      background: rgba(255, 170, 0, 0.25);
      transform: scale(0.98);
    }

    /* Wireless OTA Update Card */
    .ota-wrap {
      width: 100%;
      margin-top: 8px;
      display: flex;
      flex-direction: column;
      gap: 8px;
    }
    .ota-pick-btn {
      width: 100%;
      background: rgba(0, 240, 255, 0.08);
      border: 1px dashed var(--neon-cyan);
      color: var(--neon-cyan);
      padding: 10px 14px;
      border-radius: 10px;
      font-family: 'Orbitron', sans-serif;
      font-size: 0.72rem;
      font-weight: 800;
      letter-spacing: 1px;
      cursor: pointer;
      display: flex;
      justify-content: center;
      align-items: center;
      gap: 8px;
      transition: all 0.2s ease;
    }
    .ota-pick-btn:active {
      background: rgba(0, 240, 255, 0.2);
    }
    .ota-file-name {
      font-size: 0.72rem;
      color: var(--text-muted);
      text-align: center;
      word-break: break-all;
      padding: 2px 6px;
    }
    .ota-flash-btn {
      width: 100%;
      background: linear-gradient(135deg, #00ff88 0%, #00aa55 100%);
      border: none;
      color: #07090e;
      padding: 12px 14px;
      border-radius: 10px;
      font-family: 'Orbitron', sans-serif;
      font-size: 0.8rem;
      font-weight: 900;
      letter-spacing: 1.2px;
      cursor: pointer;
      display: flex;
      justify-content: center;
      align-items: center;
      gap: 8px;
      box-shadow: 0 0 16px rgba(0, 255, 136, 0.4);
      transition: all 0.2s ease;
    }
    .ota-flash-btn:active {
      transform: scale(0.98);
      filter: brightness(1.2);
    }
    .ota-progress-box {
      width: 100%;
      background: rgba(0,0,0,0.6);
      border: 1px solid var(--panel-border);
      border-radius: 8px;
      overflow: hidden;
      position: relative;
      height: 24px;
      display: none;
      margin-top: 4px;
    }
    .ota-progress-fill {
      height: 100%;
      width: 0%;
      background: linear-gradient(90deg, #00f0ff, #00ff88);
      transition: width 0.15s linear;
    }
    .ota-progress-lbl {
      position: absolute;
      top: 0;
      left: 0;
      width: 100%;
      height: 100%;
      display: flex;
      justify-content: center;
      align-items: center;
      font-family: 'Orbitron', sans-serif;
      font-size: 0.7rem;
      font-weight: 800;
      color: #fff;
      text-shadow: 0 1px 3px rgba(0,0,0,0.8);
    }

    /* Toast notification */
    .toast {
      position: fixed;
      bottom: 24px;
      background: rgba(10, 15, 25, 0.95);
      border: 1px solid var(--neon-green);
      color: var(--neon-green);
      font-family: 'Orbitron', sans-serif;
      font-size: 0.8rem;
      font-weight: 700;
      letter-spacing: 1px;
      padding: 12px 24px;
      border-radius: 30px;
      box-shadow: 0 10px 30px rgba(0, 255, 136, 0.3);
      opacity: 0;
      transform: translateY(20px);
      transition: all 0.3s ease;
      pointer-events: none;
      z-index: 999;
    }

    .toast.show {
      opacity: 1;
      transform: translateY(0);
    }

    /* Help / Info Modal */
    .modal-backdrop {
      position: fixed;
      top: 0; left: 0; right: 0; bottom: 0;
      background: rgba(4, 7, 12, 0.88);
      backdrop-filter: blur(14px);
      -webkit-backdrop-filter: blur(14px);
      z-index: 2000;
      display: flex;
      justify-content: center;
      align-items: center;
      padding: 16px;
      opacity: 0;
      pointer-events: none;
      transition: opacity 0.25s ease;
    }

    .modal-backdrop.open {
      opacity: 1;
      pointer-events: auto;
    }

    .modal-card {
      background: rgba(14, 20, 32, 0.98);
      border: 1px solid rgba(0, 240, 255, 0.35);
      border-radius: 24px;
      box-shadow: 0 20px 60px rgba(0, 0, 0, 0.85), 0 0 35px rgba(0, 240, 255, 0.15);
      width: 100%;
      max-width: 440px;
      max-height: 88vh;
      display: flex;
      flex-direction: column;
      transform: translateY(20px) scale(0.96);
      transition: transform 0.25s cubic-bezier(0.16, 1, 0.3, 1);
      overflow: hidden;
    }

    .modal-backdrop.open .modal-card {
      transform: translateY(0) scale(1);
    }

    .modal-header {
      padding: 16px 18px;
      display: flex;
      justify-content: space-between;
      align-items: center;
      border-bottom: 1px solid var(--panel-border);
      background: rgba(20, 28, 44, 0.7);
    }

    .modal-title {
      font-family: 'Orbitron', sans-serif;
      font-size: 1rem;
      font-weight: 900;
      letter-spacing: 1px;
      color: #fff;
      display: flex;
      align-items: center;
      gap: 8px;
    }

    .modal-close-btn {
      background: rgba(255, 255, 255, 0.08);
      border: 1px solid var(--panel-border);
      color: var(--text-muted);
      width: 32px;
      height: 32px;
      border-radius: 50%;
      cursor: pointer;
      font-size: 1.1rem;
      font-weight: bold;
      display: flex;
      align-items: center;
      justify-content: center;
      transition: all 0.15s ease;
      -webkit-tap-highlight-color: transparent;
    }

    .modal-close-btn:hover, .modal-close-btn:active {
      background: var(--neon-red);
      color: #fff;
      border-color: var(--neon-red);
      box-shadow: 0 0 12px var(--neon-red-glow);
    }

    .modal-body {
      padding: 16px 16px 20px;
      overflow-y: auto;
      -webkit-overflow-scrolling: touch;
      display: flex;
      flex-direction: column;
      gap: 12px;
      font-size: 0.85rem;
      line-height: 1.4;
      color: var(--text-main);
    }

    .modal-section {
      background: rgba(8, 12, 20, 0.7);
      border: 1px solid rgba(255, 255, 255, 0.06);
      border-radius: 14px;
      padding: 12px 14px;
    }

    .modal-section-title {
      font-family: 'Orbitron', sans-serif;
      font-size: 0.82rem;
      font-weight: 800;
      letter-spacing: 0.8px;
      color: var(--neon-cyan);
      margin-bottom: 6px;
      display: flex;
      align-items: center;
      gap: 6px;
    }

    .modal-section p {
      margin-bottom: 6px;
      color: #c4d1e2;
    }

    .modal-section p:last-child {
      margin-bottom: 0;
    }

    .modal-highlight {
      color: var(--neon-amber);
      font-weight: 700;
    }

    .modal-footer {
      padding: 12px 16px;
      border-top: 1px solid var(--panel-border);
      background: rgba(20, 28, 44, 0.5);
    }

    .modal-ack-btn {
      width: 100%;
      padding: 12px;
      background: linear-gradient(135deg, #00f0ff 0%, #0088cc 100%);
      border: none;
      border-radius: 12px;
      color: #07090e;
      font-family: 'Orbitron', sans-serif;
      font-size: 0.85rem;
      font-weight: 900;
      letter-spacing: 1px;
      cursor: pointer;
      box-shadow: 0 4px 15px rgba(0, 240, 255, 0.35);
      transition: all 0.15s ease;
      -webkit-tap-highlight-color: transparent;
    }

    .modal-ack-btn:active {
      transform: scale(0.98);
      filter: brightness(1.2);
    }
  </style>
</head>
<body>

  <!-- Top Cockpit Bar -->
  <div class="header-bar">
    <div class="brand-wrap">
      <div class="brand-title">
        SWIFT <span class="brand-badge">G13BA</span>
      </div>
      <div class="brand-sub">1.3L 8V SHOW VEZÉRLŐ</div>
    </div>
    <div class="header-actions">
      <button id="infoBtn" class="info-btn" onclick="openModal()" title="Funkciók és útmutató">ℹ</button>
      <div id="statusBadge" class="status-pill">
        <div class="status-dot"></div>
        <span id="statusText">CSATLAKOZÁS...</span>
      </div>
    </div>
  </div>

  <!-- Motorsport Tachometer Cluster -->
  <div class="cluster-card">
    <div class="gauge-wrapper">
      <div id="sparkAlert" class="spark-alert-banner">⚡ GYÚJTÁSELVÉTEL AKTÍV ⚡</div>

      <svg class="tach-svg" viewBox="0 0 320 240">
        <defs>
          <!-- Neon Glow Filter -->
          <filter id="needleGlow" x="-50%" y="-50%" width="200%" height="200%">
            <feGaussianBlur in="SourceGraphic" stdDeviation="3" result="blur" />
            <feMerge>
              <feMergeNode in="blur" />
              <feMergeNode in="SourceGraphic" />
            </feMerge>
          </filter>
          <filter id="arcGlow" x="-20%" y="-20%" width="140%" height="140%">
            <feGaussianBlur stdDeviation="3.5" result="blur" />
            <feMerge>
              <feMergeNode in="blur" />
              <feMergeNode in="SourceGraphic" />
            </feMerge>
          </filter>
          <!-- Arc Gradient Definition -->
          <linearGradient id="arcGradient" x1="0%" y1="100%" x2="100%" y2="0%">
            <stop offset="0%" stop-color="#00f0ff" />
            <stop offset="55%" stop-color="#ffaa00" />
            <stop offset="85%" stop-color="#ff2247" />
          </linearGradient>
        </defs>

        <!-- Outer Bezel Rings (Center 160, 128, R=106) -->
        <circle cx="160" cy="128" r="124" fill="none" stroke="#121824" stroke-width="2" />
        <circle cx="160" cy="128" r="118" fill="none" stroke="#1c2538" stroke-width="1.5" stroke-dasharray="3 3" />

        <!-- Background Track (260 deg sweep from 140 to 400 deg, R=105) -->
        <path id="trackBg" d="M 79.5 195.5 A 105 105 0 1 1 240.5 195.5" fill="none" stroke="#101622" stroke-width="12" stroke-linecap="round" />

        <!-- Redline Danger Zone Arc (6200 to 8000 RPM) -->
        <path d="M 220.2 67.8 A 105 105 0 0 1 240.5 195.5" fill="none" stroke="rgba(255, 34, 71, 0.35)" stroke-width="12" stroke-linecap="round" />

        <!-- Active Illuminated Arc -->
        <path id="activeArc" d="M 79.5 195.5 A 105 105 0 1 1 240.5 195.5" fill="none" stroke="url(#arcGradient)" stroke-width="12" stroke-linecap="round" filter="url(#arcGlow)" stroke-dasharray="0 476" />

        <!-- Dynamic Marker: 2-Step Launch Limit -->
        <line id="launchMarker" x1="160" y1="23" x2="160" y2="10" stroke="#ffaa00" stroke-width="3" stroke-linecap="round" filter="url(#needleGlow)" />

        <!-- Dynamic Marker: Redline Limit -->
        <line id="redlineMarker" x1="160" y1="23" x2="160" y2="10" stroke="#ff2247" stroke-width="3" stroke-linecap="round" filter="url(#needleGlow)" />

        <!-- Tick marks & numbers container (generated by JS) -->
        <g id="ticksGroup"></g>

        <!-- Needle Group: Drawn along positive X-axis (0 deg) from (160, 128) to (248, 128) -->
        <g id="needleGroup" style="transition: transform 0.05s linear; transform-origin: 160px 128px; transform: rotate(140deg);">
          <!-- Needle trail glow -->
          <line x1="160" y1="128" x2="246" y2="128" stroke="rgba(255, 34, 71, 0.45)" stroke-width="5" stroke-linecap="round" filter="url(#needleGlow)" />
          <!-- Sharp illuminated racing needle -->
          <polygon points="160,125 248,127.3 251,128 248,128.7 160,131" fill="#ff2247" filter="url(#needleGlow)" />
          <line x1="160" y1="128" x2="247" y2="128" stroke="#fff" stroke-width="1.5" stroke-linecap="round" />
        </g>

        <!-- Billet Center Cap Hub -->
        <circle cx="160" cy="128" r="16" fill="#0d111a" stroke="#253046" stroke-width="2.5" />
        <circle cx="160" cy="128" r="10" fill="#141a27" stroke="#374563" stroke-width="1.5" />
        <circle cx="160" cy="128" r="4" fill="#ff2247" />
        <circle cx="160" cy="128" r="1.5" fill="#fff" />
      </svg>
    </div>

    <!-- Dedicated Cockpit Digital Display Bar Directly Below Dial -->
    <div class="cluster-digital-bar">
      <div class="digital-left">
        <div id="rpmNum" class="digital-rpm">0</div>
        <div class="unit-tag">FORDULATSZÁM &bull; G13BA 8V</div>
      </div>
      <div class="digital-right">
        <div id="peakBadge" class="peak-tag" onclick="resetPeak()" title="Kattints a nullázáshoz">CSÚCS 0</div>
        <div class="mode-badge-mini" id="patternDisplay">KEMÉNY TILTÁS</div>
      </div>
    </div>
  </div>

  <!-- Big Tactical Launch Control / 2-Step Button -->
  <div class="button-section">
    <div class="launch-mode-selector">
      <button type="button" id="modeHandsFree" class="mode-pill active" onclick="setLaunchMode('handsfree')">
        ⚡ HANDS-FREE RAJT
      </button>
      <button type="button" id="modeShow" class="mode-pill" onclick="setLaunchMode('show')">
        🔥 SHOW MÓD (NYOMVATARTÁS)
      </button>
    </div>

    <button id="btnTwoStep" class="fire-btn" type="button">
      <div id="fireBtnTitle" class="fire-btn-title">
        🏁 RAJTAUTOMATIKA ÉLESÍTÉSE 🏁
      </div>
      <div id="fireBtnSub" class="fire-btn-sub">KOPPINTS AZ ÉLESÍTÉSHEZ • 10 MP KÉSZENLÉT</div>
    </button>
  </div>

  <!-- Settings & Tuning Console -->
  <div class="controls-card">
    
    <!-- Master Arm -->
    <div class="control-row">
      <div class="control-label-group">
        <div class="control-label">Rendszer engedélyezése</div>
        <div class="control-sub">Tiltás & gyújtáselvétel főkapcsoló</div>
      </div>
      <label class="switch-wrap">
        <input type="checkbox" id="cfgArmed" onchange="sendConfig()">
        <span class="switch-slider"></span>
      </label>
    </div>

    <!-- Ghost Cam / V8 Lumpy Idle -->
    <div class="control-row">
      <div class="control-label-group">
        <div class="control-label">🔥 Ghost Cam™ / V8 Alapjárat</div>
        <div class="control-sub">Agresszív vezérműtengely dadogás (650-1200 RPM)</div>
      </div>
      <label class="switch-wrap">
        <input type="checkbox" id="cfgGhostCam" onchange="sendConfig()">
        <span class="switch-slider"></span>
      </label>
    </div>

    <hr style="border: 0; border-top: 1px solid var(--panel-border);">

    <!-- 2-Step Launch Limit Slider -->
    <div class="slider-block">
      <div class="control-row">
        <div class="control-label-group">
          <div class="control-label">🏁 Rajtautomatika limit (2-Step)</div>
          <div class="control-sub">Állóhelyzeti tiltás rajthoz & lángokhoz</div>
        </div>
        <div class="val-adjust-group">
          <button class="step-btn" type="button" onclick="stepSlider('cfgLaunch', -50)">-</button>
          <div id="valLaunch" class="control-val-pill">3800 RPM</div>
          <button class="step-btn" type="button" onclick="stepSlider('cfgLaunch', 50)">+</button>
        </div>
      </div>
      <input type="range" id="cfgLaunch" min="1500" max="6500" step="50" value="3800" oninput="updateSliders(); sendConfig()">
    </div>

    <!-- Main Redline Rev Limiter Slider -->
    <div class="slider-block">
      <div class="control-row">
        <div class="control-label-group">
          <div class="control-label">Maximális tiltás (Redline)</div>
          <div class="control-sub">Gyújtáselvételi motorvédelmi felső határ</div>
        </div>
        <div class="val-adjust-group">
          <button class="step-btn" type="button" onclick="stepSlider('cfgRedline', -50)">-</button>
          <div id="valRedline" class="control-val-pill">6200 RPM</div>
          <button class="step-btn" type="button" onclick="stepSlider('cfgRedline', 50)">+</button>
        </div>
      </div>
      <input type="range" id="cfgRedline" min="3000" max="7500" step="50" value="6200" oninput="updateSliders(); sendConfig()">
    </div>

    <!-- Decel Overrun Pops Slider & Toggle -->
    <div class="slider-block">
      <div class="control-row">
        <div class="control-label-group">
          <div class="control-label">Gázelvételi durrogás (Overrun)</div>
          <div class="control-sub">Pufogás motorféken gázelvételkor</div>
        </div>
        <label class="switch-wrap">
          <input type="checkbox" id="cfgDecelPops" onchange="sendConfig()">
          <span class="switch-slider"></span>
        </label>
      </div>
      <div class="control-row" style="margin-top: 2px;">
        <div class="control-sub">Bekapcsolási fordulatszám küszöb:</div>
        <div class="val-adjust-group">
          <button class="step-btn" type="button" onclick="stepSlider('cfgDecel', -50)">-</button>
          <div id="valDecel" class="control-val-pill">3200 RPM</div>
          <button class="step-btn" type="button" onclick="stepSlider('cfgDecel', 50)">+</button>
        </div>
      </div>
      <input type="range" id="cfgDecel" min="1800" max="6000" step="50" value="3200" oninput="updateSliders(); sendConfig()">
    </div>

    <!-- Cut Fire Pattern Grid (4 Patterns) -->
    <div class="slider-block">
      <div class="control-label" style="margin-bottom: 4px;">Kipufogó hangzás & mintázat</div>
      <div class="pattern-grid">
        <div class="pattern-card active" id="pat0" onclick="setPattern(0)">
          <div class="pattern-icon">⚡</div>
          <div class="pattern-title">KEMÉNY TILTÁS</div>
          <div class="pattern-desc">Bee*R limiter</div>
        </div>
        <div class="pattern-card" id="pat1" onclick="setPattern(1)">
          <div class="pattern-icon">🔥</div>
          <div class="pattern-title">LÁNGCSÓVA</div>
          <div class="pattern-desc">Tűzgolyók & lángok</div>
        </div>
        <div class="pattern-card" id="pat2" onclick="setPattern(2)">
          <div class="pattern-icon">💥</div>
          <div class="pattern-title">DURROGÁS</div>
          <div class="pattern-desc">Mély, öblös ropogás</div>
        </div>
        <div class="pattern-card" id="pat3" onclick="setPattern(3)">
          <div class="pattern-icon">🎯</div>
          <div class="pattern-title">AK-47</div>
          <div class="pattern-desc">Gépkarabély sorozat</div>
        </div>
        <div class="pattern-card span-2" id="pat4" onclick="setPattern(4)">
          <div class="pattern-icon">💣</div>
          <div class="pattern-title">ÁGYÚLÖVÉS / BOMBA</div>
          <div class="pattern-desc">1.6 mp szünet (benzingyűjtés) ➔ BRUTÁLIS DÚRANÁS</div>
        </div>
      </div>
    </div>

    <!-- Anti-Flood Safety Timeout -->
    <div class="slider-block">
      <div class="control-row">
        <div class="control-label-group">
          <div class="control-label">Leállás- & túldúsulás védelem</div>
          <div class="control-sub">Max. egybefüggő tiltás ideje (védi a motort)</div>
        </div>
        <div id="valTimeout" class="control-val-pill">3.0 s</div>
      </div>
      <input type="range" id="cfgTimeout" min="1.0" max="6.0" step="0.5" value="3.0" oninput="updateSliders(); sendConfig()">
    </div>

    <!-- Save Settings Button -->
    <button class="save-flash-btn" onclick="saveToFlash()">
      <span>💾</span> BEÁLLÍTÁSOK MENTÉSE (FLASH)
    </button>

    <!-- Restore Recommended Defaults Button -->
    <button class="restore-btn" onclick="restoreDefaults()">
      <span>🔄</span> AJÁNLOTT ÉRTÉKEK VISSZAÁLLÍTÁSA
    </button>

    <!-- Wireless Firmware Update (OTA) -->
    <div class="slider-block" style="border-top: 1px solid var(--panel-border); padding-top: 14px; margin-top: 6px;">
      <div class="control-row">
        <div class="control-label-group">
          <div class="control-label" style="color: var(--neon-cyan);">📡 Vezeték nélküli frissítés (OTA)</div>
          <div class="control-sub">Frissítsd a vezérlőt telefonról, kiszerelés nélkül!</div>
        </div>
      </div>
      <div class="ota-wrap">
        <input type="file" id="otaFileInput" accept=".bin" style="display: none;" onchange="handleOtaFileSelect(event)">
        <button type="button" class="ota-pick-btn" onclick="document.getElementById('otaFileInput').click()">
          <span>📁</span> FIRMWARE (.BIN) KIVÁLASZTÁSA
        </button>
        <div id="otaFileName" class="ota-file-name">Nincs fájl kiválasztva</div>
        <button type="button" id="otaUploadBtn" class="ota-flash-btn" style="display: none;" onclick="startOtaUpload()">
          <span>🚀</span> TELEPÍTÉS VEZETÉK NÉLKÜL
        </button>
        <div id="otaProgressBox" class="ota-progress-box">
          <div id="otaProgressFill" class="ota-progress-fill"></div>
          <div id="otaProgressLbl" class="ota-progress-lbl">0%</div>
        </div>
      </div>
    </div>

  </div>

  <div id="toast" class="toast">BEÁLLÍTÁSOK ELMENTVE!</div>

  <!-- Detailed Help & Info Modal Dialog -->
  <div id="infoModal" class="modal-backdrop">
    <div class="modal-card">
      <div class="modal-header">
        <div class="modal-title">
          <span>ℹ</span> FUNKCIÓ LEÍRÁSOK
        </div>
        <button class="modal-close-btn" onclick="closeModal()">✕</button>
      </div>
      <div class="modal-body">
        
        <div class="modal-section">
          <div class="modal-section-title">🏁 Rajtautomatika / 2-Step (Launch Control)</div>
          <p>• <span class="modal-highlight">⚡ Hands-Free Rajt mód:</span> Nem kell nyomva tartani a telefont! Egy érintéssel élesíted (10 mp készenlét), mindkét kezed szabad a kormányon és a váltón. Kuplung be, padlógáz: a motor a beállított értéken (pl. 3800 RPM) dadog és lángol. Amint leugrasz a kuplungról, a hajtáslánc terhelését érzékelve a vezérlő <span class="modal-highlight">automatikusan kioldja a tiltást</span>, és azonnal kilősz!</p>
          <p>• <span class="modal-highlight">🔥 Show mód:</span> Állóhelyzeti durrogtatáshoz és lángokhoz haveroknak: a gombot nyomva tartva tiltáson tarthatod a motort.</p>
          <p>• <span class="modal-highlight">🏎️ Fizikai kapcsoló (GPIO 23):</span> Kézifékkarra vagy kuplungpedálra kötve automatikusan és közvetlenül is vezérelhető!</p>
        </div>

        <div class="modal-section">
          <div class="modal-section-title">🔥 Ghost Cam™ / V8 Alapjárat</div>
          <p>Alapjáraton (650–1200 RPM között) ritmikusan szikrát vesz el, létrehozva a hegyes vezérműtengellyel ellátott amerikai V8 drag motorok agresszív, lusta dadogását.</p>
          <p><span class="modal-highlight">Biztonsági funkció:</span> Amint rálépsz a gázra és a motor 1250 RPM fölé ér, azonnal és észrevétlenül kikapcsol, a motor simán és teljes erővel forog fel!</p>
        </div>

        <div class="modal-section">
          <div class="modal-section-title">⚡ Maximális tiltás (Redline Limiter)</div>
          <p>A motorvédelem felső plafonja (pl. 6200 RPM). A gyári lassú üzemanyag-elvétel helyett villámgyors szikra-elvétellel dolgozik, így a japán motorsportból ismert legendás <span class="modal-highlight">Bee*R limiter</span> stílusú géppuskahangot és lángokat ad.</p>
        </div>

        <div class="modal-section">
          <div class="modal-section-title">💥 Gázelvételi durrogás (Overrun Decel)</div>
          <p>Ha a beállított küszöb (pl. 3200 RPM) feletti fordulatról hirtelen leveszed a lábad a gázról, a motorféküzem során rövid szikravágásokat iktat be. A kipufogóba jutó elégetlen keverék a forró csőben robban fel látványos pufogásokkal.</p>
        </div>

        <div class="modal-section">
          <div class="modal-section-title">🎯 Kipufogó hangzás és mintázatok</div>
          <p>• <span class="modal-highlight">Kemény tiltás (Hard Cut):</span> 40ms vágás / 40ms szikra — klasszikus gyors Bee*R limiter.</p>
          <p>• <span class="modal-highlight">Lángcsóva (Flames):</span> 60ms vágás / 30ms szikra — több üzemanyag jut a kipufogóba, hatalmas lángnyelvek.</p>
          <p>• <span class="modal-highlight">Durrogás (Gunfire):</span> 50ms vágás / 40ms szikra — sűrű, mély lövések és ropogás.</p>
          <p>• <span class="modal-highlight">AK-47:</span> Ultramagas frekvenciájú, 25ms staccato géppuskasorozat!</p>
          <p>• <span class="modal-highlight">Ágyúlövés (Bomba):</span> ~1.6 másodperces teljes szikramegvonás (a kipufogó megtelik benzinnel), majd hirtelen szikravisszaadás ➔ brutális dörrenés és hatalmas lángnyelv!</p>
        </div>

        <div class="modal-section">
          <div class="modal-section-title">🛡️ Biztonság & Memória (NVS Flash)</div>
          <p>• <span class="modal-highlight">Trafó zavarszűrés:</span> 4000 µs zajzár és adaptív szűrés védi a fordulatszám jelet a gyújtótrafó visszarúgásaitól.</p>
          <p>• <span class="modal-highlight">Leállás védelem (Anti-Flood):</span> A megadott idő (pl. 3 mp) folyamatos tiltás után a vezérlő kötelezően visszaadja a gyújtást, hogy megelőzze a gyertyák beköpését. Ha a csúszkát teljesen jobbra húzod (NINCS LIMIT ∞), a védelem kikapcsol, és a tiltás folyamatos marad.</p>
          <p>• <span class="modal-highlight">Mentés & Visszaállítás:</span> A beállítások az ESP32 belső flash memóriájába íródnak, így áramtalanítás után sem vesznek el.</p>
        </div>

      </div>
      <div class="modal-footer">
        <button class="modal-ack-btn" onclick="closeModal()">RENDBEN, ÉRTETTEM</button>
      </div>
    </div>
  </div>

  <script>
    let ws;
    let currentPattern = 0;
    let peakRpm = 0;
    let isConnected = false;

    // SVG Dial Geometry Constants
    const CX = 160;
    const CY = 128;
    const R_TRACK = 105;
    const START_ANGLE = 140; // 0 RPM (bottom-left)
    const TOTAL_SWEEP = 260; // 0 to 8000 RPM
    const MAX_RPM = 8000;
    const TOTAL_ARC_LEN = 2 * Math.PI * R_TRACK * (TOTAL_SWEEP / 360); // ~476 px

    const needleGroup = document.getElementById('needleGroup');
    const activeArc = document.getElementById('activeArc');
    const rpmNum = document.getElementById('rpmNum');
    const peakBadge = document.getElementById('peakBadge');
    const statusBadge = document.getElementById('statusBadge');
    const statusText = document.getElementById('statusText');
    const sparkAlert = document.getElementById('sparkAlert');
    const btnTwoStep = document.getElementById('btnTwoStep');
    const fireBtnTitle = document.getElementById('fireBtnTitle');
    const fireBtnSub = document.getElementById('fireBtnSub');
    const launchMarker = document.getElementById('launchMarker');
    const redlineMarker = document.getElementById('redlineMarker');
    const patternDisplay = document.getElementById('patternDisplay');

    let launchMode = 'handsfree'; // 'handsfree' or 'show'
    let currentLaunchState = 0;   // 0=OFF, 1=ARMED, 2=HOLDING, 3=FIRED

    const patternNames = ["KEMÉNY TILTÁS", "LÁNGCSÓVA", "DURROGÁS", "AK-47 SOROZAT", "ÁGYÚLÖVÉS / BOMBA"];

    function setLaunchMode(mode) {
      launchMode = mode;
      document.getElementById('modeHandsFree').className = 'mode-pill' + (mode === 'handsfree' ? ' active' : '');
      document.getElementById('modeShow').className = 'mode-pill' + (mode === 'show' ? ' active' : '');
      
      if (currentLaunchState !== 0) {
        if (ws && ws.readyState === WebSocket.OPEN) ws.send("LAUNCH:DISARM");
        currentLaunchState = 0;
      }
      updateButtonUI();
    }

    function updateButtonUI() {
      btnTwoStep.className = 'fire-btn';
      if (launchMode === 'show') {
        fireBtnTitle.innerHTML = '🏁 SHOW MÓD / 2-STEP 🏁';
        fireBtnSub.innerText = 'TARTSD NYOMVA A DURROGÁSHOZ ÉS LÁNGOKHOZ';
      } else {
        if (currentLaunchState === 1) { // ARMED
          btnTwoStep.classList.add('armed');
          fireBtnTitle.innerHTML = '⚡ RAJTRA KÉSZ / ARMED ⚡';
          fireBtnSub.innerText = 'LÉPJ A GÁZRA • KUPLUNG FELENGEDÉSRE KILŐ!';
        } else if (currentLaunchState === 2) { // HOLDING
          btnTwoStep.classList.add('holding');
          fireBtnTitle.innerHTML = '🔥 TILTÁS AKTÍV! LÁNGOK! 🔥';
          fireBtnSub.innerText = 'ENGEDD FEL A KUPLUNGOT A RAJTHOZ!';
        } else if (currentLaunchState === 3) { // FIRED
          btnTwoStep.classList.add('fired');
          fireBtnTitle.innerHTML = '🚀 RAJT! SIKERES KILÖVÉS 🚀';
          fireBtnSub.innerText = 'TILTÁS FELOLDVA • PADLÓGÁZ!';
        } else { // OFF
          fireBtnTitle.innerHTML = '🏁 RAJTAUTOMATIKA ÉLESÍTÉSE 🏁';
          fireBtnSub.innerText = 'KOPPINTS AZ ÉLESÍTÉSHEZ • 10 MP KÉSZENLÉT';
        }
      }
    }

    function openModal() {
      document.getElementById('infoModal').classList.add('open');
    }

    function closeModal() {
      document.getElementById('infoModal').classList.remove('open');
    }

    document.getElementById('infoModal').addEventListener('click', (e) => {
      if (e.target === document.getElementById('infoModal')) {
        closeModal();
      }
    });

    window.addEventListener('keydown', (e) => {
      if (e.key === 'Escape') closeModal();
    });

    function showToast(msg) {
      const t = document.getElementById('toast');
      t.innerText = msg;
      t.classList.add('show');
      setTimeout(() => t.classList.remove('show'), 2500);
    }

    // Build Graduated Ticks & Numbers around the Arc
    function buildGaugeTicks() {
      const g = document.getElementById('ticksGroup');
      let html = '';

      for (let rpm = 0; rpm <= MAX_RPM; rpm += 250) {
        const fraction = rpm / MAX_RPM;
        const deg = START_ANGLE + fraction * TOTAL_SWEEP;
        const rad = deg * Math.PI / 180;

        const isMajor = (rpm % 1000 === 0);
        const isMid = (rpm % 500 === 0 && !isMajor);

        const rOuter = R_TRACK + 7;
        const rInner = isMajor ? (R_TRACK - 13) : (isMid ? (R_TRACK - 7) : (R_TRACK - 4));
        const strokeWidth = isMajor ? 2.5 : (isMid ? 1.5 : 1);
        const strokeColor = (rpm >= 6000) ? '#ff2247' : (rpm >= 4500 ? '#ffaa00' : (isMajor ? '#e0e8f5' : '#475569'));

        const x1 = (CX + rInner * Math.cos(rad)).toFixed(1);
        const y1 = (CY + rInner * Math.sin(rad)).toFixed(1);
        const x2 = (CX + rOuter * Math.cos(rad)).toFixed(1);
        const y2 = (CY + rOuter * Math.sin(rad)).toFixed(1);

        html += `<line x1="${x1}" y1="${y1}" x2="${x2}" y2="${y2}" stroke="${strokeColor}" stroke-width="${strokeWidth}" stroke-linecap="round" />`;

        // Render Numbers for major thousands
        if (isMajor) {
          const rText = R_TRACK - 24;
          const tx = (CX + rText * Math.cos(rad)).toFixed(1);
          const ty = (CY + rText * Math.sin(rad) + 4).toFixed(1);
          const num = rpm / 1000;
          const textColor = (rpm >= 6000) ? '#ff2247' : (rpm >= 4500 ? '#ffaa00' : '#a0b3cc');
          html += `<text x="${tx}" y="${ty}" font-family="'Orbitron', sans-serif" font-size="12" font-weight="900" fill="${textColor}" text-anchor="middle">${num}</text>`;
        }
      }
      g.innerHTML = html;
    }

    // Set Marker Tick on Rim
    function updateMarker(element, rpm) {
      const fraction = Math.max(0, Math.min(1, rpm / MAX_RPM));
      const deg = START_ANGLE + fraction * TOTAL_SWEEP;
      const rad = deg * Math.PI / 180;

      const r1 = R_TRACK - 14;
      const r2 = R_TRACK + 8;
      const x1 = (CX + r1 * Math.cos(rad)).toFixed(1);
      const y1 = (CY + r1 * Math.sin(rad)).toFixed(1);
      const x2 = (CX + r2 * Math.cos(rad)).toFixed(1);
      const y2 = (CY + r2 * Math.sin(rad)).toFixed(1);

      element.setAttribute('x1', x1);
      element.setAttribute('y1', y1);
      element.setAttribute('x2', x2);
      element.setAttribute('y2', y2);
    }

    // Update Live RPM Needle & Glow Arc
    function setGaugeRpm(rpm) {
      const clamped = Math.max(0, Math.min(MAX_RPM, rpm));
      const fraction = clamped / MAX_RPM;
      
      // Update digital readout
      rpmNum.innerText = clamped;
      if (clamped >= 6200) {
        rpmNum.className = 'digital-rpm rev-danger';
      } else if (clamped >= 4500) {
        rpmNum.className = 'digital-rpm rev-warning';
      } else {
        rpmNum.className = 'digital-rpm';
      }

      // Update Peak RPM
      if (clamped > peakRpm) {
        peakRpm = clamped;
        peakBadge.innerText = "CSÚCS " + peakRpm;
      }

      // Rotate Needle: Exactly matches the tick calculation!
      const deg = START_ANGLE + fraction * TOTAL_SWEEP;
      needleGroup.style.transform = `rotate(${deg}deg)`;

      // Active illuminated arc fill
      const arcFilled = fraction * TOTAL_ARC_LEN;
      activeArc.setAttribute('stroke-dasharray', `${arcFilled.toFixed(1)} ${TOTAL_ARC_LEN.toFixed(1)}`);
    }

    function resetPeak() {
      peakRpm = 0;
      peakBadge.innerText = "CSÚCS 0";
      showToast("CSÚCSÉRTÉK NULLÁZVA!");
    }

    // Startup Needle Sweep Sequence (sweeps to 8k and returns to 0)
    function runStartupSweep() {
      let step = 0;
      const sweepInterval = setInterval(() => {
        step += 250;
        if (step <= 8000) {
          setGaugeRpm(step);
        } else if (step <= 16000) {
          setGaugeRpm(16000 - step);
        } else {
          clearInterval(sweepInterval);
          setGaugeRpm(0);
        }
      }, 16);
    }

    // Connect WebSockets
    function connectWS() {
      const host = window.location.hostname || "192.168.4.1";
      try {
        if (ws) {
          ws.onopen = null;
          ws.onmessage = null;
          ws.onclose = null;
          ws.onerror = null;
          ws.close();
        }
      } catch (e) {}

      ws = new WebSocket('ws://' + host + ':81/');

      ws.onopen = () => {
        isConnected = true;
        statusBadge.className = "status-pill connected";
        statusText.innerText = "KAPCSOLÓDVA";
        ws.send("GET_CONFIG");
        runStartupSweep();
      };

      ws.onerror = () => {
        // triggers reconnect via onclose
      };

      ws.onclose = () => {
        isConnected = false;
        statusBadge.className = "status-pill";
        statusText.innerText = "ÚJRACSATLAKOZÁS...";
        setTimeout(connectWS, 1500);
      };

      ws.onmessage = (evt) => {
        const msg = evt.data;
        if (msg.startsWith("T:")) {
          const parts = msg.substring(2).split(',');
          const rpm = parseInt(parts[0]) || 0;
          const cut = parts[1] === "1";
          const lState = parts.length > 2 ? parseInt(parts[2]) : 0;

          setGaugeRpm(rpm);

          if (cut) {
            statusBadge.className = "status-pill spark-cut";
            statusText.innerText = "TILTÁS AKTÍV";
            sparkAlert.classList.add('active');
          } else {
            statusBadge.className = "status-pill connected";
            statusText.innerText = "KAPCSOLÓDVA";
            sparkAlert.classList.remove('active');
          }

          if (launchMode === 'handsfree') {
            if (lState !== currentLaunchState) {
              currentLaunchState = lState;
              updateButtonUI();
              if (lState === 3 && navigator.vibrate) {
                navigator.vibrate([80, 40, 120]);
              }
            }
          }
        } else if (msg.startsWith("CFG:")) {
          const c = JSON.parse(msg.substring(4));
          document.getElementById('cfgArmed').checked = c.armed;
          document.getElementById('cfgLaunch').value = c.launchRpm;
          document.getElementById('cfgRedline').value = c.redlineRpm;
          document.getElementById('cfgDecel').value = c.decelRpm;
          document.getElementById('cfgDecelPops').checked = c.decelPops;
          if (c.maxCutSeconds <= 0.05 || c.maxCutSeconds >= 6.0) {
            document.getElementById('cfgTimeout').value = 6.0;
          } else {
            document.getElementById('cfgTimeout').value = c.maxCutSeconds;
          }
          if (c.ghostCam !== undefined) {
            document.getElementById('cfgGhostCam').checked = !!c.ghostCam;
          }
          setPattern(c.cutPattern, false);
          updateSliders();
        }
      };
    }

    function updateSliders() {
      const launch = parseInt(document.getElementById('cfgLaunch').value);
      const redline = parseInt(document.getElementById('cfgRedline').value);
      const decel = parseInt(document.getElementById('cfgDecel').value);
      const timeout = parseFloat(document.getElementById('cfgTimeout').value);

      document.getElementById('valLaunch').innerText = launch + " RPM";
      document.getElementById('valRedline').innerText = redline + " RPM";
      document.getElementById('valDecel').innerText = decel + " RPM";
      if (timeout >= 6.0) {
        document.getElementById('valTimeout').innerText = "NINCS LIMIT (∞)";
        document.getElementById('valTimeout').style.color = "var(--neon-red)";
      } else {
        document.getElementById('valTimeout').innerText = timeout.toFixed(1) + " s";
        document.getElementById('valTimeout').style.color = "var(--neon-amber)";
      }

      // Synchronize glowing markers on the tachometer dial!
      updateMarker(launchMarker, launch);
      updateMarker(redlineMarker, redline);
    }

    function stepSlider(id, delta) {
      const el = document.getElementById(id);
      let val = parseInt(el.value) + delta;
      const min = parseInt(el.min);
      const max = parseInt(el.max);
      if (val < min) val = min;
      if (val > max) val = max;
      el.value = val;
      updateSliders();
      sendConfig();
    }

    function setPattern(idx, send = true) {
      currentPattern = idx;
      patternDisplay.innerText = patternNames[idx] || "EGYEDI";
      for (let i = 0; i < 5; i++) {
        const el = document.getElementById('pat' + i);
        if (el) {
          const baseClass = (i === 4) ? 'pattern-card span-2' : 'pattern-card';
          el.className = baseClass + (i === idx ? ' active' : '');
        }
      }
      if (send) sendConfig();
    }

    function sendConfig() {
      if (ws && ws.readyState === WebSocket.OPEN) {
        const timeoutRaw = parseFloat(document.getElementById('cfgTimeout').value);
        const effectiveTimeout = (timeoutRaw >= 6.0) ? 0.0 : timeoutRaw;
        const payload = "SET_CFG:" + JSON.stringify({
          armed: document.getElementById('cfgArmed').checked,
          launchRpm: parseInt(document.getElementById('cfgLaunch').value),
          redlineRpm: parseInt(document.getElementById('cfgRedline').value),
          decelRpm: parseInt(document.getElementById('cfgDecel').value),
          decelPops: document.getElementById('cfgDecelPops').checked,
          cutPattern: currentPattern,
          maxCutSeconds: effectiveTimeout,
          ghostCam: document.getElementById('cfgGhostCam').checked
        });
        ws.send(payload);
      }
    }

    function saveToFlash() {
      if (ws && ws.readyState === WebSocket.OPEN) {
        ws.send("SAVE_FLASH");
        showToast("BEÁLLÍTÁSOK ELMENTVE A FLASH-BE!");
      }
    }

    function restoreDefaults() {
      document.getElementById('cfgArmed').checked = true;
      document.getElementById('cfgLaunch').value = 3800;
      document.getElementById('cfgRedline').value = 6200;
      document.getElementById('cfgDecel').value = 3200;
      document.getElementById('cfgDecelPops').checked = true;
      document.getElementById('cfgTimeout').value = 3.0;
      document.getElementById('cfgGhostCam').checked = false;
      setPattern(1, false);
      updateSliders();
      sendConfig();
      saveToFlash();
      showToast("AJÁNLOTT ÉRTÉKEK VISSZAÁLLÍTVA!");
    }

    // High-speed touch & click handling with haptics
    function handleBtnInteraction(isDown) {
      if (launchMode === 'show') {
        if (isDown) {
          btnTwoStep.classList.add('pressed');
          if (navigator.vibrate) navigator.vibrate([40, 30, 40]);
          if (ws && ws.readyState === WebSocket.OPEN) ws.send("BTN:1");
        } else {
          btnTwoStep.classList.remove('pressed');
          if (ws && ws.readyState === WebSocket.OPEN) ws.send("BTN:0");
        }
      } else {
        // Hands-Free mode: tap to toggle ARM / DISARM
        if (isDown) {
          if (navigator.vibrate) navigator.vibrate(50);
          if (currentLaunchState === 0) {
            currentLaunchState = 1;
            if (ws && ws.readyState === WebSocket.OPEN) ws.send("LAUNCH:ARM");
            showToast("RAJTAUTOMATIKA ÉLESÍTVE! (10 MP)");
          } else {
            currentLaunchState = 0;
            if (ws && ws.readyState === WebSocket.OPEN) ws.send("LAUNCH:DISARM");
            showToast("ÉLESÍTÉS TÖRÖLVE!");
          }
          updateButtonUI();
        }
      }
    }

    btnTwoStep.addEventListener('touchstart', (e) => { e.preventDefault(); handleBtnInteraction(true); });
    btnTwoStep.addEventListener('touchend', (e) => { e.preventDefault(); if (launchMode === 'show') handleBtnInteraction(false); });
    btnTwoStep.addEventListener('touchcancel', (e) => { e.preventDefault(); if (launchMode === 'show') handleBtnInteraction(false); });
    btnTwoStep.addEventListener('mousedown', (e) => { handleBtnInteraction(true); });
    window.addEventListener('mouseup', () => { if (launchMode === 'show') handleBtnInteraction(false); });

    // Wireless OTA Firmware Update
    let selectedOtaFile = null;

    function handleOtaFileSelect(e) {
      const file = e.target.files[0];
      if (!file) return;
      if (!file.name.endsWith('.bin')) {
        alert("Kérlek csak lefordított .bin fájlt válassz ki!");
        e.target.value = '';
        return;
      }
      selectedOtaFile = file;
      const fNameEl = document.getElementById('otaFileName');
      const sizeKb = (file.size / 1024).toFixed(1);
      fNameEl.innerText = file.name + " (" + sizeKb + " KB)";
      fNameEl.style.color = "var(--neon-green)";
      document.getElementById('otaUploadBtn').style.display = 'flex';
    }

    function startOtaUpload() {
      if (!selectedOtaFile) {
        alert("Előbb válassz ki egy firmware .bin fájlt!");
        return;
      }

      if (!confirm("Biztosan elindítod a vezeték nélküli frissítést?\nA feltöltés kb. 15-20 másodperc. Ne zárd be a böngészőt!")) {
        return;
      }

      const upBtn = document.getElementById('otaUploadBtn');
      const pickBtn = document.querySelector('.ota-pick-btn');
      const progressBox = document.getElementById('otaProgressBox');
      const progressFill = document.getElementById('otaProgressFill');
      const progressLbl = document.getElementById('otaProgressLbl');

      upBtn.style.display = 'none';
      if (pickBtn) pickBtn.style.display = 'none';
      progressBox.style.display = 'block';
      progressFill.style.width = '0%';
      progressLbl.innerText = 'Feltöltés: 0%';

      const formData = new FormData();
      formData.append('update', selectedOtaFile, selectedOtaFile.name);

      const xhr = new XMLHttpRequest();
      xhr.open('POST', '/update', true);

      xhr.upload.onprogress = (e) => {
        if (e.lengthComputable) {
          const percent = Math.round((e.loaded / e.total) * 100);
          progressFill.style.width = percent + '%';
          progressLbl.innerText = 'Feltöltés: ' + percent + '%';
        }
      };

      xhr.onload = () => {
        if (xhr.status === 200) {
          progressFill.style.width = '100%';
          progressFill.style.background = 'var(--neon-green)';
          progressLbl.innerText = 'SIKERES! ÚJRAINDÍTÁS...';
          showToast('FIRMWARE FRISSÍTVE! ÚJRAINDULÁS...');
          setTimeout(() => {
            progressLbl.innerText = 'ÚJRACSATLAKOZÁS...';
            setTimeout(() => {
              window.location.reload();
            }, 4000);
          }, 3000);
        } else {
          progressFill.style.background = 'var(--neon-red)';
          progressLbl.innerText = 'HIBA: ' + xhr.responseText;
          alert('Frissítési hiba: ' + xhr.responseText);
          upBtn.style.display = 'flex';
          if (pickBtn) pickBtn.style.display = 'flex';
        }
      };

      xhr.onerror = () => {
        progressFill.style.background = 'var(--neon-red)';
        progressLbl.innerText = 'KAPCSOLATI HIBA!';
        alert('Hálózati hiba a frissítés során. Kérlek ellenőrizd a Wi-Fi kapcsolatot!');
        upBtn.style.display = 'flex';
        if (pickBtn) pickBtn.style.display = 'flex';
      };

      xhr.send(formData);
    }

    window.onload = () => {
      buildGaugeTicks();
      updateSliders();
      updateButtonUI();
      connectWS();
      if (!window.location.hostname || window.location.hostname === "") {
        runStartupSweep();
      }
    };
  </script>
</body>
</html>
)rawliteral";

// =========================================================================================
// TACHOMETER HARDWARE INTERRUPT SERVICE ROUTINE (ISR)
// =========================================================================================
void IRAM_ATTR handleTachPulse() {
  unsigned long now = micros();
  unsigned long interval = now - lastPulseMicros;

  // 1. Hard noise floor: Above 7500 RPM (interval < 4000 µs), physically impossible on G13BA
  if (interval < 4000) {
    return;
  }

  // 2. Cut-compensation: If spark cut was active and the interval is an integer multiple of lastValidInterval
  //    (compensates for suppressed tachometer pulses during ignition cut so calculated RPM stays accurate)
  if (isSparkCutActive && lastValidInterval >= 4000) {
    unsigned long ratio = (interval * 10) / lastValidInterval;
    int skippedFactor = (ratio + 5) / 10;
    if (skippedFactor >= 2 && skippedFactor <= 8) {
      interval = interval / skippedFactor;
    }
  } else {
    // 3. Dynamic Adaptive Debounce (only active when not cutting):
    //    An engine cannot double its RPM in half a revolution (< 55% of previous valid period).
    if (lastValidInterval > 0 && interval < (lastValidInterval * 55 / 100)) {
      return;
    }
  }

  lastPulseMicros = now;

  // 4-cylinder rolling buffer: averages all 4 cylinders (1-3-4-2) to cancel distributor backlash & opto ringing
  pulseHistory[pulseHistIdx] = interval;
  pulseHistIdx = (pulseHistIdx + 1) & 0x03;
  unsigned long rollingAvg = (pulseHistory[0] + pulseHistory[1] + pulseHistory[2] + pulseHistory[3]) >> 2;

  pulseIntervalSum += rollingAvg;
  pulseIntervalCount++;
  lastValidInterval = rollingAvg;
  ghostCamPulseCounter++;
}

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
    virtualTwoStepActive = true;
  } else if (text == "BTN:0") {
    virtualTwoStepActive = false;
  } else if (text == "LAUNCH:ARM") {
    launchState = LAUNCH_ARMED;
    launchArmTimestamp = millis();
    peakLaunchRpm = 0;
  } else if (text == "LAUNCH:DISARM") {
    launchState = LAUNCH_OFF;
  } else if (text == "GET_CONFIG") {
    String cfgJson = "CFG:{\"armed\":" + String(config.armed ? "true" : "false") +
                     ",\"launchRpm\":" + String(config.launchRpm) +
                     ",\"redlineRpm\":" + String(config.redlineRpm) +
                     ",\"decelRpm\":" + String(config.decelRpm) +
                     ",\"decelPops\":" + String(config.decelPops ? "true" : "false") +
                     ",\"cutPattern\":" + String(config.cutPattern) +
                     ",\"maxCutSeconds\":" + String(config.maxCutSeconds, 1) +
                     ",\"ghostCam\":" + String(config.ghostCam ? "true" : "false") + "}";
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
  } else if (text == "SAVE_FLASH") {
    saveConfigToNVS();
  }
}

// =========================================================================================
// NON-VOLATILE STORAGE (NVS PREFERENCES)
// =========================================================================================
void loadConfigFromNVS() {
  prefs.begin("tuning", false); // Initialize namespace (creates if not exists)
  config.armed         = prefs.getBool("armed", true);
  config.launchRpm     = prefs.getInt("launchRpm", 3800);
  config.redlineRpm    = prefs.getInt("redlineRpm", 6200);
  config.decelPops     = prefs.getBool("decelPops", true);
  config.decelRpm      = prefs.getInt("decelRpm", 3200);
  config.cutPattern    = prefs.getInt("cutPattern", 1);
  config.maxCutSeconds = prefs.getFloat("maxCutSec", 3.0f);
  config.ghostCam      = prefs.getBool("ghostCam", false);
  prefs.end();
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
  prefs.end();
}

// =========================================================================================
// IGNITION CUT CONTROL LOGIC
// =========================================================================================
void executeIgnitionCut(bool shouldCut, bool directClamp = false) {
  if (shouldCut) {
    if (!isSparkCutActive) {
      cutEngagedTimestamp = millis();
      isSparkCutActive = true;
    }
    
    // Safety check: Anti-flood timeout to prevent cylinder washdown (if enabled)
    if (!directClamp && (config.maxCutSeconds > 0.05f) && (millis() - cutEngagedTimestamp > (unsigned long)(config.maxCutSeconds * 1000.0f))) {
      digitalWrite(PIN_SPARK_CUT, LOW);  // Disengage cut for safety
      digitalWrite(PIN_STATUS_LED, LOW);
      return;
    }

    // Apply selected cut pattern
    cutCycleCounter++;
    bool applyHardwareClamp = false;

    if (directClamp) {
      applyHardwareClamp = true;
    } else if (config.cutPattern == 0) {
      // Pattern 0: Hard Cut (Bee*R style - 100% spark suppression)
      applyHardwareClamp = true;
    } else if (config.cutPattern == 1) {
      // Pattern 1: Flame Spitter (cut 3 sparks, fire 1 spark for exhaust detonation)
      applyHardwareClamp = (cutCycleCounter % 4 != 0);
    } else if (config.cutPattern == 2) {
      // Pattern 2: Gunfire Crackle (rhythmic 2-cut / 1-fire / 1-cut burst)
      int m = cutCycleCounter % 6;
      applyHardwareClamp = (m == 0 || m == 1 || m == 3 || m == 4);
    } else if (config.cutPattern == 3) {
      // Pattern 3: AK-47 Machine Gun (Rapid Staccato: 2 cut, 1 fire, 2 cut, 1 fire)
      int m = cutCycleCounter % 6;
      applyHardwareClamp = (m != 2 && m != 5);
    } else if (config.cutPattern == 4) {
      // Pattern 4: Ágyúlövés / Bomba (Fireball Cannon Blast)
      // Phase 1: 100% spark suppression for 1600ms (accumulate unburnt fuel in exhaust)
      if (!cannonInCutPhase) {
        cannonInCutPhase = true;
        cannonCutStart = millis();
      }

      unsigned long elapsed = millis() - cannonCutStart;
      if (elapsed < 1600 && currentRpm > 2100) {
        applyHardwareClamp = true; // Complete silence & fuel soak
      } else {
        applyHardwareClamp = false; // BANG! Spark restored, huge explosion & fireball
        if (elapsed > 2200) {
          cannonInCutPhase = false; // Reset cycle once revved back up
        }
      }
    }

    digitalWrite(PIN_SPARK_CUT, applyHardwareClamp ? HIGH : LOW);
    digitalWrite(PIN_STATUS_LED, applyHardwareClamp ? HIGH : LOW);
  } else {
    // Normal operation: Transistor unpowered (passes stock ECU signal 100%)
    isSparkCutActive = false;
    cannonInCutPhase = false;
    digitalWrite(PIN_SPARK_CUT, LOW);
    digitalWrite(PIN_STATUS_LED, LOW);
  }
}

// =========================================================================================
// SETUP
// =========================================================================================
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== Suzuki Swift 1.3 8V Show Tuning Initializing ===");

  // Pin Configuration
  pinMode(PIN_TACH_IN, INPUT_PULLUP);
  pinMode(PIN_SPARK_CUT, OUTPUT);
  pinMode(PIN_STATUS_LED, OUTPUT);
  pinMode(PIN_LAUNCH_SW, INPUT_PULLUP);

  // Default fail-safe state: LOW (transistor OFF = no spark cut)
  digitalWrite(PIN_SPARK_CUT, LOW);
  digitalWrite(PIN_STATUS_LED, LOW);

  // Attach Hardware Interrupt for Tachometer Sensing (Falling edge from PC817 optocoupler)
  attachInterrupt(digitalPinToInterrupt(PIN_TACH_IN), handleTachPulse, FALLING);

  // Load persistent tuning configuration from Flash
  loadConfigFromNVS();

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

  // Fallback direct browser upload form at /update
  server.on("/update", HTTP_GET, []() {
    server.send(200, "text/html",
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
    server.sendHeader("Connection", "close");
    if (Update.hasError()) {
      server.send(500, "text/plain", "OTA HIBA: " + String(Update.errorString()));
    } else {
      server.send(200, "text/plain", "SIKERES FRISSITES! A vezerlo ujraindul...");
      delay(500);
      ESP.restart();
    }
  }, []() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      // Safety: Disable spark cut so ignition is NEVER clamped during firmware flashing!
      digitalWrite(PIN_SPARK_CUT, LOW);
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
      Serial.println("[OTA] Frissítés megszakítva!");
      digitalWrite(PIN_STATUS_LED, LOW);
    }
  });

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

  // 1. Calculate Real-Time Engine RPM every 40 ms (25 Hz)
  if (currentMillis - lastRpmCalcMillis >= 40) {
    lastRpmCalcMillis = currentMillis;

    int newRpm = 0;
    // Check if engine stopped (no pulse in last 250ms = 0 RPM)
    if (micros() - lastPulseMicros > 250000) {
      newRpm = 0;
      lastValidInterval = 0;
    } else {
      noInterrupts();
      unsigned long sum = pulseIntervalSum;
      unsigned int count = pulseIntervalCount;
      unsigned long lastInt = lastValidInterval;
      pulseIntervalSum = 0;
      pulseIntervalCount = 0;
      interrupts();

      if (count > 0) {
        unsigned long avgInterval = sum / count;
        // RPM Formula: (60,000,000 micros / avgInterval) / PULSES_PER_REV
        newRpm = (60000000UL / avgInterval) / PULSES_PER_REV;
      } else if (lastInt > 0) {
        newRpm = (60000000UL / lastInt) / PULSES_PER_REV;
      }
      if (newRpm > HARD_MAX_RPM) newRpm = HARD_MAX_RPM;
    }

    // Responsive EMA filter (75% new, 25% previous) - eliminates any single-frame display jump
    if (currentRpm == 0 || newRpm == 0) {
      currentRpm = newRpm;
    } else {
      currentRpm = (newRpm * 3 + currentRpm) / 4;
    }

    rpmDelta = currentRpm - previousRpm;
    previousRpm = currentRpm;
  }

  // 2. Evaluate Spark-Cut Conditions
  bool cutRequired = false;
  bool directClamp = false;

  // Physical launch switch (Clutch / Handbrake / Steering Button) on GPIO 23
  bool physicalLaunchActive = (digitalRead(PIN_LAUNCH_SW) == LOW);

  // Hands-Free Auto Launch State Machine
  if (launchState == LAUNCH_ARMED) {
    if (currentMillis - launchArmTimestamp > 10000) {
      launchState = LAUNCH_OFF; // 10s arming timeout
    } else if (currentRpm >= (config.launchRpm - 200)) {
      launchState = LAUNCH_HOLDING;
      launchHoldStart = currentMillis;
      peakLaunchRpm = currentRpm;
    }
  } else if (launchState == LAUNCH_HOLDING) {
    if (currentRpm > peakLaunchRpm) {
      peakLaunchRpm = currentRpm;
    }

    // Auto-Release Trigger: Real drivetrain load / clutch bite detection
    // Once holding on limiter for at least 400ms, if engine drops > 400 RPM below the launch RPM setpoint, release!
    if (currentMillis - launchHoldStart >= 400 && currentRpm < (config.launchRpm - 400)) {
      launchState = LAUNCH_FIRED;
      launchArmTimestamp = currentMillis; // reuse timestamp for 2.0s fired display
    }
    // Safety timeout: If driver sits on limiter longer than maxCutSeconds (if enabled)
    else if ((config.maxCutSeconds > 0.05f) && (currentMillis - launchHoldStart > (unsigned long)(config.maxCutSeconds * 1000.0f))) {
      launchState = LAUNCH_OFF;
    }
    // Abort if throttle is released before clutch drop (drops below MIN_CUT_RPM)
    else if (currentRpm < MIN_CUT_RPM) {
      launchState = LAUNCH_OFF;
    }
  } else if (launchState == LAUNCH_FIRED) {
    if (currentMillis - launchArmTimestamp > 2000) {
      launchState = LAUNCH_OFF;
    }
  }

  bool twoStepTriggered = virtualTwoStepActive || physicalLaunchActive || (launchState == LAUNCH_HOLDING);

  if (config.armed) {
    // Bench-test mode: If engine is stopped (0 RPM on the desk) and 2-step button is pressed or physical switch active, activate cut for hardware testing!
    if ((virtualTwoStepActive || physicalLaunchActive) && currentRpm == 0) {
      cutRequired = true;
      directClamp = true;
    }
    else if (currentRpm >= MIN_CUT_RPM) {
      // Condition A: Stationary Launch 2-Step (Show mode hold OR Hands-Free launch OR Physical switch)
      if (twoStepTriggered && currentRpm >= config.launchRpm) {
        cutRequired = true;
      }
      // Condition B: Main Redline Rev Limiter (Hard Cut at redline - 100% AUTOMATIC)
      else if (currentRpm >= config.redlineRpm) {
        cutRequired = true;
      }
      // Condition C: Overrun Decel Pops (Motorfék durrogás gázelvételkor - 100% AUTOMATIC)
      else if (config.decelPops) {
        // Track acceleration vs deceleration trends
        if (rpmDelta > 20) {
          lastRpmRiseMillis = currentMillis;
          decelDropFrames = 0;
        } else if (rpmDelta < -35) {
          decelDropFrames++;
        } else if (rpmDelta > 5) {
          if (decelDropFrames > 0) decelDropFrames--;
        }

        // Csak akkor aktiválódhat a durrogás, ha:
        // 1. A fordulat magasabb a küszöbnél (pl. 3200 RPM)
        // 2. Az elmúlt 250 ms-ban NEM gyorsult a motor (gyorsulási zár, kizárja a gyorsítás közbeni rángást)
        // 3. Legalább 4 egymást követő mintavételnél (160 ms) tartósan zuhan a fordulat (valós motorfék)
        if (currentRpm >= config.decelRpm && 
            (currentMillis - lastRpmRiseMillis > 250) && 
            decelDropFrames >= 4) {
          if (!decelBurstActive) {
            decelBurstActive = true;
            decelBurstStart = currentMillis;
          }
        }

        // Aktív motorfék durrogás ablak kezelése
        if (decelBurstActive) {
          // Megszakítási feltételek: újra gázra lépsz (rpmDelta > 15), lecsökken a fordulat (2100 alatt), vagy letelik az 1.2 mp
          if (rpmDelta > 15 || currentRpm < 2100 || (currentMillis - decelBurstStart > 1200)) {
            decelBurstActive = false;
            decelDropFrames = 0;
          } else {
            cutRequired = true;
          }
        }
      }
    }
    // Condition D: Ghost Cam / V8 Lumpy Idle Simulator (Only at idle 650-1250 RPM when enabled)
    else if (config.ghostCam && currentRpm >= 650 && currentRpm <= 1250 && !twoStepTriggered) {
      unsigned long nowMicros = micros();

      if (ghostCutInProgress) {
        // We cut 1 spark: clamp for ~1.2x of pulse period, then release so next cylinder fires!
        if (nowMicros - ghostCutStartMicros > (lastValidInterval * 12 / 10)) {
          ghostCutInProgress = false; // Released!
        } else {
          cutRequired = true;
          directClamp = true;
        }
      } else {
        // Start a new 1-spark cut every 5 pulses (rotating through all 4 cylinders)
        if ((ghostCamPulseCounter % 5 == 4) && (ghostCamPulseCounter != ghostCycleId) && (lastValidInterval > 0)) {
          ghostCutInProgress = true;
          ghostCutStartMicros = nowMicros;
          ghostCycleId = ghostCamPulseCounter; // Prevent re-triggering on same pulse
          cutRequired = true;
          directClamp = true;
        }
      }
    }
  } else {
    decelBurstActive = false;
    decelDropFrames = 0;
    ghostCutInProgress = false;
  }

  executeIgnitionCut(cutRequired, directClamp);

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
  if (wsConnected && (currentMillis - lastTelemetryMillis >= 33)) {
    lastTelemetryMillis = currentMillis;
    String telemetry = "T:" + String(currentRpm) + "," + String(isSparkCutActive ? "1" : "0") + "," + String((int)launchState);
    sendWsTextMessage(telemetry);
  }
}
