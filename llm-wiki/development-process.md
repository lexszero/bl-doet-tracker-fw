# Development Process

This page captures development-process knowledge that future AI agents should preserve.

## Device access

Firmware validation needs access to real hardware.

The project operator needs to provide one of these before agents can validate hardware behavior:

- Physical access to an actual tracking device.
- Remote access to a tracking device through an approved test bench.

## Remote test bench

- Do not commit hostnames, ports, usernames, private key paths, device serial numbers, or access commands for a live bench.
- Keep bench-specific access notes in ignored local files, for example under `llm-wiki/private/dev-group/`.
- Practical workflow: build firmware locally or in CI, copy the build artifacts to the remote host, and use the remote host only for flashing, serial console, and hardware validation.

Do not assume credentials, protocols, device paths, or deployment commands until the project operator provides them through an approved private channel.

## Remote serial validation

2026-06-21: The remote test system exposed one tracker device as `/dev/ttyACM0`.

Useful serial command:

```shell
picocom -b 115200 /dev/ttyACM0
```

If the device is silent, toggle DTR in `picocom` with `Ctrl-A` then `t`. The captured boot log showed an ESP32 bootloader, Zephyr OS build `v4.2.0-rc3`, app banner `DoET :: Tracker 1.0.0`, GNSS fix acquisition, SX127x radio detection, and successful LoRaWAN OTAA join.

2026-06-22: another serial capture from the remote test bench confirmed:

- Use stable `/dev/serial/by-id` paths when possible, but keep exact bench serial identifiers in ignored private notes.
- The device is an ESP32 chip revision `v1.1` with 4 MB flash; Zephyr warns this revision is unsupported, and the board defconfig currently enables `CONFIG_ESP32_USE_UNSUPPORTED_REVISION`.
- Boot image: Zephyr OS build `v4.2.0-rc3`, app banner `DoET :: Tracker 1.0.0`.
- GNSS power domain is enabled at boot; `gnss_get_fix_rate()` currently returns `-88` before fixes are received.
- LIS2DH/LIS3DH-class accelerometer initializes (`lis2dh` log, `fs=2`, `odr=0x4`).
- SX127x radio is detected (`SX127x version 0x12 found`).
- LoRaWAN NVM is restored from NVS at boot, including DevNonce/JoinNonce, and OTAA join succeeds.
- The observed join used DR_0 with max payload 51 bytes.
- GNSS fixes arrive at roughly 1 Hz after boot. The first observed fix was `ESTIMATED_FIX`; subsequent fixes were `GNSS_FIX` / `GNSS_SPS` with 7-12 satellites and HDOP around 0.8-1.1.
- In the stationary capture, reported speed was often nonzero before settling to 0, and latitude drifted by roughly 13 m over the first 20 seconds after fix acquisition. Treat raw GPS position/speed as jittery enough to need filtering or motion gating.
- The Zephyr shell is enabled. `log disable` successfully stops runtime log spam, making shell inspection practical.
- `device list` confirms all expected tracker devices are ready: `uart0`, `uart1`, `i2c0`, `spi3`, `led_r`, `led_g`, `led_b`, `GNSS power`, `quectel-L76k`, `lora@0`, and `lis3dh@19`.
- Earlier builds showed only LoRaWAN NVM keys under `lorawan/nvm/*`. After the issue #2 settings work, product settings live under `tracker/*` once provisioned. Avoid dumping `lorawan/nvm/Crypto` or `lorawan/nvm/SecureElement` in shared logs because they may include session or key material.
- `sensor get lis3dh@19` works. One stationary sample returned acceleration around `(3.56, 6.24, 6.59) m/s^2`, with total magnitude close to 9.8 m/s^2, so the accelerometer path is producing plausible gravity readings.
- Broad `sensor attr_get lis3dh@19` is not useful on this firmware: the shell iterates many generic channels/attributes and the driver returns `-88` (`ENOTSUP`) for unsupported attributes, producing a large amount of noise. Prefer targeted reads or firmware-side sensor configuration.
- `settings -h` shows list/read/write/delete support. `lora -h` exposes raw LoRa radio config/send/recv/test commands, not LoRaWAN application commands.

2026-06-27 issue #3 bench finding:

- A stationary bench run with Zephyr `v4.4.1` and the read-only console showed GNSS-reported speed spikes above `5 m/s` while the accelerometer stayed close to gravity with small vector deltas.
- Those false speed reports were accompanied by large altitude jumps, despite good-looking satellite count and HDOP.
- Do not trust GNSS speed alone for movement detection on this hardware. Prefer accelerometer-gated motion state and treat GNSS-only speed plus still acceleration plus altitude jumps as position drift.
- The first adaptive policy now uses accelerometer polling, stationary/active/moving intervals, event-bit clearing, latest-sample GNSS callbacks, and a GNSS drift guard. Real moving-vehicle traces are still required for threshold tuning.

2026-06-27 LoRa backend validation finding:

- ChirpStack gateway-frame inspection is useful when application events are missing. It confirmed the current firmware's live RF packets reached a gateway even though they were not shown under the expected application device.
- Application event history can validate payload decoding, but always confirm the firmware DevEUI / JoinEUI / AppKey match the ChirpStack device being inspected.
- The source now supports persistent LoRaWAN identity settings under `tracker/lorawan/*`, but a device is not actually provisioned until those settings are written to its NVS storage.
- The unprovisioned fallback identity is still the old placeholder value, now centralized in `app/src/settings.c`. Backend validation against a named ChirpStack device needs real provisioned credentials or a matching temporary ChirpStack device.
- Use the temporary provisioning shell build when credentials need to be written over serial. This build enables `CONFIG_TRACKER_PROVISIONING_MODE`, so it does not start the normal GNSS/LoRaWAN tracker runtime:

```shell
tracker provision status
tracker provision set <dev_eui_hex> <join_eui_hex> <app_key_hex>
tracker provision clear
```

- The provisioning command stores DevEUI, JoinEUI, and AppKey but never prints the AppKey. Keep real AppKeys in ignored/private notes only.
- ChirpStack v4 exposes multiple root-key fields. For the validated TrackerD device profile (`LORAWAN_1_0_3`), use the ChirpStack API `nwk_key` value as the firmware's current single OTAA key. Do not use `app_key` / `gen_app_key` for that profile unless the firmware is later extended for LoRaWAN 1.1 separate root keys.

## Validation expectations

- Use local builds and tests for fast feedback when hardware is unavailable.
- Treat hardware behavior as unverified until tested on an actual device or through the remote device path.
- When a change cannot be hardware-validated, say so in the handoff and note what still needs to be checked on-device.
