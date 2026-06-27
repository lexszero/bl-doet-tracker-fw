# Project Map

This repository is organized like a Zephyr workspace application/module.

## Top-level files

- [README.md](../README.md): human-facing overview, setup, build, test, and documentation commands.
- [west.yml](../west.yml): Zephyr workspace manifest.
- [CMakeLists.txt](../CMakeLists.txt): top-level CMake entry.
- [Kconfig](../Kconfig): top-level Kconfig entry.
- [zephyr/module.yml](../zephyr/module.yml): Zephyr module metadata.

## Application

- [app/CMakeLists.txt](../app/CMakeLists.txt): application build definition.
- [app/prj.conf](../app/prj.conf): default application configuration.
- [app/debug.conf](../app/debug.conf): optional debug configuration.
- [app/src/main.c](../app/src/main.c): current application entry point.
- [app/boards](../app/boards): board-specific application overlays.

## Boards and devicetree

- [boards/dragino/trackerd_ls](../boards/dragino/trackerd_ls): Dragino TrackerD-LS ESP32 board definition.
- [dts/bindings](../dts/bindings): custom devicetree bindings.

## Drivers and libraries

- [drivers/led_status](../drivers/led_status): custom LED status driver.
- [drivers/sensor/example_sensor](../drivers/sensor/example_sensor): example sensor driver.
- [include/app/drivers](../include/app/drivers): public driver headers.
- [lib/custom](../lib/custom): custom library example.
- [lib/gnss](../lib/gnss): GNSS callback/logging helper.
- [lib/lorawan_node](../lib/lorawan_node): LoRaWAN initialization and OTAA join helper.
- [include/app/lib](../include/app/lib): public library headers.

## Tests and scripts

- [tests/lib/custom](../tests/lib/custom): Twister test coverage for the custom library.
- [scripts](../scripts): custom west command and runner examples.
- [scripts/dev/zephyr-docker.ps1](../scripts/dev/zephyr-docker.ps1): local Docker helper for workspace init/update/build/shell/clean.

## Documentation

- [doc](../doc): Sphinx documentation scaffold.
- [doc/_doxygen](../doc/_doxygen): Doxygen content.
- [AI Agent Start Here.md](../AI%20Agent%20Start%20Here.md): first file AI agents should read.
- [llm-wiki](../llm-wiki): AI-agent-maintained project wiki.

## Useful commands

Run from the repository root unless stated otherwise.

```powershell
.\scripts\dev\zephyr-docker.ps1 init
.\scripts\dev\zephyr-docker.ps1 build -Pristine
```

Documentation commands:

```shell
cd doc
pip install -r requirements.txt
doxygen
make html
```
