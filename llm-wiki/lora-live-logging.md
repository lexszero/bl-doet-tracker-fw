# LoRa Live Logging

Last updated: 2026-06-27.

The repository now includes a generic ChirpStack live position logger:

```shell
scripts/dev/chirpstack_live_map.py
```

It listens to ChirpStack application events for one device, decodes the current tracker port `4` payload, writes CSV/JSON/GeoJSON outputs, and serves a small Leaflet/OpenStreetMap live map.

The source-controlled script intentionally has no private defaults. Provide these through environment variables or a private wrapper:

- `CHIRPSTACK_SERVER`
- `CHIRPSTACK_EMAIL`
- `CHIRPSTACK_PASSWORD`
- `TRACKER_DEVEUI`

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

Outputs:

- `index.html`: live polling map;
- `points.json`: all retained decoded points;
- `latest.json`: last decoded point;
- `positions.csv`: spreadsheet-friendly log;
- `positions.geojson`: GIS/map-friendly export.

The current firmware port `4` LoRaWAN payload only contains latitude, longitude, and HDOP. Speed is available in the on-device diagnostic flash log, not in the LoRaWAN receiver stream, unless the payload format is changed later.

## Operational Notes

USB serial is not required for this path. It works from the LoRaWAN receiver side as long as the tracker is provisioned, joined, and sending application uplinks.

Bench-specific wrapper names, backend endpoints, credentials, device identities, and output paths belong in ignored development-group notes under `llm-wiki/private/dev-group/`.
