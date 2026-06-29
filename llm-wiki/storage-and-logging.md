# Storage and Logging

Last updated: 2026-06-27.

This page captures what is currently known about persistent storage and longer-term logging on the TrackerD-LS Zephyr firmware.

## Verified Storage

- The bench device identifies as ESP32-PICO-D4 revision `v1.1`.
- `esptool flash-id` detects embedded SPI flash size `4MB`.
- The board now uses the repo-local 4 MB layout in [trackerd_ls_partitions_4m.dtsi](../boards/dragino/trackerd_ls/trackerd_ls_partitions_4m.dtsi), based on Zephyr's Espressif AMP layout.
- The generated partition layout includes:
  - `mcuboot`: `0x1000`, 60 KiB;
  - `sys`: `0x10000`, 64 KiB;
  - `image-0`: `0x20000`, 1024 KiB;
  - `image-1`: `0x120000`, 1024 KiB;
  - `image-0-appcpu`: `0x220000`, 448 KiB;
  - `image-1-appcpu`: `0x290000`, 448 KiB;
  - `image-0-lpcore`: `0x300000`, 32 KiB;
  - `image-1-lpcore`: `0x308000`, 32 KiB;
  - `diagnostic-log`: `0x310000`, 640 KiB;
  - `storage`: `0x3b0000`, 192 KiB;
  - `image-scratch`: `0x3e0000`, 124 KiB;
  - `coredump`: `0x3ff000`, 4 KiB.

## Current Firmware Use

- The default firmware enables `CONFIG_SETTINGS`, `CONFIG_SETTINGS_NVS`, `CONFIG_NVS`, `CONFIG_FLASH`, and `CONFIG_FLASH_MAP`.
- Settings use Zephyr's NVS backend on `storage_partition`.
- Current generated config uses `CONFIG_SETTINGS_NVS_SECTOR_COUNT=8` and `CONFIG_SETTINGS_NVS_SECTOR_SIZE_MULT=1`, so the settings backend uses up to eight flash erase sectors from the `storage` partition. On the current ESP32 flash this is expected to be about 32 KiB.
- Product settings currently stored there include `tracker/lorawan/*`.
- Zephyr LoRaWAN MAC/session NVM also stores state through the settings subsystem under `lorawan/nvm/*`.
- The `storage` partition intentionally kept its original offset (`0x3b0000`) when the 1 MiB slot layout was introduced, so flashing the app image over the previous layout should not erase or move already-provisioned settings.
- The `diagnostic-log` partition is now used by the firmware for compact binary uplink-decision records.
- The default firmware logs to UART with `CONFIG_LOG_BACKEND_UART=y`.
- The default firmware does not enable `CONFIG_FILE_SYSTEM`, `CONFIG_DISK_ACCESS`, `CONFIG_SDHC`, `CONFIG_SDMMC_STACK`, `CONFIG_FAT_FILESYSTEM_ELM`, or LittleFS.

## Validation

2026-06-27:

- Clean production build passed with the 1 MiB slot layout; `zephyr.bin` size was `258048` bytes.
- Clean provisioning-shell build passed with the 1 MiB slot layout; `zephyr.bin` size was `323584` bytes.
- The production image was flashed to the hardware bench.
- A 90 second boot capture showed existing LoRaWAN settings still loaded from NVS, OTAA join succeeded, one position uplink was sent, and no `FATAL` or `ASSERT` markers appeared.
- The diagnostic circular log implementation was added and the default production build passed again. The production artifact remained `258048` bytes, SHA256 `6283DF36A54FFDBE7D5E61AF884E9567BE330DE080E98CD6DA4DEC6863686C37`.
- The provisioning-shell build also passed after the diagnostic log source was added. The provisioning artifact remained `323584` bytes, SHA256 `F81FB7F4A85795A1EB21C859A1570A63BCFD053F1D3E74D1F1514A6E6A8AD40C`.
- Host decoder validation passed for an empty/no-record input and for a synthetic valid record.
- The diagnostic-log production image was flashed to the hardware bench, the diagnostic partition was erased, and a ten minute run produced a raw flash export that decoded to six valid records. The run joined LoRaWAN once, sent five position uplinks in console logs, and had zero `FATAL`/`ASSERT` markers. Decoded records showed one initial `active` record with `30 s` interval and five `stationary` records with `120 s` interval.

