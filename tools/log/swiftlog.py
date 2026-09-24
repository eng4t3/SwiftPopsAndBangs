#!/usr/bin/env python3
"""
Decoder / validator for the data-logger flash format of swift_show_tuning (v2.1).

The firmware (swift_show_tuning/data_logger.cpp, header comment) writes the raw `spiffs`
partition (default.csv: offset 0x290000, 1,441,792 bytes) as two circular logs of 4 KB sectors:
captures (first 96 sectors) and the drive log (the rest). This module mirrors that format 1:1:
it scans a partition dump exactly like the firmware's boot scan and exports the same CSV text as
GET /api/log/capture.csv and /api/log/drive.csv. test_swiftlog.py keeps it in sync with the C++
source (constants, record layout, CSV columns).

Get a dump over USB (the car key can stay off; reading does not disturb anything):
    esptool.py --port COM16 read_flash 0x290000 0x160000 swiftlog.bin

Usage:
    python tools/log/swiftlog.py info     swiftlog.bin            sectors, captures, drives
    python tools/log/swiftlog.py validate swiftlog.bin            CRC / structure check (exit 1 on errors)
    python tools/log/swiftlog.py capture  swiftlog.bin 57 [-o swift_launch_57.csv]
    python tools/log/swiftlog.py drive    swiftlog.bin 12 [--from MS --to MS --step K] [-o out.csv]
    python tools/log/swiftlog.py csvcheck swift_launch_57.csv     check a CSV downloaded from the ESP
Options: --offset 0x290000 when the file is a full flash dump; --clear-seq N to hide what a
"clear" forgot (NVS value swlog/clr; the default 0 shows everything still in flash).
"""
import argparse
import struct
import sys
import zlib
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Tuple

# ---- Format constants (data_logger.cpp) -----------------------------------------------------
LOG_MAGIC = 0x474C5753          # "SWLG"
LOG_FORMAT_VERSION = 1
LOG_SECTOR_SIZE = 4096
LOG_PAGE_SIZE = 256
LOG_SECTOR_HDR = 20
LOG_REC_HDR = 4
LOG_REC_MAX_PAYLOAD = 252
LOG_REGION_CAP = 0
LOG_REGION_DRIVE = 1
LOG_CAP_SECTORS = 96
DRV_SAMPLES_PER_REC = 21
CAP_SAMPLES_PER_REC = 20
CAP_TRACE_PER_REC = 31
LOG_SAMPLE_MS = 40

REC_DRV_INFO = 0x01
REC_DRV_SAMPLES = 0x02
REC_DRV_GAP = 0x03
REC_CAP_BEGIN = 0x10
REC_CAP_SAMPLES = 0x11
REC_CAP_TRACE = 0x12
REC_CAP_END = 0x13

SF_CUT, SF_EST, SF_SHOW, SF_SWITCH, SF_ARMED, SF_INHIBIT, SF_FRESH = 1, 2, 4, 8, 16, 32, 64

CAP_TYPES = ["launch", "redline", "flood", "anomaly", "manual"]
LAUNCH_END_CODE = {1: "fired", 2: "lift", 3: "hold_timeout", 4: "arm_timeout", 5: "cancel", 6: "stall"}

SAMPLE_COLUMNS = "t_ms,rpm,cut,est,show,switch,armed,inhibit,fresh,launch,reason,cut_slots,fired_slots,meas_age_ms"
TRACE_COLUMNS = "t_us,kind,period_us,info"

SAMPLE_FMT = "<IHBBBBH"          # LogSample, 12 bytes
TRACE_FMT = "<IHBB"              # EngineTraceEvent, 8 bytes
SECTOR_HDR_FMT = "<IBBHIII"      # SectorHdr, 20 bytes
CFG_FMT = "<HHHHBBHI"            # CfgSnap, 16 bytes
DIAG_FMT = "<IIIIIIIIi"          # DiagSnap, 36 bytes
CAP_META_HEAD_FMT = "<IIBBBBIIIHHI"   # CapMeta up to traceLost, 32 bytes
CAP_META_SIZE = 120
DRV_INFO_SIZE = 32
CAP_END_FMT = "<IHHI"            # CapEnd, 12 bytes
DRV_GAP_FMT = "<III"             # DrvGap, 12 bytes

SEQ_LIMIT = 0xFFFFFF00

assert struct.calcsize(SAMPLE_FMT) == 12
assert struct.calcsize(TRACE_FMT) == 8
assert struct.calcsize(SECTOR_HDR_FMT) == LOG_SECTOR_HDR
assert struct.calcsize(CFG_FMT) == 16
assert struct.calcsize(DIAG_FMT) == 36
assert struct.calcsize(CAP_META_HEAD_FMT) == 32
assert struct.calcsize(CAP_END_FMT) == 12


# ---- CRC ------------------------------------------------------------------------------------
def crc32(data: bytes, crc: int = 0) -> int:
    """zlib CRC-32, identical to crc32Update() in the firmware (chaining included)."""
    return zlib.crc32(data, crc) & 0xFFFFFFFF


