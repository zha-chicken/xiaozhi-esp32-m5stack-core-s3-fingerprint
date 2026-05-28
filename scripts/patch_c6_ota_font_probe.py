#!/usr/bin/env python3
"""Patch a C6 OTA verifier probe in the managed xiaozhi-fonts component."""

from __future__ import annotations

from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
FONT_FILE = ROOT / "managed_components/78__xiaozhi-fonts/src/font_puhui_basic_20_4.c"

ORIGINAL = b"""    0x2f, 0xff, 0xff, 0xff, 0xff, 0xf5, 0x2f, 0xd8,
    0x88, 0x88, 0x8c, 0xf5, 0x2f, 0xa0, 0x0, 0x0,
"""

PATCHED = b"""    0x2f, 0xff, 0xff, 0xff, 0xff, 0xf5, 0x2f, 0xd8,
    0x0, 0x0, 0x0, 0x0, 0x2f, 0xa0, 0x0, 0x0,
"""


def main() -> None:
    data = FONT_FILE.read_bytes()
    if PATCHED in data:
        print(f"{FONT_FILE} already patched")
        return
    if ORIGINAL not in data:
        raise SystemExit(f"expected font probe bytes not found: {FONT_FILE}")
    FONT_FILE.write_bytes(data.replace(ORIGINAL, PATCHED, 1))
    print(f"patched {FONT_FILE}")


if __name__ == "__main__":
    main()
