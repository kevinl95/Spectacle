# Spectacle - Smart Glasses Detecting Keychain

[![Weekly Firmware Release](https://github.com/kevinl95/Spectacle/actions/workflows/weekly-firmware.yml/badge.svg)](https://github.com/kevinl95/Spectacle/actions/workflows/weekly-firmware.yml)

A passive BLE smart glasses detector for the M5Stack StickS3. Scans for Bluetooth Low Energy advertisements from known smart glasses (Meta Ray-Ban, Snap Spectacles, etc.) and alerts you when they're nearby.

## Why hardware?

- **iOS can't do this** — Apple's Core Bluetooth blocks access to raw manufacturer data in BLE advertisements
- **Always-on** — no app to launch, just clip it to your keychain
- **Phone-agnostic** — works regardless of what phone you carry (or if you carry one at all)
- **Discreet** — glancing at a keychain is less conspicuous than opening a "glasses detector" app

## How it works

The detector passively listens for BLE advertising packets and matches them against a configurable database of known smart glasses signatures. Instead of relying on manufacturer ID alone, each signature can define one or more of these checks:

- Manufacturer ID
- Manufacturer data payload strings
- Device name prefixes

All defined checks for a signature must pass. Undefined checks are skipped. This lets the detector stay strict for ambiguous vendors like Meta by requiring payload strings such as `META_RB_GLASS`, while still allowing name-prefix-based matching for devices whose company ID is not yet known.

## Hardware

- [M5Stack StickS3](https://shop.m5stack.com/products/m5sticks3-esp32s3-mini-iot-dev-kit) ($21.50)
- That's it.

## Configuration

The device signature database is generated from `generate_config.py`, which pulls manufacturer IDs from the [Nordic Semiconductor Bluetooth Numbers Database](https://github.com/NordicSemiconductor/bluetooth-numbers-database) and merges them with curated smart glasses metadata. A weekly GitHub Action runs this automatically.

**Do NOT edit `data/config.json` directly** — it is a generated artifact. To change scan settings, add devices, or update sniffed BLE data, edit `generate_config.py` and re-run:

```bash
pip install requests
python3 generate_config.py          # regenerate data/config.json
pio run -t uploadfs
```

If `pio` is not on your `PATH`, use `python3 -m platformio run -t uploadfs`.

Scan timing, display behavior, and RSSI thresholds are also defined in `generate_config.py` in the `generate_config()` function.

## Flashing

### Entering download mode

The StickS3 uses the ESP32-S3's native USB rather than a separate UART bridge chip. To flash firmware, put the device into download mode:

1. Connect the StickS3 to your computer via USB-C with a data-capable cable.
2. Press and hold the reset button on the side of the device for about 2 seconds.
3. When the internal green LED blinks, release the button. The device is now in download mode.

If you enter download mode accidentally and want to return to normal operation without flashing, press the reset button briefly.

### Windows USB troubleshooting

The ESP32-S3's native USB can be finicky on Windows. If Windows shows `USB device not recognized` when the StickS3 is in download mode:

- Try a different USB port. USB 2.0 ports tend to work more reliably than USB 3.0 ports for ESP32-S3 native USB enumeration.
- Check Device Manager. Look under `Other devices` for anything with a yellow triangle. Try `Update driver` and select `USB Serial Device` or `USB JTAG/serial debug unit` from the built-in list.
- Try [Zadig](https://zadig.akeo.org) and assign the `WinUSB` driver to the ESP32-S3 device while it is in download mode.
- Try M5Stack's [M5Burner](https://docs.m5stack.com/en/download) tool if you need a known-good recovery path or factory restore.

The device cannot be permanently bricked by a bad flash here because the ESP32-S3 ROM bootloader remains available.

### Build and upload

```bash
# Build
pio run

# Upload firmware
pio run -t upload

# Upload config to SPIFFS
pio run -t uploadfs

# Monitor serial output
pio device monitor
```

If `pio` is not on your `PATH`, the equivalent commands are `python3 -m platformio run`, `python3 -m platformio run -t upload`, `python3 -m platformio run -t uploadfs`, `python3 -m platformio run -t buildfs`, and `python3 -m platformio device monitor`.

## Automated releases

The repository now includes `.github/workflows/weekly-firmware.yml`, which:

- regenerates `data/config.json` on a weekly schedule and on manual dispatch
- commits the generated config back to the default branch when it changes
- builds both the firmware image and the SPIFFS image
- publishes a GitHub release with the flash assets and manifest
- deploys a GitHub Pages updater site backed by `esptool.js`

The Pages updater uses the real partition layout from `default_8MB.csv`:

- firmware at `0x10000`
- SPIFFS at `0x670000`

## Web flasher

The browser updater lives in `docs/` and is intended for existing StickS3 devices.

- **Routine update** flashes only the application image and the SPIFFS config image.
- **Factory reflash** performs a full erase and then restores bootloader, partition table, application, and SPIFFS.

Do not full-erase the device for a normal update unless you are also restoring the bootloader and partition table. Erasing the whole chip and then flashing only the app plus SPIFFS will leave the device unbootable.

The web flasher requires Chrome or Edge for the Web Serial API. The same USB troubleshooting steps above still apply here. If the StickS3 shows up as `USB device not recognized`, try a USB 2.0 port first and confirm the device is actually in download mode before retrying.

## Controls

- **Button A** — Force immediate rescan
- **Button B** — Cycle RSSI threshold (-75 → -85 → -95 → -50 → ...)

## Adding new glasses

Add an entry to the `GLASSES_MANUFACTURERS` list in `generate_config.py`:

```python
{
    "search_names": ["new company"],          # substring match against Nordic DB
    "device_id": "new_glasses",
    "label": "Brand Name",                    # shown on the StickS3 screen
    "name_prefixes": ["BrandName"],           # BLE device name prefixes
    "payload_strings": ["BRAND_MARKER"],      # ASCII marker in manufacturer data, if present
    "notes": "Description",
    "has_camera": True,
}
```

Then run `python3 generate_config.py` to regenerate `data/config.json`.

`search_names` is used to resolve Bluetooth company IDs from the Nordic database. If a vendor ID is ambiguous, add a `payload_strings` marker or `name_prefixes` guard so the firmware requires those checks too. If the vendor is not in the Nordic database yet, name-prefix-only matching is still possible.

## Research needed

The strongest detections come from real manufacturer data payloads and stable device-name prefixes. If you have access to any of these glasses, run an nRF Connect scan and note:

- Company ID
- BLE device name
- Manufacturer data payload hex dump
- Any readable ASCII marker inside the manufacturer payload

PRs with real-world scan data welcome — update the relevant entry in `generate_config.py` (not `data/config.json`).

## License

MIT
