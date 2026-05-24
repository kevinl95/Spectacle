#!/usr/bin/env python3
"""Package PlatformIO artifacts for GitHub Releases and GitHub Pages."""

from __future__ import annotations

import argparse
import csv
import json
import shutil
from dataclasses import dataclass
from pathlib import Path


PROJECT_NAME = "Spectacle"
BOARD_NAME = "M5Stack StickS3"
CHIP_NAME = "ESP32-S3"
BOOTLOADER_OFFSET = 0x0
PARTITIONS_OFFSET = 0x8000
FLASH_SIZE = "8MB"
BAUD_RATE = 115200

ASSET_NAMES = {
    "bootloader": "spectacle-bootloader.bin",
    "partitions": "spectacle-partitions.bin",
    "firmware": "spectacle-firmware.bin",
    "spiffs": "spectacle-spiffs.bin",
    "manifest": "spectacle-flash-manifest.json",
    "metadata": "spectacle-build-metadata.json",
}


@dataclass
class PartitionEntry:
    name: str
    entry_type: str
    subtype: str
    offset: int
    size: int


def parse_numeric(value: str) -> int:
    text = value.strip().upper()
    if not text:
        raise ValueError("Expected a numeric partition value")
    if text.startswith("0X"):
        return int(text, 16)
    if text.endswith("K"):
        return int(text[:-1], 0) * 1024
    if text.endswith("M"):
        return int(text[:-1], 0) * 1024 * 1024
    return int(text, 0)


def to_hex(value: int) -> str:
    return f"0x{value:X}"


def load_partitions(csv_path: Path) -> list[PartitionEntry]:
    partitions: list[PartitionEntry] = []

    with csv_path.open(newline="", encoding="utf-8") as handle:
        reader = csv.reader(handle)
        for row in reader:
            if not row:
                continue

            first = row[0].strip()
            if not first or first.startswith("#"):
                continue

            name, entry_type, subtype, offset, size, *_ = [column.strip() for column in row]
            partitions.append(
                PartitionEntry(
                    name=name,
                    entry_type=entry_type,
                    subtype=subtype,
                    offset=parse_numeric(offset),
                    size=parse_numeric(size),
                )
            )

    return partitions


def find_partition(partitions: list[PartitionEntry], *, name: str | None = None, subtype: str | None = None, entry_type: str | None = None) -> PartitionEntry:
    for partition in partitions:
        if name is not None and partition.name != name:
            continue
        if subtype is not None and partition.subtype != subtype:
            continue
        if entry_type is not None and partition.entry_type != entry_type:
            continue
        return partition

    raise ValueError("Required partition entry was not found")


def clean_dir(path: Path) -> None:
    if path.exists():
        shutil.rmtree(path)
    path.mkdir(parents=True, exist_ok=True)


def copy_file(source: Path, target: Path) -> int:
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, target)
    return target.stat().st_size


