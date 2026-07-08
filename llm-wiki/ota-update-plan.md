# OTA Update Plan

Last updated: 2026-07-04.

This page is public/GitHub-safe. It describes the proposed over-the-air firmware update structure for issue `#4` without private bench, backend, or credential details.

## Goal

Provide a basic field update path so trackers can receive firmware fixes without opening the enclosure and attaching USB. The first target is a reliable engineering OTA flow, not a full fleet-management product.

## Current Repo State

- The board already has an OTA-shaped 4 MB flash layout:
  - `image-0` at `0x20000`, 1 MiB;
  - `image-1` at `0x120000`, 1 MiB;
  - `image-scratch` at `0x3e0000`, 124 KiB;
  - `storage` at `0x3b0000`, preserved for settings;
  - `diagnostic-log` at `0x310000`, 640 KiB.
- `boards/dragino/trackerd_ls/Kconfig.sysbuild` defaults the board bootloader choice to `BOOTLOADER_MCUBOOT`.
- `BOOT_SIGNATURE_TYPE_NONE` is currently the default, so the existing configuration is not a signed field-update configuration.
- The current development flash workflow still flashes `zephyr.bin` at ESP32 app offset `0x1000`, as shown in generated `runners.yaml`.
- The Docker build helper currently uses plain `west build`, not an explicit `west build --sysbuild` MCUboot production flow.

The practical conclusion: the partition map is prepared for OTA, but the boot chain and update lifecycle are not yet proven.

## Design Direction

Use standard Zephyr and MCUboot update mechanisms.

Preferred layering:

1. MCUboot boot and rollback foundation.
2. Image signing and version metadata.
3. Local/test update transport.
4. WiFi-based field update transport.
5. Fleet/update-server polish only if needed.

Do not start with a custom updater. Do not start with LoRaWAN FUOTA.

## Phase 1: Prove MCUboot and Rollback

This is the first required milestone.

Tasks:

- Build with sysbuild so MCUboot and the app are built as one coherent image set.
- Produce MCUboot-compatible signed application artifacts.
- Flash MCUboot and the initial slot0 image once over USB.
- Verify that the app boots from the MCUboot-managed primary slot.
- Write a newer image to slot1.
- Mark the slot1 image pending.
- Reboot and verify MCUboot boots the pending image.
- Confirm the image only after basic app health checks.
- Verify that an unconfirmed image reverts on the next reset.

Open point to resolve early:

- The current ESP32 runner flashes the app image at `0x1000`, while the DTS declares `image-0` at `0x20000`. The OTA work must confirm the correct ESP32 + MCUboot slot offsets and artifact types before any field device is migrated.

## Phase 2: Image Confirmation Policy

The app should not confirm a new OTA image immediately at boot.

Proposed confirm requirements:

- tracker settings load successfully;
- diagnostic log initializes;
- GNSS processing thread starts;
- main tracker loop continues for a short window, for example `2-5 min`;
- watchdog/task watchdog stays healthy.

Do not require LoRaWAN join or successful uplink before confirming. RF/network conditions can be bad even when the firmware is healthy, and requiring LoRaWAN would create unnecessary rollback loops.

## Phase 3: First Transport

Preferred first transport: WiFi.

Two realistic options:

### Option A: MCUmgr/SMP over UDP

Use Zephyr's standard management subsystem and image management group.

Pros:

- Standard Zephyr ecosystem path.
- Good for engineering bring-up.
- Push update from a laptop or local update host.
- Exercises the same MCUboot slot and rollback mechanism needed later.

Cons:

- Requires a management client and reachable IP network.
- Less product-like than device-initiated polling.
- Needs careful access control before use outside a trusted network.

### Option B: HTTP Pull

Device joins WiFi, polls or is instructed to download a signed image from a simple HTTP endpoint, writes slot1, marks pending, and reboots.

Pros:

- More product-like.
- Easier to operate from a simple local server.
- Can be tied to a small status/debug web interface.

Cons:

- More application code.
- Needs robust interrupted-download handling.
- Needs update metadata format and version selection policy.

Recommended order:

1. Use MCUmgr/SMP or a very small local test writer to prove the MCUboot lifecycle.
2. Add HTTP pull after the slot/rollback path is trusted.

## Phase 4: Security Model

Development bring-up may temporarily use unsigned images only to reduce friction, but field OTA should use signed images.

Minimum field requirements:

- private signing key is never committed;
- public verification key is embedded in MCUboot;
- app images carry version/build metadata;
- device rejects unsigned or wrongly signed images;
- rollback remains enabled until the new app confirms itself.

Transport authentication is still useful, but image signing is the essential first line of defense. A signed image can safely travel through a simple or imperfect transport; an unsigned image cannot.

## Phase 5: Observability

OTA must be visible in local diagnostics.

Add diagnostic records or status fields for:

- current firmware version and git revision;
- bootloader/update mode state;
- active slot and pending/confirmed state;
- update check started;
- update available/no update;
- download started;
- download progress or byte count milestones;
- download/write failure code;
- image validation result;
- image marked pending;
- reboot for update;
- image confirmed;
- rollback detected.

Also expose the same information through the planned WiFi debug/status channel.

## Not Recommended First

### LoRaWAN FUOTA

Not a first implementation path. It is attractive because the device already has LoRaWAN, but the current project evidence shows LoRaWAN reception gaps and send failures. Firmware images are also large compared with LoRaWAN throughput and duty-cycle limits.

### HawkBit / UpdateHub First

These may be useful later, but they add server concepts before the device update lifecycle is proven. Start smaller.

### Custom Bootloader or Custom Swap Logic

Avoid this. MCUboot already provides the image format, slot handling, test/confirm semantics, and rollback model.

## Proposed MVP Definition

OTA MVP is complete when one bench tracker can:

1. boot a signed MCUboot-managed app;
2. receive a newer signed app into the secondary slot over a local WiFi path;
3. reboot into the new app;
4. confirm the app after health checks;
5. roll back automatically if the new app does not confirm;
6. preserve provisioning/settings storage;
7. record update lifecycle events in diagnostic logs.

This MVP does not require:

- a hosted fleet-management server;
- cloud accounts;
- LoRaWAN FUOTA;
- automatic staged rollouts;
- remote configuration UI polish.

## Validation Plan

Run these tests before using OTA on field devices:

1. USB flash initial MCUboot + app.
2. Verify normal tracker operation after migration.
3. Push/download image B.
4. Confirm image B after health checks.
5. Push/download image C that intentionally does not confirm.
6. Verify rollback to image B.
7. Verify settings remain intact after update and rollback.
8. Verify diagnostic log remains readable after update and rollback.
9. Verify interrupted update leaves the current app bootable.
10. Verify power loss during slot1 write does not brick the device.

## References

- Zephyr OTA overview: `https://docs.zephyrproject.org/latest/services/device_mgmt/ota.html`
- Zephyr DFU overview: `https://docs.zephyrproject.org/latest/services/device_mgmt/dfu.html`
- Zephyr MCUmgr overview: `https://docs.zephyrproject.org/latest/services/device_mgmt/mcumgr.html`
- Zephyr SMP server sample: `https://docs.zephyrproject.org/latest/samples/subsys/mgmt/mcumgr/smp_svr/README.html`
- MCUboot Zephyr notes: `https://docs.mcuboot.com/readme-zephyr.html`
