// =========================================================================================
// DATA LOGGER - see data_logger.h and docs/PROTOCOL.md ("Data logger")
// =========================================================================================
// Overview
//   * A FreeRTOS task (core 1, priority 2, vTaskDelayUntil) takes a 12-byte sample every 40 ms
//     from engineGetTelemetry() / engineGetSlotCounters() / otaIsBusy() / engineGetConfig()
//     into a RAM ring of 120 s (reduced only if the free heap would drop below 90 KB).
//   * The same task appends every sample to the flash DRIVE log and writes event CAPTURES
//     (launch / redline / flood / anomaly / manual, with the engine's per-ignition trace), both
//     only when the write policy below allows it. Unwritten samples wait in the ring.
//   * loggerLoop() (loop task) streams CSV / JSON downloads with non-blocking socket writes.
//
// Flash layout: the `spiffs` data partition (0x290000, 1,441,792 bytes in default.csv), raw,
// no file system. Nothing outside it is ever written or erased (esp_partition_* API only).
//   sectors [0, CAP)   capture region, circular log of 4 KB sectors (CAP = 96 = 384 KB)
//   sectors [CAP, N)   drive region, circular; one boot = one drive; a drive owns its sectors
//   Sector header, 20 bytes, little-endian:
//     u32 magic 0x474C5753 ("SWLG") | u8 format version (1) | u8 region (0 capture, 1 drive)
//     | u16 reserved (0) | u32 seq (global, +1 per opened sector) | u32 boot id | u32 CRC-32
//     of the first 16 bytes
//   Records from offset 20: u8 type | u8 payload length (multiple of 4, <= 252) | u16 CRC-16
//   (= low 16 bits of the CRC-32 over type, length and payload) | payload. A record never
//   crosses a 256-byte page, so each record is exactly one page program; the rest of a page
//   that cannot hold the next record stays 0xFF. 0xFF at the first record position of a page
//   = end of the written data. A bad CRC skips to the next page (torn write).
//     0x01 DRV_INFO     u32 boot, char fw[12], CfgSnap (first record of every drive sector),
//                       v2.2+: + NameField (profile name)
//     0x02 DRV_SAMPLES  1..21 x LogSample (12 B)
//     0x03 DRV_GAP      u32 from_ms (first lost sample), u32 to_ms (last lost), u32 lost
//     0x10 CAP_BEGIN    CapMeta (120 B) + summary text (UTF-8, padded to 4), v2.2+: + NameField
//                       CfgSnap byte 12 = active profile + 1 (0 = unknown / older firmware);
//                       NameField = u8 length + 24 bytes UTF-8 + 3 pad (absent in 2.1 records)
//     0x11 CAP_SAMPLES  u32 capture id + 1..20 x LogSample
//     0x12 CAP_TRACE    u32 capture id + 1..31 x EngineTraceEvent (8 B)
//     0x13 CAP_END      u32 id, u16 samples, u16 trace events, u32 CRC-32 of all sample and
//                       trace bytes in record order (commit marker: no END = no capture)
//   Boot scan: sector headers give seq / heads / erased sectors, records give the capture and
//   drive index. Boot id = max(NVS counter, highest boot id in flash) + 1. "Clear" stores a
//   clear-epoch seq in NVS: sectors below it are ignored and re-erased lazily.
//   CRC-32 = zlib / IEEE 802.3 (reflected 0xEDB88320), own nibble-table code checked against
//   the standard check value at compile time.
//
// WRITE POLICY (real-time safety). Every flash erase / program disables the flash cache: both
// CPUs stall and the engine ISRs (allocated without ESP_INTR_FLAG_IRAM) are masked for its
// duration (page program ~1 ms, sector erase ~45 ms, worst case ~400 ms).
//   * Page program (<= 256 B, >= 10 ms apart, <= 2 per 40 ms tick): only if !otaIsBusy(), no
//     cut active and none in the last 200 ms, launch OFF, show button and switch inactive. The
//     conditions are re-read from the engine right before every program.
//   * Sector erase (<= 1 per 250 ms): additionally only with the engine stopped (rpm 0, not
//     estimated, for >= 1 s) or at a stable idle (rpm < 1400 for >= 2 s, no cut for >= 1 s,
//     ghost cam off). Pools of pre-erased sectors ahead of both heads (drive 32 = ~7 min,
//     capture 16) let the logs run without an erase; an empty drive pool stops the drive log
//     (a gap is recorded) instead of erasing. The logger's own NVS writes (boot counter, clear
//     epoch, capture id) follow the erase rule (NVS may erase a page).
//   * Reads only through spi_flash_mmap (cache reads; the ISRs keep running). Never
//     esp_partition_read / spi_flash_read: their "start" guard "disables flash cache and
//     non-IRAM interrupts" around every ROM SPIRead chunk (esp_spi_flash.h, IDF 4.4). The MMU
//     maps 64 KB pages: each user keeps one 64 KB window mapped and remaps on demand (a 4 KB
//     sector never crosses a 64 KB page). Writes / erases flush the cache for their range.
//   * Everything above is skipped while otaIsBusy(); the RAM ring lock is a spinlock held for
//     one 12-byte copy at a time.
//
// Captures: the window (launch ARMED ... OFF + 3 s, redline 3 s before the first cut after >= 10
// s without one ... 3 s after the last one (max 20 s), flood (maxCutSeconds + 2 s, max 8 s)
// before ... 3 s after, anomaly 3 s / 3 s, manual last 60 s) drains engineReadTrace() into a
// trace buffer (first 512 + last 512 events). When the window ends the capture becomes
// pending (max 2, more are counted in `dropped`): config snapshot + EngineDiag at the trigger
// + summary are frozen, the trace buffer is handed over; the samples stay in the RAM ring and
// are written from there (a pending capture whose samples are about to be overwritten is
// dropped and counted).
//
// Switch (setting `enabled`, NVS swlog/en, POST /api/log/config): off = no flash program or erase
// at all, except the NVS write of the setting itself and of a clear epoch (both under the erase
// rule; a pending value is kept until written). Switching off drops the open window and pending
// captures and ends the drive where the last record was written (no gap record); switching on
// again continues the same boot's drive in a new sector with a DRV_GAP record covering the pause
// (lost = samples not logged). The RAM ring, live.csv, lists, downloads and clear keep working.
//
// Isolation: the logger only READS engine state (short critical sections inside the engine
// API). Allocation, partition or flash failures degrade the logger, never the engine.
// =========================================================================================
#include "data_logger.h"
#include <Arduino.h>
#include <Preferences.h>
#include <esp_partition.h>
#include <esp_spi_flash.h>
#include <esp_heap_caps.h>
#include <lwip/sockets.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include "engine_control.h"
#include "ota_update.h"
#include "version.h"
#include "profiles.h"

// ---- Tunables -----------------------------------------------------------------------------
#define LOG_SAMPLE_MS           40      // 25 Hz
#define LOG_RING_SEC            120
#define LOG_RING_MIN_SEC        30
#define LOG_HEAP_KEEP           (90 * 1024)   // free heap to keep after boot (OTA TLS needs ~50 KB)
#define LOG_RAM_BUDGET          (64 * 1024)   // everything the logger owns, incl. its task stack
#define LOG_TASK_STACK          4608
#define LOG_TASK_PRIO           2
#define LOG_TASK_CORE           1

#define LOG_CAP_SECTORS         96      // capture region (384 KB); the rest is the drive region
#define LOG_DRV_POOL            32      // pre-erased sectors ahead of the drive head (~7 min)
#define LOG_CAP_POOL            16      // ... ahead of the capture head (64 KB)

#define LOG_PROG_NOCUT_MS       200
#define LOG_PROG_REDLINE_MARGIN 600   // no flash programs within this many RPM below the redline
#define LOG_PROG_GAP_MS         10
#define LOG_PROG_PER_TICK       2
#define LOG_ERASE_GAP_MS        250
#define LOG_ERASE_STOP_MS       1000
#define LOG_ERASE_IDLE_RPM      1400
#define LOG_ERASE_IDLE_MS       2000
#define LOG_ERASE_IDLE_NOCUT_MS 1000
#define LOG_DRV_FLUSH_MS        2000    // a partial drive record is written once its oldest sample is this old
#define LOG_LOSS_MARGIN         25      // samples of distance kept to the ring's write position
#define LOG_NVS_CAPID_MS        60000   // capture-id counter persisted at most this often
#define LOG_MAX_FLASH_ERRORS    16      // then flash writing is switched off (RAM-only)

#define LOG_TRACE_HALF          512     // trace buffer = first 512 + last 512 events of a window
#define LOG_TRACE_BUFS          2
#define LOG_MAX_PENDING         2
#define LOG_CAP_INDEX           32
#define LOG_DRV_INDEX           24

#define CAP_TAIL_MS             3000
#define CAP_MAX_MS              60000
#define CAP_REDLINE_QUIET_MS    10000
#define CAP_REDLINE_PRE_MS      3000
#define CAP_REDLINE_MAX_MS      20000
#define CAP_FLOOD_PRE_EXTRA_MS  2000
#define CAP_FLOOD_PRE_MAX_MS    8000
#define CAP_ANOM_COUNT          3
#define CAP_ANOM_WINDOW_MS      2000
#define CAP_ANOM_PRE_MS         3000
#define CAP_ANOM_REFRACT_MS     30000
#define CAP_ANOM_MIN_RPM        1200    // slot-clock drops while cranking / stopping are normal
#define CAP_MANUAL_MS           60000
#define CAP_HOLD_SETTLE_MS      300     // launch hold range ignores the first 300 ms (overshoot)

#define LOG_STREAMS             2
#define LOG_STREAM_CHUNK        800
#define LOG_STREAM_STALL_MS     15000

// ---- Binary format (mirrored in tools/log/swiftlog.py) ----------------------------------
#define LOG_MAGIC               0x474C5753u   // "SWLG"
#define LOG_FORMAT_VERSION      1
#define LOG_SECTOR_SIZE         4096
#define LOG_PAGE_SIZE           256
#define LOG_SECTOR_HDR          20
#define LOG_REC_HDR             4
#define LOG_REC_MAX_PAYLOAD     252
#define LOG_REGION_CAP          0
#define LOG_REGION_DRIVE        1
#define DRV_SAMPLES_PER_REC     21
#define CAP_SAMPLES_PER_REC     20
#define CAP_TRACE_PER_REC       31

enum : uint8_t {
  REC_DRV_INFO    = 0x01,
  REC_DRV_SAMPLES = 0x02,
  REC_DRV_GAP     = 0x03,
  REC_CAP_BEGIN   = 0x10,
  REC_CAP_SAMPLES = 0x11,
  REC_CAP_TRACE   = 0x12,
  REC_CAP_END     = 0x13
};

enum CapType : uint8_t { CT_LAUNCH = 0, CT_REDLINE = 1, CT_FLOOD = 2, CT_ANOMALY = 3, CT_MANUAL = 4, CT_COUNT = 5 };
static const char* const CAP_TYPE_NAME[CT_COUNT] = {"launch", "redline", "flood", "anomaly", "manual"};

// Sample flag bits (LogSample::flags)
enum : uint8_t { SF_CUT = 1, SF_EST = 2, SF_SHOW = 4, SF_SWITCH = 8, SF_ARMED = 16, SF_INHIBIT = 32, SF_FRESH = 64 };

struct LogSample {         // 12 bytes, also the on-flash sample format
  uint32_t tMs;            // ms since boot
  uint16_t rpm;
  uint8_t  flags;          // SF_*
  uint8_t  state;          // bits 0-1 launch state, bits 2-5 cut reason
  uint8_t  cutSlots;       // ignition slots suppressed since the previous sample (saturating)
  uint8_t  firedSlots;     // ignition slots fired since the previous sample (saturating)
  uint16_t measAgeMs;      // age of the last real RPM measurement
};
static_assert(sizeof(LogSample) == 12, "12-byte sample");
static_assert(sizeof(EngineTraceEvent) == 8, "8-byte trace event");

struct SectorHdr {
  uint32_t magic;
  uint8_t  version;
  uint8_t  region;
  uint16_t reserved;
  uint32_t seq;
  uint32_t boot;
  uint32_t crc;            // CRC-32 of the 16 bytes above
};
static_assert(sizeof(SectorHdr) == LOG_SECTOR_HDR, "sector header");

struct CfgSnap {           // config snapshot, 16 bytes
  uint16_t launchRpm, launchDrop, redlineRpm, decelRpm;
  uint8_t  cutPattern;
  uint8_t  flags;          // bit0 armed, bit1 decelPops, bit2 ghostCam
  uint16_t maxCutCs;       // maxCutSeconds * 100, 0 = unlimited
  uint8_t  profile1;       // active settings profile + 1 (0 = unknown: written before v2.2)
  uint8_t  reserved[3];
};
static_assert(sizeof(CfgSnap) == 16, "config snapshot");

struct DiagSnap {          // EngineDiag at the trigger, 36 bytes
  uint32_t pulses, rejected, discarded, outliers, unsyncs, cutSlots, floodTrips, dwellBlocks;
  int32_t  launchDropRate;
};
static_assert(sizeof(DiagSnap) == 36, "diag snapshot");

struct NameField {         // profile name (v2.2) after CAP_BEGIN's summary and after DrvInfo, 28 bytes
  uint8_t len;             // bytes (older records have no NameField at all)
  char    name[PROFILE_NAME_BYTES];
  uint8_t pad[3];
};
static_assert(sizeof(NameField) == 28, "name field");

#define LOG_SUM_MAX 84    // BEGIN (4 + 120 + 84 + 28 = 236 B) always fits the first page of a sector
struct CapMeta {           // CAP_BEGIN payload head, 120 bytes (+ sum text)
  uint32_t id;
  uint32_t boot;
  uint8_t  type;           // CapType
  uint8_t  flags;          // other triggers folded into the window: bit (1 << CapType)
  uint8_t  launchEnd;      // LaunchEnd (launch captures)
  uint8_t  sumLen;         // bytes of summary text after this struct
  uint32_t t0Ms;           // first sample
  uint32_t triggerMs;
  uint32_t durMs;          // last sample - first sample
  uint16_t nSamples;
  uint16_t nTrace;
  uint32_t traceLost;      // events not kept (between the first and the last 512, or lost by the engine ring)
  CfgSnap  cfg;
  DiagSnap diag;
  int32_t  sv[6];          // type-specific summary values (see summaryKeys())
  char     fw[12];
};
static_assert(sizeof(CapMeta) == 120, "capture meta");
static_assert(LOG_REC_HDR + sizeof(CapMeta) + LOG_SUM_MAX + sizeof(NameField) == LOG_PAGE_SIZE - LOG_SECTOR_HDR,
              "BEGIN fits the first page of a sector (no page is ever skipped empty)");

struct DrvInfo { uint32_t boot; char fw[12]; CfgSnap cfg; };
static_assert(sizeof(DrvInfo) == 32, "drive info");
struct DrvGap { uint32_t fromMs, toMs, lost; };
struct CapEnd { uint32_t id; uint16_t nSamples, nTrace; uint32_t crc; };
static_assert(sizeof(CapEnd) == 12, "capture end");

// ---- CRC-32 (zlib / IEEE 802.3, reflected 0xEDB88320), nibble table ------------------------
static constexpr uint32_t kCrcNib[16] = {
  0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
  0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu, 0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu};
static constexpr uint32_t ceNib(uint32_t c) { return (c >> 4) ^ kCrcNib[c & 15u]; }
static constexpr uint32_t ceCrc(uint32_t c, const char* s, size_t n) {
  return n ? ceCrc(ceNib(ceNib(c ^ (uint8_t)*s)), s + 1, n - 1) : c;
}
static_assert(~ceCrc(0xFFFFFFFFu, "123456789", 9) == 0xCBF43926u, "CRC-32 check value");

// Same algorithm as ceCrc(); chaining: crc32Update(crc32Update(0, a), b) == zlib.crc32(a + b).
static uint32_t crc32Update(uint32_t crc, const uint8_t* p, size_t n) {
  crc = ~crc;
  while (n--) {
    crc ^= *p++;
    crc = (crc >> 4) ^ kCrcNib[crc & 15u];
    crc = (crc >> 4) ^ kCrcNib[crc & 15u];
  }
  return ~crc;
}

static uint16_t recCrc(uint8_t type, uint8_t plen, const uint8_t* payload) {
  uint8_t h[2] = {type, plen};
  return (uint16_t)(crc32Update(crc32Update(0, h, 2), payload, plen) & 0xFFFFu);
}

// ---- Sector table / regions -----------------------------------------------------------------
#define SEQ_ERASED   0xFFFFFFFFu
#define SEQ_DIRTY    0xFFFFFFFEu   // not erased and no valid header (needs an erase before use)
#define SEQ_ERASING  0xFFFFFFFDu
#define SEQ_LIMIT    0xFFFFFF00u
static inline bool seqValid(uint32_t q) { return q != 0 && q < SEQ_LIMIT; }

struct Region {
  uint16_t first, count;   // absolute sector numbers
  int16_t  head;           // sector written last (-1: none)
  uint16_t writeOff;       // next record offset in head (LOG_SECTOR_SIZE = closed / full)
  uint16_t poolTarget;
};

// Capture / drive index (RAM, rebuilt by the boot scan)
struct CapIdx {
  uint32_t id, boot, t0, dur, beginSeq, endSeq;
  uint16_t n, nTrace, beginSec, beginOff;
  uint8_t  type, flags, pad[2];
};
struct DrvIdx {
  uint32_t boot, firstSeq, lastSeq, t0, tEnd, n;
  uint16_t firstSec, gaps;
};
#define DRV_T0_NONE 0xFFFFFFFFu

