# scadable/libscadable

The SCADABLE platform integration library for ESP32. Add it to your
firmware project and your device shows up on the SCADABLE dashboard
with heartbeats and is ready for OTA updates — without writing any
of the plumbing yourself.

## What it does

Once a provisioned device boots and has an IP address, the library:

1. Loads its certificate + identity from NVS.
2. Calls `https://edge.scadable.com/v1/route` to discover which MQTT
   broker to connect to.
3. Opens a TLS-secured MQTT session to that broker.
4. Publishes a periodic heartbeat (default every 30 s).
5. Listens for OTA commands from the dashboard and executes them
   end-to-end (download, install, restart, rollback-on-failure).

You write the network bring-up (Wi-Fi, Ethernet, cellular — whatever
fits your hardware) and your domain logic. The library handles
everything between IP-up and the dashboard.

## Quickstart

### 1. Add the dependency

In your project's `main/idf_component.yml`:

```yaml
dependencies:
  scadable/libscadable: "^0.1.0"
```

### 2. Write `scadable_user_main` instead of `app_main`

The library owns `app_main` (as a weak symbol) so it can do bootstrap
before your code runs. You define `scadable_user_main`:

```c
// main/main.c
#include "scadable.h"
#include "esp_wifi.h"

void scadable_user_main(void) {
    // Bring up the network however you like:
    my_wifi_connect();

    // ... your domain logic ...
}
```

If you don't define `scadable_user_main`, the library boots into
headless mode — it'll log a warning and idle. That's fine for
provisioning-only workflows but not for any real product.

### 3. Provision and flash

Create a namespace in the SCADABLE dashboard, provision a device
through it, and use the dashboard's browser-based flasher (or the
returned NVS bundle) to write the credentials + firmware to your
chip. On first boot, the device appears in the dashboard with
heartbeats.

### 4. Push an update

Bind a GitHub repo to your namespace, push new firmware, hit
"Deploy" in the dashboard. The library's OTA agent receives the
command over MQTT, downloads the new image, restarts, and marks
the new firmware valid once it comes up clean.

## Configuration

All options live under `Component config → SCADABLE Gateway` in
`idf.py menuconfig`. The defaults work for production:

| Option | Default | What it does |
|---|---|---|
| `CONFIG_SCD_EDGE_URL` | `https://edge.scadable.com/v1/route` | Bootstrap endpoint. Override for dev/staging. |
| `CONFIG_SCD_EDGE_TIMEOUT_MS` | `10000` | Per-attempt HTTP timeout. |
| `CONFIG_SCD_EDGE_MAX_RETRIES` | `2` | Retries before falling back to the NVS-baked broker. |
| `CONFIG_SCD_HEARTBEAT_INTERVAL_S` | `30` | Seconds between heartbeats. |
| `CONFIG_SCD_NVS_NAMESPACE` | `scadable_cfg` | NVS namespace the provisioner writes into. |
| `CONFIG_SCD_DEBUG_VERBOSE` | `n` | Enable verbose library logs. |

## Partition table requirement

Your project's `partitions.csv` needs an `nvs` partition ≥ 24 KB at
offset `0x9000`. The ESP-IDF default factory table includes this —
most projects work without changes. If you've customized your
partition table, ensure NVS is present and large enough.

For OTA to work, you also need two `app` partitions (`ota_0` and
`ota_1`) and an `otadata` partition. The standard
`partitions_two_ota.csv` template (or your existing OTA-capable
layout) works.

## What it does NOT do (v0.1.0)

- **Network bring-up** — you handle Wi-Fi / Ethernet / cellular.
  This is intentional: every product has different connectivity
  needs and baking one into a library would constrain rather than
  help.
- **Log forwarding** — coming in v0.2.0.
- **Customer event publishing helpers** — you can call
  `scadable_mqtt_publish()` directly, but the typed-event APIs
  (`scadable_publish_event`, schema-driven codegen) are v0.2.0+.
- **Inbound command handlers** beyond OTA — generic
  `SCADABLE_ON_COMMAND(...)` registration arrives in v0.2.0.
- **Hardware port abstraction** — v0.1.0 is ESP-IDF only.

## Memory footprint

On ESP32 classic (320 KB SRAM), the library uses approximately
**22–25 KB of RAM** at steady state. Customer code gets the rest.

- ~14 KB for the TLS session (mbedTLS, ECDSA P-256 / AES-128-GCM)
- ~3 KB for resident cert + key + CA chain
- ~2 KB heartbeat task stack
- ~3 KB MQTT client task stack
- ~2 KB MQTT client buffers (1 KB out, 2 KB in)

No allocations in the heartbeat or publish hot paths — payloads are
built on the stack with `snprintf` and handed straight to the MQTT
client.

## Public API

```c
#include "scadable.h"

// Customer entry point (you implement this).
void scadable_user_main(void);

// MQTT publish primitive (used internally; available for advanced use).
typedef enum { SCD_QOS_0, SCD_QOS_1, SCD_QOS_2 } scd_qos_t;

esp_err_t scadable_mqtt_publish(const char *topic, const void *payload,
                                int len, scd_qos_t qos, bool retain);

bool scadable_mqtt_connected(void);
```

That's the entire surface for v0.1.0. Three function signatures, one
enum. v0.2.0+ adds typed event publishing and command handlers.

## License

MIT — see [LICENSE](LICENSE).

## Status

v0.1.0 — first release. Published to the ESP-IDF Component Registry
under the `scadable/` namespace.
