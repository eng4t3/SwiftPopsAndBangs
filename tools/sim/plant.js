// =========================================================================================
// Plant model for the engine-core simulator: G13BA crank dynamics, ECU dwell (IB signal),
// igniter + coil, clamp transistor, tach output and a clutch-drop load.
// =========================================================================================
// Units: time in microseconds (float), engine speed in RPM, accelerations in RPM/s.
//   * 2 ignition events ("slots") per crank revolution; slot period T = 30e6 / rpm us.
//   * ECU raises IB `dwell` us before each spark and drops it at the spark angle.
//   * Effective IB = ECU IB AND NOT clamp. The coil charges while effective IB is high.
//     Falling edge of effective IB = spark. If the falling edge is caused by the clamp while
//     the ECU still holds IB high, the spark is PREMATURE (wrong crank angle) - counted.
//     A spark after a shortened dwell is WEAK - counted.
//   * A tach edge (GPIO18 FALLING) is produced by every spark with >= 0.2 ms of coil charge.
//   * Combustion torque of a spark drives the crank during the following slot:
//       dRPM/dt = (fire * 8000 * throttleEff * cylFactor - (1500 + 0.4 rpm) - load - road) / inertia
//     -> WOT no-load: ~+5000 RPM/s at 3800 all firing, ~-3000 RPM/s all cut (spec: 4000-6000 / 2500-4000).
// =========================================================================================
'use strict';

class Rng {                       // deterministic xorshift32
  constructor(seed) { this.s = (seed >>> 0) || 1; }
  next() { let x = this.s; x ^= x << 13; x >>>= 0; x ^= x >>> 17; x ^= x << 5; x >>>= 0; this.s = x; return x / 4294967296; }
  gauss() { let u = 0, v = 0; while (u === 0) u = this.next(); v = this.next(); return Math.sqrt(-2 * Math.log(u)) * Math.cos(2 * Math.PI * v); }
}

const COMB = 8000;                // combustion accel at WOT, all cylinders firing (RPM/s)
const CYL_FACTOR = [1.02, 0.98, 1.01, 0.99];
const CYL_TACH_OFFSET_US = [0, 35, -25, 15];   // distributor backlash / rotor phase error
const DWELL_US = +(process.env.SIM_DWELL_US || 3000);         // ECU dwell (G13BA value unknown: vary it)
const DWELL_MAX_FRAC = +(process.env.SIM_DWELL_FRAC || 0.6);   // dwell cap as a fraction of the slot
const OPTO_DELAY_US = 30;

class Plant {
  constructor(opt, rng) {
    this.rng = rng;
    this.o = Object.assign({
      rpm0: 850, jitterUs: 15, ringProb: 0.0, noiseRate: 0.0, dwellGlitchProb: 0.0, missProb: 0.0,
      inertia: 1.0, road: 0,
    }, opt || {});
    this.rpm = this.o.rpm0;
    this.running = this.rpm > 0;
    this.phase = 0.3;
    this.slot = 0;
    this.thrCmd = 0; this.thr = 0;
    this.inertia = this.o.inertia; this.road = this.o.road;
    this.clamp = false;
    this.ibEff = false; this.charge = 0; this.dwellActiveEcu = false;
    this.fire = 1.0;              // torque factor of the current slot (from the last spark)
    this.premInSlot = false;
    this.clutch = null;           // {t0, depth, dur, rise, L1}
    this.edges = [];              // pending tach edges {t, low}
    this.st = { slots: 0, full: 0, weak: 0, premature: 0, cut: 0, misfire: 0, cutByCyl: [0, 0, 0, 0], firedByCyl: [0, 0, 0, 0] };
    this.slotLog = [];            // per slot: {t, cyl, fired}
    this.logSlots = false;
  }

  friction(r) { return r > 30 ? 1500 + 0.4 * r : 0; }
  idleThrottle(r) { return Math.min(0.45, Math.max(0.03, 0.23 + (850 - r) * 0.0008)); }

  clutchDrop(t, depth, durMs, riseRpmS) {
    const dur = durMs * 1000;
    // slip phase load: net decel with all cylinders firing at WOT = depth / (dur - 20 ms)
    const netDecel = depth / ((dur - 20000) / 1e6);
    const L1 = netDecel + (COMB * 1.0 - this.friction(this.rpm));
    this.clutch = { t0: t, dur, rise: riseRpmS, L1 };
  }

  load(t) {
    const c = this.clutch;
    if (!c || t < c.t0) return 0;
    const el = t - c.t0;
    if (el < c.dur) return c.L1 * Math.min(1, el / 40000);
    // coupled to the accelerating car: all-firing net = +rise
    const thrEff = Math.max(this.thr, this.idleThrottle(this.rpm));
    return Math.max(0, COMB * thrEff - this.friction(this.rpm) - c.rise);
  }

  // low: GPIO18 level when the ISR samples it at entry; lowLate: level ~10 us later, just before the
  // clamp would be engaged (the dwell interlock). A glitch during dwell is over by then (reads HIGH).
  pushEdge(t, low, lowLate) { this.edges.push({ t, low, lowLate: lowLate === undefined ? low : lowLate }); }