// ---- Trace buffers ----------------------------------------------------------------------------
struct TraceBuf {
  bool     used;
  uint16_t headN, tailN, tailPos;
  uint32_t lost;
  EngineTraceEvent ev[2 * LOG_TRACE_HALF];   // [0, 512) first events, [512, 1024) ring of the last
};

// ---- Capture window / pending captures ---------------------------------------------------------
struct Window {
  bool     open;
  bool     holdingSeen;
  uint8_t  type, flags, launchEnd, reasonBefore;
  int8_t   tb;              // trace buffer, -1 = none (yet)
  uint32_t startSeq, startMs, triggerMs, endMs;   // endMs 0 = open-ended
  uint32_t traceSeq, unsyncs, trigRpm, fullCutMs;
  CfgSnap  cfg;
  DiagSnap diag;
  char     profName[PROFILE_NAME_BYTES + 1];   // active profile at the trigger
};

enum : uint8_t { PH_BEGIN = 0, PH_SAMPLES, PH_TRACE, PH_END };
struct Pending {
  CapMeta  meta;
  char     sum[LOG_SUM_MAX];
  char     profName[PROFILE_NAME_BYTES + 1];
  uint32_t seq0;            // first sample (RAM ring sequence number)
  int8_t   tb;
  uint8_t  phase;
  uint16_t done;
  uint32_t crc;
  uint16_t beginSec, beginOff;
  uint32_t beginSeq;
};

// ---- Flash windows (spi_flash_mmap) -------------------------------------------------------------
struct FlashWin {
  spi_flash_mmap_handle_t h;
  const uint8_t* base;
  uint32_t start;           // physical address of the mapped 64 KB page
  bool mapped;
};

// ---- Download streams ---------------------------------------------------------------------------
enum StreamKind : uint8_t { SK_FREE = 0, SK_LIST, SK_CAPTURE, SK_DRIVE, SK_LIVE };
struct LogStream {
  WiFiClient client;
  uint8_t  kind = SK_FREE;
  bool     done = false;
  uint32_t lastProgressMs = 0;
  char     out[LOG_STREAM_CHUNK + 16];
  uint16_t outLen = 0, outPos = 0;
  FlashWin win = {};
  uint8_t  phase = 0;
  uint16_t line = 0;
  bool     first = true;
  uint8_t  pendHdr = 0;     // capture: "# trace" lines still to emit
  const char* err = nullptr;  // capture: error line still to emit
  int16_t  sec = -1;
  uint16_t off = 0;
  uint32_t seq = 0;
  uint8_t  rec[LOG_PAGE_SIZE];
  uint8_t  recType = 0;
  uint16_t recItem = 0, recItems = 0;
  // capture
  CapMeta  meta;
  char     sum[LOG_SUM_MAX + 1];
  char     profName[PROFILE_NAME_BYTES + 1] = "";   // capture: at the trigger; drive: current DRV_INFO
  uint16_t gotN = 0, gotNT = 0;
  // drive
  DrvIdx   drv;
  DrvInfo  info;
  bool     haveInfo = false;
  uint32_t fromMs = 0, toMs = 0xFFFFFFFFu, idx = 0;
  uint16_t step = 1;
  bool     rangeGiven = false;
  // live / list
  uint32_t next = 0, end = 0, cursor = 0;
};

// =========================================================================================
// STATE
// =========================================================================================
static WebServer* g_srv = nullptr;
static SemaphoreHandle_t g_mtx = nullptr;          // flash + index structures
static portMUX_TYPE g_ringMux = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE g_idMux = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t g_task = nullptr;

// RAM ring
static LogSample* g_ring = nullptr;
static uint32_t g_ringCap = 0;
static volatile uint32_t g_head = 0;               // samples taken = next sequence number

// Flash
static const esp_partition_t* g_part = nullptr;
static bool g_flashOk = false;
static uint32_t g_nSec = 0, g_capSec = 0;
static uint32_t* g_secSeq = nullptr;
static Region g_rg[2];
static uint32_t g_nextSeq = 1, g_clearSeq = 0, g_boot = 0, g_nextCapId = 1;
static uint32_t g_flashErrors = 0, g_scanErrors = 0, g_scanMs = 0;
static FlashWin g_wWin = {};                       // sampler task
static uint8_t g_wRec[LOG_PAGE_SIZE];              // sampler task record buffer
static uint32_t g_lastProgMs = 0, g_lastEraseMs = 0;
static bool g_flashOpSinceSample = false;
static bool g_nvsBootDirty = false, g_nvsClrDirty = false, g_nvsEnDirty = false, g_capIdDirty = false;
static volatile bool g_enabled = true;             // setting: changed by the HTTP handler under g_mtx
static bool g_enApplied = true;                    // state the sampler task has switched to
static bool g_pauseActive = false;                 // the drive was logging when switched off
static uint32_t g_pauseSeq = 0, g_pauseFromMs = 0; // first sample not logged
static uint32_t g_nvsLastMs = 0;

// Index
static CapIdx* g_capIdx = nullptr;
static uint16_t g_capN = 0;
static DrvIdx* g_drvIdx = nullptr;
static uint16_t g_drvN = 0;

// Trace buffers, window, pending captures
static TraceBuf* g_tb[LOG_TRACE_BUFS] = {nullptr, nullptr};
#define TRACE_TMP 32
static EngineTraceEvent g_traceTmp[TRACE_TMP];
static Window g_win;
static Pending g_pend[LOG_MAX_PENDING];
static uint8_t g_pendN = 0;
static uint32_t g_dropped = 0;

// Sampler bookkeeping
static bool g_haveSample = false;
static uint32_t g_prevMs = 0, g_prevFlood = 0, g_prevRpm = 0;
static EngineSlotCounters g_prevSc = {0, 0, 0, 0};
static uint8_t g_prevLaunch = LAUNCH_OFF;
static bool g_everCut = false, g_ghost = false;
static uint32_t g_lastCutMs = 0, g_zeroSince = 0, g_idleSince = 0, g_cutRunStart = 0, g_lastCutRunMs = 0;
static uint8_t g_lastCutReason = CUT_NONE;
static bool g_redSeen = false;
static uint32_t g_lastRedMs = 0, g_lastAnomMs = 0;
static bool g_anomSeen = false;
static uint32_t g_unsyncT[CAP_ANOM_COUNT];
static uint8_t g_unsyncN = 0;

// Drive writer
static uint32_t g_drvNext = 0;                     // next sample to write
static bool g_drvOurs = false;                     // head sector was opened by this boot (after the last clear)
static bool g_drvNeedInfo = false;
static bool g_drvBlocked = false;                  // no erased sector for the next record
static bool g_gapPending = false;
static DrvGap g_gap;
static uint32_t g_lastLossMs = 0;
static bool g_lossSeen = false;
static bool g_writeAllowed = false;

// Requests from the HTTP handlers
static volatile uint32_t g_snapReq = 0;
static volatile bool g_clearReq = false;

static LogStream g_streams[LOG_STREAMS];
static uint32_t g_ramBytes = 0;
static uint32_t g_wRecErr = 0;

// =========================================================================================
// SMALL HELPERS
// =========================================================================================
static inline uint8_t sat8(uint32_t v) { return v > 255 ? 255 : (uint8_t)v; }
static inline uint16_t sat16(int32_t v) { return v < 0 ? 0 : (v > 65535 ? 65535 : (uint16_t)v); }
static inline uint32_t pad4(uint32_t v) { return (v + 3u) & ~3u; }

static void copyFw(char* dst) {
  memset(dst, 0, 12);
  strncpy(dst, FW_VERSION, 11);
}

static CfgSnap makeCfg(const TuningConfig& c) {
  CfgSnap s;
  memset(&s, 0, sizeof(s));
  s.launchRpm = sat16(c.launchRpm);
  s.launchDrop = sat16(c.launchDrop);
  s.redlineRpm = sat16(c.redlineRpm);
  s.decelRpm = sat16(c.decelRpm);
  s.cutPattern = (uint8_t)(c.cutPattern < 0 ? 0 : (c.cutPattern > 255 ? 255 : c.cutPattern));
  s.flags = (uint8_t)((c.armed ? 1 : 0) | (c.decelPops ? 2 : 0) | (c.ghostCam ? 4 : 0));
  float m = c.maxCutSeconds;
  s.maxCutCs = (m > 0.0f && m < 600.0f) ? (uint16_t)(m * 100.0f + 0.5f) : 0;
  s.profile1 = (uint8_t)(profileActiveIndex() + 1);
  return s;
}

static NameField makeName(const char* name) {
  NameField f;
  memset(&f, 0, sizeof(f));
  size_t n = strnlen(name, PROFILE_NAME_BYTES);
  memcpy(f.name, name, n);
  f.len = (uint8_t)n;
  return f;
}

// Optional NameField at payload offset `off` (absent in records written before v2.2).
static void readName(const uint8_t* payload, uint32_t plen, uint32_t off, char* out /* NAME_BYTES + 1 */) {
  out[0] = '\0';
  if (off + sizeof(NameField) > plen) return;
  NameField f;
  memcpy(&f, payload + off, sizeof(f));
  size_t n = f.len <= PROFILE_NAME_BYTES ? f.len : 0;
  memcpy(out, f.name, n);
  out[n] = '\0';
}

static DiagSnap makeDiag(const EngineDiag& d) {
  DiagSnap s;
  s.pulses = d.pulses;
  s.rejected = d.rejected;
  s.discarded = d.discarded;
  s.outliers = d.outliers;
  s.unsyncs = d.unsyncs;
  s.cutSlots = d.cutSlots;
  s.floodTrips = d.floodTrips;
  s.dwellBlocks = d.dwellBlocks;
  s.launchDropRate = d.launchDropRate;
  return s;
}

static bool takeMutex(TickType_t ticks) { return g_mtx && xSemaphoreTake(g_mtx, ticks) == pdTRUE; }
static void giveMutex() { xSemaphoreGive(g_mtx); }

// ---- RAM ring ----------------------------------------------------------------------------
static void ringPush(const LogSample& s) {
  portENTER_CRITICAL(&g_ringMux);
  g_ring[g_head % g_ringCap] = s;
  g_head = g_head + 1;
  portEXIT_CRITICAL(&g_ringMux);
}

static bool ringGet(uint32_t seq, LogSample& out) {
  bool ok;
  portENTER_CRITICAL(&g_ringMux);
  uint32_t h = g_head;
  ok = seq < h && (h - seq) <= g_ringCap;
  if (ok) out = g_ring[seq % g_ringCap];
  portEXIT_CRITICAL(&g_ringMux);
  return ok;
}

static uint32_t ringOldest() {
  uint32_t h = g_head;
  return h > g_ringCap ? h - g_ringCap : 0;
}

// ---- Flash window ------------------------------------------------------------------------
// Pointer to [partOff, partOff + len) of the partition through a mapped 64 KB MMU page.
static const uint8_t* winPtr(FlashWin& w, uint32_t partOff, uint32_t len) {
  if (!g_part || len == 0 || partOff >= g_part->size || len > g_part->size - partOff) return nullptr;
  uint32_t phys = g_part->address + partOff;
  uint32_t start = phys & ~(uint32_t)(SPI_FLASH_MMU_PAGE_SIZE - 1);
  if (phys + len > start + SPI_FLASH_MMU_PAGE_SIZE) return nullptr;   // never happens for in-sector ranges
  if (!w.mapped || w.start != start) {
    if (w.mapped) {
      spi_flash_munmap(w.h);
      w.mapped = false;
    }
    const void* p = nullptr;
    if (spi_flash_mmap(start, SPI_FLASH_MMU_PAGE_SIZE, SPI_FLASH_MMAP_DATA, &p, &w.h) != ESP_OK) return nullptr;
    w.base = (const uint8_t*)p;
    w.start = start;
    w.mapped = true;
  }
  return w.base + (phys - start);
}

static void winRelease(FlashWin& w) {
  if (w.mapped) {
    spi_flash_munmap(w.h);
    w.mapped = false;
  }
}

static bool flashRead(FlashWin& w, uint32_t partOff, void* dst, uint32_t len) {
  const uint8_t* p = winPtr(w, partOff, len);
  if (!p) return false;
  memcpy(dst, p, len);
  return true;
}

static bool hdrValid(const SectorHdr& h, uint8_t region) {
  return h.magic == LOG_MAGIC && h.version == LOG_FORMAT_VERSION && h.region == region &&
         seqValid(h.seq) && h.crc == crc32Update(0, (const uint8_t*)&h, 16);
}

static bool rangeBlank(FlashWin& w, uint32_t partOff, uint32_t len) {
  const uint8_t* p = winPtr(w, partOff, len);
  if (!p) return false;
  const uint32_t* q = (const uint32_t*)p;   // partOff and len are multiples of 4
  for (uint32_t i = 0; i < len / 4; i++) {
    if (q[i] != 0xFFFFFFFFu) return false;
  }
  return true;
}

// Reads the record at sector offset *off (or the next one) into rec (header + payload) and
// advances *off. *at = its offset. Returns false at the end of the written data; *off is then
// where the next record would go (LOG_SECTOR_SIZE = sector full).
static bool nextRecord(FlashWin& w, uint32_t sec, uint16_t* off, uint8_t* rec, uint16_t* at, uint32_t* errors) {
  uint32_t o = *off;
  while (o + LOG_REC_HDR <= LOG_SECTOR_SIZE) {
    uint32_t pageStart = o & ~(uint32_t)(LOG_PAGE_SIZE - 1);
    uint32_t pageEnd = pageStart + LOG_PAGE_SIZE;
    uint32_t firstInPage = pageStart ? pageStart : LOG_SECTOR_HDR;
    if (o < firstInPage) o = firstInPage;
    const uint8_t* p = winPtr(w, sec * LOG_SECTOR_SIZE + o, pageEnd - o);
    if (!p) {
      *off = LOG_SECTOR_SIZE;
      return false;
    }
    uint8_t type = p[0];
    if (type == 0xFF) {
      if (o == firstInPage) {       // unwritten page: end of the data ...
        const uint8_t* q = pageEnd < LOG_SECTOR_SIZE ? winPtr(w, sec * LOG_SECTOR_SIZE + pageEnd, 1) : nullptr;
        if (q && *q != 0xFF) {      // ... unless the next page has data (torn write at this page's start)
          o = pageEnd;
          continue;
        }
        *off = (uint16_t)o;
        return false;
      }
      o = pageEnd;                  // the rest of this page was left empty
      continue;
    }
    uint8_t plen = p[1];
    if ((plen & 3u) || o + LOG_REC_HDR + plen > pageEnd) {
      if (errors) (*errors)++;
      o = pageEnd;
      continue;
    }
    memcpy(rec, p, LOG_REC_HDR + plen);
    uint16_t crc = (uint16_t)(rec[2] | (rec[3] << 8));
    if (recCrc(rec[0], rec[1], rec + LOG_REC_HDR) != crc) {   // torn / corrupt: skip the page
      if (errors) (*errors)++;
      o = pageEnd;
      continue;
    }
    if (at) *at = (uint16_t)o;
    *off = (uint16_t)(o + LOG_REC_HDR + plen);
    return true;
  }
  *off = LOG_SECTOR_SIZE;
  return false;
}

// ---- Regions ---------------------------------------------------------------------------------
static inline uint16_t regStep(const Region& r, uint32_t s, uint32_t k) {
  return (uint16_t)(r.first + ((s - r.first + k) % r.count));
}
static inline uint16_t regNext(const Region& r) { return r.head < 0 ? r.first : regStep(r, (uint32_t)r.head, 1); }

static uint16_t regPool(const Region& r) {
  uint16_t n = 0;
  uint16_t s = regNext(r);
  while (n < r.count - 1 && g_secSeq[s] == SEQ_ERASED) {
    n++;
    s = regStep(r, s, 1);
  }
  return n;
}

// Payload bytes available for the next record without opening a new sector (0 = full).
static uint16_t regRoom(const Region& r, uint16_t minPayload) {
  if (r.head < 0 || r.writeOff >= LOG_SECTOR_SIZE) return 0;
  uint32_t o = r.writeOff;
  uint32_t here = LOG_PAGE_SIZE - (o & (LOG_PAGE_SIZE - 1));
  if (here >= (uint32_t)LOG_REC_HDR + minPayload) return (uint16_t)(here - LOG_REC_HDR);
  uint32_t nextPage = (o & ~(uint32_t)(LOG_PAGE_SIZE - 1)) + LOG_PAGE_SIZE;
  return nextPage < LOG_SECTOR_SIZE ? (uint16_t)(LOG_PAGE_SIZE - LOG_REC_HDR) : 0;
}

// Offset for a record of `len` bytes in the head sector (0 = does not fit).
static uint16_t regPlace(const Region& r, uint32_t len) {
  if (r.head < 0 || r.writeOff >= LOG_SECTOR_SIZE) return 0;
  uint32_t o = r.writeOff;
  if ((o & (LOG_PAGE_SIZE - 1)) + len > LOG_PAGE_SIZE) o = (o & ~(uint32_t)(LOG_PAGE_SIZE - 1)) + LOG_PAGE_SIZE;
  if (o + len > LOG_SECTOR_SIZE) return 0;
  return (uint16_t)o;
}

// ---- Index maintenance (callers hold g_mtx, or run before the task starts) ---------------------
static void capIndexAdd(const CapIdx& e) {
  if (g_capN >= LOG_CAP_INDEX) {   // keep the newest: drop the lowest id
    memmove(&g_capIdx[0], &g_capIdx[1], (LOG_CAP_INDEX - 1) * sizeof(CapIdx));
    g_capN = LOG_CAP_INDEX - 1;
  }
  uint16_t i = g_capN;
  while (i > 0 && g_capIdx[i - 1].id > e.id) {
    g_capIdx[i] = g_capIdx[i - 1];
    i--;
  }
  g_capIdx[i] = e;
  g_capN++;
}

