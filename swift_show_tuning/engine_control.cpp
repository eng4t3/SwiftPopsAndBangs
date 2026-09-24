// =========================================================================================
// ENGINE CONTROL CORE - see engine_control.h
// =========================================================================================
// KEEP IN SYNC WITH tools/sim/engine_core.js. That file is a line-by-line JavaScript mirror of
// the control algorithm below (same constants, same state machines, same integer math). Any
// change here must be made there too, and `node tools/sim/run.js` must still pass.
//
// ---- Hardware model --------------------------------------------------------------------
// Tach (GPIO18): coil NEGATIVE (igniter output) -> 1 kOhm -> PC817 LED, open collector with the
//   internal pull-up. Igniter not dwelling: coil(-) ~12 V, LED on, GPIO18 LOW. Dwell: coil(-)
//   ~1 V, LED off, GPIO18 rises slowly (weak pull-up). Spark: coil(-) jumps up (+ flyback ringing
//   for 1-2 ms) -> the FALLING edge marks the spark, ringing can add edges within ~2 ms.
//   While we clamp IB the igniter never dwells: GPIO18 stays LOW, no edges at all.
// Clamp (GPIO19 HIGH): pulls the ECU->igniter IB line low. Clamping during a dwell aborts it and
//   fires a PREMATURE spark (wrong crank angle) - this must never happen. Releasing mid-dwell
//   only gives a shorter dwell (weaker spark at the right angle).
//
// ---- Execution model -------------------------------------------------------------------
// * tachIsr (GPIO18 FALLING): noise gate, RPM measurement, anchors the slot clock and decides
//   the next ignition slot. Engaging the clamp happens ONLY here, a few us after a real spark
//   (IB low, next dwell not started), and only if GPIO18 still reads LOW (= igniter not dwelling).
// * tickIsr (hardware timer, every 100 us): processes the predicted ignition events of cut slots
//   (they produce no tach pulse), releases the clamp right after them, handles timeouts, and every
//   2 ms runs the slow step (launch / decel / ghost / bench state machines, dead-man, telemetry).
// * Nothing depends on loop(): engineUpdate() is a no-op.
// * Both ISRs are allocated on the core that calls engineBegin() (core 1, loop task), level 1,
//   WITHOUT ESP_INTR_FLAG_IRAM. While a flash write/erase has the cache disabled (NVS, OTA) IDF
//   masks such interrupts, so they can never execute from flash with the cache off (no crash);
//   they are only postponed. Consequences, handled below: a postponed tick finds its predicted
//   event overdue by more than half a slot -> it releases the clamp and drops the slot clock; a
//   postponed tach edge carries a late timestamp -> rejected by the pin-level check or treated
//   as an outlier interval. The glue only writes NVS while no cut is active, and OTA inhibits
//   the engine, so the clamp is low during flash writes anyway. The ISR code is IRAM_ATTR for
//   latency (no cache misses), not because it has to run with the cache disabled.
// * All shared state is only touched inside `engMux` critical sections (ISRs and API calls).
//   No float, no heap, no Serial/String in any ISR path.
// =========================================================================================
#include "engine_control.h"
#include <soc/gpio_struct.h>
#include <esp_timer.h>
#include <esp_private/panic_internal.h>   // panic_info_t (for the panic-handler wrap below)

// =========================================================================================
// CONSTANTS
// =========================================================================================
static const uint32_t TICK_US            = ENGINE_TICK_US;
static const uint32_t SLOW_EVERY         = 20;       // slow step every 20 ticks
static const uint16_t SLOW_MS            = 2;        // = TICK_US * SLOW_EVERY / 1000
// ---- tach / RPM estimation ----
static const uint32_t RPM_CONST          = 60000000UL / PULSES_PER_REV;           // rpm = RPM_CONST / slot period [us]
static const uint32_t MIN_PERIOD_US      = RPM_CONST / ENGINE_MAX_MEASURABLE_RPM; // 3000 us: absolute noise floor (ringing)
static const uint32_t MAX_PERIOD_US      = 200000;   // 150 rpm: slower intervals are not measurements
static const uint32_t BLANK_PCT          = 45;       // noise gate: ignore edges earlier than 45 % of a slot after the
                                                     // last event. < 50 % on purpose: a lock onto a multiple of the
                                                     // period cannot persist (the real pulses always pass).
