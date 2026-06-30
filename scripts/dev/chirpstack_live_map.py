#!/usr/bin/env python3
"""Live ChirpStack position logger for the tracker firmware.

This script listens to ChirpStack application events for one or more devices,
decodes the current tracker port-4 position payload, writes CSV/GeoJSON/JSON
outputs, and serves a small live map.

Configuration is intentionally passed by CLI/env so private bench endpoints and
credentials do not need to be committed.
"""

from __future__ import annotations

import argparse
import base64
import csv
import functools
import html
import json
import os
import signal
import struct
import sys
import threading
import time
from datetime import datetime, timezone
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any

import grpc
from chirpstack_api import api
from google.protobuf.json_format import MessageToDict


POSITION_PORT = 4
POSITION_PAYLOAD = struct.Struct("<iiH")


def point_key(point: dict[str, Any]) -> tuple[Any, ...]:
    return (
        point.get("dev_eui"),
        point.get("network_time"),
        point.get("dev_addr"),
        point.get("f_cnt"),
        point.get("f_port"),
        point.get("raw_hex"),
    )


class LiveState:
    def __init__(
        self,
        out_dir: Path,
        max_points: int,
        initial_points: list[dict[str, Any]] | None = None,
        known_devices: dict[str, str] | None = None,
    ) -> None:
        self.out_dir = out_dir
        self.max_points = max_points
        self.lock = threading.Lock()
        self.points = list(initial_points or [])
        self.seen_keys: set[tuple[Any, ...]] = set()
        self.device_events = {
            dev_eui: {"dev_eui": dev_eui, "device_label": label}
            for dev_eui, label in (known_devices or {}).items()
        }
        self._trim_locked()

    def _trim_locked(self) -> None:
        if len(self.points) > self.max_points:
            self.points = self.points[-self.max_points :]
        self.seen_keys = {point_key(point) for point in self.points}

    def add_point(self, point: dict[str, Any]) -> bool:
        with self.lock:
            key = point_key(point)
            if key in self.seen_keys:
                return False

            self.points.append(point)
            self.seen_keys.add(key)
            self._trim_locked()
            write_outputs(self.out_dir, self.points)
            return True

    def flush(self) -> None:
        with self.lock:
            write_outputs(self.out_dir, self.points)
            write_device_events(self.out_dir, self.device_events)

    def record_event(
        self,
        dev_eui: str,
        device_label: str,
        description: str,
        body: dict[str, Any],
    ) -> None:
        with self.lock:
            self.device_events[dev_eui] = {
                "received_at": utc_now(),
                "network_time": (body.get("time") or "").replace("+00:00", "Z"),
                "dev_eui": dev_eui,
                "device_label": device_label,
                "event": description,
                "dev_addr": body.get("devAddr") or body.get("dev_addr") or "",
                "f_cnt": body_get(body, "f_cnt", "fCnt"),
                "f_port": body_get(body, "f_port", "fPort"),
            }
            write_device_events(self.out_dir, self.device_events)


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z")


def point_time(point: dict[str, Any]) -> str:
    return str(point.get("network_time") or point.get("received_at") or "")


def latest_point(points: list[dict[str, Any]]) -> dict[str, Any]:
    if not points:
        return {}
    return max(points, key=point_time)


def latest_points_by_device(points: list[dict[str, Any]]) -> list[dict[str, Any]]:
    latest: dict[str, dict[str, Any]] = {}
    counts: dict[str, int] = {}
    for point in points:
        dev_eui = str(point.get("dev_eui") or "unknown")
        counts[dev_eui] = counts.get(dev_eui, 0) + 1
        if dev_eui not in latest or point_time(point) >= point_time(latest[dev_eui]):
            latest[dev_eui] = point

    summaries = []
    for dev_eui, point in sorted(latest.items()):
        summaries.append(
            {
                "dev_eui": dev_eui,
                "device_label": point.get("device_label") or dev_eui,
                "point_count": counts.get(dev_eui, 0),
                "latest": point,
            }
        )
    return summaries


