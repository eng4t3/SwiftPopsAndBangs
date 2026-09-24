// =========================================================================================
// ENGINE CONTROL CORE - JavaScript mirror of swift_show_tuning/engine_control.cpp
// =========================================================================================
// KEEP IN SYNC WITH engine_control.cpp. This file is a line-by-line port of the real-time
// control algorithm (same constants, same state machines, same integer math) so the engine
// simulator (run.js) exercises exactly the logic that runs on the ESP32.
//   * C++ file statics      -> variables of the createCore() closure (same names)
//   * uint32_t arithmetic   -> u32(), (int32_t)(a - b) -> s32(), unsigned '/' -> udiv()
//   * signed '/'            -> sdiv() (truncates toward zero like C)
//   * GPIO writes / reads   -> hw.clamp(on), hw.switchActive(), hw.tachLowNow(); the tach pin level
//                              sampled at ISR entry is passed in
//   * ISR entry points      -> tachIsr(nowUs, pinLow), tickIsr(nowUs)
//   * hw.log(...)           -> simulator-only tracing, no C++ counterpart
// `python tools/sim/check_sync.py` compares both files function by function (tokens + constants).
// =========================================================================================
'use strict';

const u32 = (x) => x >>> 0;
const s32 = (x) => x | 0;
const udiv = (a, b) => Math.floor(a / b);
const sdiv = (a, b) => Math.trunc(a / b);

// ---- header constants -------------------------------------------------------------------
const PULSES_PER_REV = 2;
const MIN_CUT_RPM = 2000;
const ENGINE_MAX_MEASURABLE_RPM = 10000;
const ENGINE_LAUNCH_ARM_MS = 10000;
const ENGINE_LAUNCH_HOLD_MAX_MS = 12000;
const ENGINE_LAUNCH_FIRED_MS = 2000;
const ENGINE_SHOW_DEADMAN_MS = 600;
const ENGINE_BENCH_STOP_MS = 2000;
const ENGINE_TICK_US = 100;

const LAUNCH_OFF = 0, LAUNCH_ARMED = 1, LAUNCH_HOLDING = 2, LAUNCH_FIRED = 3;
const LAUNCH_END_NONE = 0, LAUNCH_END_FIRED = 1, LAUNCH_END_LIFT = 2, LAUNCH_END_HOLD_TIMEOUT = 3,
  LAUNCH_END_ARM_TIMEOUT = 4, LAUNCH_END_CANCEL = 5, LAUNCH_END_STALL = 6;
const CUT_NONE = 0, CUT_SHOW = 1, CUT_LAUNCH = 2, CUT_SWITCH = 3, CUT_REDLINE = 4, CUT_DECEL = 5,
  CUT_GHOST = 6, CUT_BENCH = 7, CUT_FLOOD_LOCK = 8;

// ---- engine_control.cpp constants -------------------------------------------------------
const TICK_US = ENGINE_TICK_US;
const SLOW_EVERY = 20;                    // slow step every 20 ticks
const SLOW_MS = 2;                        // = TICK_US * SLOW_EVERY / 1000
const RPM_CONST = 60000000 / PULSES_PER_REV;
const MIN_PERIOD_US = RPM_CONST / ENGINE_MAX_MEASURABLE_RPM;   // 3000
const MAX_PERIOD_US = 200000;
const BLANK_PCT = 45;                     // noise gate: ignore edges earlier than 45 % of a slot after the last event.
                                          // < 50 % on purpose: a lock onto a multiple of the period cannot persist.
