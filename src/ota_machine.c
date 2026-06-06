/*
 * ota_machine.c — OTA command listener + executor.
 *
 * Subscribes to scadable/<cn>/ota/command. Backend sends:
 *
 *   { "version": "0.2.3",
 *     "url":     "https://app.scadable.com/api/firmware/<artifact>.bin" }
 *
 * We parse, queue the request, and execute esp_https_ota in a
 * dedicated short-lived task (so the MQTT event loop isn't blocked
 * for the duration of the download). Progress + result are published
 * to scadable/<cn>/ota/status with shape:
 *
 *   { "version": "...", "state": "downloading|progress|success|failed",
 *     "details": "..." }
 *
 * On success the device restarts; the new image is marked valid on
 * the next boot (esp_ota_mark_app_valid_cancel_rollback).
 *
 * Lifted with light edits from scadable-v5/firmware/main/ota.c —
 * same wire protocol so the existing backend works unchanged.
 */

#include "scadable_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "scd.ota";

typedef struct {
    char version[64];
    char url[256];
} ota_request_t;

static QueueHandle_t        s_queue   = NULL;
static const scd_identity_t *s_id     = NULL;

/* Status publish — no cJSON, just snprintf. Payload is small. */
static void publish_status(const char *version, const char *state, const char *details) {
    if (!s_id) return;
    char buf[256];
    int n;
    if (details && *details) {
        n = snprintf(buf, sizeof(buf),
            "{\"version\":\"%s\",\"state\":\"%s\",\"details\":\"%s\"}",
            version, state, details);
    } else {
        n = snprintf(buf, sizeof(buf),
            "{\"version\":\"%s\",\"state\":\"%s\"}", version, state);
    }
    if (n > 0) {
        scd_mqtt_publish_raw(s_id->topic_ota_status, buf, n,
                             1 /* QoS 1 — status matters */, false);
    }
}

static void do_ota(const ota_request_t *req) {
    ESP_LOGI(TAG, "starting OTA version=%s url=%s", req->version, req->url);
    publish_status(req->version, "downloading", NULL);

    esp_http_client_config_t http_cfg = {
        .url                          = req->url,
        .timeout_ms                   = 60000,
        .keep_alive_enable            = true,
        .skip_cert_common_name_check  = true,
    };
    esp_https_ota_config_t ota_cfg = { .http_config = &http_cfg };

    esp_https_ota_handle_t handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_cfg, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota_begin: %s", esp_err_to_name(err));
        publish_status(req->version, "failed", esp_err_to_name(err));
        return;
    }

    int total = esp_https_ota_get_image_size(handle);
    ESP_LOGI(TAG, "image size %d bytes", total);

    int last_pct = -1;
    while (1) {
        err = esp_https_ota_perform(handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) break;
        int read = esp_https_ota_get_image_len_read(handle);
        int pct = total > 0 ? (int)((int64_t)read * 100 / total) : 0;
        /* publish every 10% to keep the status topic uncluttered */
        if ((pct / 10) != (last_pct / 10)) {
            char buf[64];
            snprintf(buf, sizeof(buf), "%d%% (%d/%d)", pct, read, total);
            publish_status(req->version, "progress", buf);
            last_pct = pct;
        }
    }

    if (err != ESP_OK || !esp_https_ota_is_complete_data_received(handle)) {
        ESP_LOGE(TAG, "ota_perform: %s", esp_err_to_name(err));
        publish_status(req->version, "failed", esp_err_to_name(err));
        esp_https_ota_abort(handle);
        return;
    }

    err = esp_https_ota_finish(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota_finish: %s", esp_err_to_name(err));
        publish_status(req->version, "failed", esp_err_to_name(err));
        return;
    }

    publish_status(req->version, "success", "restarting");
    ESP_LOGI(TAG, "OTA success; restarting in 1s");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

static void ota_task(void *arg) {
    ota_request_t req;
    while (1) {
        if (xQueueReceive(s_queue, &req, portMAX_DELAY) == pdTRUE) {
            do_ota(&req);
        }
    }
}

/* Tiny single-field JSON extractor — same pattern as edge.c. */
static int extract_str(const char *src, int srclen, const char *key,
                       char *out, int outcap) {
    char needle[64];
    int n = snprintf(needle, sizeof(needle), "\"%s\":", key);
    if (n <= 0 || n >= (int)sizeof(needle)) return -1;
    const char *p = strstr(src, needle);
    if (!p) return -1;
    p += n;
    while (p < src + srclen && (*p == ' ' || *p == '\t')) p++;
    if (p >= src + srclen || *p != '"') return -1;
    p++;
    const char *end = memchr(p, '"', src + srclen - p);
    if (!end) return -1;
    int len = end - p;
    if (len >= outcap) len = outcap - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    return 0;
}

static void on_mqtt_data(const char *topic, const char *data, int data_len) {
    if (!s_id) return;
    if (strcmp(topic, s_id->topic_ota_cmd) != 0) return;

    ota_request_t req = {0};
    if (extract_str(data, data_len, "version", req.version, sizeof(req.version)) != 0 ||
        extract_str(data, data_len, "url",     req.url,     sizeof(req.url))     != 0) {
        ESP_LOGW(TAG, "command missing version/url; ignoring");
        return;
    }

    ESP_LOGI(TAG, "command received: %s", req.version);
    if (xQueueSend(s_queue, &req, 0) != pdTRUE) {
        ESP_LOGW(TAG, "queue full; dropping command");
    }
}

void scd_ota_start(const scd_identity_t *id) {
    s_id = id;

    s_queue = xQueueCreate(2, sizeof(ota_request_t));
    if (!s_queue) {
        ESP_LOGE(TAG, "xQueueCreate failed");
        return;
    }

    /* Register our handler with the MQTT layer + subscribe. The
     * subscribe is queued if MQTT isn't connected yet — replayed
     * on each (re)connect. */
    scd_mqtt_add_data_handler(on_mqtt_data);
    scd_mqtt_subscribe(s_id->topic_ota_cmd);

    /* On boot, mark the running image valid so the bootloader doesn't
     * roll back on the next reset. Idempotent — no-op if not pending. */
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (running && esp_ota_get_state_partition(running, &state) == ESP_OK) {
        if (state == ESP_OTA_IMG_PENDING_VERIFY) {
            esp_ota_mark_app_valid_cancel_rollback();
            ESP_LOGI(TAG, "marked running image valid");
        }
    }

    xTaskCreate(ota_task, "scd_ota", 8192, NULL, 5, NULL);
}