def write_json(path: Path, payload: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser(description="Package release assets for Spectacle")
    parser.add_argument("--repo", required=True, help="GitHub repository in owner/name form")
    parser.add_argument("--tag", required=True, help="Release tag")
    parser.add_argument("--published-at", required=True, help="UTC release timestamp")
    parser.add_argument("--commit-sha", required=True, help="Commit SHA for the release")
    parser.add_argument("--config-changed", default="false", help="Whether config.json changed in this run")
    parser.add_argument("--config", required=True, help="Path to generated config.json")
    parser.add_argument("--build-dir", required=True, help="PlatformIO build directory")
    parser.add_argument("--partition-csv", required=True, help="Resolved partition CSV path")
    parser.add_argument("--release-dir", required=True, help="Directory to populate with release assets")
    parser.add_argument("--site-dir", required=True, help="Static Pages source directory")
    parser.add_argument("--pages-dir", required=True, help="Directory to populate with the deployable Pages site")
    args = parser.parse_args()

    config_path = Path(args.config)
    build_dir = Path(args.build_dir)
    partition_csv = Path(args.partition_csv)
    release_dir = Path(args.release_dir)
    site_dir = Path(args.site_dir)
    pages_dir = Path(args.pages_dir)
    pages_assets_dir = pages_dir / "assets"

    clean_dir(release_dir)
    clean_dir(pages_dir)
    shutil.copytree(site_dir, pages_dir, dirs_exist_ok=True)
    pages_assets_dir.mkdir(parents=True, exist_ok=True)

    config = json.loads(config_path.read_text(encoding="utf-8"))
    partitions = load_partitions(partition_csv)
    app_partition = find_partition(partitions, name="app0")
    spiffs_partition = find_partition(partitions, subtype="spiffs")

    release_base_url = f"https://github.com/{args.repo}/releases/download/{args.tag}"
    release_page_url = f"https://github.com/{args.repo}/releases/tag/{args.tag}"

    planned_assets = [
        {
            "slot": "bootloader",
            "label": "Bootloader",
            "source": build_dir / "bootloader.bin",
            "filename": ASSET_NAMES["bootloader"],
            "address": BOOTLOADER_OFFSET,
            "default": False,
            "description": "Required only for full-chip recovery or first-time provisioning.",
        },
        {
            "slot": "partitions",
            "label": "Partition table",
            "source": build_dir / "partitions.bin",
            "filename": ASSET_NAMES["partitions"],
            "address": PARTITIONS_OFFSET,
            "default": False,
            "description": "Required only when erasing the chip or repairing the flash layout.",
        },
        {
            "slot": "firmware",
            "label": "Application firmware",
            "source": build_dir / "firmware.bin",
            "filename": ASSET_NAMES["firmware"],
            "address": app_partition.offset,
            "default": True,
            "description": "Routine weekly detector firmware update.",
        },
        {
            "slot": "spiffs",
            "label": "Configuration filesystem",
            "source": build_dir / "spiffs.bin",
            "filename": ASSET_NAMES["spiffs"],
            "address": spiffs_partition.offset,
            "default": True,
            "description": "SPIFFS image containing /config.json for the detector firmware.",
        },
    ]

    images = []
    artifact_sizes: dict[str, int] = {}
    for asset in planned_assets:
        release_target = release_dir / asset["filename"]
        pages_target = pages_assets_dir / asset["filename"]
        size_bytes = copy_file(asset["source"], release_target)
        copy_file(asset["source"], pages_target)
        artifact_sizes[asset["filename"]] = size_bytes

        images.append(
            {
                "slot": asset["slot"],
                "label": asset["label"],
                "filename": asset["filename"],
                "address": to_hex(asset["address"]),
                "sizeBytes": size_bytes,
                "default": asset["default"],
                "path": f"./assets/{asset['filename']}",
                "releaseUrl": f"{release_base_url}/{asset['filename']}",
                "description": asset["description"],
            }
        )

    metadata = {
        "project": PROJECT_NAME,
        "board": BOARD_NAME,
        "chip": CHIP_NAME,
        "tag": args.tag,
        "publishedAt": args.published_at,
        "commitSha": args.commit_sha,
        "configChanged": args.config_changed.lower() == "true",
        "configVersion": config.get("version"),
        "deviceCount": len(config.get("devices", [])),
        "artifactSizes": artifact_sizes,
    }

    manifest = {
        "status": "ready",
        "schemaVersion": 1,
        "project": PROJECT_NAME,
        "board": BOARD_NAME,
        "chip": CHIP_NAME,
        "baudRate": BAUD_RATE,
        "flashMode": "keep",
        "flashFreq": "keep",
        "flashSize": FLASH_SIZE,
        "release": {
            "tag": args.tag,
            "publishedAt": args.published_at,
            "commitSha": args.commit_sha,
            "releaseUrl": release_page_url,
        },
        "config": {
            "sourcePath": str(config_path),
            "runtimePath": "/config.json",
            "version": config.get("version"),
            "deviceCount": len(config.get("devices", [])),
            "source": config.get("source", "Generated by generate_config.py"),
        },
        "partitions": {
            "table": partition_csv.name,
            "appOffset": to_hex(app_partition.offset),
            "spiffsOffset": to_hex(spiffs_partition.offset),
            "bootloaderOffset": to_hex(BOOTLOADER_OFFSET),
            "partitionTableOffset": to_hex(PARTITIONS_OFFSET),
        },
        "update": {
            "eraseAll": False,
            "note": "Routine updates flash only the application and SPIFFS images. Use them only on devices already provisioned with Spectacle's partition table.",
        },
        "recovery": {
            "eraseAll": True,
            "note": "Factory reflash erases the whole chip and then restores bootloader, partition table, application, and SPIFFS. Use this for first install, after a full erase, or if the device boots with SPIFFS FAILED.",
        },
        "images": images,
        "metadata": metadata,
    }

    write_json(release_dir / ASSET_NAMES["metadata"], metadata)
    write_json(release_dir / ASSET_NAMES["manifest"], manifest)
    write_json(pages_dir / "latest-release.json", manifest)


if __name__ == "__main__":
    main()