#!/usr/bin/env node
// =========================================================================================
// Engine-core simulator: runs the firmware control algorithm (engine_core.js, the 1:1 mirror
// of swift_show_tuning/engine_control.cpp) against the plant model (plant.js) through a set
// of driving scenarios and prints a pass/fail table.
//
//   node tools/sim/run.js                          all scenarios + original firmware + launchDrop sweep
//   node tools/sim/run.js --quick                  skip the sweep
//   node tools/sim/run.js --trace "<name>" [t0 t1] 1 ms CSV trace of one scenario (names: see the table)
//   SIM_DWELL_US=4000 SIM_DWELL_FRAC=0.75 node ...  vary the (unknown) ECU dwell
//   python tools/sim/check_sync.py                 verify engine_core.js still mirrors engine_control.cpp
//
// Every run of the new core starts 3 s before the 32-bit microsecond counter wraps, so wrap-around
// handling is exercised in every scenario. Exit code 1 if any scenario fails.
// =========================================================================================
'use strict';

const core = require('./engine_core');
const { createOriginalCore } = require('./original_core');
const { Plant, Rng } = require('./plant');

const T0 = 4294967296 - 3000000;       // us: u32 wrap 3 s into every run
const DT = 10;                          // plant integration step (us)
const TICK = core.TICK_US;

const PAT_NAME = ['hard', 'flames', 'gunfire', 'ak47', 'cannon'];
const STATE_NAME = ['OFF', 'ARMED', 'HOLDING', 'FIRED'];

// -----------------------------------------------------------------------------------------
// Simulation harness
// -----------------------------------------------------------------------------------------
function simulate(sc) {
  const rng = new Rng(sc.seed || 12345);
  const plant = new Plant(sc.plant, rng);
  plant.logSlots = true;
  let swLow = false, pinLate = true;
  let t = 0;
  const rec = { t: [], rpm: [], tel: [], clamp: [], state: [], reason: [], cut: [], show: [], est: [], end: [] };
  const clampIntervals = [];   // [on, off] in ms
  let clampOnAt = -1, clampThisMs = false;
  const hw = {
    clamp: (on) => {
      plant.clamp = on;
      if (on) { clampOnAt = t; clampThisMs = true; }
      else if (clampOnAt >= 0) { clampIntervals.push([clampOnAt / 1000, t / 1000]); clampOnAt = -1; }
    },
    switchActive: () => swLow,
    tachLowNow: () => pinLate,
    log: sc.log,
  };
  const orig = !!sc.original;
  const t0 = orig ? 0 : T0;            // the original firmware is run from boot (micros() starts at 0)
  const c = orig ? createOriginalCore(hw) : core.createCore(hw);
  const cfg = Object.assign(core.engineDefaultConfig(), sc.cfg || {});
  c.engineSetConfig(cfg);

  const acts = (sc.actions || []).slice().sort((a, b) => a.at - b.at);
  let ai = 0;
  let phoneHeld = false, phoneConnected = true, nextRefresh = 0, lastRefresh = -1;
  let nextTick = 0, nextLoop = 0, nextRec = 0;
  const events = [];
  let lastState = -1;
  const dur = sc.dur * 1e6;
  const inject = [];
  for (t = 0; t < dur; t += DT) {
    while (ai < acts.length && acts[ai].at * 1e6 <= t) {
      const a = acts[ai++];
      switch (a.do) {
        case 'thr': plant.thrCmd = a.v; break;
        case 'arm': c.engineLaunchArm(); break;
        case 'disarm': c.engineLaunchDisarm(); break;
        case 'clutch': plant.clutchDrop(t, a.depth, a.durMs, a.rise || 1200); break;
        case 'show': phoneHeld = a.v; if (a.v) nextRefresh = t; else c.engineSetShowButton(false); break;
        case 'disconnect': phoneConnected = false; break;
        case 'switch': swLow = a.v; break;
        case 'stop': plant.stop(); break;
        case 'start': plant.start(a.rpm); break;
        case 'edge': inject.push(t); plant.pushEdge(t, true); break;
        case 'inhibit': c.engineSetInhibit(a.v); break;
        case 'cfg': Object.assign(cfg, a.v); c.engineSetConfig(cfg); break;
        case 'gear': plant.inertia = a.inertia; plant.road = a.road || 0; break;
      }
    }
    if (phoneHeld && phoneConnected && t >= nextRefresh) { c.engineSetShowButton(true); lastRefresh = t; nextRefresh = t + 200000; }
    plant.step(t, DT);
    // deliver tach edges and control ticks strictly in time order (ISRs are serialized on one core)
    const due = plant.dueEdges(t) || [];
    let ei = 0;
    for (;;) {
      const te = ei < due.length ? Math.round(due[ei].t) + 3 : Infinity;   // ~3 us ISR latency
      const tk = orig ? nextLoop : nextTick;
      if (tk > t && te === Infinity) break;
      if (te <= tk || tk > t) { pinLate = due[ei].lowLate; c.tachIsr((t0 + te) >>> 0, due[ei].low); pinLate = true; ei++; continue; }
      if (orig) { c.loop((t0 + Math.round(nextLoop)) >>> 0, swLow); nextLoop += 30 + ((nextLoop / 7) % 20); }
      else { c.tickIsr((t0 + nextTick) >>> 0); nextTick += TICK; }
    }
    if (t >= nextRec) {
      nextRec += 1000;
      const tel = c.engineGetTelemetry();
      rec.t.push(t / 1e6); rec.rpm.push(plant.rpm); rec.tel.push(tel.rpm);
      rec.clamp.push(clampThisMs || plant.clamp ? 1 : 0); clampThisMs = false;
      rec.state.push(tel.launchState); rec.reason.push(tel.reason); rec.cut.push(tel.cutActive ? 1 : 0);
      rec.show.push(tel.showActive ? 1 : 0); rec.est.push(tel.rpmEstimated ? 1 : 0); rec.end.push(tel.launchEnd);
      if (tel.launchState !== lastState) { events.push({ t: t / 1e6, state: tel.launchState, end: tel.launchEnd }); lastState = tel.launchState; }
    }
  }
  if (clampOnAt >= 0) clampIntervals.push([clampOnAt / 1000, t / 1000]);
  return { sc, rec, events, plant, clampIntervals, diag: c.engineGetDiag ? c.engineGetDiag() : null,
    lastRefresh: lastRefresh / 1e6, endInfo: c.endInfo ? c.endInfo() : null, core: c, inject };
}

