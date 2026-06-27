# Build Environment

This page captures durable build-environment decisions.

## Direction

2026-06-27: The project should move away from an implicit/manual Zephyr workspace and toward a reproducible local Docker workflow.

Current direction:

- Use local Docker for builds.
- Use remote hardware benches only for flashing, serial logs, and hardware validation.
- Keep private bench access notes under ignored local paths such as `llm-wiki/private/`.
- Avoid committing build artifacts and local Zephyr workspaces.

## Zephyr Version

The old remote boot log showed Zephyr `v4.2.0-rc3`, but that should be treated as historical context, not a long-term target.

The manifest now pins Zephyr to `v4.4.1` instead of tracking `main`. This is intended to make fresh workspaces reproducible and align with the SDK available in the current official Zephyr Docker image used locally.

If `v4.4.1` introduces board or driver breakage, fix the firmware against that stable release unless there is a concrete hardware reason to stay on the older release candidate.

## Docker Helper

Use [scripts/dev/zephyr-docker.ps1](../scripts/dev/zephyr-docker.ps1) from Windows PowerShell.

Modern workspace initialization:

```powershell
.\scripts\dev\zephyr-docker.ps1 init
```

Modern build:

```powershell
.\scripts\dev\zephyr-docker.ps1 build -Pristine
```

Artifacts are exported to `.codex-local/artifacts/trackerd_ls/`.

Optional overlay builds can pass a repo-local config fragment with `-ExtraConf`.
For example, the temporary issue #2 provisioning shell image is built with:

```powershell
.\scripts\dev\zephyr-docker.ps1 build -Pristine -ExtraConf app\provisioning.conf -BuildDir build/trackerd_ls_provision -ArtifactName trackerd_ls_provision
```

This exports artifacts to `.codex-local/artifacts/trackerd_ls_provision/`.
That overlay enables `CONFIG_TRACKER_PROVISIONING_MODE`, so it is for writing
settings only and does not run the normal tracker GNSS/LoRaWAN loop.

2026-06-27 validation:

- `west update` completed for the `tracker-zephyr-workspace-v4.4.1` Docker volume.
- The modern build completed with Zephyr `v4.4.1` and Zephyr SDK `1.0.1`.
- Required migration: replace the old `CONFIG_LORAMAC_REGION_EU868` symbol with `CONFIG_LORAWAN_REGION_EU868`.
- Removed stale networking/Wi-Fi stack assignments while networking remains disabled.
- 2026-06-27: the default issue #2 settings/provisioning build passed and remained `258048` bytes. The separate `app/provisioning.conf` shell build passed at `323584` bytes.

## Compatibility Path

Early local builds used:

- Docker image: `zephyrprojectrtos/zephyr-build:main`
- Zephyr workspace volume: `tracker-zephyr-workspace`
- SDK volume: `tracker-zephyr-sdk-0.16.8`
- Zephyr checkout observed in the workspace: `v4.2.0-rc3`

That path exists only to compare against the already-flashed test images. It should not be expanded as the project baseline.

## Hardware Validation

2026-06-27:

- The `v4.4.1` build artifact at `.codex-local/artifacts/trackerd_ls/zephyr.bin` was flashed successfully to the hardware bench.
- esptool verified the written image hash.
- A reset/probe followed by UART capture showed normal ESP32 and Zephyr boot output, app banner `DoET :: Tracker 1.0.0`, GNSS fixes, SX127x radio detection, LoRaWAN OTAA join success, and uplink confirmations.
- No fatal/assertion markers were observed in the captured boot window.

Open validation:

- Let the `v4.4.1` image run longer and watch for stability through repeated uplinks and downlink activity.
- The default firmware now uses a read-only UART log console: no Zephyr shell, no shell command modules, and `CONFIG_LOG_BACKEND_UART=y`.
- The read-only-console image flashed on 2026-06-27 was `258048` bytes and stayed clean through a 420 second raw 115200 capture with 42 uplink confirmations and no bad chunks.
- Do not assume interactive console garbage means the UART TX baud is wrong. The observed garbage was addressed by removing shell RX/echo from the default firmware, not by changing UART baud or crystal settings.
- Do not assume close/reopen silence means the firmware stopped logging. On the current bench, default Linux serial `hupcl`/plain `picocom` modem-control behavior can leave the ESP32 silent or in a bad reset/baud state after the last serial handle closes. A pyserial helper that releases GPIO0, pulses EN reset through RTS, and then streams at `115200` fixed repeated close/reopen validation.
- Do not repeat the failed UART experiments without new evidence: overriding CPU `xtal-freq` to 26 MHz and compensating `uart0.current-speed` to `177231` both made console output worse and were reverted.
- If the upstream migration changes ESP32 UART, LoRaWAN, GNSS, or devicetree behavior later, record the fix here and in source comments only where useful.
