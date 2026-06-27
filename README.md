# Bolderland DoET Tracker Firmware

Firmware for ESP32-based vehicle trackers used by the Bolderland Burn DoET tracking stack.

The current primary target is the Dragino TrackerD-LS style ESP32 tracker with GNSS, SX127x LoRa radio, LIS2DH/LIS3DH-class accelerometer, and Zephyr shell over UART.

## Status

This repository is still early firmware. It currently behaves like a Zephyr out-of-tree application with custom board, driver, and library code. Treat the code as a tracker proof of concept that is being turned into a field-deployable firmware.

## Build Environment

The preferred development path is local Docker on Windows/PowerShell. Remote hardware hosts should be used only for flashing, serial logs, and hardware validation.

Required local tools:

- Docker Desktop
- Git
- PowerShell

The manifest pins Zephyr to `v4.4.1`. The Docker helper defaults to the compatibility SDK volume used during early bring-up, but the modern target is the SDK included in the official Zephyr Docker image.

Initialize or update the Zephyr workspace:

```powershell
.\scripts\dev\zephyr-docker.ps1 init
```

Build the tracker firmware:

```powershell
.\scripts\dev\zephyr-docker.ps1 build -Pristine
```

Build artifacts are copied to:

```text
.codex-local/artifacts/trackerd_ls/
```

Expected main artifact:

```text
.codex-local/artifacts/trackerd_ls/zephyr.bin
```

Optional temporary provisioning-shell build for writing LoRaWAN identity settings:

```powershell
.\scripts\dev\zephyr-docker.ps1 build -Pristine -ExtraConf app\provisioning.conf -BuildDir build/trackerd_ls_provision -ArtifactName trackerd_ls_provision
```

That image disables the normal tracker runtime and exposes `tracker provision status`, `tracker provision set <dev_eui_hex> <join_eui_hex> <app_key_hex>`, and `tracker provision clear` on the serial shell. For ChirpStack LoRaWAN 1.0.x devices, the API field to pass as `<app_key_hex>` is usually `nwk_key`. Do not commit real LoRaWAN keys or private bench details.

## Current Compatibility Build

During initial remote-device debugging, the working build workspace used Zephyr `v4.2.0-rc3` with Zephyr SDK `0.16.8`. If you need to reproduce that temporary path, use the existing local Docker volumes:

```powershell
.\scripts\dev\zephyr-docker.ps1 build -WorkspaceVolume tracker-zephyr-workspace -SdkVolume tracker-zephyr-sdk-0.16.8 -SdkDir /opt/toolchains/zephyr-sdk-0.16.8 -Pristine
```

Do not treat that compatibility path as the long-term target.

## Board

Primary board target:

```text
trackerd_ls/esp32/procpu
```

Relevant files:

- `boards/dragino/trackerd_ls/`
- `app/prj.conf`
- `app/src/main.c`
- `lib/lorawan_node/`
- `lib/gnss/`
- `drivers/led_status/`

## Flashing And Validation

Hardware validation needs a real tracker device. Keep live bench hostnames, ports, SSH keys, serial IDs, and LoRaWAN credentials out of git.

Typical validation flow:

1. Build locally with Docker.
2. Copy `zephyr.bin` to an approved hardware bench.
3. Flash with `esptool` using the ESP32 app offset from `runners.yaml`.
4. Capture UART logs at `115200`.
5. Confirm boot, GNSS, LoRa radio detection, LoRaWAN join, and shell behavior.

## Tests

The repository still contains example Twister tests. They are not enough to validate tracker behavior yet.

Inside an initialized Zephyr workspace:

```shell
west twister -T /workspace/bl-doet-tracker-fw/tests --integration
```

## Agent Notes

AI agents should start with:

- `AI Agent Start Here.md`
- `llm-wiki/README.md`

Update the LLM wiki when adding durable build, validation, hardware, or process knowledge.
