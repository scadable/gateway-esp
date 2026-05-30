/*
 * libscadable — SCADABLE platform integration for ESP32.
 *
 * This is the entire public surface of the v0.1.0 library. The
 * library owns app_main (as a weak symbol) — your application code
 * lives in scadable_user_main() instead, which the library calls
 * once after NVS + log initialization but BEFORE the network is up.
 *
 * Minimal customer integration:
 *
 *     #include "scadable.h"
 *     #include "esp_wifi.h"
 *
 *     void scadable_user_main(void) {
 *         my_wifi_connect();              // your code: bring up the network
 *         // ... your domain logic ...
 *     }
 *
 * Once the network has an IP, the library wakes up, hits the edge
 * router to discover its broker, opens an MQTT session, starts
 * heartbeating, and listens for OTA commands. Your scadable_user_main
 * runs in parallel — feel free to block in it forever.
 *
 * You do NOT need to define app_main. The library provides it as a
 * weak symbol and calls scadable_user_main for you. If you absolutely
 * need to own app_main (advanced), define your own and it'll override
 * ours; you're then responsible for calling whatever bootstrap you want.
 */

#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ─── MQTT QoS levels ───────────────────────────────────────────────
 *
 * Standard MQTT QoS semantics. Most telemetry uses QoS 0 (fire and
 * forget) — the library's own heartbeat does. Use QoS 1 for events
 * that matter; QoS 2 is rarely worth the cost.
 */
typedef enum {
    SCD_QOS_0 = 0,   /* at most once  — telemetry default */
    SCD_QOS_1 = 1,   /* at least once — important events */
    SCD_QOS_2 = 2,   /* exactly once  — rarely needed */
} scd_qos_t;

/* ─── Customer entry point ──────────────────────────────────────────
 *
 * The library calls this once after NVS + log are initialized. Default
 * implementation (weak) is a noop, so if you don't define it the device
 * boots into headless mode — the library still tries to come online
 * (and will once SOMETHING brings up an IP). For any real product
 * you'll want to define this and bring up your network here.
 *
 * This function is called from the FreeRTOS task that ESP-IDF starts
 * app_main on. You may block forever, return, spawn your own tasks —
 * the library's own tasks run independently.
 */
void scadable_user_main(void);

/* ─── MQTT publish ──────────────────────────────────────────────────
 *
 * Publish raw bytes to a MQTT topic on the SCADABLE broker. The
 * library is event-driven — calling this before MQTT is connected
 * returns ESP_ERR_INVALID_STATE; the caller decides whether to retry
 * or drop. Use scadable_mqtt_connected() to gate calls if you want
 * to avoid the error.
 *
 * v0.1.0 note: the library's internal heartbeat uses this same
 * function. Customer-facing event publish helpers
 * (scadable_publish_event etc.) arrive in v0.2.0+.
 *
 * @param topic    Topic string. Pass either a literal or a
 *                 SCD_TOPIC_* constant (v0.2.0+).
 * @param payload  Pointer to payload bytes (typically JSON).
 * @param len      Length of payload in bytes. Pass -1 if payload is
 *                 a null-terminated string.
 * @param qos      QoS level.
 * @param retain   Whether the broker should retain this message.
 *
 * @return ESP_OK on successful enqueue;
 *         ESP_ERR_INVALID_STATE if MQTT not connected;
 *         ESP_ERR_INVALID_ARG  if topic or payload is NULL;
 *         ESP_ERR_NO_MEM       if the client outbox is full.
 *
 * Thread-safe, non-blocking.
 */
esp_err_t scadable_mqtt_publish(
    const char *topic,
    const void *payload,
    int         len,
    scd_qos_t   qos,
    bool        retain
);

/* ─── Connection state ──────────────────────────────────────────────
 *
 * Returns true once the MQTT session is up and ready to publish. Use
 * to gate scadable_mqtt_publish calls if you want to avoid the
 * ESP_ERR_INVALID_STATE return on disconnect.
 */
bool scadable_mqtt_connected(void);

/* ─── File uploads (v0.2.0; compile-time opt-in) ────────────────────
 *
 * Streaming upload to the SCADABLE platform. Enabled by setting
 * CONFIG_SCD_UPLOAD_ENABLE=y in menuconfig — when off, none of this
 * is in the binary.
 *
 * Usage:
 *
 *     scd_upload_handle_t up;
 *     scadable_upload_begin("crashdump.bin", "application/octet-stream", &up);
 *     while (read_chunk(buf, sizeof(buf), &n)) {
 *         if (scadable_upload_chunk(up, buf, n) != ESP_OK) {
 *             scadable_upload_abort(up);
 *             return;
 *         }
 *     }
 *     char file_id[64];
 *     scadable_upload_end(up, file_id, sizeof(file_id));
 *     ESP_LOGI("app", "uploaded as %s", file_id);
 *
 * Library handles auth via X-Device-CN automatically. Server stamps
 * the org_id and namespace_id by looking up the cert CN; you don't
 * pass them.
 *
 * Memory: the upload chunks the request body, so the device doesn't
 * need to hold the full file in RAM. ~6 KB stack for the worker task.
 */
#if defined(CONFIG_SCD_UPLOAD_ENABLE)

typedef struct scd_upload* scd_upload_handle_t;

/* Open an upload session. Allocates the handle; returns ESP_OK on
 * success, ESP_ERR_NO_MEM or ESP_ERR_INVALID_STATE on failure.
 *
 * @param filename     Filename to record (purely metadata; doesn't
 *                     affect storage). NULL = let server pick.
 * @param content_type MIME type. NULL = "application/octet-stream".
 * @param out          Receives the handle on success.
 */
esp_err_t scadable_upload_begin(const char *filename,
                                const char *content_type,
                                scd_upload_handle_t *out);

/* Append bytes to an in-flight upload. Safe to call from any task,
 * but only one chunk-write may be in flight per handle at a time.
 *
 * @return ESP_OK on success, or ESP_ERR_* on network / state error.
 *         An error leaves the handle in a poisoned state; call
 *         scadable_upload_abort to free it.
 */
esp_err_t scadable_upload_chunk(scd_upload_handle_t h,
                                const void *bytes, size_t len);

/* Finalize an upload. Sends the closing chunk, reads the server's
 * JSON response, parses out the file_id, frees the handle.
 *
 * @param file_id_out   Buffer to receive the server-assigned file_id.
 *                      May be NULL if the caller doesn't care.
 * @param file_id_max   Capacity of file_id_out.
 */
esp_err_t scadable_upload_end(scd_upload_handle_t h,
                              char *file_id_out, size_t file_id_max);

/* Abort and free an in-flight upload. Call this on any error from
 * scadable_upload_chunk so the handle's resources are released.
 * Always safe to call; idempotent. */
void scadable_upload_abort(scd_upload_handle_t h);

#endif /* CONFIG_SCD_UPLOAD_ENABLE */

#ifdef __cplusplus
}
#endif
