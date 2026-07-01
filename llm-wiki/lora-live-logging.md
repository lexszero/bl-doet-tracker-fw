# LoRa Live Logging

Last updated: 2026-07-01.

The repository now includes a generic ChirpStack live position logger:

```shell
scripts/dev/chirpstack_live_map.py
```

It listens to ChirpStack application events for one or more devices, decodes the current tracker port `4` payload, writes CSV/JSON/GeoJSON outputs, and serves a small Leaflet/OpenStreetMap live map.

When ChirpStack includes RF metadata in the application event, the script also preserves gateway RSSI/SNR, frequency, spreading factor, bandwidth, and code rate. Firmware TX power is not currently visible through this receiver-side stream.

The source-controlled script intentionally has no private defaults. Provide these through environment variables or a private wrapper:

- `CHIRPSTACK_SERVER`
- `CHIRPSTACK_EMAIL`
- `CHIRPSTACK_PASSWORD`
- `TRACKER_DEVEUI` for one device, or `TRACKER_DEVEUIS` as a comma-separated multi-device list
- `TRACKER_DEVICE_LABELS` optionally as comma-separated `<dev_eui>=<label>` entries

Example shape:

```shell
python scripts/dev/chirpstack_live_map.py \
  --server "$CHIRPSTACK_SERVER" \
  --email "$CHIRPSTACK_EMAIL" \
  --password "$CHIRPSTACK_PASSWORD" \
  --dev-eui "$TRACKER_DEVEUI" \
  --out-dir lora-live-map \
  --host 127.0.0.1 \
  --port 18081
```

For multiple devices, repeat `--dev-eui` or pass comma-separated values:

```shell
python scripts/dev/chirpstack_live_map.py \
  --server "$CHIRPSTACK_SERVER" \
  --email "$CHIRPSTACK_EMAIL" \
  --password "$CHIRPSTACK_PASSWORD" \
  --dev-eui "$TRACKER108_DEVEUI" \
  --dev-eui "$TRACKER109_DEVEUI" \
  --device-label "$TRACKER108_DEVEUI=Tracker_108" \
  --device-label "$TRACKER109_DEVEUI=Tracker_109" \
  --out-dir lora-live-map \
  --host 127.0.0.1 \
  --port 18081
```

The `18081` port above is only an example local map-server port. If someone serves a copied output directory with a static file server, the page can look identical but will not be live; check `latest.json` timestamps before using it as current tracker state.

Outputs:

- `index.html`: live polling map;
- `points.json`: all retained decoded points;
- `latest.json`: last decoded point;
- `latest_by_device.json`: last decoded point and retained count per device;
- `device_events.json`: last seen ChirpStack join/uplink event per streamed device, even when the payload is not a decoded position;
- `positions.csv`: spreadsheet-friendly log;
- `positions.geojson`: GIS/map-friendly export.
- `neighbourhoods.geojson`: optional static overlay file served beside the map when available.

By default the logger keeps the latest `10000` points in those live outputs. Use `--max-points` to change that. On restart, the logger reloads an existing `points.json` file from the output directory so normal restarts do not wipe the visible map history. Duplicate/replayed event records are ignored based on network time, DevAddr, frame counter, port, and raw payload.

The browser map defaults to a `Last 24h` view and shows the latest decoded position age plus latest uplink-event age. Disable the checkbox to inspect the full retained history. The map colors each device separately and includes a device filter when retained points or event status exist for more than one DevEUI.

If `neighbourhoods.geojson` is present in the live-map output directory, the browser map offers a neighbourhood overlay. The default view anchors the `Power Hill` neighbourhood around the gateway reference coordinate `60.220101984, 24.836646496` while preserving the overlay's approximate meter scale, which is useful for comparing tracker movement against the neighbourhood-size footprint. Disable `Power Hill at gateway` to view the GeoJSON in its original coordinates.

The current firmware port `4` LoRaWAN payload only contains latitude, longitude, and HDOP. Speed is available in the on-device diagnostic flash log, not in the LoRaWAN receiver stream, unless the payload format is changed later.

## Operational Notes

USB serial is not required for this path. It works from the LoRaWAN receiver side as long as the tracker is provisioned, joined, and sending application uplinks.

Bench-specific wrapper names, backend endpoints, credentials, device identities, SSH tunnel/proxy commands, and output paths belong in ignored development-group notes under `llm-wiki/private/dev-group/` or local-only notes under `llm-wiki/private/personal-agent/`.