static const uint32_t BLANK_MAX_PERIOD   = 60000;    // 500 rpm: no relative gate below (cranking)
static const uint32_t ONTIME_PCT         = 95;       // a new cut sequence may only start on a pulse arriving >= 95 % of a slot
static const uint32_t COMP_TOL_PCT       = 30;       // pulse after cut slots must land within +-30 % of its predicted slot
static const uint32_t MAX_COMP_SLOTS     = 10;       // longer gaps are not measured (slot clock drift)
static const uint32_t JUMP_TOL_PCT       = 30;       // a slot period differing more than this from the estimate is a candidate
static const uint32_t CAND_TOL_PCT       = 20;       // ... adopted when the next interval agrees with it within 20 %
static const uint8_t  CAND_STALE_RUN     = 3;        // this many unconfirmed candidates in a row: estimate stale, drop it
static const uint32_t SPLIT_TOL_PCT      = 15;       // two intervals summing to 1 (or 2) slots within 15 % = a split slot
static const uint32_t AVG_SLOTS          = 2;        // estimate = newest intervals covering >= 2 slots
static const uint8_t  RING_LEN           = 4;
static const uint32_t PRED_MARGIN_DIV    = 8;        // predicted event processed T/8 after the predicted spark
static const uint32_t PRED_MARGIN_MIN_US = 200;
static const uint32_t FIRE_TIMEOUT_PCT   = 160;      // fired slot without pulse for 1.6 T: drop the slot clock, stop cutting
static const uint32_t MAX_CUT_GAP_US     = 1700000;  // never keep the clamp on longer than this without a real pulse
static const int32_t  MODEL_DECEL_BASE   = 1000;     // assumed decel with all sparks cut: 1000 + 0.45 * rpm [rpm/s]
static const int32_t  MODEL_DECEL_PCT    = 45;
static const int32_t  MODEL_MIN_RPM      = 300;
static const uint16_t STOP_NO_PULSE_MS   = 250;
static const uint16_t STOP_HARD_MS       = 1800;
static const uint16_t SAT_MS             = 60000;
// ---- limiter / patterns ----
static const int32_t  LIMIT_HYST         = 100;      // limiter engages at >= limit, releases below limit - 100
static const int32_t  LIMIT_ESCALATE     = 75;       // pattern cannot hold (rpm >= limit + 75): hard cut
static const uint32_t HARD_MIN_CUT       = 2;
static const uint32_t HARD_MAX_CUT       = 5;        // longer runs make the slot count ambiguous (clock drift)
static const uint16_t CLEAN_MAX_MS       = 150;      // no 1-2 slot (unambiguous) measurement for this long: fire 2 in a row
static const uint32_t CANNON_CUT_MS      = 1600;
static const int32_t  CANNON_RESTORE_RPM = MIN_CUT_RPM + 500;
static const uint32_t CANNON_FIRE_MIN_MS = 250;
static const uint32_t FLOOD_LOCK_MS      = 1000;
static const uint16_t CUT_HOLD_MS        = 60;       // telemetry: cutActive stays true this long after the clamp opens
// ---- hands-free launch ----
static const int32_t  LAUNCH_ENTER_BELOW = 200;      // ARMED -> HOLDING at launchRpm - 200
static const int32_t  LAUNCH_BAND        = 250;      // "on the limiter" band below the reference (covers the limiter ripple)
static const uint32_t LAUNCH_CONFIRM_MS  = 60;       // the decline must have lasted >= 60 ms ...
static const uint32_t LAUNCH_CONFIRM_N   = 3;        // ... and >= 3 consecutive fresh measurements below the threshold
static const int32_t  LAUNCH_CLUTCH_RATE = 3500;     // faster decline [rpm/s] = clutch for sure (a lift cannot be this fast)
static const uint32_t LAUNCH_VERIFY_MS   = 800;      // slower decline: clutch if the rpm recovers within this time ...
static const int32_t  LAUNCH_VERIFY_RISE = 100;      // ... by this much above its minimum (car moving)
static const int32_t  LAUNCH_LIFT_EXTRA  = 800;      // ... lift if it keeps falling this far below the threshold
// ---- decel pops ----
static const uint8_t  DECEL_FRAME_STEPS  = 10;       // 20 ms frames
static const uint8_t  DECEL_HIST         = 8;
static const uint32_t DECEL_SLOPE_MIN_MS = 40;       // slope needs measurements >= 40 ms apart (normalised to 80 ms)
static const int32_t  DECEL_DROP_80      = 60;       // decline >= 60 rpm / 80 ms (750 rpm/s) = decel frame
static const int32_t  DECEL_RISE_80      = 30;
static const int32_t  DECEL_STEADY_80    = 20;
static const uint32_t DECEL_TRIG_FRAMES  = 4;
static const uint8_t  DECEL_REARM_FRAMES = 5;
static const uint32_t DECEL_NO_RISE_MS   = 250;
static const uint32_t DECEL_BURST_MS     = 1200;
static const int32_t  DECEL_ABORT_RPM    = 2100;
static const int32_t  DECEL_ABORT_RISE   = 60;
static const int32_t  DECEL_REACCEL_PCT  = 70;       // burst decline shallower than 70 % of the trigger decline = throttle reopened
static const uint32_t DECEL_REACCEL_AFTER_MS = 80;   // (checked once the 80 ms slope window lies inside the burst)
static const uint32_t DECEL_REACCEL_FRAMES = 2;      // ... on this many consecutive fresh frames
static const uint32_t DECEL_CLEAN_MASK   = 0x3F;     // no clamp activity in the last 6 frames (120 ms)
static const uint16_t DECEL_QUIET_MS     = 3000;     // no decel pops within 3 s after a 2-step / launch (clutch = load)
static const int32_t  DECEL_LOAD_FACTOR  = 2;        // burst aborts when rpm falls > 2x faster than free-rev friction
// ---- ghost cam ----
static const int32_t  GHOST_MIN_RPM      = 650;
static const int32_t  GHOST_MAX_RPM      = 1250;
static const uint32_t GHOST_EVERY        = 5;        // cut 1 ignition event of every 5 (rotates through the cylinders)
static const int32_t  GHOST_DIP_DEV      = 150;      // below the slow average (our own dips are ~70 rpm)
static const int32_t  GHOST_RISE_DEV     = 60;       // above the slow average = rising / throttle / pulling away
static const uint32_t GHOST_SETTLE_MS    = 1000;
static const uint16_t GHOST_MAX_AGE_MS   = 100;
// ---- inputs ----
static const uint16_t BENCH_STOP_MS      = ENGINE_BENCH_STOP_MS;
static const uint32_t BENCH_MAX_MS       = ENGINE_BENCH_MAX_MS;   // one bench session at most 10 s, then release the button
static const uint32_t BENCH_STOP_GAP_US  = 75000;    // last tach interval >= 75 ms (< 400 rpm): the engine really stopped
static const uint8_t  SW_DEBOUNCE_STEPS  = 10;       // 20 ms

enum : uint8_t { SEQ_NONE = 0, SEQ_CYCLE = 1, SEQ_CANNON_CUT = 2, SEQ_CANNON_FIRE = 3 };

// =========================================================================================
// STATE (touched only inside engMux critical sections)
// =========================================================================================
static portMUX_TYPE engMux = portMUX_INITIALIZER_UNLOCKED;
static hw_timer_t*  ctlTimer = nullptr;
static TuningConfig config;                 // clamped copy for engineGetConfig() (task side, float allowed)

// runtime config (integers only)
static bool     rtArmed = false, rtDecelPops = true, rtGhost = false;
static int32_t  rtLaunchRpm = 3800, rtRedline = 6200, rtDecelRpm = 3200, rtLaunchDrop = 400;
static uint8_t  rtPattern = 1;
static uint32_t rtFloodMs = 3000;

// external inputs
static bool     inhibitCut = false, showHeld = false;
static uint32_t showRefreshMs = 0;

// tach / estimator
static bool     havePrev = false, synced = false;
static uint32_t lastRealUs = 0, lastEventUs = 0, slotsSinceReal = 0, slotPeriodUs = 0, periodUs = 0;
static int32_t  rpmMeas = 0, rpmSlow = 0;
static uint32_t measSeq = 0, measCenterMs = 0;      // measCenterMs: msClock at the middle of the estimate's window
static uint32_t ringDt[RING_LEN];
static uint8_t  ringN[RING_LEN];
static uint8_t  ringLen = 0, ringHead = 0;
static uint32_t measWinUs = 0;              // time span of the intervals behind the current estimate
static bool     edgeFlag = false, pulseFlag = false, cleanFlag = false;
static uint32_t candDt = 0, candN = 0, candPer = 0, splitDt = 0;
static uint8_t  candRun = 0;

// clamp
static bool     clampOn = false, clampLatch = false, cutGap = false;
static uint32_t clampOnSinceUs = 0, nextEventUs = 0;
static uint8_t  lastCutReason = CUT_NONE;

// sequencer
static uint8_t  seqMode = SEQ_NONE, seqReason = CUT_NONE;
static uint32_t seqCutLeft = 0;
static bool     seqNeedFire = false;
static int32_t  seqLimit = 0;
static uint32_t seqStartUs = 0, seqFireUntilMs = 0;
static bool     limEngaged = false;
static uint32_t ghostCount = 0;
static bool     floodLock = false;
static uint32_t floodLockUntilMs = 0;

