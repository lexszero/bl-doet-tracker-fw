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
- The `diagnostic-log` partition is reserved for a future compact on-device journal. The current firmware does not write to it yet.
- The default firmware logs to UART with `CONFIG_LOG_BACKEND_UART=y`.
- The default firmware does not enable `CONFIG_FILE_SYSTEM`, `CONFIG_DISK_ACCESS`, `CONFIG_SDHC`, `CONFIG_SDMMC_STACK`, `CONFIG_FAT_FILESYSTEM_ELM`, or LittleFS.

## Validation

2026-06-27:

- Clean production build passed with the 1 MiB slot layout; `zephyr.bin` size was `258048` bytes.
- Clean provisioning-shell build passed with the 1 MiB slot layout; `zephyr.bin` size was `323584` bytes.
- The production image was flashed to the hardware bench.
- A 90 second boot capture showed existing LoRaWAN settings still loaded from NVS, OTAA join succeeded, one position uplink was sent, and no `FATAL` or `ASSERT` markers appeared.

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

For production firmware, avoid writing every Zephyr log line to internal flash. Internal flash is better suited to a compact diagnostic journal: boot/reset reason, LoRaWAN join results, uplink result counters, motion-state transitions, GNSS fix quality, drift-guard activation, and selected error codes.

If an on-device journal is implemented, use the `diagnostic-log` flash area so high-churn logs cannot crowd out provisioning settings or LoRaWAN NVM. A compact binary circular buffer is likely safer than text logs. Dump and clear commands can live in a debug/provisioning shell build instead of the default read-only UART firmware.

## Open Questions

- Exact binary record format, retention policy, and wear-leveling strategy for the `diagnostic-log` partition.
- Whether the remaining OTA-oriented partitions are the right shape for future issue `#4`.
- Whether crash capture should use the existing 4 KiB `coredump` partition; this is useful for faults, not for routine logging.