static void capIndexForgetSeq(uint32_t q) {   // sector with seq q is being erased
  uint16_t w = 0;
  for (uint16_t i = 0; i < g_capN; i++) {
    if (g_capIdx[i].beginSeq <= q) continue;
    g_capIdx[w++] = g_capIdx[i];
  }
  g_capN = w;
}

static DrvIdx* drvFind(uint32_t boot) {
  for (uint16_t i = 0; i < g_drvN; i++) if (g_drvIdx[i].boot == boot) return &g_drvIdx[i];
  return nullptr;
}

static void drvRemove(uint16_t i) {
  if (i >= g_drvN) return;
  memmove(&g_drvIdx[i], &g_drvIdx[i + 1], (g_drvN - i - 1) * sizeof(DrvIdx));
  g_drvN--;
}

static DrvIdx* drvOpen(uint32_t boot, uint16_t sec, uint32_t seq) {
  DrvIdx* d = drvFind(boot);
  if (d) return d;
  if (g_drvN >= LOG_DRV_INDEX) {   // drop the oldest drive
    uint16_t lo = 0;
    for (uint16_t i = 1; i < g_drvN; i++) if (g_drvIdx[i].boot < g_drvIdx[lo].boot) lo = i;
    drvRemove(lo);
  }
  d = &g_drvIdx[g_drvN++];
  memset(d, 0, sizeof(*d));
  d->boot = boot;
  d->firstSec = sec;
  d->firstSeq = seq;
  d->lastSeq = seq;
  d->t0 = DRV_T0_NONE;
  return d;
}

// =========================================================================================
// BOOT SCAN (setup, before the task starts)
// =========================================================================================
static void scanCaptureRegion(FlashWin& w, uint32_t* maxId, uint32_t* maxBoot) {
  Region& r = g_rg[LOG_REGION_CAP];
  r.writeOff = LOG_SECTOR_SIZE;
  bool active = false;
  CapIdx e;
  memset(&e, 0, sizeof(e));
  uint32_t crc = 0;
  uint16_t gotN = 0, gotNT = 0;
  uint32_t lastSeq = 0;
  uint16_t start = regNext(r);
  for (uint16_t k = 0; k < r.count; k++) {
    uint16_t s = regStep(r, start, k);
    uint32_t q = g_secSeq[s];
    if (!seqValid(q) || q <= lastSeq) {
      active = false;
      continue;
    }
    lastSeq = q;
    bool live = q >= g_clearSeq;
    uint16_t off = LOG_SECTOR_HDR, at = 0;
    while (nextRecord(w, s, &off, g_wRec, &at, &g_wRecErr)) {
      uint8_t type = g_wRec[0], plen = g_wRec[1];
      const uint8_t* pl = g_wRec + LOG_REC_HDR;
      uint32_t rid = 0;
      if (plen >= 4) memcpy(&rid, pl, 4);
      if (type == REC_CAP_BEGIN && plen >= sizeof(CapMeta)) {
        CapMeta m;
        memcpy(&m, pl, sizeof(m));
        if (m.id > *maxId) *maxId = m.id;
        if (m.boot > *maxBoot) *maxBoot = m.boot;
        active = live && m.type < CT_COUNT;
        if (active) {
          memset(&e, 0, sizeof(e));
          e.id = m.id;
          e.boot = m.boot;
          e.t0 = m.t0Ms;
          e.dur = m.durMs;
          e.n = m.nSamples;
          e.nTrace = m.nTrace;
          e.type = m.type;
          e.flags = m.flags;
          e.beginSec = s;
          e.beginOff = at;
          e.beginSeq = q;
          crc = 0;
          gotN = gotNT = 0;
        }
      } else if (!active) {
        continue;
      } else if (type == REC_CAP_SAMPLES && plen >= 16 && ((plen - 4) % sizeof(LogSample)) == 0 && rid == e.id) {
        crc = crc32Update(crc, pl + 4, plen - 4);
        gotN += (plen - 4) / sizeof(LogSample);
      } else if (type == REC_CAP_TRACE && plen >= 12 && ((plen - 4) % sizeof(EngineTraceEvent)) == 0 && rid == e.id) {
        crc = crc32Update(crc, pl + 4, plen - 4);
        gotNT += (plen - 4) / sizeof(EngineTraceEvent);
      } else if (type == REC_CAP_END && plen >= sizeof(CapEnd)) {
        CapEnd ce;
        memcpy(&ce, pl, sizeof(ce));
        if (ce.id == e.id && ce.nSamples == e.n && gotN == e.n && ce.nTrace == e.nTrace && gotNT == e.nTrace &&
            ce.crc == crc) {
          e.endSeq = q;
          capIndexAdd(e);
        }
        active = false;
      } else {
        active = false;   // foreign record inside a capture: the capture is incomplete
      }
    }
    if (s == r.head && live) {
      // Resume after the last record, on a page whose remaining bytes are really blank.
      uint32_t o = off;
      while (o < LOG_SECTOR_SIZE) {
        uint32_t pageEnd = (o & ~(uint32_t)(LOG_PAGE_SIZE - 1)) + LOG_PAGE_SIZE;
        if (rangeBlank(w, s * LOG_SECTOR_SIZE + o, pageEnd - o)) break;
        o = pageEnd;
      }
      r.writeOff = (uint16_t)(o < LOG_SECTOR_SIZE ? o : LOG_SECTOR_SIZE);
    }
  }
}

static void scanDriveRegion(FlashWin& w, uint32_t* maxBoot) {
  Region& r = g_rg[LOG_REGION_DRIVE];
  r.writeOff = LOG_SECTOR_SIZE;   // a new boot always opens a new sector
  uint32_t lastSeq = 0;
  DrvIdx* cur = nullptr;
  uint16_t start = regNext(r);
  for (uint16_t k = 0; k < r.count; k++) {
    uint16_t s = regStep(r, start, k);
    uint32_t q = g_secSeq[s];
    if (!seqValid(q) || q <= lastSeq) {
      cur = nullptr;
      continue;
    }
    lastSeq = q;
    SectorHdr h;
    if (!flashRead(w, (uint32_t)s * LOG_SECTOR_SIZE, &h, sizeof(h))) continue;
    if (h.boot > *maxBoot) *maxBoot = h.boot;
    if (q < g_clearSeq) {
      cur = nullptr;
      continue;
    }
    if (!cur || cur->boot != h.boot) cur = drvOpen(h.boot, s, q);
    cur->lastSeq = q;
    uint16_t off = LOG_SECTOR_HDR, at = 0;
    while (nextRecord(w, s, &off, g_wRec, &at, &g_wRecErr)) {
      uint8_t type = g_wRec[0], plen = g_wRec[1];
      const uint8_t* pl = g_wRec + LOG_REC_HDR;
      if (type == REC_DRV_SAMPLES && plen >= sizeof(LogSample) && (plen % sizeof(LogSample)) == 0) {
        uint16_t n = plen / sizeof(LogSample);
        LogSample a, b;
        memcpy(&a, pl, sizeof(a));
        memcpy(&b, pl + (n - 1) * sizeof(LogSample), sizeof(b));
        if (cur->t0 == DRV_T0_NONE) cur->t0 = a.tMs;
        cur->tEnd = b.tMs;
        cur->n += n;
      } else if (type == REC_DRV_GAP) {
        cur->gaps++;
      }
    }
  }
}

static void scanFlash(uint32_t nvsBoot, uint32_t nvsCapId) {
  uint32_t t0 = millis();
  uint32_t maxSeq = 0, maxBoot = 0, maxId = 0;
  for (uint32_t s = 0; s < g_nSec; s++) {
    SectorHdr h;
    uint8_t region = s < g_capSec ? LOG_REGION_CAP : LOG_REGION_DRIVE;
    if (!flashRead(g_wWin, s * LOG_SECTOR_SIZE, &h, sizeof(h))) {
      g_secSeq[s] = SEQ_DIRTY;
    } else if (hdrValid(h, region)) {
      g_secSeq[s] = h.seq;
      if (h.seq > maxSeq) maxSeq = h.seq;
      if (h.boot > maxBoot) maxBoot = h.boot;
    } else if (h.magic == 0xFFFFFFFFu && rangeBlank(g_wWin, s * LOG_SECTOR_SIZE, LOG_SECTOR_SIZE)) {
      g_secSeq[s] = SEQ_ERASED;
    } else {
      g_secSeq[s] = SEQ_DIRTY;
    }
  }
  g_nextSeq = maxSeq + 1;
  if (g_nextSeq <= g_clearSeq) g_nextSeq = g_clearSeq + 1;
  for (int ri = 0; ri < 2; ri++) {
    Region& r = g_rg[ri];
    int best = -1;
    uint32_t bestSeq = 0;
    for (uint16_t k = 0; k < r.count; k++) {
      uint32_t q = g_secSeq[r.first + k];
      if (seqValid(q) && (best < 0 || q > bestSeq)) {
        best = r.first + k;
        bestSeq = q;
      }
    }
    r.head = (int16_t)best;
  }
  scanCaptureRegion(g_wWin, &maxId, &maxBoot);
  scanDriveRegion(g_wWin, &maxBoot);
  winRelease(g_wWin);
  g_boot = (nvsBoot > maxBoot ? nvsBoot : maxBoot) + 1;
  g_nextCapId = maxId + 1;
  if (nvsCapId > g_nextCapId) g_nextCapId = nvsCapId;
  g_scanErrors = g_wRecErr;
  g_scanMs = millis() - t0;
}

// =========================================================================================
// FLASH WRITES (sampler task; callers hold g_mtx and have checked the write policy)
// =========================================================================================
static void noteFlashError() {
  if (++g_flashErrors >= LOG_MAX_FLASH_ERRORS && g_flashOk) {
    g_flashOk = false;
    Serial.println("[LOG] too many flash errors: flash logging switched off (RAM ring keeps running)");
  }
}

static bool progRaw(uint32_t partOff, const void* src, size_t len) {
  if (!g_part || len == 0 || partOff >= g_part->size || len > g_part->size - partOff) return false;
  esp_err_t e = esp_partition_write(g_part, partOff, src, len);
  g_lastProgMs = millis();
  g_flashOpSinceSample = true;
  if (e != ESP_OK) {
    noteFlashError();
    return false;
  }
  return true;
}

enum : uint8_t { OP_NONE = 0, OP_DONE, OP_BLOCKED, OP_OPENED };

// Opens the next sector of region r (writes its header). OP_BLOCKED = it is not erased yet.
static uint8_t openSector(Region& r, uint8_t region) {
  uint16_t s = regNext(r);
  if (g_secSeq[s] != SEQ_ERASED) return OP_BLOCKED;
  SectorHdr h;
  h.magic = LOG_MAGIC;
  h.version = LOG_FORMAT_VERSION;
  h.region = region;
  h.reserved = 0;
  h.seq = g_nextSeq;
  h.boot = g_boot;
  h.crc = crc32Update(0, (const uint8_t*)&h, 16);
  if (!progRaw((uint32_t)s * LOG_SECTOR_SIZE, &h, sizeof(h))) {
    g_secSeq[s] = SEQ_DIRTY;   // re-erased by the pool maintenance
    return OP_DONE;
  }
  g_secSeq[s] = g_nextSeq++;
  r.head = (int16_t)s;
  r.writeOff = LOG_SECTOR_HDR;
  return OP_OPENED;
}

// Programs the record prepared in g_wRec + LOG_REC_HDR (plen payload bytes) into region r.
static bool writeRec(Region& r, uint8_t type, uint8_t plen) {
  uint32_t len = LOG_REC_HDR + plen;
  uint16_t o = regPlace(r, len);
  if (!o) return false;
  g_wRec[0] = type;
  g_wRec[1] = plen;
  uint16_t crc = recCrc(type, plen, g_wRec + LOG_REC_HDR);
  g_wRec[2] = (uint8_t)(crc & 0xFF);
  g_wRec[3] = (uint8_t)(crc >> 8);
  bool ok = progRaw((uint32_t)r.head * LOG_SECTOR_SIZE + o, g_wRec, len);
  // On failure the bytes may be half-programmed: continue on the next page.
  r.writeOff = ok ? (uint16_t)(o + len) : (uint16_t)((o & ~(LOG_PAGE_SIZE - 1)) + LOG_PAGE_SIZE);
  return ok;
}

// ---- Drive log ---------------------------------------------------------------------------------
static void driveNoteLoss(uint32_t now) {
  uint32_t head = g_head;
  if (head - g_drvNext <= g_ringCap - LOG_LOSS_MARGIN) return;
  uint32_t newNext = head - (g_ringCap - 2 * LOG_LOSS_MARGIN);
  LogSample a, b;
  bool ha = ringGet(g_drvNext, a), hb = ringGet(newNext - 1, b);
  uint32_t lost = newNext - g_drvNext;
  if (g_gapPending) {
    g_gap.toMs = hb ? b.tMs : g_gap.toMs;
    g_gap.lost += lost;
  } else {
    g_gap.fromMs = ha ? a.tMs : now;
    g_gap.toMs = hb ? b.tMs : now;
    g_gap.lost = lost;
    g_gapPending = true;
  }
  g_drvNext = newNext;
  g_lastLossMs = now;
  g_lossSeen = true;
}

static uint8_t driveStep(uint32_t now) {
  Region& r = g_rg[LOG_REGION_DRIVE];
  uint32_t backlog = g_head - g_drvNext;
  if (!backlog && !g_gapPending) return OP_NONE;
  uint16_t need = g_drvNeedInfo ? sizeof(DrvInfo) + sizeof(NameField)
                                 : (g_gapPending ? sizeof(DrvGap) : sizeof(LogSample));
  if (!g_drvOurs || regRoom(r, need) < need) {
    uint8_t res = openSector(r, LOG_REGION_DRIVE);
    g_drvBlocked = (res == OP_BLOCKED);
    if (res == OP_OPENED) {
      g_drvOurs = true;
      g_drvNeedInfo = true;   // every drive sector starts with fw + config
      DrvIdx* d = drvOpen(g_boot, (uint16_t)r.head, g_secSeq[r.head]);
      d->lastSeq = g_secSeq[r.head];
      return OP_DONE;
    }
    return res;
  }
  g_drvBlocked = false;
  if (g_drvNeedInfo) {
    DrvInfo inf;
    memset(&inf, 0, sizeof(inf));
    inf.boot = g_boot;
    copyFw(inf.fw);
    inf.cfg = makeCfg(engineGetConfig());
    char pn[PROFILE_NAME_BYTES + 1];
    profileActiveName(pn, sizeof(pn));
    NameField nf = makeName(pn);
    memcpy(g_wRec + LOG_REC_HDR, &inf, sizeof(inf));
    memcpy(g_wRec + LOG_REC_HDR + sizeof(inf), &nf, sizeof(nf));
    if (writeRec(r, REC_DRV_INFO, sizeof(inf) + sizeof(nf))) g_drvNeedInfo = false;
    return OP_DONE;
  }
  if (g_gapPending) {
    memcpy(g_wRec + LOG_REC_HDR, &g_gap, sizeof(g_gap));
    if (writeRec(r, REC_DRV_GAP, sizeof(g_gap))) {
      g_gapPending = false;
      DrvIdx* d = drvFind(g_boot);
      if (d) d->gaps++;
    }
    return OP_DONE;
  }
  uint16_t room = regRoom(r, sizeof(LogSample));
  uint32_t kCap = room / sizeof(LogSample);
  if (kCap > DRV_SAMPLES_PER_REC) kCap = DRV_SAMPLES_PER_REC;
  LogSample first;
  if (!ringGet(g_drvNext, first)) return OP_NONE;   // lost: driveNoteLoss() handles it next tick
  if (backlog < kCap && (now - first.tMs) < LOG_DRV_FLUSH_MS) return OP_NONE;
  uint32_t k = backlog < kCap ? backlog : kCap;
  uint8_t* dst = g_wRec + LOG_REC_HDR;
  for (uint32_t i = 0; i < k; i++) {
    LogSample s;
    if (!ringGet(g_drvNext + i, s)) return OP_NONE;
    memcpy(dst + i * sizeof(LogSample), &s, sizeof(s));
  }
  if (writeRec(r, REC_DRV_SAMPLES, (uint8_t)(k * sizeof(LogSample)))) {
    LogSample last;
    memcpy(&last, dst + (k - 1) * sizeof(LogSample), sizeof(last));
    g_drvNext += k;
    DrvIdx* d = drvFind(g_boot);
    if (d) {
      if (d->t0 == DRV_T0_NONE) d->t0 = first.tMs;
      d->tEnd = last.tMs;
      d->n += k;
      d->lastSeq = g_secSeq[r.head];
    }
  }
  return OP_DONE;
}

// ---- Captures ----------------------------------------------------------------------------------
static void tbRelease(int8_t tb) {
  if (tb >= 0 && tb < LOG_TRACE_BUFS && g_tb[tb]) g_tb[tb]->used = false;
}

static int8_t tbAcquire() {
  for (int8_t i = 0; i < LOG_TRACE_BUFS; i++) {
    TraceBuf* b = g_tb[i];
    if (b && !b->used) {
      b->used = true;
      b->headN = b->tailN = b->tailPos = 0;
      b->lost = 0;
      return i;
    }
  }
  return -1;
}

