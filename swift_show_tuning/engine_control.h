#pragma once
// =========================================================================================
// ENGINE CONTROL CORE - Tach sensing, RPM estimation, spark-cut scheduling, launch control
// =========================================================================================
// Owns the hardware pins below. Everything else (web UI, WebSocket, NVS, OTA) talks to the
// engine ONLY through the functions declared in this header.
//
// Real-time design (v2):
//   * Tach ISR (GPIO18 FALLING) timestamps every ignition event and re-anchors a "slot clock".
//   * A hardware-timer ISR (every ENGINE_TICK_US) processes the ignition events that are
//     suppressed while the clamp is on (predicted from the last good period) and runs all
//     state machines every 2 ms. Nothing real-time depends on loop() timing any more:
//     engineUpdate() is a no-op kept for compatibility.
//   * Every cut/fire decision is made per ignition slot, and the clamp only switches right
//     after a real or predicted ignition event (IB low), never in the middle of a dwell.
//   * Every cut pattern contains fired slots; their tach pulses are fresh RPM measurements.
//     Intervals that span cut slots are divided by the slot count, so the RPM reading does not
//     collapse while cutting.
// The algorithm is mirrored 1:1 in tools/sim/engine_core.js (keep both in sync).
// =========================================================================================
#include <Arduino.h>

// ---- Hardware pins ----------------------------------------------------------------------
#define PIN_TACH_IN       18  // Tachometer pulse input from PC817 optocoupler (Interrupt)
#define PIN_SPARK_CUT     19  // Ignition clamp trigger to PC817 + 2N2222 transistor (Output)
#define PIN_STATUS_LED     2  // Onboard blue LED indicator (mirrors the clamp)
#define PIN_LAUNCH_SW     23  // Physical Clutch switch / Handbrake / Steering button input (Active LOW with internal PULLUP)

// Engine specs: 4-cylinder, 4-stroke G13BA = 2 ignition pulses per engine crankshaft revolution
#define PULSES_PER_REV     2

// Safety limits
#define MIN_CUT_RPM     2000  // Engine stall prevention: never cut spark below this RPM (except bench test / ghost cam)
#define HARD_MAX_RPM    8000  // Failsafe upper ceiling

// ---- Behaviour constants (for UI texts / countdown bars) --------------------------------
#define ENGINE_MAX_MEASURABLE_RPM 10000  // tach noise floor: edges closer than 3000 us are ignored
#define ENGINE_LAUNCH_ARM_MS      10000  // hands-free launch arming window; launchLeftMs counts down from this
#define ENGINE_LAUNCH_HOLD_MAX_MS 12000  // safety timeout for sitting on the launch limiter (independent of anti-flood)
#define ENGINE_LAUNCH_FIRED_MS     2000  // LAUNCH_FIRED is reported this long, then LAUNCH_OFF
#define ENGINE_SHOW_DEADMAN_MS      600  // engineSetShowButton(true) must be refreshed within this time
#define ENGINE_BENCH_STOP_MS       2000  // bench test needs this long without tach pulses and without cutting
#define ENGINE_BENCH_MAX_MS       10000  // one bench session lasts at most this long, then the button must be released
#define ENGINE_TICK_US              100  // control tick period (hardware timer ISR)
#define ENGINE_HW_TIMER               0  // hardware timer number used by the engine core (do not reuse)

