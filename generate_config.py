#!/usr/bin/env python3
"""
generate_config.py - GlassesDetector Config Generator

Pulls company IDs from the Nordic Semiconductor Bluetooth Numbers Database
and cross-references them with a curated list of smart glasses manufacturers
to produce an up-to-date config.json for the GlassesDetector firmware.

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
# Each entry maps a search pattern (applied to the Nordic DB company name)
# to metadata about the device. Multiple company IDs can match one device
# (e.g., Meta has several registrations).
#
# Fields:
#   search_names: list of substrings to match in Nordic DB (case-insensitive)
#   device_id:    unique ID for the config entry
#   label:        human-readable display name on the StickS3 screen
#   category:     "glasses" = confirmed smart glasses maker
#                 "glasses_and_other" = makes glasses AND other BLE devices
#                 "meta_ambiguous" = Meta IDs shared across Ray-Ban + Quest
#   name_prefixes: known BLE device name prefixes (from real scans)
#   service_uuids: known BLE service UUIDs (from real scans)
#   adv_data_length_range: [min, max] advertisement payload length (0,0 = unknown)
#   notes:        documentation string
#   has_camera:   whether the glasses have an onboard camera

GLASSES_MANUFACTURERS = [
    {
        "search_names": ["luxottica"],
        "device_id": "meta_rayban",
        "label": "Meta Ray-Ban",
        "category": "glasses",
        "name_prefixes": ["Ray-Ban"],
        "service_uuids": [],
        "adv_data_length_range": [0, 0],
        "notes": "Luxottica - manufactures Meta Ray-Ban smart glasses. Strong glasses indicator.",
        "has_camera": True,
    },
    {
        "search_names": ["meta platforms, inc"],
        "device_id": "meta_platform",
        "label": "Meta (Generic)",
        "category": "meta_ambiguous",
        "name_prefixes": [],
        "service_uuids": [],
        "adv_data_length_range": [0, 0],
        "notes": "Meta Platforms Inc - could be Ray-Ban smart glasses OR Quest VR headset.",
        "has_camera": True,
    },
    {
        "search_names": ["meta platforms technologies"],
        "device_id": "meta_tech",
        "label": "Meta Technologies",
        "category": "meta_ambiguous",
        "name_prefixes": [],
        "service_uuids": [],
        "adv_data_length_range": [0, 0],
        "notes": "Meta Platforms Technologies - could be Ray-Ban OR Quest.",
        "has_camera": True,
    },
    {
        "search_names": ["snap inc", "snapchat"],
        "device_id": "snap_spectacles",
        "label": "Snap Spectacles",
        "category": "glasses",
        "name_prefixes": ["Spectacles"],
        "service_uuids": [],
        "adv_data_length_range": [0, 0],
        "notes": "Snapchat Inc - Spectacles smart glasses with camera.",
        "has_camera": True,
    },
    {
        "search_names": ["tcl communication"],
        "device_id": "tcl_rayneo",
        "label": "TCL RayNeo",
        "category": "glasses_and_other",
        "name_prefixes": ["RayNeo"],
        "service_uuids": [],
        "adv_data_length_range": [0, 0],
        "notes": "TCL - makes RayNeo AR glasses. Also makes phones/tablets (false positive risk).",
        "has_camera": True,
    },
    {
        "search_names": ["even realities"],
        "device_id": "even_realities",
        "label": "Even Realities G1",
        "category": "glasses",
        "name_prefixes": ["G1", "Even"],
        "service_uuids": [],
        "adv_data_length_range": [0, 0],
        "notes": "Even Realities G1 AI glasses. No camera but AI-enabled display.",
        "has_camera": False,
    },
    {
        "search_names": ["solos technology"],
        "device_id": "solos_airgo",
        "label": "Solos AirGo",
        "category": "glasses",
        "name_prefixes": ["AirGo", "Solos"],
        "service_uuids": [],
        "adv_data_length_range": [0, 0],
        "notes": "Solos AirGo smart glasses with AI assistant.",
        "has_camera": False,
    },
    {
        "search_names": ["xreal"],
        "device_id": "xreal",
        "label": "XREAL",
        "category": "glasses",
        "name_prefixes": ["XREAL"],
        "service_uuids": [],
        "adv_data_length_range": [0, 0],
        "notes": "XREAL (formerly Nreal) AR glasses.",
        "has_camera": False,
    },
    {
        "search_names": ["nreal"],
        "device_id": "nreal",
        "label": "Nreal",
        "category": "glasses",
        "name_prefixes": ["Nreal"],
        "service_uuids": [],
        "adv_data_length_range": [0, 0],
        "notes": "Nreal (now XREAL) - legacy company ID if registered separately.",
        "has_camera": False,
    },
    {
        "search_names": ["vuzix"],
        "device_id": "vuzix",
        "label": "Vuzix",
        "category": "glasses",
        "name_prefixes": ["Vuzix"],
        "service_uuids": [],
        "adv_data_length_range": [0, 0],
        "notes": "Vuzix smart glasses (enterprise and consumer).",
        "has_camera": True,
    },
    {
        "search_names": ["brilliant labs"],
        "device_id": "brilliant_monocle",
        "label": "Brilliant Labs",
        "category": "glasses",
        "name_prefixes": ["Monocle", "Frame"],
        "service_uuids": [],
        "adv_data_length_range": [0, 0],
        "notes": "Brilliant Labs - Frame and Monocle AI glasses.",
        "has_camera": True,
    },
    {
        "search_names": ["essilor"],
        "device_id": "essilor",
        "label": "EssilorLuxottica",
        "category": "glasses",
        "name_prefixes": [],
        "service_uuids": [],
        "adv_data_length_range": [0, 0],
        "notes": "EssilorLuxottica - parent company of Luxottica, may have separate BLE IDs.",
        "has_camera": True,
    },
    {
        "search_names": ["google"],
        "device_id": "google_glasses",
        "label": "Google Glass",
        "category": "glasses_and_other",
        "name_prefixes": ["Glass"],
        "service_uuids": [],
        "adv_data_length_range": [0, 0],
        "notes": "Google - makes many BLE devices. Only flag if name matches 'Glass'. Very high false positive risk.",
        "has_camera": True,
    },
    {
        "search_names": ["samsung"],
        "device_id": "samsung_glasses",
        "label": "Samsung Glasses",
        "category": "glasses_and_other",
        "name_prefixes": [],
        "service_uuids": [],
        "adv_data_length_range": [0, 0],
        "notes": "Samsung - developing Project Moohan XR and smart glasses. Extremely high false positive risk from Galaxy phones/watches/buds.",
        "has_camera": True,
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
    """Match Nordic DB entries against our curated glasses manufacturers.

    Returns a dict mapping device_id -> list of matching Nordic DB entries.
    """
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
    exclude_high_fp: bool = True,
) -> dict:
    """Generate a config.json structure from matched companies."""

    # Build a lookup from device_id -> manufacturer metadata
    mfr_lookup = {m["device_id"]: m for m in GLASSES_MANUFACTURERS}

    devices = []
    for device_id, company_matches in matches.items():
        mfr = mfr_lookup[device_id]

        # Skip devices with no camera if requested
        if not include_no_camera and not mfr.get("has_camera", False):
            continue

        # Skip high-false-positive entries unless they have real manufacturer IDs
        if exclude_high_fp and mfr["category"] == "glasses_and_other":
            if not mfr["name_prefixes"]:
                # No name prefixes = pure manufacturer ID match = too many FPs
                print(f"  Skipping {device_id}: no name prefixes, high FP risk")
                continue

        # Convert decimal codes to hex strings
        manufacturer_ids = [f"0x{c['code']:04X}" for c in company_matches]

        if not manufacturer_ids and not mfr["name_prefixes"]:
            print(f"  Skipping {device_id}: no manufacturer IDs or name prefixes found")
            continue

        device_entry = {
            "id": device_id,
            "label": mfr["label"],
            "category": mfr["category"],
            "manufacturer_ids": manufacturer_ids,
            "service_uuids": mfr["service_uuids"],
            "name_prefixes": mfr["name_prefixes"],
            "adv_data_length_range": mfr["adv_data_length_range"],
            "has_camera": mfr.get("has_camera", False),
            "notes": mfr["notes"],
        }
        devices.append(device_entry)

    config = {
        "version": 1,
        "source": "Generated by generate_config.py from Nordic Semiconductor Bluetooth Numbers Database",
        "nordic_db_url": NORDIC_DB_URL,
        "scan": {
            "window_ms": 3000,
            "interval_ms": 10000,
            "rssi_threshold": -75,
            "display_timeout_ms": 30000,
            "lcd_on_ms": 5000,
            "lcd_brightness": 128,
            "lcd_brightness_dim": 32,
        },
        "confidence": {
            "manufacturer_id_match": 40,
            "service_uuid_match": 30,
            "name_prefix_match": 20,
            "adv_length_match": 10,
            "threshold_likely": 70,
            "threshold_possible": 40,
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
        print(f"{camera_str} {mfr['label']} [{mfr['category']}]")

        if company_matches:
            for c in company_matches:
                print(f"    0x{c['code']:04X} ({c['code']:>5d})  {c['name']}")
        else:
            print(f"    (no matching company ID found in Nordic DB)")

        if mfr["name_prefixes"]:
            print(f"    Name prefixes: {mfr['name_prefixes']}")
        print()


def main():
    parser = argparse.ArgumentParser(
        description="Generate GlassesDetector config.json from Nordic BLE database"
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
        "--include-no-camera",
        action="store_true",
        default=True,
        help="Include glasses without cameras (default: True)",
    )
    parser.add_argument(
        "--camera-only",
        action="store_true",
        help="Only include glasses with cameras",
    )
    parser.add_argument(
        "--include-high-fp",
        action="store_true",
        help="Include high-false-positive entries like Samsung/Google without name filters",
    )
    args = parser.parse_args()

    # Fetch the database
    nordic_db = fetch_nordic_db()

    # Match against our curated list
    matches = match_companies(nordic_db)

    # Print results
    print_scan_results(matches)

    if args.scan_only:
        return

    # Generate config
    include_no_camera = not args.camera_only
    config = generate_config(
        matches,
        include_no_camera=include_no_camera,
        exclude_high_fp=not args.include_high_fp,
    )

    # Write output
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
