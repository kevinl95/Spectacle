# GlassesDetector

A passive BLE smart glasses detector for the M5Stack StickS3. Scans for Bluetooth Low Energy advertisements from known smart glasses (Meta Ray-Ban, Snap Spectacles, etc.) and alerts you when they're nearby.

## Why hardware?

- **iOS can't do this** — Apple's Core Bluetooth blocks access to raw manufacturer data in BLE advertisements
- **Always-on** — no app to launch, just clip it to your keychain
- **Phone-agnostic** — works regardless of what phone you carry (or if you carry one at all)
- **Discreet** — glancing at a keychain is less conspicuous than opening a "glasses detector" app

## How it works

The detector passively listens for BLE advertising packets and matches them against a configurable database of known smart glasses signatures. Instead of simple manufacturer ID matching (which causes false positives with VR headsets), it uses a **confidence scoring system** that combines:

- Manufacturer ID (e.g., Luxottica `0x0D53` = strong glasses indicator)
- Service UUIDs
- Device name prefixes
- Advertisement payload length characteristics

Each match component contributes a weighted score. Detections are classified as **LIKELY**, **POSSIBLE**, or **MAYBE** (for ambiguous manufacturers like generic Meta IDs that could be Ray-Bans or Quest headsets).

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

Scan timing, confidence weights, and RSSI thresholds are also defined in `generate_config.py` in the `generate_config()` function.

## Building

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
    "category": "glasses",
    "name_prefixes": ["BrandName"],           # BLE device name prefixes
    "service_uuids": [],                       # from nRF Connect scan
    "adv_data_length_range": [0, 0],          # from nRF Connect scan
    "notes": "Description",
    "has_camera": True,
}
```

Then run `python3 generate_config.py` to regenerate `data/config.json`.

Set `category` to `"glasses"` for confirmed smart glasses, `"glasses_and_other"` for companies that also make non-glasses BLE devices (requires `name_prefixes` to avoid false positives), or `"meta_ambiguous"` for IDs shared across product lines. The display uses category to choose alert colors.

## Research needed

The `adv_data_length_range` and `service_uuids` fields are mostly empty — they need to be populated by sniffing actual devices. If you have access to any of these glasses, run an nRF Connect scan and note:

- Full advertisement payload hex dump
- Service UUIDs advertised
- Payload length
- Advertisement interval timing

PRs with real-world scan data welcome — update the relevant entry in `generate_config.py` (not `data/config.json`).

## License

MIT