// slow step
static uint32_t slowDiv = 0, msClock = 0;
static uint16_t noEdgeMs = SAT_MS, noPulseMs = SAT_MS, clampOffMs = SAT_MS, noCutMs = SAT_MS;
static uint16_t measAgeMs = SAT_MS, cleanAgeMs = SAT_MS;
static uint32_t ageSeq = 0;
static bool     stopped = true, showActive = false, swStable = false, benchActive = false;
static uint32_t benchStartMs = 0, lastGapUs = 0;
static bool     benchNeedRelease = false, everPulsed = false;
static int32_t  rpmDisp = 0;
static uint8_t  swCnt = 0;
static int32_t  reqLimit = 6200;
static uint8_t  reqLimitReason = CUT_REDLINE;
static bool     reqBurst = false, reqGhost = false;
// launch
static uint8_t  launchState = LAUNCH_OFF, launchEnd = LAUNCH_END_NONE;
static uint32_t armMs = 0, holdMs = 0, firedMs = 0, lastAtLimitMs = 0;
static int32_t  holdRef = 0, lastAtLimitRpm = 0;
static bool     dropping = false;
static uint32_t dropN = 0, launchSeenSeq = 0;
static bool     verifyLift = false;
static uint32_t verifyStartMs = 0;
static int32_t  verifyMin = 0, verifyFloor = 0;
// decel
static uint8_t  frameDiv = 0;
static int32_t  hist[DECEL_HIST];
static uint32_t histT[DECEL_HIST];
static uint8_t  histIdx = 0, histN = 0;
static uint32_t clampHist = 0, frameSeq = 0;
static bool     frameClamp = false;
static uint32_t decelFrames = 0;
static uint8_t  steadyFrames = 0;
static bool     decelArmed = false;
static uint32_t lastRiseMs = 0;
static bool     burstActive = false;
static uint32_t burstStartMs = 0, burstSlowFrames = 0, burstRiseFrames = 0;
static int32_t  burstMin = 0, burstRate0 = 0;
static uint16_t decelQuietMs = 0;
// ghost
static uint32_t ghostUnstableMs = 0;
// telemetry / diagnostics
static EngineTelemetry tel = {0, false, LAUNCH_OFF, 0, CUT_NONE, false, false, false, SAT_MS, LAUNCH_END_NONE};
static EngineDiag diag = {0, 0, 0, 0, 0, 0, 0, 0, 0};

// =========================================================================================
// HARDWARE ACCESS (direct registers: ISR-safe, a few ns)
// =========================================================================================
static inline void IRAM_ATTR hwClamp(bool on) {
  if (on) GPIO.out_w1ts = (1UL << PIN_SPARK_CUT) | (1UL << PIN_STATUS_LED);
  else    GPIO.out_w1tc = (1UL << PIN_SPARK_CUT) | (1UL << PIN_STATUS_LED);
}
static inline bool IRAM_ATTR hwSwitchActive() { return ((GPIO.in >> PIN_LAUNCH_SW) & 1U) == 0; }
static inline bool IRAM_ATTR hwTachLow()      { return ((GPIO.in >> PIN_TACH_IN) & 1U) == 0; }
static inline uint32_t IRAM_ATTR nowUs()      { return (uint32_t)esp_timer_get_time(); }

// =========================================================================================
// HELPERS
// =========================================================================================
static inline uint32_t IRAM_ATTR predMargin(uint32_t T) {
  uint32_t m = T / PRED_MARGIN_DIV;
  return m < PRED_MARGIN_MIN_US ? PRED_MARGIN_MIN_US : m;
}
static inline uint32_t IRAM_ATTR absDiff(uint32_t a, uint32_t b) {
  int32_t d = (int32_t)(a - b);
  return (uint32_t)(d < 0 ? -d : d);
}

static void IRAM_ATTR clampSet(bool on, uint32_t now) {
  if (on) {
    if (inhibitCut) return;
    if (!clampOn) {
      clampOn = true; clampOnSinceUs = now; hwClamp(true);
      lastCutReason = benchActive ? CUT_BENCH : seqReason;
    }
    clampLatch = true; cutGap = true;
  } else if (clampOn) {
    clampOn = false; hwClamp(false);
  }
}

// RPM estimate inside a cut gap: last measurement minus the free-rev decel since then.
static int32_t IRAM_ATTR modelRpmAt(uint32_t t) {
  if (!cutGap || rpmMeas <= 0) return rpmMeas;
  int32_t d = (int32_t)(t - lastRealUs);
  uint32_t elMs = d > 0 ? (uint32_t)d / 1000U : 0;
  if (elMs > 5000) elMs = 5000;
  uint32_t decel = (uint32_t)(MODEL_DECEL_BASE + rpmMeas * MODEL_DECEL_PCT / 100);
  int32_t r = rpmMeas - (int32_t)(elMs * decel / 1000U);
  return r < MODEL_MIN_RPM ? MODEL_MIN_RPM : r;
}

static inline void IRAM_ATTR ringFlush() { ringLen = 0; ringHead = 0; }
static void IRAM_ATTR ringPush(uint32_t dt, uint32_t n) {
  ringDt[ringHead] = dt; ringN[ringHead] = (uint8_t)n;
  ringHead = (uint8_t)((ringHead + 1) % RING_LEN);
  if (ringLen < RING_LEN) ringLen++;
  uint32_t sumDt = 0, sumN = 0;
  uint8_t idx = ringHead;
  for (uint8_t i = 0; i < ringLen && sumN < AVG_SLOTS; i++) {
    idx = (uint8_t)((idx + RING_LEN - 1) % RING_LEN);
    sumDt += ringDt[idx]; sumN += ringN[idx];
  }
  periodUs = sumDt / sumN;
  measWinUs = sumDt;
}

static inline void IRAM_ATTR seqReset() { seqMode = SEQ_NONE; seqCutLeft = 0; seqNeedFire = false; }

// Anti-flood: the continuous 100 % cut (no fired slot) must not exceed maxCutSeconds.
static bool IRAM_ATTR floodCheck(uint32_t now) {
  if (rtFloodMs == 0) return true;
  uint32_t cont = clampOn ? (uint32_t)(now - clampOnSinceUs) : 0;
  if (cont + slotPeriodUs > rtFloodMs * 1000U) {
    floodLock = true; floodLockUntilMs = msClock + FLOOD_LOCK_MS; diag.floodTrips++;
    seqReset();
    return false;
  }
  return true;
}

// Modelled rpm loss over one cut slot (free-rev decel x slot period).
static int32_t IRAM_ATTR slotDrop(int32_t r) {
  uint32_t decel = (uint32_t)(MODEL_DECEL_BASE + r * MODEL_DECEL_PCT / 100);
  return (int32_t)(decel * (slotPeriodUs / 10U) / 100000U);
}

// MIN_CUT_RPM interlock: even if the rpm has been falling at the free-rev rate since the middle of
// the measurement window, it must stay >= MIN_CUT_RPM until the end of the next slot.
static bool IRAM_ATTR minCutOk(int32_t r) {
  uint32_t decel = (uint32_t)(MODEL_DECEL_BASE + r * MODEL_DECEL_PCT / 100);
  uint32_t t10 = (measWinUs / 2U + slotPeriodUs) / 10U;
  return r - (int32_t)(decel * t10 / 100000U) >= MIN_CUT_RPM;
}

