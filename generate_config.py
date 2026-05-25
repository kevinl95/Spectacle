#!/usr/bin/env python3
"""
generate_config.py - Spectacle Config Generator

Pulls company IDs from the Nordic Semiconductor Bluetooth Numbers Database
and cross-references them with a curated list of smart glasses manufacturers
to produce an up-to-date config.json for the Spectacle firmware.

*** THIS FILE IS THE SINGLE SOURCE OF TRUTH FOR DEVICE SIGNATURES. ***

Do NOT edit data/config.json directly — it is a generated build artifact
that gets overwritten by the weekly GitHub Action. To add or update a
device signature (new glasses model, sniffed BLE data, name prefixes, etc.),
edit the GLASSES_MANUFACTURERS list below and re-run this script.

Usage:
    python3 generate_config.py
    python3 generate_config.py --output data/config.json
    python3 generate_config.py --scan-only  # just show matching companies

Requires: requests (pip install requests)
"""

import json
import argparse
import sys
from pathlib import Path

try:
    import requests
except ImportError:
    print("Error: requests library required. Install with: pip install requests")
    sys.exit(1)

# ── Nordic Semiconductor Bluetooth Numbers Database ──────────────────────────

NORDIC_DB_URL = (
    "https://raw.githubusercontent.com/NordicSemiconductor/"
    "bluetooth-numbers-database/master/v1/company_ids.json"
)

# ── Curated Smart Glasses Manufacturers ──────────────────────────────────────
#
# *** CONTRIBUTING: This is where you add/update device signatures. ***
#
# To add a new device:
#   1. Add an entry to GLASSES_MANUFACTURERS below
#   2. Run: python3 generate_config.py --scan-only   (verify company ID match)
#   3. Run: python3 generate_config.py               (regenerate config.json)
#   4. Submit a PR with your changes to THIS FILE only
#
# To add sniffed BLE data for an existing device:
#   1. Use nRF Connect or similar to capture advertisements
#   2. Note: manufacturer data payload (look for ASCII strings), device name
#   3. Update the corresponding entry below
#   4. Submit a PR with your changes to THIS FILE only
#
# Detection logic (firmware side):
#   1. If manufacturer_ids is non-empty, one of them must match
#   2. If payload_strings is non-empty, at least one must appear in the
#      manufacturer data payload (e.g., Ray-Bans broadcast "META_RB_GLASS")
#   3. If name_prefixes is non-empty, the BLE device name must start with one
#   All defined checks must pass. Undefined checks are skipped.
#
# Fields:
#   search_names:    list of substrings to match in Nordic DB (case-insensitive)
#   device_id:       unique ID for the config entry
#   label:           human-readable name shown on the StickS3 screen
#   has_camera:      whether the glasses have a camera (affects alert color)
#   name_prefixes:   known BLE device name prefixes (from real scans)
#   payload_strings: strings to match in manufacturer data payload (from real scans)
#                    e.g., Ray-Bans include "META_RB_GLASS" after the company ID
#   notes:           documentation string

