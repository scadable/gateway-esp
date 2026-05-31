/*
 * libscadable — internal definitions shared across .c files but not
 * exposed to consumers. Never include this from public headers.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Buffer sizes for the credentials we load from NVS. PEM material is
 * usually ~1.5–2 KB per cert/key. We size with headroom and abort if
 * a key doesn't fit (would indicate a custom provisioner with larger
 * certs that we'd need to bump these for). */
#define SCD_DEVICE_ID_MAX    64
#define SCD_COMMON_NAME_MAX  96
#define SCD_PEM_MAX         3072
#define SCD_HOST_MAX         128
#define SCD_TOPIC_MAX        128

/* identity_t holds everything we read from NVS at boot. Resident for
 * the lifetime of the process — TLS reconnect needs the cert + key,
 * the heartbeat task needs the CN for topic assembly. */
typedef struct {
    char device_id  [SCD_DEVICE_ID_MAX];
    char common_name[SCD_COMMON_NAME_MAX];   /* "SC-<device_id>" */
    char cert_pem   [SCD_PEM_MAX];
    char key_pem    [SCD_PEM_MAX];
    char ca_pem     [SCD_PEM_MAX];
    char mqtt_host  [SCD_HOST_MAX];          /* fallback if /v1/route fails */
    int  mqtt_port;                          /* parsed from NVS string */

    /* Topics derived from CN at boot — heartbeat task uses these. */
    char topic_heartbeat [SCD_TOPIC_MAX];
    char topic_ota_cmd   [SCD_TOPIC_MAX];
    char topic_ota_status[SCD_TOPIC_MAX];
} scd_identity_t;

/* edge_route_t is what the /v1/route HTTPS GET parses into. */
typedef struct {
    char mqtt_host[SCD_HOST_MAX];
    int  mqtt_port;
    char region[16];
} scd_edge_route_t;

/* ─── identity.c ─── */
esp_err_t scd_identity_load(scd_identity_t *out);

/* ─── edge.c ─── */
/* Calls CONFIG_SCD_EDGE_URL with X-Device-CN: <common_name>, parses
 * the JSON response into out. Retries per CONFIG_SCD_EDGE_MAX_RETRIES.
 * Returns ESP_OK or the last error from the HTTP attempt. */
esp_err_t scd_edge_route(const char *common_name, scd_edge_route_t *out);

/* ─── mqtt_session.c ─── */
/* Bring up the MQTT client against the given route, using identity's
 * cert/key/ca for TLS. Idempotent — safe to call from the IP event
 * handler. Triggers heartbeat + OTA task start once connected. */
esp_err_t scd_mqtt_start(const scd_identity_t *id, const scd_edge_route_t *route);

/* Internal accessors used by the public scadable_mqtt_publish wrapper
 * and the internal heartbeat / ota modules. */
bool      scd_mqtt_is_connected(void);
esp_err_t scd_mqtt_publish_raw(const char *topic, const void *payload, int len,
                               int qos, bool retain);
int       scd_mqtt_subscribe (const char *topic);
void      scd_mqtt_set_data_handler(void (*cb)(const char *topic, const char *data, int data_len));

/* ─── heartbeat.c ─── */
void scd_heartbeat_start(const scd_identity_t *id);

/* ─── ota_machine.c ─── */
void scd_ota_start(const scd_identity_t *id);

/* ─── scadable_main.c ─── */
/* Returns a pointer to the single resident identity loaded at boot,
 * or NULL if the library hasn't reached that step (e.g. caller running
 * before scadable_user_main was invoked, which shouldn't happen for
 * customer-facing APIs). Pointer is valid for the process lifetime. */
const scd_identity_t *scd_get_identity(void);

/* v0.3.0 - extern declarations for hot-path counters live in metrics.h.
 * Re-exposed here so mqtt_session.c, edge.c, upload.c can pick them up
 * via the single internal header they already include. */
#include "metrics.h"

#ifdef __cplusplus
}
#endif
