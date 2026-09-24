# Firmware ↔ Dashboard Interfaces (v2)

This is the contract between the firmware modules and the web dashboard. Change it only
together with every side that uses it.

## Source layout

| File | Responsibility |
| --- | --- |
| `swift_show_tuning/swift_show_tuning.ino` | Glue: Wi-Fi AP, HTTP `/` + `/api/diag`, WebSocket server (port 81), config protocol, NVS |
| `swift_show_tuning/engine_control.h/.cpp` | Tach ISR + 10 kHz timer ISR: RPM estimation, slot-synchronous spark cut, patterns, launch control, limiter, decel pops, ghost cam, bench test. Mirrored 1:1 in `tools/sim/engine_core.js` |
| `swift_show_tuning/ota_update.h/.cpp` | Firmware updates: browser upload, ESP-direct GitHub update, rollback, `/api/info`, `/api/ota/*`, `/update` |
| `swift_show_tuning/web_ui.h` | Embedded dashboard (`INDEX_HTML`); `tools/ui/export_preview.py` copies it to `preview.html` |
| `swift_show_tuning/version.h` | `FW_VERSION` / `FW_VERSION_CODE`, default update repo/branch |
| `scripts/post_build.py` | After `pio run`: copies `firmware.bin` to the repo root and writes `version.json` |
| `tools/sim/` | Node.js engine + tach simulator for the engine core (`node tools/sim/run.js`, `python tools/sim/check_sync.py`) |

## WebSocket (`ws://192.168.4.1:81/`)

All messages are text frames. The firmware accepts at most 2 dashboards at once (a third
replaces the least active one). A handshake with an `Origin` header is refused unless the
Origin host equals the Host header's host (port ignored) and is an IP literal or a `.local`
name. The firmware pings every 2 s (browsers answer automatically) and drops a client that
has been silent for 7 s or whose send queue stops draining for 3 s.

### Firmware → dashboard

| Message | Meaning |
| --- | --- |
| `T:<rpm>,<cut>,<launchState>,<launchLeftMs>,<reason>` | Telemetry, ~30 Hz. `rpm` is a model estimate inside cut gaps and is not capped at 8000. `cut` 0/1 = the clamp cut a spark within the last 60 ms. `launchState` 0 OFF, 1 ARMED, 2 HOLDING, 3 FIRED. `launchLeftMs` = remaining ARMED window. `reason` = `CutReason` below. Parsers must tolerate extra trailing fields and missing fields (old firmware sends only the first 3). |
| `CFG:{json}` | Current config, sent in reply to `GET_CONFIG`. Keys below plus `"fw":"x.y.z"`. |
| `ACK:SAVED` | Reply to `SAVE_FLASH` once the settings are in flash. Can take up to ~1 s: the write waits for a moment with no spark cut, because flash writes stall the CPU. |

`reason` (`CutReason`): 0 none, 1 show button, 2 hands-free launch, 3 physical switch,
4 redline, 5 decel pops, 6 ghost cam, 7 bench test (engine stopped), 8 anti-flood lock (a 100 %
cut hit `maxCutSeconds`; the engine lets sparks through, `cut` can still be 1 because the
limiter keeps working).

### Dashboard → firmware

| Message | Meaning |
| --- | --- |
| `BTN:1` | Show-mode 2-step button held. **Re-sent every 200 ms while held** (dead-man). The glue and the engine each release the button if no `BTN:1` arrives for 600 ms; the glue also releases it when the client disconnects. |
| `BTN:0` | Show-mode button released. |
| `LAUNCH:ARM` / `LAUNCH:DISARM` | Arm / cancel hands-free launch control. Arming is ignored while the master arm is off or while already HOLDING. |
| `GET_CONFIG` | Request a `CFG:` message. |
| `SET_CFG:{json}` | Apply config: any subset of keys, applied immediately, not saved. The firmware clamps values into the ranges below and does not echo. Malformed JSON is rejected as a whole; an out-of-range `cutPattern` or a negative `maxCutSeconds` is ignored. The dashboard sends only changed keys, at most 10 messages/s, and never before the first `CFG` of a connection. |
| `SAVE_FLASH` | Persist the current config to NVS. Answered with `ACK:SAVED`. |

