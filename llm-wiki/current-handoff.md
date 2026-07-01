# Current Handoff

Last updated: 2026-07-01.

This page is public/GitHub-safe. Shared operational bench/backend details belong in ignored development-group notes under `llm-wiki/private/dev-group/`; local machine and Codex process state belongs under `llm-wiki/private/personal-agent/`.

## Branch and Git State

- Working branch: `dev/cb_tracker`.
- Do not stage ignored private files, local build outputs, diagnostic dumps, or `.codex-local/` artifacts.
- Several firmware and documentation changes are currently in the worktree for issue #2 provisioning, issue #3 adaptive uplinks, diagnostic logging, and receiver-side logging.

## Build Baseline

- The project now builds locally with Docker against Zephyr `v4.4.1`.
- Default command:

```powershell
.\scripts\dev\zephyr-docker.ps1 build
```

- The helper also supports clean builds and overlay builds, including the temporary provisioning shell configuration.
- Current production images have built successfully and run on the target hardware.

## Firmware Work Completed

### LoRaWAN Callback Stability

- Fixed a real LoRaWAN downlink crash by moving the registered downlink callback storage from stack lifetime to static lifetime.
- Zephyr retains the callback pointer after registration, so stack storage was unsafe.

### Provisioning / Per-Device Config

- Added a first settings/NVS-backed tracker settings module.
- LoRaWAN OTAA identity is now loaded from settings instead of requiring a separate firmware build per device.
- A temporary provisioning-shell build can write, inspect, or clear tracker provisioning state.
- The default production firmware keeps UART as a read-only log console.

### Adaptive Uplink Cadence

- Added first movement-aware position uplink policy.
- The current LoRaWAN position payload is unchanged: port `4`, packed `int32_t lat`, `int32_t lon`, and `uint16_t hdop`.
- Current intended intervals:
  - stationary: `120 s`;
  - active: `30 s`;
  - moving: `10 s`.
- Motion classification uses GNSS speed plus accelerometer data.
- Accelerometer handling is orientation-independent: it uses vector magnitude and vector delta, not a fixed `Z == gravity` assumption.
- GNSS fixes are gated by satellite count and HDOP.
- A drift guard suppresses stationary GNSS noise so parked GPS drift does not immediately look like real movement.

### Storage and Diagnostic Logging

- The target has 4 MB flash.
- The board layout now uses 1 MiB primary and secondary application slots.
- A 640 KiB `diagnostic-log` partition was added.
- The existing settings storage partition location was preserved so provisioned settings survive normal app flashes.
- The firmware writes compact circular diagnostic records for scheduled position-uplink decisions.
- The host decoder converts raw diagnostic flash dumps into CSV, GeoJSON, and an HTML map.

### Receiver-Side Visibility

- Added `scripts/dev/chirpstack_live_map.py`, a generic ChirpStack application-event logger.
- It decodes the current port `4` payload and writes JSON, CSV, GeoJSON, and a small Leaflet map.
- The source-controlled script has no private backend defaults.

## Latest Validation Summary

- Stationary bench testing showed normal boot, LoRaWAN join, GNSS fixes, read-only UART logs, and stationary `120 s` heartbeat behavior.
- A later movement run produced real movement data:
  - moving state was reached;
  - `10 s` moving cadence was recorded;
  - maximum logged speed was about `55 km/h`;
  - one moving window sent successfully throughout.
- A separate movement window exposed a retry bug:
  - LoRaWAN sends failed for a short period with return code `-111`;
  - after the first failure, the firmware retried much faster than the selected interval;
  - likely cause: `last_uplink_timestamp` is only updated on successful sends, so failed sends can retry on every GNSS event.

## Current RF/Retry and Battery-Diagnostic Work