const BLANK_MAX_PERIOD = 60000;           // 500 rpm: no relative gate below (cranking)
const ONTIME_PCT = 95;                    // a new cut sequence may only start on a pulse arriving >= 95 % of a slot
const COMP_TOL_PCT = 30;
const MAX_COMP_SLOTS = 10;
const JUMP_TOL_PCT = 30;                  // a slot period differing more than this from the estimate is only a candidate
const CAND_TOL_PCT = 20;                  // ... adopted when the next interval agrees with it within 20 %
const CAND_STALE_RUN = 3;                 // this many unconfirmed candidates in a row: estimate is stale, drop it
const SPLIT_TOL_PCT = 15;                 // two intervals summing to 1 (or 2) slots within 15 % = a split slot
const AVG_SLOTS = 2;
const RING_LEN = 4;
const PRED_MARGIN_DIV = 8;
const PRED_MARGIN_MIN_US = 200;
const FIRE_TIMEOUT_PCT = 160;
const MAX_CUT_GAP_US = 1700000;
const MODEL_DECEL_BASE = 1000;           // assumed decel with all sparks cut: 1000 + 0.45 * rpm  [rpm/s]
const MODEL_DECEL_PCT = 45;
const MODEL_MIN_RPM = 300;
const STOP_NO_PULSE_MS = 250;
const STOP_HARD_MS = 1800;
const SAT_MS = 60000;
const LIMIT_HYST = 100;
const LIMIT_ESCALATE = 75;
const HARD_MIN_CUT = 2;
const HARD_MAX_CUT = 5;                   // longer runs make the slot count ambiguous (clock drift)
const CLEAN_MAX_MS = 150;                 // no single-slot (unambiguous) measurement for this long: fire 2 in a row
const CANNON_CUT_MS = 1600;
const CANNON_RESTORE_RPM = MIN_CUT_RPM + 500;
const CANNON_FIRE_MIN_MS = 250;
const FLOOD_LOCK_MS = 1000;
const CUT_HOLD_MS = 60;
const LAUNCH_ENTER_BELOW = 200;
const LAUNCH_BAND = 250;                  // "on the limiter" band below the reference (covers the limiter ripple)
const LAUNCH_CONFIRM_MS = 60;             // the decline must have lasted >= 60 ms ...
const LAUNCH_CONFIRM_N = 3;               // ... and >= 3 consecutive fresh measurements below the threshold
const LAUNCH_CLUTCH_RATE = 3500;          // faster decline = clutch for sure (a throttle lift cannot be this fast)
const LAUNCH_VERIFY_MS = 800;             // slower decline: clutch if the rpm recovers within this time ...
const LAUNCH_VERIFY_RISE = 100;           // ... by this much above its minimum (car moving)
const LAUNCH_LIFT_EXTRA = 800;            // ... lift if it keeps falling this far below the threshold
const DECEL_FRAME_STEPS = 10;
const DECEL_HIST = 8;
const DECEL_DROP_80 = 60;
const DECEL_RISE_80 = 30;
const DECEL_STEADY_80 = 20;
const DECEL_TRIG_FRAMES = 4;
const DECEL_REARM_FRAMES = 5;
const DECEL_NO_RISE_MS = 250;
const DECEL_BURST_MS = 1200;
const DECEL_ABORT_RPM = 2100;
const DECEL_ABORT_RISE = 60;
const DECEL_CLEAN_MASK = 0x3f;
const DECEL_QUIET_MS = 3000;              // no decel pops within 3 s after a 2-step / launch (clutch engaging = load)
const DECEL_LOAD_FACTOR = 2;              // burst aborts when rpm falls > 2x faster than free-rev friction (load)
const GHOST_MIN_RPM = 650;
const GHOST_MAX_RPM = 1250;
const GHOST_EVERY = 5;
const GHOST_DIP_DEV = 150;                // below the slow average (our own dips are ~70 rpm)
const GHOST_RISE_DEV = 60;                // above the slow average = rising / throttle / pulling away
const GHOST_SETTLE_MS = 1000;
const GHOST_MAX_AGE_MS = 100;
const BENCH_STOP_MS = ENGINE_BENCH_STOP_MS;
const SW_DEBOUNCE_STEPS = 10;

const SEQ_NONE = 0, SEQ_CYCLE = 1, SEQ_CANNON_CUT = 2, SEQ_CANNON_FIRE = 3;

function engineDefaultConfig() {
  return {
    armed: true, launchRpm: 3800, redlineRpm: 6200, decelPops: true, decelRpm: 3200,
    cutPattern: 1, maxCutSeconds: 3.0, ghostCam: false, launchDrop: 400,
  };
}

function constrain(v, lo, hi) { return v < lo ? lo : v > hi ? hi : v; }

function engineClampConfig(c) {
  c.launchRpm = constrain(c.launchRpm, 2500, 6500);
  c.redlineRpm = constrain(c.redlineRpm, 3000, 7500);
  c.decelRpm = constrain(c.decelRpm, 2500, 6000);
  c.cutPattern = constrain(c.cutPattern, 0, 4);
  c.launchDrop = constrain(c.launchDrop, 300, 1500);
  if (!(c.maxCutSeconds >= 0.0)) c.maxCutSeconds = 3.0;          // NaN / negative -> default, never "unlimited"
  if (c.maxCutSeconds >= 6.0) c.maxCutSeconds = 0.0;              // UI slider end = unlimited
  if (c.maxCutSeconds > 0.0 && c.maxCutSeconds < 1.0) c.maxCutSeconds = 1.0;
}

