# Integration guide

A step-by-step walkthrough for adding `scadable/libscadable` to a new
or existing ESP-IDF project.

## Prerequisites

- ESP-IDF v5.1 or later.
- A SCADABLE account with a namespace you can provision devices into.
- An ESP32-family board (classic, S2, S3, C2, C3, C6, or H2).

## 1. Add the dependency

In your project's `main/idf_component.yml` (create it if it doesn't
exist):

```yaml
dependencies:
  scadable/libscadable: "^0.1.0"
```

Run `idf.py reconfigure` once to pull the component into your build.
You'll see it appear under `managed_components/scadable__libscadable/`.

## 2. Define `scadable_user_main`

The library owns `app_main` via a weak symbol. You don't define
`app_main`; you define `scadable_user_main` instead. The library
calls it once after NVS + log initialization and BEFORE the network
is up.

Minimal `main/main.c`:

```c
#include "scadable.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"

static const char *TAG = "app";

static void wifi_init_sta(void) {
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_cfg = {
        .sta = {
            .ssid     = CONFIG_MY_WIFI_SSID,
            .password = CONFIG_MY_WIFI_PASSWORD,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_connect());
}

void scadable_user_main(void) {
    ESP_LOGI(TAG, "bringing up Wi-Fi");
    wifi_init_sta();

    // ... your application logic ...
}
```

Note: `esp_netif_init()` and `esp_event_loop_create_default()` are
called by the library before `scadable_user_main` runs, so your
network code can rely on them being available. Calling them again
yourself is harmless (they're idempotent).

## 3. Configure partitions

Your `partitions.csv` must have:

- An `nvs` partition (≥ 24 KB at offset 0x9000) — holds the
  provisioning bundle.
- Two `app` partitions (`ota_0`, `ota_1`) and one `otadata`
  partition — required for OTA.

The ESP-IDF default `partitions_two_ota.csv` template works as-is:

```csv
# Name,   Type, SubType, Offset,   Size,      Flags
nvs,      data, nvs,     0x9000,   0x4000,
otadata,  data, ota,     0xd000,   0x2000,
phy_init, data, phy,     0xf000,   0x1000,
ota_0,    app,  ota_0,   0x10000,  0x180000,
ota_1,    app,  ota_1,   0x190000, 0x180000,
```

Set it via `idf.py menuconfig` → `Partition Table` → `Custom
partition table CSV`.

## 4. Provision a device

In the SCADABLE dashboard:

1. Open the namespace you want the device to live in.
2. Click **Provision device**.
3. Use the browser flasher to write firmware + NVS bundle to your
   board. The flasher writes:
   - The firmware binary to `ota_0`.
   - The NVS bundle to `0x9000` (cert, key, CA, identity, broker
     defaults).

Alternatively, if you already have a way to flash devices, the
provision endpoint returns the NVS bundle as base64 — decode and
write it to NVS at `0x9000` yourself.

## 5. Verify

Hook up `idf.py monitor` and reset the board. You should see:

```
I (...) scadable: libscadable v0.1.0 starting
I (...) scd.identity: identity loaded: cn=SC-... device_id=... default_broker=mqtt-yyz.scadable.com:8883
I (...) app: bringing up Wi-Fi
I (...) wifi: ... connected ...
I (...) scadable: IP acquired; bringing up SCADABLE plumbing
I (...) scd.edge: GET https://edge.scadable.com/v1/route (attempt 1/3)
I (...) scd.edge: broker resolved: mqtt-yyz.scadable.com:8883 (region=yyz)
I (...) scd.mqtt: started -> mqtts://mqtt-yyz.scadable.com:8883 as cn=SC-...
I (...) scd.mqtt: connected
I (...) scd.mqtt: (re)subscribed: scadable/SC-.../ota/command
I (...) scadable: online bootstrap complete
```

After ~30 seconds, the device shows up green on the dashboard.

## 6. Push an OTA update

Bind a GitHub repo to your namespace (Settings → GitHub). Push a new
commit. SCADABLE's build pipeline produces a firmware artifact. In
the dashboard, click **Deploy** on the new build — within a few
seconds, the device starts downloading. You'll see in monitor:

```
I (...) scd.ota: command received: 0.1.1
I (...) scd.ota: starting OTA version=0.1.1 url=https://app.scadable.com/api/firmware/...
I (...) scd.ota: image size 524288 bytes
I (...) scd.ota: ... progress ...
I (...) scd.ota: OTA success; restarting in 1s
```

The new firmware boots, the library marks it valid, and you're done.

## 7. Iterating during development

For dev-only workflows where you don't want to flash a new NVS bundle
every time:

- Override the edge URL at compile time via menuconfig
  (`CONFIG_SCD_EDGE_URL`) to point at your local backend.
- Or pre-baked the cert into NVS once and only reflash firmware
  with `idf.py -p /dev/cu.usbserial-XXX app-flash` (skips bootloader
  + partition table + NVS).

## Common integration patterns

### Pattern: Wi-Fi STA with hard-coded credentials (dev / lab)

Shown in the example above. SSID + password from Kconfig.

### Pattern: Ethernet

```c
void scadable_user_main(void) {
    eth_phy_config_t phy = ETH_PHY_DEFAULT_CONFIG();
    eth_mac_config_t mac = ETH_MAC_DEFAULT_CONFIG();
    // ... esp_eth_driver_install + esp_netif glue ...
}
```

Library reacts to `IP_EVENT_ETH_GOT_IP` exactly as it does for STA.

### Pattern: Customer-driven provisioning (SoftAP captive portal)

```c
void scadable_user_main(void) {
    if (have_stored_wifi_creds()) {
        connect_with_stored_creds();
    } else {
        run_softap_captive_portal();   // blocks until user provisions
        connect_with_stored_creds();
    }
}
```

The library doesn't care how the network came up, just that one
eventually did.

## Troubleshooting

See [troubleshooting.md](./troubleshooting.md).