// -----------------------------------------------------------------------------------------
// Metrics helpers
// -----------------------------------------------------------------------------------------
function idx(r, ts) { return Math.max(0, Math.min(r.t.length - 1, Math.round(ts * 1000))); }
function win(r, key, a, b) { return r[key].slice(idx(r, a), idx(r, b)); }
function stats(arr) {
  if (!arr.length) return { min: NaN, max: NaN, mean: NaN };
  let mn = Infinity, mx = -Infinity, s = 0;
  for (const v of arr) { if (v < mn) mn = v; if (v > mx) mx = v; s += v; }
  return { min: mn, max: mx, mean: s / arr.length };
}
function firstTime(r, pred, from = 0) { for (let i = idx(r, from); i < r.t.length; i++) if (pred(i)) return r.t[i]; return null; }
function stateAt(res, ts) { return res.rec.state[idx(res.rec, ts)]; }
function eventTime(res, st, from = 0) { const e = res.events.find((e) => e.state === st && e.t >= from); return e ? e.t : null; }
function maxClampMs(res, a = 0, b = 1e9) {
  let m = 0; for (const [on, off] of res.clampIntervals) if (on >= a * 1000 && on < b * 1000) m = Math.max(m, off - on); return m;
}
function slotsIn(res, a, b) { return res.plant.slotLog.filter((s) => s.t >= a * 1e6 && s.t < b * 1e6); }
function f0(v) { return v === null || v === undefined || Number.isNaN(v) ? '-' : Math.round(v).toString(); }

// -----------------------------------------------------------------------------------------
// Scenarios
// -----------------------------------------------------------------------------------------
const PROFILES = {
  mild: { depth: 800, durMs: 300 },
  typical: { depth: 1000, durMs: 200 },
  harsh: { depth: 1500, durMs: 150 },
};
const NOISY = { jitterUs: 40, ringProb: 0.3, noiseRate: 5, dwellGlitchProb: 0.3, missProb: 0.01 };

function launchScenario(pattern, profile, opts = {}) {
  const p = PROFILES[profile];
  return {
    name: opts.name || `launch ${PAT_NAME[pattern]} ${profile}`, seed: opts.seed || 1000 + pattern * 7 + profile.length,
    dur: 9.0, cfg: Object.assign({ cutPattern: pattern }, opts.cfg || {}), plant: Object.assign({ rpm0: 850 }, opts.plant || {}),
    original: !!opts.original,
    actions: [
      { at: 1.0, do: 'arm' }, { at: 2.0, do: 'thr', v: 1 },
      { at: 7.0, do: 'clutch', depth: p.depth, durMs: p.durMs, rise: 1200 },
    ],
    kind: 'launch', dropAt: 7.0,
  };
}

function checkLaunch(res) {
  const r = res.rec, sc = res.sc, cfg = Object.assign(core.engineDefaultConfig(), sc.cfg);
  const tHold = eventTime(res, core.LAUNCH_HOLDING, 1.0);
  const out = { pass: true, notes: [] };
  if (tHold === null) { out.pass = false; out.notes.push('never HOLDING'); return out; }
  // left HOLDING before the clutch drop?
  const early = res.events.find((e) => e.t > tHold && e.t < sc.dropAt && e.state !== core.LAUNCH_HOLDING);
  if (early) { out.pass = false; out.notes.push(`left HOLDING at ${early.t.toFixed(3)}s -> ${STATE_NAME[early.state]}`); }
  const w0 = tHold + 0.5, w1 = sc.dropAt;
  const tr = stats(win(r, 'rpm', w0, w1)), tl = stats(win(r, 'tel', w0, w1));
  out.hold = tr; out.holdTel = tl;
  const lo = cfg.launchRpm - 300, hi = cfg.launchRpm + 175;
  if (!(tr.max <= hi && tr.min >= lo)) { out.pass = false; out.notes.push(`hold band ${f0(tr.min)}..${f0(tr.max)} outside ${lo}..${hi}`); }
  if (tl.min <= 0) { out.pass = false; out.notes.push('reported 0 rpm while holding'); }
  const endEv = res.events.find((e) => e.t >= sc.dropAt - 0.001 && e.state !== core.LAUNCH_HOLDING);
  const tRel = endEv ? endEv.t : null;
  const tFired = eventTime(res, core.LAUNCH_FIRED, sc.dropAt - 0.001);
  out.latency = tRel === null ? null : (tRel - sc.dropAt) * 1000;             // limiter released
  out.firedMs = tFired === null ? null : (tFired - sc.dropAt) * 1000;         // FIRED reported
  out.label = tFired === null ? `no FIRED (end ${r.end[r.end.length - 1]})` : (out.firedMs - out.latency > 5 ? 'FIRED(verified)' : 'FIRED');
  if (tRel === null) { out.pass = false; out.notes.push('launch never released'); }
  else if (out.latency > 170) { out.pass = false; out.notes.push(`released late ${f0(out.latency)}ms`); }
  let lc = null; for (let i = idx(r, sc.dropAt); i < idx(r, sc.dropAt + 0.6); i++) if (r.clamp[i]) lc = r.t[i];
  out.lastCutMs = lc === null ? 0 : (lc - sc.dropAt) * 1000;
  if (tFired === null || out.firedMs > 800) { out.pass = false; out.notes.push('FIRED not reported'); }
  // no launch cut after FIRED
  const after = tFired === null ? sc.dropAt + 0.3 : tFired;
  const badCut = firstTime(r, (i) => r.clamp[i] && r.reason[i] === core.CUT_LAUNCH, after + 0.02);
  if (badCut !== null) { out.pass = false; out.notes.push(`launch cut after FIRED at ${badCut.toFixed(3)}`); }
  const stall = res.plant.stalledAt !== undefined;
  if (stall) { out.pass = false; out.notes.push('STALLED'); }
  out.prem = res.plant.st.premature; out.weak = res.plant.st.weak;
  if (out.prem > 0) { out.pass = false; out.notes.push(`${out.prem} premature sparks`); }
  out.dip = stats(win(r, 'rpm', sc.dropAt, sc.dropAt + 0.6)).min;
  return out;
}

function redlineScenario(pattern, opts = {}) {
  const gear = !!opts.gear;
  return {
    name: opts.name || `redline ${PAT_NAME[pattern]}${gear ? ' in-gear' : ''}`, seed: opts.seed || 2000 + pattern,
    dur: 6.5, cfg: Object.assign({ cutPattern: pattern }, opts.cfg || {}),
    plant: Object.assign(gear ? { rpm0: 3000, inertia: 3, road: 400 } : { rpm0: 850 }, opts.plant || {}),
    original: !!opts.original,
    actions: [{ at: 1.0, do: 'thr', v: 1 }], kind: 'redline',
  };
}