The dashboard treats the connection as dead if no message arrives for 1500 ms and reconnects.

### Config keys and ranges

| Key | Type | Range | Default |
| --- | --- | --- | --- |
| `armed` | bool | master switch; off = the controller never cuts (no redline protection either) | `true` |
| `launchRpm` | int | 2500–6500 (step 50); the effective 2-step limit is `min(launchRpm, redlineRpm)` | 3800 |
| `redlineRpm` | int | 3000–7500 (step 50) | 6200 |
| `decelPops` | bool | | `true` |
| `decelRpm` | int | 2500–6000 (step 50) | 3200 |
| `cutPattern` | int | per ignition event: 0 hard cut (2–5 cut / 1 fire), 1 flames (3/1), 2 gunfire (2/1), 3 AK-47 (1/1), 4 cannon (~1.6 s full cut) | 1 |
| `maxCutSeconds` | float | anti-flood: max continuous 100 % cut. 0 = unlimited, else 1.0–<6.0 (UI slider 1.0–6.0, 6.0 = unlimited) | 3.0 |
| `ghostCam` | bool | | `false` |
| `launchDrop` | int | 300–1500 (step 50): RPM drop below `launchRpm` that counts as the clutch being released in hands-free launch | 400 |

## HTTP API (port 80)

Authoritative details: the header comment of `swift_show_tuning/ota_update.cpp`.

| Route | Meaning |
| --- | --- |
| `GET /` | Dashboard. `Cache-Control: no-cache` + `ETag` (firmware version + page hash) → `304` on reload. |
| `GET /api/diag` | Engine counters for car testing (accepted/rejected tach pulses, cut slots, anti-flood trips, last launch drop rate, …). |
| `GET /api/info` | `{"fw","code","built","heap","uptime","repo","branch","staSsid","autoCheck","rpm"}`. `uptime` = ms since boot (proves a reboot after an update). The Wi-Fi password is never returned. |
| `GET /update` | Minimal fallback upload page (works without JavaScript). |
| `POST /update[?md5=<hex>]` | Multipart firmware upload (field `update`). `200` → reboot. `400` bad md5 / not an ESP32 image / no file, `409` engine above 1500 RPM or an online update running, `500` flash error, `403` cross-site. |
| `GET /api/ota/status` | `{"state":"idle\|connecting\|checking\|available\|uptodate\|downloading\|flashing\|done\|error","progress","msg","latest","latestCode","current","staIp"}` |
| `POST /api/ota/config` | urlencoded, every field optional: `ssid`, `pass`, `repo`, `branch`, `autoCheck` (1/0) |
| `POST /api/ota/check` / `install` / `cancel` | ESP-direct online update in a background task; poll `/api/ota/status`. |

JSON routes answer HTTP 200 (check `ok` / `state`), except `403` for a cross-site request:
every state-changing route requires the Host to be an IPv4 literal or a `.local` name, and
an `Origin` (if present) to be `http://` + that same host (DNS-rebinding safe). Requests
without `Origin` (curl, the laptop upload) are allowed. Updates are refused, and a running
online install is aborted, while the engine is above 1500 RPM.

A freshly installed image is kept only after ~10 s of healthy uptime with the AP up (or the
first `/api/info`). If it crashes or reboots before that, the bootloader boots the previous
firmware.

## Update manifest (`version.json` in the repository root)

Generated by `scripts/post_build.py` next to `firmware.bin`:

```json
{ "version": "2.0.0", "code": 20000, "size": 1072672, "md5": "…", "built": "2026-09-24T12:00:00Z" }
```

`code` is compared against `FW_VERSION_CODE`: an update is offered only when it is higher.
raw.githubusercontent.com caches files for ~5 minutes, so a fresh push can take that long to
appear. A size or MD5 mismatch is rejected, and the old firmware keeps running.

## Data logger (`swift_show_tuning/data_logger.h/.cpp`, v2.1)

The ESP records by itself (no phone needed while driving) into the otherwise unused `spiffs`
data partition (0x290000, 1,441,792 bytes) as raw sectors, without a file system. The records
survive ignition-off.

* **Samples:** 25 Hz (every 40 ms), 12 bytes each, from a FreeRTOS task. RAM ring of the
  last 120 s.