def decode_position(raw: bytes) -> tuple[float, float, float] | None:
    if len(raw) != POSITION_PAYLOAD.size:
        return None

    lat_i, lon_i, hdop = POSITION_PAYLOAD.unpack(raw)
    return (lat_i << 5) / 1_000_000_000, (lon_i << 5) / 1_000_000_000, hdop / 1000


def body_get(body: dict[str, Any], snake: str, camel: str) -> Any:
    return body.get(snake) if snake in body else body.get(camel)


def best_rx(body: dict[str, Any]) -> dict[str, Any]:
    rx_info = body.get("rx_info") or body.get("rxInfo") or []
    if not rx_info:
        return {}
    return max(rx_info, key=lambda rx: rx.get("snr", -999))


def tx_info(body: dict[str, Any]) -> dict[str, Any]:
    return body.get("tx_info") or body.get("txInfo") or {}


def tx_lora_modulation(tx: dict[str, Any]) -> dict[str, Any]:
    modulation = tx.get("modulation") or {}
    if not isinstance(modulation, dict):
        return {}

    lora = modulation.get("lora") or modulation.get("LoRa") or {}
    return lora if isinstance(lora, dict) else {}


def parse_uplink_point(body: dict[str, Any], dev_eui: str, device_label: str) -> dict[str, Any] | None:
    port = body_get(body, "f_port", "fPort")
    data_b64 = body.get("data")
    if port != POSITION_PORT or not data_b64:
        return None

    raw = base64.b64decode(data_b64)
    decoded = decode_position(raw)
    if decoded is None:
        return None

    lat, lon, hdop = decoded
    rx = best_rx(body)
    tx = tx_info(body)
    lora = tx_lora_modulation(tx)
    return {
        "received_at": utc_now(),
        "network_time": (body.get("time") or "").replace("+00:00", "Z"),
        "dev_eui": dev_eui,
        "device_label": device_label,
        "dev_addr": body.get("devAddr") or body.get("dev_addr") or "",
        "f_cnt": body_get(body, "f_cnt", "fCnt"),
        "f_port": port,
        "lat": round(lat, 9),
        "lon": round(lon, 9),
        "hdop": round(hdop, 3),
        "raw_hex": raw.hex(),
        "rssi": rx.get("rssi"),
        "snr": rx.get("snr"),
        "gateway_id": rx.get("gateway_id") or rx.get("gatewayId") or "",
        "frequency": body_get(tx, "frequency", "frequency"),
        "spreading_factor": body_get(lora, "spreading_factor", "spreadingFactor"),
        "bandwidth": body_get(lora, "bandwidth", "bandwidth"),
        "code_rate": body_get(lora, "code_rate", "codeRate"),
    }


def atomic_write(path: Path, content: str) -> None:
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(content, encoding="utf-8")
    tmp.replace(path)


def load_existing_points(path: Path, max_points: int) -> list[dict[str, Any]]:
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        return []
    except (OSError, json.JSONDecodeError) as exc:
        print(f"[{utc_now()}] ignoring unreadable existing points file {path}: {exc}", flush=True)
        return []

    if not isinstance(raw, list):
        print(f"[{utc_now()}] ignoring existing points file {path}: expected a JSON list", flush=True)
        return []

    points = [
        point
        for point in raw
        if isinstance(point, dict)
        and isinstance(point.get("lat"), (int, float))
        and isinstance(point.get("lon"), (int, float))
    ]
    if len(points) != len(raw):
        print(f"[{utc_now()}] ignored {len(raw) - len(points)} malformed existing point records", flush=True)

    deduplicated = []
    seen_keys: set[tuple[Any, ...]] = set()
    for point in points:
        key = point_key(point)
        if key in seen_keys:
            continue
        seen_keys.add(key)
        deduplicated.append(point)

    if len(deduplicated) != len(points):
        print(f"[{utc_now()}] ignored {len(points) - len(deduplicated)} duplicate existing point records", flush=True)

    return deduplicated[-max_points:]


