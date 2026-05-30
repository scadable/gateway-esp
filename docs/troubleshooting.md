# Troubleshooting

The library logs at INFO by default. Set `CONFIG_SCD_DEBUG_VERBOSE`
in menuconfig for DEBUG-level output. Look for log tags starting
with `scd.` — `scd.identity`, `scd.edge`, `scd.mqtt`, `scd.heartbeat`,
`scd.ota`.

## Symptom: "identity load failed" at boot

```
E (...) scd.identity: nvs_open("scadable_cfg") failed: ...
E (...) scadable: identity load failed — device is unprovisioned ...
```

**Cause**: the NVS partition doesn't contain a valid SCADABLE
credential bundle.

**Fix**:

1. Provision the device via the SCADABLE dashboard (creates the
   bundle).
2. Use the browser flasher to write the bundle to the device. This
   writes the NVS partition at offset 0x9000.
3. Reset.

If you've already flashed once but lost the NVS partition (e.g. you
ran `esptool erase_flash`), you need to re-flash the NVS bundle —
the firmware alone isn't enough.

---

## Symptom: device boots fine but never goes online

```
I (...) scadable: libscadable v0.1.0 starting
I (...) scd.identity: identity loaded: ...
W (...) scadable: scadable_user_main not defined — running headless ...
```

**Cause**: you haven't defined `scadable_user_main` (or you did but
your function doesn't bring up the network).

**Fix**: define `scadable_user_main` and call your Wi-Fi / Ethernet
setup from it. See [integration.md](./integration.md#2-define-scadable_user_main).

---

## Symptom: edge route returns 200 but mqtt_host is empty

```
W (...) scd.edge: no mqtt_host in response: ...
```

**Cause**: the response body wasn't the expected JSON shape, OR
the body was > 512 bytes and got truncated.

**Fix**:

- Confirm `CONFIG_SCD_EDGE_URL` points to a real SCADABLE edge
  endpoint (default: `https://edge.scadable.com/v1/route`).
- If you're running a local backend in dev, make sure it implements
  the same response shape as `backend/edge.go`:

```json
{ "mqtt_host": "...", "mqtt_port": 8883, "region": "yyz", "version": 1 }
```

---

## Symptom: edge route fails entirely, library falls back to NVS

```
W (...) scd.edge: perform: ESP_ERR_HTTP_CONNECT
W (...) scadable: edge route failed; falling back to NVS default ...
```

**Cause**: HTTPS GET couldn't reach the edge — DNS, TLS, or network
issue.

**Fix**:

- Check DNS: can the device resolve `edge.scadable.com`?
- Check TLS: does the device have the system root cert bundle?
  ESP-IDF includes it by default; if you've stripped certs to save
  flash, you'll need to add Cloudflare's CA back (the edge is
  Cloudflare-proxied).
- Check the network: can the device reach the public internet at
  all? Try `ping 1.1.1.1` from another device on the same Wi-Fi.

The library still works in fallback mode (uses the NVS-baked
`mqtt_host`/`mqtt_port`), but you lose region-switching support.

---

## Symptom: MQTT connects then immediately disconnects

```
I (...) scd.mqtt: connected
W (...) scd.mqtt: disconnected
```

Common causes:

1. **Cert validation failure** — the CA chain in NVS doesn't match
   the broker's cert chain. Re-provision to refresh.
2. **Broker rejected client_id** — only happens if two devices try
   to connect with the same CN. Each device should have a unique
   provisioning bundle.
3. **Broker dropped due to keepalive** — possible if the device is
   on a flaky network. The library reconnects automatically; if it
   happens repeatedly, increase `CONFIG_SCD_MQTT_RECONNECT_TIMEOUT_MS`.

---

## Symptom: OTA command received but download fails

```
I (...) scd.ota: command received: 0.1.1
I (...) scd.ota: starting OTA version=0.1.1 url=...
E (...) scd.ota: ota_begin: ESP_FAIL
```

Common causes:

1. **Wrong partition table** — your `partitions.csv` needs `ota_0`,
   `ota_1`, and `otadata`. See [integration.md](./integration.md#3-configure-partitions).
2. **OTA URL not reachable** — same connectivity check as for the
   edge endpoint.
3. **Image too big** — your firmware exceeds the `ota_0` partition
   size. Either shrink the firmware or grow the partitions.

---

## Symptom: heartbeats stop after some time

```
I (...) scd.heartbeat: publish skipped: ESP_ERR_INVALID_STATE
```

That's the library detecting MQTT disconnected and skipping the
publish. Normal during reconnect. If it persists, look for the
underlying MQTT disconnect cause (above).

---

## Symptom: code compiles locally but fails on the SCADABLE build

The SCADABLE build pipeline uses a recent ESP-IDF. If you're
developing against an older version (5.0 or earlier), some headers
may differ. The library targets ESP-IDF ≥ 5.1 as declared in
`idf_component.yml`. Upgrade your local ESP-IDF to match.

---

## Getting more detail

Enable verbose logging in menuconfig:

```
Component config → SCADABLE Gateway → [*] Verbose logging
```

You can also bump ESP-IDF's own log levels:

```
Component config → Log → Default log verbosity → Debug
```

That'll flood the console — useful for one-off diagnosis, not for
production.

---

## Still stuck?

Open an issue at <https://github.com/scadable/gateway-esp/issues>
with:

- The full `idf.py monitor` output from boot.
- Your ESP-IDF version (`idf.py --version`).
- The chip target (`idf.py set-target` value).
- Your `partitions.csv`.