// Hard cut: cut enough slots to fall to (limit - hysteresis), then one fired slot to measure.
static uint32_t IRAM_ATTR hardCutSlots(int32_t r, int32_t lim, bool isLimiter) {
  if (!isLimiter) return HARD_MAX_CUT;
  int32_t perSlot = slotDrop(r);
  if (perSlot < 1) perSlot = 1;
  int32_t excess = r - (lim - LIMIT_HYST);
  int32_t k = (excess + perSlot - 1) / perSlot;
  if (k < (int32_t)HARD_MIN_CUT) k = HARD_MIN_CUT;
  if (k > (int32_t)HARD_MAX_CUT) k = HARD_MAX_CUT;
  return (uint32_t)k;
}

static bool IRAM_ATTR startSeq(uint8_t reason, int32_t r, int32_t lim, bool isLimiter, uint32_t now) {
  seqReason = reason; seqLimit = lim;
  bool escalate = isLimiter && r >= lim + LIMIT_ESCALATE;
  uint8_t p = rtPattern;
  // Decel pops always use a pattern with fired slots (flames for hard / cannon): their tach pulses
  // are what lets a re-acceleration abort the burst within ~200 ms.
  if (reason == CUT_DECEL && (p == 0 || p == 4)) p = 1;
  // Cannon (continuous cut) only for the show button / physical switch 2-step. Redline and
  // hands-free launch use hard cut (a silent phase hides a clutch drop, a second of full cut in
  // gear at the redline feels like a stall).
  if (p == 4 && !escalate && !floodLock && (reason == CUT_SHOW || reason == CUT_SWITCH)) {
    seqMode = SEQ_CANNON_CUT; seqStartUs = now;
    return floodCheck(now);
  }
  uint32_t nCut;
  if (p == 0 || p == 4 || escalate) nCut = hardCutSlots(r, lim, isLimiter);
  else if (p == 1) nCut = 3;      // Flames:  cut 3 / fire 1
  else if (p == 2) nCut = 2;      // Gunfire: cut 2 / fire 1
  else nCut = 1;                  // AK-47:   cut 1 / fire 1
  seqMode = SEQ_CYCLE; seqCutLeft = nCut - 1; seqNeedFire = (seqCutLeft == 0);
  return floodCheck(now);
}

static bool IRAM_ATTR seqStillWanted() {
  if (seqReason == CUT_DECEL) return reqBurst;
  if (seqReason == CUT_GHOST) return reqGhost;
  return reqLimit <= seqLimit;
}

// Decide the NEXT ignition slot at a slot boundary (real pulse or predicted event).
// fresh: a new RPM measurement came with this pulse; onTime: the pulse is not early (noise guard);
// atReal: called from a real tach pulse (not from a predicted event).
static bool IRAM_ATTR decideNext(uint32_t now, bool fresh, bool onTime, bool atReal) {
  if (inhibitCut || !rtArmed || benchActive) { seqReset(); limEngaged = false; return false; }
  int32_t r = fresh ? rpmMeas : modelRpmAt(now);
  if (fresh) {
    if (r < MIN_CUT_RPM) limEngaged = false;
    else if (r >= reqLimit) limEngaged = true;
    else if (r < reqLimit - LIMIT_HYST) limEngaged = false;
  }

  if (seqMode == SEQ_CYCLE) {
    if (seqCutLeft > 0) {
      if (!seqStillWanted() || (seqReason != CUT_GHOST && !minCutOk(r))) { seqReset(); return false; }
      seqCutLeft--;
      if (seqCutLeft == 0) seqNeedFire = true;
      return floodCheck(now);
    }
    seqReset();          // the cycle's fired (measurement) slot
    return false;
  } else if (seqMode == SEQ_CANNON_CUT) {
    if (!seqStillWanted() || (uint32_t)(now - seqStartUs) >= CANNON_CUT_MS * 1000U || r <= CANNON_RESTORE_RPM) {
      seqMode = SEQ_CANNON_FIRE; seqFireUntilMs = msClock + CANNON_FIRE_MIN_MS; limEngaged = false;
      return false;
    }
    return floodCheck(now);
  } else if (seqMode == SEQ_CANNON_FIRE) {
    if ((int32_t)(msClock - seqFireUntilMs) < 0) {
      if (!(fresh && r >= reqLimit + LIMIT_ESCALATE)) return false;   // the bang; only a runaway escalates
    }
    seqMode = SEQ_NONE;
  }

  if (atReal && reqGhost && ghostCount < GHOST_EVERY - 1) ghostCount++;   // fired slots since the last ghost cut
  if (!fresh || !onTime) return false;
  if (cleanAgeMs > CLEAN_MAX_MS && !cleanFlag) { cleanAgeMs = 0; return false; }   // one extra fired slot per 150 ms
  if (minCutOk(r)) {
    if (limEngaged) return startSeq(reqLimitReason, r, reqLimit, true, now);
    if (reqBurst) return startSeq(CUT_DECEL, r, 0, false, now);
  }
  if (reqGhost) {
    if (ghostCount >= GHOST_EVERY - 1) {
      ghostCount = 0;
      seqReason = CUT_GHOST; seqLimit = 0; seqMode = SEQ_CYCLE; seqCutLeft = 0; seqNeedFire = true;
      return true;
    }
  }
  return false;
}

