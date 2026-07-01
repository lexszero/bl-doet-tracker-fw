#!/usr/bin/env python3
"""Decode TrackerD-LS diagnostic-log flash dumps.

Dump the flash area from the bench device first, for example:

    esptool.py -p /dev/ttyACM0 read_flash 0x310000 0xA0000 diag-log.bin

Then decode it:

    python scripts/dev/decode_diag_log.py diag-log.bin --out-dir diag-log-out
"""

from __future__ import annotations

import argparse
import csv
import html
import json
import struct
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable


MAGIC = 0xD107
VERSION_V1 = 1
VERSION_V2 = 2
VERSION_V3 = 3
VERSION_V4 = 4
VERSION_V5 = 5
VERSION_V6 = 6
RECORD_SIZE_V1 = 40
RECORD_SIZE_V2 = 48
RECORD_SIZE_V3 = 48
RECORD_SIZE_V4 = 48
RECORD_SIZE_V5 = 48
RECORD_SIZE_V6 = 48
SECTOR_SIZE = 4096
RECORD_STRUCT_V1 = struct.Struct("<HBBIIIiiHHHhBBBBHH")
RECORD_STRUCT_V2 = struct.Struct("<HBBIIIiiHHHhBBBBBBhbBHHH")
RECORD_STRUCT_V3 = struct.Struct("<HBBIIIiiHHHhBBBBBBhbBHHH")
RECORD_STRUCT_V4 = struct.Struct("<HBBIIIiiHHHhBBBBBBhbBHHH")
RECORD_STRUCT_V5 = struct.Struct("<HBBIIIiiHHHhBBBBBBhbBHHH")
RECORD_STRUCT_V6 = struct.Struct("<HBBIIIiiHHHhBBBBBBhbBHHH")

MOTION_STATES = {
    0: "unknown",
    1: "stationary",
    2: "active",
    3: "moving",
}

RESULTS = {
    1: "sent",
    2: "send_failed",
    3: "not_joined",
    4: "no_fix",
}

FLAG_ACCEL_VALID = 1 << 0
FLAG_ACCEL_MOVING = 1 << 1
FLAG_DRIFT_SUPPRESSED = 1 << 2
FLAG_MOTION_RESUME = 1 << 3
FLAG_UTC_VALID = 1 << 4

LINK_FLAG_ADR_ENABLED = 1 << 0
LINK_FLAG_DR_VALID = 1 << 1
LINK_FLAG_DOWNLINK_VALID = 1 << 2
LINK_FLAG_CONFIRMED = 1 << 3

POWER_FLAG_BATTERY_VALID = 1 << 0
POWER_FLAG_BATTERY_GPIO34_VALID = 1 << 1
POWER_FLAG_BATTERY_PIN_VALID = 1 << 1
POWER_FLAG_BATTERY_SATURATED = 1 << 2


@dataclass(frozen=True)
class Record:
    record_version: int
    offset: int
    seq: int
    uptime_ms: int
    utc_packed: int
    latitude: int
    longitude: int
    speed_cm_s: int
    hdop: int
    interval_s: int
    send_ret: int
    motion_state: int
    result: int
    satellites: int
    flags: int
    lorawan_dr: int | None = None
    link_flags: int = 0
    downlink_rssi: int | None = None
    downlink_snr: int | None = None
    power_flags: int = 0
    battery_mv: int | None = None
    battery_gpio34_mv: int | None = None
    battery_pin_mv: int | None = None

    @property
    def lat_deg(self) -> float:
        return (self.latitude << 5) / 1_000_000_000

    @property
    def lon_deg(self) -> float:
        return (self.longitude << 5) / 1_000_000_000

    @property
    def speed_m_s(self) -> float:
        return self.speed_cm_s / 100.0

    @property
    def speed_kmh(self) -> float:
        return self.speed_m_s * 3.6

    @property
    def utc_iso(self) -> str:
        return decode_utc(self.utc_packed) or ""

    @property
    def motion_name(self) -> str:
        return MOTION_STATES.get(self.motion_state, f"unknown_{self.motion_state}")

    @property
    def result_name(self) -> str:
        return RESULTS.get(self.result, f"unknown_{self.result}")


def crc16_ccitt(data: bytes) -> int:
    crc = 0xFFFF
    for value in data:
        crc ^= value << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def decode_utc(value: int) -> str | None:
    if value == 0:
        return None

    year = (value >> 26) & 0x3F
    month = (value >> 22) & 0x0F
    day = (value >> 17) & 0x1F
    hour = (value >> 12) & 0x1F
    minute = (value >> 6) & 0x3F
    second = value & 0x3F

    if not (1 <= month <= 12 and 1 <= day <= 31 and hour <= 23 and minute <= 59 and second <= 59):
        return None

    return f"20{year:02d}-{month:02d}-{day:02d}T{hour:02d}:{minute:02d}:{second:02d}Z"