function createCore(hw) {
  // ---- runtime config (integers only) ----
  let rtArmed = false, rtLaunchRpm = 3800, rtRedline = 6200, rtDecelRpm = 3200, rtLaunchDrop = 400;
  let rtPattern = 1, rtDecelPops = true, rtGhost = false, rtFloodMs = 3000;
  let config = engineDefaultConfig();

  // ---- external inputs ----
  let inhibitCut = false, showHeld = false, showRefreshMs = 0;

  // ---- tach / estimator ----
  let havePrev = false, synced = false;
  let lastRealUs = 0, lastEventUs = 0, slotsSinceReal = 0, slotPeriodUs = 0, periodUs = 0;
  let rpmMeas = 0, rpmSlow = 0, measSeq = 0;
  const ringDt = new Array(RING_LEN).fill(0), ringN = new Array(RING_LEN).fill(0);
  let ringLen = 0, ringHead = 0, measWinUs = 0;
  let edgeFlag = false, pulseFlag = false, cleanFlag = false;
  let candDt = 0, candN = 0, candPer = 0, candRun = 0, splitDt = 0;

  // ---- clamp ----
  let clampOn = false, clampLatch = false, cutGap = false;
  let clampOnSinceUs = 0, nextEventUs = 0, lastCutReason = CUT_NONE;

  // ---- sequencer ----
  let seqMode = SEQ_NONE, seqCutLeft = 0, seqNeedFire = false, seqReason = CUT_NONE, seqLimit = 0;
  let seqStartUs = 0, seqFireUntilMs = 0, limEngaged = false, ghostCount = 0;
  let floodLock = false, floodLockUntilMs = 0;

  // ---- slow step ----
  let slowDiv = 0, msClock = 0;
  let noEdgeMs = SAT_MS, noPulseMs = SAT_MS, clampOffMs = SAT_MS, noCutMs = SAT_MS, measAgeMs = SAT_MS, cleanAgeMs = SAT_MS;
  let ageSeq = 0;
  let stopped = true, rpmDisp = 0, showActive = false, swStable = false, swCnt = 0, benchActive = false;
  let reqLimit = 6200, reqLimitReason = CUT_REDLINE, reqBurst = false, reqGhost = false;
  // launch
  let launchState = LAUNCH_OFF, launchEnd = LAUNCH_END_NONE;
  let armMs = 0, holdMs = 0, firedMs = 0, lastAtLimitMs = 0;
  let holdRef = 0, dropping = false, dropN = 0, launchSeenSeq = 0, lastAtLimitRpm = 0;
  let verifyLift = false, verifyStartMs = 0, verifyMin = 0, verifyFloor = 0;
  // decel
  let frameDiv = 0; const hist = new Array(DECEL_HIST).fill(0); let histIdx = 0, histN = 0;
  let clampHist = 0, frameClamp = false, frameSeq = 0;
  let decelFrames = 0, steadyFrames = 0, decelArmed = false, lastRiseMs = 0;
  let burstActive = false, burstStartMs = 0, burstMin = 0, burstRate0 = 0, burstSlowFrames = 0;
  let decelQuietMs = 0;
  // ghost
  let ghostUnstableMs = 0;
  // telemetry / diag
  const tel = { rpm: 0, cutActive: false, launchState: 0, launchLeftMs: 0, reason: 0,
    showActive: false, switchActive: false, rpmEstimated: false, measAgeMs: SAT_MS, launchEnd: 0 };
  const diag = { pulses: 0, rejected: 0, discarded: 0, outliers: 0, unsyncs: 0, cutSlots: 0, floodTrips: 0, dwellBlocks: 0, launchDropRate: 0 };

  // =======================================================================================
  // helpers
  // =======================================================================================
  function predMargin(T) { const m = udiv(T, PRED_MARGIN_DIV); return m < PRED_MARGIN_MIN_US ? PRED_MARGIN_MIN_US : m; }
  function absDiff(a, b) { const d = s32(a - b); return u32(d < 0 ? -d : d); }

  function clampSet(on, now) {
    if (on) {
      if (inhibitCut) return;
      if (!clampOn) {
        clampOn = true; clampOnSinceUs = now; hw.clamp(true);
        lastCutReason = benchActive ? CUT_BENCH : seqReason;
      }
      clampLatch = true; cutGap = true;
    } else if (clampOn) {
      clampOn = false; hw.clamp(false);
    }
  }

  function modelRpmAt(t) {
    if (!cutGap || rpmMeas <= 0) return rpmMeas;
    const d = s32(t - lastRealUs);
    let elMs = d > 0 ? udiv(d, 1000) : 0;
    if (elMs > 5000) elMs = 5000;
    const decel = MODEL_DECEL_BASE + udiv(rpmMeas * MODEL_DECEL_PCT, 100);
    const r = rpmMeas - udiv(elMs * decel, 1000);
    return r < MODEL_MIN_RPM ? MODEL_MIN_RPM : r;
  }

  function ringFlush() { ringLen = 0; ringHead = 0; }
  function ringPush(dt, n) {
    ringDt[ringHead] = dt; ringN[ringHead] = n;
    ringHead = (ringHead + 1) % RING_LEN;
    if (ringLen < RING_LEN) ringLen++;
    let sumDt = 0, sumN = 0, idx = ringHead;
    for (let i = 0; i < ringLen && sumN < AVG_SLOTS; i++) {
      idx = (idx + RING_LEN - 1) % RING_LEN;
      sumDt += ringDt[idx]; sumN += ringN[idx];
    }
    periodUs = udiv(sumDt, sumN);
    measWinUs = sumDt;
  }

  function seqReset() { seqMode = SEQ_NONE; seqCutLeft = 0; seqNeedFire = false; }

  function floodCheck(now) {
    if (rtFloodMs === 0) return true;
    const cont = clampOn ? u32(now - clampOnSinceUs) : 0;
    if (cont + slotPeriodUs > rtFloodMs * 1000) {
      floodLock = true; floodLockUntilMs = u32(msClock + FLOOD_LOCK_MS); diag.floodTrips++;
      seqReset();
      return false;
    }
    return true;
  }

  // Modelled rpm loss over one cut slot (free-rev decel x slot period).
  function slotDrop(r) {
    const decel = MODEL_DECEL_BASE + udiv(r * MODEL_DECEL_PCT, 100);
    return udiv(decel * udiv(slotPeriodUs, 10), 100000);
  }

  // MIN_CUT_RPM interlock: even if the rpm has been falling at the free-rev rate since the middle of
  // the measurement window, it must stay >= MIN_CUT_RPM until the end of the next slot.
  function minCutOk(r) {
    const decel = MODEL_DECEL_BASE + udiv(r * MODEL_DECEL_PCT, 100);
    const t10 = udiv(udiv(measWinUs, 2) + slotPeriodUs, 10);
    return r - udiv(decel * t10, 100000) >= MIN_CUT_RPM;
  }

  function hardCutSlots(r, lim, isLimiter) {
    if (!isLimiter) return HARD_MAX_CUT;
    let perSlot = slotDrop(r);
    if (perSlot < 1) perSlot = 1;
    const excess = r - (lim - LIMIT_HYST);
    let k = sdiv(excess + perSlot - 1, perSlot);
    if (k < HARD_MIN_CUT) k = HARD_MIN_CUT;
    if (k > HARD_MAX_CUT) k = HARD_MAX_CUT;
    return k;
  }

  function startSeq(reason, r, lim, isLimiter, now) {
    seqReason = reason; seqLimit = lim;
    const escalate = isLimiter && r >= lim + LIMIT_ESCALATE;
    const p = rtPattern;
    if (p === 4 && !escalate && !floodLock && reason !== CUT_LAUNCH) {
      seqMode = SEQ_CANNON_CUT; seqStartUs = now;
      return floodCheck(now);
    }
    let nCut;
    if (p === 0 || p === 4 || escalate) nCut = hardCutSlots(r, lim, isLimiter);
    else if (p === 1) nCut = 3;
    else if (p === 2) nCut = 2;
    else nCut = 1;
    seqMode = SEQ_CYCLE; seqCutLeft = nCut - 1; seqNeedFire = (seqCutLeft === 0);
    return floodCheck(now);
  }

  function seqStillWanted() {
    if (seqReason === CUT_DECEL) return reqBurst;
    if (seqReason === CUT_GHOST) return reqGhost;
    return reqLimit <= seqLimit;
  }

  // Decide the NEXT ignition slot at a slot boundary (real pulse or predicted event).
  // fresh: a new RPM measurement arrived with this pulse; onTime: the pulse is not early (noise guard);
  // atReal: called from a real tach pulse (not from a predicted event).
  function decideNext(now, fresh, onTime, atReal) {
    if (inhibitCut || !rtArmed || benchActive) { seqReset(); limEngaged = false; return false; }
    const r = fresh ? rpmMeas : modelRpmAt(now);
    if (fresh) {
      if (r < MIN_CUT_RPM) limEngaged = false;
      else if (r >= reqLimit) limEngaged = true;
      else if (r < reqLimit - LIMIT_HYST) limEngaged = false;
    }

    if (seqMode === SEQ_CYCLE) {
      if (seqCutLeft > 0) {
        if (!seqStillWanted() || (seqReason !== CUT_GHOST && !minCutOk(r))) { seqReset(); return false; }
        seqCutLeft--;
        if (seqCutLeft === 0) seqNeedFire = true;
        return floodCheck(now);
      }
      seqReset();          // the cycle's fired (measurement) slot
      return false;
    } else if (seqMode === SEQ_CANNON_CUT) {
      if (!seqStillWanted() || u32(now - seqStartUs) >= CANNON_CUT_MS * 1000 || r <= CANNON_RESTORE_RPM) {
        seqMode = SEQ_CANNON_FIRE; seqFireUntilMs = u32(msClock + CANNON_FIRE_MIN_MS); limEngaged = false;
        return false;
      }
      return floodCheck(now);
    } else if (seqMode === SEQ_CANNON_FIRE) {
      if (s32(msClock - seqFireUntilMs) < 0) {
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

  // =======================================================================================
  // Tach ISR body
  // =======================================================================================
  function onTachEdge(now, pinLow) {
    if (hw.log) hw.log('edge', now, { pinLow, clampOn, synced, slotPeriodUs, lastEventUs, lastRealUs, slotsSinceReal, periodUs });
    edgeFlag = true;
    if (benchActive) { benchActive = false; clampSet(false, now); }
    if (!pinLow) { diag.rejected++; return; }
    if (havePrev) {
      const dtRaw = u32(now - lastRealUs);
      if (dtRaw < MIN_PERIOD_US) { diag.rejected++; return; }
      if (clampOn) { diag.rejected++; return; }              // no dwell while clamped -> cannot be a spark
      // Adaptive noise gate (coil ringing, dwell-start glitches). Relative to the shorter of the estimate and
      // a pending candidate, so a stale estimate during a fast rev-up can never reject real pulses.
      const gateT = (candPer !== 0 && candPer < slotPeriodUs) ? candPer : slotPeriodUs;
      if (synced && gateT < BLANK_MAX_PERIOD && u32(now - lastEventUs) < udiv(gateT * BLANK_PCT, 100)) {
        diag.rejected++; return;
      }
    }

    pulseFlag = true; diag.pulses++;
    let fresh = false;
    if (havePrev) {
      const dt = u32(now - lastRealUs);
      let n = 0;
      if (slotsSinceReal === 0) {
        n = 1;                                              // consecutive fired slots
      } else if (synced && slotsSinceReal + 1 <= MAX_COMP_SLOTS &&
                 absDiff(now, u32(lastEventUs + slotPeriodUs)) <= udiv(slotPeriodUs * COMP_TOL_PCT, 100)) {
        n = slotsSinceReal + 1;                             // fired slot after n-1 cut slots
      }
      const per = n > 0 ? udiv(dt, n) : 0;
      if (n > 0 && per >= MIN_PERIOD_US && per <= MAX_PERIOD_US) {
        // Plausibility: the engine cannot change speed by > 30 % within one slot.
        //  * within 30 % of the estimate               -> measurement
        //  * short interval + next interval = 1-2 slots -> a noise edge split a slot: merge them
        //  * otherwise the interval is only a candidate; it is adopted when the next interval agrees
        //    with it (missed edge, stale estimate). Single outliers never reach the estimate, and a
        //    lock onto a multiple / fraction of the true period cannot build up.
        let accDt = 0, accN = 0;
        if (ringLen > 0 && absDiff(per, periodUs) <= udiv(periodUs * JUMP_TOL_PCT, 100)) {
          accDt = dt; accN = n;
        } else if (splitDt !== 0 && n === 1 && ringLen > 0 && absDiff(splitDt + dt, periodUs) <= udiv(periodUs * SPLIT_TOL_PCT, 100)) {
          accDt = splitDt + dt; accN = 1; diag.outliers++;
        } else if (splitDt !== 0 && n === 1 && ringLen > 0 && absDiff(splitDt + dt, 2 * periodUs) <= udiv(periodUs * SPLIT_TOL_PCT, 100)) {
          accDt = splitDt + dt; accN = 2; diag.outliers++;
        } else if (candPer !== 0 && absDiff(per, candPer) <= udiv(candPer * CAND_TOL_PCT, 100)) {
          ringFlush(); ringPush(candDt, candN); accDt = dt; accN = n;
        } else {
          candDt = dt; candN = n; candPer = per; diag.outliers++;
          splitDt = (n === 1 && ringLen > 0 && per * 100 < periodUs * (100 - JUMP_TOL_PCT)) ? dt : 0;
          if (++candRun >= CAND_STALE_RUN) ringFlush();      // stale estimate: gate off until re-established
        }
        if (accN !== 0) {
          ringPush(accDt, accN); candPer = 0; candRun = 0; splitDt = 0;
          if (accN <= 2) cleanFlag = true;
          rpmMeas = udiv(RPM_CONST, periodUs);
          rpmSlow = rpmSlow === 0 ? rpmMeas : rpmSlow + sdiv(rpmMeas - rpmSlow, 16);
          measSeq = u32(measSeq + 1); fresh = true;
        }
      } else {
        ringFlush(); candPer = 0; splitDt = 0; diag.discarded++;
        if (hw.log) hw.log('discard', now, { dt, n, slotsSinceReal, synced });
      }
    }
    const prevEventUs = lastEventUs, prevPeriodUs = slotPeriodUs;
    havePrev = true;
    lastRealUs = now; lastEventUs = now; slotsSinceReal = 0; cutGap = clampOn;
    if (ringLen > 0) { slotPeriodUs = periodUs; synced = true; } else synced = false;   // no estimate -> no gate, no cuts
    const onTime = u32(now - prevEventUs) >= prevPeriodUs - udiv(prevPeriodUs * (100 - ONTIME_PCT), 100);
    let cut = synced && decideNext(now, fresh, onTime, true);
    // Dwell interlock (coil- tach wiring): GPIO18 HIGH = igniter dwelling. Never clamp then, whatever the
    // scheduler thinks - that would abort the dwell and fire a premature spark.
    if (cut && !hw.tachLowNow()) { cut = false; seqReset(); diag.dwellBlocks++; }
    if (cut) {
      clampSet(true, now);
      nextEventUs = u32(now + slotPeriodUs + predMargin(slotPeriodUs));
    } else {
      clampSet(false, now);
    }
  }

  // Predicted ignition event of a cut slot (no tach pulse can exist while clamped).
  function predictedEvent(now) {
    lastEventUs = u32(lastEventUs + slotPeriodUs);
    slotsSinceReal++;
    diag.cutSlots++;
    let r = modelRpmAt(lastEventUs);
    if (r < MODEL_MIN_RPM) r = MODEL_MIN_RPM;
    const T = udiv(RPM_CONST, r);
    slotPeriodUs = T > MAX_PERIOD_US ? MAX_PERIOD_US : T;
    if (decideNext(now, false, false, false)) nextEventUs = u32(lastEventUs + slotPeriodUs + predMargin(slotPeriodUs));
    else clampSet(false, now);
  }

  // =======================================================================================
  // Slow step (every 2 ms, inside the tick ISR)
  // =======================================================================================
  function satAdd(v) { return v < SAT_MS ? v + SLOW_MS : v; }

  function launchStep(freshNew) {
    switch (launchState) {
      case LAUNCH_ARMED:
        if (u32(msClock - armMs) >= ENGINE_LAUNCH_ARM_MS) { launchState = LAUNCH_OFF; launchEnd = LAUNCH_END_ARM_TIMEOUT; }
        else if (freshNew && !stopped && rpmMeas >= rtLaunchRpm - LAUNCH_ENTER_BELOW) {
          launchState = LAUNCH_HOLDING; holdMs = msClock; holdRef = rpmMeas; lastAtLimitMs = msClock;
          dropping = false; dropN = 0;
        }
        break;
      case LAUNCH_HOLDING:
        if (stopped) { launchState = LAUNCH_OFF; launchEnd = LAUNCH_END_STALL; break; }
        if (u32(msClock - holdMs) >= ENGINE_LAUNCH_HOLD_MAX_MS) { launchState = LAUNCH_OFF; launchEnd = LAUNCH_END_HOLD_TIMEOUT; break; }
        if (freshNew) {
          const r = rpmMeas;
          if (r > holdRef) holdRef = r;
          const ref = holdRef < rtLaunchRpm ? holdRef : rtLaunchRpm;
          if (r >= ref - LAUNCH_BAND) { lastAtLimitMs = msClock; lastAtLimitRpm = r; }
          if (r < ref - rtLaunchDrop) {
            if (!dropping) { dropping = true; dropN = 0; }
            dropN++;
            const declMs = u32(msClock - lastAtLimitMs);
            if (dropN >= LAUNCH_CONFIRM_N && declMs >= LAUNCH_CONFIRM_MS) {
              // Genuine drop (our own cut cannot pull the rpm this far below the limiter band). The limiter is
              // released now in every case; the decline rate decides the label.
              const rate = sdiv((lastAtLimitRpm - r) * 1000, declMs);
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
        break;
      case LAUNCH_FIRED:
        if (u32(msClock - firedMs) >= ENGINE_LAUNCH_FIRED_MS) launchState = LAUNCH_OFF;
        break;
      default:   // LAUNCH_OFF: slow drop verification (clutch slipped in gently, or throttle lift?)
        if (verifyLift) {
          if (freshNew) {
            const r = rpmMeas;
            if (r < verifyMin) verifyMin = r;
            if (r >= verifyMin + LAUNCH_VERIFY_RISE) {
              verifyLift = false; launchState = LAUNCH_FIRED; firedMs = msClock; launchEnd = LAUNCH_END_FIRED;
            } else if (r < verifyFloor) {
              verifyLift = false; launchEnd = LAUNCH_END_LIFT;
            }
          }
          if (verifyLift && (u32(msClock - verifyStartMs) >= LAUNCH_VERIFY_MS || stopped)) { verifyLift = false; launchEnd = LAUNCH_END_LIFT; }
        }
        break;
    }
  }

  function decelStep(twoStep) {
    if (twoStep || launchState === LAUNCH_FIRED || verifyLift) decelQuietMs = DECEL_QUIET_MS;
    else if (decelQuietMs > 0) decelQuietMs -= SLOW_MS;
    if (++frameDiv < DECEL_FRAME_STEPS) return;
    frameDiv = 0;
    const freshFrame = measSeq !== frameSeq; frameSeq = measSeq;
    clampHist = u32((clampHist << 1) | (frameClamp ? 1 : 0)); frameClamp = false;
    hist[histIdx] = rpmMeas; histIdx = (histIdx + 1) % DECEL_HIST; if (histN < DECEL_HIST) histN++;
    const haveSlope = histN >= 5;
    let slope = 0;
    if (haveSlope) slope = hist[(histIdx + DECEL_HIST - 1) % DECEL_HIST] - hist[(histIdx + DECEL_HIST - 5) % DECEL_HIST];
    const clean = haveSlope && freshFrame && !stopped && (clampHist & DECEL_CLEAN_MASK) === 0;
    if (clean) {
      if (slope >= DECEL_RISE_80) lastRiseMs = msClock;
      if (slope <= -DECEL_DROP_80) decelFrames++; else decelFrames = 0;
      if (slope > -DECEL_STEADY_80) { if (steadyFrames < 255) steadyFrames++; } else steadyFrames = 0;
      if (steadyFrames >= DECEL_REARM_FRAMES) decelArmed = true;
    } else {
      decelFrames = 0; steadyFrames = 0;
    }

    const r = rpmDisp;
    // free-rev decline over 80 ms with all sparks cut; much faster = an external load (clutch engaging)
    const loadDrop80 = udiv((MODEL_DECEL_BASE + udiv(rpmMeas * MODEL_DECEL_PCT, 100)) * 80 * DECEL_LOAD_FACTOR, 1000);
    if (burstActive) {
      if (freshFrame && rpmMeas < burstMin) burstMin = rpmMeas;
      const el = u32(msClock - burstStartMs);
      if (rtPattern >= 1 && rtPattern <= 3 && freshFrame && haveSlope && el >= 100) {
        if (slope > sdiv(burstRate0, 2)) burstSlowFrames++; else burstSlowFrames = 0;
      }
      if (!rtDecelPops || twoStep || decelQuietMs > 0 || limEngaged || el >= DECEL_BURST_MS || r < DECEL_ABORT_RPM ||
          (freshFrame && rpmMeas > burstMin + DECEL_ABORT_RISE) || burstSlowFrames >= 2 ||
          (freshFrame && haveSlope && slope < -loadDrop80)) {
        burstActive = false; decelFrames = 0; steadyFrames = 0;
      }
    } else if (rtDecelPops && rtArmed && decelArmed && decelFrames >= DECEL_TRIG_FRAMES &&
               u32(msClock - lastRiseMs) >= DECEL_NO_RISE_MS && r >= rtDecelRpm && !twoStep && decelQuietMs === 0 &&
               !limEngaged && slope >= -loadDrop80) {
      burstActive = true; burstStartMs = msClock; burstMin = rpmMeas; burstRate0 = slope; burstSlowFrames = 0;
      decelArmed = false; decelFrames = 0;
    }
  }

  function ghostStep(twoStep) {
    const dev = rpmMeas - rpmSlow;
    const ok = !stopped && measAgeMs < GHOST_MAX_AGE_MS && rpmMeas >= GHOST_MIN_RPM &&
      rpmMeas <= GHOST_MAX_RPM && rpmSlow <= GHOST_MAX_RPM && dev <= GHOST_RISE_DEV && dev >= -GHOST_DIP_DEV;
    if (!ok) ghostUnstableMs = msClock;
    reqGhost = rtGhost && rtArmed && !inhibitCut && ok && !twoStep && !burstActive &&
      u32(msClock - ghostUnstableMs) >= GHOST_SETTLE_MS;
  }

  function benchStep(now) {
    const want = rtArmed && !inhibitCut && showActive;
    if (benchActive) {
      if (!want) { benchActive = false; clampSet(false, now); }
    } else if (want && stopped && noEdgeMs >= BENCH_STOP_MS && noCutMs >= BENCH_STOP_MS) {
      seqReset(); benchActive = true; clampSet(true, now);
    }
  }

  function slowStep(now) {
    msClock = u32(msClock + SLOW_MS);
    if (edgeFlag) { edgeFlag = false; noEdgeMs = 0; } else noEdgeMs = satAdd(noEdgeMs);
    if (pulseFlag) { pulseFlag = false; noPulseMs = 0; } else noPulseMs = satAdd(noPulseMs);
    if (measSeq !== ageSeq) { ageSeq = measSeq; measAgeMs = 0; } else measAgeMs = satAdd(measAgeMs);
    if (cleanFlag) { cleanFlag = false; cleanAgeMs = 0; } else cleanAgeMs = satAdd(cleanAgeMs);
    const clampSeen = clampOn || clampLatch;
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
    if (showHeld && u32(msClock - showRefreshMs) > ENGINE_SHOW_DEADMAN_MS) showHeld = false;
    showActive = showHeld;
    // Physical switch, debounced
    const raw = hw.switchActive();
    if (raw !== swStable) { if (++swCnt >= SW_DEBOUNCE_STEPS) { swStable = raw; swCnt = 0; } } else swCnt = 0;

    if (!rtArmed && (launchState !== LAUNCH_OFF || verifyLift)) { launchState = LAUNCH_OFF; launchEnd = LAUNCH_END_CANCEL; verifyLift = false; }
    const freshNew = measSeq !== launchSeenSeq; launchSeenSeq = measSeq;
    launchStep(freshNew);
    const twoStep = showActive || swStable || launchState === LAUNCH_HOLDING;
    decelStep(twoStep);
    ghostStep(twoStep);

    if (twoStep && rtLaunchRpm <= rtRedline) {
      reqLimit = rtLaunchRpm;
      reqLimitReason = showActive ? CUT_SHOW : (swStable ? CUT_SWITCH : CUT_LAUNCH);
    } else {
      reqLimit = rtRedline; reqLimitReason = CUT_REDLINE;
    }
    reqBurst = burstActive;
    if (floodLock && s32(msClock - floodLockUntilMs) >= 0) floodLock = false;
    benchStep(now);

    // Telemetry snapshot
    const cutRecent = clampOn || clampOffMs < CUT_HOLD_MS;
    let reason = CUT_NONE;
    if (benchActive) reason = CUT_BENCH;
    else if (floodLock && (limEngaged || reqBurst)) reason = CUT_FLOOD_LOCK;
    else if (cutRecent) reason = lastCutReason;
    tel.rpm = rpmDisp;
    tel.cutActive = cutRecent;
    tel.launchState = launchState;
    const armedFor = u32(msClock - armMs);
    tel.launchLeftMs = (launchState === LAUNCH_ARMED && armedFor < ENGINE_LAUNCH_ARM_MS) ? ENGINE_LAUNCH_ARM_MS - armedFor : 0;
    tel.reason = reason;
    tel.showActive = showActive;
    tel.switchActive = swStable;
    tel.rpmEstimated = cutGap && !stopped;
    tel.measAgeMs = measAgeMs;
    tel.launchEnd = launchEnd;
  }

  function onTick(now) {
    if (inhibitCut) {
      if (clampOn) clampSet(false, now);
      benchActive = false; seqReset();
    } else if (!benchActive) {
      if (clampOn) {
        if (!synced || u32(now - lastRealUs) > MAX_CUT_GAP_US) {
          clampSet(false, now); seqReset(); synced = false; diag.unsyncs++;
        } else if (s32(now - nextEventUs) >= 0) {
          if (u32(now - nextEventUs) > udiv(slotPeriodUs, 2)) {
            clampSet(false, now); seqReset(); synced = false; diag.unsyncs++;   // late tick (flash stall)
          } else {
            predictedEvent(now);
          }
        }
      } else if (synced && u32(now - lastEventUs) > udiv(slotPeriodUs * FIRE_TIMEOUT_PCT, 100)) {
        synced = false; seqReset(); diag.unsyncs++;
      }
    }
    if (++slowDiv >= SLOW_EVERY) { slowDiv = 0; slowStep(now); }
  }

  // =======================================================================================
  // API (task context in C++; the critical sections are implicit in JS)
  // =======================================================================================
  function engineSetConfig(cfg) {
    const c = Object.assign({}, cfg); engineClampConfig(c);
    const floodMs = c.maxCutSeconds > 0.0 ? Math.floor(c.maxCutSeconds * 1000.0 + 0.5) : 0;
    config = c;
    rtArmed = c.armed; rtLaunchRpm = c.launchRpm; rtRedline = c.redlineRpm; rtDecelPops = c.decelPops;
    rtDecelRpm = c.decelRpm; rtPattern = c.cutPattern; rtFloodMs = floodMs; rtGhost = c.ghostCam;
    rtLaunchDrop = c.launchDrop;
  }
  function engineSetShowButton(held) {
    if (held) { showHeld = true; showRefreshMs = msClock; } else showHeld = false;
  }
  function engineLaunchArm() {
    if (rtArmed && launchState !== LAUNCH_HOLDING) { launchState = LAUNCH_ARMED; armMs = msClock; launchEnd = LAUNCH_END_NONE; verifyLift = false; }
  }
  function engineLaunchDisarm() {
    if (launchState !== LAUNCH_OFF || verifyLift) { launchState = LAUNCH_OFF; launchEnd = LAUNCH_END_CANCEL; verifyLift = false; }
  }
  function engineSetInhibit(inh) {
    inhibitCut = inh;
    if (inh) {
      if (clampOn) { clampOn = false; } hw.clamp(false);
      benchActive = false; seqReset();
    }
  }

  return {
    engineSetConfig, engineGetConfig: () => Object.assign({}, config),
    engineSetShowButton, engineLaunchArm, engineLaunchDisarm, engineSetInhibit,
    engineGetTelemetry: () => Object.assign({}, tel), engineGetRpm: () => tel.rpm,
    engineGetDiag: () => Object.assign({}, diag),
    tachIsr: (now, pinLow) => onTachEdge(u32(now), pinLow),
    tickIsr: (now) => onTick(u32(now)),
    // introspection for the simulator only
    _dbg: () => ({ clampOn, synced, periodUs, slotPeriodUs, rpmMeas, seqMode, seqReason, limEngaged, burstActive,
      reqGhost, benchActive, floodLock, measSeq, stopped, launchState, holdRef, cutGap }),
  };
}

module.exports = {
  createCore, engineDefaultConfig, engineClampConfig, TICK_US,
  LAUNCH_OFF, LAUNCH_ARMED, LAUNCH_HOLDING, LAUNCH_FIRED,
  LAUNCH_END_NONE, LAUNCH_END_FIRED, LAUNCH_END_LIFT, LAUNCH_END_HOLD_TIMEOUT, LAUNCH_END_ARM_TIMEOUT,
  LAUNCH_END_CANCEL, LAUNCH_END_STALL,
  CUT_NONE, CUT_SHOW, CUT_LAUNCH, CUT_SWITCH, CUT_REDLINE, CUT_DECEL, CUT_GHOST, CUT_BENCH, CUT_FLOOD_LOCK,
};
