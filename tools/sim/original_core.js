// =========================================================================================
// ORIGINAL (v2.0.0, commit 258cb19) engine_control.cpp ported 1:1 to JavaScript, so the
// simulator can reproduce the "launch disables instantly" bug. Control runs in loop()
// (engineUpdate() called at the loop rate), exactly like the original firmware.
// Not maintained - reference only.
// =========================================================================================
'use strict';

const u32 = (x) => x >>> 0;
const udiv = (a, b) => Math.floor(a / b);
const LAUNCH_OFF = 0, LAUNCH_ARMED = 1, LAUNCH_HOLDING = 2, LAUNCH_FIRED = 3;
const CUT_NONE = 0, CUT_SHOW = 1, CUT_LAUNCH = 2, CUT_SWITCH = 3, CUT_REDLINE = 4, CUT_DECEL = 5,
  CUT_GHOST = 6, CUT_BENCH = 7, CUT_FLOOD_LOCK = 8;
const MIN_CUT_RPM = 2000, HARD_MAX_RPM = 8000, PULSES_PER_REV = 2;

function createOriginalCore(hw) {
  let config = { armed: true, launchRpm: 3800, redlineRpm: 6200, decelPops: true, decelRpm: 3200,
    cutPattern: 1, maxCutSeconds: 3.0, ghostCam: false, launchDrop: 400 };
  let lastPulseMicros = 0, pulseIntervalSum = 0, pulseIntervalCount = 0, lastValidInterval = 0;
  const pulseHistory = [35000, 35000, 35000, 35000]; let pulseHistIdx = 0;
  let currentRpm = 0, virtualTwoStepActive = false, isSparkCutActive = false, cutEngagedTimestamp = 0;
  let lastRpmCalcMillis = 0, cutCycleCounter = 0, inhibitCut = false, cutReason = CUT_NONE;
  let launchState = LAUNCH_OFF, launchArmTimestamp = 0, launchHoldStart = 0, peakLaunchRpm = 0;
  let previousRpm = 0, rpmDelta = 0, decelBurstStart = 0, decelBurstActive = false, decelDropFrames = 0, lastRpmRiseMillis = 0;
  let ghostCamPulseCounter = 0, ghostCutStartMicros = 0, ghostCutInProgress = false, ghostCycleId = 0;
  let cannonCutStart = 0, cannonInCutPhase = false;
  let nowUs = 0;
  const micros = () => u32(nowUs);
  const millis = () => u32(Math.floor(nowUs / 1000));
  let pin = false;

  function handleTachPulse() {
    const now = micros();
    let interval = u32(now - lastPulseMicros);
    if (interval < 4000) return;
    if (isSparkCutActive && lastValidInterval >= 4000) {
      const ratio = udiv(interval * 10, lastValidInterval);
      const skippedFactor = udiv(ratio + 5, 10);
      if (skippedFactor >= 2 && skippedFactor <= 8) interval = udiv(interval, skippedFactor);
    } else {
      if (lastValidInterval > 0 && interval < udiv(lastValidInterval * 55, 100)) return;
    }
    lastPulseMicros = now;
    pulseHistory[pulseHistIdx] = interval;
    pulseHistIdx = (pulseHistIdx + 1) & 3;
    const rollingAvg = (pulseHistory[0] + pulseHistory[1] + pulseHistory[2] + pulseHistory[3]) >>> 2;
    pulseIntervalSum += rollingAvg; pulseIntervalCount++;
    lastValidInterval = rollingAvg;
    ghostCamPulseCounter++;
  }

  function write(v) { if (v !== pin) { pin = v; hw.clamp(v); } }

  function executeIgnitionCut(shouldCut, directClamp) {
    if (shouldCut) {
      if (!isSparkCutActive) { cutEngagedTimestamp = millis(); isSparkCutActive = true; }
      if (!directClamp && config.maxCutSeconds > 0.05 && u32(millis() - cutEngagedTimestamp) > Math.floor(config.maxCutSeconds * 1000)) {
        write(false); cutReason = CUT_FLOOD_LOCK; return;
      }
      cutCycleCounter = u32(cutCycleCounter + 1);
      let clamp = false;
      if (directClamp) clamp = true;
      else if (config.cutPattern === 0) clamp = true;
      else if (config.cutPattern === 1) clamp = (cutCycleCounter % 4 !== 0);
      else if (config.cutPattern === 2) { const m = cutCycleCounter % 6; clamp = (m === 0 || m === 1 || m === 3 || m === 4); }
      else if (config.cutPattern === 3) { const m = cutCycleCounter % 6; clamp = (m !== 2 && m !== 5); }
      else if (config.cutPattern === 4) {
        if (!cannonInCutPhase) { cannonInCutPhase = true; cannonCutStart = millis(); }
        const elapsed = u32(millis() - cannonCutStart);
        if (elapsed < 1600 && currentRpm > 2100) clamp = true;
        else { clamp = false; if (elapsed > 2200) cannonInCutPhase = false; }
      }
      write(clamp);
    } else {
      isSparkCutActive = false; cannonInCutPhase = false; write(false);
    }
  }

  function engineUpdate(switchLow) {
    const currentMillis = millis();
    if (u32(currentMillis - lastRpmCalcMillis) >= 40) {
      lastRpmCalcMillis = currentMillis;
      let newRpm = 0;
      if (u32(micros() - lastPulseMicros) > 250000) { newRpm = 0; lastValidInterval = 0; }
      else {
        const sum = pulseIntervalSum, count = pulseIntervalCount, lastInt = lastValidInterval;
        pulseIntervalSum = 0; pulseIntervalCount = 0;
        if (count > 0) newRpm = udiv(udiv(60000000, udiv(sum, count)), PULSES_PER_REV);
        else if (lastInt > 0) newRpm = udiv(udiv(60000000, lastInt), PULSES_PER_REV);
        if (newRpm > HARD_MAX_RPM) newRpm = HARD_MAX_RPM;
      }
      if (currentRpm === 0 || newRpm === 0) currentRpm = newRpm;
      else currentRpm = Math.trunc((newRpm * 3 + currentRpm) / 4);
      rpmDelta = currentRpm - previousRpm; previousRpm = currentRpm;
    }
    let cutRequired = false, directClamp = false, reason = CUT_NONE;
    const physicalLaunchActive = switchLow;
    if (launchState === LAUNCH_ARMED) {
      if (u32(currentMillis - launchArmTimestamp) > 10000) launchState = LAUNCH_OFF;
      else if (currentRpm >= config.launchRpm - 200) { launchState = LAUNCH_HOLDING; launchHoldStart = currentMillis; peakLaunchRpm = currentRpm; }
    } else if (launchState === LAUNCH_HOLDING) {
      if (currentRpm > peakLaunchRpm) peakLaunchRpm = currentRpm;
      if (u32(currentMillis - launchHoldStart) >= 400 && currentRpm < config.launchRpm - config.launchDrop) {
        launchState = LAUNCH_FIRED; launchArmTimestamp = currentMillis; endInfo = { why: 'FIRED (rpm read ' + currentRpm + ')', t: currentMillis };
      } else if (config.maxCutSeconds > 0.05 && u32(currentMillis - launchHoldStart) > Math.floor(config.maxCutSeconds * 1000)) {
        launchState = LAUNCH_OFF; endInfo = { why: 'OFF (hold timeout)', t: currentMillis };
      } else if (currentRpm < MIN_CUT_RPM) {
        launchState = LAUNCH_OFF; endInfo = { why: 'OFF (rpm read ' + currentRpm + ' < MIN_CUT_RPM)', t: currentMillis };
      }
    } else if (launchState === LAUNCH_FIRED) {
      if (u32(currentMillis - launchArmTimestamp) > 2000) launchState = LAUNCH_OFF;
    }
    const twoStepTriggered = virtualTwoStepActive || physicalLaunchActive || launchState === LAUNCH_HOLDING;
    const twoStepReason = virtualTwoStepActive ? CUT_SHOW : (physicalLaunchActive ? CUT_SWITCH : CUT_LAUNCH);
    if (inhibitCut) { /* nothing */ }
    else if (config.armed) {
      if ((virtualTwoStepActive || physicalLaunchActive) && currentRpm === 0) { cutRequired = true; directClamp = true; reason = CUT_BENCH; }
      else if (currentRpm >= MIN_CUT_RPM) {
        if (twoStepTriggered && currentRpm >= config.launchRpm) { cutRequired = true; reason = twoStepReason; }
        else if (currentRpm >= config.redlineRpm) { cutRequired = true; reason = CUT_REDLINE; }
        else if (config.decelPops) {
          if (rpmDelta > 20) { lastRpmRiseMillis = currentMillis; decelDropFrames = 0; }
          else if (rpmDelta < -35) decelDropFrames++;
          else if (rpmDelta > 5) { if (decelDropFrames > 0) decelDropFrames--; }
          if (currentRpm >= config.decelRpm && u32(currentMillis - lastRpmRiseMillis) > 250 && decelDropFrames >= 4) {
            if (!decelBurstActive) { decelBurstActive = true; decelBurstStart = currentMillis; }
          }
          if (decelBurstActive) {
            if (rpmDelta > 15 || currentRpm < 2100 || u32(currentMillis - decelBurstStart) > 1200) { decelBurstActive = false; decelDropFrames = 0; }
            else { cutRequired = true; reason = CUT_DECEL; }
          }
        }
      } else if (config.ghostCam && currentRpm >= 650 && currentRpm <= 1250 && !twoStepTriggered) {
        const nm = micros();
        if (ghostCutInProgress) {
          if (u32(nm - ghostCutStartMicros) > udiv(lastValidInterval * 12, 10)) ghostCutInProgress = false;
          else { cutRequired = true; directClamp = true; reason = CUT_GHOST; }
        } else if (ghostCamPulseCounter % 5 === 4 && ghostCamPulseCounter !== ghostCycleId && lastValidInterval > 0) {
          ghostCutInProgress = true; ghostCutStartMicros = nm; ghostCycleId = ghostCamPulseCounter;
          cutRequired = true; directClamp = true; reason = CUT_GHOST;
        }
      }
    } else { decelBurstActive = false; decelDropFrames = 0; ghostCutInProgress = false; }
    cutReason = reason;
    executeIgnitionCut(cutRequired, directClamp);
  }
  let endInfo = null;

  return {
    engineSetConfig: (c) => { config = Object.assign({}, c); },
    engineSetShowButton: (h) => { virtualTwoStepActive = h; },
    engineLaunchArm: () => { launchState = LAUNCH_ARMED; launchArmTimestamp = millis(); peakLaunchRpm = 0; },
    engineLaunchDisarm: () => { launchState = LAUNCH_OFF; },
    engineSetInhibit: (i) => { inhibitCut = i; if (i) write(false); },
    engineGetTelemetry: () => ({ rpm: currentRpm, cutActive: isSparkCutActive, launchState, reason: cutReason,
      launchLeftMs: 0 }),
    tachIsr: (now, pinLow) => { nowUs = now; handleTachPulse(); },
    loop: (now, switchLow) => { nowUs = now; engineUpdate(switchLow); },
    endInfo: () => endInfo,
  };
}

module.exports = { createOriginalCore };
