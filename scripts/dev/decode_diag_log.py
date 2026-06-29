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
from typing import Iterable


MAGIC = 0xD107
VERSION = 1
RECORD_SIZE = 40
SECTOR_SIZE = 4096
RECORDS_PER_SECTOR = SECTOR_SIZE // RECORD_SIZE
RECORD_STRUCT = struct.Struct("<HBBIIIiiHHHhBBBBHH")

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
}

FLAG_ACCEL_VALID = 1 << 0
FLAG_ACCEL_MOVING = 1 << 1
FLAG_DRIFT_SUPPRESSED = 1 << 2
FLAG_MOTION_RESUME = 1 << 3
FLAG_UTC_VALID = 1 << 4


@dataclass(frozen=True)
class Record:
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


def iter_record_offsets(size: int) -> Iterable[int]:
    sectors = size // SECTOR_SIZE
    for sector in range(sectors):
        sector_offset = sector * SECTOR_SIZE
        for slot in range(RECORDS_PER_SECTOR):
            offset = sector_offset + slot * RECORD_SIZE
            if offset + RECORD_SIZE <= size:
                yield offset


def parse_record(raw: bytes, offset: int) -> Record | None:
    chunk = raw[offset : offset + RECORD_SIZE]
    if len(chunk) != RECORD_SIZE or chunk == b"\xff" * RECORD_SIZE:
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
    ) = RECORD_STRUCT.unpack(chunk)

    if magic != MAGIC or version != VERSION or record_size != RECORD_SIZE:
        return None

    if crc16_ccitt(chunk[:36]) != crc16:
        return None

    return Record(
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


def parse_dump(path: Path) -> list[Record]:
    raw = path.read_bytes()
    records = [
        record
        for offset in iter_record_offsets(len(raw))
        if (record := parse_record(raw, offset)) is not None
    ]
    return sorted(records, key=lambda record: record.seq)


def record_to_row(record: Record) -> dict[str, object]:
    flags = record.flags
    return {
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
    }


def write_csv(records: list[Record], path: Path) -> None:
    rows = [record_to_row(record) for record in records]
    fieldnames = list(rows[0].keys()) if rows else list(record_to_row(Record(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)).keys())
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def write_geojson(records: list[Record], path: Path) -> None:
    features = []
    for record in records:
        row = record_to_row(record)
        features.append(
            {
                "type": "Feature",
                "geometry": {
                    "type": "Point",
                    "coordinates": [record.lon_deg, record.lat_deg],
                },
                "properties": row,
            }
        )

    path.write_text(
        json.dumps({"type": "FeatureCollection", "features": features}, indent=2),
        encoding="utf-8",
    )


def write_html_map(records: list[Record], path: Path) -> None:
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
        }
        for record in records
    ]

    data_json = json.dumps(points)
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
    .legend {{ margin-top: 8px; display: grid; gap: 4px; }}
    .dot {{ display: inline-block; width: 10px; height: 10px; border-radius: 50%; margin-right: 6px; }}
  </style>
</head>
<body>
  <div id="map"></div>
  <div class="panel">
    <strong>Tracker diagnostic log</strong>
    <div id="summary"></div>
    <div class="legend">
      <div><span class="dot" style="background:#2563eb"></span>moving</div>
      <div><span class="dot" style="background:#f59e0b"></span>active</div>
      <div><span class="dot" style="background:#16a34a"></span>stationary</div>
      <div><span class="dot" style="background:#dc2626"></span>send failed</div>
      <div><span class="dot" style="background:#7c3aed"></span>not joined</div>
    </div>
  </div>
  <script src="https://unpkg.com/leaflet@1.9.4/dist/leaflet.js"></script>
  <script>
    const points = {data_json};
    const map = L.map('map');
    L.tileLayer('https://tile.openstreetmap.org/{{z}}/{{x}}/{{y}}.png', {{
      maxZoom: 19,
      attribution: '&copy; OpenStreetMap contributors'
    }}).addTo(map);

    const colorFor = (point) => {{
      if (point.result === 'send_failed') return '#dc2626';
      if (point.result === 'not_joined') return '#7c3aed';
      if (point.motion_state === 'moving') return '#2563eb';
      if (point.motion_state === 'active') return '#f59e0b';
      if (point.motion_state === 'stationary') return '#16a34a';
      return '#6b7280';
    }};

    const latLngs = [];
    for (const point of points) {{
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
        Satellites: ${{point.satellites}}
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
      map.fitBounds(latLngs, {{ padding: [28, 28] }});
    }} else if (latLngs.length === 1) {{
      map.setView(latLngs[0], 15);
    }} else {{
      map.setView([0, 0], 2);
    }}

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
    args = parser.parse_args()

    dump_path = args.dump
    out_dir = args.out_dir or dump_path.with_name(f"{dump_path.stem}-decoded")
    out_dir.mkdir(parents=True, exist_ok=True)

    records = parse_dump(dump_path)
    csv_path = out_dir / "diagnostic-log.csv"
    geojson_path = out_dir / "diagnostic-log.geojson"
    html_path = out_dir / "diagnostic-log-map.html"

    write_csv(records, csv_path)
    write_geojson(records, geojson_path)
    write_html_map(records, html_path)

    print(f"decoded_records={len(records)}")
    print(f"csv={csv_path}")
    print(f"geojson={geojson_path}")
    print(f"html_map={html_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
