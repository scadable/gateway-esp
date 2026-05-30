# AGENTS.md

Compact, structured reference for AI agents (and humans) integrating
`scadable/libscadable` into an ESP-IDF project. Read this first; it
beats grep-ing the source.

## What this library is

A drop-in ESP-IDF component that handles the SCADABLE platform
integration: cert load, broker discovery, TLS MQTT, heartbeat, OTA.
Customer brings their own network bring-up (Wi-Fi / Ethernet /
cellular) and their domain logic. Library handles everything between
"IP is up" and "device is on dashboard, ready for OTA."

Registry: `scadable/libscadable` on `components.espressif.com`.
Repo: `https://github.com/scadable/gateway-esp`.

## Integration in three rules

**Rule 1.** Add the dep:
```yaml
# customer's main/idf_component.yml
dependencies:
  scadable/libscadable: "^0.1.0"
```

**Rule 2.** Define `scadable_user_main`, NOT `app_main`. The library
owns `app_main`.
```c
#include "scadable.h"
void scadable_user_main(void) {
    bring_up_wifi_or_eth_or_cellular();   // YOUR code
    your_domain_logic();
}
```

**Rule 3.** Provision the device via the SCADABLE dashboard before
flashing. The provisioner writes a credential bundle into the NVS
partition. Without it, the library logs an error and idles.

## Public API (entire surface for v0.1.0)

```c
void scadable_user_main(void);                 // YOU implement this
bool scadable_mqtt_connected(void);            // for gating publishes
esp_err_t scadable_mqtt_publish(               // raw publish primitive
    const char *topic, const void *payload, int len,
    scd_qos_t qos, bool retain);

typedef enum { SCD_QOS_0, SCD_QOS_1, SCD_QOS_2 } scd_qos_t;
```

That's it. No init function, no start function, no struct config.
The library boots itself.

## What the library does at boot (timeline)

1. ESP-IDF startup → library's weak `app_main()`.
2. `nvs_flash_init()`.
3. Read 7 string keys from NVS namespace `scadable_cfg`:
   `device_id`, `common_name`, `cert_pem`, `key_pem`, `ca_pem`,
   `mqtt_host`, `mqtt_port`. Abort if any are missing.
4. Register an `IP_EVENT_ANY_ID` handler. The handler is dormant
   until something brings up an IP.
5. Call your `scadable_user_main()`. You bring up the network.
6. IP event fires → library spawns `bootstrap_online_task`:
   - HTTPS GET `https://edge.scadable.com/v1/route` with
     `X-Device-CN: <common_name>` header. Parse `mqtt_host` +
     `mqtt_port`. Fall back to NVS values on failure.
   - Open `mqtts://<host>:<port>` MQTT session. Validate broker
     cert against `ca_pem`. Client ID = `common_name`.
   - On `MQTT_EVENT_CONNECTED`: subscribe to
     `scadable/<cn>/ota/command`.
   - Spawn heartbeat task (publishes every
     `CONFIG_SCD_HEARTBEAT_INTERVAL_S` to
     `scadable/<cn>/heartbeat`).
   - Mark running OTA image as valid
     (`esp_ota_mark_app_valid_cancel_rollback`).
   - Register OTA command handler that downloads + installs via
     `esp_https_ota`.

## Partition table requirements

```
nvs       data, nvs,   0x9000,  0x4000     # required (≥ 24 KB)
otadata   data, ota,   0xd000,  0x2000     # required for OTA
ota_0     app,  ota_0, 0x10000, 0x180000   # required for OTA
ota_1     app,  ota_1, 0x190000, 0x180000  # required for OTA
```

ESP-IDF's `partitions_two_ota.csv` template works as-is.

## Topic conventions (v0.1.0)

Library-internal; customer doesn't see these directly. CN-in-topic;
identity-free topics arrive in v0.2.0.

| Topic | Direction | QoS |
|---|---|---|
| `scadable/<cn>/heartbeat` | device → backend | 0 |
| `scadable/<cn>/ota/command` | backend → device | (broker) |
| `scadable/<cn>/ota/status` | device → backend | 1 |

`<cn>` = certificate Common Name, format `SC-<device_id>`.