function checkRedline(res) {
  const r = res.rec, cfg = Object.assign(core.engineDefaultConfig(), res.sc.cfg);
  const out = { pass: true, notes: [] };
  const tReach = firstTime(r, (i) => r.rpm[i] >= cfg.redlineRpm - 50, 1.0);
  if (tReach === null) { out.pass = false; out.notes.push('never reached redline'); return out; }
  const w0 = tReach + 0.5, w1 = 6.5;
  const tr = stats(win(r, 'rpm', w0, w1)), tl = stats(win(r, 'tel', w0, w1));
  out.hold = tr; out.holdTel = tl;
  const peak = stats(win(r, 'rpm', tReach, w1)).max;
  out.peak = peak;
  const cannon = cfg.cutPattern === 4;
  if (peak > cfg.redlineRpm + 150) { out.pass = false; out.notes.push(`overshoot ${f0(peak)}`); }
  if (!cannon && tr.min < cfg.redlineRpm - 400) { out.pass = false; out.notes.push(`dips to ${f0(tr.min)}`); }
  if (!cannon && Math.abs(tr.mean - (cfg.redlineRpm - 60)) > 150) { out.pass = false; out.notes.push(`mean ${f0(tr.mean)}`); }
  if (tl.min <= 0) { out.pass = false; out.notes.push('reported 0 rpm'); }
  out.maxClamp = maxClampMs(res, w0, w1);
  const maxAllowed = cannon ? 1700 : 60;
  if (out.maxClamp > maxAllowed) { out.pass = false; out.notes.push(`clamp stuck ${f0(out.maxClamp)}ms`); }
  out.prem = res.plant.st.premature; out.weak = res.plant.st.weak;
  if (out.prem > 0) { out.pass = false; out.notes.push(`${out.prem} premature`); }
  const decel = firstTime(r, (i) => r.reason[i] === core.CUT_DECEL, 1.0);
  if (decel !== null) { out.pass = false; out.notes.push(`false decel pops at ${decel.toFixed(2)}`); }
  if (res.plant.stalledAt !== undefined) { out.pass = false; out.notes.push('STALLED'); }
  return out;
}

// bursts of reason DECEL (clamp intervals grouped when gaps < 150 ms)
function bursts(res, reason) {
  const r = res.rec; const list = []; let cur = null;
  for (let i = 0; i < r.t.length; i++) {
    const on = r.clamp[i] && r.reason[i] === reason;
    if (on) { if (cur && r.t[i] - cur.end < 0.15) cur.end = r.t[i]; else { cur = { start: r.t[i], end: r.t[i] }; list.push(cur); } }
  }
  return list;
}

function decelScenarios() {
  return [
    { name: 'decel neutral x2', seed: 3001, dur: 7.0, cfg: {}, plant: { rpm0: 850 }, kind: 'decel',
      actions: [{ at: 1.0, do: 'thr', v: 1 }, { at: 1.85, do: 'thr', v: 0 }, { at: 4.0, do: 'thr', v: 1 }, { at: 4.85, do: 'thr', v: 0 }],
      lifts: [1.85, 4.85], expectBursts: 2 },
    { name: 'decel re-accel abort', seed: 3002, dur: 4.0, cfg: {}, plant: { rpm0: 850 }, kind: 'decel',
      actions: [{ at: 1.0, do: 'thr', v: 1 }, { at: 1.85, do: 'thr', v: 0 }, { at: 2.35, do: 'thr', v: 1 }, { at: 2.6, do: 'thr', v: 0.3 }],
      lifts: [1.85], expectBursts: 1, reaccel: 2.35 },
    { name: 'decel in-gear coast', seed: 3003, dur: 6.0, cfg: {}, plant: { rpm0: 2500, inertia: 3, road: 300 }, kind: 'decel',
      actions: [{ at: 0.5, do: 'thr', v: 1 }, { at: 3.3, do: 'thr', v: 0 }], lifts: [3.3], expectBursts: 1 },
    { name: 'decel pops disabled', seed: 3004, dur: 4.0, cfg: { decelPops: false }, plant: { rpm0: 850 }, kind: 'decel',
      actions: [{ at: 1.0, do: 'thr', v: 1 }, { at: 1.85, do: 'thr', v: 0 }], lifts: [1.85], expectBursts: 0 },
    { name: 'decel gentle part-throttle', seed: 3005, dur: 6.0, cfg: {}, plant: { rpm0: 3000, inertia: 3, road: 300 }, kind: 'decel',
      actions: [{ at: 0.2, do: 'thr', v: 0.55 }, { at: 3.0, do: 'thr', v: 0.45 }], lifts: [], expectBursts: 0 },
  ];
}

function checkDecel(res) {
  const sc = res.sc, out = { pass: true, notes: [] };
  const b = bursts(res, core.CUT_DECEL);
  out.bursts = b;
  if (b.length !== sc.expectBursts) { out.pass = false; out.notes.push(`${b.length} bursts (expected ${sc.expectBursts})`); }
  out.lat = []; out.len = []; out.endRpm = [];
  b.forEach((bb, k) => {
    const lift = sc.lifts[k];
    if (lift !== undefined) out.lat.push((bb.start - lift) * 1000);
    out.len.push((bb.end - bb.start) * 1000);
    out.endRpm.push(res.rec.rpm[idx(res.rec, bb.end)]);
    if (bb.end - bb.start > 1.25) { out.pass = false; out.notes.push(`burst ${k} ${f0((bb.end - bb.start) * 1000)}ms`); }
    if (res.rec.rpm[idx(res.rec, bb.end)] < 2000) { out.pass = false; out.notes.push(`burst ${k} ends at ${f0(res.rec.rpm[idx(res.rec, bb.end)])}`); }
    if (lift !== undefined && bb.start - lift > 0.4) { out.pass = false; out.notes.push(`burst ${k} late ${f0((bb.start - lift) * 1000)}ms`); }
  });
  if (sc.reaccel && b.length) {
    out.abortMs = (b[0].end - sc.reaccel) * 1000;
    if (out.abortMs > 250) { out.pass = false; out.notes.push(`abort after re-accel ${f0(out.abortMs)}ms`); }
  }
  out.prem = res.plant.st.premature;
  if (out.prem > 0) { out.pass = false; out.notes.push(`${out.prem} premature`); }
  if (res.plant.stalledAt !== undefined) { out.pass = false; out.notes.push('STALLED'); }
  return out;
}