// =========================================================================================
// TACH EDGE (GPIO18 FALLING)
// =========================================================================================
static void IRAM_ATTR onTachEdge(uint32_t now, bool pinLow) {
  edgeFlag = true;
  if (benchActive) { benchActive = false; clampSet(false, now); }   // bench test ends on ANY edge
  if (!pinLow) { diag.rejected++; return; }                          // glitch on the slow rising (dwell) edge
  if (havePrev) {
    uint32_t dtRaw = now - lastRealUs;
    if (dtRaw < MIN_PERIOD_US) { diag.rejected++; return; }          // coil ringing (< 3 ms)
    if (clampOn) { diag.rejected++; return; }                        // no dwell while clamped -> cannot be a spark
    // Adaptive noise gate, relative to the shorter of the estimate and a pending candidate, so a stale
    // estimate during a fast rev-up can never reject real pulses.
    uint32_t gateT = (candPer != 0 && candPer < slotPeriodUs) ? candPer : slotPeriodUs;
    if (synced && gateT < BLANK_MAX_PERIOD && (uint32_t)(now - lastEventUs) < gateT * BLANK_PCT / 100U) {
      diag.rejected++; return;
    }
  }

  pulseFlag = true; diag.pulses++;
  bool fresh = false;
  if (havePrev) {
    uint32_t dt = now - lastRealUs;
    lastGapUs = dt; everPulsed = true;
    uint32_t n = 0;
    if (slotsSinceReal == 0) {
      n = 1;                                              // consecutive fired slots
    } else if (synced && slotsSinceReal + 1 <= MAX_COMP_SLOTS &&
               absDiff(now, lastEventUs + slotPeriodUs) <= slotPeriodUs * COMP_TOL_PCT / 100U) {
      n = slotsSinceReal + 1;                             // fired slot after n-1 cut slots
    }
    uint32_t per = n > 0 ? dt / n : 0;
    if (n > 0 && per >= MIN_PERIOD_US && per <= MAX_PERIOD_US) {
      // Plausibility: the engine cannot change speed by > 30 % within one slot.
      //  * within 30 % of the estimate               -> measurement
      //  * short interval + next interval = 1-2 slots -> a noise edge split a slot: merge them
      //  * otherwise the interval is only a candidate; it is adopted when the next interval agrees
      //    with it (missed edge, stale estimate). Single outliers never reach the estimate, and a
      //    lock onto a multiple / fraction of the true period cannot build up.
      uint32_t accDt = 0, accN = 0;
      if (ringLen > 0 && absDiff(per, periodUs) <= periodUs * JUMP_TOL_PCT / 100U) {
        accDt = dt; accN = n;
      } else if (splitDt != 0 && n == 1 && ringLen > 0 && absDiff(splitDt + dt, periodUs) <= periodUs * SPLIT_TOL_PCT / 100U) {
        accDt = splitDt + dt; accN = 1; diag.outliers++;
      } else if (splitDt != 0 && n == 1 && ringLen > 0 && absDiff(splitDt + dt, 2U * periodUs) <= periodUs * SPLIT_TOL_PCT / 100U) {
        accDt = splitDt + dt; accN = 2; diag.outliers++;
      } else if (candPer != 0 && absDiff(per, candPer) <= candPer * CAND_TOL_PCT / 100U) {
        ringFlush(); ringPush(candDt, candN); accDt = dt; accN = n;
      } else {
        candDt = dt; candN = n; candPer = per; diag.outliers++;
        splitDt = (n == 1 && ringLen > 0 && per * 100U < periodUs * (100U - JUMP_TOL_PCT)) ? dt : 0;
        if (++candRun >= CAND_STALE_RUN) ringFlush();    // stale estimate: gate off until re-established
      }
      if (accN != 0) {
        ringPush(accDt, accN); candPer = 0; candRun = 0; splitDt = 0;
        if (accN <= 2) cleanFlag = true;
        rpmMeas = (int32_t)(RPM_CONST / periodUs);
        measCenterMs = msClock - measWinUs / 2000U;         // the estimate describes the middle of its window
        rpmSlow = (rpmSlow == 0) ? rpmMeas : rpmSlow + (rpmMeas - rpmSlow) / 16;
        measSeq++; fresh = true;
      }
    } else {
      ringFlush(); candPer = 0; splitDt = 0; diag.discarded++;
    }
  }
  uint32_t prevEventUs = lastEventUs, prevPeriodUs = slotPeriodUs;
  havePrev = true;
  lastRealUs = now; lastEventUs = now; slotsSinceReal = 0; cutGap = clampOn;
  if (ringLen > 0) { slotPeriodUs = periodUs; synced = true; } else synced = false;   // no estimate -> no gate, no cuts
  bool onTime = (uint32_t)(now - prevEventUs) >= prevPeriodUs - prevPeriodUs * (100U - ONTIME_PCT) / 100U;
  bool cut = synced && decideNext(now, fresh, onTime, true);
  // Dwell interlock (coil- tach wiring): GPIO18 HIGH = igniter dwelling. Never clamp then, whatever the
  // scheduler thinks - that would abort the dwell and fire a premature spark.
  if (cut && !hwTachLow()) { cut = false; seqReset(); diag.dwellBlocks++; }
  if (cut) {
    clampSet(true, now);
    nextEventUs = now + slotPeriodUs + predMargin(slotPeriodUs);
  } else {
    clampSet(false, now);
  }
}

// Predicted ignition event of a cut slot (no tach pulse can exist while clamped).
static void IRAM_ATTR predictedEvent(uint32_t now) {
  lastEventUs += slotPeriodUs;
  slotsSinceReal++;
  diag.cutSlots++;
  int32_t r = modelRpmAt(lastEventUs);
  if (r < MODEL_MIN_RPM) r = MODEL_MIN_RPM;
  uint32_t T = RPM_CONST / (uint32_t)r;
  slotPeriodUs = T > MAX_PERIOD_US ? MAX_PERIOD_US : T;
  if (decideNext(now, false, false, false)) nextEventUs = lastEventUs + slotPeriodUs + predMargin(slotPeriodUs);
  else clampSet(false, now);
}

// =========================================================================================
// SLOW STEP (every 2 ms, inside the tick ISR)
// =========================================================================================
static inline uint16_t IRAM_ATTR satAdd(uint16_t v) { return v < SAT_MS ? (uint16_t)(v + SLOW_MS) : v; }

static void IRAM_ATTR launchStep(bool freshNew) {
  if (launchState == LAUNCH_ARMED) {
    if ((uint32_t)(msClock - armMs) >= ENGINE_LAUNCH_ARM_MS) { launchState = LAUNCH_OFF; launchEnd = LAUNCH_END_ARM_TIMEOUT; }
    else if (freshNew && !stopped && rpmMeas >= rtLaunchRpm - LAUNCH_ENTER_BELOW) {
      launchState = LAUNCH_HOLDING; holdMs = msClock; holdRef = rpmMeas; lastAtLimitMs = msClock;
      dropping = false; dropN = 0;
    }
  } else if (launchState == LAUNCH_HOLDING) {
    if (stopped) { launchState = LAUNCH_OFF; launchEnd = LAUNCH_END_STALL; return; }
    if ((uint32_t)(msClock - holdMs) >= ENGINE_LAUNCH_HOLD_MAX_MS) { launchState = LAUNCH_OFF; launchEnd = LAUNCH_END_HOLD_TIMEOUT; return; }
    if (freshNew) {
      int32_t r = rpmMeas;
      if (r > holdRef) holdRef = r;
      int32_t ref = holdRef < rtLaunchRpm ? holdRef : rtLaunchRpm;
      if (r >= ref - LAUNCH_BAND) { lastAtLimitMs = msClock; lastAtLimitRpm = r; }
      if (r < ref - rtLaunchDrop) {
        if (!dropping) { dropping = true; dropN = 0; }
        dropN++;
        uint32_t declMs = msClock - lastAtLimitMs;
        if (dropN >= LAUNCH_CONFIRM_N && declMs >= LAUNCH_CONFIRM_MS) {
          // Genuine drop (our own cut cannot pull the rpm this far below the limiter band). The limiter
          // is released now in every case; the decline rate decides the label.
          int32_t rate = (lastAtLimitRpm - r) * 1000 / (int32_t)declMs;
          diag.launchDropRate = rate;
          if (rate >= LAUNCH_CLUTCH_RATE) { launchState = LAUNCH_FIRED; firedMs = msClock; launchEnd = LAUNCH_END_FIRED; }
          else {
            launchState = LAUNCH_OFF; launchEnd = LAUNCH_END_NONE;
            verifyLift = true; verifyStartMs = msClock; verifyMin = r; verifyFloor = ref - rtLaunchDrop - LAUNCH_LIFT_EXTRA;
          }
        }
      } else {
        dropping = false;
      }
    }
  } else if (launchState == LAUNCH_FIRED) {
    if ((uint32_t)(msClock - firedMs) >= ENGINE_LAUNCH_FIRED_MS) launchState = LAUNCH_OFF;
  } else if (verifyLift) {   // LAUNCH_OFF: slow drop verification (clutch slipped in gently, or throttle lift?)
    if (freshNew) {
      int32_t r = rpmMeas;
      if (r < verifyMin) verifyMin = r;
      if (r >= verifyMin + LAUNCH_VERIFY_RISE) {
        verifyLift = false; launchState = LAUNCH_FIRED; firedMs = msClock; launchEnd = LAUNCH_END_FIRED;
      } else if (r < verifyFloor) {
        verifyLift = false; launchEnd = LAUNCH_END_LIFT;
      }
    }
    if (verifyLift && ((uint32_t)(msClock - verifyStartMs) >= LAUNCH_VERIFY_MS || stopped)) { verifyLift = false; launchEnd = LAUNCH_END_LIFT; }
  }
}