## Heartbeat payload shape

```json
{"uptime_s": 1234, "free_heap": 187392, "boot": 1, "fw": "0.1.0"}
```

## OTA command payload (incoming, backend → device)

```json
{"version": "0.2.3",
 "url": "https://app.scadable.com/api/firmware/<sha>.bin"}
```

## OTA status payload (outgoing, device → backend)

```json
{"version": "0.2.3",
 "state":   "downloading|progress|success|failed",
 "details": "42% (256000/608000)"}
```

## Configuration (Kconfig — `idf.py menuconfig`)

`Component config → SCADABLE Gateway`:

```
CONFIG_SCD_EDGE_URL                 string  "https://edge.scadable.com/v1/route"
CONFIG_SCD_EDGE_TIMEOUT_MS          int     10000
CONFIG_SCD_EDGE_MAX_RETRIES         int     2
CONFIG_SCD_HEARTBEAT_INTERVAL_S     int     30
CONFIG_SCD_MQTT_RECONNECT_TIMEOUT_MS int    5000
CONFIG_SCD_NVS_NAMESPACE            string  "scadable_cfg"
CONFIG_SCD_DEBUG_VERBOSE            bool    n
```

Defaults are production-correct. Override only for dev / staging.

## File map (one concern per file)

```
src/scadable_main.c   — weak app_main, bootstrap orchestration
src/identity.c        — NVS read
src/edge.c            — HTTPS GET /v1/route
src/mqtt_session.c    — MQTT lifecycle + publish primitive
src/heartbeat.c       — periodic publish task
src/ota_machine.c     — OTA listener + esp_https_ota executor
include/scadable.h    — public API
private_include/scadable_internal.h — cross-file internal types
```

## Dependencies (in `idf_component.yml`)

```yaml
dependencies:
  idf: ">=5.1"
```

The library uses standard ESP-IDF components (`nvs_flash`, `mqtt`,
`esp_http_client`, `esp_https_ota`, `app_update`, `esp_netif`,
`esp_event`, `esp_timer`, `mbedtls`, `log`). No third-party deps.

## What NOT to do

- **Don't define `app_main`** unless you absolutely must. Define
  `scadable_user_main` instead.
- **Don't try to bring up Wi-Fi inside the library.** That's the
  customer's job. The library is wire-agnostic.
- **Don't reuse the cert CN across devices.** Each device gets a
  unique provisioning bundle. Two devices with the same CN will
  fight on the broker.
- **Don't expect log forwarding in v0.1.0.** Logs only appear in
  serial/`idf.py monitor` for now. v0.2.0 adds MQTT log forwarding.
- **Don't expect custom event publishing helpers in v0.1.0.** Use
  `scadable_mqtt_publish` directly with raw topics if you must.

## Out of scope for v0.1.0

- Customer event publishing helpers (`scadable_publish_event`)
- Inbound command handlers beyond OTA (`SCADABLE_ON_COMMAND`)
- Schema-driven typed event codegen
- Log forwarding to broker
- mTLS at broker (one-way TLS only; the library carries the cert
  for future use)
- Provisioning over BLE / SoftAP (customer flashes via dashboard)
- Hardware port abstraction (ESP-IDF only)
- Region failover beyond `edge → fallback NVS`

All of these are coherent v0.2.0+ work.

## How to verify the device is online

1. `idf.py monitor` and watch for:
   ```
   I (...) scadable: online bootstrap complete
   I (...) scd.mqtt: connected
   ```
2. Within `CONFIG_SCD_HEARTBEAT_INTERVAL_S` seconds, the device
   appears as "online" on the dashboard.

If both happen, the integration is working.

## Common log tags

| Tag | What it logs about |
|---|---|
| `scadable` | Top-level bootstrap, app_main, user_main wiring |
| `scd.identity` | NVS read of credentials |
| `scd.edge` | HTTPS edge call + JSON parsing |
| `scd.mqtt` | MQTT lifecycle + subscriptions |
| `scd.heartbeat` | Heartbeat publish attempts |
| `scd.ota` | OTA command receive + execute |

Grep these to find what the library is doing at any moment.