- The firmware has been updated locally so failed position sends consume the selected uplink cadence slot instead of retrying on every GNSS event.
- ADR is disabled at LoRaWAN start for now, matching the current moving-tracker hypothesis that ADR can leave the device with stale RF settings after moving away from the gateway.
- Position uplinks now use a periodic confirmed message as a link check. The current interval is `600 s`; other position uplinks remain unconfirmed.
- Confirmed uplink timeouts no longer immediately mark the local LoRaWAN session down. Confirmed attempts are rate-limited by attempt time; hard repeated send failures still trigger rejoin handling.
- If the local LoRaWAN link is marked down and cannot recover for `30 min`, the firmware performs a cold reboot to reset radio/MAC state.
- Local diagnostic records are now version `6` and `48` bytes. Fields capture:
  - current LoRaWAN datarate when the stack reports one;
  - whether ADR was enabled;
  - whether the uplink was confirmed;
  - last downlink RSSI/SNR when a downlink callback has been observed;
  - IO34 battery voltage in millivolts when the ADC conversion is in range;
  - IO34 ADC pin millivolts and a saturation flag when the ADC conversion is clipped.
- Zephyr's public LoRaWAN API used here does not expose current TX power directly, so receiver-side RSSI/SNR plus datarate/ADR/downlink information is the current observable proxy.
- Battery-voltage monitoring is diagnostic-only data. Local code reads ESP32 IO34 / ADC1 channel 6 with the TrackerD-LS schematic's `100k` / `470k` divider scale when the ADC conversion is in range. A live external measurement around `4.09 V` showed the current calibrated ESP32 ADC conversion path can saturate; saturated readings are no longer reported as valid pack voltage.
- The LoRaWAN position payload is unchanged.
- The host diagnostic decoder was updated to read old `40` byte v1 records and `48` byte v2/v3/v4/v5/v6 records.
- The ChirpStack live-map script now also preserves frequency and LoRa modulation metadata when the application event includes it.
- The live-map script supports multiple DevEUIs, writes `latest_by_device.json` and `device_events.json`, colors devices separately, and defaults the browser view to decoded positions from the last 24 hours. `device_events.json` shows latest join/uplink events even when a payload is not decoded as a current firmware position point. The 24 hour filter is a view filter only; retained JSON/CSV/GeoJSON history still follows `--max-points`.
- Local validation completed:
  - Python syntax checks passed;
  - the updated decoder successfully decoded the existing `1394` record movement dump.
- Docker Desktop was started and the Zephyr `v4.4.1` production build passed. The generated `zephyr.bin` size was `258048` bytes, SHA256 `BE8FBFDD95D2838F35DD49B9AE34E1E435B8A82CBD34EF876215750947D08DBD`.
- The battery-ADC diagnostic comparison image was flashed to the bench at app offset `0x1000`, preserving settings. Exact bench paths and device mapping are private.
- A 130 second post-flash console capture showed:
  - normal boot;
  - provisioned LoRaWAN identity loaded from settings;
  - both candidate battery ADC paths initialized in the comparison build;
  - `LoRaWAN ADR disabled`;
  - successful OTAA join;
  - datarate callback `DR_0`;
  - one confirmed active position uplink;
  - zero `FATAL`, `ASSERT`, `lorawan_send failed`, or `position uplink failed` markers in the filtered capture.
- New v4 diagnostic records were dumped and decoded. Two comparison records showed GPIO35 near ground and GPIO34 in the expected battery-sense range; GPIO34 is the useful battery-voltage path.
- The TrackerD-LS v1.3 schematic confirms IO34 as the battery ADC path with a `100k` / `470k` divider. The current v6 diagnostic format keeps `battery_mv` only for in-range readings and adds `battery_pin_mv` plus `battery_saturated`.
- The local v6 build passed. Generated `zephyr.bin` size was `258048` bytes, SHA256 `C61929741707486AF4396A5B6941EB30551A9210226CD35BA79470C8231BD001`.
- The v6 image has now been flashed to one provisioned development tracker, preserving settings. A short boot capture showed settings loaded, diagnostic log ready, IO34 battery monitor ready, ADR disabled, OTAA join success, and datarate `DR_0`. Exact device mapping and bench paths are private.
- Additional stock TrackerD-LS units are now on the bench. Exact USB serials, ChirpStack mapping observations, stock backup details, and flash cautions are documented only in ignored private development-group notes.

## Next Best Steps

1. Run another movement test and decode the diagnostic log.
2. Check whether send failures are now rate-limited and whether confirmed link checks produce downlink/RF evidence.
3. Tune motion thresholds only after reviewing real movement traces.
4. Decide later whether the LoRaWAN payload should include speed or whether speed should remain diagnostic-log-only.
5. Keep public docs sanitized; keep bench/backend runbooks and personal agent state in ignored private notes.
