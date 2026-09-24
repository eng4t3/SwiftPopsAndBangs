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

### Profiles (v2.2)

Three setting profiles are stored on the ESP (NVS). The active profile IS the current config:
changing a setting edits the active profile in RAM, and `SAVE_FLASH` writes it into the active
slot. Switching profiles loads the other slot's saved values; unsaved changes are discarded (the
dashboard asks first). The master switch `armed` is global and not part of any profile.

| Slot | Factory name | Factory values (all other keys = config defaults) |
| --- | --- | --- |
| 0 | `UTCA` | `decelPops` false, `ghostCam` false, `cutPattern` 0 |
| 1 | `SHOW` | `decelPops` true, `ghostCam` true, `cutPattern` 1 |
| 2 | `RAJT` | `launchRpm` 3500, `decelPops` false, `ghostCam` false, `cutPattern` 0 |

Config defaults for the other keys: `launchRpm` 3800, `launchDrop` 400, `redlineRpm` 6200,
`decelRpm` 3200, `maxCutSeconds` 3.0.

Names: 1–12 characters (UTF-8, Hungarian accents allowed, max 24 bytes). The firmware drops
control characters, `"` and `\`, and trims surrounding spaces; an empty result keeps the old name.

Migration from 2.0/2.1 (no profile data in NVS yet): the previously saved config goes into
slot 1 (`SHOW`), which becomes active; slots 0 and 2 get their factory values.

Protocol additions:

| Message | Direction | Meaning |
| --- | --- | --- |
| `CFG:{…,"profile":1,"profiles":["UTCA","SHOW","RAJT"]}` | → dashboard | Active slot and all names, in every `CFG:` |
| `PROFILE:<n>` | → firmware | Load slot `n` (0–2), apply it at once, remember it as active (persisted with the NVS no-cut rule). Replies with `CFG:` then `ACK:PROFILE`. |
| `SAVE_FLASH` | → firmware | Writes the current config (except `armed`) into the ACTIVE slot, and `armed` globally. Replies `ACK:SAVED`. |
| `PROFILE_RESET` | → firmware | Active slot back to its factory values: applied and saved. Replies with `CFG:` then `ACK:RESET`. |
| `PROFILE_NAME:<n>:<name>` | → firmware | Rename slot `n` (saved). Replies with `CFG:`. |
| `ERR:<Hungarian text>` | → dashboard | A command was refused (bad slot, bad name, …). |

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
survive ignition-off. The binary format is documented at the top of `data_logger.cpp` and
decoded by `tools/log/swiftlog.py` (`info` / `validate` / `capture` / `drive` / `csvcheck` of a
dump read with `esptool.py read_flash 0x290000 0x160000 swiftlog.bin`); `tools/log/test_swiftlog.py`
checks the decoder against the C++ source and this section.

* **Samples:** 25 Hz (every 40 ms), 12 bytes each, from a FreeRTOS task (core 1, priority 2).
  RAM ring of the last 120 s; shortened only if the free heap would otherwise drop below 90 KB
  after boot (`ringSec` in the status). The whole logger uses ≤ 64 KB of RAM.
* **Drive log:** every sample is appended to the flash drive region (1 MB: ~56 min raw, ~49 min
  kept, because 32 sectors are always pre-erased). One drive = one boot (`boot` = boot counter
  in NVS). Samples wait in the RAM ring while writing is not allowed (see the write policy); if
  they would be overwritten there, or no pre-erased sector is left, the drive gets a gap
  (`# gap` line in the CSV, `gaps` in the list). A gap also marks logging switched off and on
  again within the same boot (see `enabled`).
* **Captures (events), capture region 384 KB:** automatic windows with a per-ignition trace:
  * `launch`: ARMED … 3 s after the launch returns to OFF (max 60 s). A new launch always
    starts its own capture. The trace restarts when HOLDING begins.
  * `redline`: first redline cut after ≥ 10 s without one; from 3 s before it until 3 s after
    the last redline cut of that episode (max 20 s).
  * `flood`: anti-flood trip; from `maxCutSeconds` + 2 s before (max 8 s) to 3 s after.
  * `anomaly`: ≥ 3 slot-clock drops within 2 s at ≥ 1200 RPM (drops right after the logger's
    own flash writes or during OTA are ignored), at most one per 30 s; 3 s before / 3 s after.
  * `manual`: `POST /api/log/snap`, the last 60 s.
  A trigger while another window is open is folded into it (`# also=` in the CSV, `+redline` in
  the summary). The trace holds the first 512 and the last 512 engine events of the window
  (`trace_lost` = events in between). When the window ends the capture is pending (at most 2;
  more are counted in `dropped`): config snapshot, `EngineDiag` at the trigger, trace and the
  Hungarian summary are kept in RAM, the samples stay in the RAM ring and are written from there
  when writing is safe. A pending capture whose samples would leave the ring first is dropped
  (and counted); the slack is 60 s for `manual` and ~95-110 s for the others.