function ghostScenario(opts = {}) {
  return { name: opts.name || 'ghost cam idle', seed: 4001, dur: 11.0, cfg: { ghostCam: true }, plant: Object.assign({ rpm0: 850 }, opts.plant || {}),
    original: !!opts.original, kind: 'ghost',
    actions: [{ at: 6.0, do: 'thr', v: 0.35 }, { at: 6.25, do: 'thr', v: 0 }] };
}

function checkGhost(res) {
  const out = { pass: true, notes: [] };
  const sl = slotsIn(res, 2.0, 6.0);
  let cut = 0, maxRun = 0, run = 0; const cyl = [0, 0, 0, 0]; const gaps = {};
  let lastCut = -1;
  sl.forEach((s, i) => {
    if (!s.fired) { cut++; cyl[s.cyl]++; run++; maxRun = Math.max(maxRun, run); if (lastCut >= 0) gaps[i - lastCut] = (gaps[i - lastCut] || 0) + 1; lastCut = i; }
    else run = 0;
  });
  out.frac = cut / sl.length; out.maxRun = maxRun; out.cyl = cyl; out.gaps = gaps;
  if (Math.abs(out.frac - 0.2) > 0.02) { out.pass = false; out.notes.push(`cut fraction ${(out.frac * 100).toFixed(1)}%`); }
  if (maxRun > 1) { out.pass = false; out.notes.push(`${maxRun} consecutive cut slots`); }
  if (Math.min(...cyl) < 0.8 * Math.max(...cyl)) { out.pass = false; out.notes.push(`cylinder spread ${cyl.join('/')}`); }
  const idle = stats(win(res.rec, 'rpm', 2.0, 6.0)); out.idle = idle;
  if (idle.min < 650) { out.pass = false; out.notes.push(`idle dips to ${f0(idle.min)}`); }
  // no ghost cut while above 1250 during the blip; resumes afterwards
  const blip = res.rec.t.map((t, i) => i).filter((i) => res.rec.t[i] > 6.0 && res.rec.t[i] < 8.0 && res.rec.rpm[i] > 1300 && res.rec.clamp[i]);
  if (blip.length) { out.pass = false; out.notes.push('cut during blip'); }
  const resumed = slotsIn(res, 9.0, 11.0).filter((s) => !s.fired).length;
  out.resumed = resumed;
  if (resumed < 5) { out.pass = false; out.notes.push('did not resume after blip'); }
  out.prem = res.plant.st.premature; out.weak = res.plant.st.weak;
  if (out.prem > 0) { out.pass = false; out.notes.push(`${out.prem} premature`); }
  if (out.weak > 0) { out.pass = false; out.notes.push(`${out.weak} weak`); }
  if (res.plant.stalledAt !== undefined) { out.pass = false; out.notes.push('STALLED'); }
  return out;
}

function deadmanScenario() {
  return { name: 'show button dead-man', seed: 5001, dur: 7.0, cfg: {}, plant: { rpm0: 850 }, kind: 'deadman',
    actions: [{ at: 0.5, do: 'show', v: true }, { at: 1.0, do: 'thr', v: 1 }, { at: 4.0, do: 'disconnect' }] };
}

function checkDeadman(res) {
  const r = res.rec, out = { pass: true, notes: [] };
  const hold = stats(win(r, 'rpm', 2.0, 4.0)); out.hold = hold;
  if (hold.max > 3950 || hold.min < 3500) { out.pass = false; out.notes.push(`hold ${f0(hold.min)}..${f0(hold.max)}`); }
  const rel = firstTime(r, (i) => !r.show[i], 4.0);
  out.releaseMs = rel === null ? null : (rel - res.lastRefresh) * 1000;
  if (rel === null || out.releaseMs > 610) { out.pass = false; out.notes.push(`show not released (${f0(out.releaseMs)}ms)`); }
  const up = stats(win(r, 'rpm', 5.5, 7.0)); out.after = up;
  if (up.max < 6000) { out.pass = false; out.notes.push('limiter still at launchRpm'); }
  return out;
}

function benchScenarios() {
  return [
    { name: 'bench: engine never ran', seed: 6001, dur: 4.0, cfg: {}, plant: { rpm0: 0 }, kind: 'bench1',
      actions: [{ at: 0.5, do: 'show', v: true }, { at: 3.0, do: 'show', v: false }] },
    { name: 'bench: key-off then button', seed: 6002, dur: 8.0, cfg: {}, plant: { rpm0: 850 }, kind: 'bench2',
      actions: [{ at: 1.0, do: 'stop' }, { at: 1.5, do: 'show', v: true }, { at: 4.5, do: 'edge' }, { at: 7.5, do: 'show', v: false }] },
  ];
}

function checkBench(res) {
  const r = res.rec, out = { pass: true, notes: [] };
  const on = firstTime(r, (i) => r.reason[i] === core.CUT_BENCH && r.clamp[i], 0);
  out.on = on;
  if (res.sc.kind === 'bench1') {
    if (on === null || on > 0.6) { out.pass = false; out.notes.push(`bench on at ${on}`); }
    const offAt = firstTime(r, (i) => !r.clamp[i], 3.0);
    if (offAt === null || offAt > 3.01) { out.pass = false; out.notes.push('not released'); }
  } else {
    // engine stopped at 1.0 -> bench only after 2 s of silence; the injected edge at 4.5 must release it at once
    const lastSpark = res.plant.slotLog.filter((s) => s.fired && s.t < 1.2e6).pop();
    out.lastSpark = lastSpark ? lastSpark.t / 1e6 : 0;
    if (on === null || on < out.lastSpark + 1.99 || on > out.lastSpark + 2.02) { out.pass = false; out.notes.push(`bench on at ${on}`); }
    const edgeT = 4.5;
    const offAt = firstTime(r, (i) => !r.clamp[i], edgeT);
    out.releaseMs = offAt === null ? null : (offAt - edgeT) * 1000;
    if (offAt === null || out.releaseMs > 1.5) { out.pass = false; out.notes.push(`edge release ${f0(out.releaseMs)}ms`); }
    const again = firstTime(r, (i) => r.clamp[i], edgeT + 0.01);
    out.again = again;
    if (again !== null && again < edgeT + 1.99) { out.pass = false; out.notes.push(`re-engaged at ${again}`); }
  }
  return out;
}

function starvationScenario(pattern, original) {
  return { name: `${original ? 'ORIGINAL ' : ''}show held WOT ${PAT_NAME[pattern]} (starvation)`, seed: 6100 + pattern, dur: 7.0,
    cfg: { cutPattern: pattern }, plant: { rpm0: 850 }, original, kind: 'starve',
    actions: [{ at: 0.5, do: 'show', v: true }, { at: 1.0, do: 'thr', v: 1 }] };
}

