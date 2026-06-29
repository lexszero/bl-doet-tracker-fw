# Current Handoff

Last updated: 2026-06-29.

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

## Next Best Steps

1. Fix send retry/backoff behavior so failed LoRaWAN sends are rate-limited.
2. Rebuild and flash the production image.
3. Run another movement test and decode the diagnostic log.
4. Tune motion thresholds only after reviewing real movement traces.
5. Decide later whether the LoRaWAN payload should include speed or whether speed should remain diagnostic-log-only.
6. Keep public docs sanitized; keep bench/backend runbooks and personal agent state in ignored private notes.
