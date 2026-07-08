# Hardware Notes

This page captures public-safe hardware observations for the tracker firmware.
Keep private bench serials, access details, and provisioned identities in
ignored private notes.

## Dragino TrackerD-LS

The current primary target is the Dragino TrackerD-LS / ESP32-PICO-D4 style
tracker with GNSS, SX127x LoRa radio, accelerometer, battery sensing, and at
least one I2C peripheral bus.

## I2C Bus

The board devicetree enables `i2c0`:

- SDA: GPIO21;
- SCL: GPIO22;
- bitrate: standard I2C.

Current firmware uses this bus for the accelerometer only.

## Accelerometer

The schematic identifies the accelerometer as `LIS3DHLGA-16`. It is wired for
I2C mode:

- `SCL/SPC` -> SCL;
- `SDA/SDI/SDO` -> SDA;
- `CS` tied high;
- `SDO/SA0` pulled up to `3V3`, selecting I2C address `0x19`;
- `INT1` routed to the board `INT` net.

The Zephyr board definition matches this as `lis3dh@19` with compatible
`st,lis3dh` / `st,lis2dh` and interrupt GPIO14.

## Battery Sense

The schematic shows the battery ADC path as:

```text
BAT+ -> 100k -> IO34/PA2 -> 470k -> GND
```

The firmware currently reads ESP32 IO34 / ADC1 channel 6 and scales by
`(100 + 470) / 470`.

## Possible Temperature/Humidity Sensor

The schematic includes an 8-pin I2C device marked as U12 with pins named:

- `SDA`;
- `SCL`;
- `ADDR`;
- `ALERT`;
- `RST!`;
- `VDD`;
- `GND`.

This pinout is consistent with an SHT3x-style temperature/humidity sensor, but
the exact part number and I2C address have not been confirmed from package
marking or an I2C scan.

Stock TrackerD-LS firmware has printed values such as:

```text
HUM:48.40
TEM:24.44
```

The current Zephyr firmware does not define or read this sensor. Future work
could add it after confirming the exact chip/address and Zephyr driver support,
then decide whether temperature/humidity belong in diagnostics, LoRaWAN
payloads, or only local debug output.