* **Drive log:** every sample is appended to the flash drive region (~58 min circular). One
  drive = one boot (`boot` = boot counter in NVS). If the flash cannot be written (see the
  write policy), the drive log gets a gap; the RAM ring keeps running.
* **Captures (events):** automatic windows with a per-ignition trace: `launch` (ARMED … end
  + 3 s), `redline` (first redline cut after ≥ 10 s without one, 3 s before / 3 s after),
  `flood` (anti-flood trip), `anomaly` (≥ 3 slot-clock drops within 2 s), `manual`
  (`POST /api/log/snap`: last 60 s).
* **Write policy (real-time safety):** every flash erase/program stops both CPUs and masks the
  engine ISRs for its duration (page program ~1 ms, sector erase ~45 ms, worst case ~400 ms).
  Page programs therefore run only while nothing is cutting or about to cut (no cut for
  200 ms, launch OFF, show button and switch inactive, no OTA). Sector erases run only with
  the engine stopped or at a stable idle. A pool of pre-erased sectors lets the drive log
  continue through several minutes without idle. Reads use `esp_partition_mmap` (through the
  cache), which does NOT mask the ISRs.

### Sample columns (CSV)

`t_ms,rpm,cut,est,show,switch,armed,inhibit,fresh,launch,reason,cut_slots,fired_slots,meas_age_ms`

| Column | Meaning |
| --- | --- |
| `t_ms` | ms since boot |
| `rpm` | RPM (model estimate when `est`=1) |
| `cut`,`est`,`show`,`switch`,`armed`,`inhibit`,`fresh` | 0/1 flags: clamp cutting, estimated RPM, show button, GPIO 23 switch, master arm, engine inhibited (OTA), a fresh tach measurement since the previous sample |
| `launch` | 0 OFF, 1 ARMED, 2 HOLDING, 3 FIRED |
| `reason` | `CutReason` (see above) |
| `cut_slots`,`fired_slots` | ignition slots suppressed / fired since the previous sample |
| `meas_age_ms` | age of the last real RPM measurement |

Capture CSVs also contain a trace section after a `# trace` line:
`t_us,kind,period_us,info` (`kind` = `TraceKind` in engine_control.h: 0 pulse, 1 cut slot,
2 outlier, 3 rejected edge, 4 clamp on, 5 clamp off, 6 unsync, 7 state change).
Every CSV starts with `# key=value` comment lines: `fw`, `type`, `id`, `boot`, `t0_ms`,
`trigger_ms`, the config snapshot (`launchRpm`, `launchDrop`, `redlineRpm`, `cutPattern`,
`maxCutSeconds`, `decelPops`, `decelRpm`, `ghostCam`, `armed`), `EngineDiag` counters at the
trigger, and the capture summary fields.

### HTTP API

State-changing POSTs follow the same Host/Origin rule as the OTA routes.

| Route | Meaning |
| --- | --- |
| `GET /api/log/status` | `{"ok":true,"flash":true,"capacity":1441792,"captures":7,"drives":3,"pending":0,"dropped":0,"poolFree":{"cap":20,"drive":32},"writing":true,"gapNow":false,"heap":123456,"boot":12,"ringSec":120}` |
| `GET /api/log/list` | `{"captures":[{"id":57,"boot":12,"type":"launch","t0":123456,"dur":8200,"n":205,"trace":612,"sum":"<short Hungarian summary>"}],"drives":[{"boot":12,"t0":0,"dur":1834000,"n":45850,"gaps":1}]}`, newest first. `type` ∈ `launch`,`redline`,`flood`,`anomaly`,`manual`. |
| `GET /api/log/capture.csv?id=<id>` | Capture as CSV (attachment `swift_<type>_<id>.csv`) |
| `GET /api/log/drive.csv?boot=<n>[&from=<ms>&to=<ms>][&step=<k>]` | Drive log as CSV, optionally a time range and every k-th sample (attachment `swift_drive_<boot>.csv`) |
| `GET /api/log/live.csv?sec=<1-120>` | Last N seconds from the RAM ring (no flash involved) |
| `POST /api/log/snap` | Manual capture of the last 60 s → `{"ok":true,"id":58}` (saved when writing is safe) |
| `POST /api/log/clear` | Forget all captures and drives (instant; sectors are re-erased lazily) → `{"ok":true}` |
