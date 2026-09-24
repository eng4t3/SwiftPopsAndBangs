#!/usr/bin/env python3
"""
Tests for the data-logger format (python tools/log/test_swiftlog.py, or python -m unittest).

There is no host C++ compiler on the development machine, so the firmware's encoder cannot run
here. These tests instead
  * parse swift_show_tuning/data_logger.cpp and check that every format constant, struct size,
    flag bit, record type, CSV column line and the CRC nibble table match this decoder (so the two
    cannot drift apart silently), and that the CSV columns match docs/PROTOCOL.md;
  * exercise the record / sector / CSV logic through LogWriter, which mirrors the firmware's
    placement rules (regRoom / regPlace / openSector / driveStep / captureStep).
"""
import re
import struct
import sys
import unittest
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import swiftlog as sl  # noqa: E402

ROOT = HERE.parents[1]
CPP = (ROOT / "swift_show_tuning" / "data_logger.cpp").read_text(encoding="utf-8")
PROTOCOL = (ROOT / "docs" / "PROTOCOL.md").read_text(encoding="utf-8")
ENGINE_H = (ROOT / "swift_show_tuning" / "engine_control.h").read_text(encoding="utf-8")


def cpp_define(name):
    m = re.search(r"^#define\s+%s\s+(\S+)" % name, CPP, re.M)
    if not m:
        raise AssertionError("#define %s not found" % name)
    return int(m.group(1).rstrip("uU"), 0)


def cpp_enum(name):
    m = re.search(r"\b%s\s*=\s*(0x[0-9A-Fa-f]+|\d+)" % name, CPP)
    if not m:
        raise AssertionError("enum %s not found" % name)
    return int(m.group(1), 0)


def cpp_sizeof(struct_name):
    m = re.search(r"static_assert\(sizeof\(%s\)\s*==\s*(\w+)" % struct_name, CPP)
    if not m:
        raise AssertionError("static_assert(sizeof(%s)) not found" % struct_name)
    v = m.group(1)
    return int(v) if v.isdigit() else cpp_define(v)


def cpp_string(name):
    m = re.search(r"static const char %s\[\]\s*=\s*((?:\s*\"(?:[^\"\\]|\\.)*\")+)\s*;" % name, CPP)
    if not m:
        raise AssertionError("string %s not found" % name)
    parts = re.findall(r"\"((?:[^\"\\]|\\.)*)\"", m.group(1))
    return "".join(parts).encode().decode("unicode_escape")


def cpp_function(name):
    """Body of a C++ function of data_logger.cpp (brace matched)."""
    m = re.search(r"^static [^\n;]*\b%s\([^)]*\)\s*\{" % name, CPP, re.M)
    if not m:
        raise AssertionError("function %s not found" % name)
    depth, i = 1, m.end()
    while depth:
        c = CPP[i]
        depth += (c == "{") - (c == "}")
        i += 1
    return CPP[m.end():i - 1]


