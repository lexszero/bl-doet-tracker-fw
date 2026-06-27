# Current Handoff

Last updated: 2026-06-27.

This page captures the current state for the next Codex chat. It should stay free of private bench access details.

## Branch and Git State

- Working branch: `dev/cb_tracker`.
- Local commit already created before this handoff: `f67e610 Fix LoRaWAN downlink callback lifetime and add agent wiki`.
- That commit has not been pushed to `origin` because the current GitHub user lacked write permission to `lexszero/bl-doet-tracker-fw`.
- Normal push retry after permissions are granted:

```shell
git push -u origin dev/cb_tracker
```

- Avoid `git push --all`, `git push --mirror`, and `git add -f` on ignored private/build paths.

## Important Firmware Fix Already In Branch

`lib/lorawan_node/lorawan_node.c` now keeps the LoRaWAN downlink callback structure in static storage. This fixed a real crash observed on hardware after a LoRaWAN downlink indication.

Root cause:

- `lorawan_node_init()` had registered a stack-allocated `struct lorawan_downlink_cb`.
- Zephyr stores that pointer for later use.
- When a downlink arrived later, the callback pointer could refer to invalid stack memory.

Validation before the build-environment work:

- Rebuilt with the older local Zephyr `v4.2.0-rc3` compatibility workspace.
- Flashed to the hardware bench.
- Boot passed the old crash point and showed GNSS fix plus multiple LoRaWAN uplink confirmations.

## Build Environment Direction

The repo has been moved toward a reproducible local Docker build instead of an implicit/manual Zephyr workspace.

Modern default path:

```powershell
.\scripts\dev\zephyr-docker.ps1 init
.\scripts\dev\zephyr-docker.ps1 build
```

Current modern baseline:

- Zephyr manifest revision: `v4.4.1`.
- Docker image: `zephyrprojectrtos/zephyr-build:main`.
- Zephyr SDK from image: `1.0.1`.
- Docker workspace volume: `tracker-zephyr-workspace-v4.4.1`.
- Board target: `trackerd_ls/esp32/procpu`.
- Artifact path: `.codex-local/artifacts/trackerd_ls/zephyr.bin`.

Verified on 2026-06-27:

- `west update` completed for the `tracker-zephyr-workspace-v4.4.1` Docker volume.
- `.\scripts\dev\zephyr-docker.ps1 build` succeeds.
- Latest modern `zephyr.bin` size after build: `323584` bytes.

Compatibility path:

```powershell
.\scripts\dev\zephyr-docker.ps1 build -WorkspaceVolume tracker-zephyr-workspace -SdkVolume tracker-zephyr-sdk-0.16.8 -SdkDir /opt/toolchains/zephyr-sdk-0.16.8 -Pristine
```

This older path uses the existing local Zephyr `v4.2.0-rc3` workspace and SDK `0.16.8`. It exists for comparison, not as the long-term baseline.

## Zephyr 4.4 Migration Done

Files adjusted for the modern build:

- `west.yml`: pins Zephyr to `v4.4.1` instead of `main`.
- `app/prj.conf`: uses `CONFIG_LORAWAN_REGION_EU868=y`.
- `app/prj.conf`: removed stale network stack assignments while networking is disabled.
- `boards/dragino/trackerd_ls/trackerd_ls_esp32_procpu_defconfig`: removed stale ESP32 Wi-Fi DHCP assignment while Wi-Fi/networking is disabled.
- `.gitattributes`: added LF defaults and binary classifications.
- Global Git config was set locally: `core.autocrlf=false`.

## Current Hardware/Serial Situation

The remote tracker is physically reachable through an approved hardware bench. Exact access details are private and should stay in ignored notes.

Observed earlier from a working boot before local reflashes:

- Hardware identifies as ESP32-PICO-D4 revision `v1.1` with 4 MB flash.
- Zephyr boot log showed `v4.2.0-rc3`.
- App banner: `DoET :: Tracker 1.0.0`.
- GNSS, LIS2DH/LIS3DH-class accelerometer, and SX127x LoRa radio initialized.
- LoRaWAN OTAA join succeeded.
- Zephyr shell was available on the UART console.

Latest validation:

- The modern Zephyr `v4.4.1` image from `.codex-local/artifacts/trackerd_ls/zephyr.bin` was flashed to the remote tracker on 2026-06-27.
- Artifact size: `323584` bytes.
- Flash command completed successfully and esptool verified the written image hash.
- A first passive UART capture after flashing only showed the picocom banner. A reset/probe followed by console capture produced normal boot logs.
- Boot log showed ESP32-PICO-D4 revision `v1.1`, Zephyr `v4.4.1`, app banner `DoET :: Tracker 1.0.0`, GNSS power/init, LIS2DH init, SX127x radio detection, LoRaWAN OTAA join success, GNSS fixes, and LoRaWAN uplink confirmations.
- No `FATAL` or `ASSERT` markers were observed in the captured boot window.

