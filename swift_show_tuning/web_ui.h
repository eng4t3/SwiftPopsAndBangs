#pragma once
// =========================================================================================
// EMBEDDED MOBILE WEB DASHBOARD (HTML5, CSS3, Vanilla JS)
// Served by the firmware at http://192.168.4.1/  (see swift_show_tuning.ino)
//
// - Talks to the firmware over the WebSocket on port 81 and the HTTP /api routes
//   (contract: docs/PROTOCOL.md).
// - No external runtime dependencies: web fonts are requested only after page load and
//   have system fallbacks (the car's Wi-Fi has no internet).
// - Built-in demo mode (simulated engine + mocked API) when opened from file:// or with
//   ?demo=1. tools/ui/export_preview.py writes this page to preview.html.
// - Never put the raw-literal terminator (close paren + rawliteral + quote) in the page.
// =========================================================================================
#include <Arduino.h>

const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="hu">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0, user-scalable=no, maximum-scale=1.0, viewport-fit=cover">
<meta name="theme-color" content="#07090e">
<title>Suzuki Swift 1.3 8V - Show Tuning Vezérlő</title>
<link rel="icon" href="data:,">
<style>
  :root {
    --bg: #07090e; --panel: #101622eb; --inset: #080c14bf; --line: #ffffff14;
    --cyan: #00f0ff; --amber: #ffaa00; --red: #ff2247; --red-glow: #ff224780; --green: #00ff88;
    --text: #f0f4fc; --muted: #8391aa;
    --disp: 'Orbitron', 'Segoe UI', Roboto, Arial, sans-serif;
    --body: 'Rajdhani', 'Roboto Condensed', 'Arial Narrow', sans-serif-condensed, system-ui, sans-serif;
  }
  * { box-sizing: border-box; margin: 0; padding: 0; -webkit-tap-highlight-color: transparent; }
  [hidden] { display: none !important; }
  body {
    background: var(--bg) radial-gradient(circle at 50% 0%, #151d2c 0%, #07090e 70%) no-repeat;
    color: var(--text); font: 600 16px/1.3 var(--body); min-height: 100vh; overflow-x: hidden;
    padding: 10px 12px calc(28px + env(safe-area-inset-bottom)); user-select: none; -webkit-user-select: none; -webkit-text-size-adjust: 100%;
  }
  input { user-select: text; -webkit-user-select: text; }
  button { font: inherit; color: inherit; cursor: pointer; touch-action: manipulation; }
  :focus-visible { outline: 2px solid var(--cyan); outline-offset: 2px; }
  .wrap { max-width: 440px; margin: 0 auto; display: flex; flex-direction: column; gap: 12px; }
  .card { background: var(--panel); border: 1px solid var(--line); border-radius: 20px; box-shadow: 0 12px 32px #00000080; }
  @keyframes blink { from { opacity: .25; } to { opacity: 1; } }
  @keyframes flash { from { transform: scale(.97); opacity: .85; } to { transform: scale(1.03); opacity: 1; } }
  @keyframes pulseAmber { 0%, 100% { box-shadow: 0 0 18px #fa06; } 50% { box-shadow: 0 0 36px #ffaa00d9; } }

  /* Top bar */
  .topbar { display: flex; align-items: center; justify-content: space-between; gap: 8px; padding: 9px 10px 9px 14px; border-radius: 16px; }
  .brand { flex: 1; min-width: 0; }
  .brand-title { font: 900 .95rem var(--disp); letter-spacing: 1.5px; display: flex; align-items: center; gap: 6px; }
  .badge { background: linear-gradient(135deg, #ff2247, #aa0720); font-size: .6rem; padding: 2px 7px; border-radius: 4px; letter-spacing: .5px; }
  .brand-sub { font-size: .7rem; color: var(--muted); font-weight: 700; letter-spacing: 1.2px; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  .fw-chip { color: var(--cyan); }
  .top-actions { display: flex; align-items: center; gap: 8px; flex-shrink: 0; }
  .pill { display: flex; align-items: center; gap: 6px; font: 800 .6rem var(--disp); letter-spacing: .8px; padding: 6px 11px; border-radius: 20px; background: #00000080; border: 1px solid var(--line); color: var(--muted); white-space: nowrap; }
  .pill.up { color: var(--green); border-color: #0f86; background: #00ff881a; }
  .pill.down { color: var(--amber); border-color: #fa06; }
  .pill.down .dot, .pill.connecting .dot { animation: blink .8s infinite alternate; }
  .dot { width: 7px; height: 7px; border-radius: 50%; background: currentColor; box-shadow: 0 0 6px currentColor; }
  .icon-btn { width: 40px; height: 40px; border-radius: 50%; background: #00f0ff1f; border: 1px solid #00f0ff73; color: var(--cyan); font: 900 1rem var(--disp); }
  .icon-btn:active { background: var(--cyan); color: var(--bg); }
  .demo-bar { display: flex; align-items: center; justify-content: space-between; gap: 10px; padding: 8px 10px 8px 14px; border-radius: 14px; border: 1px dashed #ffaa008c; background: #ffaa0014; font-size: .8rem; color: #ffd98a; }
  .demo-bar .btn { width: auto; flex-shrink: 0; touch-action: none; }

  /* RPM readout */
  .cluster { padding: 12px 14px 12px; display: flex; flex-direction: column; gap: 10px; }
  .cluster.cutting { border-color: #ff224799; box-shadow: 0 0 28px #ff22474c; }
  .cut-line { font: 900 .78rem var(--disp); letter-spacing: 1.5px; text-align: center; padding: 8px 10px; border-radius: 12px; border: 1px solid var(--line); background: #00000059; color: var(--muted); white-space: nowrap; }
  .cut-line.on { background: #ff224740; border-color: var(--red); color: #fff; box-shadow: 0 0 16px var(--red-glow); }
  .cut-line.lock { background: #ffaa002e; border-color: var(--amber); color: #ffe2a8; }
  .readout { display: flex; justify-content: space-between; align-items: flex-end; gap: 8px; }
  .rpm { font: 700 4.2rem/.85 var(--body); letter-spacing: -1px; font-variant-numeric: tabular-nums; text-shadow: 0 0 14px #00f0ff73; }
  .rpm.warn { color: var(--amber); text-shadow: 0 0 18px #fa09; }
  .rpm.danger { color: var(--red); text-shadow: 0 0 22px #ff2247cc; }
  .rpm-wrap { display: flex; align-items: baseline; gap: 6px; }
  .unit { font: 700 .7rem var(--disp); letter-spacing: 1.5px; color: var(--muted); }
  .legend { display: flex; justify-content: space-between; font: 700 .62rem var(--disp); letter-spacing: 1px; color: var(--muted); }
  .legend span::before { content: ''; display: inline-block; width: 3px; height: 10px; margin-right: 6px; vertical-align: -1px; background: var(--amber); }
  .legend b { color: var(--amber); }
  .legend span + span::before { background: var(--red); }
  .legend span + span b { color: var(--red); }
  .digital-r { display: flex; flex-direction: column; align-items: flex-end; gap: 6px; }
  .peak { font: 800 .68rem var(--disp); letter-spacing: 1px; color: var(--amber); background: #ffaa001f; border: 1px solid #ffaa004c; border-radius: 8px; padding: 8px 10px; white-space: nowrap; }
  .pat-mini { font-size: .76rem; font-weight: 700; color: var(--muted); letter-spacing: 1px; }
  .rbar { position: relative; margin-top: 4px; }
  .rtrack { position: relative; height: 12px; border-radius: 6px; overflow: hidden; background: #101622; border: 1px solid #ffffff0f; }
  .rtrack i { position: absolute; top: 0; bottom: 0; }
  #zoneWarn { background: #fa03; }
  #zoneRed { right: 0; background: #ff224761; }
  .rpm-fill { left: 0; width: 100%; background: linear-gradient(90deg, #0088cc, var(--cyan)); transform-origin: left; transform: scaleX(0); transition: transform .05s linear; }
  .rpm-fill.warn { background: var(--amber); }
  .rpm-fill.danger, .cutting .rpm-fill { background: var(--red); }
  .mk { position: absolute; top: -5px; bottom: -5px; width: 3px; margin-left: -1.5px; border-radius: 2px; background: var(--amber); box-shadow: 0 0 6px var(--amber); }
  .mk.r { background: var(--red); box-shadow: 0 0 6px var(--red); }
  .scale { display: flex; justify-content: space-between; font: 700 .6rem var(--disp); color: var(--muted); margin-top: 4px; }
  .scale span { width: 0; display: flex; justify-content: center; }

  /* Launch / show control */
  .launch { padding: 12px; }
  .seg { display: grid; grid-template-columns: 1fr 1fr; gap: 8px; margin-bottom: 10px; }
  .seg button { padding: 8px 6px; min-height: 48px; border-radius: 12px; background: #0e1420e6; border: 1px solid var(--line); color: var(--muted); display: flex; flex-direction: column; align-items: center; }
  .seg b { font: 800 .66rem var(--disp); letter-spacing: .5px; }
  .seg small { font-size: .72rem; }
  .seg button.active { background: #00f0ff24; border-color: var(--cyan); color: var(--cyan); box-shadow: 0 0 14px #00f0ff40; }
  .fire-btn { width: 100%; min-height: 104px; padding: 12px 10px 16px; border-radius: 20px; border: 2px solid var(--red); background: linear-gradient(180deg, #2a0b12, #130407); display: flex; flex-direction: column; justify-content: center; align-items: center; gap: 6px; position: relative; overflow: hidden; box-shadow: 0 8px 28px #ff22474c, inset 0 1px 0 #ffffff26; transition: transform .05s, background .1s; -webkit-touch-callout: none; }
  .fire-btn.show { touch-action: none; }
  .fire-btn::before { content: ''; position: absolute; inset: 0; background: repeating-linear-gradient(45deg, #ffffff05 0 8px, transparent 8px 16px); pointer-events: none; }
  .fire-title { font: 900 1.15rem var(--disp); letter-spacing: 1.2px; text-align: center; text-shadow: 0 0 12px var(--red-glow); }
  .fire-sub { font-size: .8rem; font-weight: 700; letter-spacing: 1px; color: var(--red); text-align: center; }
  .fire-cd { position: absolute; left: 0; right: 0; bottom: 0; height: 7px; background: #00000073; opacity: 0; }
  .fire-cd i { display: block; height: 100%; background: linear-gradient(90deg, #ff8800, var(--amber)); transform-origin: left; transition: transform .1s linear; }
  .fire-btn.pressed { background: var(--red); transform: translateY(3px) scale(.985); box-shadow: 0 0 45px #ff2247f2; }
  .fire-btn.pressed .fire-sub, .fire-btn.holding .fire-sub { color: #fff; }
  .fire-btn.armed { background: linear-gradient(180deg, #332505, #171102); border-color: var(--amber); animation: pulseAmber 1.2s infinite ease-in-out; }
  .fire-btn.armed .fire-sub { color: var(--amber); }
  .fire-btn.armed .fire-cd { opacity: 1; }
  .fire-btn.holding { background: var(--red); border-color: #fff; box-shadow: 0 0 50px #ff2247f2; animation: flash .12s infinite alternate; }
  .fire-btn.fired { background: linear-gradient(180deg, #053315, #02170a); border-color: var(--green); box-shadow: 0 0 35px #00ff88b2; }
  .fire-btn.fired .fire-sub { color: var(--green); }
  .fire-btn.offline { border-color: #3a4560; background: #10151f; box-shadow: none; }
  .fire-btn.offline .fire-title, .fire-btn.offline .fire-sub { text-shadow: none; color: var(--muted); }
  .ls-steps { display: grid; grid-template-columns: repeat(4, 1fr); gap: 6px; margin-top: 10px; }
  .ls-steps span { font: 800 .58rem var(--disp); letter-spacing: .8px; text-align: center; padding: 6px 0; border-radius: 8px; border: 1px solid var(--line); color: #56627a; background: #00000059; }
  .ls-steps span.on { color: var(--c, #fff); border-color: var(--c, #6b7a96); }
  .ls-steps span.on[data-s="2"] { background: var(--red); color: #fff; }
  .warn-line { font-size: .85rem; color: #ffd98a; background: #ffaa001a; border: 1px solid #ffaa0059; border-radius: 10px; padding: 8px 10px; }
  .launch .warn-line { margin-top: 10px; }

  /* Collapsible setting cards */
  .settings { display: flex; flex-direction: column; gap: 12px; }
  .settings.loading details { opacity: .5; pointer-events: none; }
  summary { list-style: none; cursor: pointer; display: flex; align-items: center; gap: 8px; }
  summary::-webkit-details-marker { display: none; }
  details.card > summary { gap: 10px; min-height: 56px; padding: 10px 16px; }
  .ico { font-size: 1.15rem; width: 26px; text-align: center; }
  .st { flex: 1; font: 800 .78rem var(--disp); letter-spacing: 1px; }
  .sv { font-weight: 700; color: var(--amber); font-variant-numeric: tabular-nums; white-space: nowrap; }
  .sv.off { color: var(--muted); }
  .sv.bad { color: var(--red); }
  .chev { width: 9px; height: 9px; border-right: 2px solid var(--muted); border-bottom: 2px solid var(--muted); transform: rotate(45deg); margin: 0 3px 4px 4px; transition: transform .2s; }
  details[open] > summary .chev { transform: rotate(-135deg); margin-bottom: -4px; }
  .cb { padding: 2px 16px 16px; display: flex; flex-direction: column; gap: 16px; }
  .row { display: flex; align-items: center; justify-content: space-between; gap: 12px; }
  .lbl { display: flex; flex-direction: column; min-width: 0; }
  .lbl b { font-size: 1.05rem; font-weight: 700; }
  .lbl small, .hint { font-size: .8rem; color: var(--muted); line-height: 1.3; }
  .hint.center { text-align: center; }
  .hint.good { color: var(--green); }
  .hint.bad { color: #ff8095; }
  .val { font: 700 1.1rem var(--body); padding: 3px 10px; border-radius: 8px; background: #0000008c; border: 1px solid var(--line); color: var(--amber); font-variant-numeric: tabular-nums; white-space: nowrap; }
  .val.danger { color: var(--red); }
  .sl { display: flex; flex-direction: column; gap: 8px; }
  .sl.off { opacity: .45; }
  .sl-row { display: flex; align-items: center; gap: 8px; }
  .step { width: 44px; height: 44px; flex-shrink: 0; border-radius: 12px; background: #ffffff12; border: 1px solid var(--line); font: 700 1.4rem/1 var(--body); }
  .step:active { background: var(--cyan); color: #000; }
  input[type=range] { -webkit-appearance: none; appearance: none; flex: 1; min-width: 0; height: 10px; border-radius: 6px; border: 1px solid #ffffff0f; background: linear-gradient(90deg, #ff2247bf var(--p, 50%), #141b2a var(--p, 50%)); }
  input[type=range]::-webkit-slider-thumb { -webkit-appearance: none; width: 28px; height: 28px; border-radius: 50%; background: linear-gradient(135deg, #ff416c, #ff2247); border: 2px solid #fff; box-shadow: 0 0 14px #ff2247b2; }
  input[type=range]::-moz-range-thumb { width: 24px; height: 24px; border-radius: 50%; background: #ff2247; border: 2px solid #fff; }
  .sw { position: relative; width: 58px; height: 32px; flex-shrink: 0; }
  .sw input { position: absolute; opacity: 0; width: 100%; height: 100%; cursor: pointer; z-index: 1; }
  .sw span { position: absolute; inset: 0; background: #1a2233; border: 1px solid var(--line); border-radius: 32px; transition: .2s; pointer-events: none; }
  .sw span::before { content: ''; position: absolute; width: 24px; height: 24px; left: 3px; top: 3px; border-radius: 50%; background: #8899af; transition: .2s; }
  .sw input:checked + span { background: var(--red); border-color: var(--red); box-shadow: 0 0 14px var(--red-glow); }
  .sw input:checked + span::before { transform: translateX(26px); background: #fff; }
  .sw input:focus-visible + span { outline: 2px solid var(--cyan); outline-offset: 2px; }
  .pats { display: grid; grid-template-columns: 1fr 1fr; gap: 8px; }
  .pat { background: #0e121cd9; border: 1px solid var(--line); border-radius: 12px; padding: 10px 8px; display: flex; flex-direction: column; align-items: center; gap: 3px; text-align: center; }
  .pat:last-child { grid-column: span 2; }
  .pat b { font: 800 .68rem var(--disp); letter-spacing: .5px; }
  .pat small { font-size: .74rem; color: var(--muted); line-height: 1.15; }
  .pat.active { background: linear-gradient(180deg, #2b1016, #19090d); border-color: var(--red); box-shadow: 0 0 16px #ff224759; }
  .seq { display: flex; gap: 3px; margin-top: 4px; }
  .seq i { width: 7px; height: 10px; border-radius: 2px; background: var(--red); }
  .seq i.f { background: var(--green); }
  .seq i.L { width: 60px; }
  .save-btn { padding: 12px 14px; min-height: 56px; border-radius: 14px; border: 1px solid #ffffff1a; background: linear-gradient(180deg, #1c2538, #131926); display: flex; flex-direction: column; align-items: center; gap: 2px; box-shadow: 0 4px 15px #0006; }
  .save-btn b { font: 800 .85rem var(--disp); letter-spacing: 1.2px; }
  .save-btn small { font-size: .78rem; color: var(--muted); }
  .save-btn.dirty, .save-btn.saving { position: sticky; bottom: calc(10px + env(safe-area-inset-bottom)); z-index: 20; }
  .save-btn.dirty { background: linear-gradient(180deg, #3a2a06, #211703); border-color: var(--amber); animation: pulseAmber 1.6s infinite ease-in-out; }
  .save-btn.dirty b { color: var(--amber); }
  .save-btn.dirty small { color: #ffd98a; }
  .save-btn.saving { opacity: .85; border-color: var(--cyan); }

  /* Buttons, forms, update */
  .btn { width: 100%; min-height: 46px; padding: 10px 12px; border-radius: 12px; border: 1px solid #00f0ff80; background: #00f0ff14; color: var(--cyan); font: 800 .72rem var(--disp); letter-spacing: 1px; }
  .btn:active { background: #00f0ff40; }
  .btn:disabled { opacity: .4; }
  .btn.go { background: linear-gradient(135deg, #00ff88, #00aa55); border: none; color: var(--bg); font-weight: 900; box-shadow: 0 0 16px #00ff8859; }
  .btn.warn { border-color: #ffaa0073; color: var(--amber); background: #ffaa0014; }
  .btn.ghost { border-color: var(--line); color: var(--muted); background: none; }
  .btn.dashed { border-style: dashed; }
  .btn.sm { min-height: 40px; font-size: .64rem; padding: 8px 10px; }
  .btn-row { display: flex; gap: 8px; }
  .fw-cur { display: flex; justify-content: space-between; align-items: baseline; gap: 8px; flex-wrap: wrap; }
  .fw-cur b { font: 800 .9rem var(--disp); color: var(--cyan); }
  .box { background: var(--inset); border: 1px solid var(--line); border-radius: 14px; padding: 12px; display: flex; flex-direction: column; gap: 10px; }
  .box-h { font: 800 .74rem var(--disp); letter-spacing: .8px; flex: 1; }
  .tag { font: 800 .55rem var(--disp); letter-spacing: 1px; color: var(--bg); background: var(--green); border-radius: 4px; padding: 2px 5px; }
  details.box > summary { min-height: 30px; }
  .sub > summary { font-size: .85rem; color: var(--muted); min-height: 36px; }
  .sub > summary::before { content: '▸'; transition: transform .2s; }
  .sub[open] > summary::before { transform: rotate(90deg); }
  .sub-b { display: flex; flex-direction: column; gap: 10px; margin-top: 8px; }
  .fld { display: flex; flex-direction: column; gap: 4px; font-size: .82rem; color: var(--muted); }
  .fld input { font: 600 1rem var(--body); color: var(--text); background: #0b111c; border: 1px solid #253046; border-radius: 10px; padding: 10px 12px; min-height: 44px; width: 100%; }
  .fld input:focus { outline: none; border-color: var(--cyan); }
  .fld input:disabled { opacity: .4; }
  .chk { display: flex; align-items: center; gap: 8px; font-size: .85rem; color: var(--muted); min-height: 40px; }
  .chk input { width: 20px; height: 20px; accent-color: var(--red); }
  .ota-st { border-radius: 10px; padding: 9px 11px; font-size: .88rem; line-height: 1.35; background: #00f0ff0f; border: 1px solid #00f0ff40; color: #cfe9f5; display: flex; flex-direction: column; gap: 7px; word-break: break-word; }
  .ota-st.ok { background: #00ff8812; border-color: #0f86; color: #b8ffd9; }
  .ota-st.go { background: #00ff881a; border-color: var(--green); color: #fff; }
  .ota-st.warn { background: #ffaa0014; border-color: #ffaa0073; color: #ffe0a0; }
  .ota-st.err { background: #ff224714; border-color: #ff224780; color: #ffc2cd; }
  .bar { display: none; height: 8px; border-radius: 5px; background: #0009; overflow: hidden; }
  .ota-st.prog .bar { display: block; }
  .bar i { display: block; height: 100%; width: 0; background: linear-gradient(90deg, #00f0ff, #00ff88); transition: width .2s linear; }
  .kv { display: grid; grid-template-columns: auto 1fr; gap: 6px 12px; font-size: .92rem; }
  .kv dt { color: var(--muted); }
  .kv dd { text-align: right; font-variant-numeric: tabular-nums; word-break: break-all; }
  .toast { position: fixed; left: 50%; top: calc(12px + env(safe-area-inset-top)); transform: translate(-50%, -20px); width: max-content; max-width: calc(100% - 24px); text-align: center; background: #0a0f19f7; border: 1px solid var(--green); color: var(--green); font: 700 .74rem/1.35 var(--disp); letter-spacing: .6px; padding: 11px 18px; border-radius: 22px; box-shadow: 0 10px 30px #0009; opacity: 0; transition: .25s; pointer-events: none; z-index: 3000; }
  .toast.show { opacity: 1; transform: translate(-50%, 0); }
  .toast.warn { border-color: var(--amber); color: var(--amber); }
  .toast.err { border-color: var(--red); color: #ffc2cd; }

  /* Data logger */
  .loglist { display: flex; flex-direction: column; gap: 6px; }
  .li { display: flex; align-items: center; gap: 10px; text-align: left; padding: 8px 10px; border-radius: 12px; background: var(--inset); border: 1px solid var(--line); }
  .li > :nth-child(2) { flex: 1; }
  .li small { display: block; color: var(--muted); }
  .li .btn { width: auto; }
  a.btn { display: flex; align-items: center; justify-content: center; text-decoration: none; }
  #lvCv { width: 100%; background: #0b111c; border-radius: 10px; }
  #logView input { background: #141b2a; }

  /* Help modal */
  .modal { position: fixed; inset: 0; background: #04070ce6; z-index: 2000; display: flex; justify-content: center; align-items: center; padding: 12px; opacity: 0; pointer-events: none; transition: opacity .2s; }
  .modal.open { opacity: 1; pointer-events: auto; }
  .modal-card { background: #0e1420; border: 1px solid #00f0ff59; border-radius: 22px; width: 100%; max-width: 440px; max-height: 90vh; display: flex; flex-direction: column; overflow: hidden; box-shadow: 0 20px 60px #000000d9; }
  .modal-head { display: flex; justify-content: space-between; align-items: center; padding: 12px 12px 12px 18px; border-bottom: 1px solid var(--line); font: 900 .95rem var(--disp); letter-spacing: 1px; }
  .modal-body { padding: 14px; overflow-y: auto; display: flex; flex-direction: column; gap: 10px; font-size: .92rem; line-height: 1.4; user-select: text; -webkit-user-select: text; }
  .msec { background: var(--inset); border: 1px solid #ffffff0f; border-radius: 14px; padding: 12px 14px; }
  .msec h3 { font: 800 .78rem var(--disp); letter-spacing: .8px; color: var(--cyan); margin-bottom: 6px; }
  .msec p { color: #c4d1e2; margin-bottom: 6px; }
  .msec p:last-child { margin-bottom: 0; }
  .msec b { color: var(--amber); }
  .modal-foot { padding: 12px 14px; border-top: 1px solid var(--line); }
  .modal-foot .btn { background: linear-gradient(135deg, #00f0ff, #0088cc); color: var(--bg); border: none; font-weight: 900; }
  @media (prefers-reduced-motion: reduce) { *, *::before { animation: none !important; transition: none !important; } }
</style>
</head>
<body>
<div class="wrap">

<header class="card topbar">
  <div class="brand">
    <div class="brand-title">SWIFT <span class="badge">G13BA</span></div>
    <div class="brand-sub"><span id="fwChip" class="fw-chip" hidden></span>1.3L 8V SHOW VEZÉRLŐ</div>
  </div>
  <div class="top-actions">
    <div id="statusPill" class="pill connecting"><i class="dot"></i><span id="statusText">KERESÉS…</span></div>
    <button class="icon-btn" onclick="openModal()" aria-label="Súgó és funkcióleírások">ℹ</button>
  </div>
</header>

<div id="demoBar" class="demo-bar" hidden>
  <span><b>DEMÓ MÓD</b> – szimulált motor, nincs kapcsolat az autóval.</span>
  <button id="demoGas" class="btn sm">🦶 GÁZ (TARTSD)</button>
</div>

<section id="cluster" class="card cluster">
  <div id="cutLine" class="cut-line">NINCS ADAT</div>
  <div class="readout">
    <div class="rpm-wrap"><span id="rpmNum" class="rpm">0</span><span class="unit">RPM</span></div>
    <div class="digital-r">
      <button id="peak" class="peak" onclick="resetPeak()" aria-label="Csúcsérték nullázása">CSÚCS 0</button>
      <div id="patMini" class="pat-mini"></div>
    </div>
  </div>
  <div class="rbar" aria-hidden="true">
    <div class="rtrack"><i id="zoneWarn"></i><i id="zoneRed"></i><i id="rpmFill" class="rpm-fill"></i></div>
    <i id="mkLaunch" class="mk"></i><i id="mkRed" class="mk r"></i>
  </div>
  <div class="scale" aria-hidden="true"><span>0</span><span>1</span><span>2</span><span>3</span><span>4</span><span>5</span><span>6</span><span>7</span><span>8</span></div>
  <div class="legend"><span>RAJT <b id="lgLaunch"></b></span><span>LIMIT <b id="lgRed"></b></span></div>
</section>

<section class="card launch">
  <div class="seg" role="group" aria-label="Rajt mód">
    <button id="modeHandsFree" onclick="setLaunchMode('handsfree')"><b>⚡ HANDS-FREE RAJT</b><small>egy koppintás</small></button>
    <button id="modeShow" onclick="setLaunchMode('show')"><b>🔥 SHOW MÓD</b><small>nyomva tartás</small></button>
  </div>
  <button id="btnTwoStep" class="fire-btn">
    <span id="fireTitle" class="fire-title"></span>
    <span id="fireSub" class="fire-sub"></span>
    <span class="fire-cd"><i id="cdFill"></i></span>
  </button>
  <div id="lsSteps" class="ls-steps"><span data-s="0">OFF</span><span data-s="1" style="--c:#ffaa00">ARMED</span><span data-s="2" style="--c:#ff2247">HOLDING</span><span data-s="3" style="--c:#00ff88">FIRED</span></div>
  <div id="armWarn" class="warn-line" hidden>⚠ A rendszer ki van kapcsolva, a vezérlő most nem vesz el szikrát. Bekapcsolás: Biztonság kártya.</div>
</section>

<!-- slider blocks are built from SLIDERS -->
<div id="settings" class="settings loading">
  <details class="card" id="grpLaunch" open>
    <summary><span class="ico">🏁</span><span class="st">RAJT / LAUNCH</span><span id="sumLaunch" class="sv"></span><i class="chev"></i></summary>
    <div class="cb">
      <div class="sl" data-sl="cfgLaunch"></div>
      <div class="sl" data-sl="cfgDrop"><div id="dropHint" class="hint"></div></div>
      <div id="launchWarn" class="warn-line" hidden>⚠ A rajt limit a redline fölött van – ott a redline tilt előbb.</div>
    </div>
  </details>

  <details class="card" id="grpLimiter" open>
    <summary><span class="ico">⚡</span><span class="st">LIMITER / REDLINE</span><span id="sumLimiter" class="sv"></span><i class="chev"></i></summary>
    <div class="cb"><div class="sl" data-sl="cfgRedline"></div></div>
  </details>

  <details class="card" id="grpDecel" open>
    <summary><span class="ico">💥</span><span class="st">DURROGÁS / OVERRUN</span><span id="sumDecel" class="sv"></span><i class="chev"></i></summary>
    <div class="cb">
      <div class="row"><div class="lbl"><b>Gázelvételi durrogás</b><small>Pufogás motorféken, ha hirtelen elveszed a gázt</small></div><label class="sw"><input type="checkbox" id="cfgDecelPops" checked aria-label="Gázelvételi durrogás"><span></span></label></div>
      <div class="sl" data-sl="cfgDecel" id="decelBlock"></div>
    </div>
  </details>

  <details class="card" id="grpSound" open>
    <summary><span class="ico">🎯</span><span class="st">HANGZÁS / MINTÁZAT</span><span id="sumSound" class="sv"></span><i class="chev"></i></summary>
    <div class="cb">
      <div class="pats" id="pats">
        <button class="pat" data-p="0" data-seq="xxxxxxxx"><b>⚡ KEMÉNY TILTÁS</b><small>Minden szikra kimarad a limit fölött (Bee*R)</small></button>
        <button class="pat" data-p="1" data-seq="xxxoxxxo"><b>🔥 LÁNGCSÓVA</b><small>3 szikra kimarad, 1 gyújt</small></button>
        <button class="pat" data-p="2" data-seq="xxoxxoxx"><b>💥 DURROGÁS</b><small>2 szikra kimarad, 1 gyújt</small></button>
        <button class="pat" data-p="3" data-seq="xoxoxoxo"><b>🎯 AK-47</b><small>1 kimarad, 1 gyújt – gyors staccato</small></button>
        <button class="pat" data-p="4" data-seq="Looo"><b>💣 ÁGYÚLÖVÉS</b><small>~1,6 s teljes tiltás, majd nagy dörrenés – show módban és kapcsolóval; redline-on és rajtkor kemény tiltás</small></button>
      </div>
      <div class="hint center">Gyújtásonként: piros = kimaradó szikra, zöld = gyújtás</div>
      <div class="row"><div class="lbl"><b>👻 Ghost Cam™ / V8 alapjárat</b><small>Hegyes vezértengelyes dadogás alapjáraton (650–1250 RPM)</small></div><label class="sw"><input type="checkbox" id="cfgGhostCam" aria-label="Ghost Cam"><span></span></label></div>
    </div>
  </details>

  <details class="card" id="grpSafety" open>
    <summary><span class="ico">🛡️</span><span class="st">BIZTONSÁG</span><span id="sumSafety" class="sv"></span><i class="chev"></i></summary>
    <div class="cb">
      <div class="row"><div class="lbl"><b>Rendszer élesítve (főkapcsoló)</b><small>Kikapcsolva soha nem vesz el szikrát – a gyári gyújtás 100%-ban működik</small></div><label class="sw"><input type="checkbox" id="cfgArmed" checked aria-label="Rendszer élesítve"><span></span></label></div>
      <div class="sl" data-sl="cfgTimeout"><div class="hint">Teljesen jobbra húzva: NINCS LIMIT (∞) – csak óvatosan!</div></div>
    </div>
  </details>

  <button id="saveBtn" class="save-btn" onclick="saveToFlash()"><b id="saveTxt"></b><small id="saveSub"></small></button>
</div>

<details class="card" id="grpLog">
  <summary><span class="ico">📈</span><span class="st">ADATNAPLÓ</span><span id="sumLog" class="sv"></span><i class="chev"></i></summary>
  <div class="cb">
    <div id="logSt" class="ota-st" hidden><span class="ota-msg"></span><span class="bar"><i></i></span></div>
    <div class="btn-row"><button class="btn" onclick="logSnap()">📌 MENTÉS MOST (utolsó 60 mp)</button><button class="btn warn" onclick="logClear()">🗑 NAPLÓ TÖRLÉSE</button></div>
    <div id="logView" class="box" hidden>
      <div class="row"><b id="lvTitle" class="box-h"></b><button class="icon-btn" onclick="lvClose()" aria-label="Bezárás">✕</button></div>
      <canvas id="lvCv"></canvas>
      <div class="sl-row">🔍<input type="range" id="lvZoom" max="60" value="0" aria-label="Nagyítás">↔<input type="range" id="lvPan" max="1000" value="0" aria-label="Görgetés"></div>
      <p class="hint">Piros háttér: tiltás • csík: sárga ARMED, piros HOLDING, zöld FIRED • szaggatott: rajt, oldás, redline • alsó sáv: zöld szikra, piros kimaradt, szürke zaj</p>
      <dl id="lvKv" class="kv"></dl>
      <a id="lvCsv" class="btn">⬇ CSV LETÖLTÉSE</a>
    </div>
    <b class="box-h">Események</b><div id="logCaps" class="loglist"></div>
    <b class="box-h">Utak</b><div id="logDrives" class="loglist"></div>
  </div>
</details>

<details class="card" id="grpFw">
  <summary><span class="ico">📡</span><span class="st">FIRMWARE FRISSÍTÉS</span><span id="sumFw" class="sv off">–</span><i class="chev"></i></summary>
  <div class="cb">
    <div class="fw-cur"><span>Telepített verzió: <b id="fwCur">?</b></span><small id="fwBuilt" class="hint"></small></div>

    <div class="box">
      <div class="row"><span class="box-h">📱 FRISSÍTÉS TELEFONON ÁT</span><span class="tag">AJÁNLOTT</span></div>
      <p class="hint">A telefon mobilneten letölti a GitHubról a legújabb firmware-t, és feltölti a vezérlőre. Androidon az autó Wi-Fi-je mellett is megy, ha be van kapcsolva a mobilnet; iPhone-on gyakran nem – ott a Wi-Fi-s mód vagy a kézi feltöltés segít.</p>
      <div class="btn-row">
        <button id="p1Check" class="btn" onclick="phoneCheck()">🔍 FRISSÍTÉS KERESÉSE</button>
        <button id="p1Install" class="btn go" onclick="phoneInstall()" hidden>⬇ TELEPÍTÉS</button>
      </div>
      <div id="p1Status" class="ota-st" hidden><span class="ota-msg"></span><span class="bar"><i></i></span></div>
    </div>

    <details class="box" id="p2Box">
      <summary><span class="box-h">📶 FRISSÍTÉS WI-FI-N (AZ ESP TÖLTI LE)</span><i class="chev"></i></summary>
      <div class="sub-b">
        <p class="hint">Az ESP egy internetes Wi-Fi-n (router vagy hotspot) maga tölti le a frissítést. Az autó Wi-Fi-je közben megszakadhat (csatornaváltás) – az oldal újracsatlakozik. Hotspotnál indítsd el a telepítést, az ESP egyedül befejezi; utána csatlakozz vissza a Swift-PopsAndBangs hálózatra.</p>
        <label class="fld"><span>Wi-Fi hálózat neve (SSID)</span><input id="staSsid" data-track autocomplete="off" autocapitalize="off" spellcheck="false" placeholder="pl. OtthoniWifi"></label>
        <label class="fld"><span>Wi-Fi jelszó</span><input id="staPass" type="password" autocomplete="off" placeholder="a Wi-Fi jelszava"></label>
        <label class="chk"><input type="checkbox" id="staOpen" onchange="$('staPass').disabled = this.checked"> Nyílt hálózat (nincs jelszó)</label>
        <details class="sub">
          <summary>Haladó: GitHub tároló és ág</summary>
          <div class="sub-b">
            <label class="fld"><span>GitHub tároló (felhasználó/név)</span><input id="espRepo" data-track autocomplete="off" autocapitalize="off" spellcheck="false" placeholder="eng4t3/SwiftPopsAndBangs"></label>
            <label class="fld"><span>Ág (branch)</span><input id="espBranch" data-track autocomplete="off" autocapitalize="off" spellcheck="false" placeholder="main"></label>
          </div>
        </details>
        <div class="row"><div class="lbl"><b>Automatikus ellenőrzés</b><small>Az ESP magától keres új verziót</small></div><label class="sw"><input type="checkbox" id="espAuto" data-track aria-label="Automatikus ellenőrzés"><span></span></label></div>
        <button class="btn" onclick="espSaveCfg()">💾 WI-FI BEÁLLÍTÁSOK MENTÉSE</button>
        <div class="btn-row">
          <button id="p2Check" class="btn" onclick="espAction('check')">🔍 ELLENŐRZÉS</button>
          <button id="p2Install" class="btn go" onclick="espAction('install')" disabled>⬇ TELEPÍTÉS</button>
        </div>
        <button id="p2Cancel" class="btn ghost" onclick="espAction('cancel')" hidden>✕ MEGSZAKÍTÁS</button>
        <div id="p2Status" class="ota-st" hidden><span class="ota-msg"></span><span class="bar"><i></i></span></div>
      </div>
    </details>

    <details class="box" id="p3Box">
      <summary><span class="box-h">📁 KÉZI FELTÖLTÉS (.BIN)</span><i class="chev"></i></summary>
      <div class="sub-b">
        <p class="hint">Tartalék: egy már letöltött firmware.bin feltöltése.</p>
        <input type="file" id="otaFile" accept=".bin,application/octet-stream" hidden onchange="pickFile(this)">
        <button class="btn dashed" onclick="$('otaFile').click()">📁 FIRMWARE (.BIN) KIVÁLASZTÁSA</button>
        <div id="p3Name" class="hint center">Nincs fájl kiválasztva</div>
        <button id="p3Install" class="btn go" onclick="fileInstall()" hidden>🚀 FELTÖLTÉS ÉS TELEPÍTÉS</button>
        <div id="p3Status" class="ota-st" hidden><span class="ota-msg"></span><span class="bar"><i></i></span></div>
      </div>
    </details>
  </div>
</details>

<details class="card" id="grpSys">
  <summary><span class="ico">⚙️</span><span class="st">RENDSZER</span><i class="chev"></i></summary>
  <div class="cb">
    <dl class="kv">
      <dt>Firmware</dt><dd id="sysFw">–</dd>
      <dt>Szabad memória</dt><dd id="sysHeap">–</dd>
      <dt>Telemetria</dt><dd id="sysRate">–</dd>
    </dl>
    <button class="btn warn" onclick="restoreDefaults()">🔄 AJÁNLOTT ÉRTÉKEK VISSZAÁLLÍTÁSA</button>
  </div>
</details>
</div>

<div id="toast" class="toast" role="status" aria-live="polite"></div>

<div id="infoModal" class="modal" aria-hidden="true">
  <div class="modal-card" role="dialog" aria-modal="true" aria-label="Funkcióleírások">
    <div class="modal-head"><span>ℹ FUNKCIÓLEÍRÁSOK</span><button class="icon-btn" onclick="closeModal()" aria-label="Bezárás">✕</button></div>
<div class="modal-body">
  <div class="msec">
    <h3>🏁 Hands-free rajt (launch control)</h3>
    <p>• Koppints a gombra: indul a <b>10 mp-es készenlét (ARMED)</b>, a gomb alján visszaszámlálással.</p>
    <p>• Kuplung be, padlógáz: a fordulat a <b>rajt limiten</b> (pl. 3800 RPM) ragad, a motor durrog és lángol <b>(HOLDING)</b>.</p>
    <p>• Kuplung felengedésekor a terhelés lehúzza a fordulatot. Ha a <b>kuplung-felengedés érzékenység</b> értékével (alapból 400 RPM) a limit alá esik, és ott is marad (legalább 3 valódi mérés, 60 ms), a tiltás azonnal megszűnik: <b>RAJT! (FIRED)</b>. Kisebb érték = hamarabb old, nagyobb = biztosabb.</p>
    <p>• A saját tiltás okozta ingadozás nem old ki: a vezérlő csak a ténylegesen elsült szikrákból mér. Ágyúlövés mintázatnál rajtkor kemény tiltással tart, hogy a kuplung felengedése látszódjon.</p>
    <p>• Befejeződik, ha 10 mp alatt nem éred el a rajt limitet, ha elveszed a gázt kuplungfelengedés előtt (újra kell élesíteni), ha 12 mp-nél tovább állsz a limiten, vagy ha újra koppintasz. Gyors dupla koppintásnál a második érintés nem számít.</p>
    <p>• Rajt után 3 mp-ig nincs gázelvételi durrogás, hogy váltáskor ne rángasson.</p>
  </div>
  <div class="msec">
    <h3>🔥 Show mód / 2-step</h3>
    <p>Amíg nyomva tartod a gombot, a motor a rajt limiten tilt (állóhelyzeti durrogtatás, lángok); elengedve azonnal leáll.</p>
    <p><b>Biztonság:</b> a telefon 200 ms-onként megerősíti a nyomva tartást; ha a kapcsolat megszakad vagy elhagyod az oldalt, a vezérlő 0,6 mp-en belül magától elengedi.</p>
    <p><b>Padteszt:</b> ha a motor tényleg áll (legalább 2 mp-e, nem csak járás közben szakadt meg a jel), a show gomb folyamatosan tilt (bekötés-ellenőrzés, a kék LED világít), legfeljebb kb. 10 mp-ig – utána engedd el és nyomd meg újra. Padteszt közben a motor nem indul – indítás előtt engedd el a gombot.</p>
    <p><b>GPIO 23 kapcsoló:</b> kézifékre vagy kuplungra kötve ugyanígy 2-step, de padtesztet soha nem indít, így kinyomott kuplunggal is indul a motor.</p>
  </div>
  <div class="msec">
    <h3>⚡ Limiter / redline</h3>
    <p>Felső fordulatkorlát (pl. 6200 RPM): e fölött mindig tilt, a kiválasztott mintázattal. Ha a mintázat nem bírja megtartani (75 RPM-mel a limit fölé megy), kemény tiltásra vált. Az anti-flood a limitert soha nem kapcsolja ki. A fordulatszám-sáv piros zónája és a kijelző színe ezt követi.</p>
  </div>
  <div class="msec">
    <h3>💥 Gázelvételi durrogás</h3>
    <p>Ha a küszöb (pl. 3200 RPM) fölül hirtelen elveszed a gázt, motorfék közben rövid szikraelvételek jönnek, és a keverék a forró kipufogóban durran el. Gyorsításkor nem kapcsol be, és ha közben újra gázt adsz, bármelyik mintázatnál azonnal abbamarad.</p>
  </div>
  <div class="msec">
    <h3>🎯 Mintázatok (gyújtásonként)</h3>
    <p>A mintázat minden tiltásnál eldönti, melyik szikra marad ki:</p>
    <p>• <b>Kemény tiltás:</b> a limit fölött 2–5 szikra kimarad, majd 1 gyújt, ebből méri a fordulatot (Bee*R limiter).</p>
    <p>• <b>Lángcsóva:</b> 3 kimarad, 1 gyújt – sok keverék a kipufogóban, nagy lángok.</p>
    <p>• <b>Durrogás:</b> 2 kimarad, 1 gyújt – sűrű, mély lövések.</p>
    <p>• <b>AK-47:</b> 1 kimarad, 1 gyújt – gyors, géppuskaszerű staccato.</p>
    <p>• <b>Ágyúlövés:</b> kb. 1,6 s teljes tiltás (a kipufogó megtelik keverékkel), majd visszajön a szikra: egy nagy dörrenés és tűzgolyó. Ez show módban és a 2-step kapcsolóval működik; a redline-on és hands-free rajtkor kemény tiltásként viselkedik.</p>
  </div>
  <div class="msec">
    <h3>👻 Ghost Cam™ / V8 alapjárat</h3>
    <p>Stabil alapjáraton (650–1250 RPM) minden 5. szikrát kihagyja (sosem kettőt egymás után), mint egy hegyes vezértengelyes V8. Gázadáskor, elinduláskor és 1250 RPM fölött magától szünetel.</p>
  </div>
  <div class="msec">
    <h3>📟 Mi tilt most?</h3>
    <p>A fordulatszám fölötti sávban: <b>2-STEP</b> (show gomb), <b>RAJT</b>, <b>KAPCSOLÓ</b> (GPIO 23), <b>REDLINE</b>, <b>DURROGÁS</b>, <b>GHOST CAM</b>, <b>PADTESZT</b>. <b>ANTI-FLOOD ZÁR:</b> a 100%-os tiltás elérte az időkorlátot, ezért a vezérlő szikrákat is enged (a limiter közben is működik).</p>
  </div>
  <div class="msec">
    <h3>🛡️ Biztonság</h3>
    <p>• <b>Főkapcsoló:</b> kikapcsolva a vezérlő soha nem vesz el szikrát.</p>
    <p>• <b>Anti-flood:</b> ha a tiltás a beállított ideig (pl. 3 s) egybefüggően 100%-os (pl. ágyúlövésnél), a vezérlő szikrákat is enged, hogy a gyertyák ne ázzanak el. A mintázatos tiltásban eleve vannak szikrák. Jobb szélen (∞) kikapcsol.</p>
    <p>• 2000 RPM alatt nincs tiltás (kivéve Ghost Cam, padteszt). A főkapcsoló kikapcsolt állásában a redline limiter sem működik, csak a gyári ECU. Áramtalanítva, újraindulás és frissítés alatt a gyári gyújtás 100%-ban működik.</p>
  </div>
  <div class="msec">
    <h3>💾 Mentés</h3>
    <p>A változtatások azonnal, de ideiglenesen élnek; a kiemelt gombbal mentsd őket (a vezérlő visszaigazolja).</p>
  </div>
  <div class="msec">
    <h3>📈 Adatnapló</h3>
    <p>A vezérlő magától rögzít a flash-ébe: az egész útról 25 mintát másodpercenként (fordulat, tiltás, rajt állapot), eseményeknél (rajt, redline, anti-flood, rendellenesség, kézi mentés) gyújtásonkénti részletességgel is. Gyújtáslevétel után is megmarad, vezetés közben nem kell hozzá a telefon, és csak akkor ír, amikor semmi nem tilt. A CSV letölthető, és elküldheted elemzésre, hangolásra.</p>
  </div>
  <div class="msec">
    <h3>📡 Firmware frissítés</h3>
    <p>Három mód (részletek a Firmware frissítés kártyán): <b>telefonon át</b> (ajánlott; mobilnet kell az autó Wi-Fi-je mellett), <b>Wi-Fi-n</b> (az ESP maga tölti le egy internetes Wi-Fi-n) és <b>kézi .bin feltöltés</b>. Utána a vezérlő újraindul, az oldal ellenőrzi az új verziót; hiba esetén a régi marad. Álló motornál frissíts.</p>
  </div>
</div>
    <div class="modal-foot"><button class="btn" onclick="closeModal()">RENDBEN, ÉRTETTEM</button></div>
  </div>
</div>

<script>
'use strict';
// ---- Helpers
const $ = id => document.getElementById(id);
const DEMO = location.protocol === 'file:' || /[?&]demo=1(&|$)/.test(location.search);
const store = {
  get(k, d) { try { const v = localStorage.getItem('swift.' + k); return v === null ? d : v; } catch (e) { return d; } },
  set(k, v) { try { localStorage.setItem('swift.' + k, v); } catch (e) {} }
};
const sleep = ms => new Promise(r => setTimeout(r, ms));
// haptics need a prior user tap (Chrome logs an error otherwise)
const vibrate = p => { try { const a = navigator.userActivation; if (navigator.vibrate && (!a || a.hasBeenActive)) navigator.vibrate(p); } catch (e) {} };
const dec = (v, n) => Number(v).toFixed(n).replace('.', ',');
const kb = b => Math.round((b || 0) / 1024) + ' KB';
const txt = (id, s) => { $(id).textContent = s; };

let toastTimer = 0;
function toast(msg, kind) {
  const t = $('toast');
  t.textContent = msg;
  t.className = 'toast show ' + (kind || '');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => t.classList.remove('show'), kind === 'err' ? 5000 : 2600);
}

// Live state reported by the firmware
const S = { connected: false, rpm: 0, cut: false, reason: 0 };

// ---- RPM readout: big number + slim bar (0..8000 RPM)
const MAX_RPM = 8000;
const G = { rpm: 0, shown: -1, peak: 0, redline: 0, warn: 0, raf: 0 };
const elRpm = $('rpmNum'), elFill = $('rpmFill');
const pct = rpm => Math.max(0, Math.min(100, rpm / MAX_RPM * 100)) + '%';

// Zones and readout colors follow the configured redline
function setRedline(redline) {
  if (redline === G.redline) return;
  G.redline = redline;
  G.warn = Math.max(2000, redline - 1700);
  $('zoneWarn').style.left = pct(G.warn);
  $('zoneWarn').style.width = pct(redline - G.warn);
  $('zoneRed').style.left = pct(redline);
  G.shown = -1;
  drawRpm();
}
function setRpm(rpm) {
  G.rpm = rpm;
  if (!G.raf) G.raf = requestAnimationFrame(drawRpm);
}
function drawRpm() {
  G.raf = 0;
  // The firmware does not cap the reading: show the real number, clamp only the bar
  const rpm = Math.max(0, Math.min(9999, G.rpm | 0));
  if (rpm === G.shown) return;
  G.shown = rpm;
  const zone = rpm >= G.redline ? ' danger' : rpm >= G.warn ? ' warn' : '';
  elRpm.textContent = rpm;
  elRpm.className = 'rpm' + zone;
  elFill.className = 'rpm-fill' + zone;
  elFill.style.transform = 'scaleX(' + (Math.min(rpm, MAX_RPM) / MAX_RPM).toFixed(4) + ')';
  if (rpm > G.peak) { G.peak = rpm; txt('peak', 'CSÚCS ' + rpm); }
}
function resetPeak() { G.peak = 0; txt('peak', 'CSÚCS 0'); toast('Csúcsérték nullázva'); }

// What is cutting the spark right now (telemetry `reason`)
const REASONS = ['', '2-STEP', 'RAJT', 'KAPCSOLÓ', 'REDLINE', 'DURROGÁS', 'GHOST CAM', 'PADTESZT', 'ANTI-FLOOD ZÁR'];
let cutKey = 'x', cutHoldUntil = 0;
function renderCut() {
  const now = Date.now(), lock = S.reason === 8, on = S.connected && (S.cut || lock);
  const key = !S.connected ? 'x' : on ? (lock ? 'L' : 'C' + S.reason) : '';
  if (on) cutHoldUntil = now + 400;
  else if (key === '' && now < cutHoldUntil) return;   // hold the text on a fast-toggling limiter
  if (key === cutKey) return;
  cutKey = key;
  const b = $('cutLine');
  b.className = 'cut-line' + (on ? (lock ? ' lock' : ' on') : '');
  b.textContent = key === 'x' ? 'NINCS ADAT' : lock ? '⏸ ANTI-FLOOD ZÁR' : on ? '⚡ TILTÁS: ' + (REASONS[S.reason] || 'AKTÍV') : '✓ NINCS TILTÁS';
  $('cluster').classList.toggle('cutting', on && !lock);
  if (L.showHeld) renderLaunch();
}

// ---- WebSocket link (port 81): stale detection + reconnect
let ws = null, wsTimer = 0, wsFails = 0, wsStartedAt = 0, wsSuspended = false;
let lastMsgAt = 0, cfgReceived = false, cfgAskAt = 0, rateN = 0, rateAt = Date.now();

function wsSend(msg) {
  if (DEMO) { if (!sim.online) return false; sim.recv(msg); return true; }
  if (ws && ws.readyState === 1) { try { ws.send(msg); return true; } catch (e) {} }
  return false;
}
function connectWS() {
  clearTimeout(wsTimer);
  if (DEMO || wsSuspended) return;
  dropWS();
  let sock;
  try { sock = new WebSocket('ws://' + (location.hostname || '192.168.4.1') + ':81/'); }
  catch (e) { scheduleReconnect(); return; }
  ws = sock;
  wsStartedAt = Date.now();
  sock.onopen = () => { if (ws === sock) linkUp(); };
  sock.onmessage = e => { if (ws === sock) onMessage(String(e.data)); };
  sock.onclose = () => { if (ws === sock) { ws = null; linkDown(); scheduleReconnect(); } };
}
function dropWS() {
  if (!ws) return;
  const s = ws;
  ws = null;
  s.onopen = s.onmessage = s.onclose = null;
  try { s.close(); } catch (e) {}
}
function scheduleReconnect() {
  clearTimeout(wsTimer);
  if (wsSuspended) return;
  wsFails++;
  wsTimer = setTimeout(connectWS, Math.min(1000 * wsFails, 4000));
}
// closed during a firmware upload (frees the ESP), resumed afterwards
function suspendWS(on) {
  if (DEMO || wsSuspended === on) return;
  wsSuspended = on;
  if (on) { clearTimeout(wsTimer); dropWS(); linkDown(); }
  else { wsFails = 0; connectWS(); }
}
function setConn(state) {
  $('statusPill').className = 'pill ' + state;
  txt('statusText', wsSuspended ? 'FRISSÍTÉS…' : state !== 'up' ? 'KERESÉS…' : DEMO ? 'DEMÓ' : 'KAPCSOLÓDVA');
}
function linkUp() {
  S.connected = true;
  lastMsgAt = cfgAskAt = Date.now();
  cfgReceived = false;
  wsSend('GET_CONFIG');
  setConn('up');
  renderLaunch();
  refreshInfo();
}
function linkDown() {
  S.connected = false;
  S.cut = false;
  S.reason = 0;
  showRelease();
  setConn('down');
  renderCut();
  renderLaunch();
  if (saving) saveFailed('Megszakadt a kapcsolat.');
}
function onMessage(msg) {
  lastMsgAt = Date.now();
  wsFails = 0;
  if (msg.startsWith('T:')) { rateN++; onTelemetry(msg.slice(2)); }
  else if (msg.startsWith('CFG:')) onConfig(msg.slice(4));
  else if (msg.startsWith('ACK:SAVED')) onSaved();
}
// T:<rpm>,<cut>,<launchState>,<launchLeftMs>,<reason>   (old firmware sends only the first 3)
function onTelemetry(p) {
  const f = p.split(',');
  const ls = f.length > 2 ? (parseInt(f[2], 10) || 0) : 0;
  const left = f.length > 3 ? parseInt(f[3], 10) : NaN;
  const reason = f.length > 4 ? parseInt(f[4], 10) : NaN;
  S.rpm = parseInt(f[0], 10) || 0;
  S.cut = f[1] === '1';
  S.reason = isNaN(reason) ? -1 : reason;
  setRpm(S.rpm);
  renderCut();
  onLaunchTelemetry(ls, isNaN(left) ? null : left);
}
// Watchdog: stale link, missing CFG, launch countdown, telemetry rate
setInterval(() => {
  const now = Date.now();
  if (!DEMO && ws && ((ws.readyState === 1 && now - lastMsgAt > 1500) || (ws.readyState === 0 && now - wsStartedAt > 4000))) {
    dropWS(); linkDown(); scheduleReconnect();
  }
  if (S.connected && !cfgReceived && now - cfgAskAt > 2000) { cfgAskAt = now; wsSend('GET_CONFIG'); }
  renderCut();
  if (S.connected && L.state === 1 && L.mode === 'handsfree') renderCountdown(now);
  if (now - rateAt >= 1000) {
    const hz = Math.round(rateN * 1000 / (now - rateAt));
    rateN = 0; rateAt = now;
    txt('sysRate', S.connected ? hz + ' Hz' : 'nincs kapcsolat');
  }
}, 100);

// ---- Launch control (hands-free) and show-mode 2-step button
const L = {
  mode: store.get('mode', 'handsfree') === 'show' ? 'show' : 'handsfree',
  state: 0, leftMs: 0, leftAt: 0, totalMs: 10000,
  cmdAt: 0, cmdState: 0, armSentAt: 0, lastTapAt: 0, fixAt: 0,
  showHeld: false, showTimer: 0, pid: null
};
const btn = $('btnTwoStep');

function setLaunchMode(m) {
  if (m === L.mode) return;
  showRelease();
  if (L.state === 1 || L.state === 2) { wsSend('LAUNCH:DISARM'); setLaunchLocal(0); }
  L.mode = m;
  store.set('mode', m);
  renderLaunch();
}
function setLaunchLocal(st) {
  L.cmdAt = Date.now();
  L.cmdState = L.state = st;
  if (st !== 1) L.leftAt = 0;
  renderLaunch();
}
function onLaunchTelemetry(ls, left) {
  const now = Date.now();
  if (now - L.cmdAt < 600 && ls !== L.cmdState) return;   // older than our last command
  if (L.mode === 'show') {
    // hands-free launch must never stay armed behind the show-mode UI
    if ((ls === 1 || ls === 2) && now - L.fixAt > 1000) { L.fixAt = now; wsSend('LAUNCH:DISARM'); }
    return;
  }
  if (ls === 1) {
    if (left !== null) { L.leftMs = left; L.leftAt = now; if (left > L.totalMs) L.totalMs = left; }
    else if (!L.leftAt) { L.leftMs = 10000; L.leftAt = now; }   // old firmware: local countdown
  }
  if (ls === L.state) return;
  const prev = L.state;
  L.state = ls;
  if (ls !== 1) L.leftAt = 0;
  if (ls === 2) vibrate(40);
  else if (ls === 3) vibrate([80, 40, 120]);
  else if (ls === 0 && prev === 1) toast(now - L.armSentAt < 2000 ? 'A vezérlő nem nyugtázta az élesítést – koppints újra' : 'Rajt-élesítés lejárt', 'warn');
  else if (ls === 0 && prev === 2) toast('Rajt megszakítva (gázelvétel vagy anti-flood)', 'warn');
  renderLaunch();
}
function onLaunchTap() {
  const now = Date.now();
  if (now - L.lastTapAt < 400) return;   // ignore the 2nd tap of a double tap
  L.lastTapAt = now;
  if (!S.connected) { toast('Nincs kapcsolat a vezérlővel', 'err'); vibrate([30, 30, 30]); return; }
  if (L.state === 0) {
    if (!$('cfgArmed').checked) { toast('A rendszer ki van kapcsolva (Biztonság kártya)', 'warn'); vibrate([30, 30, 30]); return; }
    if (!wsSend('LAUNCH:ARM')) { toast('Nincs kapcsolat a vezérlővel', 'err'); return; }
    L.armSentAt = now;
    L.totalMs = L.leftMs = 10000;
    setLaunchLocal(1);
    L.leftAt = now;
    renderCountdown(now);
    vibrate(50);
  } else {
    if (!wsSend('LAUNCH:DISARM')) { toast('Nincs kapcsolat a vezérlővel', 'err'); return; }
    setLaunchLocal(0);
    vibrate([30, 40, 30]);
    toast('Élesítés törölve');
  }
}
function renderCountdown(now) {
  const left = L.leftAt ? Math.max(0, L.leftMs - (now - L.leftAt)) : 0;
  $('cdFill').style.transform = `scaleX(${Math.min(1, left / L.totalMs).toFixed(3)})`;
  txt('fireSub', 'MÉG ' + dec(left / 1000, 1) + ' MP • KOPPINTÁS = MÉGSE');
}
function renderLaunch() {
  const hf = L.mode === 'handsfree';
  [['modeHandsFree', hf], ['modeShow', !hf]].forEach(([id, on]) => { $(id).classList.toggle('active', on); $(id).setAttribute('aria-pressed', on); });
  let cls = hf ? 'hf' : 'show', title, sub = '';
  if (!S.connected) {
    cls += ' offline'; title = '🔌 NINCS KAPCSOLAT'; sub = 'VÁRAKOZÁS A VEZÉRLŐRE…';
  } else if (!hf) {
    if (L.showHeld) {
      cls += ' pressed'; title = '🔥 2-STEP AKTÍV 🔥';
      sub = S.reason === 7 ? 'PADTESZT • INDÍTÁS ELŐTT ENGEDD EL' : S.reason === 8 ? 'ANTI-FLOOD ZÁR • ENGEDD EL' : 'ENGEDD EL A LEÁLLÍTÁSHOZ';
    } else { title = '🏁 SHOW MÓD / 2-STEP'; sub = 'TARTSD NYOMVA A DURROGÁSHOZ ÉS LÁNGOKHOZ'; }
  } else if (L.state === 1) {
    cls += ' armed'; title = '⚡ RAJTRA KÉSZ • ARMED';
  } else if (L.state === 2) {
    cls += ' holding'; title = '🔥 TILTÁS • ' + $('cfgLaunch').value + ' RPM'; sub = 'ENGEDD FEL A KUPLUNGOT A RAJTHOZ!';
  } else if (L.state === 3) {
    cls += ' fired'; title = '🚀 RAJT! KILÖVÉS'; sub = 'TILTÁS FELOLDVA • PADLÓGÁZ!';
  } else {
    title = '🏁 RAJTAUTOMATIKA ÉLESÍTÉSE'; sub = 'KOPPINTS AZ ÉLESÍTÉSHEZ • 10 MP KÉSZENLÉT';
  }
  btn.className = 'fire-btn ' + cls;
  txt('fireTitle', title);
  if (sub) txt('fireSub', sub); else renderCountdown(Date.now());
  $('lsSteps').hidden = !hf;
  document.querySelectorAll('#lsSteps span').forEach(s => s.classList.toggle('on', S.connected && +s.dataset.s === L.state));
  $('armWarn').hidden = $('cfgArmed').checked;
}
// Show mode: BTN:1 on press, re-sent every 200 ms while held (dead-man), BTN:0 on release
function showPress() {
  if (L.showHeld) return false;
  if (!S.connected || !wsSend('BTN:1')) { toast('Nincs kapcsolat a vezérlővel', 'err'); vibrate([30, 30, 30]); return false; }
  L.showHeld = true;
  L.showTimer = setInterval(() => { if (!wsSend('BTN:1')) showRelease(); }, 200);
  vibrate(40);
  renderLaunch();
  return true;
}
function showRelease() {
  L.pid = null;
  if (!L.showHeld) return;
  L.showHeld = false;
  clearInterval(L.showTimer);
  wsSend('BTN:0');
  renderLaunch();
}
btn.addEventListener('click', () => { if (L.mode === 'handsfree') onLaunchTap(); });
btn.addEventListener('pointerdown', e => {
  if (L.mode !== 'show' || e.button > 0) return;
  e.preventDefault();
  if (showPress()) { L.pid = e.pointerId; try { btn.setPointerCapture(e.pointerId); } catch (x) {} }
});
['pointerup', 'pointercancel', 'pointerleave', 'lostpointercapture'].forEach(n => btn.addEventListener(n, e => {
  if (L.pid === null || e.pointerId === L.pid) showRelease();
}));
btn.addEventListener('keydown', e => { if (L.mode === 'show' && e.key === ' ') { e.preventDefault(); if (!e.repeat) showPress(); } });
btn.addEventListener('keyup', e => { if (e.key === ' ') showRelease(); });
btn.addEventListener('contextmenu', e => e.preventDefault());
document.addEventListener('visibilitychange', () => { if (document.hidden) showRelease(); });
window.addEventListener('blur', showRelease);

// ---- Settings: SET_CFG, SAVE_FLASH -> ACK:SAVED
// id: [min, max, step, default, title, subtitle]
const SLIDERS = {
  cfgLaunch: [2500, 6500, 50, 3800, 'Rajt limit (2-step)', 'Ezen a fordulaton tart a rajt és a show mód'],
  cfgDrop: [300, 1500, 50, 400, 'Kuplung-felengedés érzékenység', 'Ennyivel a limit alá eső fordulat = kuplung fent'],
  cfgRedline: [3000, 7500, 50, 6200, 'Fordulatszám-limit', 'E fölött mindig tilt – motorvédelem'],
  cfgDecel: [2500, 6000, 50, 3200, 'Bekapcsolási küszöb', 'Csak e fordulat fölötti gázelvételnél durrog'],
  cfgTimeout: [1, 6, 0.5, 3, 'Anti-flood: max. teljes tiltás', 'Ennyi 100%-os tiltás után szikrát is enged (gyertyavédelem)']
};
document.querySelectorAll('[data-sl]').forEach(d => {
  const id = d.dataset.sl, [min, max, step, val, title, sub] = SLIDERS[id];
  const stepBtn = (sign, label) => `<button class="step" data-t="${id}" data-d="${sign}${step}" aria-label="${label}">${sign === '-' ? '−' : '+'}</button>`;
  d.insertAdjacentHTML('afterbegin', `<div class="row"><div class="lbl"><b>${title}</b><small>${sub}</small></div><output id="val${id.slice(3)}" class="val"></output></div>`
    + `<div class="sl-row">${stepBtn('-', 'csökkentés')}<input type="range" id="${id}" min="${min}" max="${max}" step="${step}" value="${val}" aria-label="${title}">${stepBtn('', 'növelés')}</div>`);
});

const CFG_DEF = { armed: true, launchRpm: 3800, redlineRpm: 6200, decelPops: true, decelRpm: 3200, cutPattern: 1, maxCutSeconds: 3.0, ghostCam: false, launchDrop: 400 };
const INPUTS = { armed: 'cfgArmed', launchRpm: 'cfgLaunch', launchDrop: 'cfgDrop', redlineRpm: 'cfgRedline', decelPops: 'cfgDecelPops', decelRpm: 'cfgDecel', ghostCam: 'cfgGhostCam', maxCutSeconds: 'cfgTimeout' };
const KEY_OF = {};
for (const k in INPUTS) KEY_OF[INPUTS[k]] = k;
const PATTERN_NAMES = ['KEMÉNY TILTÁS', 'LÁNGCSÓVA', 'DURROGÁS', 'AK-47', 'ÁGYÚLÖVÉS'];
let curPattern = 1, cfgLoaded = false;
// dirty = changed since the last confirmed save; pend = not sent yet
const dirty = new Set(), pend = new Set();
let saving = false, savingKeys = null, saveTimer = 0, lastCfgSentAt = 0, cfgSendTimer = 0;
const isDirty = () => dirty.size > 0;

function readCfg() {
  const c = {};
  for (const k in INPUTS) { const el = $(INPUTS[k]); c[k] = el.type === 'checkbox' ? el.checked : Number(el.value); }
  if (c.maxCutSeconds >= 6) c.maxCutSeconds = 0;   // slider end = unlimited
  c.cutPattern = curPattern;
  return c;
}
function applyCfg(c, skip) {
  const use = k => c[k] !== undefined && c[k] !== null && !(skip && skip.has(k));
  for (const k in INPUTS) {
    if (!use(k)) continue;
    const el = $(INPUTS[k]), v = c[k];
    if (el.type === 'checkbox') el.checked = !!v;
    else el.value = k === 'maxCutSeconds' && (v <= 0.05 || v >= 6) ? 6 : v;
  }
  if (use('cutPattern')) curPattern = Math.max(0, Math.min(4, c.cutPattern | 0));
}
function renderCfg() {
  const c = readCfg();
  const sum = (id, s, cls) => { txt(id, s); $(id).className = 'sv ' + (cls || ''); };
  txt('valLaunch', c.launchRpm + ' RPM');
  txt('valDrop', '−' + c.launchDrop + ' RPM');
  txt('valRedline', c.redlineRpm + ' RPM');
  txt('valDecel', c.decelRpm + ' RPM');
  txt('valTimeout', c.maxCutSeconds ? dec(c.maxCutSeconds, 1) + ' s' : 'NINCS LIMIT (∞)');
  $('valTimeout').classList.toggle('danger', !c.maxCutSeconds);
  txt('dropHint', 'Rajtkor akkor old, ha a fordulat ' + Math.max(0, c.launchRpm - c.launchDrop) + ' RPM alá esik.');
  $('launchWarn').hidden = c.launchRpm < c.redlineRpm;
  $('decelBlock').classList.toggle('off', !c.decelPops);
  sum('sumLaunch', c.launchRpm + ' RPM');
  sum('sumLimiter', c.redlineRpm + ' RPM');
  sum('sumDecel', c.decelPops ? c.decelRpm + ' RPM' : 'KI', c.decelPops ? '' : 'off');
  sum('sumSound', PATTERN_NAMES[curPattern] + (c.ghostCam ? ' + GHOST' : ''));
  sum('sumSafety', !c.armed ? 'KIKAPCSOLVA' : c.maxCutSeconds ? dec(c.maxCutSeconds, 1) + ' s' : '∞', !c.armed || !c.maxCutSeconds ? 'bad' : '');
  document.querySelectorAll('.pat').forEach(p => p.classList.toggle('active', +p.dataset.p === curPattern));
  txt('patMini', PATTERN_NAMES[curPattern]);
  document.querySelectorAll('input[type=range]').forEach(el => el.style.setProperty('--p', ((el.value - el.min) / (el.max - el.min) * 100) + '%'));
  $('mkLaunch').style.left = pct(c.launchRpm);
  $('mkRed').style.left = pct(c.redlineRpm);
  txt('lgLaunch', c.launchRpm);
  txt('lgRed', c.redlineRpm);
  setRedline(c.redlineRpm);
  $('armWarn').hidden = c.armed;
}
function onUserChange(keys) {
  keys.forEach(k => { dirty.add(k); pend.add(k); if (savingKeys) savingKeys.delete(k); });
  renderCfg();
  renderSave();
  queueSend();
}
// Only changed keys, <= 10/s with a trailing send, never before this connection's CFG
function queueSend() {
  if (!S.connected || !cfgReceived || !pend.size) return;
  const wait = 100 - (Date.now() - lastCfgSentAt);
  if (wait <= 0) sendCfgNow();
  else if (!cfgSendTimer) cfgSendTimer = setTimeout(sendCfgNow, wait);
}
function sendCfgNow() {
  clearTimeout(cfgSendTimer);
  cfgSendTimer = 0;
  if (!cfgReceived || !pend.size) return;
  const c = readCfg(), part = {};
  pend.forEach(k => { part[k] = c[k]; });
  if (wsSend('SET_CFG:' + JSON.stringify(part))) { lastCfgSentAt = Date.now(); pend.clear(); }
}
function onConfig(json) {
  let c;
  try { c = JSON.parse(json); } catch (e) { return; }
  if (!c || typeof c !== 'object') return;
  cfgReceived = true;
  if (c.fw) setFw(String(c.fw));
  // device values win, except unsaved user changes (re-sent: the ESP may have rebooted)
  const keep = new Set([...dirty, ...pend]);
  applyCfg(c, keep);
  keep.forEach(k => pend.add(k));
  if (!cfgLoaded) { cfgLoaded = true; $('settings').classList.remove('loading'); }
  renderCfg();
  renderSave();
  renderLaunch();
  sendCfgNow();
}
function renderSave() {
  const d = isDirty();
  $('saveBtn').className = 'save-btn' + (saving ? ' saving' : d ? ' dirty' : '');
  $('saveBtn').disabled = saving || !cfgLoaded;
  txt('saveTxt', !cfgLoaded ? '⏳ BEÁLLÍTÁSOK BETÖLTÉSE…' : saving ? '⏳ MENTÉS…' : d ? '💾 MENTÉS A FLASH-BE' : '✔ BEÁLLÍTÁSOK MENTVE');
  txt('saveSub', !cfgLoaded ? 'A vezérlő elküldi a jelenlegi értékeket' : saving ? 'Várakozás a vezérlő visszaigazolására' : d ? 'Nem mentett változás – újraindításkor elveszne' : 'Induláskor is ezeket tölti be');
}
function saveToFlash() {
  if (saving) return;
  if (!S.connected || !cfgReceived) { toast(S.connected ? 'Várj, amíg a beállítások betöltődnek' : 'Nincs kapcsolat – most nem lehet menteni', 'err'); return; }
  sendCfgNow();
  saving = true;
  savingKeys = new Set(dirty);
  renderSave();
  // separate the frames; the ACK may take ~1 s
  setTimeout(() => {
    if (!saving) return;
    if (!wsSend('SAVE_FLASH')) { saveFailed('Nincs kapcsolat.'); return; }
    saveTimer = setTimeout(() => saveFailed('Nem jött visszaigazolás.'), 4000);
  }, Math.max(0, 150 - (Date.now() - lastCfgSentAt)));
}
function onSaved() {
  if (!saving) return;
  clearTimeout(saveTimer);
  saving = false;
  savingKeys.forEach(k => dirty.delete(k));   // keys changed during the save stay dirty
  savingKeys = null;
  renderSave();
  toast('✔ Beállítások elmentve a flash-be');
  vibrate(30);
}
function saveFailed(why) {
  clearTimeout(saveTimer);
  saving = false;
  savingKeys = null;
  renderSave();
  toast('A mentés nem sikerült: ' + why + ' Próbáld újra.', 'err');
}
function restoreDefaults() {
  if (!S.connected || !cfgReceived) { toast('Nincs kapcsolat a vezérlővel', 'err'); return; }
  if (!confirm('Visszaállítod az ajánlott értékeket?\nA jelenlegi beállítások felülíródnak és a flash-be mentődnek.')) return;
  applyCfg(CFG_DEF);
  onUserChange(Object.keys(CFG_DEF));
  saveToFlash();
}
// Inputs, pattern cards and the -/+ steppers
document.querySelectorAll('#settings input').forEach(el => el.addEventListener(el.type === 'range' ? 'input' : 'change', () => onUserChange([KEY_OF[el.id]])));
document.querySelectorAll('#pats .pat').forEach(p => {
  p.insertAdjacentHTML('beforeend', '<span class="seq">' + p.dataset.seq.split('').map(ch => `<i class="${ch === 'o' ? 'f' : ch === 'L' ? 'L' : ''}"></i>`).join('') + '</span>');
  p.addEventListener('click', () => { if (curPattern !== +p.dataset.p) { curPattern = +p.dataset.p; onUserChange(['cutPattern']); } });
});
function stepSlider(b) {
  const el = $(b.dataset.t);
  const v = Math.min(+el.max, Math.max(+el.min, Math.round((+el.value + parseFloat(b.dataset.d)) * 10) / 10));
  if (v !== +el.value) { el.value = v; onUserChange([KEY_OF[el.id]]); }
}
document.querySelectorAll('.step').forEach(b => b.addEventListener('click', () => stepSlider(b)));

// ---- Device info + firmware update (HTTP API)
const DEF_REPO = 'eng4t3/SwiftPopsAndBangs';
const FW = { info: null, fw: '', code: 0, repo: DEF_REPO, branch: 'main', manifest: null, busy: false, reloading: false };
const verCode = v => { const m = /^(\d+)\.(\d+)\.(\d+)/.exec(v || ''); return m ? m[1] * 10000 + m[2] * 100 + +m[3] : 0; };

async function api(path, o = {}) {
  if (DEMO) return sim.api(path, o);
  const ctl = new AbortController(), tm = setTimeout(() => ctl.abort(), o.timeout || 4000);
  let r, body;
  try {
    r = await fetch(path, { method: o.method || 'GET', body: o.body, cache: 'no-store', signal: ctl.signal,
      headers: o.body !== undefined ? { 'Content-Type': 'application/x-www-form-urlencoded' } : {} });
    body = await r.text();
  } catch (e) {
    const err = new Error('Nem érem el a vezérlőt (' + (ctl.signal.aborted ? 'időtúllépés' : 'hálózati hiba') + ').');
    err.net = true;
    throw err;
  } finally { clearTimeout(tm); }
  let j = null;
  try { j = JSON.parse(body); } catch (e) {}
  if (!r.ok) throw new Error((j && j.msg) || body.trim().slice(0, 200) || 'HTTP ' + r.status);
  return j === null ? body : j;
}
function setFw(fw, code) {
  if (!fw) return;
  FW.fw = fw;
  FW.code = code || verCode(fw);
  txt('fwChip', 'v' + fw + ' • ');
  $('fwChip').hidden = false;
  txt('fwCur', 'v' + fw);
  txt('sumFw', 'v' + fw);
  txt('sysFw', 'v' + fw + ' (' + FW.code + ')');
}
async function refreshInfo() {
  try { const i = await api('/api/info', { timeout: 3000 }); applyInfo(i); return i; }
  catch (e) { return null; }
}
function applyInfo(i) {
  if (!i || typeof i !== 'object') return;
  FW.info = i;
  setFw(String(i.fw || ''), +i.code || 0);
  if (/^[\w.-]+\/[\w.-]+$/.test(i.repo || '')) FW.repo = i.repo;
  if (i.branch) FW.branch = String(i.branch);
  txt('fwBuilt', i.built ? 'fordítva: ' + i.built : '');
  txt('sysHeap', i.heap ? kb(i.heap) : '–');
  // prefill the ESP form, never over the user's edits
  const fill = (id, v) => { const el = $(id); if (v !== undefined && !el.dataset.edited && document.activeElement !== el) { if (el.type === 'checkbox') el.checked = !!v; else el.value = v || ''; } };
  fill('staSsid', i.staSsid);
  fill('espRepo', i.repo);
  fill('espBranch', i.branch);
  fill('espAuto', i.autoCheck);
  $('staPass').placeholder = i.staSsid ? 'üresen hagyva változatlan' : 'a Wi-Fi jelszava';
}
// Status line + progress bar of an update path
function otaUi(id) {
  const box = $(id), msg = box.querySelector('.ota-msg'), bar = box.querySelector('.bar i');
  return {
    set(text, kind, pct) {
      const prog = pct !== undefined && pct !== null;
      box.hidden = false;
      box.className = 'ota-st ' + (kind || '') + (prog ? ' prog' : '');
      msg.textContent = text;
      if (prog) bar.style.width = pct + '%';
    },
    progress(label, got, total) {
      const pct = total ? Math.min(100, Math.round(got / total * 100)) : 0;
      this.set(label + ': ' + (total ? pct + '% (' + kb(got) + ' / ' + kb(total) + ')' : kb(got)), 'info', pct);
    }
  };
}
const p1 = otaUi('p1Status'), p2 = otaUi('p2Status'), p3 = otaUi('p3Status');
function setBusy(on) {
  FW.busy = on;
  ['p1Check', 'p1Install', 'p3Install'].forEach(id => { $(id).disabled = on; });
}
// POST /update (multipart field "update"); ?md5= lets the ESP verify the image
function flashBlob(blob, md5, ui) {
  if (DEMO) return sim.upload(blob, ui);
  return new Promise((resolve, reject) => {
    suspendWS(true);
    let sentAll = false;
    const fd = new FormData();
    fd.append('update', blob, 'firmware.bin');
    const x = new XMLHttpRequest();
    x.open('POST', '/update' + (/^[0-9a-f]{32}$/i.test(md5 || '') ? '?md5=' + md5.toLowerCase() : ''));
    x.timeout = 180000;
    x.upload.onprogress = e => { if (e.lengthComputable) { sentAll = e.loaded >= e.total; ui.progress('Feltöltés a vezérlőre', e.loaded, e.total); } };
    x.onload = () => {
      if (x.status === 200) { ui.set('Feltöltve – a vezérlő újraindul…', 'ok', 100); resolve(); return; }
      suspendWS(false);
      const t = (x.responseText || '').trim().slice(0, 200);
      reject(new Error(x.status === 409
        ? 'A vezérlő most nem frissíthető' + (t ? ': ' + t : '') + '. Állítsd le a motort (vagy hagyd alapjáraton), és próbáld újra.'
        : 'A vezérlő elutasította a frissítést (HTTP ' + x.status + (t ? ': ' + t : '') + '). A régi firmware megmaradt.'));
    };
    x.onerror = x.ontimeout = () => {
      if (sentAll) { resolve(); return; }   // the reboot can cut the reply short
      suspendWS(false);
      reject(new Error('Megszakadt a kapcsolat feltöltés közben. Ellenőrizd, hogy a telefon a Swift-PopsAndBangs Wi-Fi-n van, és próbáld újra – a régi firmware addig megmarad.'));
    };
    x.send(fd);
  });
}
// After flashing, confirm the reboot via /api/info: by the target version if known, else by
// `uptime` (ms since boot) < time since the upload; without `uptime`: soft message.
async function waitForVersion(expect, ui) {
  const t0 = Date.now(), el = () => Date.now() - t0;
  const fail = msg => { suspendWS(false); throw new Error(msg); };
  ui.set('Újraindulás – várakozás a vezérlőre…', 'info');
  await sleep(4000);
  while (el() < 90000) {
    const i = await refreshInfo();
    if (i) {
      const code = +i.code || verCode(i.fw);
      const rebooted = typeof i.uptime === 'number' ? i.uptime < el() + 2000 : null;
      if (expect ? code >= expect : rebooted !== false) return updateDone(i, ui, !!expect || rebooted);
      if (rebooted || (rebooted === null && el() > 12000)) fail('A vezérlő újraindult, de még a v' + i.fw + ' fut – az új firmware nem aktiválódott. Próbáld újra.');
      if (el() > 25000) fail('A vezérlő nem indult újra, még a v' + i.fw + ' fut – a frissítés nem történt meg. Próbáld újra.');
    }
    ui.set('Újraindulás – várakozás a vezérlőre… (' + Math.round(el() / 1000) + ' s)', 'info');
    await sleep(1500);
  }
  fail('A vezérlő 90 mp alatt nem jelentkezett. Csatlakozz újra a Swift-PopsAndBangs Wi-Fi-hez, és töltsd újra az oldalt.');
}
function updateDone(i, ui, sure) {
  suspendWS(false);
  if (!sure) { ui.set('Újraindult? Ellenőrizd a verziót – a vezérlő most ezt jelzi: v' + i.fw, 'warn'); return; }
  ui.set('✔ Sikeres frissítés – most a v' + i.fw + ' fut.' + (DEMO ? '' : ' Az oldal újratöltődik…'), 'ok', 100);
  toast('✔ Firmware frissítve: v' + i.fw);
  vibrate([60, 40, 60]);
  if (!DEMO) { FW.reloading = true; setTimeout(() => location.reload(), 2500); }
}

// Path 1: the phone downloads from GitHub (raw.githubusercontent.com, CORS ok), then uploads
const OTHER_WAYS = ' Használd a „Frissítés Wi-Fi-n” módot (az ESP tölti le), vagy a kézi feltöltést.';
async function ghFetch(path, timeout) {
  if (DEMO) return sim.gh(path);
  const branchPath = FW.branch.split('/').map(encodeURIComponent).join('/');
  const url = 'https://raw.githubusercontent.com/' + FW.repo + '/' + branchPath + '/' + path + '?t=' + Date.now();
  const ctl = new AbortController(), tm = setTimeout(() => ctl.abort(), timeout);
  let r;
  try { r = await fetch(url, { cache: 'no-store', signal: ctl.signal }); }
  catch (e) {
    throw new Error((ctl.signal.aborted
      ? 'A GitHub nem válaszolt időben – lassú vagy nincs mobilnet.'
      : 'Nem érem el a GitHubot: nincs mobilnet, vagy a telefon nem használja az autó Wi-Fi-je mellett (Androidon kapcsold be, és maradj a Wi-Fi-n internet nélkül; iPhone-on többnyire nem megy).') + OTHER_WAYS);
  } finally { clearTimeout(tm); }
  if (r.ok) return r;
  const where = ' (' + FW.repo + ', ág: ' + FW.branch + ')';
  if (r.status === 404) throw new Error('A ' + path + ' nem található, vagy a tároló nem elérhető' + where + '. Ellenőrizd a tároló és az ág nevét (Wi-Fi-s frissítés → Haladó).');
  throw new Error('A GitHub most nem elérhető (HTTP ' + r.status + ')' + where + '. Próbáld újra pár perc múlva.' + OTHER_WAYS);
}
async function phoneCheck() {
  if (FW.busy) return;
  setBusy(true);
  $('p1Install').hidden = true;
  p1.set('Verzió lekérdezése a GitHubról…', 'info');
  try {
    await refreshInfo();
    const r = await ghFetch('version.json', 15000);
    let m;
    try { m = JSON.parse(await r.text()); } catch (e) { throw new Error('A version.json hibás (nem JSON).'); }
    if (!m || !m.version || !(+m.code > 0)) throw new Error('A version.json hiányos (version / code).');
    m.code = +m.code;
    FW.manifest = m;
    const newer = !FW.code || m.code > FW.code;
    if (!FW.code) p1.set('A GitHubon a v' + m.version + ' (' + kb(m.size) + ') érhető el; a vezérlő verziója nem ismert.', 'go');
    else if (newer) p1.set('Új verzió elérhető: v' + m.version + ' (' + kb(m.size) + ') – most: v' + FW.fw, 'go');
    else if (m.code === FW.code) p1.set('✔ Naprakész – a legújabb v' + m.version + ' fut.', 'ok');
    else p1.set('A vezérlőn újabb (v' + FW.fw + ') fut, mint a GitHubon (v' + m.version + ').', 'ok');
    $('p1Install').hidden = !newer;
    txt('p1Install', '⬇ TELEPÍTÉS: v' + m.version);
  } catch (e) { p1.set(e.message, 'err'); }
  setBusy(false);
}
async function downloadFirmware(m) {
  const r = await ghFetch('firmware.bin', 20000);
  const total = +m.size || parseInt(r.headers.get('content-length'), 10) || 0;
  const rd = r.body.getReader(), parts = [];
  let got = 0;
  for (;;) {
    let stall;
    const chunk = await Promise.race([rd.read(), new Promise((_, rej) => { stall = setTimeout(() => rej(new Error('Elakadt a letöltés (20 mp óta nem jön adat). Próbáld újra.')), 20000); })])
      .finally(() => clearTimeout(stall))
      .catch(e => { rd.cancel().catch(() => {}); throw e; });
    if (chunk.done) break;
    parts.push(chunk.value);
    got += chunk.value.length;
    p1.progress('Letöltés a GitHubról', got, total);
  }
  const data = new Uint8Array(got);
  let o = 0;
  for (const p of parts) { data.set(p, o); o += p.length; }
  if (+m.size && data.length !== +m.size) throw new Error('A letöltött fájl mérete (' + data.length + ' B) eltér a várttól (' + m.size + ' B). Próbáld újra.');
  if (data[0] !== 0xE9) throw new Error('A letöltött fájl nem érvényes ESP32 firmware. Próbáld újra később.');
  return new Blob([data], { type: 'application/octet-stream' });
}
async function phoneInstall() {
  const m = FW.manifest;
  if (!m || FW.busy) return;
  if (!confirm('Telepíted a v' + m.version + ' firmware-t (' + kb(m.size) + ')? A vezérlő utána újraindul.\n\nÁlló motornál frissíts, és ne zárd be az oldalt.')) return;
  setBusy(true);
  try {
    await flashBlob(await downloadFirmware(m), m.md5, p1);
    await waitForVersion(+m.code, p1);
    $('p1Install').hidden = true;
  } catch (e) { p1.set(e.message, 'err'); }
  setBusy(false);
}

// Path 2: the ESP downloads by itself over an internet Wi-Fi; the page polls its status
const OTA_LABEL = { idle: 'Tétlen', connecting: 'Csatlakozás a Wi-Fi-hez…', checking: 'Verzió ellenőrzése…', available: 'Új verzió elérhető', uptodate: 'Naprakész', downloading: 'Letöltés…', flashing: 'Írás a flash-be…', done: 'Kész – újraindulás…', error: 'Hiba' };
const OTA_BUSY = ['connecting', 'checking', 'downloading', 'flashing'];
const poll = { on: false, timer: 0, fails: 0, wasBusy: false, expect: 0, waiting: false };

async function espSaveCfg() {
  const i = FW.info || {}, p = new URLSearchParams();
  const ssid = $('staSsid').value.trim(), open = $('staOpen').checked, pass = open ? '' : $('staPass').value;
  const repo = $('espRepo').value.trim(), branch = $('espBranch').value.trim(), auto = $('espAuto').checked;
  if (open && !ssid) { toast('Add meg a nyílt hálózat nevét', 'err'); return; }
  if (ssid !== (i.staSsid || '') || pass || open) p.set('ssid', ssid);
  if (pass || open) p.set('pass', pass);   // empty field alone = keep the password
  if (repo && repo !== i.repo) {
    if (!/^[\w.-]+\/[\w.-]+$/.test(repo)) { toast('A tároló formátuma: felhasználó/név', 'err'); return; }
    p.set('repo', repo);
  }
  if (branch && branch !== i.branch) p.set('branch', branch);
  if (auto !== !!i.autoCheck) p.set('autoCheck', auto ? '1' : '0');
  if (![...p.keys()].length) { toast('Nincs mit menteni – nem változott semmi', 'warn'); return; }
  try {
    const r = await api('/api/ota/config', { method: 'POST', body: p.toString(), timeout: 5000 });
    if (r && r.ok === false) throw new Error(r.msg || 'A vezérlő elutasította a beállításokat.');
    ['staSsid', 'espRepo', 'espBranch', 'espAuto'].forEach(id => delete $(id).dataset.edited);
    $('staPass').value = '';
    $('staPass').disabled = $('staOpen').checked = false;
    toast('✔ Frissítési beállítások elmentve');
    refreshInfo();
  } catch (e) { toast(e.message, 'err'); }
}
async function espAction(kind) {
  if (kind === 'install' && !confirm('Az ESP letölti és telepíti az új firmware-t, majd újraindul (1–2 perc). Közben a telefon lecsatlakozhat – ha nem jön vissza magától, csatlakozz újra.\n\nÁlló motornál frissíts. Indulhat?')) return;
  try {
    const r = await api('/api/ota/' + kind, { method: 'POST', body: '', timeout: 6000 });
    if (r && r.ok === false) throw new Error(r.msg || 'A vezérlő elutasította a kérést.');
    if (kind !== 'cancel') p2.set(kind === 'install' ? 'Telepítés indul…' : 'Ellenőrzés indul…', 'info');
  } catch (e) {
    // the reply can get lost when the ESP switches Wi-Fi channel: keep polling
    if (!e.net) { p2.set(e.message, 'err'); return; }
    p2.set('A kérés elment, de nem jött válasz – figyelem az állapotot…', 'warn');
  }
  pollStart();
}
function pollStart() {
  if (poll.on) return;
  poll.on = true;
  pollTick();
}
async function pollTick() {
  clearTimeout(poll.timer);
  let s = null;
  try { s = await api('/api/ota/status', { timeout: 2500 }); poll.fails = 0; }
  catch (e) { poll.fails++; }
  if (s) renderOta(s);
  else if (poll.fails > 1) p2.set('⚠ Nincs kapcsolat (az ESP épp a másik Wi-Fi-n lehet) – újrapróbálom… (' + poll.fails + ')', 'warn');
  const busy = s ? OTA_BUSY.includes(s.state) : poll.wasBusy;
  if (poll.waiting || (!busy && !($('grpFw').open && $('p2Box').open))) { poll.on = false; return; }
  poll.timer = setTimeout(pollTick, s ? 1000 : 2000);
}
function renderOta(s) {
  const st = s.state || 'idle', busy = OTA_BUSY.includes(st);
  let t = st === 'available' && s.latest ? 'Új verzió elérhető: v' + s.latest + (s.current ? ' (most: v' + s.current + ')' : '') : OTA_LABEL[st] || st;
  if (s.msg) t += ' – ' + s.msg;
  if (s.staIp && busy) t += ' • ESP IP: ' + s.staIp;
  const pct = st === 'downloading' || st === 'flashing' ? Math.max(0, Math.min(100, s.progress | 0)) : null;
  p2.set(t, st === 'error' ? 'err' : st === 'uptodate' ? 'ok' : st === 'available' || st === 'done' ? 'go' : 'info', pct);
  $('p2Install').disabled = st !== 'available';
  $('p2Check').disabled = busy;
  $('p2Cancel').hidden = !busy;
  if (s.latestCode) poll.expect = +s.latestCode;
  if (pct !== null) poll.wasBusy = true;
  // done, or already rebooted into the new version
  const finished = st === 'done' || (poll.wasBusy && !busy && poll.expect > 0 && verCode(s.current) >= poll.expect);
  if (!finished && !busy) poll.wasBusy = false;   // cancelled, failed or idle
  if (finished && !poll.waiting) {
    poll.waiting = true;
    poll.wasBusy = false;
    waitForVersion(poll.expect, p2).catch(e => p2.set(e.message, 'err')).finally(() => { poll.waiting = false; });
  }
}

// Path 3: manual .bin upload
let pickedFile = null;
async function pickFile(inp) {
  const f = inp.files && inp.files[0], nm = $('p3Name');
  inp.value = '';
  pickedFile = null;
  $('p3Install').hidden = true;
  if (!f) return;
  let ok = /\.bin$/i.test(f.name);
  if (ok) { try { ok = new Uint8Array(await f.slice(0, 1).arrayBuffer())[0] === 0xE9; } catch (e) {} }
  nm.className = 'hint center ' + (ok ? 'good' : 'bad');
  if (!ok) { nm.textContent = '✕ ' + f.name + ' – ez nem ESP32 firmware (.bin) fájl.'; return; }
  pickedFile = f;
  nm.textContent = '✔ ' + f.name + ' (' + kb(f.size) + ')';
  $('p3Install').hidden = false;
}
async function fileInstall() {
  if (!pickedFile || FW.busy) return;
  if (!confirm('Telepíted: ' + pickedFile.name + '? A vezérlő utána újraindul.\n\nÁlló motornál frissíts, és ne zárd be az oldalt.')) return;
  setBusy(true);
  try { await flashBlob(pickedFile, '', p3); await waitForVersion(0, p3); }
  catch (e) { p3.set(e.message, 'err'); }
  setBusy(false);
}

// ---- Data logger (/api/log/*): status, lists, CSV viewer
const LOG_ICON = { launch: '🏁', redline: '⚡', flood: '🛡', anomaly: '⚠', manual: '📌' };
const SMP_COLS = 't_ms,rpm,cut,est,show,switch,armed,inhibit,fresh,launch,reason,cut_slots,fired_slots,meas_age_ms'.split(',');
const esc = v => String(v == null ? '' : v).replace(/[&<>"]/g, c => '&#' + c.charCodeAt(0) + ';');
const mmss = ms => (ms / 60000 | 0) + ':' + String((ms / 1000 | 0) % 60).padStart(2, '0');
const durTxt = ms => ms < 60000 ? dec(ms / 1000, 1) + ' s' : mmss(ms);
const logUi = otaUi('logSt'), LV = {};
let logKey = '';

async function logRefresh(force) {
  const st = await api('/api/log/status', { timeout: 3000 }).catch(e => e.message);
  if (!st.ok) return logUi.set(typeof st === 'string' && st || 'Ezen a firmware-en nincs adatnapló (v2.1 kell).', 'warn');
  const pf = st.poolFree || {}, low = pf.cap === 0 || pf.drive === 0;
  logUi.set([st.flash ? '✔ Flash rendben' : '⚠ Flash hiba, csak RAM-puffer', st.captures + ' esemény, ' + st.drives + ' út',
    st.writing ? '● felvétel fut' : '⏸ írás szünetel', st.pending && st.pending + ' mentésre vár',
    st.gapNow ? 'most hézag van az útnaplóban' : low && 'elfogyott a tartalék – alapjáraton pótolja'].filter(x => x).join(' • '), !st.flash || st.gapNow || low ? 'warn' : 'ok');
  txt('sumLog', (st.writing ? '● ' : '') + st.captures + ' esemény');
  const key = [st.captures, st.drives, st.boot] + '';
  if (force || key !== logKey) { logKey = key; logList(); }
}
async function logList() {
  const j = await api('/api/log/list', { timeout: 5000 }).catch(() => ({})), none = w => '<p class="hint">Még nincs ' + w + '.</p>';
  $('logCaps').innerHTML = (j.captures || []).map(c => `<button class="li" data-cap="${+c.id}"><span>${LOG_ICON[c.type] || '•'}</span><span><b>${c.boot}. út, ${mmss(c.t0)}</b> · ${durTxt(c.dur)}<small>${esc(c.sum)}</small></span></button>`).join('') || none('esemény');
  $('logDrives').innerHTML = (j.drives || []).map(d => `<div class="li"><span>🚗</span><span><b>${d.boot}. út</b> · ${durTxt(d.dur)}<small>${+d.gaps} hézag</small></span>`
    + `<button class="btn sm" data-drive="${+d.boot}" data-n="${+d.n}">MEGNYITÁS</button><a class="btn sm" href="/api/log/drive.csv?boot=${+d.boot}" download="swift_drive_${+d.boot}.csv">⬇ CSV</a></div>`).join('') || none('út');
}
async function logPost(what, done) {
  const r = await api('/api/log/' + what, { method: 'POST', body: '', timeout: 5000 }).catch(e => ({ ok: false, msg: e.message }));
  if (r.ok === false) toast(r.msg || 'A vezérlő elutasította.', 'err'); else done(r);
}
function logSnap() { logPost('snap', r => { toast('📌 Mentve: #' + r.id); setTimeout(() => logRefresh(true), 1500); }); }
function logClear() {
  if (confirm('Törlöd az összes eseményt és utat az adatnaplóból?\nEz nem vonható vissza.')) logPost('clear', () => { toast('🗑 Adatnapló törölve'); lvClose(); logRefresh(true); });
}
// `# key=value` header, samples, then an optional `# trace` section
function parseLog(text) {
  const d = { h: {}, t: [], r: [], c: [], l: [], tr: [], max: 0 };
  let cols = SMP_COLS, tr = 0;
  for (const line of String(text).split('\n')) {
    const v = line.trim(), a = v.split(','), m = /^#\s*([\w.]+)=(.*)/.exec(v), g = k => +a[cols.indexOf(k)] || 0;
    if (/^#\s*trace/.test(v)) { tr = 1; cols = ['t_us', 'kind']; }
    else if (m) d.h[m[1]] = m[2].trim();
    else if (!v || v[0] === '#') continue;
    else if (isNaN(a[0])) cols = a;
    else if (tr) {
      let t = g('t_us') / 1000;
      while (t < d.t[0] - 2147483) t += 4294967.296;   // micros() wraps every ~71.6 min
      d.tr.push([t, g('kind')]);
    } else { d.t.push(g('t_ms')); d.r.push(g('rpm')); d.c.push(g('cut')); d.l.push(g('launch')); d.max = Math.max(d.max, g('rpm')); }
  }
  return d;
}
async function lvOpen(url, title, drive) {
  $('logView').hidden = false;
  txt('lvTitle', title + ' …');
  const d = parseLog(await api(url, { timeout: 30000 }).catch(e => { toast(e.message, 'err'); return ''; }));
  txt('lvTitle', title);
  if (!d.t.length) return lvClose();
  const h = d.h, hold = d.r.filter((r, i) => d.l[i] === 2), row = (k, v) => `<dt>${esc(k)}</dt><dd>${esc(v)}</dd>`;
  LV.d = Object.assign(d, { drive });
  $('lvZoom').value = $('lvPan').value = 0;
  $('lvCsv').href = drive ? '/api/log/drive.csv?boot=' + drive : url;
  $('lvCsv').download = 'swift_' + (drive ? 'drive_' + drive : h.type + '_' + h.id) + '.csv';
  // counts, max, HOLDING stats, header summary / diag fields
  $('lvKv').innerHTML = row('Minták', d.t.length + (d.tr.length ? ' + ' + d.tr.length + ' gyújtás' : '')) + row('Max. RPM', d.max)
    + (hold.length ? row('HOLDING min/átl/max', Math.min(...hold) + ' / ' + Math.round(hold.reduce((a, b) => a + b) / hold.length) + ' / ' + Math.max(...hold)) : '')
    + Object.keys(h).filter(k => !(k in CFG_DEF) && !/^(fw|type|id|boot|t0_ms|trigger_ms)$/.test(k)).map(k => row(k, h[k])).join('');
  lvDraw();
  $('logView').scrollIntoView({ block: 'nearest' });
}
function lvClose() { $('logView').hidden = true; LV.d = null; }
// RPM line, cut shading, launch strip, limits, trace lane
function lvDraw() {
  const d = LV.d, cv = $('lvCv');
  if (!d) return;
  const W = cv.clientWidth, H = d.tr.length ? 206 : 180, p = devicePixelRatio || 1, g = cv.getContext('2d'), n = d.t.length, num = k => +d.h[k] || 0;
  cv.style.height = H + 'px';
  cv.width = W * p; cv.height = H * p;
  g.setTransform(p, 0, 0, p, 0, 0);
  g.font = '10px sans-serif';
  g.fillStyle = '#8391aa';
  const T0 = d.t[0], span = d.t[n - 1] - T0 || 1, vs = span / 2 ** ($('lvZoom').value / 10), va = T0 + $('lvPan').value / 1000 * (span - vs);
  const ym = Math.ceil(Math.max(d.max, num('redlineRpm')) / 1000 + 0.3) * 1000, gap = span / n * 4, ref = d.drive ? 0 : num('trigger_ms') || T0;
  const X = t => 34 + (t - va) / vs * (W - 38), Y = r => 140 - r / ym * 134, lr = num('launchRpm'), rl = num('redlineRpm');
  const hl = (r, c, dash) => { g.strokeStyle = c; g.setLineDash(dash); g.beginPath(); g.moveTo(34, Y(r)); g.lineTo(W, Y(r)); g.stroke(); };
  for (let r = 0; r <= ym; r += ym > 5000 ? 2000 : 1000) { hl(r, '#1c2538', []); g.fillText(r, 2, Y(r) + 3); }
  ['left', 'center', 'right'].forEach((a, k) => {
    const t = va + vs * k / 2, x = t - ref;
    g.textAlign = a;
    g.fillText(d.drive ? mmss(t) : (x > 0 ? '+' : '') + dec(x / 1000, 2) + ' s', 34 + (W - 38) * k / 2, H - 4);
  });
  g.save();
  g.beginPath(); g.rect(34, 0, W, H); g.clip();
  for (let i = 0; i < n - 1; i++) {
    const x = X(d.t[i]), w = Math.max(1, X(d.t[i + 1]) - x), hole = d.t[i + 1] - d.t[i] > gap;
    g.fillStyle = hole ? '#ffffff10' : '#ff224738';
    if (hole || d.c[i]) g.fillRect(x, 6, w, 134);
    if (d.l[i] && !hole) { g.fillStyle = ['', '#fa0', '#f24', '#0f8'][d.l[i]]; g.fillRect(x, 144, w, 6); }
  }
  if (lr) { hl(lr, '#fa0', [4, 4]); hl(lr - num('launchDrop'), '#fa07', [2, 4]); }
  if (rl) hl(rl, '#f24', [4, 4]);
  g.setLineDash([]);
  g.strokeStyle = '#0ef';
  g.lineWidth = 1.5;
  g.beginPath();
  d.t.forEach((t, i) => g[i && t - d.t[i - 1] <= gap ? 'lineTo' : 'moveTo'](X(t), Y(d.r[i])));
  g.stroke();
  // trace: green = pulse, red = cut slot, grey = outlier / reject
  for (const [t, k] of d.tr) if (k < 4) { g.fillStyle = ['#0f8', '#f24', '#6b7a96', '#6b7a96'][k]; g.fillRect(X(t), 156, 1, 20); }
  g.restore();
}
['lvZoom', 'lvPan'].forEach(id => $(id).addEventListener('input', lvDraw));
window.addEventListener('resize', lvDraw);
$('logCaps').addEventListener('click', e => {
  const b = e.target.closest('[data-cap]');
  if (b) lvOpen('/api/log/capture.csv?id=' + b.dataset.cap, b.firstChild.textContent + ' ' + b.querySelector('b').textContent, 0);
});
$('logDrives').addEventListener('click', e => {
  const b = e.target.closest('[data-drive]');
  if (b) lvOpen('/api/log/drive.csv?boot=' + b.dataset.drive + '&step=' + Math.ceil(b.dataset.n / 3000 || 1), '🚗 ' + b.dataset.drive + '. út', +b.dataset.drive);
});
setInterval(() => { if ($('grpLog').open && !document.hidden) logRefresh(); }, 5000);

// ---- Help modal
function openModal() { $('infoModal').classList.add('open'); $('infoModal').setAttribute('aria-hidden', 'false'); }
function closeModal() { $('infoModal').classList.remove('open'); $('infoModal').setAttribute('aria-hidden', 'true'); }
$('infoModal').addEventListener('click', e => { if (e.target === $('infoModal')) closeModal(); });
window.addEventListener('keydown', e => { if (e.key === 'Escape') closeModal(); });

// ---- Demo mode (file:// or ?demo=1): simulated engine, firmware and API, no network
const sim = DEMO ? makeSim() : null;
function makeSim() {
  const s = {
    online: true, bootAt: Date.now() - 600000, cfg: Object.assign({}, CFG_DEF), rpm: 850, gas: false, btn: false, btnAt: 0, thr: false, liftAt: 0,
    ls: 0, armAt: 0, holdAt: 0, firedAt: 0, cutStart: 0, job: 0, pending: null, next: { fw: '2.1.0', code: 20100 },
    info: { fw: '2.0.0', code: 20000, built: '2026-09-24T12:00:00Z', heap: 180000, repo: DEF_REPO, branch: 'main', staSsid: '', autoCheck: false },
    ota: { state: 'idle', progress: 0, msg: '', current: '2.0.0' }
  };
  s.recv = m => {
    const now = Date.now();
    if (m === 'BTN:1') { s.btn = true; s.btnAt = now; }
    else if (m === 'BTN:0') s.btn = false;
    else if (m === 'LAUNCH:ARM') { s.ls = 1; s.armAt = now; }
    else if (m === 'LAUNCH:DISARM') s.ls = 0;
    else if (m === 'GET_CONFIG') setTimeout(() => onMessage('CFG:' + JSON.stringify(Object.assign({ fw: s.info.fw }, s.cfg))), 60);
    else if (m.startsWith('SET_CFG:')) Object.assign(s.cfg, JSON.parse(m.slice(8)));
    else if (m === 'SAVE_FLASH') setTimeout(() => onMessage('ACK:SAVED'), 300);
  };
  // ~30 Hz engine model (launch states, cut priority, anti-flood)
  s.tick = () => {
    if (!s.online) return;
    const now = Date.now(), c = s.cfg;
    if (s.btn && now - s.btnAt > 600) s.btn = false;   // dead-man, like the firmware
    // demo driver: floors it when armed, drops the clutch after ~2.5 s
    const clutch = s.ls === 2 && now - s.holdAt > Math.min(2500, (c.maxCutSeconds || 9) * 1000 - 600);
    const thr = s.gas || s.btn || s.ls === 2 || (s.ls === 1 && now - s.armAt > 1200) || (s.ls === 3 && now - s.firedAt < 1800);
    if (s.thr && !thr) s.liftAt = now;
    s.thr = thr;
    const idle = c.ghostCam ? 900 + Math.sin(now / 230) * 150 : 850 + Math.sin(now / 1500) * 30;
    s.rpm = clutch ? s.rpm - 260 : thr ? s.rpm + 110 + Math.random() * 70 : Math.max(idle, s.rpm - 120);
    if (s.ls === 1) { if (now - s.armAt > 10000) s.ls = 0; else if (s.rpm >= c.launchRpm - 200) { s.ls = 2; s.holdAt = now; } }
    else if (s.ls === 2) {
      if (now - s.holdAt >= 400 && s.rpm < c.launchRpm - c.launchDrop) { s.ls = 3; s.firedAt = now; }
      else if (c.maxCutSeconds > 0 && now - s.holdAt > c.maxCutSeconds * 1000) s.ls = 0;
    } else if (s.ls === 3 && now - s.firedAt > 2000) s.ls = 0;
    let want = 0;
    if (c.armed) {
      if ((s.btn || s.ls === 2) && s.rpm >= c.launchRpm) want = s.btn ? 1 : 2;
      else if (s.rpm >= c.redlineRpm) want = 4;
      else if (c.decelPops && !thr && s.rpm >= c.decelRpm && now - s.liftAt < 1200) want = 5;
      else if (c.ghostCam && !thr && s.rpm < 1300 && now % 700 < 90) want = 6;
    }
    if (!want || want === 6) s.cutStart = 0; else if (!s.cutStart) s.cutStart = now;
    const lock = s.cutStart && c.maxCutSeconds > 0 && now - s.cutStart > c.maxCutSeconds * 1000;
    const lim = want === 4 ? c.redlineRpm : want && want < 3 ? c.launchRpm : 0;
    if (lim && !lock && s.rpm > lim) s.rpm = lim - 60 - Math.random() * 220;
    s.rpm = Math.max(0, Math.min(MAX_RPM, s.rpm + (Math.random() - 0.5) * 24));
    onMessage('T:' + Math.round(s.rpm) + ',' + (want ? 1 : 0) + ',' + s.ls + ',' + (s.ls === 1 ? Math.max(0, 10000 - (now - s.armAt)) : 0) + ',' + (lock ? 8 : want));
  };
  s.reboot = t => {
    s.online = false;
    linkDown();
    setTimeout(() => {
      Object.assign(s.info, { fw: t.fw, code: t.code });
      s.ota = { state: 'idle', progress: 0, msg: '', current: t.fw };
      s.ls = 0; s.btn = false; s.online = true; s.bootAt = Date.now();
      linkUp();
    }, 5000);
  };
  const step = (state, msg, progress) => Object.assign(s.ota, { state, msg, progress: progress || 0 });
  s.run = async install => {
    const job = ++s.job, wait = async ms => { await sleep(ms); if (job !== s.job) throw 0; };
    step('connecting', s.info.staSsid);
    await wait(1500);
    s.ota.staIp = '192.168.1.50';
    if (!install) {
      step('checking', '');
      await wait(1200);
      Object.assign(s.ota, { latest: s.next.fw, latestCode: s.next.code, current: s.info.fw });
      step(s.next.code > s.info.code ? 'available' : 'uptodate', '');
      return;
    }
    for (let p = 0; p <= 100; p += 4) { step('downloading', '', p); await wait(160); }
    step('flashing', '', 100);
    await wait(1200);
    step('done', '', 100);
    await wait(2500);
    s.reboot(s.next);
  };
  s.api = async (path, o) => {
    await sleep(150 + Math.random() * 150);
    if (!s.online) { const e = new Error('Nem érem el a vezérlőt (hálózati hiba).'); e.net = true; throw e; }
    const p = new URLSearchParams(o.body || ''), bad = msg => ({ ok: false, msg });
    if (path.includes('.csv')) return s.csv(path);
    switch (path) {
      case '/api/info': return Object.assign({}, s.info, { rpm: Math.round(s.rpm), uptime: Date.now() - s.bootAt, heap: 176000 + (Math.random() * 6000 | 0) });
      case '/api/ota/status': return Object.assign({}, s.ota);
      case '/api/ota/config':
        ['staSsid:ssid', 'repo:repo', 'branch:branch'].forEach(k => { const [to, from] = k.split(':'); if (p.has(from)) s.info[to] = p.get(from); });
        if (p.has('autoCheck')) s.info.autoCheck = p.get('autoCheck') === '1';
        return { ok: true };
      case '/api/ota/check':
        if (!s.info.staSsid) return bad('Nincs mentett Wi-Fi hálózat.');
        s.run(false).catch(() => {}); return { ok: true };
      case '/api/ota/install':
        if (s.ota.state !== 'available') return bad('Előbb futtass ellenőrzést.');
        s.run(true).catch(() => {}); return { ok: true };
      case '/api/ota/cancel': s.job++; step('idle', 'Megszakítva.'); return { ok: true };
      case '/api/log/status': return { ok: true, flash: true, captures: s.caps.length, drives: s.drives.length, poolFree: { cap: 20, drive: 32 }, writing: !s.cutStart && !s.ls, boot: 12 };
      case '/api/log/list': return { captures: s.caps, drives: s.drives };
      case '/api/log/snap': s.caps.unshift({ id: s.nextId, boot: 12, type: 'manual', t0: Date.now() - s.bootAt, dur: 60000, sum: 'Kézi mentés' }); return { ok: true, id: s.nextId++ };
      case '/api/log/clear': s.caps = []; s.drives = []; return { ok: true };
    }
    throw new Error('HTTP 404');
  };
  // Data logger mock: captures (with a trace per ignition slot) and drives
  s.nextId = 58;
  s.caps = [
    { id: 57, boot: 12, type: 'launch', t0: 754000, dur: 8200, sum: 'Rajt: 2,9 s tartás' },
    { id: 56, boot: 12, type: 'redline', t0: 512000, dur: 6000, sum: 'Redline: 1,5 s' },
    { id: 55, boot: 12, type: 'manual', t0: 300000, dur: 60000, sum: 'Kézi mentés' }
  ];
  s.drives = [{ boot: 12, t0: 0, dur: 1834000, n: 45850, gaps: 1 }, { boot: 11, t0: 0, dur: 912000, n: 22800, gaps: 0 }];
  const shape = (type, t) => {   // [rpm, cut, launch state] at t seconds
    const L = s.cfg.launchRpm, R = s.cfg.redlineRpm, up = 900 + (t - 1.2) * (L - 900) / 0.9;
    if (type === 'launch') return t < 1.2 ? [900, 0, 1] : t < 2.1 ? [up, 0, up < L - 200 ? 1 : 2] : t < 5 ? [L - 50 + Math.sin(t * 40) * 70, 1, 2]
      : t < 5.25 ? [L - (t - 5) * 2800, 0, t < 5.12 ? 2 : 3] : [L - 700 + (t - 5.25) * 700, 0, t < 7.2 ? 3 : 0];
    if (type === 'redline') return t < 3 ? [R - 1400 + t * 470, 0, 0] : t < 4.5 ? [R - 80 + Math.sin(t * 50) * 70, 1, 0] : [R - (t - 4.5) * 1500, +(t < 5.2), 0];
    return [880 + Math.max(0, Math.sin(t / 3)) ** 8 * 2600, 0, 0];
  };
  s.csv = url => {
    const q = new URLSearchParams(url.split('?')[1]), c = s.caps.find(x => x.id == q.get('id')), it = c || s.drives.find(x => x.boot == q.get('boot'));
    if (!it) throw new Error('HTTP 404');
    const rows = [], tr = [], k = c ? 1 : +q.get('step') || 1;
    let slot = 0;
    for (let t = 0; t < it.dur; t += 40 * k) {
      if (!c && it.gaps && t > 6e5 && t < 64e4) continue;
      const [r0, cut, l] = c ? shape(c.type, t / 1000) : [850 + Math.max(0, Math.sin(t / 41000) * 2200), 0, 0];
      const r = Math.round(r0 + Math.random() * 30), m = c && c.type !== 'manual' ? Math.round(r / 750) : 0;
      rows.push([t + it.t0, r, cut, 0, 0, 0, 1, 0, 1, l, cut && (c.type === 'launch' ? 2 : 4), cut * 3, cut ? 1 : 4, 12]);
      for (let j = 0; j < m; j++, slot++) tr.push(Math.round((it.t0 + t + j * 40 / m) * 1000) + ',' + (Math.random() < 0.015 ? 2 : cut && slot % 4 < 3 ? 1 : 0) + ',7900,0');
    }
    const h = Object.assign({ fw: s.info.fw, type: c ? c.type : 'drive', id: c ? c.id : 0, boot: it.boot, t0_ms: it.t0, trigger_ms: c ? it.t0 + (c.type === 'redline' ? 3000 : 0) : 0 }, s.cfg, c && { pulses: slot });
    return Object.keys(h).map(x => '# ' + x + '=' + h[x]).join('\n') + '\n' + SMP_COLS + '\n' + rows.join('\n') + (tr.length ? '\n# trace\nt_us,kind,period_us,info\n' + tr.join('\n') : '') + '\n';
  };
  // GitHub mock: manifest, then a streamed fake image (0xE9 first)
  s.gh = async path => {
    await sleep(500);
    if (path === 'version.json') return new Response(JSON.stringify({ version: s.next.fw, code: s.next.code, size: 880000, md5: '0123456789abcdef0123456789abcdef', built: '2026-09-24T12:00:00Z' }));
    let sent = 0;
    s.pending = s.next;
    return new Response(new ReadableStream({ async pull(ctl) {
      await sleep(45);
      const n = Math.min(32768, 880000 - sent);
      if (n <= 0) return ctl.close();
      const b = new Uint8Array(n);
      b[0] = sent ? 0 : 0xE9;
      sent += n;
      ctl.enqueue(b);
    } }));
  };
  s.upload = async (blob, ui) => {
    for (let p = 0; p <= 100; p += 5) { ui.progress('Feltöltés a vezérlőre', blob.size * p / 100, blob.size); await sleep(90); }
    ui.set('Feltöltve – a vezérlő újraindul…', 'ok', 100);
    const t = s.pending || s.info;
    s.pending = null;
    setTimeout(() => s.reboot(t), 300);
  };
  return s;
}

// ---- Start
document.querySelectorAll('[data-track]').forEach(el => el.addEventListener(el.type === 'checkbox' ? 'change' : 'input', () => { el.dataset.edited = '1'; }));
document.querySelectorAll('details.card').forEach(d => {
  const saved = store.get('open.' + d.id, null);
  if (saved !== null) d.open = saved === '1';
  d.addEventListener('toggle', () => { store.set('open.' + d.id, d.open ? '1' : '0'); if (!d.open) return; if (d.id === 'grpFw' || d.id === 'grpSys') refreshInfo(); if (d.id === 'grpLog') logRefresh(true); });
});
$('p2Box').addEventListener('toggle', () => { if ($('p2Box').open) { refreshInfo(); pollStart(); } });
window.addEventListener('beforeunload', e => { if (isDirty() && !FW.reloading) { e.preventDefault(); e.returnValue = ''; } });
// Web fonts after load, never blocking (the car's Wi-Fi has no internet)
window.addEventListener('load', () => setTimeout(() => {
  const l = document.createElement('link');
  l.rel = 'stylesheet';
  l.href = 'https://fonts.googleapis.com/css2?family=Orbitron:wght@700;800;900&family=Rajdhani:wght@600;700&display=swap';
  document.head.appendChild(l);
}, 200));

applyCfg(CFG_DEF);
renderCfg();
renderSave();
renderLaunch();
if (DEMO) {
  const gas = $('demoGas'), setGas = v => { sim.gas = v; };
  $('demoBar').hidden = false;
  gas.addEventListener('pointerdown', e => { e.preventDefault(); setGas(true); try { gas.setPointerCapture(e.pointerId); } catch (x) {} });
  ['pointerup', 'pointercancel', 'lostpointercapture'].forEach(n => gas.addEventListener(n, () => setGas(false)));
  setInterval(sim.tick, 33);
  document.addEventListener('click', e => {   // CSV links: generated files
    const a = e.target.closest('a[download]');
    if (!a) return;
    e.preventDefault();
    const x = document.createElement('a');
    x.href = URL.createObjectURL(new Blob([sim.csv(a.getAttribute('href'))]));
    x.download = a.download;
    x.click();
  });
  linkUp();
} else {
  setConn('connecting');
  connectWS();
}
</script>
</body>
</html>
)rawliteral";
