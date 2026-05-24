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

Edit `data/config.json` to customize:

- **Scan timing** — how long to scan, how often, RSSI threshold
- **Confidence weights** — tune how much each signal type contributes
- **Device signatures** — add new glasses models as they come out

Upload the config to SPIFFS:
```
pio run -t uploadfs
```

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

## Controls

- **Button A** — Force immediate rescan
- **Button B** — Cycle RSSI threshold (-75 → -85 → -95 → -50 → ...)

## Adding new glasses

Add entries to the `devices` array in `data/config.json`:

```json
{
  "id": "new_glasses",
  "label": "Brand Name",
  "category": "glasses",
  "manufacturer_ids": ["0xABCD"],
  "service_uuids": ["0000abcd-0000-1000-8000-00805f9b34fb"],
  "name_prefixes": ["BrandName"],
  "adv_data_length_range": [20, 40],
  "notes": "Description"
}
```

Set `category` to `"glasses"` for confirmed smart glasses, or a custom value like `"meta_ambiguous"` for IDs shared across product lines. The display uses category to choose alert colors.

## Research needed

The `adv_data_length_range` and `service_uuids` fields are mostly empty — they need to be populated by sniffing actual devices. If you have access to any of these glasses, run an nRF Connect scan and note:

- Full advertisement payload hex dump
- Service UUIDs advertised
- Payload length
- Advertisement interval timing

PRs with real-world scan data welcome.

## License

MIT