def iter_record_offsets(size: int, record_size: int) -> Iterable[int]:
    sectors = size // SECTOR_SIZE
    records_per_sector = SECTOR_SIZE // record_size
    for sector in range(sectors):
        sector_offset = sector * SECTOR_SIZE
        for slot in range(records_per_sector):
            offset = sector_offset + slot * record_size
            if offset + record_size <= size:
                yield offset


def parse_record_v1(raw: bytes, offset: int) -> Record | None:
    chunk = raw[offset : offset + RECORD_SIZE_V1]
    if len(chunk) != RECORD_SIZE_V1 or chunk == b"\xff" * RECORD_SIZE_V1:
        return None

    (
        magic,
        version,
        record_size,
        seq,
        uptime_ms,
        utc_packed,
        latitude,
        longitude,
        speed_cm_s,
        hdop,
        interval_s,
        send_ret,
        motion_state,
        result,
        satellites,
        flags,
        crc16,
        _reserved,
    ) = RECORD_STRUCT_V1.unpack(chunk)

    if magic != MAGIC or version != VERSION_V1 or record_size != RECORD_SIZE_V1:
        return None

    if crc16_ccitt(chunk[:36]) != crc16:
        return None

    return Record(
        record_version=version,
        offset=offset,
        seq=seq,
        uptime_ms=uptime_ms,
        utc_packed=utc_packed,
        latitude=latitude,
        longitude=longitude,
        speed_cm_s=speed_cm_s,
        hdop=hdop,
        interval_s=interval_s,
        send_ret=send_ret,
        motion_state=motion_state,
        result=result,
        satellites=satellites,
        flags=flags,
    )


def parse_record_v2(raw: bytes, offset: int) -> Record | None:
    chunk = raw[offset : offset + RECORD_SIZE_V2]
    if len(chunk) != RECORD_SIZE_V2 or chunk == b"\xff" * RECORD_SIZE_V2:
        return None

    (
        magic,
        version,
        record_size,
        seq,
        uptime_ms,
        utc_packed,
        latitude,
        longitude,
        speed_cm_s,
        hdop,
        interval_s,
        send_ret,
        motion_state,
        result,
        satellites,
        flags,
        lorawan_dr,
        link_flags,
        downlink_rssi,
        downlink_snr,
        _reserved0,
        crc16,
        _reserved,
        _reserved1,
    ) = RECORD_STRUCT_V2.unpack(chunk)

    if magic != MAGIC or version != VERSION_V2 or record_size != RECORD_SIZE_V2:
        return None

    if crc16_ccitt(chunk[:42]) != crc16:
        return None

    return Record(
        record_version=version,
        offset=offset,
        seq=seq,
        uptime_ms=uptime_ms,
        utc_packed=utc_packed,
        latitude=latitude,
        longitude=longitude,
        speed_cm_s=speed_cm_s,
        hdop=hdop,
        interval_s=interval_s,
        send_ret=send_ret,
        motion_state=motion_state,
        result=result,
        satellites=satellites,
        flags=flags,
        lorawan_dr=lorawan_dr,
        link_flags=link_flags,
        downlink_rssi=downlink_rssi,
        downlink_snr=downlink_snr,
    )


def parse_record_v3(raw: bytes, offset: int) -> Record | None:
    chunk = raw[offset : offset + RECORD_SIZE_V3]
    if len(chunk) != RECORD_SIZE_V3 or chunk == b"\xff" * RECORD_SIZE_V3:
        return None

    (
        magic,
        version,
        record_size,
        seq,
        uptime_ms,
        utc_packed,
        latitude,
        longitude,
        speed_cm_s,
        hdop,
        interval_s,
        send_ret,
        motion_state,
        result,
        satellites,
        flags,
        lorawan_dr,
        link_flags,
        downlink_rssi,
        downlink_snr,
        power_flags,
        battery_mv,
        crc16,
        _reserved,
    ) = RECORD_STRUCT_V3.unpack(chunk)

    if magic != MAGIC or version != VERSION_V3 or record_size != RECORD_SIZE_V3:
        return None

    if crc16_ccitt(chunk[:44]) != crc16:
        return None

    return Record(
        record_version=version,
        offset=offset,
        seq=seq,
        uptime_ms=uptime_ms,
        utc_packed=utc_packed,
        latitude=latitude,
        longitude=longitude,
        speed_cm_s=speed_cm_s,
        hdop=hdop,
        interval_s=interval_s,
        send_ret=send_ret,
        motion_state=motion_state,
        result=result,
        satellites=satellites,
        flags=flags,
        lorawan_dr=lorawan_dr,
        link_flags=link_flags,
        downlink_rssi=downlink_rssi,
        downlink_snr=downlink_snr,
        power_flags=power_flags,
        battery_mv=battery_mv if power_flags & POWER_FLAG_BATTERY_VALID else None,
    )