static void IRAM_ATTR decelStep(bool twoStep) {
  if (twoStep || launchState == LAUNCH_FIRED || verifyLift) decelQuietMs = DECEL_QUIET_MS;
  else if (decelQuietMs > 0) decelQuietMs = (uint16_t)(decelQuietMs - SLOW_MS);
  if (++frameDiv < DECEL_FRAME_STEPS) return;
  frameDiv = 0;
  bool freshFrame = measSeq != frameSeq; frameSeq = measSeq;
  clampHist = (clampHist << 1) | (frameClamp ? 1U : 0U); frameClamp = false;
  hist[histIdx] = rpmMeas; histT[histIdx] = measCenterMs;
  histIdx = (uint8_t)((histIdx + 1) % DECEL_HIST); if (histN < DECEL_HIST) histN++;
  // rpm change per 80 ms between the newest sample and the one 4 frames older, normalised by the real
  // time between the two measurements (during a burst a new measurement only comes every few slots)
  uint8_t iNew = (uint8_t)((histIdx + DECEL_HIST - 1) % DECEL_HIST), iOld = (uint8_t)((histIdx + DECEL_HIST - 5) % DECEL_HIST);
  uint32_t spanMs = histT[iNew] - histT[iOld];
  bool haveSlope = histN >= 5 && spanMs >= DECEL_SLOPE_MIN_MS && spanMs < 1000;
  int32_t slope = 0;
  if (haveSlope) slope = (hist[iNew] - hist[iOld]) * 80 / (int32_t)spanMs;
  // reference decline for a burst: whole history (7 frames), less sensitive to measurement ripple
  uint8_t iFirst = (uint8_t)(histIdx % DECEL_HIST);
  uint32_t spanLongMs = histT[iNew] - histT[iFirst];
  int32_t slopeLong = (histN >= DECEL_HIST && spanLongMs >= DECEL_SLOPE_MIN_MS && spanLongMs < 1000) ?
                      (hist[iNew] - hist[iFirst]) * 80 / (int32_t)spanLongMs : slope;
  bool clean = haveSlope && freshFrame && !stopped && (clampHist & DECEL_CLEAN_MASK) == 0;
  if (clean) {
    if (slope >= DECEL_RISE_80) lastRiseMs = msClock;
    if (slope <= -DECEL_DROP_80) decelFrames++; else decelFrames = 0;
    if (slope > -DECEL_STEADY_80) { if (steadyFrames < 255) steadyFrames++; } else steadyFrames = 0;
    if (steadyFrames >= DECEL_REARM_FRAMES) decelArmed = true;
  } else {
    decelFrames = 0; steadyFrames = 0;
  }

  int32_t r = rpmDisp;
  // free-rev decline over 80 ms with all sparks cut; much faster = an external load (clutch engaging)
  int32_t loadDrop80 = (MODEL_DECEL_BASE + rpmMeas * MODEL_DECEL_PCT / 100) * 80 * DECEL_LOAD_FACTOR / 1000;
  if (burstActive) {
    if (freshFrame && rpmMeas < burstMin) burstMin = rpmMeas;
    if (freshFrame) { if (rpmMeas > burstMin + DECEL_ABORT_RISE) burstRiseFrames++; else burstRiseFrames = 0; }
    uint32_t el = msClock - burstStartMs;
    if (freshFrame && haveSlope && el >= DECEL_REACCEL_AFTER_MS) {
      if (slope * 100 > burstRate0 * DECEL_REACCEL_PCT) burstSlowFrames++; else burstSlowFrames = 0;
    }
    if (!rtDecelPops || twoStep || decelQuietMs > 0 || limEngaged || el >= DECEL_BURST_MS || r < DECEL_ABORT_RPM ||
        burstRiseFrames >= DECEL_REACCEL_FRAMES || burstSlowFrames >= DECEL_REACCEL_FRAMES ||
        (freshFrame && haveSlope && slope < -loadDrop80)) {
      burstActive = false; decelFrames = 0; steadyFrames = 0;
    }
  } else if (rtDecelPops && rtArmed && decelArmed && decelFrames >= DECEL_TRIG_FRAMES &&
             (uint32_t)(msClock - lastRiseMs) >= DECEL_NO_RISE_MS && r >= rtDecelRpm && !twoStep && decelQuietMs == 0 &&
             !limEngaged && slope >= -loadDrop80) {
    burstActive = true; burstStartMs = msClock; burstMin = rpmMeas; burstRate0 = slopeLong; burstSlowFrames = 0; burstRiseFrames = 0;
    decelArmed = false; decelFrames = 0;
  }
}

static void IRAM_ATTR ghostStep(bool twoStep) {
  int32_t dev = rpmMeas - rpmSlow;
  bool ok = !stopped && measAgeMs < GHOST_MAX_AGE_MS && rpmMeas >= GHOST_MIN_RPM &&
            rpmMeas <= GHOST_MAX_RPM && rpmSlow <= GHOST_MAX_RPM && dev <= GHOST_RISE_DEV && dev >= -GHOST_DIP_DEV;
  if (!ok) ghostUnstableMs = msClock;
  reqGhost = rtGhost && rtArmed && !inhibitCut && ok && !twoStep && !burstActive &&
             (uint32_t)(msClock - ghostUnstableMs) >= GHOST_SETTLE_MS;
}

// Bench test: show button only (never the physical switch - a clutch switch pressed while cranking
// would clamp IB and the engine could not start). Engine truly stopped = no edge and no cut for 2 s
// AND either no tach pulse since boot or the last interval before the pulses stopped was slow
// (< 400 rpm, engine stalling / cranking): a tach wire lost while the engine runs at idle or in gear
// never qualifies. One session lasts at most 10 s, then the button has to be released. Any tach edge
// ends it (while clamped the igniter cannot dwell, so in practice that edge is noise).
static void IRAM_ATTR benchStep(uint32_t now) {
  bool want = rtArmed && !inhibitCut && showActive;
  if (!showActive) benchNeedRelease = false;
  if (benchActive) {
    if (!want) { benchActive = false; clampSet(false, now); }
    else if ((uint32_t)(msClock - benchStartMs) >= BENCH_MAX_MS) { benchActive = false; clampSet(false, now); benchNeedRelease = true; }
  } else if (want && !benchNeedRelease && stopped && noEdgeMs >= BENCH_STOP_MS && noCutMs >= BENCH_STOP_MS &&
             (!everPulsed || lastGapUs >= BENCH_STOP_GAP_US)) {
    seqReset(); benchActive = true; benchStartMs = msClock; clampSet(true, now);
  }
}