def rec_crc16(rtype: int, payload: bytes) -> int:
    return crc32(bytes([rtype, len(payload)]) + payload) & 0xFFFF


# ---- Data types -------------------------------------------------------------------------------
@dataclass
class Sample:
    t_ms: int
    rpm: int
    flags: int
    state: int
    cut_slots: int
    fired_slots: int
    meas_age_ms: int

    @staticmethod
    def unpack(b: bytes) -> "Sample":
        return Sample(*struct.unpack(SAMPLE_FMT, b))

    def pack(self) -> bytes:
        return struct.pack(SAMPLE_FMT, self.t_ms, self.rpm, self.flags, self.state, self.cut_slots,
                           self.fired_slots, self.meas_age_ms)

    @property
    def launch(self) -> int:
        return self.state & 3

    @property
    def reason(self) -> int:
        return (self.state >> 2) & 15

    def csv(self) -> str:
        f = self.flags
        return "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n" % (
            self.t_ms, self.rpm, 1 if f & SF_CUT else 0, 1 if f & SF_EST else 0, 1 if f & SF_SHOW else 0,
            1 if f & SF_SWITCH else 0, 1 if f & SF_ARMED else 0, 1 if f & SF_INHIBIT else 0,
            1 if f & SF_FRESH else 0, self.launch, self.reason, self.cut_slots, self.fired_slots, self.meas_age_ms)


@dataclass
class TraceEvent:
    t_us: int
    period_us: int
    kind: int
    info: int

    @staticmethod
    def unpack(b: bytes) -> "TraceEvent":
        return TraceEvent(*struct.unpack(TRACE_FMT, b))

    def pack(self) -> bytes:
        return struct.pack(TRACE_FMT, self.t_us, self.period_us, self.kind, self.info)