Console follow-up:

- A user-observed interactive `tracker-console` session showed normal log output followed by garbage after the `uart:~$` prompt.
- Two firmware experiments were tried and backed out:
  - overriding CPU `xtal-freq` to 26 MHz;
  - compensating `uart0.current-speed` to `177231`.
- Both experiments made the console worse and were reverted. The tracker was reflashed with the restored `v4.4.1` build using `uart0.current-speed = <115200>`.
- A 330 second raw serial capture at 115200 after reset produced `171474` bytes, stayed printable throughout, and showed boot, join, GNSS fixes, and repeated uplink confirmations with no `FATAL`, `ASSERT`, or `modem_chat` markers.
- A live `picocom -b 115200 /dev/ttyACM0` capture after that also stayed clean for 25 seconds with no nonprintable bytes.

Console fix:

- `app/prj.conf` now removes the interactive Zephyr shell and shell command modules from the default firmware.
- Logging is routed directly to UART with `CONFIG_LOG_BACKEND_UART=y`.
- `CONFIG_UART_INTERRUPT_DRIVEN=y` is set explicitly because the GNSS NMEA modem backend needs it now that the shell no longer selects it indirectly.
- The current flashed image is the read-only-console build, copied to the bench as `/home/christian/codex/flash/trackerd_ls/zephyr-readonly-console.bin`.
- Artifact size: `258048` bytes.
- A 60 second `picocom -b 115200` capture after reset showed normal boot, `DoET`, GNSS, LoRaWAN join, and uplink logs. It showed no `uart:~$` prompt, no command echo, no `FATAL`, and no `ASSERT`.
- A 420 second raw 115200 capture after reset produced `162237` bytes with no bad chunks. Marker counts included one boot, one `DoET`, one LoRaWAN join, `1002` `app_gnss` occurrences, `544` `lorawan` occurrences, and `42` `McpsRequest success` confirmations. It showed `0` `uart:~$`, `0` `FATAL`, `0` `ASSERT`, and `0` `command not found`.
- A follow-up live `picocom -b 115200` capture after the long run stayed printable with no nonprintable bytes.
- Follow-up close/reopen testing showed the remaining console failure was host modem-control state, not the firmware UART baud:
  - reopening the port with default Linux `hupcl` or plain `picocom` could leave the ESP32 silent or in a bad serial/reset state;
  - an explicit pyserial run-mode reset, with GPIO0 released (`DTR=false`) and an EN reset pulse on `RTS`, restored clean logs at `115200`;
  - the remote helper `~/bin/tracker-reset-console` and symlink `~/bin/tracker-console` now implement that pyserial reset-and-stream path instead of plain `picocom`.
- Validated two back-to-back helper sessions on 2026-06-27. Captures were fully printable and showed Zephyr boot, `DoET`, GNSS logs, LoRaWAN join, uplink confirmations, and no `FATAL`, `ASSERT`, or `modem_chat` markers.
- Because the default firmware console is read-only, exit the helper with `Ctrl-C`. Existing SSH shells that loaded an older `tracker-console` alias may need `source ~/.bash_aliases`, a fresh SSH shell, or direct use of `/home/christian/bin/tracker-console`.

Issue #3 adaptive uplink work:

- `app/src/main.c` now has an initial movement-aware uplink policy for issue #3.
- The LoRaWAN position payload format is unchanged: port `4`, packed `int32_t lat`, `int32_t lon`, and `uint16_t hdop`.
- GNSS remains powered/hot; the policy changes when position fixes are sent, not GNSS power state.
- The app polls the LIS2DH/LIS3DH accelerometer on each GNSS position event and classifies motion as `active`, `moving`, or `stationary`.
- Uplink intervals are currently:
  - moving: `10 s`;
  - active/working: `30 s`;
  - stationary heartbeat: `120 s`.
- The first implementation exposed two important correctness fixes:
  - `k_event_wait()` now clears GNSS event bits so the main loop does not keep re-processing stale position events.
  - `lib/gnss/gnss.c` now copies the latest GNSS sample into `gnss_data` before posting the position callback.
- GNSS fix gating skips invalid/no-fix positions, fewer than `4` satellites, or HDOP worse than `2.5`.
- When the accelerometer is available and still, GNSS speed alone does not resume motion. This is intentional: stationary bench logs showed GNSS-reported speed spikes while acceleration stayed near gravity.
- A drift guard suppresses GNSS-only speed/position use for `60 s` when the accelerometer is still and GNSS altitude jumps by more than `5 m` between fixes.
- If the accelerometer is unavailable, the policy falls back to GNSS speed so the firmware still sends positions.

