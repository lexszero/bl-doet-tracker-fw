# GitHub Issues Snapshot

This page summarizes the public GitHub issues Lex pointed to from the assignment context.

Recheck GitHub before starting implementation work because issue state, comments, or priorities can change.

## Source

- Repository issues: https://github.com/lexszero/bl-doet-tracker-fw/issues
- Snapshot date: 2026-06-22
- Visible open issues at snapshot: `#2`, `#3`, and `#4`.

## Issue #3: Investigate optimal uplink interval and GPS accuracy

- URL: https://github.com/lexszero/bl-doet-tracker-fw/issues/3
- Opened by Lex on 2026-05-25.
- Status at snapshot: open.
- Labels at snapshot: none.

The core task is to improve vehicle position quality without sending excessive updates into the LoRaWAN/network and time-series stack. Lex notes that commercial tracker intervals around 30-60 seconds are too slow for Bolderland Burn scale when vehicles are moving.

Important implementation direction:

- Use more frequent updates for moving vehicles.
- Allow slower updates for vehicles operating in one area.
- Allow much slower updates for stationary vehicles.
- Consider keeping the GPS receiver powered so it can maintain a long-term satellite lock; power is less constrained because devices may use large batteries or vehicle electrical supply.
- Investigate filtering, motion detection, or sensor fusion so movement is detected quickly while GPS jitter is not treated as real travel.
- A simpler first approach could use an onboard accelerometer as the motion trigger, then rely on a warm GPS receiver for better position quality.
- Before implementing from scratch, check whether existing libraries or Zephyr-friendly patterns solve part of this.

## Issue #2: Implement settings storage

- URL: https://github.com/lexszero/bl-doet-tracker-fw/issues/2
- Opened by Lex on 2026-05-25.
- Status at snapshot: open.
- Labels at snapshot: `enhancement`.

The firmware needs runtime-configurable LoRaWAN and Wi-Fi parameters instead of requiring constants in headers and reflashing each device.

Important implementation direction:

- Store settings in non-volatile memory or another suitable persistent storage mechanism.
- Evaluate Zephyr's settings subsystem first, but keep a simpler option open if it fits the product better.
- Support over-the-air configuration, for example HTTP over Wi-Fi.
- Support initial provisioning with fixed management Wi-Fi credentials and per-device LoRaWAN EUIs/keys so devices can be provisioned into ChirpStack in batches.
- Support field parameter changes, reset-to-defaults behavior, and fallback AP mode with known credentials.
- Fallback AP mode should be activatable on demand, for example with a button, so Wi-Fi does not need to run continuously.
- Optional development convenience: expose settings through the Zephyr shell/CLI.

## Issue #4: OTA firmware update [low priority]

- URL: https://github.com/lexszero/bl-doet-tracker-fw/issues/4
- Opened by Lex on 2026-05-25.
- Status at snapshot: open.
- Labels at snapshot: `enhancement`.
- Priority note from title: low priority.

OTA updates would reduce field maintenance pain when USB access is hard or unavailable. Lex specifically called out TrackerD enclosures as painful to open at fleet scale.

Important implementation direction:

- Prefer standard Zephyr ecosystem components if OTA is implemented.
- The need is basic field update capability, not a custom or feature-heavy updater.
- It may be acceptable to skip OTA if the firmware reaches a reliable "good enough" state and the team accepts the risk of not updating devices during the event.

## Work ordering notes

- `#3` is the most central to the assignment goal of improving GPS tracking quality.
- `#2` is central to field deployability and batch provisioning.
- `#4` is explicitly lower priority, but it affects last-minute field fixes and maintenance cost.