function checkStarve(res) {
  const r = res.rec, out = { pass: true, notes: [] };
  const bench = firstTime(r, (i) => r.reason[i] === core.CUT_BENCH, 1.0);
  if (bench !== null) { out.pass = false; out.notes.push(`BENCH clamp at ${bench.toFixed(3)}s`); }
  const zero = firstTime(r, (i) => r.tel[i] === 0, 1.5);
  if (zero !== null) { out.pass = false; out.notes.push(`reported 0 rpm at ${zero.toFixed(3)}s`); }
  if (res.plant.stalledAt !== undefined) { out.pass = false; out.notes.push(`STALLED at ${(res.plant.stalledAt / 1e6).toFixed(2)}s`); }
  out.hold = stats(win(r, 'rpm', 2.0, 7.0));
  out.maxClamp = maxClampMs(res, 1.0, 7.0);
  return out;
}

function liftScenario(pattern) {
  return { name: `launch lift (no clutch) ${PAT_NAME[pattern]}`, seed: 7001 + pattern, dur: 9.0, cfg: { cutPattern: pattern },
    plant: { rpm0: 850 }, kind: 'lift',
    actions: [{ at: 1.0, do: 'arm' }, { at: 2.0, do: 'thr', v: 1 }, { at: 5.0, do: 'thr', v: 0 }, { at: 7.0, do: 'thr', v: 1 }] };
}

function checkLift(res) {
  const r = res.rec, out = { pass: true, notes: [] };
  const fired = eventTime(res, core.LAUNCH_FIRED, 0);
  if (fired !== null) { out.pass = false; out.notes.push(`FIRED at ${fired.toFixed(3)}`); }
  const off = res.events.find((e) => e.t > 5.0 && e.state === core.LAUNCH_OFF);
  out.offMs = off ? (off.t - 5.0) * 1000 : null;
  out.end = r.end[idx(r, 6.9)];
  if (!off || out.offMs > 400) { out.pass = false; out.notes.push(`OFF after lift: ${f0(out.offMs)}ms`); }
  if (out.end !== core.LAUNCH_END_LIFT) { out.pass = false; out.notes.push(`end code ${out.end}`); }
  const launchCut = firstTime(r, (i) => r.clamp[i] && r.reason[i] === core.CUT_LAUNCH, 5.2);
  if (launchCut !== null) { out.pass = false; out.notes.push(`launch cut after lift at ${launchCut.toFixed(2)}`); }
  const re = stats(win(r, 'rpm', 8.0, 9.0)); out.after = re;
  if (re.max < 6000) { out.pass = false; out.notes.push('re-rev did not reach redline (still limiting?)'); }
  const early = res.events.find((e) => e.t > 2.2 && e.t < 5.0 && e.state !== core.LAUNCH_HOLDING);
  if (early) { out.pass = false; out.notes.push(`left HOLDING at ${early.t.toFixed(3)}`); }
  return out;
}

function miscScenarios() {
  return [
    { name: 'physical switch 2-step', seed: 8001, dur: 6.0, cfg: {}, plant: { rpm0: 850 }, kind: 'switch',
      actions: [{ at: 0.5, do: 'switch', v: true }, { at: 1.0, do: 'thr', v: 1 }, { at: 4.0, do: 'switch', v: false }] },
    { name: 'clutch-switch 2-step, launch', seed: 8006, dur: 7.0, cfg: { cutPattern: 4 }, plant: { rpm0: 850 }, kind: 'swlaunch',
      actions: [{ at: 0.5, do: 'switch', v: true }, { at: 1.0, do: 'thr', v: 1 }, { at: 4.0, do: 'switch', v: false },
        { at: 4.03, do: 'clutch', depth: 800, durMs: 300, rise: 1200 }] },
    { name: 'engine start from cranking', seed: 8007, dur: 3.0, cfg: {}, plant: { rpm0: 0 }, kind: 'start',
      actions: [{ at: 0.5, do: 'start', rpm: 250 }] },
    { name: 'show pressed on the redline', seed: 8008, dur: 6.0, cfg: {}, plant: { rpm0: 850 }, kind: 'showhigh',
      actions: [{ at: 1.0, do: 'thr', v: 1 }, { at: 3.0, do: 'show', v: true }] },
    { name: 'pattern change while holding', seed: 8009, dur: 7.0, cfg: {}, plant: { rpm0: 850 }, kind: 'patchange',
      actions: [{ at: 0.5, do: 'show', v: true }, { at: 1.0, do: 'thr', v: 1 }, { at: 3.0, do: 'cfg', v: { cutPattern: 0 } },
        { at: 4.0, do: 'cfg', v: { cutPattern: 3 } }, { at: 5.0, do: 'cfg', v: { cutPattern: 4 } }, { at: 6.0, do: 'cfg', v: { cutPattern: 2 } }] },
    { name: 'master disarm while cutting', seed: 8010, dur: 4.0, cfg: {}, plant: { rpm0: 850 }, kind: 'disarm',
      actions: [{ at: 0.5, do: 'show', v: true }, { at: 1.0, do: 'thr', v: 1 }, { at: 3.0, do: 'cfg', v: { armed: false } },
        { at: 3.2, do: 'thr', v: 0 }] },
    { name: 'inhibit while cutting', seed: 8002, dur: 6.0, cfg: {}, plant: { rpm0: 850 }, kind: 'inhibit',
      actions: [{ at: 0.5, do: 'show', v: true }, { at: 1.0, do: 'thr', v: 1 }, { at: 3.0, do: 'inhibit', v: true }, { at: 4.0, do: 'inhibit', v: false }] },
    { name: 'anti-flood cannon 1.0s @7000', seed: 8003, dur: 7.0, cfg: { cutPattern: 4, maxCutSeconds: 1.0, redlineRpm: 7000 }, plant: { rpm0: 850 }, kind: 'flood',
      actions: [{ at: 1.0, do: 'thr', v: 1 }] },
    { name: 'hold timeout 12 s', seed: 8004, dur: 16.0, cfg: {}, plant: { rpm0: 850 }, kind: 'holdto',
      actions: [{ at: 0.5, do: 'arm' }, { at: 1.0, do: 'thr', v: 1 }] },
    { name: 'arm window 10 s', seed: 8005, dur: 12.0, cfg: {}, plant: { rpm0: 850 }, kind: 'armto',
      actions: [{ at: 0.5, do: 'arm' }] },
  ];
}