static uint16_t tbCount(const TraceBuf* b) { return b ? (uint16_t)(b->headN + b->tailN) : 0; }

static const EngineTraceEvent& tbAt(const TraceBuf* b, uint16_t i) {
  if (i < b->headN) return b->ev[i];
  uint16_t j = i - b->headN;
  uint16_t start = (b->tailN < LOG_TRACE_HALF) ? 0 : b->tailPos;
  return b->ev[LOG_TRACE_HALF + (start + j) % LOG_TRACE_HALF];
}

static void tbPush(TraceBuf* b, const EngineTraceEvent& e) {
  if (b->headN < LOG_TRACE_HALF) {
    b->ev[b->headN++] = e;
    return;
  }
  b->ev[LOG_TRACE_HALF + b->tailPos] = e;
  b->tailPos = (uint16_t)((b->tailPos + 1) % LOG_TRACE_HALF);
  if (b->tailN < LOG_TRACE_HALF) b->tailN++;
  else b->lost++;
}

static void pendingPop() {   // drop g_pend[0]
  tbRelease(g_pend[0].tb);
  for (uint8_t i = 1; i < g_pendN; i++) g_pend[i - 1] = g_pend[i];
  if (g_pendN) g_pendN--;
}

// A pending capture whose next unwritten sample is about to leave the RAM ring is dropped.
static void pendingNoteLoss() {
  for (uint8_t i = 0; i < g_pendN;) {
    Pending& p = g_pend[i];
    uint32_t nextSeq = p.seq0 + (p.phase == PH_SAMPLES ? p.done : 0);
    bool needSamples = p.phase <= PH_SAMPLES && (p.phase == PH_BEGIN || p.done < p.meta.nSamples);
    if (needSamples && (g_head - nextSeq) > g_ringCap - LOG_LOSS_MARGIN) {
      g_dropped++;
      tbRelease(p.tb);
      for (uint8_t j = i + 1; j < g_pendN; j++) g_pend[j - 1] = g_pend[j];
      g_pendN--;
      continue;
    }
    i++;
  }
}

static uint8_t captureStep() {
  if (!g_pendN) return OP_NONE;
  Region& r = g_rg[LOG_REGION_CAP];
  Pending& p = g_pend[0];
  const TraceBuf* tb = (p.tb >= 0) ? g_tb[p.tb] : nullptr;
  uint32_t payload = 0;
  switch (p.phase) {
    case PH_BEGIN: payload = sizeof(CapMeta) + pad4(p.meta.sumLen) + sizeof(NameField); break;
    case PH_SAMPLES: payload = 4 + sizeof(LogSample); break;
    case PH_TRACE: payload = 4 + sizeof(EngineTraceEvent); break;
    default: payload = sizeof(CapEnd); break;
  }
  uint16_t room = regRoom(r, (uint16_t)payload);
  if (room < payload) {
    uint8_t res = openSector(r, LOG_REGION_CAP);
    return res == OP_OPENED ? (uint8_t)OP_DONE : res;
  }
  uint8_t* dst = g_wRec + LOG_REC_HDR;
  if (p.phase == PH_BEGIN) {
    memset(dst, 0, payload);
    memcpy(dst, &p.meta, sizeof(CapMeta));
    memcpy(dst + sizeof(CapMeta), p.sum, p.meta.sumLen);
    NameField nf = makeName(p.profName);
    memcpy(dst + sizeof(CapMeta) + pad4(p.meta.sumLen), &nf, sizeof(nf));
    uint16_t o = regPlace(r, LOG_REC_HDR + payload);
    if (writeRec(r, REC_CAP_BEGIN, (uint8_t)payload)) {
      p.beginSec = (uint16_t)r.head;
      p.beginOff = o;
      p.beginSeq = g_secSeq[r.head];
      p.phase = PH_SAMPLES;
      p.done = 0;
      p.crc = 0;
      if (p.meta.nSamples == 0) p.phase = PH_TRACE;
      if (p.phase == PH_TRACE && p.meta.nTrace == 0) p.phase = PH_END;
    }
    return OP_DONE;
  }
  if (p.phase == PH_SAMPLES) {
    uint32_t k = (room - 4) / sizeof(LogSample);
    if (k > CAP_SAMPLES_PER_REC) k = CAP_SAMPLES_PER_REC;
    if (k > (uint32_t)(p.meta.nSamples - p.done)) k = p.meta.nSamples - p.done;
    memcpy(dst, &p.meta.id, 4);
    for (uint32_t i = 0; i < k; i++) {
      LogSample s;
      if (!ringGet(p.seq0 + p.done + i, s)) {   // left the ring: this capture cannot be completed
        g_dropped++;
        pendingPop();
        return OP_NONE;
      }
      memcpy(dst + 4 + i * sizeof(LogSample), &s, sizeof(s));
    }
    uint32_t plen = 4 + k * sizeof(LogSample);
    if (writeRec(r, REC_CAP_SAMPLES, (uint8_t)plen)) {
      p.crc = crc32Update(p.crc, dst + 4, plen - 4);
      p.done += k;
      if (p.done >= p.meta.nSamples) {
        p.phase = p.meta.nTrace ? PH_TRACE : PH_END;
        p.done = 0;
      }
    }
    return OP_DONE;
  }
  if (p.phase == PH_TRACE) {
    uint32_t k = (room - 4) / sizeof(EngineTraceEvent);
    if (k > CAP_TRACE_PER_REC) k = CAP_TRACE_PER_REC;
    if (k > (uint32_t)(p.meta.nTrace - p.done)) k = p.meta.nTrace - p.done;
    memcpy(dst, &p.meta.id, 4);
    for (uint32_t i = 0; i < k; i++) {
      EngineTraceEvent ev;
      if (tb && p.done + i < tbCount(tb)) ev = tbAt(tb, (uint16_t)(p.done + i));
      else memset(&ev, 0, sizeof(ev));
      memcpy(dst + 4 + i * sizeof(EngineTraceEvent), &ev, sizeof(ev));
    }
    uint32_t plen = 4 + k * sizeof(EngineTraceEvent);
    if (writeRec(r, REC_CAP_TRACE, (uint8_t)plen)) {
      p.crc = crc32Update(p.crc, dst + 4, plen - 4);
      p.done += k;
      if (p.done >= p.meta.nTrace) {
        p.phase = PH_END;
        p.done = 0;
      }
    }
    return OP_DONE;
  }
  // PH_END
  CapEnd ce;
  ce.id = p.meta.id;
  ce.nSamples = p.meta.nSamples;
  ce.nTrace = p.meta.nTrace;
  ce.crc = p.crc;
  memcpy(dst, &ce, sizeof(ce));
  if (writeRec(r, REC_CAP_END, sizeof(ce))) {
    CapIdx e;
    memset(&e, 0, sizeof(e));
    e.id = p.meta.id;
    e.boot = p.meta.boot;
    e.t0 = p.meta.t0Ms;
    e.dur = p.meta.durMs;
    e.n = p.meta.nSamples;
    e.nTrace = p.meta.nTrace;
    e.type = p.meta.type;
    e.flags = p.meta.flags;
    e.beginSec = p.beginSec;
    e.beginOff = p.beginOff;
    e.beginSeq = p.beginSeq;
    e.endSeq = g_secSeq[r.head];
    capIndexAdd(e);
    pendingPop();
    g_capIdDirty = true;
  }
  return OP_DONE;
}

// ---- Erases / NVS ------------------------------------------------------------------------------
// Writes the dirty NVS values. `full` = logging enabled: the boot counter and the capture-id
// counter too; while disabled only the setting itself and a clear epoch are written. Flags are
// cleared only after a successful write (a pending value is retried).
static void persistNvs(bool full) {
  Preferences p;
  bool ok = p.begin("swlog", false);
  if (ok) {
    uint8_t en = g_enabled ? 1 : 0;
    if (g_nvsEnDirty && p.getUChar("en", 1) != en) ok = p.putUChar("en", en) != 0 && ok;
    if (g_nvsClrDirty && p.getUInt("clr", 0) != g_clearSeq) ok = p.putUInt("clr", g_clearSeq) != 0 && ok;
    if (full && g_nvsBootDirty && p.getUInt("boot", 0) != g_boot) ok = p.putUInt("boot", g_boot) != 0 && ok;
    if (full && g_capIdDirty && p.getUInt("capid", 0) < g_nextCapId) ok = p.putUInt("capid", g_nextCapId) != 0 && ok;
    p.end();
  }
  if (ok) {
    g_nvsEnDirty = g_nvsClrDirty = false;
    if (full) g_nvsBootDirty = g_capIdDirty = false;
  }
  g_nvsLastMs = millis();
  g_lastEraseMs = g_nvsLastMs | 1;   // counts as an erase-class operation
  g_flashOpSinceSample = true;
}

static inline bool loggingOn() { return g_flashOk && g_enabled && g_enApplied; }

static bool nvsPending(uint32_t now) {
  if (g_nvsEnDirty || g_nvsClrDirty) return true;
  return loggingOn() && (g_nvsBootDirty || (g_capIdDirty && now - g_nvsLastMs >= LOG_NVS_CAPID_MS));
}

// Accounting before a drive sector is recycled: its samples / gaps leave the drive's index entry.
static void driveRecycle(uint16_t s, uint32_t q) {
  for (uint16_t i = 0; i < g_drvN; i++) {
    DrvIdx& d = g_drvIdx[i];
    if (q < d.firstSeq || q > d.lastSeq) continue;
    uint32_t n = 0, gaps = 0;
    uint16_t off = LOG_SECTOR_HDR;
    while (nextRecord(g_wWin, s, &off, g_wRec, nullptr, nullptr)) {
      if (g_wRec[0] == REC_DRV_SAMPLES) n += g_wRec[1] / sizeof(LogSample);
      else if (g_wRec[0] == REC_DRV_GAP) gaps++;
    }
    d.n -= (n < d.n) ? n : d.n;
    d.gaps -= (uint16_t)((gaps < d.gaps) ? gaps : d.gaps);
    const Region& r = g_rg[LOG_REGION_DRIVE];
    uint16_t ns = regStep(r, s, 1);
    uint32_t nq = g_secSeq[ns];
    SectorHdr h;
    if (q >= d.lastSeq || !seqValid(nq) || nq <= q || nq > d.lastSeq ||
        !flashRead(g_wWin, (uint32_t)ns * LOG_SECTOR_SIZE, &h, sizeof(h)) || h.boot != d.boot) {
      if (d.boot == g_boot && q >= d.lastSeq) {   // (cannot happen: the head is never erased)
        d.n = 0;
        d.t0 = DRV_T0_NONE;
      } else {
        drvRemove(i);
      }
      return;
    }
    d.firstSec = ns;
    d.firstSeq = nq;
    d.t0 = DRV_T0_NONE;
    off = LOG_SECTOR_HDR;
    while (nextRecord(g_wWin, ns, &off, g_wRec, nullptr, nullptr)) {
      if (g_wRec[0] == REC_DRV_SAMPLES && g_wRec[1] >= sizeof(LogSample)) {
        LogSample a;
        memcpy(&a, g_wRec + LOG_REC_HDR, sizeof(a));
        d.t0 = a.tMs;
        break;
      }
    }
    return;
  }
}

static bool eraseCandidate(uint8_t ri, uint16_t* sec) {
  const Region& r = g_rg[ri];
  uint16_t pool = regPool(r);
  if (pool >= r.poolTarget || pool >= r.count - 1) return false;
  uint16_t s = regStep(r, regNext(r), pool);   // first non-erased sector ahead of the head
  if (r.head >= 0 && s == (uint16_t)r.head) return false;
  *sec = s;
  return true;
}

static bool eraseNeeded(uint32_t now) {
  uint16_t s;
  return nvsPending(now) ||
         (loggingOn() && (eraseCandidate(LOG_REGION_DRIVE, &s) || eraseCandidate(LOG_REGION_CAP, &s)));
}

static void eraseStep(uint32_t now) {   // holds g_mtx
  if (nvsPending(now)) {
    persistNvs(loggingOn());
    return;
  }
  if (!loggingOn()) return;   // switched off: no sector erases
  for (uint8_t ri = LOG_REGION_DRIVE;; ri = LOG_REGION_CAP) {   // drive pool first
    uint16_t s;
    if (eraseCandidate(ri, &s)) {
      uint32_t q = g_secSeq[s];
      if (seqValid(q) && q >= g_clearSeq) {
        if (ri == LOG_REGION_CAP) capIndexForgetSeq(q);
        else driveRecycle(s, q);
      }
      g_secSeq[s] = SEQ_ERASING;
      esp_err_t e = esp_partition_erase_range(g_part, (size_t)s * LOG_SECTOR_SIZE, LOG_SECTOR_SIZE);
      g_lastEraseMs = millis() | 1;
      g_flashOpSinceSample = true;
      g_secSeq[s] = (e == ESP_OK) ? SEQ_ERASED : SEQ_DIRTY;
      if (e != ESP_OK) noteFlashError();
      return;
    }
    if (ri == LOG_REGION_CAP) return;
  }
}

// ---- Write policy ----------------------------------------------------------------------------
static bool progSafe(uint32_t now, const EngineTelemetry& t) {
  if (otaIsBusy()) return false;
  if (t.cutActive || t.launchState != LAUNCH_OFF || t.showActive || t.switchActive) return false;
  if (g_everCut && (now - g_lastCutMs) < LOG_PROG_NOCUT_MS) return false;
  // A program stalls both CPUs ~1 ms and can drop the engine's slot clock: keep the flash
  // quiet while the engine approaches the rev limiter, so its first cut is never delayed.
  if (t.rpm >= engineGetConfig().redlineRpm - LOG_PROG_REDLINE_MARGIN) return false;
  return true;
}

static bool eraseSafe(uint32_t now, const EngineTelemetry& t) {
  if (!progSafe(now, t)) return false;
  if (g_lastEraseMs && (now - g_lastEraseMs) < LOG_ERASE_GAP_MS) return false;
  bool stopped = t.rpm == 0 && !t.rpmEstimated && g_zeroSince && (now - g_zeroSince) >= LOG_ERASE_STOP_MS;
  bool idle = t.rpm > 0 && t.rpm < LOG_ERASE_IDLE_RPM && g_idleSince && (now - g_idleSince) >= LOG_ERASE_IDLE_MS &&
              (!g_everCut || (now - g_lastCutMs) >= LOG_ERASE_IDLE_NOCUT_MS) && !g_ghost;
  return stopped || idle;
}

static void applyClear() {   // holds g_mtx; the handler already reset the index and the epoch
  g_clearReq = false;
  g_rg[LOG_REGION_CAP].writeOff = LOG_SECTOR_SIZE;
  g_rg[LOG_REGION_DRIVE].writeOff = LOG_SECTOR_SIZE;
  g_drvOurs = false;
  g_gapPending = false;
  while (g_pendN) pendingPop();
  g_pauseActive = false;   // the drive before the clear is forgotten: no pause gap after it
  g_nvsClrDirty = true;
}

// Applies a change of `enabled` (sampler task). Retried next tick if the lock is busy.
static void applyEnabledChange() {
  bool en = g_enabled;
  if (en == g_enApplied || !takeMutex(pdMS_TO_TICKS(2))) return;
  en = g_enabled;
  if (!en) {
    if (g_win.open) {   // automatic capture in progress: discarded
      g_win.open = false;
      tbRelease(g_win.tb);
    }
    while (g_pendN) pendingPop();   // pending captures are discarded (not counted in `dropped`)
    portENTER_CRITICAL(&g_idMux);
    g_snapReq = 0;
    portEXIT_CRITICAL(&g_idMux);
    // The drive ends at its last written record; the unwritten backlog is not logged.
    DrvIdx* d = drvFind(g_boot);
    g_pauseActive = g_drvOurs && d && d->n > 0;
    g_pauseSeq = g_drvNext;
    LogSample s;
    g_pauseFromMs = ringGet(g_drvNext, s) ? s.tMs : millis();
    g_drvNext = g_head;
    g_gapPending = false;
    g_drvOurs = false;   // switching on again opens a new sector (DRV_INFO with the config of then)
    g_drvNeedInfo = false;
    g_drvBlocked = false;
  } else {
    uint32_t head = g_head;
    if (g_pauseActive && drvFind(g_boot) && head > g_pauseSeq) {   // documented pause in the same drive
      LogSample s;
      g_gap.fromMs = g_pauseFromMs;
      g_gap.toMs = ringGet(head - 1, s) ? s.tMs : millis();
      g_gap.lost = head - g_pauseSeq;
      g_gapPending = true;
    }
    g_pauseActive = false;
    g_drvNext = head;
    g_unsyncN = 0;
  }
  g_enApplied = en;
  giveMutex();
}

static void flashWork(uint32_t now) {
  if (g_clearReq && takeMutex(pdMS_TO_TICKS(5))) {
    applyClear();
    giveMutex();
  }
  bool on = loggingOn();
  if (on) {
    driveNoteLoss(now);
    pendingNoteLoss();
  } else if (!g_enApplied || !g_flashOk) {
    g_drvNext = g_head;   // nothing is logged: no backlog, no bogus gap later
  }                       // (a switch-off not applied yet keeps g_drvNext for the pause record)
  if (otaIsBusy()) {   // pause everything while the firmware is being written
    g_writeAllowed = false;
    return;
  }
  EngineTelemetry t = engineGetTelemetry();
  g_writeAllowed = on && progSafe(now, t);
  for (int op = 0; on && op < LOG_PROG_PER_TICK; op++) {
    if (op) vTaskDelay(pdMS_TO_TICKS(LOG_PROG_GAP_MS));
    now = millis();
    if ((now - g_lastProgMs) < LOG_PROG_GAP_MS) break;
    t = engineGetTelemetry();   // fresh, right before the program
    if (!progSafe(now, t)) break;
    if (!takeMutex(pdMS_TO_TICKS(2))) break;
    if (g_clearReq) applyClear();
    uint8_t res = OP_NONE;
    if (loggingOn()) {   // re-checked under the lock: POST /api/log/config switches under it
      res = driveStep(now);
      if (res != OP_DONE) res = captureStep();
    }
    giveMutex();
    if (res != OP_DONE) break;
  }
  now = millis();
  t = engineGetTelemetry();
  if (eraseSafe(now, t) && eraseNeeded(now) && takeMutex(pdMS_TO_TICKS(2))) {
    eraseStep(now);
    giveMutex();
  }
}

