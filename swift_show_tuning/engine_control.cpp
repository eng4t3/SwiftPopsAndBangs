// =========================================================================================
// ENGINE CONTROL CORE - see engine_control.h
// =========================================================================================
#include "engine_control.h"

static TuningConfig config;

// Runtime dynamic state
static volatile unsigned long lastPulseMicros = 0;
static volatile unsigned long pulseIntervalSum = 0;
static volatile unsigned int  pulseIntervalCount = 0;
static volatile unsigned long lastValidInterval = 0;
static volatile unsigned long pulseHistory[4] = {35000, 35000, 35000, 35000};
static volatile uint8_t       pulseHistIdx = 0;

static int  currentRpm = 0;
static bool virtualTwoStepActive = false;
static volatile bool isSparkCutActive = false;
static unsigned long cutEngagedTimestamp = 0;
static unsigned long lastRpmCalcMillis = 0;
static unsigned int  cutCycleCounter = 0;
static bool inhibitCut = false;
static uint8_t cutReason = CUT_NONE;

// Hands-Free Smart Launch State Machine
static LaunchState launchState = LAUNCH_OFF;
static unsigned long launchArmTimestamp = 0;
static unsigned long launchHoldStart = 0;
static int peakLaunchRpm = 0;

// Automatic Overrun Decel Tracking
static int  previousRpm = 0;
static int  rpmDelta = 0;
static unsigned long decelBurstStart = 0;
static bool decelBurstActive = false;
static int  decelDropFrames = 0;            // Consecutive 40ms frames of real decel
static unsigned long lastRpmRiseMillis = 0; // Timestamp of when engine was last actively accelerating

// Ghost Cam / V8 Lumpy Idle State
static volatile unsigned int ghostCamPulseCounter = 0;
static unsigned long ghostCutStartMicros = 0;
static bool ghostCutInProgress = false;
static unsigned int ghostCycleId = 0;

// Pattern 4: Fireball Cannon State
static unsigned long cannonCutStart = 0;
static bool cannonInCutPhase = false;

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
  c.cutPattern    = 1;      // Pattern 1 (Flame Spitter) - optimal for overrun flames & pops
  c.maxCutSeconds = 3.0f;
  c.ghostCam      = false;
  c.launchDrop    = 400;
  return c;
}

void engineClampConfig(TuningConfig& c) {
  c.launchRpm  = constrain(c.launchRpm, 2500, 6500);
  c.redlineRpm = constrain(c.redlineRpm, 3000, 7500);
  c.decelRpm   = constrain(c.decelRpm, 2500, 6000);
  c.cutPattern = constrain(c.cutPattern, 0, 4);
  c.launchDrop = constrain(c.launchDrop, 300, 1500);
  if (!(c.maxCutSeconds >= 0.0f) || c.maxCutSeconds >= 6.0f) c.maxCutSeconds = 0.0f;  // NaN / >=6 = unlimited
  if (c.maxCutSeconds > 0.0f && c.maxCutSeconds < 1.0f) c.maxCutSeconds = 1.0f;
}

void engineSetConfig(const TuningConfig& cfg) {
  config = cfg;
  engineClampConfig(config);
}

TuningConfig engineGetConfig() { return config; }

void engineSetShowButton(bool held) { virtualTwoStepActive = held; }

void engineLaunchArm() {
  launchState = LAUNCH_ARMED;
  launchArmTimestamp = millis();
  peakLaunchRpm = 0;
}

void engineLaunchDisarm() { launchState = LAUNCH_OFF; }

void engineSetInhibit(bool inhibit) {
  inhibitCut = inhibit;
  if (inhibit) {
    digitalWrite(PIN_SPARK_CUT, LOW);
  }
}

int engineGetRpm() { return currentRpm; }

EngineTelemetry engineGetTelemetry() {
  EngineTelemetry t;
  t.rpm = currentRpm;
  t.cutActive = isSparkCutActive;
  t.launchState = (uint8_t)launchState;
  unsigned long armedFor = millis() - launchArmTimestamp;
  t.launchLeftMs = (launchState == LAUNCH_ARMED && armedFor < 10000) ? (10000 - armedFor) : 0;
  t.reason = cutReason;
  return t;
}

// =========================================================================================
// TACHOMETER HARDWARE INTERRUPT SERVICE ROUTINE (ISR)
// =========================================================================================
static void IRAM_ATTR handleTachPulse() {
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
// IGNITION CUT CONTROL LOGIC
// =========================================================================================
static void executeIgnitionCut(bool shouldCut, bool directClamp = false) {
  if (shouldCut) {
    if (!isSparkCutActive) {
      cutEngagedTimestamp = millis();
      isSparkCutActive = true;
    }

    // Safety check: Anti-flood timeout to prevent cylinder washdown (if enabled)
    if (!directClamp && (config.maxCutSeconds > 0.05f) && (millis() - cutEngagedTimestamp > (unsigned long)(config.maxCutSeconds * 1000.0f))) {
      digitalWrite(PIN_SPARK_CUT, LOW);  // Disengage cut for safety
      digitalWrite(PIN_STATUS_LED, LOW);
      cutReason = CUT_FLOOD_LOCK;
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
void engineBegin(const TuningConfig& cfg) {
  engineSetConfig(cfg);

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
}

// =========================================================================================
// CONTROL STEP (called from loop())
// =========================================================================================
void engineUpdate() {
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
  uint8_t reason = CUT_NONE;

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
    // Once holding on limiter for at least 400ms, if engine drops > launchDrop RPM below the launch RPM setpoint, release!
    if (currentMillis - launchHoldStart >= 400 && currentRpm < (config.launchRpm - config.launchDrop)) {
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
  uint8_t twoStepReason = virtualTwoStepActive ? CUT_SHOW : (physicalLaunchActive ? CUT_SWITCH : CUT_LAUNCH);

  if (inhibitCut) {
    // Firmware update or other external inhibit: never cut
  } else if (config.armed) {
    // Bench-test mode: If engine is stopped (0 RPM on the desk) and 2-step button is pressed or physical switch active, activate cut for hardware testing!
    if ((virtualTwoStepActive || physicalLaunchActive) && currentRpm == 0) {
      cutRequired = true;
      directClamp = true;
      reason = CUT_BENCH;
    }
    else if (currentRpm >= MIN_CUT_RPM) {
      // Condition A: Stationary Launch 2-Step (Show mode hold OR Hands-Free launch OR Physical switch)
      if (twoStepTriggered && currentRpm >= config.launchRpm) {
        cutRequired = true;
        reason = twoStepReason;
      }
      // Condition B: Main Redline Rev Limiter (Hard Cut at redline - 100% AUTOMATIC)
      else if (currentRpm >= config.redlineRpm) {
        cutRequired = true;
        reason = CUT_REDLINE;
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
            reason = CUT_DECEL;
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
          reason = CUT_GHOST;
        }
      } else {
        // Start a new 1-spark cut every 5 pulses (rotating through all 4 cylinders)
        if ((ghostCamPulseCounter % 5 == 4) && (ghostCamPulseCounter != ghostCycleId) && (lastValidInterval > 0)) {
          ghostCutInProgress = true;
          ghostCutStartMicros = nowMicros;
          ghostCycleId = ghostCamPulseCounter; // Prevent re-triggering on same pulse
          cutRequired = true;
          directClamp = true;
          reason = CUT_GHOST;
        }
      }
    }
  } else {
    decelBurstActive = false;
    decelDropFrames = 0;
    ghostCutInProgress = false;
  }

  cutReason = reason;
  executeIgnitionCut(cutRequired, directClamp);
}