function checkMisc(res) {
  const r = res.rec, out = { pass: true, notes: [] }, k = res.sc.kind;
  if (k === 'switch') {
    out.hold = stats(win(r, 'rpm', 2.0, 4.0));
    if (out.hold.max > 3950 || out.hold.min < 3500) { out.pass = false; out.notes.push(`hold ${f0(out.hold.min)}..${f0(out.hold.max)}`); }
    const rs = firstTime(r, (i) => r.reason[i] === core.CUT_SWITCH, 1.0);
    if (rs === null) { out.pass = false; out.notes.push('reason SWITCH never reported'); }
    const up = stats(win(r, 'rpm', 5.0, 6.0));
    if (up.max < 6000) { out.pass = false; out.notes.push('did not release'); }
  } else if (k === 'swlaunch') {
    const pops = firstTime(r, (i) => r.reason[i] === core.CUT_DECEL, 4.0);
    out.dip = stats(win(r, 'rpm', 4.0, 7.0)).min;
    if (pops !== null) { out.pass = false; out.notes.push(`decel pops under clutch load at ${pops.toFixed(3)}`); }
    const cutAfter = firstTime(r, (i) => r.clamp[i], 4.05);
    if (cutAfter !== null && cutAfter < 6.0) { out.pass = false; out.notes.push(`cut at ${cutAfter.toFixed(3)} after release`); }
    if (res.plant.stalledAt !== undefined || out.dip < 2000) { out.pass = false; out.notes.push(`bog to ${f0(out.dip)}`); }
  } else if (k === 'start') {
    const t800 = firstTime(r, (i) => r.tel[i] >= 700, 0.5);
    out.t800 = t800;
    if (t800 === null || t800 > 1.2) { out.pass = false; out.notes.push(`rpm reading slow: ${t800}`); }
    const idle = stats(win(r, 'tel', 2.0, 3.0)); out.idle = idle;
    if (Math.abs(idle.mean - 850) > 60) { out.pass = false; out.notes.push(`idle reads ${f0(idle.mean)}`); }
    if (firstTime(r, (i) => r.clamp[i], 0) !== null) { out.pass = false; out.notes.push('clamp during start'); }
  } else if (k === 'showhigh') {
    const settle = firstTime(r, (i) => r.rpm[i] < 3950, 3.0); out.settleMs = settle === null ? null : (settle - 3.0) * 1000;
    out.before = stats(win(r, 'rpm', 2.0, 3.0)); out.after = stats(win(r, 'rpm', (settle || 3.0) + 0.3, 6.0));
    if (settle === null || out.settleMs > 1500) { out.pass = false; out.notes.push('did not come down to launchRpm'); }
    if (out.after.max > 3975 || out.after.min < 3500) { out.pass = false; out.notes.push(`hold ${f0(out.after.min)}..${f0(out.after.max)}`); }
    out.prem = res.plant.st.premature;
    if (out.prem) { out.pass = false; out.notes.push(`${out.prem} premature`); }
    if (res.plant.stalledAt !== undefined) { out.pass = false; out.notes.push('STALLED'); }
  } else if (k === 'patchange') {
    out.hold = stats(win(r, 'rpm', 2.0, 7.0));
    if (out.hold.max > 3990 || out.hold.min < 2300) { out.pass = false; out.notes.push(`hold ${f0(out.hold.min)}..${f0(out.hold.max)}`); }
    out.prem = res.plant.st.premature;
    if (out.prem) { out.pass = false; out.notes.push(`${out.prem} premature`); }
    if (res.plant.stalledAt !== undefined) { out.pass = false; out.notes.push('STALLED'); }
  } else if (k === 'disarm') {
    const lastClamp = res.clampIntervals.filter(([on]) => on < 3000).pop();
    const offAt = lastClamp ? lastClamp[1] / 1000 : 0;
    out.releaseMs = (offAt - 3.0) * 1000;
    if (out.releaseMs > 12) { out.pass = false; out.notes.push(`released ${f0(out.releaseMs)}ms after disarm`); }
    if (res.clampIntervals.some(([on]) => on > 3001)) { out.pass = false; out.notes.push('cut while disarmed'); }
  } else if (k === 'inhibit') {
    const during = win(r, 'clamp', 3.0005, 4.0).reduce((a, b) => a + b, 0);
    if (during > 1) { out.pass = false; out.notes.push(`clamp during inhibit ${during}ms`); }
    const again = firstTime(r, (i) => r.clamp[i], 4.0);
    if (again === null || again > 4.6) { out.pass = false; out.notes.push('no cut after inhibit released'); }
    out.inhibitClampMs = during;
    out.prem = res.plant.st.premature;
  } else if (k === 'flood') {
    const lock = firstTime(r, (i) => r.reason[i] === core.CUT_FLOOD_LOCK, 1.0);
    out.lockAt = lock; out.maxClamp = maxClampMs(res, 0, 7);
    if (lock === null) { out.pass = false; out.notes.push('flood lock never reported'); }
    if (out.maxClamp > 1000) { out.pass = false; out.notes.push(`continuous cut ${f0(out.maxClamp)}ms`); }
    // cutActive must follow the real clamp: every ms flagged cutActive must be within 60 ms of a clamp ms
    let lastClamp = -1e9, bad = 0;
    for (let i = 0; i < r.t.length - 3; i++) {
      if (r.clamp[i]) lastClamp = i;
      if (r.cut[i] && i - lastClamp > 63) bad++;                       // telemetry is refreshed every 2 ms
      if (r.clamp[i] && !r.cut[i] && !r.cut[i + 1] && !r.cut[i + 2]) bad++;
    }
    if (bad) { out.pass = false; out.notes.push(`cutActive disagrees with the clamp in ${bad} ms`); }
    const peak = stats(win(r, 'rpm', 2.0, 7.0)).max; out.peak = peak;
    if (peak > 7150) { out.pass = false; out.notes.push(`overshoot ${f0(peak)}`); }
  } else if (k === 'holdto') {
    const off = res.events.find((e) => e.t > 1.0 && e.state === core.LAUNCH_OFF);
    out.offAt = off ? off.t : null;
    const tHold = eventTime(res, core.LAUNCH_HOLDING, 0);
    out.heldS = off && tHold ? off.t - tHold : null;
    if (!off || off.end !== core.LAUNCH_END_HOLD_TIMEOUT || Math.abs(out.heldS - 12) > 0.05) { out.pass = false; out.notes.push(`OFF at ${out.offAt} end ${off && off.end}`); }
  } else if (k === 'armto') {
    const off = res.events.find((e) => e.t > 0.6 && e.state === core.LAUNCH_OFF);
    out.offAt = off ? off.t : null;
    if (!off || Math.abs(off.t - 10.5) > 0.01 || off.end !== core.LAUNCH_END_ARM_TIMEOUT) { out.pass = false; out.notes.push(`OFF at ${out.offAt}`); }
  }
  return out;
}

