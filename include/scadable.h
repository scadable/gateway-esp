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

#ifdef __cplusplus
}
#endif