def parse_record_v4(raw: bytes, offset: int) -> Record | None:
    chunk = raw[offset : offset + RECORD_SIZE_V4]
    if len(chunk) != RECORD_SIZE_V4 or chunk == b"\xff" * RECORD_SIZE_V4:
        return None

    (
        magic,
        version,
        record_size,
        seq,
        uptime_ms,
        utc_packed,
        latitude,
        longitude,
        speed_cm_s,
        hdop,
        interval_s,
        send_ret,
        motion_state,
        result,
        satellites,
        flags,
        lorawan_dr,
        link_flags,
        downlink_rssi,
        downlink_snr,
        power_flags,
        battery_mv,
        battery_gpio34_mv,
        crc16,
    ) = RECORD_STRUCT_V4.unpack(chunk)

    if magic != MAGIC or version != VERSION_V4 or record_size != RECORD_SIZE_V4:
        return None

    if crc16_ccitt(chunk[:46]) != crc16:
        return None

    return Record(
        record_version=version,
        offset=offset,
        seq=seq,
        uptime_ms=uptime_ms,
        utc_packed=utc_packed,
        latitude=latitude,
        longitude=longitude,
        speed_cm_s=speed_cm_s,
        hdop=hdop,
        interval_s=interval_s,
        send_ret=send_ret,
        motion_state=motion_state,
        result=result,
        satellites=satellites,
        flags=flags,
        lorawan_dr=lorawan_dr,
        link_flags=link_flags,
        downlink_rssi=downlink_rssi,
        downlink_snr=downlink_snr,
        power_flags=power_flags,
        battery_mv=battery_mv if power_flags & POWER_FLAG_BATTERY_VALID else None,
        battery_gpio34_mv=(
            battery_gpio34_mv
            if power_flags & POWER_FLAG_BATTERY_GPIO34_VALID
            else None
        ),
    )


def parse_record_v5(raw: bytes, offset: int) -> Record | None:
    chunk = raw[offset : offset + RECORD_SIZE_V5]
    if len(chunk) != RECORD_SIZE_V5 or chunk == b"\xff" * RECORD_SIZE_V5:
        return None

    (
        magic,
        version,
        record_size,
        seq,
        uptime_ms,
        utc_packed,
        latitude,
        longitude,
        speed_cm_s,
        hdop,
        interval_s,
        send_ret,
        motion_state,
        result,
        satellites,
        flags,
        lorawan_dr,
        link_flags,
        downlink_rssi,
        downlink_snr,
        power_flags,
        battery_mv,
        crc16,
        _reserved,
    ) = RECORD_STRUCT_V5.unpack(chunk)

    if magic != MAGIC or version != VERSION_V5 or record_size != RECORD_SIZE_V5:
        return None

    if crc16_ccitt(chunk[:44]) != crc16:
        return None

    return Record(
        record_version=version,
        offset=offset,
        seq=seq,
        uptime_ms=uptime_ms,
        utc_packed=utc_packed,
        latitude=latitude,
        longitude=longitude,
        speed_cm_s=speed_cm_s,
        hdop=hdop,
        interval_s=interval_s,
        send_ret=send_ret,
        motion_state=motion_state,
        result=result,
        satellites=satellites,
        flags=flags,
        lorawan_dr=lorawan_dr,
        link_flags=link_flags,
        downlink_rssi=downlink_rssi,
        downlink_snr=downlink_snr,
        power_flags=power_flags,
        battery_mv=battery_mv if power_flags & POWER_FLAG_BATTERY_VALID else None,
    )


def parse_record_v6(raw: bytes, offset: int) -> Record | None:
    chunk = raw[offset : offset + RECORD_SIZE_V6]
    if len(chunk) != RECORD_SIZE_V6 or chunk == b"\xff" * RECORD_SIZE_V6:
        return None

    (
        magic,
        version,
        record_size,
        seq,
        uptime_ms,
        utc_packed,
        latitude,
        longitude,
        speed_cm_s,
        hdop,
        interval_s,
        send_ret,
        motion_state,
        result,
        satellites,
        flags,
        lorawan_dr,
        link_flags,
        downlink_rssi,
        downlink_snr,
        power_flags,
        battery_mv,
        battery_pin_mv,
        crc16,
    ) = RECORD_STRUCT_V6.unpack(chunk)

    if magic != MAGIC or version != VERSION_V6 or record_size != RECORD_SIZE_V6:
        return None

    if crc16_ccitt(chunk[:46]) != crc16:
        return None

    return Record(
        record_version=version,
        offset=offset,
        seq=seq,
        uptime_ms=uptime_ms,
        utc_packed=utc_packed,
        latitude=latitude,
        longitude=longitude,
        speed_cm_s=speed_cm_s,
        hdop=hdop,
        interval_s=interval_s,
        send_ret=send_ret,
        motion_state=motion_state,
        result=result,
        satellites=satellites,
        flags=flags,
        lorawan_dr=lorawan_dr,
        link_flags=link_flags,
        downlink_rssi=downlink_rssi,
        downlink_snr=downlink_snr,
        power_flags=power_flags,
        battery_mv=battery_mv if power_flags & POWER_FLAG_BATTERY_VALID else None,
        battery_pin_mv=(
            battery_pin_mv
            if power_flags & POWER_FLAG_BATTERY_PIN_VALID
            else None
        ),
    )