static void IRAM_ATTR slowStep(uint32_t now) {
  msClock += SLOW_MS;
  if (edgeFlag) { edgeFlag = false; noEdgeMs = 0; } else noEdgeMs = satAdd(noEdgeMs);
  if (pulseFlag) { pulseFlag = false; noPulseMs = 0; } else noPulseMs = satAdd(noPulseMs);
  if (measSeq != ageSeq) { ageSeq = measSeq; measAgeMs = 0; } else measAgeMs = satAdd(measAgeMs);
  if (cleanFlag) { cleanFlag = false; cleanAgeMs = 0; } else cleanAgeMs = satAdd(cleanAgeMs);
  bool clampSeen = clampOn || clampLatch;
  if (clampSeen) clampOffMs = 0; else clampOffMs = satAdd(clampOffMs);
  if (clampSeen && !benchActive) noCutMs = 0; else noCutMs = satAdd(noCutMs);
  if (clampSeen) frameClamp = true;
  clampLatch = false;

  // Engine stopped? (no pulses while not cutting, or no pulses for very long)
  stopped = (noPulseMs >= STOP_NO_PULSE_MS && clampOffMs >= STOP_NO_PULSE_MS) || noPulseMs >= STOP_HARD_MS;
  if (stopped && havePrev) {
    havePrev = false; synced = false; periodUs = 0; rpmMeas = 0; rpmSlow = 0; ringFlush(); candPer = 0; candRun = 0; splitDt = 0;
    seqReset(); limEngaged = false; cutGap = false;
  }
  rpmDisp = stopped ? 0 : (cutGap ? modelRpmAt(now) : rpmMeas);

  // Show button dead-man
  if (showHeld && (uint32_t)(msClock - showRefreshMs) > ENGINE_SHOW_DEADMAN_MS) showHeld = false;
  showActive = showHeld;
  // Physical switch, debounced
  bool raw = hwSwitchActive();
  if (raw != swStable) { if (++swCnt >= SW_DEBOUNCE_STEPS) { swStable = raw; swCnt = 0; } } else swCnt = 0;

  if (!rtArmed && (launchState != LAUNCH_OFF || verifyLift)) { launchState = LAUNCH_OFF; launchEnd = LAUNCH_END_CANCEL; verifyLift = false; }
  bool freshNew = measSeq != launchSeenSeq; launchSeenSeq = measSeq;
  launchStep(freshNew);
  bool twoStep = showActive || swStable || launchState == LAUNCH_HOLDING;
  decelStep(twoStep);
  ghostStep(twoStep);

  if (twoStep && rtLaunchRpm <= rtRedline) {
    reqLimit = rtLaunchRpm;
    reqLimitReason = showActive ? CUT_SHOW : (swStable ? CUT_SWITCH : CUT_LAUNCH);
  } else {
    reqLimit = rtRedline; reqLimitReason = CUT_REDLINE;   // the redline stays authoritative
  }
  reqBurst = burstActive;
  if (floodLock && (int32_t)(msClock - floodLockUntilMs) >= 0) floodLock = false;
  benchStep(now);

  // Telemetry snapshot
  bool cutRecent = clampOn || clampOffMs < CUT_HOLD_MS;
  uint8_t reason = CUT_NONE;
  if (benchActive) reason = CUT_BENCH;
  else if (floodLock && (limEngaged || reqBurst)) reason = CUT_FLOOD_LOCK;
  else if (cutRecent) reason = lastCutReason;
  tel.rpm = rpmDisp;
  tel.cutActive = cutRecent;
  tel.launchState = launchState;
  uint32_t armedFor = msClock - armMs;
  tel.launchLeftMs = (launchState == LAUNCH_ARMED && armedFor < ENGINE_LAUNCH_ARM_MS) ? ENGINE_LAUNCH_ARM_MS - armedFor : 0;
  tel.reason = reason;
  tel.showActive = showActive;
  tel.switchActive = swStable;
  tel.rpmEstimated = cutGap && !stopped;
  tel.measAgeMs = measAgeMs;
  tel.launchEnd = launchEnd;
}

static void IRAM_ATTR onTick(uint32_t now) {
  if (inhibitCut) {
    if (clampOn) clampSet(false, now);
    benchActive = false; seqReset();
  } else if (!benchActive) {
    if (clampOn) {
      if (!synced || (uint32_t)(now - lastRealUs) > MAX_CUT_GAP_US) {
        clampSet(false, now); seqReset(); synced = false; diag.unsyncs++;
      } else if ((int32_t)(now - nextEventUs) >= 0) {
        if ((uint32_t)(now - nextEventUs) > slotPeriodUs / 2U) {
          clampSet(false, now); seqReset(); synced = false; diag.unsyncs++;   // late tick (flash stall)
        } else {
          predictedEvent(now);
        }
      }
    } else if (synced && (uint32_t)(now - lastEventUs) > slotPeriodUs * FIRE_TIMEOUT_PCT / 100U) {
      synced = false; seqReset(); diag.unsyncs++;
    }
  }
  if (++slowDiv >= SLOW_EVERY) { slowDiv = 0; slowStep(now); }
}

// =========================================================================================
// ISR ENTRY POINTS
// =========================================================================================
static void IRAM_ATTR tachIsr() {
  bool low = hwTachLow();               // sample the level first: a glitch is over within microseconds
  portENTER_CRITICAL_ISR(&engMux);
  uint32_t now = nowUs();               // timestamp inside the lock: monotonic w.r.t. the tick ISR
  onTachEdge(now, low);
  portEXIT_CRITICAL_ISR(&engMux);
}

static void IRAM_ATTR tickIsr() {
  portENTER_CRITICAL_ISR(&engMux);
  onTick(nowUs());
  portEXIT_CRITICAL_ISR(&engMux);
}

// =========================================================================================
// CRASH FAIL-SAFE
// =========================================================================================
// Linked with -Wl,--wrap=esp_panic_handler (platformio.ini). Every fatal path - CPU exception,
// interrupt watchdog, task watchdog (TASK_WDT_PANIC=y -> abort), abort()/assert, stack overflow -
// ends in IDF's panic_handler(), which stalls the other core and then calls esp_panic_handler().
// That prints the Guru Meditation and writes the core dump to flash with interrupts disabled for
// hundreds of ms, so whatever the clamp was doing would freeze there. Drop GPIO19 (and the LED)
// first. Runs from IRAM with the cache possibly disabled: register write only.
extern "C" void __real_esp_panic_handler(panic_info_t* info);
extern "C" void IRAM_ATTR __wrap_esp_panic_handler(panic_info_t* info) {
  GPIO.out_w1tc = (1UL << PIN_SPARK_CUT) | (1UL << PIN_STATUS_LED);
  __real_esp_panic_handler(info);
}

