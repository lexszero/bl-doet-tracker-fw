# Project Context

This repository supports a tracking system for following where Burn vehicles are during the Bolderland Burn.

## Ownership

- Git/project owner: Lex.

## Assignment source

- 2026-05-28: Lex posted an assignment titled `Vehicle tracker firmware improvements`.
- The assignment goal is to improve the quality of vehicle GPS position tracking.
- Lex pointed to `https://github.com/lexszero/bl-doet-tracker-fw/tree/dev/tracker` as the firmware repository/branch and noted that open issues have more detail.
- Public GitHub issue summaries are captured in [GitHub issues snapshot](github-issues.md).
- Public branch notes are captured in [Branch inventory](branch-inventory.md).
- Private source notes may exist in ignored local files. Do not commit raw chat exports or access details.

## Hardware scope

- The fleet has around 20-25 ESP32-based tracker units.
- Primary tracker hardware named by Lex: Dragino TrackerD-LS.
- Lex described the Dragino TrackerD-LS as a standard ESP32-style board with GPS and LoRa radios in a rugged enclosure with power handling.
- Target hardware also named by Lex in the assignment/wiki context: TTGO T-Beam.
- Private project context mentions an alternative Meshtastic-era hardware build; Lex would like to support both hardware types from the same Zephyr codebase if practical.
- Custom firmware is needed because the required customizations could not be done with stock firmware.
- Devices need to be registered on the LoRaWAN network, assigned to vehicles, physically installed, and sometimes wired into vehicle electrical systems for power.

## Surrounding DoET stack

- DoET has a custom API, map/data frontend, and monitoring stack.
- Vehicle tracker data is expected to feed the realtime sensor/map/monitoring stack, but the exact tracker payload and backend decoder contract are not documented in this repository yet.

## Current firmware state

- The firmware is built on Zephyr.
- Current firmware is a proof-of-concept skeleton.
- Current behavior sends GPS coordinates over LoRaWAN.
- Needed work includes making the firmware easily deployable in the field and experimenting to improve positioning accuracy.
- Private project context described the custom firmware as already working as a tracker but needing configurability to be practical; later context described the `dev/tracker` branch as partially broken and not touched since the previous year.

## Current framing

- The codebase currently resembles a Zephyr out-of-tree application/module scaffold.
- Some documentation and source text still uses upstream `example-application` wording.
- Product-specific documentation should replace inherited example wording as the firmware and hardware requirements become clear.

## Known gaps

- Exact hardware revisions, pinouts, peripheral wiring, batteries, antennas, and enclosure constraints for the Dragino TrackerD-LS and TTGO T-Beam units are not documented yet.
- Exact identity, pinout, and Zephyr board target for the Meshtastic-era hardware are not documented yet.
- The field behavior expected at Bolderland Burn is not documented yet.
- Detailed data flow between devices, network services, monitoring, and UI/backend is not documented yet.
- GitHub issue state and comments should be rechecked before starting implementation work.