// =========================================================================================
// CAPTURE WINDOWS (sampler task)
// =========================================================================================
static void drainTrace() {
  if (!g_win.open) return;
  if (g_win.tb < 0) {
    g_win.tb = tbAcquire();
    if (g_win.tb < 0) return;
    g_win.traceSeq = 0;   // whole engine ring (retroactive events)
  }
  TraceBuf* b = g_tb[g_win.tb];
  for (int i = 0; i < 16; i++) {
    uint32_t lost = 0;
    size_t n = engineReadTrace(g_traceTmp, TRACE_TMP, &g_win.traceSeq, &lost);
    b->lost += lost;
    if (n > TRACE_TMP) n = TRACE_TMP;
    for (size_t k = 0; k < n; k++) tbPush(b, g_traceTmp[k]);
    if (n < TRACE_TMP) break;
  }
}

static void restartTrace() {
  if (g_win.tb >= 0) {
    TraceBuf* b = g_tb[g_win.tb];
    b->headN = b->tailN = b->tailPos = 0;
    b->lost = 0;
    g_win.traceSeq = 0;
  }
  drainTrace();
}

static uint32_t allocCapId() {
  portENTER_CRITICAL(&g_idMux);
  uint32_t id = g_nextCapId++;
  portEXIT_CRITICAL(&g_idMux);
  return id;
}

static void openWindow(uint8_t type, uint32_t startSeq, uint32_t now, uint32_t endMs, int rpm) {
  memset(&g_win, 0, sizeof(g_win));
  g_win.open = true;
  g_win.type = type;
  g_win.tb = -1;
  g_win.startSeq = startSeq;
  LogSample s;
  g_win.startMs = ringGet(startSeq, s) ? s.tMs : now;
  g_win.triggerMs = now;
  g_win.endMs = endMs;
  g_win.trigRpm = rpm > 0 ? (uint32_t)rpm : 0;
  g_win.cfg = makeCfg(engineGetConfig());
  profileActiveName(g_win.profName, sizeof(g_win.profName));
  g_win.diag = makeDiag(engineGetDiag());
  drainTrace();
}

static void trigger(uint8_t type, uint32_t now, uint32_t seq, uint32_t preMs, uint32_t postMs, int rpm) {
  if (g_win.open) {   // folded into the open window
    g_win.flags |= (uint8_t)(1u << type);
    if (g_win.endMs) {
      uint32_t e = now + postMs;
      uint32_t cap = g_win.startMs + CAP_MAX_MS;
      if ((int32_t)(e - cap) > 0) e = cap;
      if ((int32_t)(e - g_win.endMs) > 0) g_win.endMs = e;
    }
    return;
  }
  uint32_t pre = preMs / LOG_SAMPLE_MS;
  uint32_t start = seq >= pre ? seq - pre : 0;
  uint32_t oldest = ringOldest();
  if (start < oldest) start = oldest;
  openWindow(type, start, now, now + postMs, rpm);
}

static int fmtSec(char* b, size_t cap, uint32_t ms) {   // "1,8 s"
  return snprintf(b, cap, "%lu,%lu s", (unsigned long)(ms / 1000), (unsigned long)((ms % 1000) / 100));
}

static void utf8Trim(char* s, uint8_t* len) {   // drop a sequence cut in half by snprintf
  size_t n = strlen(s);
  size_t i = n;
  while (i > 0 && ((uint8_t)s[i - 1] & 0xC0) == 0x80) i--;
  if (i > 0) {
    uint8_t lead = (uint8_t)s[i - 1];
    size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
    if (need > 1 && n - (i - 1) < need) n = i - 1;
  }
  s[n] = 0;
  *len = (uint8_t)n;
}

static const char* launchEndText(uint8_t e) {
  switch (e) {
    case LAUNCH_END_FIRED: return "FIRED";
    case LAUNCH_END_LIFT: return "gázelvétel";
    case LAUNCH_END_HOLD_TIMEOUT: return "tartás időtúllépés";
    case LAUNCH_END_ARM_TIMEOUT: return "élesítés lejárt";
    case LAUNCH_END_CANCEL: return "megszakítva";
    case LAUNCH_END_STALL: return "lefulladt";
    default: return "vége";
  }
}

static const char* reasonText(uint8_t r) {
  switch (r) {
    case CUT_SHOW: return "show gomb";
    case CUT_LAUNCH: return "rajt";
    case CUT_SWITCH: return "kapcsoló";
    case CUT_REDLINE: return "redline";
    case CUT_DECEL: return "decel";
    case CUT_GHOST: return "ghost cam";
    case CUT_BENCH: return "próbapad";
    default: return "tiltás";
  }
}

// Summary values + Hungarian text of a capture, from its samples (RAM ring) and trace.
static void buildSummary(Pending& p, const Window& w) {
  CapMeta& m = p.meta;
  const TraceBuf* tb = (p.tb >= 0) ? g_tb[p.tb] : nullptr;
  memset(m.sv, 0, sizeof(m.sv));
  int32_t peak = 0;
  uint32_t limiterMs = 0, redSlots = 0, cutTotal = 0, firedTotal = 0;
  int holdStart = -1, lastHold = -1, fired = -1;
  uint32_t prevT = 0;
  for (uint32_t i = 0; i < m.nSamples; i++) {
    LogSample s;
    if (!ringGet(p.seq0 + i, s)) continue;
    if (s.rpm > peak) peak = s.rpm;
    uint8_t ls = s.state & 3u, rs = (s.state >> 2) & 15u;
    uint32_t dt = (i && prevT) ? s.tMs - prevT : LOG_SAMPLE_MS;
    if (dt > 200) dt = LOG_SAMPLE_MS;
    prevT = s.tMs;
    if ((s.flags & SF_CUT) && rs == CUT_REDLINE) limiterMs += dt;
    if (rs == CUT_REDLINE) redSlots += s.cutSlots;
    cutTotal += s.cutSlots;
    firedTotal += s.firedSlots;
    if (ls == LAUNCH_HOLDING) {
      if (holdStart < 0) holdStart = (int)i;
      lastHold = (int)i;
    }
    if (ls == LAUNCH_FIRED && fired < 0 && holdStart >= 0) fired = (int)i;
  }
  m.sv[4] = peak;
  char txt[LOG_SUM_MAX + 32];
  char sec[24];
  int n = 0;
  switch (m.type) {
    case CT_LAUNCH: {
      m.launchEnd = w.launchEnd;
      int32_t holdMin = 0, holdMax = 0, releaseMs = -1, holdMs = 0;
      if (holdStart >= 0) {
        int holdEnd = lastHold;
        int d = -1;
        LogSample a, b;
        if (fired > 0) {   // the drop starts at the local maximum before FIRED
          d = fired - 1;
          while (d > holdStart && ringGet(p.seq0 + d - 1, a) && ringGet(p.seq0 + d, b) && a.rpm >= b.rpm) d--;
          holdEnd = d;
        }
        LogSample h0;
        uint32_t tHold0 = ringGet(p.seq0 + holdStart, h0) ? h0.tMs : 0;
        for (int pass = 0; pass < 2 && holdMax == 0; pass++) {   // pass 0 skips the settling time
          for (int i = holdStart; i <= holdEnd; i++) {
            LogSample s;
            if (!ringGet(p.seq0 + i, s)) continue;
            if (pass == 0 && s.tMs - tHold0 < CAP_HOLD_SETTLE_MS) continue;
            if (holdMax == 0 || s.rpm < holdMin) holdMin = s.rpm;
            if (s.rpm > holdMax) holdMax = s.rpm;
          }
        }
        LogSample ef, ed, el;
        if (fired > 0 && ringGet(p.seq0 + fired, ef) && ringGet(p.seq0 + d, ed)) releaseMs = (int32_t)(ef.tMs - ed.tMs);
        int endIdx = fired > 0 ? fired : lastHold;
        if (ringGet(p.seq0 + endIdx, el)) holdMs = (int32_t)(el.tMs - tHold0);
      }
      m.sv[0] = holdMin;
      m.sv[1] = holdMax;
      m.sv[2] = releaseMs;
      m.sv[3] = holdMs;
      m.sv[5] = engineGetDiag().launchDropRate;
      if (holdStart < 0) {
        n = snprintf(txt, sizeof(txt), "%s, nem érte el a limitet", launchEndText(m.launchEnd));
      } else if (releaseMs >= 0) {
        n = snprintf(txt, sizeof(txt), "FIRED, tartás %ld–%ld RPM, kioldás %ld ms", (long)holdMin, (long)holdMax,
                     (long)releaseMs);
      } else {
        fmtSec(sec, sizeof(sec), (uint32_t)holdMs);
        n = snprintf(txt, sizeof(txt), "%s, tartás %ld–%ld RPM, %s", launchEndText(m.launchEnd), (long)holdMin,
                     (long)holdMax, sec);
      }
      break;
    }
    case CT_REDLINE:
      m.sv[0] = (int32_t)limiterMs;
      m.sv[1] = (int32_t)redSlots;
      fmtSec(sec, sizeof(sec), limiterMs);
      n = snprintf(txt, sizeof(txt), "csúcs %ld RPM, %s limiteren", (long)peak, sec);
      break;
    case CT_FLOOD:
      m.sv[0] = w.reasonBefore;
      m.sv[1] = (int32_t)w.fullCutMs;
      fmtSec(sec, sizeof(sec), w.fullCutMs);
      n = snprintf(txt, sizeof(txt), "anti-flood: %s, %s tiltás után, csúcs %ld RPM", reasonText(w.reasonBefore), sec,
                   (long)peak);
      break;
    case CT_ANOMALY: {
      uint32_t rej = 0, outl = 0;
      for (uint16_t i = 0; tb && i < tbCount(tb); i++) {
        uint8_t k = tbAt(tb, i).kind;
        if (k == TR_REJECT) rej++;
        else if (k == TR_OUTLIER) outl++;
      }
      m.sv[0] = (int32_t)w.unsyncs;
      m.sv[1] = (int32_t)rej;
      m.sv[2] = (int32_t)outl;
      m.sv[3] = (int32_t)w.trigRpm;
      n = snprintf(txt, sizeof(txt), "%lu szinkronvesztés, %lu RPM, %lu elutasított él, %lu kiugró",
                   (unsigned long)w.unsyncs, (unsigned long)w.trigRpm, (unsigned long)rej, (unsigned long)outl);
      break;
    }
    default:   // CT_MANUAL
      m.sv[0] = (int32_t)((m.durMs + 500) / 1000);
      m.sv[1] = (int32_t)cutTotal;
      m.sv[2] = (int32_t)firedTotal;
      n = snprintf(txt, sizeof(txt), "kézi mentés, %ld s, csúcs %ld RPM", (long)m.sv[0], (long)peak);
      break;
  }
  if (n < 0) n = 0;
  for (uint8_t t = 0; t < CT_COUNT; t++) {   // triggers folded into this window
    if (!(m.flags & (1u << t)) || t == m.type) continue;
    size_t len = strlen(txt);
    if (len + 16 < sizeof(txt)) snprintf(txt + len, sizeof(txt) - len, ", +%s", CAP_TYPE_NAME[t]);
  }
  txt[LOG_SUM_MAX] = 0;   // hard limit (then repaired to a UTF-8 boundary)
  uint8_t len = 0;
  utf8Trim(txt, &len);
  memcpy(p.sum, txt, len);
  m.sumLen = len;
}

static Pending* newPending(uint32_t id, uint8_t type, uint32_t seq0, uint32_t lastSeq, uint32_t trigMs) {
  if (g_pendN >= LOG_MAX_PENDING) {
    g_dropped++;
    return nullptr;
  }
  Pending& p = g_pend[g_pendN];
  memset(&p, 0, sizeof(p));
  CapMeta& m = p.meta;
  m.id = id;
  m.boot = g_boot;
  m.type = type;
  uint32_t n = lastSeq - seq0 + 1;
  if (n > 65535) n = 65535;
  m.nSamples = (uint16_t)n;
  LogSample a, b;
  m.t0Ms = ringGet(seq0, a) ? a.tMs : trigMs;
  m.durMs = ringGet(seq0 + n - 1, b) ? b.tMs - m.t0Ms : 0;
  m.triggerMs = trigMs;
  copyFw(m.fw);
  p.seq0 = seq0;
  p.tb = -1;
  p.phase = PH_BEGIN;
  return &p;
}

static void closeWindow(uint32_t lastSeq) {
  if (!g_win.open) return;
  g_win.open = false;
  if (lastSeq < g_win.startSeq) {
    tbRelease(g_win.tb);
    return;
  }
  drainTrace();
  Pending* p = newPending(allocCapId(), g_win.type, g_win.startSeq, lastSeq, g_win.triggerMs);
  if (!p) {
    tbRelease(g_win.tb);
    return;
  }
  p->meta.flags = g_win.flags;
  p->meta.cfg = g_win.cfg;
  memcpy(p->profName, g_win.profName, sizeof(p->profName));
  p->meta.diag = g_win.diag;
  p->tb = g_win.tb;
  const TraceBuf* tb = (p->tb >= 0) ? g_tb[p->tb] : nullptr;
  p->meta.nTrace = tbCount(tb);
  p->meta.traceLost = tb ? tb->lost : 0;
  buildSummary(*p, g_win);
  g_pendN++;
}

static void makeManual(uint32_t id, uint32_t now, uint32_t seq, int rpm) {
  uint32_t want = CAP_MANUAL_MS / LOG_SAMPLE_MS;
  uint32_t oldest = ringOldest();
  uint32_t seq0 = (seq + 1 >= want) ? seq + 1 - want : 0;
  if (seq0 < oldest) seq0 = oldest;
  Pending* p = newPending(id, CT_MANUAL, seq0, seq, now);
  if (!p) return;
  p->meta.cfg = makeCfg(engineGetConfig());
  profileActiveName(p->profName, sizeof(p->profName));
  p->meta.diag = makeDiag(engineGetDiag());
  p->tb = tbAcquire();
  if (p->tb >= 0) {   // the engine's whole trace ring (the most recent events)
    TraceBuf* b = g_tb[p->tb];
    uint32_t tseq = 0;
    for (int i = 0; i < 16; i++) {
      uint32_t lost = 0;
      size_t n = engineReadTrace(g_traceTmp, TRACE_TMP, &tseq, &lost);
      b->lost += lost;
      if (n > TRACE_TMP) n = TRACE_TMP;
      for (size_t k = 0; k < n; k++) tbPush(b, g_traceTmp[k]);
      if (n < TRACE_TMP) break;
    }
    p->meta.nTrace = tbCount(b);
    p->meta.traceLost = b->lost;
  }
  Window w;
  memset(&w, 0, sizeof(w));
  w.trigRpm = rpm > 0 ? (uint32_t)rpm : 0;
  buildSummary(*p, w);
  g_pendN++;
}

