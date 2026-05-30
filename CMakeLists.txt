# scadable/libscadable — ESP-IDF component registration.
#
# Public header lives in include/; private internals in private_include/.
# Customers only see what's in include/scadable.h.
#
# REQUIRES is the public-facing dependency surface (things mentioned in
# our public headers). PRIV_REQUIRES is everything else — implementation
# details that callers don't need to pull transitively.

idf_component_register(
    SRCS
        "src/scadable_main.c"
        "src/identity.c"
        "src/edge.c"
        "src/mqtt_session.c"
        "src/heartbeat.c"
        "src/ota_machine.c"
        "src/upload.c"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "private_include"
    REQUIRES
        # Public surface deps (transitively visible to consumers via scadable.h)
        esp_event              # esp_event_base_t in handler registration sugar
        mqtt                   # esp_mqtt_client_handle_t types if we ever expose them
    PRIV_REQUIRES
        # Implementation deps — callers don't need these
        nvs_flash              # cert/key/CA load from NVS
        esp_http_client        # HTTPS GET /v1/route
        esp_https_ota          # OTA executor
        app_update             # esp_ota_mark_app_valid_cancel_rollback
        esp_netif              # network interface awareness
        esp_wifi               # for ip_event_got_ip_t (not for Wi-Fi setup — customer's job)
        esp_timer              # uptime, scheduled heartbeats
        mbedtls                # TLS for HTTP + MQTT
        log                    # ESP_LOG family
)
