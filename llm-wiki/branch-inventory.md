# Branch Inventory

This page records the public remote branches visible from `origin`.

Recheck branch state before starting implementation work because remote heads can move.

## Source

- Remote: `https://github.com/lexszero/bl-doet-tracker-fw.git`
- Snapshot date: 2026-06-22
- Checked with: `git ls-remote --heads origin`

## Remote heads at snapshot

- `main`: `ca6d1b141f9d806d89a4ff22f6efd3ce9418d0da`
- `dev/tracker`: `2976538091f22e1d8726202a202600687d36fac9`
- `copilot/check-prompt-quota-status`: `b82f1561b07330de7367b2380f664f433f667572`

## `main`

`main` is the default branch in this checkout. It is still close to the upstream Zephyr example application template and does not contain the tracker proof-of-concept firmware.

The current local checkout is on `main`.

## `dev/tracker`

`dev/tracker` is the substantive tracker firmware branch Lex referenced in the assignment context.

At snapshot, `dev/tracker` is 3 commits ahead of `origin/main` and `origin/main` has no commits not already in `dev/tracker`.

Commits after `main`:

- `30f1c2d63bbe6e527b23e4c72910a7947c5bfae9`: `First working code: HW support and GNSS working`
- `ac87356a5054051a3e21e9677d97bebd2edfd9e4`: `+ LoRaWAN support and basic GNSS position sending`
- `2976538091f22e1d8726202a202600687d36fac9`: `Update config`

Important contents:

- Adds the Dragino TrackerD-LS board under `boards/dragino/trackerd_ls`.
- Removes the example `custom_plank` board files.
- Adds GNSS support in `lib/gnss` and `include/app/lib/gnss.h`.
- Adds LoRaWAN support in `lib/lorawan_node` and `include/app/lib/lorawan_node.h`.
- Replaces the example blink driver with a `led_status` driver.
- Adds settings scaffolding in `app/src/settings.c` and `include/app/settings.h`.
- Updates `west.yml` to use Zephyr `main` and allowlist `hal_espressif`, `loramac-node`, `littlefs`, and `mbedtls`.
- Adds a large top-level `config` file that appears to be generated Zephyr configuration output.

Runtime behavior seen in code:

- `app/src/main.c` initializes GNSS, LED status, and LoRaWAN.
- The app joins LoRaWAN over OTAA, using EU868 and LoRaWAN Class C.
- After joining, the app sends a small test message and then sends packed latitude, longitude, and HDOP over LoRaWAN.
- The current uplink interval is a fixed 10 seconds.
- Position messages are sent on LoRaWAN port 4.

Code-read cautions before building from this branch:

- `include/app/lib/lorawan_node.h` contains hardcoded placeholder LoRaWAN EUIs and key constants. Treat them as development placeholders, not deployable credentials.
- `app/src/settings.c` appears incomplete and may not compile as-is if included in the build. It references undefined handler names and has rough error handling/structure.
- `main.c` includes `app/settings.h` but does not call `settings_init()`, so persistent settings are not wired into runtime behavior.
- `app/sample.yaml` still lists deleted/example platforms such as `custom_plank` and `nucleo_f302r8` alongside `trackerd_ls`.
- Wi-Fi is enabled in the board devicetree, but most networking and Wi-Fi options are commented out in `app/prj.conf`.
- The branch has not been locally built in this wiki session, so build status is not verified.

## `copilot/check-prompt-quota-status`

This branch has one bot-authored commit, `Initial plan`, on top of `main`.

At snapshot, it has no tree diff from `origin/main`; it does not appear to contain firmware or documentation changes relevant to the tracker work.

## Practical guidance

- Use `dev/tracker` as the starting point for tracker firmware investigation unless Lex says otherwise.
- Recheck `dev/tracker` before implementation because it is the branch most likely to move.
- Preserve this `main`-branch wiki unless intentionally merging or porting it to the tracker branch.
