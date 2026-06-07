# Config variables — device-side implementation (v0.4.0)

Implements the device half of the published
[Config Variables (v0.4.0)](https://docs.scadable.com/docs/config-variables)
design: retained-topic delivery, NVS caching, typed getters, change
callback, heartbeat version echo, and the `sys.heartbeat_interval_ms`
reserved key. This page covers implementation specifics that the public
docs page doesn't; read that page first.

## Conformance to the public spec

| Spec item | Implementation |
|---|---|
| `scadable_config_int/bool/float/str` | ✅ `include/scadable.h`, exact signatures |
| `scadable_config_on_change(cb, ctx)` | ✅ single slot, fired on the MQTT task after apply |
| Topic `scadable/devices/<cn>/cmd/config`, retained, QoS 1 | ✅ subscribed on connect; replayed on reconnect |
| Payload `{"version": N, "config": {...}}` | ✅ full-replace semantics |
| NVS cache, readable before network | ✅ loaded in `app_main` before `scadable_user_main` |
| Heartbeat `config_version` echo | ✅ added to the always-on heartbeat fields |
| `sys.heartbeat_interval_ms` | ✅ heartbeat task re-reads each tick (clamped ≥ 1 s) |
| `sys.log_flush_interval_ms` | ✅ log_sink flush task re-reads each cycle (clamped ≥ 5 s) |
| Schema validation / codegen | n/a — build-pipeline concern; the library is schema-agnostic |

## Implementation notes

* **Storage:** the received payload is kept verbatim — one heap string
  in RAM, one NVS blob (+ version u32) on flash, committed together.
  NVS keys cap at 15 chars so per-key entries can't hold real names;
  the blob also makes the apply effectively atomic. Getters scan the
  cached JSON on demand (hand-rolled, no cJSON) — no RAM proportional
  to key count.
* **Versioning:** a re-delivered equal version is ignored (normal for
  a retained topic on reconnect). Any *different* version is applied —
  including a lower one, so a backend that resets its counter doesn't
  strand the fleet.
* **Type safety:** getters check the JSON token shape — a quoted value
  returned through `scadable_config_int` falls back, a bare token
  through `scadable_config_str` returns 0. Wrong type can't produce a
  half-right value.
* **Failure:** malformed/oversized payloads are rejected and the last
  good map kept. If the NVS write fails, the map is still applied to
  RAM (live behavior correct; persistence degraded) and logged; the
  heartbeat echoes what's actually applied either way.
* **NVS namespace** `scd_config` (Kconfig-overridable) — separate from
  the credential namespace, so a config wipe can never touch identity.

## Backend implementers' checklist

* Publish the **full resolved map** on every change — retained, QoS 1,
  to `scadable/devices/<cn>/cmd/config`. Replace, never patch.
* Removing the last key: publish `{"version":N,"config":{}}` (still
  retained) so devices converge to empty rather than keeping stale
  values.
* **Size:** the map must fit `CONFIG_SCD_CONFIG_MAX_JSON` (default
  1536 bytes) and a single MQTT read (client inbound buffer 2048).
* **Sync state:** compare your latest version against the device
  heartbeat's `config_version`: equal → synced; less → pending;
  `0` → never applied.
* **Broker ACL:** devices need subscribe on their own
  `scadable/devices/<cn>/cmd/config` (per-CN, same shape as the
  existing `ota/command` rule).

## Kconfig

| Option | Default | Purpose |
|---|---|---|
| `SCD_CONFIG_ENABLE` | `y` | Part of the standard surface; `n` removes it (~2 KB) |
| `SCD_CONFIG_MAX_JSON` | `1536` | Max accepted payload size |
| `SCD_CONFIG_NVS_NAMESPACE` | `"scd_config"` | Cache location |

## Log tag

`scd.config` — boot cache load, applied versions, rejections.
