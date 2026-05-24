#!/usr/bin/env python3
"""Validate recovery image metadata emitted by PlatformIO."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Verify required recovery images are present in PlatformIO idedata.json"
    )
    parser.add_argument(
        "--build-dir",
        required=True,
        help="PlatformIO build directory containing idedata.json",
    )
    args = parser.parse_args()

    build_dir = Path(args.build_dir)
    idedata_path = build_dir / "idedata.json"
    idedata = json.loads(idedata_path.read_text(encoding="utf-8"))

    flash_images = {
        Path(image["path"]).name: Path(image["path"])
        for image in idedata.get("extra", {}).get("flash_images", [])
    }

    required = ("bootloader.bin", "partitions.bin", "boot_app0.bin")
    missing = [name for name in required if name not in flash_images]
    if missing:
        raise SystemExit(
            f"Missing required recovery images in idedata.json: {', '.join(missing)}"
        )

    for name in required:
        path = flash_images[name]
        if not path.is_file():
            raise SystemExit(f"Required recovery image is missing on disk: {path}")


if __name__ == "__main__":
    main()