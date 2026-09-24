#pragma once
// =========================================================================================
// ENGINE CONTROL CORE - Tach sensing, RPM estimation, spark-cut scheduling, launch control
// =========================================================================================
// Owns the hardware pins below. Everything else (web UI, WebSocket, NVS, OTA) talks to the
// engine ONLY through the functions declared in this header.
// =========================================================================================
#include <Arduino.h>

// ---- Hardware pins ----------------------------------------------------------------------
#define PIN_TACH_IN       18  // Tachometer pulse input from PC817 optocoupler (Interrupt)
#define PIN_SPARK_CUT     19  // Ignition clamp trigger to PC817 + 2N2222 transistor (Output)
#define PIN_STATUS_LED     2  // Onboard blue LED indicator
#define PIN_LAUNCH_SW     23  // Physical Clutch switch / Handbrake / Steering button input (Active LOW with internal PULLUP)

// Engine specs: 4-cylinder, 4-stroke G13BA = 2 ignition pulses per engine crankshaft revolution
#define PULSES_PER_REV     2

// Safety limits
#define MIN_CUT_RPM     2000  // Engine stall prevention: Never cut spark below this RPM
#define HARD_MAX_RPM    8000  // Failsafe upper ceiling

// ---- Tuning parameters (persisted in NVS by the main sketch) ----------------------------
struct TuningConfig {
  bool  armed;              // Master system armed toggle
  int   launchRpm;          // 2-Step stationary launch limit (default 3800 RPM)
  int   redlineRpm;         // Main redline rev limiter (default 6200 RPM)
  bool  decelPops;          // Overrun decel pop mode enabled
  int   decelRpm;           // Minimum RPM for decel pops to trigger (default 3200 RPM)
  int   cutPattern;         // 0 = Hard Cut, 1 = Flame Spitter, 2 = Gunfire Crackle, 3 = AK-47, 4 = Cannon
  float maxCutSeconds;      // Anti-flood safety limit (default 3.0s, 0.0 = unlimited)
  bool  ghostCam;           // Ghost Cam / Lumpy V8 idle simulation
  int   launchDrop;         // Hands-free launch: RPM drop below launchRpm treated as clutch release
};

// Factory defaults
TuningConfig engineDefaultConfig();
// Forces every field into its valid range (call on anything received from the network / NVS)
void engineClampConfig(TuningConfig& cfg);

// ---- Runtime state reported to the UI ---------------------------------------------------
enum LaunchState : uint8_t {
  LAUNCH_OFF     = 0,  // Normal driving
  LAUNCH_ARMED   = 1,  // Ready for throttle (arming window running)
  LAUNCH_HOLDING = 2,  // On limiter at launchRpm (cutting spark)
  LAUNCH_FIRED   = 3   // Clutch drop detected, launched!
};

// Which feature is currently requesting the spark cut (highest priority wins)
enum CutReason : uint8_t {
  CUT_NONE       = 0,
  CUT_SHOW       = 1,  // Show-mode button held in the web UI
  CUT_LAUNCH     = 2,  // Hands-free launch control holding
  CUT_SWITCH     = 3,  // Physical switch on GPIO 23
  CUT_REDLINE    = 4,  // Redline rev limiter
  CUT_DECEL      = 5,  // Overrun decel pops
  CUT_GHOST      = 6,  // Ghost cam idle
  CUT_BENCH      = 7,  // Bench test (engine stopped, button held)
  CUT_FLOOD_LOCK = 8   // Anti-flood timeout tripped: cut requested but blocked
};

struct EngineTelemetry {
  int      rpm;
  bool     cutActive;     // true while a spark-cut request is being applied
  uint8_t  launchState;   // LaunchState
  uint32_t launchLeftMs;  // remaining ARMED window in ms (0 when not armed)
  uint8_t  reason;        // CutReason
};

// ---- API --------------------------------------------------------------------------------
void engineBegin(const TuningConfig& cfg);  // configures pins + tach ISR, starts control
void engineSetConfig(const TuningConfig& cfg);
TuningConfig engineGetConfig();

// Show-mode 2-step button. `held=true` must be refreshed at least every 600 ms (dead-man):
// the engine releases the button by itself if the refresh stops (e.g. phone lost Wi-Fi).
void engineSetShowButton(bool held);
void engineLaunchArm();
void engineLaunchDisarm();

// true = never cut spark (firmware flashing etc.). Takes effect immediately.
void engineSetInhibit(bool inhibit);

// Call every loop() iteration. May be a no-op if control runs in its own task/timer.
void engineUpdate();

EngineTelemetry engineGetTelemetry();
int engineGetRpm();