// -----------------------------------------------------------------------------------------
// Table output
// -----------------------------------------------------------------------------------------
const rows = [];
function row(name, pass, text) { rows.push({ name, pass, text }); }
function printTable(title, list) {
  console.log(`\n${title}`);
  const w = Math.max(...list.map((r) => r.name.length), 10);
  console.log(`${'scenario'.padEnd(w)}  result  details`);
  console.log(`${'-'.repeat(w)}  ------  ${'-'.repeat(60)}`);
  for (const r of list) console.log(`${r.name.padEnd(w)}  ${r.pass === null ? 'info  ' : r.pass ? 'PASS  ' : 'FAIL  '}  ${r.text}`);
}

function runAll(opts) {
  // (a)+(b) launch, every pattern, three clutch profiles, clean and noisy tach
  for (let p = 0; p <= 4; p++) {
    for (const prof of ['mild', 'typical', 'harsh']) {
      const res = simulate(launchScenario(p, prof));
      const m = checkLaunch(res);
      row(res.sc.name, m.pass, `hold ${f0(m.hold && m.hold.min)}/${f0(m.hold && m.hold.max)}/${f0(m.hold && m.hold.mean)} (tel ${f0(m.holdTel && m.holdTel.min)}..${f0(m.holdTel && m.holdTel.max)}) ` +
        `lastCut +${f0(m.lastCutMs)}ms released +${f0(m.latency)}ms ${m.label} +${f0(m.firedMs)}ms dip ${f0(m.dip)} weak ${m.weak} prem ${m.prem} ${m.notes.join('; ')}`);
    }
  }
  for (const p of [0, 1, 4]) {
    const res = simulate(launchScenario(p, 'typical', { name: `launch ${PAT_NAME[p]} typical NOISY tach`, plant: NOISY, seed: 1500 + p }));
    const m = checkLaunch(res);
    row(res.sc.name, m.pass, `hold ${f0(m.hold && m.hold.min)}/${f0(m.hold && m.hold.max)}/${f0(m.hold && m.hold.mean)} released +${f0(m.latency)}ms ${m.label} +${f0(m.firedMs)}ms ` +
      `rej ${res.diag.rejected} disc ${res.diag.discarded} outl ${res.diag.outliers} dwellBlk ${res.diag.dwellBlocks} prem ${m.prem} ${m.notes.join('; ')}`);
  }
  // (c) lift
  for (const p of [0, 1, 4]) {
    const res = simulate(liftScenario(p)); const m = checkLift(res);
    row(res.sc.name, m.pass, `OFF(LIFT) +${f0(m.offMs)}ms after lift (${res.diag.launchDropRate} rpm/s), re-rev max ${f0(m.after.max)} ${m.notes.join('; ')}`);
  }
  // (d) redline
  for (let p = 0; p <= 4; p++) {
    for (const gear of [false, true]) {
      const res = simulate(redlineScenario(p, { gear })); const m = checkRedline(res);
      row(res.sc.name, m.pass, `peak ${f0(m.peak)} hold ${f0(m.hold && m.hold.min)}/${f0(m.hold && m.hold.max)}/${f0(m.hold && m.hold.mean)} ` +
        `maxClamp ${f0(m.maxClamp)}ms weak ${m.weak} prem ${m.prem} ${m.notes.join('; ')}`);
    }
  }
  // (e) decel
  for (const sc of decelScenarios()) {
    const res = simulate(sc); const m = checkDecel(res);
    row(sc.name, m.pass, `bursts ${m.bursts.length} lat ${m.lat.map(f0).join(',') || '-'}ms len ${m.len.map(f0).join(',') || '-'}ms ` +
      `endRpm ${m.endRpm.map(f0).join(',') || '-'}${m.abortMs !== undefined ? ` abort +${f0(m.abortMs)}ms` : ''} ${m.notes.join('; ')}`);
  }
  // (f) ghost
  {
    const res = simulate(ghostScenario()); const m = checkGhost(res);
    row(res.sc.name, m.pass, `cut ${(m.frac * 100).toFixed(1)}% maxRun ${m.maxRun} perCyl ${m.cyl.join('/')} gaps ${JSON.stringify(m.gaps)} ` +
      `idle ${f0(m.idle.min)}..${f0(m.idle.max)} resumed ${m.resumed} weak ${m.weak} prem ${m.prem} ${m.notes.join('; ')}`);
  }
  // (g) dead-man
  {
    const res = simulate(deadmanScenario()); const m = checkDeadman(res);
    row(res.sc.name, m.pass, `hold ${f0(m.hold.min)}..${f0(m.hold.max)} released ${f0(m.releaseMs)}ms after last BTN:1, then max ${f0(m.after.max)} ${m.notes.join('; ')}`);
  }
  // (h) bench
  for (const sc of benchScenarios()) {
    const res = simulate(sc); const m = checkBench(res);
    row(sc.name, m.pass, `bench on at ${m.on === null ? '-' : m.on.toFixed(3)}s${m.releaseMs !== undefined ? ` edge-release ${m.releaseMs.toFixed(2)}ms re-engage ${m.again === null ? 'none' : m.again.toFixed(3)}` : ''} ${m.notes.join('; ')}`);
  }
  for (const p of [0, 1, 4]) {
    const res = simulate(starvationScenario(p, false)); const m = checkStarve(res);
    row(res.sc.name, m.pass, `no bench, rpm ${f0(m.hold.min)}..${f0(m.hold.max)}, max clamp ${f0(m.maxClamp)}ms ${m.notes.join('; ')}`);
  }
  // misc
  for (const sc of miscScenarios()) {
    const res = simulate(sc); const m = checkMisc(res);
    let txt = '';
    if (sc.kind === 'switch') txt = `hold ${f0(m.hold.min)}..${f0(m.hold.max)}`;
    if (sc.kind === 'start') txt = `reads >= 700 rpm at ${m.t800 && m.t800.toFixed(3)}s, idle reads ${f0(m.idle.mean)}`;
    if (sc.kind === 'showhigh') txt = `${f0(m.before.mean)} -> below 3950 in ${f0(m.settleMs)}ms, then ${f0(m.after.min)}..${f0(m.after.max)}`;
    if (sc.kind === 'patchange') txt = `rpm ${f0(m.hold.min)}..${f0(m.hold.max)} across 5 pattern changes`;
    if (sc.kind === 'disarm') txt = `clamp released ${f0(m.releaseMs)}ms after armed=false`;
    if (sc.kind === 'swlaunch') txt = `min rpm after release ${f0(m.dip)}`;
    if (sc.kind === 'inhibit') txt = `clamp ms during inhibit ${m.inhibitClampMs}, prem ${m.prem}`;
    if (sc.kind === 'flood') txt = `lock reported at ${m.lockAt && m.lockAt.toFixed(2)}s, max continuous ${f0(m.maxClamp)}ms, peak ${f0(m.peak)}`;
    if (sc.kind === 'holdto') txt = `held ${m.heldS && m.heldS.toFixed(2)}s -> OFF`;
    if (sc.kind === 'armto') txt = `ARMED -> OFF at ${m.offAt}s`;
    row(sc.name, m.pass, `${txt} ${m.notes.join('; ')}`);
  }
  const fails = rows.filter((r) => r.pass === false).length;
  printTable(`NEW ENGINE CORE - ${rows.length} scenarios, ${fails} failed`, rows);
  return fails;
}