// =========================================================================================
// CONFIG
// =========================================================================================
TuningConfig engineDefaultConfig() {
  TuningConfig c;
  c.armed         = true;
  c.launchRpm     = 3800;
  c.redlineRpm    = 6200;
  c.decelPops     = true;
  c.decelRpm      = 3200;
  c.cutPattern    = 1;      // Flames - optimal for overrun flames & pops
  c.maxCutSeconds = 3.0f;
  c.ghostCam      = false;
  c.launchDrop    = 400;    // simulator: limiter ripple reaches ~180 rpm below launchRpm; 400 leaves
                            // >200 rpm margin against a false FIRED, clutch drops still fire in < 170 ms
  return c;
}

void engineClampConfig(TuningConfig& c) {
  c.launchRpm  = constrain(c.launchRpm, 2500, 6500);
  c.redlineRpm = constrain(c.redlineRpm, 3000, 7500);
  c.decelRpm   = constrain(c.decelRpm, 2500, 6000);
  c.cutPattern = constrain(c.cutPattern, 0, 4);
  c.launchDrop = constrain(c.launchDrop, 300, 1500);
  if (!(c.maxCutSeconds >= 0.0f)) c.maxCutSeconds = 3.0f;       // NaN / negative -> default, never "unlimited"
  if (c.maxCutSeconds >= 6.0f) c.maxCutSeconds = 0.0f;           // UI slider end = unlimited
  if (c.maxCutSeconds > 0.0f && c.maxCutSeconds < 1.0f) c.maxCutSeconds = 1.0f;
  // Cross-field rules are applied at run time, not by rewriting the user's values: the 2-step
  // limit is min(launchRpm, redlineRpm) and decel pops can never cut above the redline.
}

void engineSetConfig(const TuningConfig& cfg) {
  TuningConfig c = cfg;
  engineClampConfig(c);
  uint32_t floodMs = (c.maxCutSeconds > 0.0f) ? (uint32_t)(c.maxCutSeconds * 1000.0f + 0.5f) : 0;  // float only here (task)
  portENTER_CRITICAL(&engMux);
  config       = c;
  rtArmed      = c.armed;
  rtLaunchRpm  = c.launchRpm;
  rtRedline    = c.redlineRpm;
  rtDecelPops  = c.decelPops;
  rtDecelRpm   = c.decelRpm;
  rtPattern    = (uint8_t)c.cutPattern;
  rtFloodMs    = floodMs;
  rtGhost      = c.ghostCam;
  rtLaunchDrop = c.launchDrop;
  portEXIT_CRITICAL(&engMux);
}

TuningConfig engineGetConfig() {
  portENTER_CRITICAL(&engMux);
  TuningConfig c = config;
  portEXIT_CRITICAL(&engMux);
  return c;
}

// =========================================================================================
// API
// =========================================================================================
void engineSetShowButton(bool held) {
  portENTER_CRITICAL(&engMux);
  if (held) { showHeld = true; showRefreshMs = msClock; } else showHeld = false;
  portEXIT_CRITICAL(&engMux);
}

void engineLaunchArm() {
  portENTER_CRITICAL(&engMux);
  if (rtArmed && launchState != LAUNCH_HOLDING) { launchState = LAUNCH_ARMED; armMs = msClock; launchEnd = LAUNCH_END_NONE; verifyLift = false; }
  portEXIT_CRITICAL(&engMux);
}

void engineLaunchDisarm() {
  portENTER_CRITICAL(&engMux);
  if (launchState != LAUNCH_OFF || verifyLift) { launchState = LAUNCH_OFF; launchEnd = LAUNCH_END_CANCEL; verifyLift = false; }
  portEXIT_CRITICAL(&engMux);
}

void engineSetInhibit(bool inhibit) {
  if (!inhibit && !inhibitCut) return;                       // fast path: loop() calls this every iteration
  portENTER_CRITICAL(&engMux);
  inhibitCut = inhibit;
  if (inhibit) {
    GPIO.out_w1tc = (1UL << PIN_SPARK_CUT);                  // always, immediately (LED left to the caller)
    if (clampOn) { clampOn = false; GPIO.out_w1tc = (1UL << PIN_STATUS_LED); }
    benchActive = false; seqReset();
  }
  portEXIT_CRITICAL(&engMux);
}

void engineUpdate() {}   // control runs in the tach ISR and the timer ISR

EngineTelemetry engineGetTelemetry() {
  portENTER_CRITICAL(&engMux);
  EngineTelemetry t = tel;
  portEXIT_CRITICAL(&engMux);
  return t;
}

int engineGetRpm() {
  portENTER_CRITICAL(&engMux);
  int r = tel.rpm;
  portEXIT_CRITICAL(&engMux);
  return r;
}

EngineDiag engineGetDiag() {
  portENTER_CRITICAL(&engMux);
  EngineDiag d = diag;
  portEXIT_CRITICAL(&engMux);
  return d;
}

// TODO(engine): placeholder so the data logger can be built against the API; replace with the
// real counters and the ISR trace ring.
EngineSlotCounters engineGetSlotCounters() {
  portENTER_CRITICAL(&engMux);
  EngineSlotCounters c = {diag.pulses, 0, diag.cutSlots, diag.unsyncs};
  portEXIT_CRITICAL(&engMux);
  return c;
}

size_t engineReadTrace(EngineTraceEvent* out, size_t max, uint32_t* seq, uint32_t* lost) {
  (void)out;
  (void)max;
  (void)seq;
  if (lost) *lost = 0;
  return 0;
}

// =========================================================================================
// SETUP
// =========================================================================================
void engineBegin(const TuningConfig& cfg) {
  engineSetConfig(cfg);

  // Fail-safe first: LOW = transistor off = stock ignition
  pinMode(PIN_SPARK_CUT, OUTPUT);
  pinMode(PIN_STATUS_LED, OUTPUT);
  digitalWrite(PIN_SPARK_CUT, LOW);
  digitalWrite(PIN_STATUS_LED, LOW);
  pinMode(PIN_TACH_IN, INPUT_PULLUP);
  pinMode(PIN_LAUNCH_SW, INPUT_PULLUP);

  if (ctlTimer != nullptr) return;   // already running
  // Tach: falling edge = spark (see the hardware notes at the top of this file)
  attachInterrupt(digitalPinToInterrupt(PIN_TACH_IN), tachIsr, FALLING);
  // Control tick: 1 MHz timer clock (80 MHz APB / 80), alarm every ENGINE_TICK_US, auto-reload
  ctlTimer = timerBegin(ENGINE_HW_TIMER, 80, true);
  timerAttachInterrupt(ctlTimer, &tickIsr, false);
  timerAlarmWrite(ctlTimer, TICK_US, true);
  timerAlarmEnable(ctlTimer);
}
