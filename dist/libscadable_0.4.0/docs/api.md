# API reference

The entire v0.1.0 public API — three functions and one enum.

```c
#include "scadable.h"
```

## `scadable_user_main`

```c
void scadable_user_main(void);
```

The library's entry hook into your code. The library calls it once
after NVS + log initialization, BEFORE the network is up.

You define this; the library provides a weak no-op default. If you
don't define it, the device boots into headless mode (logs a warning
and idles).

Inside `scadable_user_main`:
- You're free to block forever, return, or spawn your own tasks.
- `esp_netif_init()` and `esp_event_loop_create_default()` have
  already been called — your network setup can rely on them.
- The library's bootstrap is **dormant**, waiting for an `IP_EVENT`.
  As soon as your code brings up an IP (Wi-Fi STA, Ethernet, etc.),
  the library wakes up and connects to the SCADABLE platform.

### Example

```c
void scadable_user_main(void) {
    my_wifi_connect();      // your responsibility

    while (1) {
        do_application_work();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
```

### Advanced: owning `app_main`

If you need lower-level control (e.g. peripheral init that has to
run before anything else), define your own `app_main` strongly:

```c
void app_main(void) {
    my_pre_bootstrap_init();
    // ... whatever you want; library's weak app_main is overridden ...
}
```

You're then responsible for setting up the network and the library
won't do anything automatically. This is rarely needed.

---

## `scd_qos_t`

```c
typedef enum {
    SCD_QOS_0 = 0,   /* at most once  — telemetry default */
    SCD_QOS_1 = 1,   /* at least once — important events */
    SCD_QOS_2 = 2,   /* exactly once  — rarely needed */
} scd_qos_t;
```

Standard MQTT QoS levels. Library's own heartbeat uses `SCD_QOS_0`;
OTA status uses `SCD_QOS_1`.

---

## `scadable_mqtt_publish`

```c
esp_err_t scadable_mqtt_publish(
    const char *topic,
    const void *payload,
    int         len,
    scd_qos_t   qos,
    bool        retain
);
```

Publish raw bytes to a MQTT topic on the SCADABLE broker.

### Parameters

| Param | Description |
|---|---|
| `topic` | Topic string. In v0.1.0 you typically don't call this directly. v0.2.0+ adds `SCD_TOPIC_*` constants generated from `.scadable/mqtt-config.yaml`. |
| `payload` | Pointer to payload bytes. Typically JSON but can be anything. |
| `len` | Length of payload in bytes. Pass `-1` for null-terminated strings. |
| `qos` | QoS level (see above). |
| `retain` | Whether the broker should retain this message. |

### Return values

| Value | Meaning |
|---|---|
| `ESP_OK` | Successfully enqueued for transmission. |
| `ESP_ERR_INVALID_STATE` | MQTT not connected. Use `scadable_mqtt_connected()` to gate. |
| `ESP_ERR_INVALID_ARG` | `topic` or `payload` is NULL. |
| `ESP_ERR_NO_MEM` | MQTT client outbox is full. |

### Properties

- **Thread-safe** — safe to call from any task.
- **Non-blocking** — returns immediately; transmission happens
  asynchronously in the MQTT client task.
- **No heap allocation in the call path** — your `payload` pointer
  is handed straight to `esp_mqtt_client_publish`, which copies once
  into its outbox.

### Example

```c
char buf[64];
int n = snprintf(buf, sizeof(buf), "{\"v\":%.2f}", reading);
if (scadable_mqtt_connected()) {
    scadable_mqtt_publish("my/custom/topic", buf, n, SCD_QOS_0, false);
}
```

---

## `scadable_mqtt_connected`

```c
bool scadable_mqtt_connected(void);
```

Returns `true` once the MQTT session is up and ready to publish.

Use this to gate `scadable_mqtt_publish` calls if you want to avoid
the `ESP_ERR_INVALID_STATE` return on disconnect — or to defer some
domain logic until the platform link is live.

### Example

```c
while (!scadable_mqtt_connected()) {
    ESP_LOGI("app", "waiting for SCADABLE link");
    vTaskDelay(pdMS_TO_TICKS(1000));
}
ESP_LOGI("app", "online; starting reading loop");
```

---

## What's NOT in v0.1.0

These will land in v0.2.0+:

```c
// Semantic event publish (auto-prefixes scadable/events/, wraps with
// metadata envelope):
esp_err_t scadable_publish_event(const char *name, const char *json);

// Command handler registration:
void SCADABLE_ON_COMMAND(const char *name, scd_command_handler_t cb);

// Typed event publish (codegen'd from .scadable/events.yaml):
esp_err_t scadable_publish_temperature(const scd_evt_temperature_t *e);

// Log forwarding (subscribes to ESP_LOG_*, batches to broker):
esp_err_t scadable_logs_start(void);
```

If you need any of these in v0.1.0, you can build them yourself on
top of `scadable_mqtt_publish`. They're convenience layers; nothing
is impossible without them.