def parse_dump(path: Path) -> list[Record]:
    raw = path.read_bytes()
    found: dict[tuple[int, int], Record] = {}

    for offset in iter_record_offsets(len(raw), RECORD_SIZE_V1):
        if (record := parse_record_v1(raw, offset)) is not None:
            found[(record.offset, record.record_version)] = record

    for offset in iter_record_offsets(len(raw), RECORD_SIZE_V2):
        if (record := parse_record_v2(raw, offset)) is not None:
            found[(record.offset, record.record_version)] = record

    for offset in iter_record_offsets(len(raw), RECORD_SIZE_V3):
        if (record := parse_record_v3(raw, offset)) is not None:
            found[(record.offset, record.record_version)] = record

    for offset in iter_record_offsets(len(raw), RECORD_SIZE_V4):
        if (record := parse_record_v4(raw, offset)) is not None:
            found[(record.offset, record.record_version)] = record

    for offset in iter_record_offsets(len(raw), RECORD_SIZE_V5):
        if (record := parse_record_v5(raw, offset)) is not None:
            found[(record.offset, record.record_version)] = record

    for offset in iter_record_offsets(len(raw), RECORD_SIZE_V6):
        if (record := parse_record_v6(raw, offset)) is not None:
            found[(record.offset, record.record_version)] = record

    return sorted(found.values(), key=lambda record: (record.seq, record.record_version))


def optional_int(value: int | None) -> str:
    return "" if value is None else str(value)


def battery_gpio35_mv(record: Record) -> int | None:
    if record.record_version in (VERSION_V3, VERSION_V4):
        if record.power_flags & POWER_FLAG_BATTERY_VALID:
            return record.battery_mv
    return None


def battery_gpio34_mv(record: Record) -> int | None:
    if record.record_version == VERSION_V4:
        if record.power_flags & POWER_FLAG_BATTERY_GPIO34_VALID:
            return record.battery_gpio34_mv
    if record.record_version >= VERSION_V5:
        if record.power_flags & POWER_FLAG_BATTERY_VALID:
            return record.battery_mv
    return None


def canonical_battery_mv(record: Record) -> int | None:
    return battery_gpio34_mv(record)


def battery_pin_mv(record: Record) -> int | None:
    if record.record_version >= VERSION_V6:
        if record.power_flags & POWER_FLAG_BATTERY_PIN_VALID:
            return record.battery_pin_mv
    return None


def battery_saturated(record: Record) -> bool:
    return record.record_version >= VERSION_V6 and bool(
        record.power_flags & POWER_FLAG_BATTERY_SATURATED
    )


def record_to_row(record: Record) -> dict[str, object]:
    flags = record.flags
    link_flags = record.link_flags
    downlink_valid = bool(link_flags & LINK_FLAG_DOWNLINK_VALID)
    canonical_battery = canonical_battery_mv(record)
    gpio35_battery = battery_gpio35_mv(record)
    gpio34_battery = battery_gpio34_mv(record)
    pin_battery = battery_pin_mv(record)
    return {
        "record_version": record.record_version,
        "seq": record.seq,
        "offset_hex": f"0x{record.offset:05x}",
        "uptime_ms": record.uptime_ms,
        "uptime_s": f"{record.uptime_ms / 1000:.3f}",
        "utc": record.utc_iso,
        "lat": f"{record.lat_deg:.9f}",
        "lon": f"{record.lon_deg:.9f}",
        "speed_m_s": f"{record.speed_m_s:.2f}",
        "speed_kmh": f"{record.speed_kmh:.2f}",
        "hdop": record.hdop,
        "interval_s": record.interval_s,
        "motion_state": record.motion_name,
        "result": record.result_name,
        "send_ret": record.send_ret,
        "satellites": record.satellites,
        "accel_valid": bool(flags & FLAG_ACCEL_VALID),
        "accel_moving": bool(flags & FLAG_ACCEL_MOVING),
        "drift_suppressed": bool(flags & FLAG_DRIFT_SUPPRESSED),
        "motion_resume": bool(flags & FLAG_MOTION_RESUME),
        "utc_valid": bool(flags & FLAG_UTC_VALID),
        "flags_hex": f"0x{flags:02x}",
        "lorawan_dr": optional_int(record.lorawan_dr) if link_flags & LINK_FLAG_DR_VALID else "",
        "adr_enabled": bool(link_flags & LINK_FLAG_ADR_ENABLED),
        "confirmed": bool(link_flags & LINK_FLAG_CONFIRMED),
        "downlink_valid": downlink_valid,
        "downlink_rssi": optional_int(record.downlink_rssi) if downlink_valid else "",
        "downlink_snr": optional_int(record.downlink_snr) if downlink_valid else "",
        "link_flags_hex": f"0x{link_flags:02x}",
        "battery_valid": canonical_battery is not None,
        "battery_mv": optional_int(canonical_battery),
        "battery_gpio35_valid": gpio35_battery is not None,
        "battery_gpio35_mv": optional_int(gpio35_battery),
        "battery_gpio34_valid": gpio34_battery is not None,
        "battery_gpio34_mv": optional_int(gpio34_battery),
        "battery_pin_valid": pin_battery is not None,
        "battery_pin_mv": optional_int(pin_battery),
        "battery_saturated": battery_saturated(record),
        "power_flags_hex": f"0x{record.power_flags:02x}",
    }


