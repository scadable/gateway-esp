/*
 * metrics.h - counter externs + heartbeat-payload collector.
 *
 * Counters are bumped from sibling translation units (mqtt_session.c,
 * edge.c, upload.c) on the hot path. The heartbeat task calls
 * scd_metrics_collect_json() once per heartbeat to fold the opt-in
 * metric fields into the outgoing JSON payload.
 *
 * All collectors are guarded by their respective CONFIG_SCD_METRICS_*
 * Kconfig flags - a metric not enabled at build time compiles to
 * nothing and emits no JSON.
 */
#pragma once

#include <stdatomic.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Hot-path counters - incremented from mqtt/edge/upload, read at
 * heartbeat-build time. atomic_uint_least64_t for bytes (can wrap
 * after many GB); atomic_uint_least32_t for event counts. */
extern _Atomic uint_least64_t scd_bytes_out;
extern _Atomic uint_least64_t scd_bytes_in;
extern _Atomic uint_least32_t scd_mqtt_publish_count;
extern _Atomic uint_least32_t scd_mqtt_publish_fail_count;
extern _Atomic uint_least32_t scd_mqtt_reconnect_count;
extern _Atomic uint_least32_t scd_wifi_disconnect_count;

/* Build the comma-prefixed opt-in metrics JSON fragment into buf.
 *
 * Each enabled metric (CONFIG_SCD_METRICS_*=y) appends ",\"name\":value"
 * to buf. The leading comma matters: the heartbeat caller has already
 * written the always-on fields, so the collector's first byte is always
 * a comma when at least one opt-in metric is enabled.
 *
 * Returns bytes written excluding the NUL terminator. Returns 0 if no
 * opt-in metric is enabled. Returns -1 if buf_len is too small to hold
 * any output (safety guard - won't half-write a JSON field).
 */
int scd_metrics_collect_json(char *buf, int buf_len);

/* Stash the per-boot reset reason as a static string (set once in
 * scadable_main.c at app_main entry). Heartbeat reads it directly to
 * avoid calling esp_reset_reason() repeatedly. */
const char *scd_reset_reason_str(void);
void        scd_metrics_set_reset_reason(int esp_reset_reason_enum);

/* boot_count is NVS-persisted across reboots. Set by scadable_main.c
 * at app_main entry; read by heartbeat. */
uint32_t    scd_boot_count(void);
void        scd_metrics_set_boot_count(uint32_t n);

#ifdef __cplusplus
}
#endif
