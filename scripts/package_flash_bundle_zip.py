#!/usr/bin/env python3
"""Pack IDF build/ into a flash bundle .zip suitable for platform Admin upload."""

from __future__ import annotations

import json
import sys
import zipfile
from pathlib import Path

# Project root = parent of scripts/
ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build"


def main() -> None:
    args_path = BUILD / "flasher_args.json"
    if not args_path.is_file():
        print("Missing build/flasher_args.json — run idf.py build first", file=sys.stderr)
        sys.exit(1)

    data = json.loads(args_path.read_text(encoding="utf-8"))
    files: dict[str, str] = data.get("flash_files") or {}
    if not files:
        print("flasher_args.json has no flash_files", file=sys.stderr)
        sys.exit(1)

    version_line = next((ln for ln in (ROOT / "CMakeLists.txt").read_text().splitlines() if ln.strip().startswith("set(PROJECT_VER")), None)
    ver = "unknown"
    if version_line and '"' in version_line:
        ver = version_line.split('"', 2)[1]

    out_dir = ROOT / "releases"
    out_dir.mkdir(exist_ok=True)
    out_zip = out_dir / f"flash-bundle-waveshare-c6-lcd-1.69-v{ver}.zip"

    with zipfile.ZipFile(out_zip, "w", compression=zipfile.ZIP_DEFLATED) as zf:
        zf.write(args_path, "flasher_args.json")
        for _off, rel in sorted(files.items(), key=lambda kv: kv[0]):
            src = BUILD / rel
            if not src.is_file():
                print(f"Missing build artifact: {rel} ({src})", file=sys.stderr)
                sys.exit(1)
            zf.write(src, rel)

    print(out_zip)


if __name__ == "__main__":
    main()