def write_csv(records: list[Record], path: Path) -> None:
    rows = [record_to_row(record) for record in records]
    fieldnames = list(rows[0].keys()) if rows else list(
        record_to_row(Record(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)).keys()
    )
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def write_geojson(records: list[Record], path: Path) -> None:
    features = []
    for record in records:
        row = record_to_row(record)
        geometry = None
        if record.result_name != "no_fix":
            geometry = {
                "type": "Point",
                "coordinates": [record.lon_deg, record.lat_deg],
            }
        features.append(
            {
                "type": "Feature",
                "geometry": geometry,
                "properties": row,
            }
        )

    path.write_text(
        json.dumps({"type": "FeatureCollection", "features": features}, indent=2),
        encoding="utf-8",
    )


def load_geojson(path: Path) -> dict[str, Any]:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ValueError(f"could not read GeoJSON overlay {path}: {exc}") from exc

    if not isinstance(data, dict) or data.get("type") != "FeatureCollection":
        raise ValueError(f"GeoJSON overlay {path} is not a FeatureCollection")
    return data


def find_neighbourhoods_geojson(dump_path: Path, out_dir: Path) -> Path | None:
    for candidate in (out_dir / "neighbourhoods.geojson", dump_path.with_name("neighbourhoods.geojson")):
        if candidate.is_file():
            return candidate
    return None


