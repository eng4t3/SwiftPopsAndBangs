#!/usr/bin/env python3
"""Export the embedded dashboard (INDEX_HTML in swift_show_tuning/web_ui.h) to preview.html.

The page switches to its built-in demo mode (simulated engine, mocked /api and OTA flows)
when it is opened from a file:// URL or with ?demo=1, so preview.html can be opened
directly in any browser without the ESP32.

Usage (from anywhere):
    python tools/ui/export_preview.py            write preview.html, print the page size
    python tools/ui/export_preview.py --check    exit 1 if preview.html is out of date
    python tools/ui/export_preview.py --js FILE  also write the inline <script> code to FILE
                                                 (e.g. for `node --check FILE`)
"""
import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HEADER = ROOT / "swift_show_tuning" / "web_ui.h"
PREVIEW = ROOT / "preview.html"
OPEN = 'R"rawliteral('
CLOSE = ')rawliteral"'
FLASH_BUDGET = 104 * 1024  # page budget in flash (v2.2: tabs, profiles, logger)


def extract_page(header_text: str) -> str:
    start = header_text.find(OPEN)
    if start < 0:
        sys.exit(f"error: {OPEN} not found in {HEADER}")
    start += len(OPEN)
    end = header_text.find(CLOSE, start)
    if end < 0:
        sys.exit(f"error: {CLOSE} not found in {HEADER}")
    if header_text.find(CLOSE, end + 1) >= 0:
        sys.exit(f"error: {CLOSE} appears more than once in {HEADER}")
    page = header_text[start:end]
    # The literal starts right after the opening parenthesis; drop that first newline
    return page[1:] if page.startswith("\n") else page


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true", help="only verify that preview.html is up to date")
    ap.add_argument("--js", metavar="FILE", help="write the inline script code to FILE")
    args = ap.parse_args()

    # newline="" keeps the file's own line endings (the literal is compiled byte for byte)
    header_text = HEADER.read_text(encoding="utf-8", newline="")
    page = extract_page(header_text)
    size = len(page.encode("utf-8"))  # sizeof(INDEX_HTML) - 1 would add the leading newline

    if args.js:
        scripts = re.findall(r"<script>(.*?)</script>", page, flags=re.S)
        Path(args.js).write_text("\n".join(scripts), encoding="utf-8", newline="")

    if args.check:
        current = PREVIEW.read_text(encoding="utf-8", newline="") if PREVIEW.exists() else ""
        if current.replace("\r\n", "\n") != page.replace("\r\n", "\n"):
            print("preview.html is out of date: run tools/ui/export_preview.py")
            return 1
        print("preview.html is up to date")
        return 0

    PREVIEW.write_text(page, encoding="utf-8", newline="")
    lf_size = len(page.replace("\r\n", "\n").encode("utf-8"))
    print(f"wrote {PREVIEW.relative_to(ROOT)}: {size} bytes ({size / 1024:.1f} KB)"
          + (f", {lf_size} bytes with LF line endings" if lf_size != size else ""))
    if lf_size > FLASH_BUDGET:
        print(f"warning: page is larger than the {FLASH_BUDGET // 1024} KB budget")
    return 0


if __name__ == "__main__":
    sys.exit(main())