// ---- Tuning parameters (persisted in NVS by the main sketch) ----------------------------
struct TuningConfig {
  bool  armed;              // Master system armed toggle
  int   launchRpm;          // 2-Step stationary launch limit (default 3800 RPM)
  int   redlineRpm;         // Main redline rev limiter (default 6200 RPM)
  bool  decelPops;          // Overrun decel pop mode enabled
  int   decelRpm;           // Minimum RPM for decel pops to trigger (default 3200 RPM)
  int   cutPattern;         // Per ignition slot: 0 = Hard Cut, 1 = Flames (cut 3 / fire 1),
                            // 2 = Gunfire (cut 2 / fire 1), 3 = AK-47 (cut 1 / fire 1), 4 = Cannon
  float maxCutSeconds;      // Anti-flood: max continuous 100% cut (no fired slot), s (default 3.0, 0.0 = unlimited)
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

// Why the last launch sequence ended (EngineTelemetry::launchEnd). Informational only.
enum LaunchEnd : uint8_t {
  LAUNCH_END_NONE         = 0,  // nothing finished yet / a sequence is running
  LAUNCH_END_FIRED        = 1,  // clutch drop detected -> FIRED
  LAUNCH_END_LIFT         = 2,  // rpm dropped and kept falling = throttle lifted before a clutch drop -> OFF
                                //   (the limiter is released at once; re-arm to try again)
  LAUNCH_END_HOLD_TIMEOUT = 3,  // held on the limiter longer than ENGINE_LAUNCH_HOLD_MAX_MS -> OFF
  LAUNCH_END_ARM_TIMEOUT  = 4,  // armed but launchRpm not reached within ENGINE_LAUNCH_ARM_MS -> OFF
  LAUNCH_END_CANCEL       = 5,  // engineLaunchDisarm() or master arm switched off
  LAUNCH_END_STALL        = 6   // engine stopped while holding
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
  CUT_BENCH      = 7,  // Bench test (engine stopped, show button held)
  CUT_FLOOD_LOCK = 8   // Anti-flood timeout tripped: cut requested but blocked
};

struct EngineTelemetry {
  int      rpm;           // engine RPM (model estimate inside cut gaps; 0 only when the engine is stopped)
  bool     cutActive;     // clamp is cutting sparks (stays true 60 ms after the last cut slot)
  uint8_t  launchState;   // LaunchState
  uint32_t launchLeftMs;  // remaining ARMED window in ms (0 when not armed)
  uint8_t  reason;        // CutReason of the active cut (CUT_NONE when not cutting)
  // ---- v2 additions (appended; older code may ignore them) ----
  bool     showActive;    // show button effective (after the dead-man)
  bool     switchActive;  // physical switch on GPIO 23 active (debounced 20 ms)
  bool     rpmEstimated;  // rpm is a model estimate (cut gap, no fresh tach measurement yet)
  uint16_t measAgeMs;     // age of the last real RPM measurement (saturates at 60000)
  uint8_t  launchEnd;     // LaunchEnd: why the last launch sequence ended
};

// Counters for real-car diagnostics (monotonic, wrap at 2^32)
struct EngineDiag {
  uint32_t pulses;         // accepted tach pulses
  uint32_t rejected;       // tach edges rejected by the noise gate (pin level, ringing, early edge, while clamped)
  uint32_t discarded;      // intervals that could not be measured (gap too long / slot count unknown)
  uint32_t outliers;       // implausible intervals held back (missed / extra edge, split slot)
  uint32_t unsyncs;        // slot clock dropped (expected pulse missing, late tick, over-long cut gap)
  uint32_t cutSlots;       // ignition slots suppressed by the scheduler
  uint32_t floodTrips;     // anti-flood lock activations
  uint32_t dwellBlocks;    // clamp engages refused because GPIO18 read HIGH (igniter dwelling)
  int32_t  launchDropRate; // rpm/s decline measured at the last launch release (tuning aid)
};

// ---- API --------------------------------------------------------------------------------
void engineBegin(const TuningConfig& cfg);  // configures pins, tach ISR and control timer, starts control
void engineSetConfig(const TuningConfig& cfg);
TuningConfig engineGetConfig();

// Show-mode 2-step button. `held=true` must be refreshed at least every 600 ms (dead-man):
// the engine releases the button by itself if the refresh stops (e.g. phone lost Wi-Fi).
void engineSetShowButton(bool held);
void engineLaunchArm();     // ignored while the master arm is off or while already HOLDING
void engineLaunchDisarm();

// true = never cut spark (firmware flashing etc.). Takes effect immediately, from any context.
void engineSetInhibit(bool inhibit);

// Call every loop() iteration. No-op: control runs in the tach ISR and a hardware-timer ISR.
void engineUpdate();

EngineTelemetry engineGetTelemetry();
int engineGetRpm();
EngineDiag engineGetDiag();