// =========================================================================================
// SAMPLER TASK
// =========================================================================================
static void sampleOnce(uint32_t now) {
  EngineTelemetry t = engineGetTelemetry();
  EngineSlotCounters sc = engineGetSlotCounters();
  EngineDiag dg = engineGetDiag();
  TuningConfig cfg = engineGetConfig();
  bool inhibit = otaIsBusy();
  uint32_t dt = g_haveSample ? now - g_prevMs : LOG_SAMPLE_MS;

  LogSample s;
  s.tMs = now;
  s.rpm = sat16(t.rpm);
  uint8_t f = 0;
  if (t.cutActive) f |= SF_CUT;
  if (t.rpmEstimated) f |= SF_EST;
  if (t.showActive) f |= SF_SHOW;
  if (t.switchActive) f |= SF_SWITCH;
  if (cfg.armed) f |= SF_ARMED;
  if (inhibit) f |= SF_INHIBIT;
  if (t.rpm > 0 && t.measAgeMs < dt) f |= SF_FRESH;
  s.flags = f;
  s.state = (uint8_t)((t.launchState & 3u) | ((t.reason & 15u) << 2));
  uint32_t dCut = g_haveSample ? sc.cutSlots - g_prevSc.cutSlots : 0;
  uint32_t dFired = g_haveSample ? sc.firedSlots - g_prevSc.firedSlots : 0;
  uint32_t dUnsync = g_haveSample ? sc.unsyncs - g_prevSc.unsyncs : 0;
  uint32_t dFlood = g_haveSample ? dg.floodTrips - g_prevFlood : 0;
  s.cutSlots = sat8(dCut);
  s.firedSlots = sat8(dFired);
  s.measAgeMs = t.measAgeMs;
  ringPush(s);
  uint32_t seq = g_head - 1;

  // Write-policy history
  if (t.cutActive || dCut) {
    g_lastCutMs = now;
    g_everCut = true;
  }
  if (t.rpm == 0 && !t.rpmEstimated) {
    if (!g_zeroSince) g_zeroSince = now | 1;
  } else {
    g_zeroSince = 0;
  }
  if (t.rpm > 0 && t.rpm < LOG_ERASE_IDLE_RPM) {
    if (!g_idleSince) g_idleSince = now | 1;
  } else {
    g_idleSince = 0;
  }
  g_ghost = cfg.ghostCam;
  if (t.cutActive) {
    if (!g_cutRunStart) g_cutRunStart = now | 1;
    g_lastCutRunMs = now - g_cutRunStart;
    if (t.reason != CUT_FLOOD_LOCK && t.reason != CUT_NONE) g_lastCutReason = t.reason;
  } else {
    g_cutRunStart = 0;
  }

  applyEnabledChange();
  if (g_flashOk && g_enApplied && g_enabled) {
    // ---- launch: ARMED ... OFF + 3 s
    uint8_t ls = t.launchState, pls = g_prevLaunch;
    if (ls != LAUNCH_OFF && pls == LAUNCH_OFF) {
      if (g_win.open) closeWindow(seq ? seq - 1 : 0);   // every launch gets its own capture
      openWindow(CT_LAUNCH, seq, now, 0, t.rpm);
      g_win.holdingSeen = (ls == LAUNCH_HOLDING);   // the retroactive trace already has the rev-up
    } else if (g_win.open && g_win.type == CT_LAUNCH) {
      if (ls == LAUNCH_HOLDING && !g_win.holdingSeen) {
        g_win.holdingSeen = true;
        restartTrace();   // the trace of interest starts at the limiter (the engine ring has the rev-up)
      }
      if (ls == LAUNCH_OFF && pls != LAUNCH_OFF) {
        g_win.endMs = now + CAP_TAIL_MS;
        g_win.launchEnd = t.launchEnd;
      }
    }
    // ---- redline: first redline cut after >= 10 s without one
    if (t.cutActive && t.reason == CUT_REDLINE) {
      bool firstCut = !g_redSeen || (now - g_lastRedMs) >= CAP_REDLINE_QUIET_MS;
      g_redSeen = true;
      g_lastRedMs = now;
      if (firstCut) {
        trigger(CT_REDLINE, now, seq, CAP_REDLINE_PRE_MS, CAP_TAIL_MS, t.rpm);
      } else if (g_win.open && g_win.type == CT_REDLINE) {   // keep the whole limiter episode
        uint32_t e = now + CAP_TAIL_MS;
        uint32_t cap = g_win.startMs + CAP_REDLINE_MAX_MS;
        if ((int32_t)(e - cap) > 0) e = cap;
        if ((int32_t)(e - g_win.endMs) > 0) g_win.endMs = e;
      }
    }
    // ---- flood: anti-flood trip
    if (dFlood) {
      uint32_t pre = (cfg.maxCutSeconds > 0.0f) ? (uint32_t)(cfg.maxCutSeconds * 1000.0f) + CAP_FLOOD_PRE_EXTRA_MS
                                                : CAP_FLOOD_PRE_MAX_MS;
      if (pre > CAP_FLOOD_PRE_MAX_MS) pre = CAP_FLOOD_PRE_MAX_MS;
      bool wasOpen = g_win.open;
      trigger(CT_FLOOD, now, seq, pre, CAP_TAIL_MS, t.rpm);
      if (!wasOpen && g_win.open) {
        g_win.reasonBefore = g_lastCutReason;
        g_win.fullCutMs = g_lastCutRunMs;
      }
    }
    // ---- anomaly: >= 3 slot-clock drops within 2 s (not caused by our own flash stalls)
    if (g_win.open) g_win.unsyncs += dUnsync;
    bool gate = !inhibit && !g_flashOpSinceSample && t.rpm >= CAP_ANOM_MIN_RPM && g_prevRpm >= CAP_ANOM_MIN_RPM;
    if (dUnsync && gate) {
      for (uint32_t k = 0; k < dUnsync && k < CAP_ANOM_COUNT; k++) {
        if (g_unsyncN < CAP_ANOM_COUNT) {
          g_unsyncT[g_unsyncN++] = now;
        } else {
          memmove(&g_unsyncT[0], &g_unsyncT[1], (CAP_ANOM_COUNT - 1) * sizeof(uint32_t));
          g_unsyncT[CAP_ANOM_COUNT - 1] = now;
        }
      }
      if (g_unsyncN >= CAP_ANOM_COUNT && (now - g_unsyncT[0]) <= CAP_ANOM_WINDOW_MS &&
          (!g_anomSeen || (now - g_lastAnomMs) >= CAP_ANOM_REFRACT_MS)) {
        g_anomSeen = true;
        g_lastAnomMs = now;
        g_unsyncN = 0;
        bool wasOpen = g_win.open;
        trigger(CT_ANOMALY, now, seq, CAP_ANOM_PRE_MS, CAP_TAIL_MS, t.rpm);
        if (!wasOpen && g_win.open) g_win.unsyncs = CAP_ANOM_COUNT;
      }
    }
    // ---- window progress
    if (g_win.open) {
      drainTrace();
      bool end = (g_win.endMs && (int32_t)(now - g_win.endMs) >= 0) || (now - g_win.startMs) >= CAP_MAX_MS;
      if (end) closeWindow(seq);
    }
    // ---- manual snapshot (POST /api/log/snap)
    uint32_t snapId = 0;
    portENTER_CRITICAL(&g_idMux);
    snapId = g_snapReq;
    g_snapReq = 0;
    portEXIT_CRITICAL(&g_idMux);
    if (snapId) makeManual(snapId, now, seq, t.rpm);
  }

  g_prevSc = sc;
  g_prevFlood = dg.floodTrips;
  g_prevMs = now;
  g_prevRpm = t.rpm > 0 ? (uint32_t)t.rpm : 0;
  g_prevLaunch = t.launchState;
  g_haveSample = true;
  g_flashOpSinceSample = false;
}

static void samplerTask(void*) {
  const TickType_t period = pdMS_TO_TICKS(LOG_SAMPLE_MS);
  TickType_t last = xTaskGetTickCount();
  for (;;) {
    vTaskDelayUntil(&last, period);
    uint32_t now = millis();
    sampleOnce(now);
    flashWork(now);
    TickType_t cur = xTaskGetTickCount();
    if ((TickType_t)(cur - last) > 2 * period) last = cur;   // after an erase / OTA stall: no burst
  }
}

// =========================================================================================
// CSV / JSON FORMATTING
// =========================================================================================
static const char SAMPLE_COLUMNS[] =
    "t_ms,rpm,cut,est,show,switch,armed,inhibit,fresh,launch,reason,cut_slots,fired_slots,meas_age_ms\n";
static const char TRACE_COLUMNS[] = "t_us,kind,period_us,info\n";

static int sampleLine(const LogSample& s, char* b, size_t cap) {
  return snprintf(b, cap, "%lu,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u\n", (unsigned long)s.tMs, (unsigned)s.rpm,
                  (s.flags & SF_CUT) ? 1u : 0u, (s.flags & SF_EST) ? 1u : 0u, (s.flags & SF_SHOW) ? 1u : 0u,
                  (s.flags & SF_SWITCH) ? 1u : 0u, (s.flags & SF_ARMED) ? 1u : 0u, (s.flags & SF_INHIBIT) ? 1u : 0u,
                  (s.flags & SF_FRESH) ? 1u : 0u, (unsigned)(s.state & 3u), (unsigned)((s.state >> 2) & 15u),
                  (unsigned)s.cutSlots, (unsigned)s.firedSlots, (unsigned)s.measAgeMs);
}

// Trace timestamps are the low 32 bits of the microsecond clock; unwrapped against the capture's
// trigger time (ms since boot) they become full microseconds since boot.
static uint64_t unwrapUs(uint32_t tUs, uint32_t refMs) {
  int64_t ref = (int64_t)refMs * 1000;
  int64_t full = (ref & ~(int64_t)0xFFFFFFFF) | (int64_t)tUs;
  if (full - ref > (int64_t)0x80000000LL) full -= (int64_t)0x100000000LL;
  else if (ref - full > (int64_t)0x80000000LL) full += (int64_t)0x100000000LL;
  return full < 0 ? (uint64_t)tUs : (uint64_t)full;
}

static int traceLine(const EngineTraceEvent& e, uint32_t refMs, char* b, size_t cap) {
  return snprintf(b, cap, "%llu,%u,%u,%u\n", (unsigned long long)unwrapUs(e.tUs, refMs), (unsigned)e.kind,
                  (unsigned)e.periodUs, (unsigned)e.info);
}

static void cfgFromSnap(const CfgSnap& c, const char* name, char* b, size_t cap, const char* sep, const char* pre) {
  // pre + key=value (+ sep) for every config key, in PROTOCOL.md order, then the profile
  // (-1 / empty name = unknown, data written before v2.2). profile_name is last: it may contain spaces.
  snprintf(b, cap,
           "%slaunchRpm=%u%s%slaunchDrop=%u%s%sredlineRpm=%u%s%scutPattern=%u%s%smaxCutSeconds=%u.%02u%s"
           "%sdecelPops=%u%s%sdecelRpm=%u%s%sghostCam=%u%s%sarmed=%u%s%sprofile=%d%s%sprofile_name=%s%s",
           pre, c.launchRpm, sep, pre, c.launchDrop, sep, pre, c.redlineRpm, sep, pre, c.cutPattern, sep, pre,
           c.maxCutCs / 100, c.maxCutCs % 100, sep, pre, (c.flags & 2) ? 1u : 0u, sep, pre, c.decelRpm, sep, pre,
           (c.flags & 4) ? 1u : 0u, sep, pre, (c.flags & 1) ? 1u : 0u, sep, pre, (int)c.profile1 - 1, sep, pre,
           c.profile1 ? name : "", sep);
}

static int diagLines(const DiagSnap& d, char* b, size_t cap) {
  return snprintf(b, cap,
                  "# pulses=%lu\n# rejected=%lu\n# discarded=%lu\n# outliers=%lu\n# unsyncs=%lu\n"
                  "# cutSlots=%lu\n# floodTrips=%lu\n# dwellBlocks=%lu\n# launchDropRate=%ld\n",
                  (unsigned long)d.pulses, (unsigned long)d.rejected, (unsigned long)d.discarded,
                  (unsigned long)d.outliers, (unsigned long)d.unsyncs, (unsigned long)d.cutSlots,
                  (unsigned long)d.floodTrips, (unsigned long)d.dwellBlocks, (long)d.launchDropRate);
}

static const char* launchEndCode(uint8_t e) {
  switch (e) {
    case LAUNCH_END_FIRED: return "fired";
    case LAUNCH_END_LIFT: return "lift";
    case LAUNCH_END_HOLD_TIMEOUT: return "hold_timeout";
    case LAUNCH_END_ARM_TIMEOUT: return "arm_timeout";
    case LAUNCH_END_CANCEL: return "cancel";
    case LAUNCH_END_STALL: return "stall";
    default: return "none";
  }
}

// Type-specific summary lines of a capture CSV header.
static int summaryLines(const CapMeta& m, char* b, size_t cap) {
  const int32_t* v = m.sv;
  switch (m.type) {
    case CT_LAUNCH:
      return snprintf(b, cap, "# end=%s\n# hold_min=%ld\n# hold_max=%ld\n# release_ms=%ld\n# hold_ms=%ld\n"
                      "# peak_rpm=%ld\n# drop_rate=%ld\n",
                      launchEndCode(m.launchEnd), (long)v[0], (long)v[1], (long)v[2], (long)v[3], (long)v[4], (long)v[5]);
    case CT_REDLINE:
      return snprintf(b, cap, "# limiter_ms=%ld\n# redline_cut_slots=%ld\n# peak_rpm=%ld\n", (long)v[0], (long)v[1],
                      (long)v[4]);
    case CT_FLOOD:
      return snprintf(b, cap, "# cut_reason=%ld\n# full_cut_ms=%ld\n# peak_rpm=%ld\n", (long)v[0], (long)v[1],
                      (long)v[4]);
    case CT_ANOMALY:
      return snprintf(b, cap, "# window_unsyncs=%ld\n# trace_rejects=%ld\n# trace_outliers=%ld\n# trigger_rpm=%ld\n"
                      "# peak_rpm=%ld\n",
                      (long)v[0], (long)v[1], (long)v[2], (long)v[3], (long)v[4]);
    default:
      return snprintf(b, cap, "# sec=%ld\n# cut_slots_total=%ld\n# fired_slots_total=%ld\n# peak_rpm=%ld\n",
                      (long)v[0], (long)v[1], (long)v[2], (long)v[4]);
  }
}

// Header line i of a capture CSV (-1 = no more lines). Lines are whole "# key=value\n" blocks.
static int capHeaderLine(const CapMeta& m, const char* sum, const char* prof, uint16_t i, char* b, size_t cap) {
  char tmp[400];
  switch (i) {
    case 0:
      return snprintf(b, cap, "# fw=%.12s\n# type=%s\n# id=%lu\n# boot=%lu\n# t0_ms=%lu\n# trigger_ms=%lu\n",
                      m.fw, m.type < CT_COUNT ? CAP_TYPE_NAME[m.type] : "?", (unsigned long)m.id,
                      (unsigned long)m.boot, (unsigned long)m.t0Ms, (unsigned long)m.triggerMs);
    case 1:
      cfgFromSnap(m.cfg, prof, tmp, sizeof(tmp), "\n", "# ");
      return snprintf(b, cap, "%s", tmp);
    case 2: return diagLines(m.diag, b, cap);
    case 3: {
      char also[64] = "";
      for (uint8_t t = 0; t < CT_COUNT; t++) {
        if (!(m.flags & (1u << t)) || t == m.type) continue;
        size_t l = strlen(also);
        snprintf(also + l, sizeof(also) - l, "%s%s", l ? "," : "", CAP_TYPE_NAME[t]);
      }
      return snprintf(b, cap, "# dur_ms=%lu\n# n=%u\n# trace=%u\n# trace_lost=%lu\n# also=%s\n",
                      (unsigned long)m.durMs, (unsigned)m.nSamples, (unsigned)m.nTrace, (unsigned long)m.traceLost,
                      also);
    }
    case 4: return summaryLines(m, b, cap);
    case 5: return snprintf(b, cap, "# sum=%s\n", sum);
    case 6: return snprintf(b, cap, "%s", SAMPLE_COLUMNS);
    default: return -1;
  }
}

static size_t jsonEsc(char* out, size_t cap, const char* in, size_t n) {
  size_t o = 0;
  for (size_t i = 0; i < n && o + 7 < cap; i++) {
    unsigned char c = (unsigned char)in[i];
    if (c == '"' || c == '\\') {
      out[o++] = '\\';
      out[o++] = (char)c;
    } else if (c < 0x20) {
      o += snprintf(out + o, cap - o, "\\u%04x", c);
    } else {
      out[o++] = (char)c;
    }
  }
  out[o] = 0;
  return o;
}

// =========================================================================================
// DOWNLOAD STREAMS (loop task; chunked transfer encoding, non-blocking sends)
// =========================================================================================
static bool isTransient(int e) { return e == EAGAIN || e == EWOULDBLOCK || e == ENOMEM || e == EINTR; }

static void streamClose(LogStream& st) {
  st.client.stop();
  winRelease(st.win);
  st.kind = SK_FREE;
}

static LogStream* streamStart(uint8_t kind, const char* ctype, const char* filename) {
  LogStream* st = nullptr;
  for (int i = 0; i < LOG_STREAMS; i++) {
    if (g_streams[i].kind == SK_FREE) {
      st = &g_streams[i];
      break;
    }
  }
  if (!st) {
    g_srv->sendHeader("Cache-Control", "no-store");
    g_srv->send(503, "application/json; charset=utf-8",
                "{\"ok\":false,\"msg\":\"Már fut két letöltés, próbáld újra később\"}");
    return nullptr;
  }
  WiFiClient c = g_srv->client();
  if (!c) return nullptr;
  st->client = c;
  st->kind = kind;
  st->done = false;
  st->phase = 0;
  st->line = 0;
  st->first = true;
  st->pendHdr = 0;
  st->err = nullptr;
  st->profName[0] = 0;
  st->recItem = st->recItems = 0;
  st->gotN = st->gotNT = 0;
  st->lastProgressMs = millis();
  char disp[96] = "";
  if (filename) snprintf(disp, sizeof(disp), "Content-Disposition: attachment; filename=\"%s\"\r\n", filename);
  int n = snprintf(st->out, sizeof(st->out),
                   "HTTP/1.1 200 OK\r\nContent-Type: %s\r\n%sCache-Control: no-store\r\n"
                   "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n",
                   ctype, disp);
  st->outLen = (uint16_t)(n > 0 && n < (int)sizeof(st->out) ? n : 0);
  st->outPos = 0;
  return st;
}

// Appends a whole line to the chunk data, false if it does not fit.
static bool put(char* dst, size_t cap, size_t& len, const char* line, int n) {
  if (n <= 0) return true;
  if (len + (size_t)n > cap) return false;
  memcpy(dst + len, line, (size_t)n);
  len += (size_t)n;
  return true;
}