def nibble_crc(data, crc=0):
    """Line-by-line Python copy of crc32Update() in data_logger.cpp."""
    table = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{8})u", CPP.split("kCrcNib[16] = {", 1)[1][:400])]
    assert len(table) >= 16
    table = table[:16]
    crc = ~crc & 0xFFFFFFFF
    for b in data:
        crc ^= b
        crc = (crc >> 4) ^ table[crc & 15]
        crc = (crc >> 4) ^ table[crc & 15]
    return ~crc & 0xFFFFFFFF


def mk_samples(n, t0=1000, rpm=850, step=40, **kw):
    out = []
    for i in range(n):
        out.append(sl.Sample(t0 + i * step, rpm + (i % 7), kw.get("flags", sl.SF_ARMED | sl.SF_FRESH),
                             kw.get("state", 0), kw.get("cut", 0), kw.get("fired", 1), kw.get("age", 12)))
    return out


def mk_meta(cid, ctype=0, boot=3, trig=5000):
    return sl.CapMeta(id=cid, boot=boot, type=ctype, flags=0, launchEnd=1, sumLen=0, t0Ms=1000, triggerMs=trig,
                      durMs=0, nSamples=0, nTrace=0, traceLost=0, cfg=sl.Config(), diag=list(range(1, 10)),
                      sv=[3752, 3880, 118, 2500, 3950, -12000], fw="2.1.0")


class FirmwareParity(unittest.TestCase):
    def test_crc_table_and_check_value(self):
        # the firmware's nibble table reproduces zlib's CRC-32, incl. chaining
        for data in [b"", b"123456789", bytes(range(256)) * 3]:
            self.assertEqual(nibble_crc(data), zlib.crc32(data) & 0xFFFFFFFF)
        self.assertEqual(nibble_crc(b"456789", nibble_crc(b"123")), zlib.crc32(b"123456789"))
        self.assertIn("0xCBF43926u", CPP)   # compile-time check value in the firmware

    def test_constants(self):
        for name in ["LOG_MAGIC", "LOG_FORMAT_VERSION", "LOG_SECTOR_SIZE", "LOG_PAGE_SIZE", "LOG_SECTOR_HDR",
                     "LOG_REC_HDR", "LOG_REC_MAX_PAYLOAD", "LOG_REGION_CAP", "LOG_REGION_DRIVE", "LOG_CAP_SECTORS",
                     "DRV_SAMPLES_PER_REC", "CAP_SAMPLES_PER_REC", "CAP_TRACE_PER_REC", "LOG_SAMPLE_MS"]:
            self.assertEqual(cpp_define(name), getattr(sl, name), name)
        for name in ["REC_DRV_INFO", "REC_DRV_SAMPLES", "REC_DRV_GAP", "REC_CAP_BEGIN", "REC_CAP_SAMPLES",
                     "REC_CAP_TRACE", "REC_CAP_END", "SF_CUT", "SF_EST", "SF_SHOW", "SF_SWITCH", "SF_ARMED",
                     "SF_INHIBIT", "SF_FRESH", "CT_LAUNCH", "CT_REDLINE", "CT_FLOOD", "CT_ANOMALY", "CT_MANUAL"]:
            want = getattr(sl, name) if hasattr(sl, name) else sl.CAP_TYPES.index(name[3:].lower())
            self.assertEqual(cpp_enum(name), want, name)

    def test_struct_sizes(self):
        self.assertEqual(cpp_sizeof("LogSample"), struct.calcsize(sl.SAMPLE_FMT))
        self.assertEqual(cpp_sizeof("SectorHdr"), struct.calcsize(sl.SECTOR_HDR_FMT))
        self.assertEqual(cpp_sizeof("CfgSnap"), struct.calcsize(sl.CFG_FMT))
        self.assertEqual(cpp_sizeof("DiagSnap"), struct.calcsize(sl.DIAG_FMT))
        self.assertEqual(cpp_sizeof("CapMeta"), sl.CAP_META_SIZE)
        self.assertEqual(cpp_sizeof("DrvInfo"), sl.DRV_INFO_SIZE)
        self.assertEqual(cpp_sizeof("CapEnd"), struct.calcsize(sl.CAP_END_FMT))
        self.assertEqual(cpp_sizeof("EngineTraceEvent"), struct.calcsize(sl.TRACE_FMT))
        # field order of the 12-byte sample (C++ struct body)
        body = CPP.split("struct LogSample {", 1)[1].split("};", 1)[0]
        fields = re.findall(r"^\s*(uint\d+_t)\s+(\w+);", body, re.M)
        self.assertEqual([f[1] for f in fields], ["tMs", "rpm", "flags", "state", "cutSlots", "firedSlots", "measAgeMs"])
        self.assertEqual("".join({"uint32_t": "I", "uint16_t": "H", "uint8_t": "B"}[f[0]] for f in fields), "IHBBBBH")
        # CapMeta field order up to traceLost
        body = CPP.split("struct CapMeta {", 1)[1].split("};", 1)[0]
        order = re.findall(r"\b(id|boot|type|flags|launchEnd|sumLen|t0Ms|triggerMs|durMs|nSamples|nTrace|traceLost|"
                           r"cfg|diag|sv|fw)\b", re.sub(r"//.*", "", body))
        self.assertEqual(order, ["id", "boot", "type", "flags", "launchEnd", "sumLen", "t0Ms", "triggerMs", "durMs",
                                 "nSamples", "nTrace", "traceLost", "cfg", "diag", "sv", "fw"])

    def test_trace_event_layout_matches_engine_header(self):
        body = ENGINE_H.split("struct EngineTraceEvent {", 1)[1].split("};", 1)[0]
        types = re.findall(r"^\s*(uint\d+_t)\s+\w+;", body, re.M)
        self.assertEqual("".join({"uint32_t": "I", "uint16_t": "H", "uint8_t": "B"}[t] for t in types), "IHBB")
        self.assertEqual(sl.TRACE_FMT, "<IHBB")

    def test_csv_columns(self):
        self.assertEqual(cpp_string("SAMPLE_COLUMNS"), sl.SAMPLE_COLUMNS + "\n")
        self.assertEqual(cpp_string("TRACE_COLUMNS"), sl.TRACE_COLUMNS + "\n")
        self.assertIn("`%s`" % sl.SAMPLE_COLUMNS, PROTOCOL)
        self.assertIn("`%s`" % sl.TRACE_COLUMNS, PROTOCOL)

    def test_csv_header_keys_match_firmware(self):
        """Key order of the capture / drive CSV headers: C++ format strings vs this decoder."""
        cap_hdr = cpp_function("capHeaderLine")
        case0 = cap_hdr.split("case 0:", 1)[1].split("case 1:", 1)[0]
        case3 = cap_hdr.split("case 3:", 1)[1].split("case 4:", 1)[0]
        cfg_keys = re.findall(r"%s(\w+)=", cpp_function("cfgFromSnap"))
        diag_keys = re.findall(r"# (\w+)=", cpp_function("diagLines"))
        self.assertEqual(cfg_keys, [k for k, _ in sl.Config().pairs()])
        self.assertEqual(diag_keys, sl.DIAG_KEYS)
        summ = cpp_function("summaryLines")
        blocks = re.split(r"case CT_\w+:|default:", summ)[1:]
        self.assertEqual(len(blocks), 5)
        for ctype in range(5):
            want = (re.findall(r"# (\w+)=", case0) + cfg_keys + diag_keys + re.findall(r"# (\w+)=", case3)
                    + re.findall(r"# (\w+)=", blocks[ctype]) + ["sum"])
            m = mk_meta(1, ctype=ctype)
            text = sl.capture_csv(sl.Capture(m, "x", complete=True))
            got = [ln[2:].split("=", 1)[0] for ln in text.split("\n") if ln.startswith("# ") and "=" in ln]
            self.assertEqual(got, want, sl.CAP_TYPES[ctype])
        drv = cpp_function("genDrive")
        want = re.findall(r"# (\w+)=", drv.split("case 0:", 1)[1].split("case 2:", 1)[0])
        d = sl.Drive(5, fw="2.1.0", cfg=sl.Config(), t0=0, t_end=0)
        got = [ln[2:].split("=", 1)[0] for ln in sl.drive_csv(d, 0, 10, 2).split("\n") if ln.startswith("# ")]
        self.assertEqual(got, want + cfg_keys)
        self.assertIn('"# gap from_ms=%lu to_ms=%lu lost=%lu\\n"', drv)
        self.assertIn('"# config%s\\n"', drv)

    def test_routes_and_status_keys_documented(self):
        routes = re.findall(r'server\.on\("(/api/log/[\w./]+)", HTTP_(GET|POST)', CPP)
        self.assertEqual(len(routes), 8)
        for path, method in routes:
            self.assertIn("`%s %s" % (method, path), PROTOCOL, path)
        status = cpp_function("handleStatus")
        keys = re.findall(r'\\"(\w+)\\":', status)
        self.assertIn("enabled", keys)
        doc = PROTOCOL.split("`GET /api/log/status`", 1)[1].splitlines()[0]
        for k in keys:
            self.assertIn('"%s":' % k, doc, k)
        self.assertIn('"A naplózás ki van kapcsolva"', PROTOCOL)
        self.assertIn('\\"A naplózás ki van kapcsolva\\"', CPP)

    def test_capture_type_names(self):
        m = re.search(r"CAP_TYPE_NAME\[CT_COUNT\]\s*=\s*\{([^}]*)\}", CPP)
        self.assertEqual(re.findall(r"\"(\w+)\"", m.group(1)), sl.CAP_TYPES)
        for t in sl.CAP_TYPES:
            self.assertIn("`%s`" % t, PROTOCOL)


class Records(unittest.TestCase):
    def test_no_record_crosses_a_page_and_capacity(self):
        w = sl.LogWriter()
        w.write_drive(7, mk_samples(5000), sl.Config())
        img = sl.LogImage(bytes(w.img))
        d = img.drives[7]
        self.assertEqual(d.n, 5000)
        # full drive sector: DRV_INFO + 16 samples in page 0, then 15 pages x 21
        per_sector = 16 + 15 * 21
        self.assertEqual(len(d.sectors), -(-5000 // per_sector))
        drive_sectors = img.n_sec - img.cap_sec
        minutes = drive_sectors * per_sector * sl.LOG_SAMPLE_MS / 60000.0
        self.assertGreater(minutes, 56.0)   # raw capacity of the drive region (~56.5 min)
        for s in d.sectors:   # page rule (the writer asserts it too)
            for rec in sl.iter_records(img.sector(s)):
                if rec[0] == "end":
                    break
                off, _t, pl = rec
                self.assertEqual(off // 256, (off + 4 + len(pl) - 1) // 256)

    def test_torn_record_skips_to_next_page(self):
        w = sl.LogWriter()
        w.write_drive(2, mk_samples(100), sl.Config())
        img = bytearray(w.img)
        s = sl.LogImage(bytes(img)).drives[2].sectors[0]
        base = s * 4096
        # corrupt the second record (first sample record is at offset 56 after DRV_INFO at 20)
        img[base + 56 + 10] ^= 0x5A
        li = sl.LogImage(bytes(img))
        self.assertEqual(li.bad_records, 1)
        self.assertEqual(li.drives[2].n, 100 - 16)   # the 16 samples of the rest of page 0 are lost

    def test_bad_header_is_dirty_and_blank_is_erased(self):
        w = sl.LogWriter()
        w.write_drive(2, mk_samples(10), sl.Config())
        img = bytearray(w.img)
        s = sl.LogImage(bytes(img)).drives[2].sectors[0]
        img[s * 4096 + 8] ^= 1          # seq bit -> header CRC fails
        img[5 * 4096 + 100] = 0         # a non-blank capture sector
        li = sl.LogImage(bytes(img))
        self.assertEqual(li.sectors[s].state, "dirty")
        self.assertEqual(li.sectors[5].state, "dirty")
        self.assertEqual(li.sectors[6].state, "erased")
        self.assertNotIn(2, li.drives)

    def test_wrap_order_and_clear_epoch(self):
        w = sl.LogWriter(n_sec=42)                         # capture 42 // 4 = 10, drive 32 sectors (firmware rule)
        per_sector = 16 + 15 * 21
        for boot in range(1, 12):                          # 11 drives x 5 sectors > 32: the log wraps
            w.write_drive(boot, mk_samples(5 * per_sector, t0=0), sl.Config())
        li = sl.LogImage(bytes(w.img))
        # 55 sectors written into 32: boots 1-4 overwritten, boot 5 keeps its newest 2 sectors
        self.assertEqual(sorted(li.drives), list(range(5, 12)))
        self.assertEqual(li.drives[5].n, 2 * per_sector)
        self.assertEqual(li.drives[5].t0, 3 * per_sector * 40)
        self.assertTrue(all(li.drives[b].n == 5 * per_sector for b in range(6, 12)))
        head = li.heads[sl.LOG_REGION_DRIVE]
        self.assertEqual(li.sectors[head].boot, 11)
        # clear epoch = seq of the first sector of boot 10: boots below are forgotten
        first10 = li.drives[10].sectors[0]
        li2 = sl.LogImage(bytes(w.img), clear_seq=li.sectors[first10].seq)
        self.assertEqual(sorted(li2.drives), [10, 11])


class Captures(unittest.TestCase):
    def _img_with(self, n_samples=205, n_trace=612, stop_after=None):
        w = sl.LogWriter()
        m = mk_meta(57)
        tr = [sl.TraceEvent((4_000_000_000 + i * 997) & 0xFFFFFFFF, 7894, i % 8, i & 0xFF) for i in range(n_trace)]
        w.write_capture(m, "FIRED, tartás 3752–3880 RPM, kioldás 118 ms", mk_samples(n_samples, t0=900),
                        tr, boot=3, stop_after=stop_after)
        return sl.LogImage(bytes(w.img)), w

    def test_roundtrip_and_csv(self):
        li, _ = self._img_with()
        c = li.capture(57)
        self.assertIsNotNone(c)
        self.assertEqual((len(c.samples), len(c.trace)), (205, 612))
        text = sl.capture_csv(c)
        self.assertEqual(sl.check_csv(text), [])
        lines = text.split("\n")
        keys = [ln[2:].split("=", 1)[0] for ln in lines if ln.startswith("# ") and "=" in ln]
        self.assertEqual(keys[:6], ["fw", "type", "id", "boot", "t0_ms", "trigger_ms"])
        self.assertEqual(keys[6:15], ["launchRpm", "launchDrop", "redlineRpm", "cutPattern", "maxCutSeconds",
                                      "decelPops", "decelRpm", "ghostCam", "armed"])
        self.assertEqual(keys[15:24], sl.DIAG_KEYS)
        self.assertIn("# sum=FIRED, tartás 3752–3880 RPM, kioldás 118 ms", lines)
        self.assertIn("# end=fired", lines)
        self.assertIn("# release_ms=118", lines)
        self.assertEqual(lines[lines.index("# trace") + 1], sl.TRACE_COLUMNS)

    def test_trace_time_is_unwrapped(self):
        # trigger at 5 s, 32-bit micros wrapped: events near 2^32 before the trigger unwrap to <= 5e6
        self.assertEqual(sl.unwrap_us(4_999_000, 5000), 4_999_000)
        self.assertEqual(sl.unwrap_us(1000, 4_295_000), 4_294_968_296)   # after the 71.6 min wrap
        self.assertEqual(sl.unwrap_us(0xFFFFFF00, 4_295_000), 0xFFFFFF00)

    def test_incomplete_capture_is_not_listed(self):
        li, _ = self._img_with(stop_after=5)
        self.assertIsNone(li.capture(57))
        self.assertTrue(li.captures and not li.captures[0].complete)
        self.assertIn("# error=", sl.capture_csv(li.captures[0]))

    def test_crc_mismatch_rejects_capture(self):
        li, w = self._img_with()
        c = li.captures[0]
        img = bytearray(w.img)
        # rewrite one sample record with valid record CRC but different data: END CRC must catch it
        for rec in sl.iter_records(img[c.sector * 4096:(c.sector + 1) * 4096]):
            if rec[0] != "end" and rec[1] == sl.REC_CAP_SAMPLES:
                off, _t, pl = rec
                pl = bytearray(pl)
                pl[6] ^= 0xFF
                crc = sl.rec_crc16(sl.REC_CAP_SAMPLES, bytes(pl))
                base = c.sector * 4096 + off
                img[base + 2:base + 4] = bytes([crc & 0xFF, crc >> 8])
                img[base + 4:base + 4 + len(pl)] = pl
                break
        self.assertIsNone(sl.LogImage(bytes(img)).capture(57))

    def test_capture_spanning_sectors(self):
        w = sl.LogWriter()
        m = mk_meta(9, ctype=4)
        w.write_capture(m, "kézi mentés, 60 s, csúcs 4012 RPM", mk_samples(1500), [], boot=1)
        c = sl.LogImage(bytes(w.img)).capture(9)
        self.assertIsNotNone(c)
        self.assertEqual(len(c.samples), 1500)
        text = sl.capture_csv(c)
        self.assertEqual(sl.check_csv(text), [])
        self.assertTrue(text.endswith("# trace\n" + sl.TRACE_COLUMNS + "\n"))


class DriveCsv(unittest.TestCase):
    def test_filters_gaps_and_config_changes(self):
        w = sl.LogWriter()
        per_sector = 16 + 15 * 21
        cfg2 = sl.Config(launchRpm=4000)
        w.write_drive(4, mk_samples(3 * per_sector, t0=0), sl.Config(), gaps={100: (4000, 7960, 100)},
                      cfg_changes={per_sector: cfg2})
        d = sl.LogImage(bytes(w.img)).drives[4]
        self.assertEqual(d.gaps, 1)
        full = sl.drive_csv(d)
        self.assertEqual(sl.check_csv(full), [])
        self.assertIn("# gap from_ms=4000 to_ms=7960 lost=100\n", full)
        self.assertIn("# config launchRpm=4000 launchDrop=400 redlineRpm=6200 cutPattern=1 maxCutSeconds=3.00 "
                      "decelPops=1 decelRpm=3200 ghostCam=0 armed=1\n", full)
        part = sl.drive_csv(d, from_ms=1000, to_ms=2000, step=5)
        rows = [ln for ln in part.split("\n") if ln and not ln.startswith("#") and not ln.startswith("t_ms")]
        self.assertEqual(len(rows), 6)                    # 1000..2000 ms = 26 samples, every 5th
        self.assertEqual(rows[0].split(",")[0], "1000")
        self.assertIn("# step=5\n", part)


class CsvCheck(unittest.TestCase):
    def test_detects_problems(self):
        good = ("# fw=2.1.0\n# type=drive\n# boot=1\n# t0_ms=0\n" + sl.SAMPLE_COLUMNS + "\n"
                "0,850,0,0,0,0,1,0,1,0,0,0,1,12\n40,851,0,0,0,0,1,0,1,0,0,0,1,12\n")
        self.assertEqual(sl.check_csv(good), [])
        self.assertTrue(sl.check_csv(good.replace("40,851", "30,851").replace("0,850", "50,850")))
        self.assertTrue(sl.check_csv(good.replace("# boot=1\n", "")))
        self.assertTrue(sl.check_csv(good.replace(",1,12\n40", ",1\n40")))


if __name__ == "__main__":
    unittest.main(verbosity=2)