## SD Card Status

- The board DTS has an alias/chosen entry pointing at `sdhc1`, but the generated devicetree still leaves both ESP32 SDHC slots disabled.
- The board YAML advertises `nvs`, not SD/MMC or filesystem support.
- No SD card slot, SD-card wiring, or working SD driver path has been verified in this repository.
- Treat removable SD-card logging as unavailable until the physical board or schematic confirms a slot and a Zephyr SDHC test build proves it works.

## Practical Longer-Term Logging

Use bench-side serial capture for raw long logs. It avoids flash wear and keeps full UART text:

```shell
tracker-console 2>&1 | tee tracker-serial.log
```

For a timed run on a bench host:

```shell
timeout -s INT 6h tracker-console 2>&1 | tee tracker-serial-6h.log
```

For production firmware, avoid writing every Zephyr log line to internal flash. Internal flash is better suited to compact diagnostic records.

## Diagnostic Circular Log

The firmware now includes `CONFIG_TRACKER_DIAGNOSTIC_LOG=y` by default. It appends one compact binary record for each scheduled position uplink decision:

- successful port `4` position sends;
- failed port `4` position send attempts;
- `not_joined` decisions, where GNSS/motion says the tracker would send a position at the selected interval, but LoRaWAN is not joined yet.

It intentionally does not log every GNSS fix. At a 1 Hz GNSS rate, internal flash would fill too quickly and would add unnecessary erase/write churn.

Each record is `40` bytes and includes:

- sequence number;
- boot uptime in milliseconds;
- packed GNSS UTC date/time when available;
- latitude and longitude in the same compressed integer scale used by the LoRaWAN payload;
- GNSS speed in cm/s;
- HDOP;
- selected send interval in seconds;
- motion state;
- send result and return code;
- satellite count;
- flags for accelerometer validity/motion, drift guard, motion resume, and UTC validity.

The `640 KiB` partition is split into `160` erase sectors of `4096` bytes. Each sector stores `102` records, leaving a small unused tail so records never cross sector boundaries. Total capacity is `16320` records.

Approximate retention:

| Position-send cadence | Retention |
| --- | ---: |
| 10 s moving cadence | 45.3 hours / 1.9 days |
| 30 s active cadence | 5.7 days |
| 120 s stationary cadence | 22.7 days |

The ring resumes after reboot by scanning valid records and appending after the highest sequence number. When it wraps, it erases one `4 KiB` sector at a time before reusing it.

## Dump and Decode Workflow

Read the diagnostic partition from the device:

```shell
esptool.py -p /dev/ttyACM0 read_flash 0x310000 0xA0000 diag-log.bin
```

Decode on the PC:

```powershell
python scripts\dev\decode_diag_log.py diag-log.bin --out-dir diag-log-out
```

The decoder writes:

- `diagnostic-log.csv` for spreadsheet/manual analysis;
- `diagnostic-log.geojson` for GIS tools;
- `diagnostic-log-map.html` with an embedded route overlay and Leaflet/OpenStreetMap tiles.

To clear only the diagnostic log partition for a fresh run:

```shell
esptool.py -p /dev/ttyACM0 erase_region 0x310000 0xA0000
```

Do not erase `0x3b0000` unless intentionally clearing provisioning/settings storage.

## Open Questions

- Whether the remaining OTA-oriented partitions are the right shape for future issue `#4`.
- Whether crash capture should use the existing 4 KiB `coredump` partition; this is useful for faults, not for routine logging.
- Whether a future debug/provisioning shell should expose `tracker diag status`, `tracker diag dump`, and `tracker diag clear`; the current workflow uses raw `esptool` dumps and PC-side decoding.