@dataclass
class Config:
    launchRpm: int = 3800
    launchDrop: int = 400
    redlineRpm: int = 6200
    decelRpm: int = 3200
    cutPattern: int = 1
    armed: bool = True
    decelPops: bool = True
    ghostCam: bool = False
    maxCutCs: int = 300

    @staticmethod
    def unpack(b: bytes) -> "Config":
        lr, ld, rr, dr, pat, fl, mc, _ = struct.unpack(CFG_FMT, b)
        return Config(lr, ld, rr, dr, pat, bool(fl & 1), bool(fl & 2), bool(fl & 4), mc)

    def pack(self) -> bytes:
        fl = (1 if self.armed else 0) | (2 if self.decelPops else 0) | (4 if self.ghostCam else 0)
        return struct.pack(CFG_FMT, self.launchRpm, self.launchDrop, self.redlineRpm, self.decelRpm,
                           self.cutPattern, fl, self.maxCutCs, 0)

    def pairs(self) -> List[Tuple[str, str]]:
        """key=value pairs in the firmware's order (cfgFromSnap)."""
        return [("launchRpm", str(self.launchRpm)), ("launchDrop", str(self.launchDrop)),
                ("redlineRpm", str(self.redlineRpm)), ("cutPattern", str(self.cutPattern)),
                ("maxCutSeconds", "%d.%02d" % (self.maxCutCs // 100, self.maxCutCs % 100)),
                ("decelPops", "1" if self.decelPops else "0"), ("decelRpm", str(self.decelRpm)),
                ("ghostCam", "1" if self.ghostCam else "0"), ("armed", "1" if self.armed else "0")]


DIAG_KEYS = ["pulses", "rejected", "discarded", "outliers", "unsyncs", "cutSlots", "floodTrips",
             "dwellBlocks", "launchDropRate"]


@dataclass
class CapMeta:
    id: int
    boot: int
    type: int
    flags: int
    launchEnd: int
    sumLen: int
    t0Ms: int
    triggerMs: int
    durMs: int
    nSamples: int
    nTrace: int
    traceLost: int
    cfg: Config
    diag: List[int]
    sv: List[int]
    fw: str

    @staticmethod
    def unpack(b: bytes) -> "CapMeta":
        head = struct.unpack(CAP_META_HEAD_FMT, b[:32])
        cfg = Config.unpack(b[32:48])
        diag = list(struct.unpack(DIAG_FMT, b[48:84]))
        sv = list(struct.unpack("<6i", b[84:108]))
        fw = b[108:120].split(b"\0", 1)[0].decode("ascii", "replace")
        return CapMeta(*head, cfg=cfg, diag=diag, sv=sv, fw=fw)

    def pack(self) -> bytes:
        fwb = self.fw.encode("ascii")[:11].ljust(12, b"\0")
        return (struct.pack(CAP_META_HEAD_FMT, self.id, self.boot, self.type, self.flags, self.launchEnd,
                            self.sumLen, self.t0Ms, self.triggerMs, self.durMs, self.nSamples, self.nTrace,
                            self.traceLost)
                + self.cfg.pack() + struct.pack(DIAG_FMT, *self.diag) + struct.pack("<6i", *self.sv) + fwb)


@dataclass
class Capture:
    meta: CapMeta
    sum: str
    samples: List[Sample] = field(default_factory=list)
    trace: List[TraceEvent] = field(default_factory=list)
    complete: bool = False
    sector: int = -1
    offset: int = 0
    problem: str = ""


@dataclass
class Drive:
    boot: int
    sectors: List[int] = field(default_factory=list)
    items: List[Tuple[str, object]] = field(default_factory=list)   # ("info", (fw, Config)) / ("samples", [..]) / ("gap", (from,to,lost))
    n: int = 0
    gaps: int = 0
    t0: Optional[int] = None
    t_end: Optional[int] = None
    fw: str = "?"
    cfg: Optional[Config] = None


@dataclass
class SectorInfo:
    index: int
    region: int
    state: str               # "erased" | "dirty" | "valid"
    seq: int = 0
    boot: int = 0
    bad_records: int = 0


# ---- Record iteration (mirror of nextRecord()) --------------------------------------------------
def iter_records(sec: bytes, start: int = LOG_SECTOR_HDR):
    """Yields (offset, type, payload) of the valid records of one sector and, at the end,
    ("end", resume_offset, bad_record_count)."""
    o = start
    bad = 0
    while o + LOG_REC_HDR <= LOG_SECTOR_SIZE:
        page_start = o & ~(LOG_PAGE_SIZE - 1)
        page_end = page_start + LOG_PAGE_SIZE
        first = page_start if page_start else LOG_SECTOR_HDR
        if o < first:
            o = first
        rtype = sec[o]
        if rtype == 0xFF:
            if o == first:
                yield ("end", o, bad)
                return
            o = page_end
            continue
        plen = sec[o + 1]
        if (plen & 3) or o + LOG_REC_HDR + plen > page_end:
            bad += 1
            o = page_end
            continue
        payload = bytes(sec[o + LOG_REC_HDR:o + LOG_REC_HDR + plen])
        crc = sec[o + 2] | (sec[o + 3] << 8)
        if rec_crc16(rtype, payload) != crc:
            bad += 1
            o = page_end
            continue
        yield (o, rtype, payload)
        o += LOG_REC_HDR + plen
    yield ("end", LOG_SECTOR_SIZE, bad)


def parse_sector_header(sec: bytes, region: int) -> Optional[Tuple[int, int]]:
    magic, ver, reg, _res, seq, boot, crc = struct.unpack(SECTOR_HDR_FMT, bytes(sec[:LOG_SECTOR_HDR]))
    if magic != LOG_MAGIC or ver != LOG_FORMAT_VERSION or reg != region:
        return None
    if not (0 < seq < SEQ_LIMIT) or crc != crc32(bytes(sec[:16])):
        return None
    return seq, boot


# ---- Image scan (mirror of scanFlash()) ---------------------------------------------------------
class LogImage:
    def __init__(self, data: bytes, clear_seq: int = 0, cap_sectors: Optional[int] = None):
        if len(data) % LOG_SECTOR_SIZE:
            raise ValueError("image size is not a multiple of 4096")
        self.data = data
        self.n_sec = len(data) // LOG_SECTOR_SIZE
        self.cap_sec = cap_sectors if cap_sectors is not None else min(self.n_sec // 4, LOG_CAP_SECTORS)
        self.clear_seq = clear_seq
        self.sectors: List[SectorInfo] = []
        self.captures: List[Capture] = []       # complete and incomplete, in flash order
        self.drives: Dict[int, Drive] = {}
        self.heads = {LOG_REGION_CAP: -1, LOG_REGION_DRIVE: -1}
        self.bad_records = 0
        self.max_seq = 0
        self.max_boot = 0
        self.max_cap_id = 0
        self._scan()

    def sector(self, i: int) -> bytes:
        return self.data[i * LOG_SECTOR_SIZE:(i + 1) * LOG_SECTOR_SIZE]

    def region_of(self, i: int) -> int:
        return LOG_REGION_CAP if i < self.cap_sec else LOG_REGION_DRIVE

    def region_range(self, region: int) -> Tuple[int, int]:
        return (0, self.cap_sec) if region == LOG_REGION_CAP else (self.cap_sec, self.n_sec - self.cap_sec)

    def alloc_order(self, region: int) -> List[int]:
        """Sectors oldest first (allocation order: the one after the head ... the head)."""
        first, count = self.region_range(region)
        head = self.heads[region]
        start = first if head < 0 else first + ((head - first + 1) % count)
        return [first + ((start - first + k) % count) for k in range(count)]

    def _scan(self):
        for i in range(self.n_sec):
            sec = self.sector(i)
            region = self.region_of(i)
            hdr = parse_sector_header(sec, region)
            if hdr:
                self.sectors.append(SectorInfo(i, region, "valid", hdr[0], hdr[1]))
                self.max_seq = max(self.max_seq, hdr[0])
                self.max_boot = max(self.max_boot, hdr[1])
            elif sec[:4] == b"\xff\xff\xff\xff" and sec == b"\xff" * LOG_SECTOR_SIZE:
                self.sectors.append(SectorInfo(i, region, "erased"))
            else:
                self.sectors.append(SectorInfo(i, region, "dirty"))
        for region in (LOG_REGION_CAP, LOG_REGION_DRIVE):
            first, count = self.region_range(region)
            best = -1
            for i in range(first, first + count):
                s = self.sectors[i]
                if s.state == "valid" and (best < 0 or s.seq > self.sectors[best].seq):
                    best = i
            self.heads[region] = best
        self._scan_captures()
        self._scan_drives()

    def _scan_captures(self):
        cur: Optional[Capture] = None
        crc = 0
        last_seq = 0
        for i in self.alloc_order(LOG_REGION_CAP):
            info = self.sectors[i]
            if info.state != "valid" or info.seq <= last_seq:
                if cur:
                    cur.problem = cur.problem or "hole in the log"
                cur = None
                continue
            last_seq = info.seq
            live = info.seq >= self.clear_seq
            for rec in iter_records(self.sector(i)):
                if rec[0] == "end":
                    info.bad_records += rec[2]
                    self.bad_records += rec[2]
                    break
                off, rtype, pl = rec
                rid = struct.unpack("<I", pl[:4])[0] if len(pl) >= 4 else 0
                if rtype == REC_CAP_BEGIN and len(pl) >= CAP_META_SIZE:
                    meta = CapMeta.unpack(pl)
                    self.max_cap_id = max(self.max_cap_id, meta.id)
                    self.max_boot = max(self.max_boot, meta.boot)
                    if cur:
                        cur.problem = cur.problem or "no END"
                    cur = None
                    if live and meta.type < len(CAP_TYPES):
                        sl = meta.sumLen if meta.sumLen <= len(pl) - CAP_META_SIZE else 0
                        text = pl[CAP_META_SIZE:CAP_META_SIZE + sl].decode("utf-8", "replace")
                        cur = Capture(meta, text, sector=i, offset=off)
                        self.captures.append(cur)
                        crc = 0
                elif cur is None:
                    continue
                elif rtype == REC_CAP_SAMPLES and len(pl) >= 16 and (len(pl) - 4) % 12 == 0 and rid == cur.meta.id:
                    crc = crc32(pl[4:], crc)
                    cur.samples += [Sample.unpack(pl[k:k + 12]) for k in range(4, len(pl), 12)]
                elif rtype == REC_CAP_TRACE and len(pl) >= 12 and (len(pl) - 4) % 8 == 0 and rid == cur.meta.id:
                    crc = crc32(pl[4:], crc)
                    cur.trace += [TraceEvent.unpack(pl[k:k + 8]) for k in range(4, len(pl), 8)]
                elif rtype == REC_CAP_END and len(pl) >= 12:
                    cid, ns, nt, ccrc = struct.unpack(CAP_END_FMT, pl[:12])
                    m = cur.meta
                    if (cid == m.id and ns == m.nSamples == len(cur.samples) and nt == m.nTrace == len(cur.trace)
                            and ccrc == crc):
                        cur.complete = True
                    else:
                        cur.problem = "END mismatch"
                    cur = None
                else:
                    cur.problem = "foreign record"
                    cur = None
        if cur:
            cur.problem = cur.problem or "no END"

    def _scan_drives(self):
        last_seq = 0
        cur: Optional[Drive] = None
        for i in self.alloc_order(LOG_REGION_DRIVE):
            info = self.sectors[i]
            if info.state != "valid" or info.seq <= last_seq:
                cur = None
                continue
            last_seq = info.seq
            if info.seq < self.clear_seq:
                cur = None
                continue
            if cur is None or cur.boot != info.boot:
                cur = self.drives.setdefault(info.boot, Drive(info.boot))
            cur.sectors.append(i)
            for rec in iter_records(self.sector(i)):
                if rec[0] == "end":
                    info.bad_records += rec[2]
                    self.bad_records += rec[2]
                    break
                _off, rtype, pl = rec
                if rtype == REC_DRV_SAMPLES and len(pl) >= 12 and len(pl) % 12 == 0:
                    ss = [Sample.unpack(pl[k:k + 12]) for k in range(0, len(pl), 12)]
                    cur.items.append(("samples", ss))
                    if cur.t0 is None:
                        cur.t0 = ss[0].t_ms
                    cur.t_end = ss[-1].t_ms
                    cur.n += len(ss)
                elif rtype == REC_DRV_GAP and len(pl) >= 12:
                    cur.items.append(("gap", struct.unpack(DRV_GAP_FMT, pl[:12])))
                    cur.gaps += 1
                elif rtype == REC_DRV_INFO and len(pl) >= DRV_INFO_SIZE:
                    fw = pl[4:16].split(b"\0", 1)[0].decode("ascii", "replace")
                    cfg = Config.unpack(pl[16:32])
                    cur.items.append(("info", (fw, cfg)))
                    if cur.cfg is None:
                        cur.fw, cur.cfg = fw, cfg

    def capture(self, cid: int) -> Optional[Capture]:
        for c in self.captures:
            if c.meta.id == cid and c.complete:
                return c
        return None

    def pool(self, region: int) -> int:
        first, count = self.region_range(region)
        head = self.heads[region]
        s = first if head < 0 else first + ((head - first + 1) % count)
        n = 0
        while n < count - 1 and self.sectors[s].state == "erased":
            n += 1
            s = first + ((s - first + 1) % count)
        return n


# ---- CSV (mirror of the firmware generators) ----------------------------------------------------
def unwrap_us(t_us: int, ref_ms: int) -> int:
    ref = ref_ms * 1000
    full = (ref & ~0xFFFFFFFF) | t_us
    if full - ref > 0x80000000:
        full -= 0x100000000
    elif ref - full > 0x80000000:
        full += 0x100000000
    return t_us if full < 0 else full


def summary_lines(m: CapMeta) -> List[str]:
    v = m.sv
    if m.type == 0:
        return ["end=%s" % LAUNCH_END_CODE.get(m.launchEnd, "none"), "hold_min=%d" % v[0], "hold_max=%d" % v[1],
                "release_ms=%d" % v[2], "hold_ms=%d" % v[3], "peak_rpm=%d" % v[4], "drop_rate=%d" % v[5]]
    if m.type == 1:
        return ["limiter_ms=%d" % v[0], "redline_cut_slots=%d" % v[1], "peak_rpm=%d" % v[4]]
    if m.type == 2:
        return ["cut_reason=%d" % v[0], "full_cut_ms=%d" % v[1], "peak_rpm=%d" % v[4]]
    if m.type == 3:
        return ["window_unsyncs=%d" % v[0], "trace_rejects=%d" % v[1], "trace_outliers=%d" % v[2],
                "trigger_rpm=%d" % v[3], "peak_rpm=%d" % v[4]]
    return ["sec=%d" % v[0], "cut_slots_total=%d" % v[1], "fired_slots_total=%d" % v[2], "peak_rpm=%d" % v[4]]


def capture_csv(c: Capture) -> str:
    m = c.meta
    out = ["# fw=%s\n" % m.fw, "# type=%s\n" % CAP_TYPES[m.type], "# id=%d\n" % m.id, "# boot=%d\n" % m.boot,
           "# t0_ms=%d\n" % m.t0Ms, "# trigger_ms=%d\n" % m.triggerMs]
    out += ["# %s=%s\n" % kv for kv in m.cfg.pairs()]
    out += ["# %s=%d\n" % (k, val) for k, val in zip(DIAG_KEYS, m.diag)]
    also = ",".join(CAP_TYPES[t] for t in range(len(CAP_TYPES)) if (m.flags >> t) & 1 and t != m.type)
    out += ["# dur_ms=%d\n" % m.durMs, "# n=%d\n" % m.nSamples, "# trace=%d\n" % m.nTrace,
            "# trace_lost=%d\n" % m.traceLost, "# also=%s\n" % also]
    out += ["# %s\n" % s for s in summary_lines(m)]
    out.append("# sum=%s\n" % c.sum)
    out.append(SAMPLE_COLUMNS + "\n")
    out += [s.csv() for s in c.samples]
    out.append("# trace\n" + TRACE_COLUMNS + "\n")
    out += ["%d,%d,%d,%d\n" % (unwrap_us(e.t_us, m.triggerMs), e.kind, e.period_us, e.info) for e in c.trace]
    if not c.complete:
        out.append("# error=%s\n" % (c.problem or "incomplete"))
    return "".join(out)


def drive_csv(d: Drive, from_ms: Optional[int] = None, to_ms: Optional[int] = None, step: int = 1) -> str:
    step = max(1, min(1000, step))
    lo = 0 if from_ms is None else from_ms
    hi = 0xFFFFFFFF if to_ms is None else to_ms
    t0 = d.t0 if d.t0 is not None else 0
    dur = (d.t_end - d.t0) if d.t0 is not None else 0
    out = ["# fw=%s\n# type=drive\n# boot=%d\n# t0_ms=%d\n# dur_ms=%d\n# n=%d\n# gaps=%d\n"
           % (d.fw if d.cfg else "?", d.boot, t0, dur, d.n, d.gaps)]
    if from_ms is not None or to_ms is not None or step > 1:
        out.append("# from_ms=%d\n# to_ms=%d\n# step=%d\n" % (lo, hi, step))
    if d.cfg:
        out += ["# %s=%s\n" % kv for kv in d.cfg.pairs()]
    out.append(SAMPLE_COLUMNS + "\n")
    idx = 0
    cfg = d.cfg
    for kind, item in d.items:
        if kind == "samples":
            for s in item:
                if s.t_ms > hi:
                    return "".join(out)
                if s.t_ms >= lo:
                    if idx % step == 0:
                        out.append(s.csv())
                    idx += 1
        elif kind == "gap":
            f, t, lost = item
            if t >= lo and f <= hi:
                out.append("# gap from_ms=%d to_ms=%d lost=%d\n" % (f, t, lost))
        elif kind == "info":
            _fw, c = item
            if cfg is not None and c != cfg:
                out.append("# config " + " ".join("%s=%s" % kv for kv in c.pairs()) + "\n")
            cfg = c
    return "".join(out)


# ---- CSV checker (for files downloaded from the ESP) ---------------------------------------------
REQUIRED_KEYS = ["fw", "type", "boot", "t0_ms"]
CAPTURE_KEYS = ["id", "trigger_ms", "launchRpm", "launchDrop", "redlineRpm", "cutPattern", "maxCutSeconds",
                "decelPops", "decelRpm", "ghostCam", "armed"] + DIAG_KEYS + ["dur_ms", "n", "trace", "sum"]


def check_csv(text: str) -> List[str]:
    """Structural check of a logger CSV. Returns a list of problems (empty = OK)."""
    problems = []
    lines = text.split("\n")
    if lines and lines[-1] == "":
        lines.pop()
    keys: Dict[str, str] = {}
    i = 0
    while i < len(lines) and lines[i].startswith("# "):
        kv = lines[i][2:]
        if "=" in kv:
            k, v = kv.split("=", 1)
            keys[k] = v
        i += 1
    for k in REQUIRED_KEYS:
        if k not in keys:
            problems.append("missing header key %s" % k)
    ctype = keys.get("type", "")
    if ctype in CAP_TYPES:
        for k in CAPTURE_KEYS:
            if k not in keys:
                problems.append("missing capture key %s" % k)
    if i >= len(lines) or lines[i] != SAMPLE_COLUMNS:
        problems.append("sample column line missing or wrong at line %d" % (i + 1))
        return problems
    i += 1
    n = 0
    last_t = -1
    while i < len(lines) and lines[i] != "# trace":
        ln = lines[i]
        i += 1
        if ln.startswith("#"):
            if ln.startswith("# error="):
                problems.append("stream reports %s" % ln[2:])
            continue
        f = ln.split(",")
        if len(f) != 14 or not all(x.isdigit() for x in f):
            problems.append("bad sample line %d: %r" % (i, ln))
            continue
        v = [int(x) for x in f]
        if any(x > 1 for x in v[2:9]) or v[9] > 3 or v[10] > 15:
            problems.append("out-of-range flag in line %d" % i)
        if v[0] < last_t:
            problems.append("t_ms goes backwards in line %d" % i)
        last_t = v[0]
        n += 1
    if ctype in CAP_TYPES:
        if "n" in keys and keys["n"].isdigit() and int(keys["n"]) != n:
            problems.append("header n=%s but %d sample lines" % (keys["n"], n))
        if i >= len(lines):
            problems.append("trace section missing")
            return problems
        i += 1
        if i >= len(lines) or lines[i] != TRACE_COLUMNS:
            problems.append("trace column line missing or wrong")
            return problems
        i += 1
        nt = 0
        while i < len(lines):
            ln = lines[i]
            i += 1
            if ln.startswith("#"):
                if ln.startswith("# error="):
                    problems.append("stream reports %s" % ln[2:])
                continue
            f = ln.split(",")
            if len(f) != 4 or not all(x.isdigit() for x in f) or int(f[1]) > 7:
                problems.append("bad trace line %d: %r" % (i, ln))
                continue
            nt += 1
        if "trace" in keys and keys["trace"].isdigit() and int(keys["trace"]) != nt:
            problems.append("header trace=%s but %d trace lines" % (keys["trace"], nt))
    return problems


# ---- Writer (used by the tests; mirrors the firmware's placement rules) -------------------------------
class LogWriter:
    """Builds partition images the way data_logger.cpp writes them (for tests and experiments)."""

    def __init__(self, n_sec: int = 352, cap_sectors: Optional[int] = None):
        self.img = bytearray(b"\xff" * (n_sec * LOG_SECTOR_SIZE))
        self.n_sec = n_sec
        self.cap_sec = cap_sectors if cap_sectors is not None else min(n_sec // 4, LOG_CAP_SECTORS)
        self.next_seq = 1
        self.head = {LOG_REGION_CAP: -1, LOG_REGION_DRIVE: -1}
        self.off = {LOG_REGION_CAP: LOG_SECTOR_SIZE, LOG_REGION_DRIVE: LOG_SECTOR_SIZE}

    def _range(self, region):
        return (0, self.cap_sec) if region == LOG_REGION_CAP else (self.cap_sec, self.n_sec - self.cap_sec)

    def next_sector(self, region) -> int:
        first, count = self._range(region)
        h = self.head[region]
        return first if h < 0 else first + ((h - first + 1) % count)

    def erase(self, s: int):
        self.img[s * LOG_SECTOR_SIZE:(s + 1) * LOG_SECTOR_SIZE] = b"\xff" * LOG_SECTOR_SIZE

    def open_sector(self, region: int, boot: int) -> int:
        s = self.next_sector(region)
        base = s * LOG_SECTOR_SIZE
        if self.img[base:base + LOG_SECTOR_SIZE] != b"\xff" * LOG_SECTOR_SIZE:
            self.erase(s)   # the firmware erases ahead of time (pool); same result
        hdr16 = struct.pack("<IBBHII", LOG_MAGIC, LOG_FORMAT_VERSION, region, 0, self.next_seq, boot)
        self.img[base:base + LOG_SECTOR_HDR] = hdr16 + struct.pack("<I", crc32(hdr16))
        self.next_seq += 1
        self.head[region] = s
        self.off[region] = LOG_SECTOR_HDR
        return s

    def room(self, region: int, min_payload: int) -> int:   # regRoom()
        if self.head[region] < 0 or self.off[region] >= LOG_SECTOR_SIZE:
            return 0
        o = self.off[region]
        here = LOG_PAGE_SIZE - (o & (LOG_PAGE_SIZE - 1))
        if here >= LOG_REC_HDR + min_payload:
            return here - LOG_REC_HDR
        nxt = (o & ~(LOG_PAGE_SIZE - 1)) + LOG_PAGE_SIZE
        return LOG_PAGE_SIZE - LOG_REC_HDR if nxt < LOG_SECTOR_SIZE else 0

    def place(self, region: int, length: int) -> int:     # regPlace()
        o = self.off[region]
        if (o & (LOG_PAGE_SIZE - 1)) + length > LOG_PAGE_SIZE:
            o = (o & ~(LOG_PAGE_SIZE - 1)) + LOG_PAGE_SIZE
        return o if o + length <= LOG_SECTOR_SIZE else 0

    def write_rec(self, region: int, rtype: int, payload: bytes) -> Tuple[int, int]:
        assert len(payload) % 4 == 0 and len(payload) <= LOG_REC_MAX_PAYLOAD
        o = self.place(region, LOG_REC_HDR + len(payload))
        assert o, "record does not fit"
        crc = rec_crc16(rtype, payload)
        base = self.head[region] * LOG_SECTOR_SIZE + o
        rec = bytes([rtype, len(payload), crc & 0xFF, crc >> 8]) + payload
        assert (o // LOG_PAGE_SIZE) == ((o + len(rec) - 1) // LOG_PAGE_SIZE), "crosses a page"
        self.img[base:base + len(rec)] = rec
        self.off[region] = o + len(rec)
        return self.head[region], o

    def ensure(self, region: int, need: int, boot: int, on_open=None):
        if self.room(region, need) < need:
            self.open_sector(region, boot)
            if on_open:
                on_open()

    # -- drive log (driveStep with an unlimited backlog)
    def write_drive(self, boot: int, samples: List[Sample], cfg: Config, fw: str = "2.1.0",
                    gaps: Optional[Dict[int, Tuple[int, int, int]]] = None, cfg_changes=None):
        """gaps: {sample index: (from, to, lost)} written before that sample; cfg_changes:
        {sample index: Config} taking effect in the next sector opened after that index."""
        gaps = gaps or {}
        cfg_changes = cfg_changes or {}
        state = {"cfg": cfg}
        info = lambda: struct.pack("<I", boot) + fw.encode()[:11].ljust(12, b"\0") + state["cfg"].pack()
        self.off[LOG_REGION_DRIVE] = LOG_SECTOR_SIZE   # a boot starts a new sector
        i = 0
        while i < len(samples):
            if i in cfg_changes:
                state["cfg"] = cfg_changes.pop(i)
            if i in gaps:
                self.ensure(LOG_REGION_DRIVE, 12, boot,
                            lambda: self.write_rec(LOG_REGION_DRIVE, REC_DRV_INFO, info()))
                self.write_rec(LOG_REGION_DRIVE, REC_DRV_GAP, struct.pack(DRV_GAP_FMT, *gaps.pop(i)))
            self.ensure(LOG_REGION_DRIVE, 12, boot, lambda: self.write_rec(LOG_REGION_DRIVE, REC_DRV_INFO, info()))
            room = self.room(LOG_REGION_DRIVE, 12)
            k = min(DRV_SAMPLES_PER_REC, room // 12, len(samples) - i)
            for j in range(i + 1, i + k):          # keep gap positions exact
                if j in gaps or j in cfg_changes:
                    k = j - i
                    break
            self.write_rec(LOG_REGION_DRIVE, REC_DRV_SAMPLES, b"".join(s.pack() for s in samples[i:i + k]))
            i += k

    # -- capture (captureStep)
    def write_capture(self, meta: CapMeta, summary: str, samples: List[Sample], trace: List[TraceEvent],
                      boot: int, stop_after: Optional[int] = None) -> Tuple[int, int]:
        """stop_after = number of records to write (simulates a power loss)."""
        sb = summary.encode("utf-8")[:132]
        meta.sumLen = len(sb)
        meta.nSamples = len(samples)
        meta.nTrace = len(trace)
        payload = meta.pack() + sb + b"\0" * ((4 - len(sb) % 4) % 4)
        written = 0

        def budget():
            return stop_after is None or written < stop_after

        self.ensure(LOG_REGION_CAP, len(payload), boot)
        at = self.write_rec(LOG_REGION_CAP, REC_CAP_BEGIN, payload)
        written += 1
        crc = 0
        idb = struct.pack("<I", meta.id)
        i = 0
        while i < len(samples) and budget():
            self.ensure(LOG_REGION_CAP, 16, boot)
            k = min(CAP_SAMPLES_PER_REC, (self.room(LOG_REGION_CAP, 16) - 4) // 12, len(samples) - i)
            data = b"".join(s.pack() for s in samples[i:i + k])
            self.write_rec(LOG_REGION_CAP, REC_CAP_SAMPLES, idb + data)
            crc = crc32(data, crc)
            i += k
            written += 1
        i = 0
        while i < len(trace) and budget():
            self.ensure(LOG_REGION_CAP, 12, boot)
            k = min(CAP_TRACE_PER_REC, (self.room(LOG_REGION_CAP, 12) - 4) // 8, len(trace) - i)
            data = b"".join(e.pack() for e in trace[i:i + k])
            self.write_rec(LOG_REGION_CAP, REC_CAP_TRACE, idb + data)
            crc = crc32(data, crc)
            i += k
            written += 1
        if budget():
            self.ensure(LOG_REGION_CAP, 12, boot)
            self.write_rec(LOG_REGION_CAP, REC_CAP_END,
                           struct.pack(CAP_END_FMT, meta.id, len(samples), len(trace), crc))
        return at


# ---- CLI --------------------------------------------------------------------------------------------
def load(path: str, offset: int, clear_seq: int) -> LogImage:
    with open(path, "rb") as f:
        data = f.read()
    if offset:
        data = data[offset:offset + 0x160000]
    return LogImage(data, clear_seq=clear_seq)


def cmd_info(img: LogImage):
    st = {"valid": 0, "erased": 0, "dirty": 0}
    for s in img.sectors:
        st[s.state] += 1
    print("sectors: %d (capture %d, drive %d)  valid %d, erased %d, dirty %d, bad records %d"
          % (img.n_sec, img.cap_sec, img.n_sec - img.cap_sec, st["valid"], st["erased"], st["dirty"],
             img.bad_records))
    print("max seq %d, max boot %d, next capture id %d, pool: capture %d, drive %d"
          % (img.max_seq, img.max_boot, img.max_cap_id + 1, img.pool(LOG_REGION_CAP), img.pool(LOG_REGION_DRIVE)))
    print("\ncaptures (newest first):")
    for c in sorted(img.captures, key=lambda c: -c.meta.id):
        m = c.meta
        print("  id %4d boot %4d %-8s t0 %9d ms dur %6d ms n %4d trace %4d %s  %s"
              % (m.id, m.boot, CAP_TYPES[m.type], m.t0Ms, m.durMs, len(c.samples), len(c.trace),
                 "OK " if c.complete else "BAD(%s)" % c.problem, c.sum))
    print("\ndrives (newest first):")
    for b in sorted(img.drives, reverse=True):
        d = img.drives[b]
        dur = (d.t_end - d.t0) if d.t0 is not None else 0
        print("  boot %4d  t0 %9s ms  dur %8d ms  n %6d  gaps %d  sectors %d  fw %s"
              % (b, d.t0, dur, d.n, d.gaps, len(d.sectors), d.fw))


def cmd_validate(img: LogImage) -> int:
    errors = img.bad_records
    for c in img.captures:
        if not c.complete:
            print("capture %d (sector %d +%d): %s" % (c.meta.id, c.sector, c.offset, c.problem or "incomplete"))
            errors += 1
    dirty = [s.index for s in img.sectors if s.state == "dirty"]
    if dirty:
        print("dirty sectors (not erased, no valid header; normal on the first boot or after a torn write): %s"
              % dirty[:40])
    print("bad records: %d, incomplete captures: %d" % (img.bad_records, sum(1 for c in img.captures if not c.complete)))
    return 1 if errors else 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", choices=["info", "validate", "capture", "drive", "csvcheck"])
    ap.add_argument("file")
    ap.add_argument("ident", nargs="?", type=int, help="capture id / drive boot")
    ap.add_argument("-o", "--out")
    ap.add_argument("--offset", type=lambda s: int(s, 0), default=0)
    ap.add_argument("--clear-seq", type=int, default=0)
    ap.add_argument("--from", dest="from_ms", type=int)
    ap.add_argument("--to", dest="to_ms", type=int)
    ap.add_argument("--step", type=int, default=1)
    a = ap.parse_args(argv)
    if a.cmd == "csvcheck":
        with open(a.file, encoding="utf-8") as f:
            probs = check_csv(f.read())
        for p in probs:
            print(p)
        print("OK" if not probs else "%d problem(s)" % len(probs))
        return 1 if probs else 0
    img = load(a.file, a.offset, a.clear_seq)
    if a.cmd == "info":
        cmd_info(img)
        return 0
    if a.cmd == "validate":
        return cmd_validate(img)
    if a.ident is None:
        ap.error("capture id / drive boot missing")
    if a.cmd == "capture":
        c = next((c for c in img.captures if c.meta.id == a.ident), None)
        if c is None:
            print("no capture %d" % a.ident)
            return 1
        text = capture_csv(c)
    else:
        d = img.drives.get(a.ident)
        if d is None:
            print("no drive %d" % a.ident)
            return 1
        text = drive_csv(d, a.from_ms, a.to_ms, a.step)
    if a.out:
        with open(a.out, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