def write_outputs(out_dir: Path, points: list[dict[str, Any]]) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)

    atomic_write(out_dir / "points.json", json.dumps(points, indent=2))
    atomic_write(out_dir / "latest.json", json.dumps(latest_point(points), indent=2))
    atomic_write(out_dir / "latest_by_device.json", json.dumps(latest_points_by_device(points), indent=2))

    fieldnames = [
        "received_at",
        "network_time",
        "dev_eui",
        "device_label",
        "dev_addr",
        "f_cnt",
        "f_port",
        "lat",
        "lon",
        "hdop",
        "rssi",
        "snr",
        "gateway_id",
        "frequency",
        "spreading_factor",
        "bandwidth",
        "code_rate",
        "raw_hex",
    ]
    csv_path = out_dir / "positions.csv"
    with csv_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(points)

    features = [
        {
            "type": "Feature",
            "geometry": {"type": "Point", "coordinates": [point["lon"], point["lat"]]},
            "properties": point,
        }
        for point in points
    ]
    atomic_write(
        out_dir / "positions.geojson",
        json.dumps({"type": "FeatureCollection", "features": features}, indent=2),
    )


def write_device_events(out_dir: Path, device_events: dict[str, dict[str, Any]]) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)
    events = sorted(device_events.values(), key=lambda event: str(event.get("device_label") or event.get("dev_eui") or ""))
    atomic_write(out_dir / "device_events.json", json.dumps(events, indent=2))