function runOriginal() {
  const list = [];
  const res = simulate(launchScenario(1, 'typical', { original: true, name: 'ORIGINAL launch flames typical' }));
  const tHold = eventTime(res, core.LAUNCH_HOLDING, 1.0);
  const firstCut = firstTime(res.rec, (i) => res.rec.clamp[i], 1.5);
  const end = res.events.find((e) => tHold !== null && e.t > tHold && e.state !== core.LAUNCH_HOLDING);
  const rpmAtEnd = end ? res.rec.rpm[idx(res.rec, end.t)] : null;
  list.push({ name: res.sc.name, pass: null, text: `HOLDING at ${tHold && tHold.toFixed(3)}s, first cut ${firstCut && firstCut.toFixed(3)}s, ` +
    `left HOLDING ${end ? `+${f0((end.t - firstCut) * 1000)}ms after first cut -> ${STATE_NAME[end.state]}` : 'never'} ` +
    `(${res.endInfo ? res.endInfo.why : '-'}), true rpm then ${f0(rpmAtEnd)}; premature sparks ${res.plant.st.premature}` });
  for (const p of [0, 1]) {
    const rr = simulate(redlineScenario(p, { original: true, name: `ORIGINAL redline ${PAT_NAME[p]}` }));
    const w = stats(win(rr.rec, 'rpm', 2.6, 6.5));
    list.push({ name: rr.sc.name, pass: null, text: `true rpm ${f0(w.min)}..${f0(w.max)} mean ${f0(w.mean)}, premature sparks ${rr.plant.st.premature}, weak ${rr.plant.st.weak}` });
  }
  const st = simulate(starvationScenario(0, true));
  const m = checkStarve(st);
  list.push({ name: st.sc.name, pass: null, text: m.notes.join('; ') || 'ok' });
  const gh = simulate(ghostScenario({ original: true, name: 'ORIGINAL ghost cam idle' }));
  list.push({ name: gh.sc.name, pass: null, text: `premature sparks ${gh.plant.st.premature}, weak ${gh.plant.st.weak}, cut slots ${gh.plant.st.cut}` });
  printTable('ORIGINAL FIRMWARE (258cb19) through the same simulator - reference', list);
}

function sweepLaunchDrop() {
  const drops = [300, 350, 400, 450, 500, 600];
  console.log('\nlaunchDrop sweep: false exits while holding / FIRED latency (ms) worst-case over patterns 0-3, profiles, clean+noisy tach, 3 seeds');
  console.log('drop  falseExit  missed  latency(min/mean/max)  maxHoldDip');
  for (const d of drops) {
    let falseExit = 0, missed = 0; const lat = []; let worstDip = 0;
    for (let p = 0; p <= 3; p++) for (const prof of ['mild', 'typical', 'harsh']) for (const noisy of [false, true]) for (const seed of [11, 22, 33]) {
      const res = simulate(launchScenario(p, prof, { cfg: { launchDrop: d }, plant: noisy ? NOISY : {}, seed: seed * 100 + p }));
      const m = checkLaunch(res);
      if (m.notes.some((n) => n.startsWith('left HOLDING'))) falseExit++;
      if (m.latency === null || m.latency > 150) missed++;
      if (m.latency !== null) lat.push(m.latency);
      if (m.holdTel) worstDip = Math.max(worstDip, 3800 - m.holdTel.min);
    }
    const s = stats(lat);
    console.log(`${String(d).padEnd(5)} ${String(falseExit).padEnd(10)} ${String(missed).padEnd(7)} ${f0(s.min)}/${f0(s.mean)}/${f0(s.max)}`.padEnd(46) + ` ${f0(worstDip)}`);
  }
}

function trace(name) {
  const all = [];
  for (let p = 0; p <= 4; p++) for (const prof of ['mild', 'typical', 'harsh']) all.push(launchScenario(p, prof));
  for (let p = 0; p <= 4; p++) { all.push(redlineScenario(p)); all.push(redlineScenario(p, { gear: true })); }
  for (const p of [0, 1, 4]) all.push(liftScenario(p));
  all.push(...decelScenarios(), ghostScenario(), deadmanScenario(), ...benchScenarios(), ...miscScenarios());
  for (const p of [0, 1, 4]) all.push(starvationScenario(p, false));
  all.push(launchScenario(1, 'typical', { original: true, name: 'ORIGINAL launch flames typical' }));
  const sc = all.find((s) => s.name === name);
  if (!sc) { console.error('unknown scenario; names:\n' + all.map((s) => s.name).join('\n')); process.exit(2); }
  const res = simulate(sc);
  const from = +(process.argv[4] || 0), to = +(process.argv[5] || sc.dur);
  console.log('t,rpm,tel,clamp,state,reason,cut,est');
  for (let i = idx(res.rec, from); i < idx(res.rec, to); i++) {
    console.log(`${res.rec.t[i].toFixed(3)},${res.rec.rpm[i].toFixed(0)},${res.rec.tel[i]},${res.rec.clamp[i]},${res.rec.state[i]},${res.rec.reason[i]},${res.rec.cut[i]},${res.rec.est[i]}`);
  }
  console.error(JSON.stringify({ events: res.events, diag: res.diag, st: res.plant.st }));
}

if (require.main === module) {
  const args = process.argv.slice(2);
  if (args[0] === '--trace') { trace(args[1]); process.exit(0); }
  const t0 = Date.now();
  const fails = runAll();
  runOriginal();
  if (!args.includes('--quick')) sweepLaunchDrop();
  console.log(`\n(${((Date.now() - t0) / 1000).toFixed(1)} s)`);
  process.exit(fails ? 1 : 0);
}

module.exports = { simulate, launchScenario, redlineScenario, checkLaunch, checkRedline };