def write_html_map(records: list[Record], path: Path, neighbourhoods_geojson: dict[str, Any] | None = None) -> None:
    points = [
        {
            "seq": record.seq,
            "lat": record.lat_deg,
            "lon": record.lon_deg,
            "utc": record.utc_iso,
            "uptime_s": round(record.uptime_ms / 1000, 3),
            "speed_kmh": round(record.speed_kmh, 2),
            "interval_s": record.interval_s,
            "motion_state": record.motion_name,
            "result": record.result_name,
            "satellites": record.satellites,
            "send_ret": record.send_ret,
            "record_version": record.record_version,
            "lorawan_dr": record.lorawan_dr if record.link_flags & LINK_FLAG_DR_VALID else None,
            "adr_enabled": bool(record.link_flags & LINK_FLAG_ADR_ENABLED),
            "confirmed": bool(record.link_flags & LINK_FLAG_CONFIRMED),
            "downlink_rssi": record.downlink_rssi if record.link_flags & LINK_FLAG_DOWNLINK_VALID else None,
            "downlink_snr": record.downlink_snr if record.link_flags & LINK_FLAG_DOWNLINK_VALID else None,
            "battery_mv": canonical_battery_mv(record),
            "battery_gpio35_mv": battery_gpio35_mv(record),
            "battery_gpio34_mv": battery_gpio34_mv(record),
            "battery_pin_mv": battery_pin_mv(record),
            "battery_saturated": battery_saturated(record),
        }
        for record in records
    ]

    data_json = json.dumps(points)
    neighbourhoods_json = json.dumps(neighbourhoods_geojson).replace("</", "<\\/") if neighbourhoods_geojson else "null"
    escaped_title = html.escape(path.stem)

    path.write_text(
        f"""<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>{escaped_title}</title>
  <link rel="stylesheet" href="https://unpkg.com/leaflet@1.9.4/dist/leaflet.css">
  <style>
    html, body, #map {{ height: 100%; margin: 0; }}
    body {{ font-family: system-ui, -apple-system, Segoe UI, sans-serif; }}
    .panel {{
      position: absolute;
      z-index: 1000;
      top: 12px;
      left: 12px;
      max-width: 360px;
      background: rgba(255, 255, 255, 0.94);
      border: 1px solid #c9ced6;
      border-radius: 6px;
      padding: 10px 12px;
      box-shadow: 0 6px 22px rgba(31, 41, 55, 0.16);
      font-size: 13px;
      line-height: 1.35;
    }}
    .panel strong {{ display: block; font-size: 14px; margin-bottom: 4px; }}
    .controls {{
      display: flex;
      gap: 8px;
      align-items: center;
      flex-wrap: wrap;
      margin: 8px 0;
    }}
    .controls label {{
      display: inline-flex;
      gap: 6px;
      align-items: center;
      white-space: nowrap;
    }}
    .legend {{ margin-top: 8px; display: grid; gap: 4px; }}
    .dot {{ display: inline-block; width: 10px; height: 10px; border-radius: 50%; margin-right: 6px; }}
    .neighbourhood-label {{
      background: rgba(17, 24, 39, 0.82);
      border: 0;
      border-radius: 4px;
      color: #fff;
      font-size: 11px;
      font-weight: 600;
      padding: 2px 5px;
      box-shadow: none;
    }}
    .neighbourhood-label::before {{ display: none; }}
  </style>
</head>
<body>
  <div id="map"></div>
  <div class="panel">
    <strong>Tracker diagnostic log</strong>
    <div class="controls">
      <label><input type="checkbox" id="showNeighbourhoods" checked> Neighbourhoods</label>
      <label><input type="checkbox" id="centerNeighbourhoods" checked> Power Hill at gateway</label>
    </div>
    <div id="summary"></div>
    <div class="legend">
      <div><span class="dot" style="background:#2563eb"></span>moving</div>
      <div><span class="dot" style="background:#f59e0b"></span>active</div>
      <div><span class="dot" style="background:#16a34a"></span>stationary</div>
      <div><span class="dot" style="background:#dc2626"></span>send failed</div>
      <div><span class="dot" style="background:#7c3aed"></span>not joined</div>
      <div><span class="dot" style="background:#6b7280"></span>no GPS fix / heartbeat</div>
    </div>
  </div>
  <script src="https://unpkg.com/leaflet@1.9.4/dist/leaflet.js"></script>
  <script>
    const points = {data_json};
    const neighbourhoodGeoJson = {neighbourhoods_json};
    const map = L.map('map');
    L.tileLayer('https://tile.openstreetmap.org/{{z}}/{{x}}/{{y}}.png', {{
      maxZoom: 19,
      attribution: '&copy; OpenStreetMap contributors'
    }}).addTo(map);
    map.createPane('neighbourhoodPane');
    map.getPane('neighbourhoodPane').style.zIndex = 350;

    const neighbourhoodLayer = L.layerGroup().addTo(map);
    const showNeighbourhoods = document.getElementById('showNeighbourhoods');
    const centerNeighbourhoods = document.getElementById('centerNeighbourhoods');
    const neighbourhoodAnchor = {{
      featureName: 'Power Hill',
      target: {{ lat: 60.220101984, lon: 24.836646496 }}
    }};
    if (!neighbourhoodGeoJson) {{
      showNeighbourhoods.checked = false;
      showNeighbourhoods.disabled = true;
      centerNeighbourhoods.disabled = true;
    }}

    function escapeHtml(value) {{
      const replacements = {{ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }};
      return String(value ?? '').replace(/[&<>"']/g, char => replacements[char]);
    }}

    const colorFor = (point) => {{
      if (point.result === 'send_failed') return '#dc2626';
      if (point.result === 'not_joined') return '#7c3aed';
      if (point.motion_state === 'moving') return '#2563eb';
      if (point.motion_state === 'active') return '#f59e0b';
      if (point.motion_state === 'stationary') return '#16a34a';
      return '#6b7280';
    }};

    function isLonLat(value) {{
      return Array.isArray(value)
        && value.length >= 2
        && typeof value[0] === 'number'
        && typeof value[1] === 'number';
    }}

    function walkCoordinates(coords, visitor) {{
      if (isLonLat(coords)) {{
        visitor(coords[0], coords[1]);
        return;
      }}
      if (!Array.isArray(coords)) return;
      for (const child of coords) walkCoordinates(child, visitor);
    }}

    function transformCoordinates(coords, transform) {{
      if (isLonLat(coords)) {{
        const [lon, lat, ...rest] = coords;
        const [newLon, newLat] = transform(lon, lat);
        return [newLon, newLat, ...rest];
      }}
      if (!Array.isArray(coords)) return coords;
      return coords.map(child => transformCoordinates(child, transform));
    }}

    function geoJsonCenter(geojson, featureName = '') {{
      let minLon = Infinity;
      let minLat = Infinity;
      let maxLon = -Infinity;
      let maxLat = -Infinity;
      const features = Array.isArray(geojson?.features) ? geojson.features : [];
      const wantedName = String(featureName || '').toLowerCase();
      for (const feature of features) {{
        if (wantedName) {{
          const props = (feature && feature.properties) || {{}};
          const names = [props.name, props._name].map(value => String(value || '').toLowerCase());
          if (!names.includes(wantedName)) continue;
        }}
        const geometry = feature?.geometry;
        if (!geometry?.coordinates) continue;
        walkCoordinates(geometry.coordinates, (lon, lat) => {{
          minLon = Math.min(minLon, lon);
          minLat = Math.min(minLat, lat);
          maxLon = Math.max(maxLon, lon);
          maxLat = Math.max(maxLat, lat);
        }});
      }}
      if (!Number.isFinite(minLon) || !Number.isFinite(minLat)) return null;
      return {{
        lon: (minLon + maxLon) / 2,
        lat: (minLat + maxLat) / 2
      }};
    }}

    function translateLonLat(lon, lat, sourceCenter, targetCenter) {{
      const metersPerDegreeLat = 111320;
      const sourceLatRad = sourceCenter.lat * Math.PI / 180;
      const targetLatRad = targetCenter.lat * Math.PI / 180;
      const eastMeters = (lon - sourceCenter.lon) * metersPerDegreeLat * Math.cos(sourceLatRad);
      const northMeters = (lat - sourceCenter.lat) * metersPerDegreeLat;
      const targetCos = Math.max(0.1, Math.abs(Math.cos(targetLatRad)));
      return [
        targetCenter.lon + eastMeters / (metersPerDegreeLat * targetCos),
        targetCenter.lat + northMeters / metersPerDegreeLat
      ];
    }}

    function neighbourhoodName(feature) {{
      const props = (feature && feature.properties) || {{}};
      return props.name || props._name || props.type || 'Neighbourhood';
    }}

    function neighbourhoodPopup(feature) {{
      const props = (feature && feature.properties) || {{}};
      const details = [];
      if (props.tagline) details.push(escapeHtml(props.tagline));
      if (props.camping_allowed !== undefined) details.push(`camping: ${{escapeHtml(props.camping_allowed)}}`);
      if (centerNeighbourhoods.checked) {{
        details.push(`${{neighbourhoodAnchor.featureName}} anchored at gateway ${{neighbourhoodAnchor.target.lat.toFixed(9)}}, ${{neighbourhoodAnchor.target.lon.toFixed(9)}}`);
      }}
      return `<strong>${{escapeHtml(neighbourhoodName(feature))}}</strong>${{details.length ? '<br>' + details.join('<br>') : ''}}`;
    }}

    function translatedNeighbourhoodGeoJson() {{
      if (!neighbourhoodGeoJson) return null;
      if (!centerNeighbourhoods.checked) return neighbourhoodGeoJson;

      const sourceCenter = geoJsonCenter(neighbourhoodGeoJson, neighbourhoodAnchor.featureName) || geoJsonCenter(neighbourhoodGeoJson);
      if (!sourceCenter) return neighbourhoodGeoJson;
      const targetCenter = neighbourhoodAnchor.target;

      const clone = JSON.parse(JSON.stringify(neighbourhoodGeoJson));
      for (const feature of clone.features || []) {{
        if (!feature.geometry?.coordinates) continue;
        feature.geometry.coordinates = transformCoordinates(
          feature.geometry.coordinates,
          (lon, lat) => translateLonLat(lon, lat, sourceCenter, targetCenter)
        );
      }}
      return clone;
    }}

    function drawNeighbourhoods() {{
      neighbourhoodLayer.clearLayers();
      if (!showNeighbourhoods.checked || !neighbourhoodGeoJson) return null;

      const geojson = translatedNeighbourhoodGeoJson();
      if (!geojson) return null;

      const layer = L.geoJSON(geojson, {{
        pane: 'neighbourhoodPane',
        style: () => ({{
          color: '#111827',
          weight: 2,
          opacity: 0.82,
          fillColor: '#f59e0b',
          fillOpacity: 0.13,
          dashArray: centerNeighbourhoods.checked ? '5 4' : undefined
        }}),
        onEachFeature: (feature, featureLayer) => {{
          featureLayer.bindPopup(neighbourhoodPopup(feature));
          featureLayer.bindTooltip(escapeHtml(neighbourhoodName(feature)), {{
            sticky: true,
            className: 'neighbourhood-label'
          }});
        }}
      }}).addTo(neighbourhoodLayer);

      const bounds = layer.getBounds();
      return bounds.isValid() ? bounds : null;
    }}

    function fitMap(overlayBounds) {{
      let bounds = latLngs.length ? L.latLngBounds(latLngs) : null;
      if (overlayBounds && overlayBounds.isValid()) {{
        bounds = bounds ? bounds.extend(overlayBounds) : overlayBounds;
      }}
      if (bounds && bounds.isValid()) {{
        map.fitBounds(bounds, {{ padding: [28, 28], maxZoom: 17 }});
      }} else {{
        map.setView([0, 0], 2);
      }}
    }}

    const latLngs = [];
    for (const point of points) {{
      if (point.result === 'no_fix') continue;
      if (!Number.isFinite(point.lat) || !Number.isFinite(point.lon)) continue;
      latLngs.push([point.lat, point.lon]);
      const popup = `
        <strong>seq ${{point.seq}}</strong><br>
        UTC: ${{point.utc || 'n/a'}}<br>
        Uptime: ${{point.uptime_s}} s<br>
        Speed: ${{point.speed_kmh}} km/h<br>
        Interval: ${{point.interval_s}} s<br>
        State: ${{point.motion_state}}<br>
        Result: ${{point.result}} (${{point.send_ret}})<br>
        Type: ${{point.confirmed ? 'confirmed' : 'unconfirmed'}}<br>
        DR: ${{point.lorawan_dr ?? 'n/a'}} ADR: ${{point.adr_enabled ? 'on' : 'off'}}<br>
        Downlink: ${{point.downlink_rssi ?? 'n/a'}} dBm / ${{point.downlink_snr ?? 'n/a'}} dB<br>
        Battery: ${{point.battery_mv ?? 'n/a'}} mV<br>
        Battery ADC pin: ${{point.battery_pin_mv ?? 'n/a'}} mV${{point.battery_saturated ? ' (saturated)' : ''}}<br>
        Battery GPIO35 candidate: ${{point.battery_gpio35_mv ?? 'n/a'}} mV<br>
        Battery GPIO34: ${{point.battery_gpio34_mv ?? 'n/a'}} mV<br>
        Satellites: ${{point.satellites}}<br>
        Record v${{point.record_version}}
      `;
      L.circleMarker([point.lat, point.lon], {{
        radius: 5,
        color: colorFor(point),
        fillColor: colorFor(point),
        fillOpacity: 0.78,
        weight: 1
      }}).bindPopup(popup).addTo(map);
    }}

    if (latLngs.length > 1) {{
      L.polyline(latLngs, {{ color: '#111827', weight: 2, opacity: 0.55 }}).addTo(map);
    }}
    fitMap(drawNeighbourhoods());

    showNeighbourhoods.addEventListener('change', () => fitMap(drawNeighbourhoods()));
    centerNeighbourhoods.addEventListener('change', () => fitMap(drawNeighbourhoods()));

    const first = points[0];
    const last = points[points.length - 1];
    document.getElementById('summary').textContent = points.length
      ? `${{points.length}} records, seq ${{first.seq}} to ${{last.seq}}`
      : 'No valid records found';
  </script>
</body>
</html>
""",
        encoding="utf-8",
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dump", type=Path, help="Raw diagnostic-log flash dump")
    parser.add_argument(
        "--out-dir",
        type=Path,
        default=None,
        help="Output directory. Defaults to '<dump stem>-decoded' next to the dump.",
    )
    parser.add_argument(
        "--record-version",
        type=int,
        action="append",
        choices=[VERSION_V1, VERSION_V2, VERSION_V3, VERSION_V4, VERSION_V5],
        default=None,
        help=(
            "Only emit records with this diagnostic record version. "
            "Can be passed multiple times."
        ),
    )
    parser.add_argument(
        "--neighbourhoods-geojson",
        type=Path,
        default=None,
        help=(
            "Optional FeatureCollection overlay embedded in diagnostic-log-map.html. "
            "If omitted, the decoder also checks for neighbourhoods.geojson in the output "
            "directory or beside the dump."
        ),
    )
    args = parser.parse_args()

    dump_path = args.dump
    out_dir = args.out_dir or dump_path.with_name(f"{dump_path.stem}-decoded")
    out_dir.mkdir(parents=True, exist_ok=True)

    all_records = parse_dump(dump_path)
    records = [
        record
        for record in all_records
        if args.record_version is None or record.record_version in args.record_version
    ]
    csv_path = out_dir / "diagnostic-log.csv"
    geojson_path = out_dir / "diagnostic-log.geojson"
    html_path = out_dir / "diagnostic-log-map.html"
    neighbourhoods_path = args.neighbourhoods_geojson or find_neighbourhoods_geojson(dump_path, out_dir)
    neighbourhoods_geojson = None
    if neighbourhoods_path is not None:
        try:
            neighbourhoods_geojson = load_geojson(neighbourhoods_path)
        except ValueError as exc:
            parser.error(str(exc))

    write_csv(records, csv_path)
    write_geojson(records, geojson_path)
    write_html_map(records, html_path, neighbourhoods_geojson)

    print(f"decoded_records={len(records)}")
    if args.record_version is not None:
        print(f"raw_decoded_records={len(all_records)}")
        print(f"record_versions={','.join(str(version) for version in args.record_version)}")
    print(f"csv={csv_path}")
    print(f"geojson={geojson_path}")
    print(f"html_map={html_path}")
    if neighbourhoods_path is not None:
        print(f"neighbourhoods_geojson={neighbourhoods_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
