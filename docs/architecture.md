# Architecture

How the library is structured and what happens at boot.

## File map

```
gateway-esp/
├── include/scadable.h               ← Public API (everything customers see)
├── private_include/scadable_internal.h  ← Cross-file internal types
└── src/
    ├── scadable_main.c              ← Weak app_main + bootstrap orchestration
    ├── identity.c                   ← NVS cert/key/CA/CN load
    ├── edge.c                       ← HTTPS GET /v1/route
    ├── mqtt_session.c               ← MQTT lifecycle + publish primitive
    ├── heartbeat.c                  ← Periodic publish task
    └── ota_machine.c                ← OTA command listener + executor
```

Each `.c` file is one concern. Header comments at the top of each
file explain responsibility and the in/out data flow.

## Boot sequence

```
ESP-IDF startup
       │
       ▼
 app_main()                              ← library, weak symbol
       │
       ├─ nvs_flash_init()
       │
       ├─ scd_identity_load()
       │     └─ nvs_open("scadable_cfg", NVS_READONLY)
       │     └─ read device_id, common_name, cert_pem, key_pem,
       │       ca_pem, mqtt_host, mqtt_port
       │     └─ assemble topic strings (heartbeat, ota/command,
       │       ota/status) using common_name
       │
       ├─ esp_netif_init() + esp_event_loop_create_default()
       ├─ register on_ip_event handler for IP_EVENT_ANY
       │
       ▼
 scadable_user_main()                    ← customer (weak default = noop)
       │  ├─ my_wifi_connect()           ← THEIR code
       │  └─ ... their app logic ...
       │
       ▼  (IP event fires, posts to internal task)
 bootstrap_online_task():
       ├─ scd_edge_route(cn)
       │     └─ HTTPS GET CONFIG_SCD_EDGE_URL with X-Device-CN header
       │     └─ retries CONFIG_SCD_EDGE_MAX_RETRIES times w/ backoff
       │     └─ on failure: fall back to NVS-baked mqtt_host/port
       │
       ├─ scd_mqtt_start(id, route)
       │     └─ esp_mqtt_client_init(uri=mqtts://host:port, ca=ca_pem)
       │     └─ client_id = common_name
       │     └─ register MQTT event handler
       │     └─ esp_mqtt_client_start
       │
       ├─ on MQTT_EVENT_CONNECTED:
       │     └─ replay_subscriptions() — subscribes scadable/<cn>/ota/command
       │
       ├─ scd_heartbeat_start(id) — spawns scd_hb task (2 KB stack)
       └─ scd_ota_start(id)       — registers MQTT data handler,
                                    subscribes ota/command,
                                    spawns scd_ota task (8 KB stack),
                                    marks running image valid
```

## Why the library owns `app_main`

Customers don't write `app_main`. The library provides it as a weak
symbol that does NVS init, identity load, event-loop setup, and then
calls `scadable_user_main`.

Benefits:
- **Customer can't forget bootstrap.** Removes a whole class of "I
  forgot to call scadable_init" bugs.
- **Ordering guaranteed.** NVS, log sink, event loop, identity all
  initialized in a known sequence before any customer code runs.
- **Headless mode works for free.** A device with no
  `scadable_user_main` still has the library running; the OTA path
  can fix the firmware later.
- **AI-friendly.** Exactly one entry-point shape to teach.
  `scadable_user_main(void) { ... }`. No init-then-start sequence to
  get wrong.

Escape hatch: if a customer needs to own `app_main` (e.g. for
low-level peripheral init before anything else), they define their
own `app_main`. The strong symbol overrides ours via standard linker
rules. They become responsible for the bootstrap.

## Event-driven, not blocking

The library never calls `wait_for_network()` — it registers a
handler for `IP_EVENT_ANY_ID` and goes about its business. When
*any* network interface (Wi-Fi STA, AP, Ethernet, PPP) acquires an
IP, the handler fires once and the rest of the bootstrap runs in a
spawned task.

This is why the library doesn't care which transport the customer
brings up. Wi-Fi, Ethernet, cellular — all produce an
`IP_EVENT_GOT_IP` variant. The library reacts to whichever comes
first; subsequent IP events are ignored (guarded by
`s_online_bootstrapped`).

## Topic conventions (v0.1.0)

CN-in-topic, matching what the existing SCADABLE backend speaks.
Future Style A (identity-free topics, broker stamps CN) is a
v0.2.0+ coordinated refactor.

| Topic | Direction | QoS |
|---|---|---|
| `scadable/<cn>/heartbeat` | device → backend | 0 |
| `scadable/<cn>/ota/command` | backend → device | (broker's choice) |
| `scadable/<cn>/ota/status` | device → backend | 1 |

`<cn>` is the certificate Common Name, format `SC-<device_id>`.
Topics are assembled once at boot in `scd_identity_load` and stored
in the resident `scd_identity_t` struct.

## TLS posture (v0.1.0)

- **One-way TLS** to the broker. Device validates the broker's
  Let's Encrypt cert against the CA chain loaded from NVS.
- **mTLS at broker not enforced.** The library carries the device
  cert + key for future use, but does NOT present them at the TLS
  handshake. The backend enforces identity via `X-Device-CN` on the
  edge HTTP call and via topic-prefix matching, not via cert
  validation at the broker.
- **Plan to add full mTLS in v0.2.0+** once backend support lands.

## Memory discipline

The hot paths (heartbeat publish, MQTT publish, OTA status) do zero
heap allocation:

- **Heartbeat** builds its ~150-byte JSON payload with `snprintf`
  into a 192-byte stack buffer, then hands the pointer to
  `esp_mqtt_client_publish` (which internally copies once into its
  own outbox — that's the single allocation).
- **OTA status** uses the same pattern with a 256-byte stack buffer.
- **Edge JSON parsing** uses a tiny in-place extractor (no cJSON)
  for the three fields we need (`mqtt_host`, `mqtt_port`, `region`).

Resident memory:

| Item | Size |
|---|---|
| `scd_identity_t` (single static) | ~10 KB (mostly the 3 PEM buffers @ 3 KB each) |
| Subscription table | < 1 KB (4 entries × 128 bytes) |
| MQTT client buffers (configured) | 2 KB in + 1 KB out |
| MQTT client task stack | 3 KB |
| Heartbeat task stack | 2 KB |
| OTA task stack | 8 KB (spawned on demand) |
| TLS session (mbedTLS, P-256/AES-GCM) | ~14 KB |
| **Total at steady state** | **~22–25 KB** |

## Why no `core/` + `port/esp_idf/` split (yet)

The original design called for an Approach-B layout — a
platform-agnostic `core/` separated from `port/esp_idf/` — so
future STM32 / POSIX ports could land without API churn.

For v0.1.0's ~1400 LOC of code, splitting adds files without
benefit when there's only one impl. The decision is **reversible**:
splitting six files into `core/` + `port/esp_idf/` is a mechanical
move and adds no semantic change. We revisit if/when a non-ESP-IDF
target becomes real.

## Why CN-in-topic instead of Style A (identity-free topics)?

Style A — `scadable/heartbeat` with the broker stamping the cert CN
on receipt — is cleaner long-term: smaller packets, no string
assembly. But adopting it requires backend changes (the EMQX
webhook handler currently parses topic strings to extract device
IDs). Doing both refactors at once would couple two risks. We do
the library extraction first, then the topic refactor as a
coordinated backend + library v0.2.0 change.

Already-flashed v0.1.0 devices speaking CN-in-topic will continue
to work indefinitely after the v0.2.0 backend lands; the backend
will speak both conventions during the transition.