// Reads the next record of a capture / drive stream. 1 = record in st.rec, 0 = try later (flash
// busy), -1 = end of the data (or the sector was recycled).
static int streamNextRecord(LogStream& st, bool driveKind) {
  if (!takeMutex(0)) return 0;
  int res = -1;
  for (int guard = 0; guard < 4; guard++) {
    if (st.sec < 0 || (uint32_t)st.sec >= g_nSec || g_secSeq[st.sec] != st.seq) break;   // recycled
    if (nextRecord(st.win, (uint32_t)st.sec, &st.off, st.rec, nullptr, nullptr)) {
      res = 1;
      break;
    }
    // end of this sector: continue in the next one of the same log
    const Region& r = g_rg[driveKind ? LOG_REGION_DRIVE : LOG_REGION_CAP];
    uint16_t ns = regStep(r, (uint32_t)st.sec, 1);
    uint32_t nq = g_secSeq[ns];
    if (!seqValid(nq) || nq <= st.seq || nq < g_clearSeq) break;
    if (driveKind) {
      SectorHdr h;
      if (!flashRead(st.win, (uint32_t)ns * LOG_SECTOR_SIZE, &h, sizeof(h)) || h.boot != st.drv.boot) break;
    }
    st.sec = (int16_t)ns;
    st.seq = nq;
    st.off = LOG_SECTOR_HDR;
  }
  giveMutex();
  return res;
}

static bool genLive(LogStream& st, char* dst, size_t cap, size_t& len) {
  char line[420];
  while (true) {
    if (st.phase == 0) {
      int n;
      TuningConfig c = engineGetConfig();
      CfgSnap cs = makeCfg(c);
      LogSample s0;
      uint32_t t0 = ringGet(st.next, s0) ? s0.tMs : 0;
      switch (st.line) {
        case 0:
          n = snprintf(line, sizeof(line), "# fw=%s\n# type=live\n# boot=%lu\n# t0_ms=%lu\n# sec=%lu\n# ringSec=%lu\n",
                       FW_VERSION, (unsigned long)g_boot, (unsigned long)t0, (unsigned long)st.cursor,
                       (unsigned long)(g_ringCap * LOG_SAMPLE_MS / 1000));
          break;
        case 1: {
          char tmp[400], pn[PROFILE_NAME_BYTES + 1];
          profileActiveName(pn, sizeof(pn));
          cfgFromSnap(cs, pn, tmp, sizeof(tmp), "\n", "# ");
          n = snprintf(line, sizeof(line), "%s", tmp);
          break;
        }
        case 2: n = diagLines(makeDiag(engineGetDiag()), line, sizeof(line)); break;
        case 3: n = snprintf(line, sizeof(line), "%s", SAMPLE_COLUMNS); break;
        default: st.phase = 1; continue;
      }
      if (!put(dst, cap, len, line, n)) return false;
      st.line++;
      continue;
    }
    if (st.next >= st.end) return true;
    LogSample s;
    if (!ringGet(st.next, s)) {   // overwritten while the client was slow
      uint32_t oldest = ringOldest() + LOG_LOSS_MARGIN;
      int n = snprintf(line, sizeof(line), "# gap lost=%lu\n", (unsigned long)(oldest - st.next));
      if (!put(dst, cap, len, line, n)) return false;
      st.next = oldest;
      continue;
    }
    int n = sampleLine(s, line, sizeof(line));
    if (!put(dst, cap, len, line, n)) return false;
    st.next++;
  }
}

static bool genCapture(LogStream& st, char* dst, size_t cap, size_t& len) {
  char line[420];
  while (true) {
    int n;
    if (st.phase == 0) {   // header (from the BEGIN record read at request time)
      n = capHeaderLine(st.meta, st.sum, st.profName, st.line, line, sizeof(line));
      if (n < 0) {
        st.phase = 1;
        continue;
      }
      if (!put(dst, cap, len, line, n)) return false;
      st.line++;
      continue;
    }
    if (st.pendHdr) {   // trace section header (also for a capture without trace events)
      n = snprintf(line, sizeof(line), "# trace\n%s", TRACE_COLUMNS);
      if (!put(dst, cap, len, line, n)) return false;
      st.pendHdr = 0;
      continue;
    }
    if (st.recItem < st.recItems) {
      if (st.recType == REC_CAP_SAMPLES) {
        LogSample s;
        memcpy(&s, st.rec + LOG_REC_HDR + 4 + st.recItem * sizeof(LogSample), sizeof(s));
        n = sampleLine(s, line, sizeof(line));
      } else {
        EngineTraceEvent e;
        memcpy(&e, st.rec + LOG_REC_HDR + 4 + st.recItem * sizeof(EngineTraceEvent), sizeof(e));
        n = traceLine(e, st.meta.triggerMs, line, sizeof(line));
      }
      if (!put(dst, cap, len, line, n)) return false;
      st.recItem++;
      continue;
    }
    if (st.err) {
      n = snprintf(line, sizeof(line), "# error=%s\n", st.err);
      if (!put(dst, cap, len, line, n)) return false;
      st.err = nullptr;
      st.phase = 2;
      continue;
    }
    if (st.phase == 2) return true;
    int r = streamNextRecord(st, false);
    if (r == 0) return false;   // flash busy: continue in the next loop()
    if (r < 0) {
      st.err = "incomplete";
      continue;
    }
    uint8_t type = st.rec[0], plen = st.rec[1];
    uint32_t rid = 0;
    if (plen >= 4) memcpy(&rid, st.rec + LOG_REC_HDR, 4);
    st.recItem = st.recItems = 0;
    if (type == REC_CAP_SAMPLES && rid == st.meta.id && plen >= 16) {
      st.recType = type;
      st.recItems = (plen - 4) / sizeof(LogSample);
      st.gotN += st.recItems;
    } else if (type == REC_CAP_TRACE && rid == st.meta.id && plen >= 12) {
      if (!st.gotNT) st.pendHdr = 1;
      st.recType = type;
      st.recItems = (plen - 4) / sizeof(EngineTraceEvent);
      st.gotNT += st.recItems;
    } else if (type == REC_CAP_END && rid == st.meta.id) {
      if (st.gotN != st.meta.nSamples || st.gotNT != st.meta.nTrace) st.err = "count";
      else if (!st.gotNT) st.pendHdr = 1;   // empty trace section
      st.phase = 2;
    } else {
      st.err = "unexpected record";
    }
  }
}

static bool genDrive(LogStream& st, char* dst, size_t cap, size_t& len) {
  char line[420];
  while (true) {
    int n = 0;
    if (st.phase == 0) {
      switch (st.line) {
        case 0:
          n = snprintf(line, sizeof(line), "# fw=%.12s\n# type=drive\n# boot=%lu\n# t0_ms=%lu\n# dur_ms=%lu\n# n=%lu\n"
                       "# gaps=%u\n",
                       st.haveInfo ? st.info.fw : "?", (unsigned long)st.drv.boot,
                       (unsigned long)(st.drv.t0 == DRV_T0_NONE ? 0 : st.drv.t0),
                       (unsigned long)(st.drv.t0 == DRV_T0_NONE ? 0 : st.drv.tEnd - st.drv.t0),
                       (unsigned long)st.drv.n, (unsigned)st.drv.gaps);
          break;
        case 1:
          if (st.rangeGiven || st.step > 1) {
            n = snprintf(line, sizeof(line), "# from_ms=%lu\n# to_ms=%lu\n# step=%u\n", (unsigned long)st.fromMs,
                         (unsigned long)st.toMs, (unsigned)st.step);
          }
          break;
        case 2:
          if (st.haveInfo) {
            char tmp[400];
            cfgFromSnap(st.info.cfg, st.profName, tmp, sizeof(tmp), "\n", "# ");
            n = snprintf(line, sizeof(line), "%s", tmp);
          }
          break;
        case 3: n = snprintf(line, sizeof(line), "%s", SAMPLE_COLUMNS); break;
        default: st.phase = 1; continue;
      }
      if (!put(dst, cap, len, line, n)) return false;
      st.line++;
      continue;
    }
    if (st.recItem < st.recItems) {   // one line per item; a line that does not fit is retried
      if (st.recType == REC_DRV_SAMPLES) {
        LogSample s;
        memcpy(&s, st.rec + LOG_REC_HDR + st.recItem * sizeof(LogSample), sizeof(s));
        if (s.tMs > st.toMs) return true;   // past the requested range
        bool inRange = s.tMs >= st.fromMs;
        if (inRange && (st.idx % st.step) == 0) {
          n = sampleLine(s, line, sizeof(line));
          if (!put(dst, cap, len, line, n)) return false;
        }
        if (inRange) st.idx++;
      } else if (st.recType == REC_DRV_GAP) {
        DrvGap g;
        memcpy(&g, st.rec + LOG_REC_HDR, sizeof(g));
        if (g.toMs >= st.fromMs && g.fromMs <= st.toMs) {
          n = snprintf(line, sizeof(line), "# gap from_ms=%lu to_ms=%lu lost=%lu\n", (unsigned long)g.fromMs,
                       (unsigned long)g.toMs, (unsigned long)g.lost);
          if (!put(dst, cap, len, line, n)) return false;
        }
      } else if (st.recType == REC_DRV_INFO) {   // config changed during the drive
        DrvInfo inf;
        memcpy(&inf, st.rec + LOG_REC_HDR, sizeof(inf));
        char pn[PROFILE_NAME_BYTES + 1];
        readName(st.rec + LOG_REC_HDR, st.rec[1], sizeof(DrvInfo), pn);
        if (st.haveInfo && (memcmp(&inf.cfg, &st.info.cfg, sizeof(CfgSnap)) != 0 || strcmp(pn, st.profName) != 0)) {
          char tmp[400];
          cfgFromSnap(inf.cfg, pn, tmp, sizeof(tmp), "", " ");
          n = snprintf(line, sizeof(line), "# config%s\n", tmp);
          if (!put(dst, cap, len, line, n)) return false;
        }
        st.info = inf;
        memcpy(st.profName, pn, sizeof(st.profName));
        st.haveInfo = true;
      }
      st.recItem++;
      continue;
    }
    int r = streamNextRecord(st, true);
    if (r == 0) return false;
    if (r < 0) return true;   // end of this drive
    uint8_t type = st.rec[0], plen = st.rec[1];
    st.recType = type;
    st.recItem = 0;
    st.recItems = 0;
    if (type == REC_DRV_SAMPLES && plen >= sizeof(LogSample) && (plen % sizeof(LogSample)) == 0) {
      st.recItems = plen / sizeof(LogSample);
    } else if ((type == REC_DRV_GAP && plen >= sizeof(DrvGap)) || (type == REC_DRV_INFO && plen >= sizeof(DrvInfo))) {
      st.recItems = 1;
    }
  }
}

static bool genList(LogStream& st, char* dst, size_t cap, size_t& len) {
  char line[420];
  while (true) {
    int n = 0;
    if (st.phase == 0) {
      n = snprintf(line, sizeof(line), "{\"ok\":true,\"captures\":[");
      if (!put(dst, cap, len, line, n)) return false;
      st.phase = 1;
      st.cursor = 0xFFFFFFFFu;
      st.first = true;
      continue;
    }
    if (st.phase == 1 || st.phase == 3) {
      if (!takeMutex(0)) return false;
      bool found = false;
      if (st.phase == 1) {   // next capture with id < cursor (newest first)
        int best = -1;
        for (uint16_t i = 0; i < g_capN; i++) {
          if (g_capIdx[i].id < st.cursor && (best < 0 || g_capIdx[i].id > g_capIdx[best].id)) best = i;
        }
        if (best >= 0) {
          found = true;
          CapIdx e = g_capIdx[best];
          char sum[LOG_SUM_MAX + 1] = "";
          uint16_t off = e.beginOff, at;
          if (g_secSeq[e.beginSec] == e.beginSeq &&
              nextRecord(st.win, e.beginSec, &off, st.rec, &at, nullptr) && st.rec[0] == REC_CAP_BEGIN &&
              st.rec[1] >= sizeof(CapMeta)) {
            CapMeta m;
            memcpy(&m, st.rec + LOG_REC_HDR, sizeof(m));
            uint8_t sl = m.sumLen <= st.rec[1] - sizeof(CapMeta) ? m.sumLen : 0;
            memcpy(sum, st.rec + LOG_REC_HDR + sizeof(CapMeta), sl);
            sum[sl] = 0;
          }
          char esc[LOG_SUM_MAX * 2 + 8];
          jsonEsc(esc, sizeof(esc), sum, strlen(sum));
          n = snprintf(line, sizeof(line),
                       "%s{\"id\":%lu,\"boot\":%lu,\"type\":\"%s\",\"t0\":%lu,\"dur\":%lu,\"n\":%u,\"trace\":%u,"
                       "\"sum\":\"%s\"}",
                       st.first ? "" : ",", (unsigned long)e.id, (unsigned long)e.boot,
                       e.type < CT_COUNT ? CAP_TYPE_NAME[e.type] : "?", (unsigned long)e.t0, (unsigned long)e.dur,
                       (unsigned)e.n, (unsigned)e.nTrace, esc);
          if (len + (size_t)n <= cap) st.cursor = e.id;
        }
      } else {   // next drive with boot < cursor
        int best = -1;
        for (uint16_t i = 0; i < g_drvN; i++) {
          if (g_drvIdx[i].boot < st.cursor && (best < 0 || g_drvIdx[i].boot > g_drvIdx[best].boot)) best = i;
        }
        if (best >= 0) {
          found = true;
          DrvIdx d = g_drvIdx[best];
          bool empty = d.t0 == DRV_T0_NONE;
          n = snprintf(line, sizeof(line), "%s{\"boot\":%lu,\"t0\":%lu,\"dur\":%lu,\"n\":%lu,\"gaps\":%u}",
                       st.first ? "" : ",", (unsigned long)d.boot, (unsigned long)(empty ? 0 : d.t0),
                       (unsigned long)(empty ? 0 : d.tEnd - d.t0), (unsigned long)d.n, (unsigned)d.gaps);
          if (len + (size_t)n <= cap) st.cursor = d.boot;
        }
      }
      giveMutex();
      if (found) {
        if (!put(dst, cap, len, line, n)) return false;
        st.first = false;
        continue;
      }
      if (st.phase == 1) {
        n = snprintf(line, sizeof(line), "],\"drives\":[");
        if (!put(dst, cap, len, line, n)) return false;
        st.phase = 3;
        st.cursor = 0xFFFFFFFFu;
        st.first = true;
        continue;
      }
      n = snprintf(line, sizeof(line), "]}");
      if (!put(dst, cap, len, line, n)) return false;
      return true;
    }
    return true;
  }
}

// Fills st.out with the next chunk ("XXXX\r\n" + data + "\r\n", plus the terminator at the end).
static void streamFill(LogStream& st) {
  char* data = st.out + 6;
  const size_t cap = LOG_STREAM_CHUNK;
  size_t len = 0;
  bool finished = false;
  switch (st.kind) {
    case SK_LIVE: finished = genLive(st, data, cap, len); break;
    case SK_CAPTURE: finished = genCapture(st, data, cap, len); break;
    case SK_DRIVE: finished = genDrive(st, data, cap, len); break;
    case SK_LIST: finished = genList(st, data, cap, len); break;
    default: finished = true; break;
  }
  size_t o = 0;
  if (len) {
    static const char hex[] = "0123456789ABCDEF";
    st.out[0] = hex[(len >> 12) & 15];
    st.out[1] = hex[(len >> 8) & 15];
    st.out[2] = hex[(len >> 4) & 15];
    st.out[3] = hex[len & 15];
    st.out[4] = '\r';
    st.out[5] = '\n';
    data[len] = '\r';
    data[len + 1] = '\n';
    o = 6 + len + 2;
  }
  if (finished) {
    memcpy(st.out + o, "0\r\n\r\n", 5);
    o += 5;
    st.done = true;
  }
  st.outLen = (uint16_t)o;
  st.outPos = 0;
}

static void streamPump(LogStream& st, uint32_t now) {
  int fd = st.client.fd();
  if (fd < 0) {
    streamClose(st);
    return;
  }
  for (int i = 0; i < 4; i++) {
    if (st.outPos < st.outLen) {
      ssize_t n = lwip_send(fd, st.out + st.outPos, st.outLen - st.outPos, MSG_DONTWAIT);
      if (n > 0) {
        st.outPos += (uint16_t)n;
        st.lastProgressMs = now;
        continue;
      }
      if (n < 0 && !isTransient(errno)) {
        streamClose(st);
        return;
      }
      break;   // socket buffer full
    }
    if (st.done) {
      streamClose(st);   // everything handed to TCP; close() still delivers it, then FIN
      return;
    }
    streamFill(st);
    if (st.outLen == 0) break;   // generator waits (flash busy)
  }
  if ((now - st.lastProgressMs) > LOG_STREAM_STALL_MS) streamClose(st);
}

// =========================================================================================
// HTTP HANDLERS
// =========================================================================================
static void sendJson(int code, const char* body) {
  g_srv->sendHeader("Cache-Control", "no-store");
  g_srv->send(code, "application/json; charset=utf-8", body);
}

static bool argU32(const char* name, uint32_t& v) {
  if (!g_srv->hasArg(name)) return false;
  String s = g_srv->arg(name);
  if (!s.length()) return false;
  char* end = nullptr;
  unsigned long x = strtoul(s.c_str(), &end, 10);
  if (!end || *end) return false;
  v = (uint32_t)x;
  return true;
}

