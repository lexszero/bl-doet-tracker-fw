# Current Handoff

Last updated: 2026-07-03.

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
- The current LoRaWAN position payload layout remains port `4`, packed `int32_t lat`, `int32_t lon`, and `uint16_t hdop`.
- `hdop=65535` (`0xffff`) is reserved as a stale/no-current-fix heartbeat marker. In that case latitude/longitude are the last known usable position, not a fresh fix.
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
- The earlier `30 min` reboot fallback has been replaced by staged recovery handling:
  - repeated hard position-send failures mark the link down and force the LoRaWAN thread back into join/rejoin handling;
  - the main loop keeps processing GNSS and diagnostic records while not joined;
  - software last-resort reboots for LoRaWAN non-join/non-recovery are currently disabled;
  - recovery wait records are still logged periodically so long no-link periods remain visible.
- LoRaWAN join retry backoff is now motion-aware:
  - normal stationary/unknown retry backoff still grows up to `300 s`;
  - active motion caps retry delay at `60 s`;
  - moving caps retry delay at `30 s`;
  - the retry sleep checks motion in short chunks, so movement can shorten an already-running long backoff.
- Local diagnostic records are now version `7` and `48` bytes. Version `7` keeps the version `6` binary layout and adds explicit LoRaWAN link-event result codes. Fields capture:
  - current LoRaWAN datarate when the stack reports one;
  - whether ADR was enabled;
  - whether the uplink was confirmed;
  - last downlink RSSI/SNR when a downlink callback has been observed;
  - IO34 battery voltage in millivolts;
  - IO34 ADC pin millivolts and a saturation flag field retained for clipped/fallback readings.
- Link-event records now cover boot, init attempt/result, join attempt/result, link marked down, and recovery wait. Older logs may still contain last-resort reboot records. The host decoder labels records as `position` or `link_event` and avoids plotting locationless link events at `0,0`.
- Zephyr's public LoRaWAN API used here does not expose current TX power directly, so receiver-side RSSI/SNR plus datarate/ADR/downlink information is the current observable proxy.
- Battery-voltage monitoring is diagnostic-only data. The TrackerD-LS v1.3 schematic confirms `BAT+ -> 100k -> IO34/PA2 -> 470k -> GND`, so the firmware scales IO34 by `(100 + 470) / 470`. Stock Tracker_109 reports about `4002 mV`; the Zephyr ADC helper path clipped at about `3088 mV` pack voltage. The current experimental firmware bypasses Zephyr's clipped millivolt helper and reads IO34 with the ESP HAL ADC path plus Espressif line-fitting calibration.
- The LoRaWAN position payload remains the same 10 byte binary layout, with `hdop=65535` reserved for stale/no-current-fix heartbeats using the last known usable position.
- The host diagnostic decoder was updated to read old `40` byte v1 records and `48` byte v2/v3/v4/v5/v6/v7 records.
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
- The local v6 build passed. Generated `zephyr.bin` size was `258048` bytes, SHA256 `C61929741707486AF4396A5B6941EB30551A9210226CD35BA79470C8231BD001`.
- The v6 image has now been flashed to one provisioned development tracker, preserving settings. A short boot capture showed settings loaded, diagnostic log ready, IO34 battery monitor ready, ADR disabled, OTAA join success, and datarate `DR_0`. Exact device mapping and bench paths are private.
- The newer ESP-HAL ADC experiment build passed and was flashed to another provisioned development tracker. Boot capture showed `raw=4095`, IO34 `3441 mV`, scaled battery `4173 mV`, GNSS fix, OTAA join, downlink, and a confirmed port `4` position uplink. Exact device mapping and bench paths are private.
- The v7 link-recovery build passed on 2026-07-02. Generated `zephyr.bin` size was `258048` bytes, SHA256 `DF87D3B1FDF2536FF92DD1C23F7470BC5231A2A7E4DAAE91964685D6D7064012`.
- The v7 image was flashed app-only to the permanent bench tracker, preserving settings. A short boot capture showed settings loaded, diagnostic log ready, IO34 battery monitor ready, GNSS fix, OTAA join success, downlink callback, datarate `DR_0`, and one confirmed port `4` position uplink. No `FATAL`, `ASSERT`, send-failure, or last-resort reboot markers appeared in the filtered capture.
- A post-flash diagnostic dump decoded successfully. The partition still contained older records, but the new v7 records included `link_boot`, `link_init_attempt`, `link_init_ok`, `link_join_attempt`, `link_recovery_wait`, `link_join_success`, `not_joined`, and `sent` records, confirming that link-state events and normal position records decode together.
- The motion-aware no-reboot recovery build passed on 2026-07-03. Generated `zephyr.bin` size was `258048` bytes, SHA256 `BBB02AD37AD5C4F057EF8A246FA18C28F70DE374F5320BFEF805D302169D387B`.
- Additional stock TrackerD-LS units are now on the bench. Exact USB serials, ChirpStack mapping observations, stock backup details, and flash cautions are documented only in ignored private development-group notes.

## Next Best Steps

1. Use one tracker as a permanent bench/recovery-test target and one tracker as the daily movement/poor-coverage test target.
2. Before each run, note the expected gateway condition and avoid unnecessary resets.
3. After each run, compare receiver-side events against the local diagnostic dump:
   - local `sent` plus receiver point means normal path;
   - local `sent` without receiver point points to RF/gateway/backend visibility;
   - `send_failed` points to local LoRaWAN send failure;
   - `not_joined` plus `link_recovery_wait` means recovery is active while the main loop still runs;
   - no new local records points to firmware, GNSS event flow, flash write, or power.
4. Run a gateway-off / poor-coverage test and decode the diagnostic log to confirm that GNSS handling and diagnostic logging continue while not joined.
5. Check whether send failures are rate-limited and whether confirmed link checks produce downlink/RF evidence.
6. Tune motion thresholds only after reviewing real movement traces.
7. Decide later whether the LoRaWAN payload should include speed or whether speed should remain diagnostic-log-only.
8. Keep public docs sanitized; keep bench/backend runbooks and personal agent state in ignored private notes.