Issue #3 bench validation:

- The adaptive-uplink build was flashed to the remote tracker on 2026-06-27.
- Artifact size remained `258048` bytes.
- Final 190 second capture showed:
  - Zephyr `v4.4.1` boot and app banner;
  - accelerometer ready as `lis3dh@19`;
  - LoRaWAN OTAA join success;
  - no `FATAL`, `ASSERT`, or `modem_chat` markers;
  - motion state changed from startup `active` to `stationary`;
  - one startup position uplink after speed settled;
  - one stationary heartbeat uplink about `120 s` later;
  - later GNSS drift guard activation on a false GNSS speed/altitude jump without forcing active/moving state.
- This validates the stationary bench case. A real moving/shaking vehicle test is still needed to tune thresholds and prove movement resumes `10 s`/`30 s` cadence as intended.

LoRa backend observation:

- ChirpStack access through the private bench path works. Keep hostnames, internal IPs, credentials, exact URLs, and device identifiers in `llm-wiki/private/`.
- MQTT subscription attempts did not show useful Tracker_108 application events during this session. ChirpStack's gRPC API did work for application event and gateway-frame inspection.
- ChirpStack application event history for the Lex-provided Tracker_108 device includes port `4` uplinks whose 10 byte payload decodes exactly as the firmware format: little-endian `int32_t lat`, `int32_t lon`, and `uint16_t hdop`.
- The currently flashed firmware does **not** identify as that Tracker_108 device. Gateway-frame inspection of a live reset showed the current firmware JoinRequest uses the placeholder DevEUI / JoinEUI from `include/app/lib/lorawan_node.h`.
- Live gateway frames confirmed the current firmware's LoRa packets are reaching the gateway:
  - JoinRequest from the placeholder DevEUI;
  - unconfirmed uplink on port `13` for the `de ad ca fe` boot marker;
  - unconfirmed uplink on port `4` for the position payload.
- Because the current firmware uses placeholder LoRaWAN identity values, current live uplinks should not be expected to appear under Lex's Tracker_108 application device until issue #2/settings/provisioning is addressed or the firmware is temporarily built with the Tracker_108 credentials.
- The stock Tracker_108 decoder reports a codec error on the port `13` boot marker. If the boot marker stays, the ChirpStack decoder should ignore or handle non-position ports.

Historical caveat:

- Before the modern image was flashed, the remote tracker had a locally built `origin/dev/tracker` reference candidate from commit `2976538`.
- Its UART output was mostly unreadable garbage and did not show normal Zephyr markers in a noninteractive capture.
- The same kind of bad UART behavior was seen with other locally built `v4.2.0-rc3` images.
- That behavior is no longer the current flashed state.

## Next Best Steps

1. Decide how to provision real LoRaWAN identity values. The current hardcoded placeholder DevEUI is enough to prove gateway RF receipt, but not enough to view current data under Lex's Tracker_108 device.
2. For a quick backend validation, temporarily build with the Tracker_108 DevEUI / JoinEUI / AppKey in private/local config or create a matching temporary ChirpStack device for the placeholder identity.
3. Update the ChirpStack decoder to handle or ignore port `13`, or remove the `de ad ca fe` boot marker before relying on the app event stream.
4. Test the adaptive uplink policy on a physically moving tracker or by safely shaking/moving the bench device while watching `motion state:` and `position uplink:` logs.
5. Tune `GNSS_ACTIVE_SPEED_MM_S`, `GNSS_MOVING_SPEED_MM_S`, `ACCEL_VECTOR_DELTA_MM_S2`, and the three interval constants from real movement traces.
6. Keep the read-only UART log console unless interactive shell access is explicitly needed.
7. Let the current build run longer and capture whether it remains stable through repeated stationary heartbeats, movement resumes, and any downlink activity.
8. If an interactive shell is needed later, prefer a separate debug config or first try `CONFIG_SHELL_ECHO_STATUS=n`; do not reintroduce shell echo into the default firmware without retesting the bench console and DTR/RTS behavior.
9. If Lex provides an original or known-good `zephyr.bin`, keep it for comparison, but it is no longer required just to prove the UART/reset path.

## Useful Local Commands

Build current modern firmware:

```powershell
.\scripts\dev\zephyr-docker.ps1 build
```

Build clean:

```powershell
.\scripts\dev\zephyr-docker.ps1 build -Pristine
```

Check Git state:

```powershell
git status --short --branch
git log --oneline origin/dev/tracker..HEAD
git diff --check
```

Private bench details, if present, are under `llm-wiki/private/` and must not be staged.