static void handleStatus() {
  char body[460];
  uint16_t capPool = 0, drvPool = 0;
  if (g_flashOk && g_secSeq) {
    capPool = regPool(g_rg[LOG_REGION_CAP]);
    drvPool = regPool(g_rg[LOG_REGION_DRIVE]);
  }
  bool gapNow = loggingOn() && (g_drvBlocked || (g_lossSeen && millis() - g_lastLossMs < 5000));
  uint32_t backlog = g_head - g_drvNext;
  snprintf(body, sizeof(body),
           "{\"ok\":%s,\"enabled\":%s,\"flash\":%s,\"capacity\":%lu,\"captures\":%u,\"drives\":%u,\"pending\":%u,\"dropped\":%lu,"
           "\"poolFree\":{\"cap\":%u,\"drive\":%u},\"writing\":%s,\"gapNow\":%s,\"heap\":%u,\"boot\":%lu,"
           "\"ringSec\":%lu,\"backlog\":%lu,\"ramBytes\":%lu,\"stackFree\":%u,\"flashErrors\":%lu,"
           "\"scanErrors\":%lu}",
           g_ring ? "true" : "false", g_enabled ? "true" : "false", g_flashOk ? "true" : "false", (unsigned long)(g_part ? g_part->size : 0), (unsigned)g_capN,
           (unsigned)g_drvN, (unsigned)g_pendN, (unsigned long)g_dropped, (unsigned)capPool, (unsigned)drvPool,
           (g_flashOk && g_writeAllowed) ? "true" : "false", gapNow ? "true" : "false", (unsigned)ESP.getFreeHeap(),
           (unsigned long)g_boot, (unsigned long)(g_ringCap * LOG_SAMPLE_MS / 1000),
           (unsigned long)(loggingOn() ? backlog : 0), (unsigned long)g_ramBytes,
           (unsigned)(g_task ? uxTaskGetStackHighWaterMark(g_task) : 0), (unsigned long)g_flashErrors,
           (unsigned long)g_scanErrors);
  sendJson(200, body);
}

static void handleList() {
  if (!g_ring) {
    sendJson(200, "{\"ok\":false,\"msg\":\"A naplózó nem fut\"}");
    return;
  }
  LogStream* st = streamStart(SK_LIST, "application/json; charset=utf-8", nullptr);
  if (st) streamPump(*st, millis());
}

static void handleCapture() {
  uint32_t id = 0;
  if (!argU32("id", id)) {
    sendJson(400, "{\"ok\":false,\"msg\":\"id hiányzik\"}");
    return;
  }
  if (!g_flashOk || !takeMutex(pdMS_TO_TICKS(50))) {
    sendJson(503, "{\"ok\":false,\"msg\":\"A napló most nem olvasható\"}");
    return;
  }
  CapIdx e;
  memset(&e, 0, sizeof(e));
  bool found = false;
  for (uint16_t i = 0; i < g_capN; i++) {
    if (g_capIdx[i].id == id && g_secSeq[g_capIdx[i].beginSec] == g_capIdx[i].beginSeq) {
      e = g_capIdx[i];
      found = true;
      break;
    }
  }
  LogStream* st = nullptr;
  if (found) {
    char fname[48];
    snprintf(fname, sizeof(fname), "swift_%s_%lu.csv", e.type < CT_COUNT ? CAP_TYPE_NAME[e.type] : "capture",
             (unsigned long)id);
    st = streamStart(SK_CAPTURE, "text/csv; charset=utf-8", fname);
    if (st) {
      uint16_t off = e.beginOff, at;
      st->sec = (int16_t)e.beginSec;
      st->seq = e.beginSeq;
      if (nextRecord(st->win, e.beginSec, &off, st->rec, &at, nullptr) && st->rec[0] == REC_CAP_BEGIN &&
          st->rec[1] >= sizeof(CapMeta)) {
        memcpy(&st->meta, st->rec + LOG_REC_HDR, sizeof(CapMeta));
        uint8_t sl = st->meta.sumLen <= st->rec[1] - sizeof(CapMeta) ? st->meta.sumLen : 0;
        memcpy(st->sum, st->rec + LOG_REC_HDR + sizeof(CapMeta), sl);
        st->sum[sl] = 0;
        readName(st->rec + LOG_REC_HDR, st->rec[1], sizeof(CapMeta) + pad4(sl), st->profName);
        st->off = off;
      } else {
        memset(&st->meta, 0, sizeof(st->meta));
        st->sum[0] = 0;
        st->phase = 1;   // no header, just "# error=begin"
        st->err = "begin";
        st->sec = -1;
      }
    }
  }
  giveMutex();
  if (!found) {
    sendJson(404, "{\"ok\":false,\"msg\":\"Nincs ilyen mentés\"}");
    return;
  }
  if (st) streamPump(*st, millis());
}

static void handleDrive() {
  uint32_t boot = 0;
  if (!argU32("boot", boot)) {
    sendJson(400, "{\"ok\":false,\"msg\":\"boot hiányzik\"}");
    return;
  }
  uint32_t from = 0, to = 0xFFFFFFFFu, step = 1;
  bool hasFrom = argU32("from", from), hasTo = argU32("to", to);
  argU32("step", step);
  if (step < 1) step = 1;
  if (step > 1000) step = 1000;
  if (!g_flashOk || !takeMutex(pdMS_TO_TICKS(50))) {
    sendJson(503, "{\"ok\":false,\"msg\":\"A napló most nem olvasható\"}");
    return;
  }
  DrvIdx* d = drvFind(boot);
  LogStream* st = nullptr;
  bool found = d && seqValid(g_secSeq[d->firstSec]) && g_secSeq[d->firstSec] == d->firstSeq;
  if (found) {
    char fname[40];
    snprintf(fname, sizeof(fname), "swift_drive_%lu.csv", (unsigned long)boot);
    st = streamStart(SK_DRIVE, "text/csv; charset=utf-8", fname);
    if (st) {
      st->drv = *d;
      st->fromMs = from;
      st->toMs = to;
      st->step = (uint16_t)step;
      st->idx = 0;
      st->rangeGiven = hasFrom || hasTo;
      st->sec = (int16_t)d->firstSec;
      st->seq = d->firstSeq;
      st->off = LOG_SECTOR_HDR;
      st->haveInfo = false;
      uint16_t off = LOG_SECTOR_HDR, at;   // config / fw from the first DRV_INFO
      if (nextRecord(st->win, d->firstSec, &off, st->rec, &at, nullptr) && st->rec[0] == REC_DRV_INFO &&
          st->rec[1] >= sizeof(DrvInfo)) {
        memcpy(&st->info, st->rec + LOG_REC_HDR, sizeof(DrvInfo));
        readName(st->rec + LOG_REC_HDR, st->rec[1], sizeof(DrvInfo), st->profName);
        st->haveInfo = true;
      }
    }
  }
  giveMutex();
  if (!found) {
    sendJson(404, "{\"ok\":false,\"msg\":\"Nincs ilyen út\"}");
    return;
  }
  if (st) streamPump(*st, millis());
}

static void handleLive() {
  if (!g_ring) {
    sendJson(503, "{\"ok\":false,\"msg\":\"A naplózó nem fut\"}");
    return;
  }
  uint32_t sec = 30;
  argU32("sec", sec);
  uint32_t ringSec = g_ringCap * LOG_SAMPLE_MS / 1000;
  if (sec < 1) sec = 1;
  if (sec > 120) sec = 120;
  if (sec > ringSec) sec = ringSec;
  LogStream* st = streamStart(SK_LIVE, "text/csv; charset=utf-8", "swift_live.csv");
  if (!st) return;
  uint32_t head = g_head;
  uint32_t want = sec * 1000 / LOG_SAMPLE_MS;
  uint32_t oldest = ringOldest();
  st->end = head;
  st->next = head > want ? head - want : 0;
  if (st->next < oldest) st->next = oldest;
  st->cursor = sec;
  streamPump(*st, millis());
}

static void handleSnap() {
  if (!otaRequestAllowed()) {
    sendJson(403, "{\"ok\":false,\"msg\":\"Elutasítva: a kérés egy másik weboldalról érkezett\"}");
    return;
  }
  if (!g_flashOk || !g_task) {
    sendJson(200, "{\"ok\":false,\"msg\":\"Nincs használható napló-partíció\"}");
    return;
  }
  if (!g_enabled) {
    sendJson(200, "{\"ok\":false,\"msg\":\"A naplózás ki van kapcsolva\"}");
    return;
  }
  if (g_pendN >= LOG_MAX_PENDING) {
    sendJson(200, "{\"ok\":false,\"msg\":\"Két mentés már írásra vár, próbáld pár másodperc múlva\"}");
    return;
  }
  uint32_t id;
  portENTER_CRITICAL(&g_idMux);
  if (g_snapReq) {
    id = g_snapReq;
  } else {
    id = g_nextCapId++;
    g_snapReq = id;
  }
  portEXIT_CRITICAL(&g_idMux);
  char body[64];
  snprintf(body, sizeof(body), "{\"ok\":true,\"id\":%lu}", (unsigned long)id);
  sendJson(200, body);
}

// POST /api/log/config  enabled=1|0 (x-www-form-urlencoded). Applies at once; the NVS write
// follows the erase rule (the value stays pending until it is written).
static void handleConfig() {
  if (!otaRequestAllowed()) {
    sendJson(403, "{\"ok\":false,\"msg\":\"Elutasítva: a kérés egy másik weboldalról érkezett\"}");
    return;
  }
  String v = g_srv->hasArg("enabled") ? g_srv->arg("enabled") : String();
  v.trim();
  if (v != "1" && v != "0") {
    sendJson(200, "{\"ok\":false,\"msg\":\"Hibás kérés: enabled = 1 vagy 0\"}");
    return;
  }
  if (!g_task || !takeMutex(pdMS_TO_TICKS(500))) {
    sendJson(200, "{\"ok\":false,\"msg\":\"A naplózó most nem érhető el, próbáld újra\"}");
    return;
  }
  bool en = (v == "1");
  if (en != g_enabled) {
    g_enabled = en;       // flash writes stop / resume at once (the writer checks under this lock)
    g_nvsEnDirty = true;
  }
  giveMutex();
  sendJson(200, en ? "{\"ok\":true,\"enabled\":true}" : "{\"ok\":true,\"enabled\":false}");
}

static void handleClear() {
  if (!otaRequestAllowed()) {
    sendJson(403, "{\"ok\":false,\"msg\":\"Elutasítva: a kérés egy másik weboldalról érkezett\"}");
    return;
  }
  if (!g_flashOk) {
    sendJson(200, "{\"ok\":false,\"msg\":\"Nincs használható napló-partíció\"}");
    return;
  }
  if (!takeMutex(pdMS_TO_TICKS(500))) {
    sendJson(200, "{\"ok\":false,\"msg\":\"A napló foglalt, próbáld újra\"}");
    return;
  }
  g_clearSeq = g_nextSeq;   // every sector written so far is forgotten (re-erased lazily)
  g_capN = 0;
  g_drvN = 0;
  g_clearReq = true;        // sampler: close the heads, drop pending captures, persist the epoch
  giveMutex();
  sendJson(200, "{\"ok\":true}");
}

// =========================================================================================
// SETUP / LOOP
// =========================================================================================
void loggerLoop() {
  uint32_t now = millis();
  for (int i = 0; i < LOG_STREAMS; i++) {
    if (g_streams[i].kind != SK_FREE) streamPump(g_streams[i], now);
  }
}

void loggerBegin(WebServer& server) {
  g_srv = &server;
  uint32_t heap0 = ESP.getFreeHeap();
  uint32_t dyn = 0;
  g_mtx = xSemaphoreCreateMutex();

  uint32_t nvsBoot = 0, nvsCapId = 0;
  {
    Preferences p;
    if (p.begin("swlog", true)) {   // read-only; fails harmlessly on the first boot
      nvsBoot = p.getUInt("boot", 0);
      g_clearSeq = p.getUInt("clr", 0);
      nvsCapId = p.getUInt("capid", 0);
      g_enabled = p.getUChar("en", 1) != 0;
      p.end();
    }
  }

  g_enApplied = g_enabled;

  // Flash: the spiffs data partition, raw
  g_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
  if (g_part && (g_part->encrypted || (g_part->address % LOG_SECTOR_SIZE) || g_part->size < 32 * LOG_SECTOR_SIZE)) {
    Serial.println("[LOG] spiffs partition unusable (encrypted / unaligned / too small): RAM-only");
    g_part = nullptr;
  }
  if (g_part) {
    g_nSec = g_part->size / LOG_SECTOR_SIZE;
    g_capSec = g_nSec / 4 < LOG_CAP_SECTORS ? g_nSec / 4 : LOG_CAP_SECTORS;
    g_secSeq = (uint32_t*)heap_caps_malloc(g_nSec * sizeof(uint32_t), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    g_capIdx = (CapIdx*)heap_caps_malloc(LOG_CAP_INDEX * sizeof(CapIdx), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    g_drvIdx = (DrvIdx*)heap_caps_malloc(LOG_DRV_INDEX * sizeof(DrvIdx), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (!g_secSeq || !g_capIdx || !g_drvIdx || !g_mtx) {
      free(g_secSeq);
      free(g_capIdx);
      free(g_drvIdx);
      g_secSeq = nullptr;
      g_capIdx = nullptr;
      g_drvIdx = nullptr;
      g_part = nullptr;
    } else {
      dyn += g_nSec * sizeof(uint32_t) + LOG_CAP_INDEX * sizeof(CapIdx) + LOG_DRV_INDEX * sizeof(DrvIdx);
      g_rg[LOG_REGION_CAP] = {0, (uint16_t)g_capSec, -1, LOG_SECTOR_SIZE, LOG_CAP_POOL};
      g_rg[LOG_REGION_DRIVE] = {(uint16_t)g_capSec, (uint16_t)(g_nSec - g_capSec), -1, LOG_SECTOR_SIZE, LOG_DRV_POOL};
    }
  }
  if (g_part) {
    for (int i = 0; i < LOG_TRACE_BUFS; i++) {
      g_tb[i] = (TraceBuf*)heap_caps_malloc(sizeof(TraceBuf), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
      if (g_tb[i]) {
        g_tb[i]->used = false;
        dyn += sizeof(TraceBuf);
      }
    }
  }

  // RAM ring, sized last: 120 s unless the heap floor (90 KB) or the RAM budget says otherwise
  const uint32_t staticBytes = sizeof(g_streams) + sizeof(g_pend) + sizeof(g_win) + sizeof(g_wRec) +
                               sizeof(g_traceTmp) + 256;
  int32_t byHeap = (int32_t)ESP.getFreeHeap() - LOG_HEAP_KEEP - LOG_TASK_STACK - 2048;
  int32_t byBudget = (int32_t)LOG_RAM_BUDGET - (int32_t)(staticBytes + dyn + LOG_TASK_STACK);
  int32_t byBlock = (int32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL) - 64;
  int32_t bytes = (int32_t)(LOG_RING_SEC * 1000 / LOG_SAMPLE_MS * sizeof(LogSample));
  if (byHeap < bytes) bytes = byHeap;
  if (byBudget < bytes) bytes = byBudget;
  if (byBlock < bytes) bytes = byBlock;
  uint32_t samples = bytes > 0 ? (uint32_t)bytes / sizeof(LogSample) : 0;
  if (samples >= LOG_RING_MIN_SEC * 1000 / LOG_SAMPLE_MS) {
    g_ring = (LogSample*)heap_caps_malloc(samples * sizeof(LogSample), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
  }
  server.on("/api/log/status", HTTP_GET, handleStatus);
  server.on("/api/log/list", HTTP_GET, handleList);
  server.on("/api/log/capture.csv", HTTP_GET, handleCapture);
  server.on("/api/log/drive.csv", HTTP_GET, handleDrive);
  server.on("/api/log/live.csv", HTTP_GET, handleLive);
  server.on("/api/log/snap", HTTP_POST, handleSnap);
  server.on("/api/log/clear", HTTP_POST, handleClear);
  server.on("/api/log/config", HTTP_POST, handleConfig);

  if (!g_ring) {
    for (int i = 0; i < LOG_TRACE_BUFS; i++) {
      free(g_tb[i]);
      g_tb[i] = nullptr;
    }
    g_part = nullptr;
    Serial.printf("[LOG] not enough RAM for the ring (free heap %u): logger OFF\n", (unsigned)ESP.getFreeHeap());
    return;   // the engine is not affected
  }
  g_ringCap = samples;
  dyn += samples * sizeof(LogSample);

  if (g_part) {
    scanFlash(nvsBoot, nvsCapId);
    g_flashOk = true;
    g_nvsBootDirty = true;   // boot counter (persisted at the first erase-safe moment while enabled)
    g_nvsLastMs = millis();
  }

  if (xTaskCreatePinnedToCore(samplerTask, "logger", LOG_TASK_STACK, nullptr, LOG_TASK_PRIO, &g_task, LOG_TASK_CORE) !=
      pdPASS) {
    g_task = nullptr;
    g_flashOk = false;
    Serial.println("[LOG] sampler task not started: logger OFF");
  }
  g_ramBytes = staticBytes + dyn + LOG_TASK_STACK;


  Serial.printf("[LOG] boot %lu, ring %lu s, RAM %lu B, free heap %u -> %u (min block %u)\n", (unsigned long)g_boot,
                (unsigned long)(g_ringCap * LOG_SAMPLE_MS / 1000), (unsigned long)g_ramBytes, (unsigned)heap0,
                (unsigned)ESP.getFreeHeap(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  if (!g_enabled) Serial.println("[LOG] logging switched OFF (setting): RAM ring only, no flash writes");
  if (g_flashOk) {
    Serial.printf("[LOG] flash %u sectors (capture %u, drive %u), pool cap %u / drive %u, %u captures, %u drives, "
                  "scan %lu ms, %lu bad records\n",
                  (unsigned)g_nSec, (unsigned)g_capSec, (unsigned)(g_nSec - g_capSec),
                  (unsigned)regPool(g_rg[LOG_REGION_CAP]), (unsigned)regPool(g_rg[LOG_REGION_DRIVE]),
                  (unsigned)g_capN, (unsigned)g_drvN, (unsigned long)g_scanMs, (unsigned long)g_scanErrors);
  } else {
    Serial.println("[LOG] no usable log partition: RAM-only (live.csv works, nothing is stored)");
  }
}
