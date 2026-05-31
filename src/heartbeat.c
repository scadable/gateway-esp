/*
 * heartbeat.c — periodic heartbeat publish task.
 *
 * Stack-only: no cJSON, no malloc, single snprintf into a 160-byte
 * stack buffer. The publish gate (scd_mqtt_is_connected) means we
 * silently skip ticks when the session is down — the next tick after
 * reconnect catches up.
 */

#include "scadable_internal.h"

#include <stdio.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "scd.heartbeat";

/* v0.3.0: buffer now sized for always-on prefix + all opt-in metrics
 * folded in. Snprintf truncates rather than overflowing. */
#define HEARTBEAT_BUF_SIZE 512

static void heartbeat_task(void *arg) {
    const scd_identity_t *id = (const scd_identity_t *)arg;
    const TickType_t period = pdMS_TO_TICKS(CONFIG_SCD_HEARTBEAT_INTERVAL_S * 1000);
    char buf[HEARTBEAT_BUF_SIZE];

    /* Give MQTT a beat to come up before the first publish attempt. */
    vTaskDelay(pdMS_TO_TICKS(2000));

    while (1) {
        if (scd_mqtt_is_connected()) {
            /* v0.3.0 extended payload. Always-on fields go first; the
             * metrics collector appends opt-in fields with a leading
             * comma each. We close the JSON object manually after both
             * have written so the collector doesn't need to know about
             * the closing brace. */
            int64_t now_ms = esp_timer_get_time() / 1000;
            int n = snprintf(buf, sizeof(buf),
                "{\"uptime_s\":%lld,\"free_heap\":%u,\"boot\":%u,\"fw\":\"%s\","
                "\"reset_reason\":\"%s\",\"client_ts_ms\":%lld",
                (long long)(esp_timer_get_time() / 1000000),
                (unsigned)esp_get_free_heap_size(),
                (unsigned)scd_boot_count(),
                "0.3.0",
                scd_reset_reason_str(),
                (long long)now_ms);
            if (n > 0 && n < (int)sizeof(buf)) {
                int m = scd_metrics_collect_json(buf + n, (int)sizeof(buf) - n);
                if (m > 0) n += m;

                /* Close the JSON object. If we somehow ran out of room
                 * before the close, force the last byte to be '}' so
                 * the broker still sees valid JSON. */
                if (n < (int)sizeof(buf) - 1) {
                    buf[n++] = '}';
                    buf[n]   = '\0';
                } else {
                    buf[sizeof(buf) - 2] = '}';
                    buf[sizeof(buf) - 1] = '\0';
                    n = (int)sizeof(buf) - 1;
                }

                esp_err_t err = scd_mqtt_publish_raw(
                    id->topic_heartbeat, buf, n, 0 /* QoS 0 */, false);
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "publish skipped: %s", esp_err_to_name(err));
                }
            }
        }
        vTaskDelay(period);
    }
}

void scd_heartbeat_start(const scd_identity_t *id) {
    BaseType_t ok = xTaskCreate(
        heartbeat_task, "scd_hb", 2048, (void *)id, 3, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate failed");
    }
}