* **Switch (`enabled`, default on, NVS `swlog/en`):** off = no flash program or erase at all,
  except the NVS write of the setting itself and of a clear. Switching off discards an open
  capture window and the pending captures (not counted in `dropped`) and ends the current drive
  at its last written record (no gap). No automatic captures, `snap` is refused. Still working:
  the RAM ring and `live.csv`, the lists and all downloads, `clear`. Switching on again
  continues the same boot's drive in a new sector with one gap covering the pause, and resumes
  captures. Changes apply at once; the NVS write waits for the erase rule and stays pending
  until done (a reboot before that returns to the previous setting).
* **Write policy (real-time safety):** every flash erase/program stops both CPUs and masks the
  engine ISRs for its duration (page program ~1 ms, sector erase ~45 ms, worst case ~400 ms).
  * Page programs (≤ 256 B, one record each, ≥ 10 ms apart, ≤ 2 per 40 ms tick) only while
    nothing is cutting or about to cut: no cut now or in the last 200 ms, launch OFF, show
    button and switch inactive, RPM more than 600 below the redline, no OTA. Re-checked right
    before every program.
  * Sector erases (≤ 1 per 250 ms) additionally only with the engine stopped (0 RPM, not
    estimated, ≥ 1 s) or at a stable idle (< 1400 RPM for ≥ 2 s, no cut for ≥ 1 s, ghost cam
    off). Pre-erased pools ahead of both logs (drive 32 sectors ≈ 7 min, capture 16) cover the
    time in between; an empty drive pool stops the drive log (gap) instead of erasing.
  * The logger's NVS writes (boot counter, capture-id counter, clear epoch, `enabled`) follow
    the erase rule.
  * Nothing is written while an update runs. Reads go through `spi_flash_mmap` (64 KB MMU
    windows through the cache), which does NOT mask the ISRs; `esp_partition_read` is never used.

### Sample columns (CSV)

`t_ms,rpm,cut,est,show,switch,armed,inhibit,fresh,launch,reason,cut_slots,fired_slots,meas_age_ms`

| Column | Meaning |
| --- | --- |
| `t_ms` | ms since boot |
| `rpm` | RPM (model estimate when `est`=1) |
| `cut`,`est`,`show`,`switch`,`armed`,`inhibit`,`fresh` | 0/1 flags: clamp cutting, estimated RPM, show button, GPIO 23 switch, master arm, engine inhibited (OTA), a fresh tach measurement since the previous sample |
| `launch` | 0 OFF, 1 ARMED, 2 HOLDING, 3 FIRED |
| `reason` | `CutReason` (see above) |
| `cut_slots`,`fired_slots` | ignition slots suppressed / fired since the previous sample (saturate at 255) |
| `meas_age_ms` | age of the last real RPM measurement |

Capture CSVs also contain a trace section after a `# trace` line (also when it is empty):
`t_us,kind,period_us,info` (`t_us` = µs since boot, unwrapped from the engine's 32-bit
timestamps; `kind` = `TraceKind` in engine_control.h: 0 pulse, 1 cut slot, 2 outlier,
3 rejected edge, 4 clamp on, 5 clamp off, 6 unsync, 7 state change).

Every CSV starts with `# key=value` comment lines, in this order:

* capture: `fw`, `type`, `id`, `boot`, `t0_ms`, `trigger_ms`; the config snapshot `launchRpm`,
  `launchDrop`, `redlineRpm`, `cutPattern`, `maxCutSeconds`, `decelPops`, `decelRpm`,
  `ghostCam`, `armed` (booleans 0/1); `EngineDiag` at the trigger `pulses`, `rejected`,
  `discarded`, `outliers`, `unsyncs`, `cutSlots`, `floodTrips`, `dwellBlocks`,
  `launchDropRate`; `dur_ms`, `n`, `trace`, `trace_lost`, `also`; the type's summary fields;
  `sum`. Summary fields: launch `end` (`fired`/`lift`/`hold_timeout`/`arm_timeout`/`cancel`/
  `stall`/`none`), `hold_min`, `hold_max`, `release_ms` (-1 = no release), `hold_ms`,
  `peak_rpm`, `drop_rate`; redline `limiter_ms`, `redline_cut_slots`, `peak_rpm`; flood
  `cut_reason`, `full_cut_ms`, `peak_rpm`; anomaly `window_unsyncs`, `trace_rejects`,
  `trace_outliers`, `trigger_rpm`, `peak_rpm`; manual `sec`, `cut_slots_total`,
  `fired_slots_total`, `peak_rpm`. A capture that cannot be read completely ends with
  `# error=<reason>`.
* drive: `fw`, `type=drive`, `boot`, `t0_ms`, `dur_ms`, `n`, `gaps`, then `from_ms`, `to_ms`,
  `step` if a range or step was requested, then the config at the start of the drive. Inside
  the data: `# gap from_ms=<first lost> to_ms=<last lost> lost=<samples>` and, when the config
  changed during the drive, `# config launchRpm=… armed=…`.
* live: `fw`, `type=live`, `boot`, `t0_ms`, `sec`, `ringSec`, the current config and
  `EngineDiag`; `# gap lost=<n>` if the client was too slow for the ring.

### HTTP API

State-changing POSTs follow the same Host/Origin rule as the OTA routes (`403` otherwise).
JSON answers are HTTP 200 (check `ok`), except `400` for a missing parameter, `404` for an
unknown capture / drive and `503` when two downloads are already running.

| Route | Meaning |
| --- | --- |
| `GET /api/log/status` | `{"ok":true,"enabled":true,"flash":true,"capacity":1441792,"captures":7,"drives":3,"pending":0,"dropped":0,"poolFree":{"cap":16,"drive":32},"writing":true,"gapNow":false,"heap":123456,"boot":12,"ringSec":120,"backlog":3,"ramBytes":64728,"stackFree":1500,"flashErrors":0,"scanErrors":0}`. `ok` false = logger not running (no RAM). `writing` = flash writes are allowed right now (on, flash usable, write policy satisfied); `gapNow` = the drive log is currently losing samples (no pre-erased sector, or samples lost in the last 5 s); `backlog` = samples waiting in the RAM ring. |
| `GET /api/log/list` | `{"ok":true,"captures":[{"id":57,"boot":12,"type":"launch","t0":123456,"dur":8200,"n":205,"trace":612,"sum":"FIRED, tartás 3752–3880 RPM, kioldás 118 ms"}],"drives":[{"boot":12,"t0":0,"dur":1834000,"n":45850,"gaps":1}]}`, newest first (at most 32 captures and 24 drives). `type` ∈ `launch`,`redline`,`flood`,`anomaly`,`manual`. |
| `GET /api/log/capture.csv?id=<id>` | Capture as CSV (attachment `swift_<type>_<id>.csv`) |
| `GET /api/log/drive.csv?boot=<n>[&from=<ms>&to=<ms>][&step=<k>]` | Drive log as CSV, optionally a time range and every k-th sample, k ≤ 1000 (attachment `swift_drive_<boot>.csv`) |
| `GET /api/log/live.csv?sec=<1-120>` | Last N seconds (default 30) from the RAM ring, no flash involved (attachment `swift_live.csv`) |
| `POST /api/log/snap` | Manual capture of the last 60 s → `{"ok":true,"id":58}` (saved when writing is safe; it appears in the list once written). `{"ok":false,"msg":"A naplózás ki van kapcsolva"}` while switched off. |
| `POST /api/log/clear` | Forget all captures and drives → `{"ok":true}`. Instant in RAM; the clear epoch reaches NVS at the next erase-safe moment (also while switched off), sectors are re-erased lazily. |
| `POST /api/log/config` | x-www-form-urlencoded `enabled=1` or `enabled=0` → `{"ok":true,"enabled":false}`; `{"ok":false,"msg":"…"}` for a missing / invalid value. Applies at once, persisted as described under *Switch*. |