  spark(t, charge, premature) {
    // charge in us
    const cyl = this.slot % 4;
    if (charge >= 200) {
      let te = t + OPTO_DELAY_US + CYL_TACH_OFFSET_US[cyl] + this.o.jitterUs * this.rng.gauss();
      if (!(this.o.missProb > 0 && this.rng.next() < this.o.missProb)) this.pushEdge(te, true);
      // flyback ringing: extra falling edges up to 2 ms after the spark, pin level random (LED flickers)
      if (this.o.ringProb > 0 && this.rng.next() < this.o.ringProb) this.pushEdge(te + 150 + 1850 * this.rng.next(), this.rng.next() < 0.5);
    }
    const strength = charge >= 0.35 * DWELL_US ? Math.min(1, charge / (0.7 * DWELL_US)) : 0;
    return { strength, premature };
  }

  // one integration step of dt us at absolute time t (us)
  step(t, dt) {
    if (this.running) {
      const T = 30e6 / Math.max(this.rpm, 30);
      const dwell = Math.min(DWELL_US, DWELL_MAX_FRAC * T);
      this.phase += dt / T;
      let sparkEvent = false;
      if (this.phase >= 1) { this.phase -= 1; sparkEvent = true; }
      const ibEcu = !sparkEvent && (1 - this.phase) * T <= dwell;
      const prevEff = this.ibEff;
      const eff = ibEcu && !this.clamp;
      if (eff && !prevEff) this.charge = 0;
      if (eff) this.charge += dt;
      this.ibEff = eff;
      if (ibEcu && this.o.dwellGlitchProb > 0 && !this.dwellActiveEcu && this.rng.next() < this.o.dwellGlitchProb) {
        this.pushEdge(t + 5, false);          // spurious "falling" IRQ on the slow rising edge (pin reads high)
      }
      this.dwellActiveEcu = ibEcu;
      if (prevEff && !eff && !sparkEvent) {
        // clamp pulled IB low while the ECU was dwelling: premature spark
        const s = this.spark(t, this.charge, true);
        this.st.premature++;
        this.premFire = s.strength * 0.6;
        this.premInSlot = true;
        this.charge = 0;
      }
      if (sparkEvent) {
        const cyl = this.slot % 4;
        let fired = 0;
        if (prevEff) {
          const s = this.spark(t, this.charge, false);
          fired = s.strength;
          if (s.strength >= 0.999) this.st.full++;
          else if (s.strength > 0) this.st.weak++;
          else this.st.misfire++;
        } else if (this.premInSlot) {
          fired = this.premFire;
        } else {
          this.st.cut++; this.st.cutByCyl[cyl]++;
        }
        if (fired > 0) this.st.firedByCyl[cyl]++;
        if (this.logSlots) this.slotLog.push({ t, cyl, fired: fired > 0, prem: this.premInSlot });
        this.charge = 0; this.premInSlot = false;
        this.fire = fired;
        this.slot++; this.st.slots++;
      }
      // crank dynamics
      this.thr += (this.thrCmd - this.thr) * Math.min(1, dt / 40000);
      const thrEff = Math.max(this.thr, this.idleThrottle(this.rpm));
      const acc = (this.fire * COMB * thrEff * CYL_FACTOR[this.slot % 4] - this.friction(this.rpm) - this.load(t) - this.road) / this.inertia;
      this.rpm += acc * dt / 1e6;
      if (this.rpm < 30) { this.rpm = 0; this.running = false; this.stalledAt = t; }
    }
    // random noise edges: GPIO18 is LOW while the igniter is not dwelling (coil- at 12 V, opto LED on) and
    // HIGH (weak pull-up) during dwell. A glitch during dwell is over when the ISR samples the pin (reads
    // HIGH); to stress the scheduler, a share of the glitches (noiseLowShare) is assumed to still read LOW.
    if (this.o.noiseRate > 0 && this.rng.next() < this.o.noiseRate * dt / 1e6) {
      const share = this.o.noiseLowShare === undefined ? 0.5 : this.o.noiseLowShare;
      if (!this.ibEff) this.pushEdge(t, true, true);           // igniter idle: pin is driven LOW anyway
      else this.pushEdge(t, this.rng.next() < share, false);  // glitch during dwell: over ~10 us later
    }
  }

  stop() { this.running = false; this.rpm = 0; this.fire = 0; }
  start(rpm) { this.running = true; this.rpm = rpm; this.fire = 1; this.phase = 0.3; }

  // pop edges due by time t, sorted
  dueEdges(t) {
    if (this.edges.length === 0) return null;
    let due = null;
    for (let i = this.edges.length - 1; i >= 0; i--) {
      if (this.edges[i].t <= t) { (due || (due = [])).push(this.edges[i]); this.edges.splice(i, 1); }
    }
    if (due) due.sort((a, b) => a.t - b.t);
    return due;
  }
}

module.exports = { Plant, Rng, COMB };