GLASSES_MANUFACTURERS = [
    {
        "search_names": ["luxottica"],
        "device_id": "meta_rayban_luxottica",
        "label": "Meta Ray-Ban",
        "has_camera": True,
        "name_prefixes": [],
        "payload_strings": [],
        "notes": "Luxottica company ID (0x0D53). Luxottica only makes eyewear, so this ID alone is a strong glasses indicator.",
    },
    {
        "search_names": ["meta platforms technologies"],
        "device_id": "meta_rayban_confirmed",
        "label": "Meta Ray-Ban",
        "has_camera": True,
        "name_prefixes": [],
        "payload_strings": ["META_RB_GLASS"],
        "notes": "Meta Platforms Technologies ID (0x058E) with payload string 'META_RB_GLASS'. Confirmed Ray-Ban — payload string eliminates Quest false positives.",
    },
    {
        "search_names": ["meta platforms, inc"],
        "device_id": "meta_rayban_generic",
        "label": "Meta Ray-Ban",
        "has_camera": True,
        "name_prefixes": [],
        "payload_strings": ["META_RB_GLASS"],
        "notes": "Meta Platforms Inc ID (0x01AB) with payload string 'META_RB_GLASS'. Only triggers on confirmed Ray-Ban payload, not Quest.",
    },
    {
        "search_names": ["snap inc", "snapchat"],
        "device_id": "snap_spectacles",
        "label": "Snap Spectacles",
        "has_camera": True,
        "name_prefixes": ["Spectacles"],
        "payload_strings": [],
        "notes": "Snapchat Inc (0x03C2). Snap mainly makes Spectacles for BLE devices.",
    },
    {
        "search_names": ["tcl communication"],
        "device_id": "tcl_rayneo",
        "label": "TCL RayNeo",
        "has_camera": True,
        "name_prefixes": ["RayNeo"],
        "payload_strings": [],
        "notes": "TCL makes RayNeo AR glasses but also phones/tablets. Name prefix required to avoid false positives.",
    },
    {
        "search_names": ["even realities"],
        "device_id": "even_realities",
        "label": "Even Realities G1",
        "has_camera": False,
        "name_prefixes": ["G1", "Even"],
        "payload_strings": [],
        "notes": "Even Realities G1 AI glasses. No camera, AI display only.",
    },
    {
        "search_names": ["solos technology"],
        "device_id": "solos_airgo",
        "label": "Solos AirGo",
        "has_camera": False,
        "name_prefixes": ["AirGo", "Solos"],
        "payload_strings": [],
        "notes": "Solos AirGo smart glasses with AI assistant. No camera.",
    },
    {
        "search_names": ["xreal"],
        "device_id": "xreal",
        "label": "XREAL",
        "has_camera": False,
        "name_prefixes": ["XREAL"],
        "payload_strings": [],
        "notes": "XREAL (formerly Nreal) AR display glasses.",
    },
    {
        "search_names": ["nreal"],
        "device_id": "nreal",
        "label": "Nreal",
        "has_camera": False,
        "name_prefixes": ["Nreal"],
        "payload_strings": [],
        "notes": "Nreal (now XREAL) - legacy company ID if registered separately.",
    },
    {
        "search_names": ["vuzix"],
        "device_id": "vuzix",
        "label": "Vuzix",
        "has_camera": True,
        "name_prefixes": ["Vuzix"],
        "payload_strings": [],
        "notes": "Vuzix smart glasses (enterprise and consumer) with camera.",
    },
    {
        "search_names": ["brilliant labs"],
        "device_id": "brilliant_monocle",
        "label": "Brilliant Labs",
        "has_camera": True,
        "name_prefixes": ["Monocle", "Frame"],
        "payload_strings": [],
        "notes": "Brilliant Labs Frame and Monocle AI glasses with camera.",
    },
    {
        "search_names": ["essilor"],
        "device_id": "essilor",
        "label": "EssilorLuxottica",
        "has_camera": True,
        "name_prefixes": [],
        "payload_strings": [],
        "notes": "EssilorLuxottica - parent company of Luxottica, may have separate BLE IDs.",
    },
]


def fetch_nordic_db() -> list[dict]:
    """Fetch company IDs from Nordic Semiconductor's Bluetooth Numbers Database."""
    print(f"Fetching Nordic BLE database from:\n  {NORDIC_DB_URL}")
    resp = requests.get(NORDIC_DB_URL, timeout=30)
    resp.raise_for_status()
    data = resp.json()
    print(f"  Loaded {len(data)} company identifiers")
    return data


def match_companies(nordic_db: list[dict]) -> dict[str, list[dict]]:
    """Match Nordic DB entries against our curated glasses manufacturers."""
    matches: dict[str, list[dict]] = {}

    for mfr in GLASSES_MANUFACTURERS:
        device_id = mfr["device_id"]
        matches[device_id] = []

        for company in nordic_db:
            company_name_lower = company["name"].lower()
            for search_name in mfr["search_names"]:
                if search_name.lower() in company_name_lower:
                    matches[device_id].append(company)
                    break

    return matches


