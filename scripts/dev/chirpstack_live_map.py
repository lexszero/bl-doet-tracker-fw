#!/usr/bin/env python3
"""Live ChirpStack position logger for the tracker firmware.

This script listens to ChirpStack application events for one device, decodes
the current tracker port-4 position payload, writes CSV/GeoJSON/JSON outputs,
and serves a small live map.

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


class LiveState:
    def __init__(self, out_dir: Path, max_points: int) -> None:
        self.out_dir = out_dir
        self.max_points = max_points
        self.lock = threading.Lock()
        self.points: list[dict[str, Any]] = []

    def add_point(self, point: dict[str, Any]) -> None:
        with self.lock:
            self.points.append(point)
            if len(self.points) > self.max_points:
                self.points = self.points[-self.max_points :]
            write_outputs(self.out_dir, self.points)


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z")


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


def parse_uplink_point(body: dict[str, Any], dev_eui: str) -> dict[str, Any] | None:
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
    return {
        "received_at": utc_now(),
        "network_time": (body.get("time") or "").replace("+00:00", "Z"),
        "dev_eui": dev_eui,
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
    }


def atomic_write(path: Path, content: str) -> None:
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(content, encoding="utf-8")
    tmp.replace(path)


def write_outputs(out_dir: Path, points: list[dict[str, Any]]) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)

    atomic_write(out_dir / "points.json", json.dumps(points, indent=2))
    latest = points[-1] if points else {}
    atomic_write(out_dir / "latest.json", json.dumps(latest, indent=2))

    fieldnames = [
        "received_at",
        "network_time",
        "dev_eui",
        "dev_addr",
        "f_cnt",
        "f_port",
        "lat",
        "lon",
        "hdop",
        "rssi",
        "snr",
        "gateway_id",
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
      width: min(380px, calc(100vw - 24px));
      background: rgba(255, 255, 255, 0.95);
      border: 1px solid #c9ced6;
      border-radius: 6px;
      padding: 10px 12px;
      box-shadow: 0 6px 22px rgba(31, 41, 55, 0.16);
      font-size: 13px;
      line-height: 1.35;
    }}
    .panel strong {{ display: block; font-size: 14px; margin-bottom: 4px; }}
    .muted {{ color: #475569; }}
  </style>
</head>
<body>
  <div id="map"></div>
  <div class="panel">
    <strong>{escaped_title}</strong>
    <div id="summary" class="muted">Waiting for LoRaWAN positions...</div>
  </div>
  <script src="https://unpkg.com/leaflet@1.9.4/dist/leaflet.js"></script>
  <script>
    const map = L.map('map').setView([0, 0], 2);
    L.tileLayer('https://tile.openstreetmap.org/{{z}}/{{x}}/{{y}}.png', {{
      maxZoom: 19,
      attribution: '&copy; OpenStreetMap contributors'
    }}).addTo(map);

    const layer = L.layerGroup().addTo(map);
    let line = null;
    let fitted = false;

    function popup(point) {{
      return `
        <strong>fcnt ${{point.f_cnt ?? 'n/a'}}</strong><br>
        network: ${{point.network_time || 'n/a'}}<br>
        received: ${{point.received_at}}<br>
        lat: ${{point.lat}}<br>
        lon: ${{point.lon}}<br>
        hdop: ${{point.hdop}}<br>
        rssi: ${{point.rssi ?? 'n/a'}} snr: ${{point.snr ?? 'n/a'}}
      `;
    }}

    async function refresh() {{
      const response = await fetch('points.json?ts=' + Date.now());
      const points = await response.json();
      layer.clearLayers();
      if (line) {{
        map.removeLayer(line);
        line = null;
      }}

      const latLngs = [];
      for (const point of points) {{
        latLngs.push([point.lat, point.lon]);
        L.circleMarker([point.lat, point.lon], {{
          radius: 5,
          color: '#2563eb',
          fillColor: '#2563eb',
          fillOpacity: 0.78,
          weight: 1
        }}).bindPopup(popup(point)).addTo(layer);
      }}

      if (latLngs.length > 1) {{
        line = L.polyline(latLngs, {{ color: '#111827', weight: 2, opacity: 0.55 }}).addTo(map);
      }}
      if (latLngs.length && !fitted) {{
        map.fitBounds(latLngs, {{ padding: [28, 28], maxZoom: 17 }});
        fitted = true;
      }}

      const latest = points[points.length - 1];
      document.getElementById('summary').innerHTML = latest
        ? `${{points.length}} positions<br>latest: ${{latest.network_time || latest.received_at}}<br>${{latest.lat}}, ${{latest.lon}}<br>hdop: ${{latest.hdop}}`
        : 'Waiting for LoRaWAN positions...';
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


def stream_events(args: argparse.Namespace, state: LiveState, stop_event: threading.Event) -> None:
    while not stop_event.is_set():
        try:
            internal, metadata = connect(args.server, args.email, args.password)
            req = api.StreamDeviceEventsRequest(dev_eui=args.dev_eui)
            print(f"[{utc_now()}] connected to ChirpStack stream for {args.dev_eui}", flush=True)
            for event in internal.StreamDeviceEvents(req, metadata=metadata):
                outer = MessageToDict(event, preserving_proto_field_name=True)
                body = json.loads(outer.get("body", "{}"))
                description = outer.get("description", "")
                point = parse_uplink_point(body, args.dev_eui)
                if point is not None:
                    state.add_point(point)
                    print(
                        "[{now}] position fcnt={fcnt} lat={lat:.9f} lon={lon:.9f} hdop={hdop:.3f}".format(
                            now=utc_now(),
                            fcnt=point.get("f_cnt"),
                            lat=point["lat"],
                            lon=point["lon"],
                            hdop=point["hdop"],
                        ),
                        flush=True,
                    )
                elif description in {"join", "up"}:
                    print(
                        f"[{utc_now()}] event={description} port={body_get(body, 'f_port', 'fPort')} "
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
    parser.add_argument("--dev-eui", default=os.environ.get("TRACKER_DEVEUI") or os.environ.get("TRACKER108_DEVEUI"), help="Device EUI to stream")
    parser.add_argument("--out-dir", type=Path, default=Path("lora-live-map"), help="Output directory")
    parser.add_argument("--host", default="127.0.0.1", help="HTTP bind host")
    parser.add_argument("--port", type=int, default=18081, help="HTTP bind port")
    parser.add_argument("--max-points", type=int, default=1000, help="Maximum points kept in live outputs")
    parser.add_argument("--reconnect-delay-s", type=float, default=5.0, help="Delay before reconnecting stream")
    parser.add_argument("--no-server", action="store_true", help="Write outputs without serving HTTP")
    args = parser.parse_args()

    missing = [name for name in ("server", "email", "password", "dev_eui") if not getattr(args, name)]
    if missing:
        parser.error("missing required configuration: " + ", ".join(missing))

    args.dev_eui = args.dev_eui.lower()
    return args


def main() -> int:
    args = parse_args()
    args.out_dir.mkdir(parents=True, exist_ok=True)
    write_outputs(args.out_dir, [])
    write_index(args.out_dir, f"Tracker LoRaWAN Live Map {args.dev_eui}")

    stop_event = threading.Event()
    signal.signal(signal.SIGTERM, lambda *_: stop_event.set())
    signal.signal(signal.SIGINT, lambda *_: stop_event.set())

    state = LiveState(args.out_dir, args.max_points)
    worker = threading.Thread(target=stream_events, args=(args, state, stop_event), daemon=True)
    worker.start()

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
