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

#include "sdkconfig.h"  /* must precede the CONFIG_SCD_CONFIG_ENABLE check */
#ifdef CONFIG_SCD_CONFIG_ENABLE
#include "scadable.h"       /* scadable_config_int for sys.heartbeat_interval_ms */
#include "config_store.h"   /* scd_config_version for the heartbeat echo */
#endif

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

/* Per-tick period. Kconfig default, overridable live via the reserved
 * sys.heartbeat_interval_ms config variable (clamped to >= 1 s so a
 * dashboard typo can't melt the broker). */
static TickType_t heartbeat_period(void) {
    int32_t ms = CONFIG_SCD_HEARTBEAT_INTERVAL_S * 1000;
#ifdef CONFIG_SCD_CONFIG_ENABLE
    ms = scadable_config_int("sys.heartbeat_interval_ms", ms);
    if (ms < 1000) ms = 1000;
#endif
    return pdMS_TO_TICKS(ms);
}

static void heartbeat_task(void *arg) {
    const scd_identity_t *id = (const scd_identity_t *)arg;
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
#ifdef CONFIG_SCD_CONFIG_ENABLE
                /* config_version echoes the applied config map so the
                 * dashboard can show each device as synced / pending —
                 * drift surfaces within one heartbeat interval. */
                int e = snprintf(buf + n, sizeof(buf) - n,
                                 ",\"config_version\":%u",
                                 (unsigned)scd_config_version());
                if (e > 0 && e < (int)sizeof(buf) - n) n += e;
#endif
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
        vTaskDelay(heartbeat_period());   /* re-read: config can change live */
    }
}

void scd_heartbeat_start(const scd_identity_t *id) {
    /* Stack: 4096, not 2048. The task snprintfs a 512-byte JSON, reads
     * config vars (v0.4.0), and — decisive — when CONFIG_SCD_LOGS_ENABLE
     * hooks vprintf, any ESP_LOG from this task runs a second formatting
     * pass through the sink. A customer task with an identical profile
     * at 2048 hit "stack overflow in task" boot loops on real hardware
     * (ESP32-S3, 2026-06-06); this task only escaped because it logs
     * rarely. Don't thin this back down. */
    BaseType_t ok = xTaskCreate(
        heartbeat_task, "scd_hb", 4096, (void *)id, 3, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate failed");
    }
}