def generate_config(
    matches: dict[str, list[dict]],
    include_no_camera: bool = True,
) -> dict:
    """Generate a config.json structure from matched companies."""

    mfr_lookup = {m["device_id"]: m for m in GLASSES_MANUFACTURERS}

    devices = []
    for device_id, company_matches in matches.items():
        mfr = mfr_lookup[device_id]

        if not include_no_camera and not mfr.get("has_camera", False):
            continue

        # Convert decimal codes to hex strings
        manufacturer_ids = [f"0x{c['code']:04X}" for c in company_matches]

        # Must have at least one matching signal the firmware can evaluate.
        if not manufacturer_ids and not mfr["payload_strings"] and not mfr["name_prefixes"]:
            print(
                f"  Skipping {device_id}: no manufacturer IDs, payload strings, or name prefixes found"
            )
            continue

        device_entry = {
            "id": device_id,
            "label": mfr["label"],
            "has_camera": mfr.get("has_camera", False),
            "manufacturer_ids": manufacturer_ids,
            "payload_strings": mfr["payload_strings"],
            "name_prefixes": mfr["name_prefixes"],
            "notes": mfr["notes"],
        }
        devices.append(device_entry)

    config = {
        "version": 2,
        "source": "Generated by generate_config.py from Nordic Semiconductor Bluetooth Numbers Database",
        "nordic_db_url": NORDIC_DB_URL,
        "scan": {
            "window_ms": 1500,
            "interval_ms": 20000,
            "rssi_threshold": -75,
            "display_timeout_ms": 30000,
            "lcd_on_ms": 3000,
            "lcd_brightness": 128,
            "lcd_brightness_dim": 32,
        },
        "devices": devices,
    }

    return config


def print_scan_results(matches: dict[str, list[dict]]):
    """Print all matched companies for review."""
    mfr_lookup = {m["device_id"]: m for m in GLASSES_MANUFACTURERS}

    print("\n═══ Smart Glasses Company ID Matches ═══\n")
    for device_id, company_matches in matches.items():
        mfr = mfr_lookup[device_id]
        camera_str = "📷" if mfr.get("has_camera") else "  "
        print(f"{camera_str} {mfr['label']} [{device_id}]")

        if company_matches:
            for c in company_matches:
                print(f"    0x{c['code']:04X} ({c['code']:>5d})  {c['name']}")
        else:
            print("    (no matching company ID found in Nordic DB)")

        if mfr["payload_strings"]:
            print(f"    Payload strings: {mfr['payload_strings']}")
        if mfr["name_prefixes"]:
            print(f"    Name prefixes: {mfr['name_prefixes']}")
        print()


def main():
    parser = argparse.ArgumentParser(
        description="Generate Spectacle config.json from Nordic BLE database"
    )
    parser.add_argument(
        "--output", "-o",
        default="data/config.json",
        help="Output path for config.json (default: data/config.json)",
    )
    parser.add_argument(
        "--scan-only",
        action="store_true",
        help="Only scan and print matches, don't write config",
    )
    parser.add_argument(
        "--camera-only",
        action="store_true",
        help="Only include glasses with cameras",
    )
    args = parser.parse_args()

    nordic_db = fetch_nordic_db()
    matches = match_companies(nordic_db)
    print_scan_results(matches)

    if args.scan_only:
        return

    include_no_camera = not args.camera_only
    config = generate_config(matches, include_no_camera=include_no_camera)

    output_path = Path(args.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)

    with open(output_path, "w") as f:
        json.dump(config, f, indent=2)

    device_count = len(config["devices"])
    total_ids = sum(len(d["manufacturer_ids"]) for d in config["devices"])
    print(f"═══ Config Generated ═══")
    print(f"  Devices: {device_count}")
    print(f"  Total manufacturer IDs: {total_ids}")
    print(f"  Output: {output_path}")
    print()
    print(f"Upload to SPIFFS with: pio run -t uploadfs")


if __name__ == "__main__":
    main()