def write_index(out_dir: Path, title: str) -> None:
    escaped_title = html.escape(title)
    atomic_write(
        out_dir / "index.html",
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
      width: min(430px, calc(100vw - 24px));
      background: rgba(255, 255, 255, 0.95);
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
    .controls select {{
      min-width: 150px;
      max-width: 100%;
    }}
    .muted {{ color: #475569; }}
    .fresh {{ color: #047857; }}
    .stale {{ color: #b45309; }}
    .device-row {{
      display: flex;
      justify-content: space-between;
      gap: 10px;
      border-top: 1px solid #e2e8f0;
      padding-top: 5px;
      margin-top: 5px;
    }}
    .dot {{
      display: inline-block;
      width: 9px;
      height: 9px;
      border-radius: 50%;
      margin-right: 5px;
      vertical-align: -1px;
    }}
  </style>
</head>
<body>
  <div id="map"></div>
  <div class="panel">
    <strong>{escaped_title}</strong>
    <div class="controls">
      <label><input type="checkbox" id="recentOnly" checked> Last 24h</label>
      <select id="deviceFilter" aria-label="Device filter">
        <option value="">All devices</option>
      </select>
    </div>
    <div id="summary" class="muted">Waiting for LoRaWAN positions...</div>
  </div>
  <script src="https://unpkg.com/leaflet@1.9.4/dist/leaflet.js"></script>
  <script>
    const map = L.map('map').setView([0, 0], 2);
    L.tileLayer('https://tile.openstreetmap.org/{{z}}/{{x}}/{{y}}.png', {{
      maxZoom: 19,
      attribution: '&copy; OpenStreetMap contributors'
    }}).addTo(map);

    const pointLayer = L.layerGroup().addTo(map);
    let lines = [];
    let fitted = false;
    const recentOnly = document.getElementById('recentOnly');
    const deviceFilter = document.getElementById('deviceFilter');
    const colors = ['#2563eb', '#dc2626', '#059669', '#7c3aed', '#c2410c', '#0891b2', '#be123c', '#4d7c0f'];
    const colorByDevice = new Map();

    recentOnly.addEventListener('change', () => {{
      fitted = false;
      refresh().catch(() => {{}});
    }});
    deviceFilter.addEventListener('change', () => {{
      fitted = false;
      refresh().catch(() => {{}});
    }});

    function escapeHtml(value) {{
      const replacements = {{ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }};
      return String(value ?? '').replace(/[&<>"']/g, char => replacements[char]);
    }}

    function normalizeIso(value) {{
      if (!value) return '';
      return String(value).replace(/\\.(\\d{{3}})\\d+(Z|[+-]\\d\\d:\\d\\d)$/, '.$1$2');
    }}

    function pointTimeMs(point) {{
      const value = normalizeIso(point.network_time || point.received_at);
      const parsed = Date.parse(value);
      return Number.isFinite(parsed) ? parsed : 0;
    }}

    function formatTime(point) {{
      const ms = pointTimeMs(point);
      if (!ms) return 'n/a';
      return new Date(ms).toLocaleString();
    }}

    function ageText(ms) {{
      if (!Number.isFinite(ms) || ms < 0) return 'n/a';
      const minutes = Math.floor(ms / 60000);
      if (minutes < 1) return 'just now';
      if (minutes < 60) return `${{minutes}} min`;
      const hours = Math.floor(minutes / 60);
      if (hours < 48) return `${{hours}} h`;
      return `${{Math.floor(hours / 24)}} d`;
    }}

    function deviceId(point) {{
      return point.dev_eui || 'unknown';
    }}

    function shortEui(devEui) {{
      return devEui && devEui.length > 8 ? devEui.slice(-8) : devEui || 'unknown';
    }}

    function deviceLabel(point) {{
      return point.device_label || shortEui(deviceId(point));
    }}

    function colorFor(devEui) {{
      if (!colorByDevice.has(devEui)) {{
        colorByDevice.set(devEui, colors[colorByDevice.size % colors.length]);
      }}
      return colorByDevice.get(devEui);
    }}

    function latestPoint(points) {{
      let latest = null;
      for (const point of points) {{
        if (!latest || pointTimeMs(point) >= pointTimeMs(latest)) latest = point;
      }}
      return latest;
    }}

    function updateDeviceFilter(points, events) {{
      const selected = deviceFilter.value;
      const devices = new Map();
      for (const point of points) {{
        const id = deviceId(point);
        if (!devices.has(id)) devices.set(id, deviceLabel(point));
      }}
      for (const event of events) {{
        const id = deviceId(event);
        if (!devices.has(id)) devices.set(id, deviceLabel(event));
      }}

      deviceFilter.innerHTML = '<option value="">All devices</option>';
      for (const [id, label] of [...devices.entries()].sort((a, b) => a[1].localeCompare(b[1]))) {{
        const option = document.createElement('option');
        option.value = id;
        option.textContent = label;
        deviceFilter.appendChild(option);
      }}
      deviceFilter.value = devices.has(selected) ? selected : '';
    }}

    function filterPoints(points) {{
      const selectedDevice = deviceFilter.value;
      const cutoff = Date.now() - (24 * 60 * 60 * 1000);
      return points
        .filter(point => !selectedDevice || deviceId(point) === selectedDevice)
        .filter(point => !recentOnly.checked || pointTimeMs(point) >= cutoff)
        .sort((a, b) => pointTimeMs(a) - pointTimeMs(b));
    }}

    function groupedByDevice(points) {{
      const grouped = new Map();
      for (const point of points) {{
        const id = deviceId(point);
        if (!grouped.has(id)) grouped.set(id, []);
        grouped.get(id).push(point);
      }}
      return grouped;
    }}

    async function fetchJson(path, fallback) {{
      try {{
        const response = await fetch(path + '?ts=' + Date.now());
        if (!response.ok) return fallback;
        return await response.json();
      }} catch {{
        return fallback;
      }}
    }}

    function popup(point) {{
      return `
        <strong>${{escapeHtml(deviceLabel(point))}} fcnt ${{point.f_cnt ?? 'n/a'}}</strong><br>
        network: ${{point.network_time || 'n/a'}}<br>
        received: ${{point.received_at}}<br>
        lat: ${{point.lat}}<br>
        lon: ${{point.lon}}<br>
        hdop: ${{point.hdop}}<br>
        rssi: ${{point.rssi ?? 'n/a'}} snr: ${{point.snr ?? 'n/a'}}<br>
        freq: ${{point.frequency ?? 'n/a'}} sf: ${{point.spreading_factor ?? 'n/a'}}
      `;
    }}

    async function refresh() {{
      const points = await fetchJson('points.json', []);
      const events = await fetchJson('device_events.json', []);
      updateDeviceFilter(points, events);

      pointLayer.clearLayers();
      for (const existingLine of lines) {{
        map.removeLayer(existingLine);
      }}
      lines = [];

      const visible = filterPoints(points);
      const latLngs = [];
      for (const [devEui, devicePoints] of groupedByDevice(visible)) {{
        const color = colorFor(devEui);
        const deviceLatLngs = [];
        const latestForDevice = latestPoint(devicePoints);
        for (const point of devicePoints) {{
          const latLng = [point.lat, point.lon];
          latLngs.push(latLng);
          deviceLatLngs.push(latLng);
          const isLatest = point === latestForDevice;
          L.circleMarker(latLng, {{
            radius: isLatest ? 7 : 5,
            color,
            fillColor: color,
            fillOpacity: isLatest ? 0.9 : 0.72,
            weight: isLatest ? 2 : 1
          }}).bindPopup(popup(point)).addTo(pointLayer);
        }}
        if (deviceLatLngs.length > 1) {{
          lines.push(L.polyline(deviceLatLngs, {{ color, weight: 2, opacity: 0.58 }}).addTo(map));
        }}
      }}

      if (latLngs.length && !fitted) {{
        map.fitBounds(latLngs, {{ padding: [28, 28], maxZoom: 17 }});
        fitted = true;
      }}

      const latest = latestPoint(points);
      const latestMs = latest ? pointTimeMs(latest) : 0;
      const ageMs = latestMs ? Date.now() - latestMs : NaN;
      const ageClass = Number.isFinite(ageMs) && ageMs <= 30 * 60 * 1000 ? 'fresh' : 'stale';
      const eventByDevice = new Map(events.map(event => [deviceId(event), event]));
      const pointGroups = groupedByDevice(points);
      const allDeviceIds = new Set([...pointGroups.keys(), ...eventByDevice.keys()]);
      const latestEvent = latestPoint(events);
      const latestEventLine = latestEvent
        ? `<div>latest uplink: ${{escapeHtml(deviceLabel(latestEvent))}} event ${{escapeHtml(latestEvent.event || 'n/a')}} port ${{latestEvent.f_port ?? 'n/a'}} fcnt ${{latestEvent.f_cnt ?? 'n/a'}} (${{ageText(Date.now() - pointTimeMs(latestEvent))}} ago)</div>`
        : '';
      const deviceSummaries = [...allDeviceIds].sort().map(devEui => {{
        const devicePoints = pointGroups.get(devEui) || [];
        const latestDevicePoint = latestPoint(devicePoints);
        const latestDeviceEvent = eventByDevice.get(devEui);
        const reference = latestDevicePoint || latestDeviceEvent || {{ dev_eui: devEui }};
        const color = colorFor(devEui);
        const pointText = latestDevicePoint
          ? `${{devicePoints.length}} pts, pos ${{ageText(Date.now() - pointTimeMs(latestDevicePoint))}}`
          : '0 pts';
        const eventText = latestDeviceEvent
          ? `event ${{escapeHtml(latestDeviceEvent.event || 'n/a')}} p${{latestDeviceEvent.f_port ?? 'n/a'}} f${{latestDeviceEvent.f_cnt ?? 'n/a'}}, ${{ageText(Date.now() - pointTimeMs(latestDeviceEvent))}}`
          : 'no event';
        return `<div class="device-row"><span><span class="dot" style="background:${{color}}"></span>${{escapeHtml(deviceLabel(reference))}}</span><span>${{pointText}}; ${{eventText}}</span></div>`;
      }}).join('');

      document.getElementById('summary').innerHTML = latest
        ? `<div>visible: ${{visible.length}} / retained: ${{points.length}}</div>
           <div class="${{ageClass}}">latest: ${{escapeHtml(deviceLabel(latest))}} ${{formatTime(latest)}} (${{ageText(ageMs)}} ago)</div>
           ${{latestEventLine}}
           <div>latest position: ${{latest.lat}}, ${{latest.lon}} hdop ${{latest.hdop}}</div>
           ${{visible.length ? '' : '<div class="stale">No positions in the selected view.</div>'}}
           ${{deviceSummaries}}`
        : `${{latestEventLine || 'Waiting for LoRaWAN positions...'}}${{deviceSummaries}}`;
    }}

    refresh().catch(() => {{}});
    setInterval(() => refresh().catch(() => {{}}), 3000);
  </script>
</body>
</html>
""",
    )


def connect(server: str, email: str, password: str) -> tuple[Any, list[tuple[str, str]]]:
    channel = grpc.insecure_channel(server)
    internal = api.InternalServiceStub(channel)
    login = internal.Login(api.LoginRequest(email=email, password=password), timeout=10)
    return internal, [("authorization", "Bearer " + login.jwt)]


def split_arg_values(values: list[str], env_value: str | None = None) -> list[str]:
    raw_values = list(values)
    if env_value:
        raw_values.append(env_value)

    items = []
    for value in raw_values:
        for item in value.split(","):
            item = item.strip()
            if item:
                items.append(item)
    return items


def normalize_dev_eui(value: str) -> str:
    return value.lower().replace(":", "").replace("-", "").strip()


def parse_device_labels(values: list[str], env_value: str | None = None) -> dict[str, str]:
    labels = {}
    for item in split_arg_values(values, env_value):
        if "=" not in item:
            continue
        dev_eui, label = item.split("=", 1)
        dev_eui = normalize_dev_eui(dev_eui)
        label = label.strip()
        if dev_eui and label:
            labels[dev_eui] = label
    return labels


def apply_device_labels(points: list[dict[str, Any]], labels: dict[str, str]) -> None:
    for point in points:
        dev_eui = normalize_dev_eui(str(point.get("dev_eui") or ""))
        if dev_eui in labels:
            point["device_label"] = labels[dev_eui]


def stream_events(
    args: argparse.Namespace,
    state: LiveState,
    stop_event: threading.Event,
    dev_eui: str,
    device_label: str,
) -> None:
    while not stop_event.is_set():
        try:
            internal, metadata = connect(args.server, args.email, args.password)
            req = api.StreamDeviceEventsRequest(dev_eui=dev_eui)
            print(f"[{utc_now()}] connected to ChirpStack stream for {device_label} ({dev_eui})", flush=True)
            for event in internal.StreamDeviceEvents(req, metadata=metadata):
                outer = MessageToDict(event, preserving_proto_field_name=True)
                body = json.loads(outer.get("body", "{}"))
                description = outer.get("description", "")
                if description in {"join", "up"}:
                    state.record_event(dev_eui, device_label, description, body)

                point = parse_uplink_point(body, dev_eui, device_label)
                if point is not None:
                    if state.add_point(point):
                        print(
                            "[{now}] {label} position fcnt={fcnt} lat={lat:.9f} lon={lon:.9f} hdop={hdop:.3f}".format(
                                now=utc_now(),
                                label=device_label,
                                fcnt=point.get("f_cnt"),
                                lat=point["lat"],
                                lon=point["lon"],
                                hdop=point["hdop"],
                            ),
                            flush=True,
                        )
                elif description in {"join", "up"}:
                    print(
                        f"[{utc_now()}] {device_label} event={description} port={body_get(body, 'f_port', 'fPort')} "
                        f"fcnt={body_get(body, 'f_cnt', 'fCnt')}",
                        flush=True,
                    )
        except grpc.RpcError as exc:
            print(f"[{utc_now()}] stream ended: {exc.code().name} {exc.details()}; reconnecting", flush=True)
        except Exception as exc:
            print(f"[{utc_now()}] stream error: {exc!r}; reconnecting", flush=True)
        stop_event.wait(args.reconnect_delay_s)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", default=os.environ.get("CHIRPSTACK_SERVER"), help="ChirpStack API host:port")
    parser.add_argument("--email", default=os.environ.get("CHIRPSTACK_EMAIL"), help="ChirpStack login email")
    parser.add_argument("--password", default=os.environ.get("CHIRPSTACK_PASSWORD"), help="ChirpStack login password")
    parser.add_argument(
        "--dev-eui",
        dest="dev_euis",
        action="append",
        default=[],
        help="Device EUI to stream; repeat the option or pass comma-separated values",
    )
    parser.add_argument(
        "--device-label",
        dest="device_labels",
        action="append",
        default=[],
        help="Optional display label as <dev_eui>=<label>; repeat or comma-separate",
    )
    parser.add_argument("--out-dir", type=Path, default=Path("lora-live-map"), help="Output directory")
    parser.add_argument("--host", default="127.0.0.1", help="HTTP bind host")
    parser.add_argument("--port", type=int, default=18081, help="HTTP bind port")
    parser.add_argument("--max-points", type=int, default=10000, help="Maximum points kept in live outputs")
    parser.add_argument("--reconnect-delay-s", type=float, default=5.0, help="Delay before reconnecting stream")
    parser.add_argument("--no-server", action="store_true", help="Write outputs without serving HTTP")
    args = parser.parse_args()

    env_dev_euis = os.environ.get("TRACKER_DEVEUIS") or os.environ.get("TRACKER_DEVEUI") or os.environ.get("TRACKER108_DEVEUI")
    dev_euis = split_arg_values(args.dev_euis, env_dev_euis)
    args.dev_euis = []
    seen_dev_euis = set()
    for dev_eui in dev_euis:
        normalized = normalize_dev_eui(dev_eui)
        if normalized and normalized not in seen_dev_euis:
            args.dev_euis.append(normalized)
            seen_dev_euis.add(normalized)

    args.device_labels = parse_device_labels(args.device_labels, os.environ.get("TRACKER_DEVICE_LABELS"))

    missing = [name for name in ("server", "email", "password") if not getattr(args, name)]
    if not args.dev_euis:
        missing.append("dev_eui")
    if missing:
        parser.error("missing required configuration: " + ", ".join(missing))

    return args


def main() -> int:
    args = parse_args()
    args.out_dir.mkdir(parents=True, exist_ok=True)
    map_title = "Tracker LoRaWAN Live Map"
    if len(args.dev_euis) == 1:
        map_title = f"{map_title} {args.device_labels.get(args.dev_euis[0], args.dev_euis[0])}"
    write_index(args.out_dir, map_title)

    stop_event = threading.Event()
    signal.signal(signal.SIGTERM, lambda *_: stop_event.set())
    signal.signal(signal.SIGINT, lambda *_: stop_event.set())

    existing_points = load_existing_points(args.out_dir / "points.json", args.max_points)
    apply_device_labels(existing_points, args.device_labels)
    if existing_points:
        print(f"[{utc_now()}] loaded {len(existing_points)} existing points", flush=True)

    known_devices = {dev_eui: args.device_labels.get(dev_eui, dev_eui) for dev_eui in args.dev_euis}
    state = LiveState(args.out_dir, args.max_points, existing_points, known_devices)
    state.flush()
    workers = []
    for dev_eui in args.dev_euis:
        device_label = args.device_labels.get(dev_eui, dev_eui)
        worker = threading.Thread(
            target=stream_events,
            args=(args, state, stop_event, dev_eui, device_label),
            daemon=True,
        )
        worker.start()
        workers.append(worker)

    if args.no_server:
        while not stop_event.is_set():
            time.sleep(0.5)
        return 0

    handler = functools.partial(SimpleHTTPRequestHandler, directory=str(args.out_dir))
    server = ThreadingHTTPServer((args.host, args.port), handler)
    server.timeout = 0.5
    print(f"[{utc_now()}] serving http://{args.host}:{args.port}/ from {args.out_dir}", flush=True)
    try:
        while not stop_event.is_set():
            server.handle_request()
    finally:
        server.server_close()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
