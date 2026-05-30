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

/* boot_count is read at boot from NVS and incremented; useful for
 * fleet-health analysis (frequent restarts = a problem). v0.1.0
 * keeps this in RAM only — bumping NVS each boot would wear flash. */
static uint32_t s_boot_count = 1;

/* The full payload fits comfortably under 160 bytes; if it ever
 * doesn't, snprintf truncates rather than overflowing. */
#define HEARTBEAT_BUF_SIZE 192

static void heartbeat_task(void *arg) {
    const scd_identity_t *id = (const scd_identity_t *)arg;
    const TickType_t period = pdMS_TO_TICKS(CONFIG_SCD_HEARTBEAT_INTERVAL_S * 1000);
    char buf[HEARTBEAT_BUF_SIZE];

    /* Give MQTT a beat to come up before the first publish attempt. */
    vTaskDelay(pdMS_TO_TICKS(2000));

    while (1) {
        if (scd_mqtt_is_connected()) {
            int n = snprintf(buf, sizeof(buf),
                "{\"uptime_s\":%lld,\"free_heap\":%u,\"boot\":%u,\"fw\":\"%s\"}",
                (long long)(esp_timer_get_time() / 1000000),
                (unsigned)esp_get_free_heap_size(),
                (unsigned)s_boot_count,
                "0.1.0");
            if (n > 0) {
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
