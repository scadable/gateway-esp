/*
 * config_store.c - see config_store.h for the wire contract.
 *
 * Design notes:
 *
 *   Storage. The whole received payload is kept verbatim — one heap
 *   string in RAM, one NVS blob on flash. NVS keys are limited to 15
 *   chars so per-variable NVS entries can't hold real key names; a
 *   single blob also makes the apply effectively atomic (blob + version
 *   written under one commit). Getters scan the cached JSON on demand
 *   with the same strstr-style extraction the rest of libscadable uses
 *   — no cJSON, no per-variable buffers, no RAM proportional to the
 *   number of keys.
 *
 *   Apply semantics. Full replace, never patch — the backend always
 *   sends the complete resolved map. A version equal to the applied
 *   one is ignored (retained messages re-deliver on every reconnect;
 *   that's normal). Any DIFFERENT version is applied, including a
 *   lower one, so a backend that resets its counter doesn't strand
 *   the fleet. Sync state reaches the dashboard via the heartbeat's
 *   "config_version" echo.
 *
 *   Failure. A malformed or oversized payload is rejected and the last
 *   good map kept. If the NVS write fails, the new map is still applied
 *   to RAM (live behavior stays correct; persistence is what degraded)
 *   and the failure is logged — the heartbeat still echoes the applied
 *   version, which is the truth about what the device is running.
 */

#include "config_store.h"

#include "sdkconfig.h"

#if !CONFIG_SCD_CONFIG_ENABLE

/* Subsystem disabled - stub out the entry points so callers compile
 * regardless of the flag. */
void     scd_config_boot_load(void)                  { /* noop */ }
void     scd_config_start(const scd_identity_t *id)  { (void)id; }
uint32_t scd_config_version(void)                    { return 0; }

#else

#include "scadable.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

static const char *TAG = "scd.config";

#define CFG_JSON_MAX   CONFIG_SCD_CONFIG_MAX_JSON
#define CFG_NVS_NS     CONFIG_SCD_CONFIG_NVS_NAMESPACE
#define CFG_NVS_BLOB   "cfg_json"     /* <= 15 chars (NVS key limit) */
#define CFG_NVS_VER    "cfg_ver"

static SemaphoreHandle_t      s_mux       = NULL;
static char                  *s_json      = NULL;  /* heap, NUL-terminated; NULL = no config */
static _Atomic uint32_t       s_version   = 0;
static scadable_config_cb_t   s_cb        = NULL;
static void                  *s_cb_ctx    = NULL;
static char                   s_topic_cfg[SCD_TOPIC_MAX];

/* ───── JSON helpers (same hand-rolled style as edge.c / ota_machine.c) ───── */

/* Return a pointer just past the '{' of the "config" object, or NULL. */
static const char *find_map(const char *json) {
    const char *p = strstr(json, "\"config\"");
    if (!p) return NULL;
    p += 8;
    while (*p == ' ' || *p == '\t' || *p == ':') p++;
    if (*p != '{') return NULL;
    return p + 1;
}

/* Extract an unsigned int field ("version") — true on success. */
static bool extract_u32(const char *json, const char *key, uint32_t *out) {
    char needle[24];
    int n = snprintf(needle, sizeof(needle), "\"%s\":", key);
    if (n <= 0 || n >= (int)sizeof(needle)) return false;
    const char *p = strstr(json, needle);
    if (!p) return false;
    p += n;
    while (*p == ' ' || *p == '\t') p++;
    char *end = NULL;
    unsigned long v = strtoul(p, &end, 10);
    if (end == p) return false;
    *out = (uint32_t)v;
    return true;
}

/* Locate the raw JSON value for `key` inside the config map. Returns a
 * pointer to the first byte of the value and sets *len / *quoted, or
 * NULL if the key is absent. Caller must hold s_mux.
 *
 * The needle's leading quote anchors the match to a whole key, so
 * "rate" can't match inside "sample_rate_hz". Values are primitives by
 * contract (the schema has no nested types), so a value ends at the
 * next ',' or '}' — or at the closing quote for strings. */
static const char *find_value(const char *key, int *len, bool *quoted) {
    if (!s_json) return NULL;
    const char *map = find_map(s_json);
    if (!map) return NULL;

    char needle[80];
    int n = snprintf(needle, sizeof(needle), "\"%s\":", key);
    if (n <= 0 || n >= (int)sizeof(needle)) return NULL;

    const char *p = strstr(map, needle);
    if (!p) return NULL;
    p += n;
    while (*p == ' ' || *p == '\t') p++;

    if (*p == '"') {                        /* string value */
        p++;
        const char *end = strchr(p, '"');
        if (!end) return NULL;
        *len = (int)(end - p);
        *quoted = true;
        return p;
    }
    const char *end = p;                    /* number / true / false / null */
    while (*end && *end != ',' && *end != '}' &&
           *end != ' ' && *end != '\t' && *end != '\n' && *end != '\r') end++;
    if (end == p) return NULL;
    *len = (int)(end - p);
    *quoted = false;
    return p;
}

/* Copy the raw value of key into out (NUL-terminated). Returns bytes
 * copied, 0 if absent or it doesn't fit. Sets *quoted. */
static size_t copy_value(const char *key, char *out, size_t cap, bool *quoted) {
    if (!key || !out || cap == 0 || !s_mux) return 0;
    size_t copied = 0;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    int len = 0; bool q = false;
    const char *p = find_value(key, &len, &q);
    if (p && (size_t)len < cap) {
        memcpy(out, p, (size_t)len);
        out[len] = '\0';
        copied = (size_t)len;
        if (quoted) *quoted = q;
    }
    xSemaphoreGive(s_mux);
    return copied;
}

/* ───── NVS persistence ───── */

static esp_err_t persist(const char *json, size_t len, uint32_t version) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(CFG_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    if ((err = nvs_set_blob(h, CFG_NVS_BLOB, json, len)) == ESP_OK &&
        (err = nvs_set_u32 (h, CFG_NVS_VER,  version))   == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

/* ───── Inbound config handler ───── */

static void on_mqtt_data(const char *topic, const char *data, int data_len) {
    if (strcmp(topic, s_topic_cfg) != 0) return;
    if (data_len <= 0) return;

    if (data_len >= CFG_JSON_MAX) {
        ESP_LOGW(TAG, "config payload %d bytes > max %d; rejecting",
                 data_len, CFG_JSON_MAX);
        return;
    }

    /* Copy out — the MQTT event buffer is neither NUL-terminated nor
     * ours to keep. */
    char *json = malloc((size_t)data_len + 1);
    if (!json) {
        ESP_LOGE(TAG, "malloc(%d) failed", data_len + 1);
        return;
    }
    memcpy(json, data, (size_t)data_len);
    json[data_len] = '\0';

    uint32_t version = 0;
    if (!extract_u32(json, "version", &version) || !find_map(json)) {
        ESP_LOGW(TAG, "malformed config payload; keeping version %u",
                 (unsigned)atomic_load(&s_version));
        free(json);
        return;
    }

    /* Same version re-delivered (normal for a retained topic on
     * reconnect) — nothing to do; the heartbeat already echoes it. */
    if (version == atomic_load(&s_version) && s_json != NULL) {
        free(json);
        return;
    }

    /* Persist first; the RAM swap happens regardless so live behavior
     * is correct even when flash is unhappy. */
    esp_err_t err = persist(json, (size_t)data_len, version);

    xSemaphoreTake(s_mux, portMAX_DELAY);
    char *old = s_json;
    s_json = json;
    atomic_store(&s_version, version);
    xSemaphoreGive(s_mux);
    free(old);

    ESP_LOGI(TAG, "applied config version %u (%d bytes)%s", (unsigned)version,
             data_len, err == ESP_OK ? "" : " — NVS write FAILED");

    /* Fire the customer callback outside the lock. Runs on the MQTT
     * event task — documented as "keep it short". */
    scadable_config_cb_t cb = s_cb;
    if (cb) cb(s_cb_ctx);
}

/* ───── Entry points (called by scadable_main.c / heartbeat.c) ───── */

void scd_config_boot_load(void) {
    s_mux = xSemaphoreCreateMutex();
    if (!s_mux) {
        ESP_LOGE(TAG, "mutex create failed");
        return;
    }

    nvs_handle_t h;
    if (nvs_open(CFG_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "no cached config (first boot)");
        return;
    }

    size_t len = 0;
    if (nvs_get_blob(h, CFG_NVS_BLOB, NULL, &len) != ESP_OK ||
        len == 0 || len >= CFG_JSON_MAX) {
        nvs_close(h);
        ESP_LOGI(TAG, "no cached config (first boot)");
        return;
    }

    char *json = malloc(len + 1);
    if (!json) {
        nvs_close(h);
        ESP_LOGE(TAG, "malloc(%u) failed", (unsigned)(len + 1));
        return;
    }
    if (nvs_get_blob(h, CFG_NVS_BLOB, json, &len) != ESP_OK) {
        free(json);
        nvs_close(h);
        return;
    }
    json[len] = '\0';

    uint32_t version = 0;
    (void)nvs_get_u32(h, CFG_NVS_VER, &version);
    nvs_close(h);

    if (!find_map(json)) {   /* corrupted cache — discard; retained msg re-converges */
        ESP_LOGW(TAG, "cached config invalid; discarding");
        free(json);
        return;
    }

    s_json = json;
    atomic_store(&s_version, version);
    ESP_LOGI(TAG, "loaded cached config version %u (%u bytes)",
             (unsigned)version, (unsigned)len);
}

void scd_config_start(const scd_identity_t *id) {
    if (!id || !s_mux) return;   /* mutex missing = boot_load failed; stay inert */
    snprintf(s_topic_cfg, sizeof(s_topic_cfg),
             "scadable/devices/%s/cmd/config", id->common_name);

    /* Subscribe is queued if MQTT isn't connected yet and replayed on
     * every (re)connect — and because the config topic is RETAINED,
     * each replay re-delivers the current map. */
    scd_mqtt_add_data_handler(on_mqtt_data);
    scd_mqtt_subscribe(s_topic_cfg);
}

uint32_t scd_config_version(void) {
    return atomic_load(&s_version);
}

/* ───── Public API (declared in scadable.h) ───── */

int32_t scadable_config_int(const char *key, int32_t fallback) {
    char buf[24]; bool quoted;
    if (!copy_value(key, buf, sizeof(buf), &quoted) || quoted) return fallback;
    char *end = NULL;
    long v = strtol(buf, &end, 10);
    if (end == buf) return fallback;
    return (int32_t)v;
}

float scadable_config_float(const char *key, float fallback) {
    char buf[32]; bool quoted;
    if (!copy_value(key, buf, sizeof(buf), &quoted) || quoted) return fallback;
    char *end = NULL;
    float v = strtof(buf, &end);
    if (end == buf) return fallback;
    return v;
}

bool scadable_config_bool(const char *key, bool fallback) {
    char buf[8]; bool quoted;
    if (!copy_value(key, buf, sizeof(buf), &quoted)) return fallback;
    if (!strcmp(buf, "true")  || !strcmp(buf, "1")) return true;
    if (!strcmp(buf, "false") || !strcmp(buf, "0")) return false;
    return fallback;
}

size_t scadable_config_str(const char *key, char *buf, size_t len) {
    bool quoted;
    size_t n = copy_value(key, buf, len, &quoted);
    /* Schema enums + strings arrive quoted; a bare token here means a
     * type mismatch — report absent rather than half-right. */
    if (n > 0 && !quoted) { buf[0] = '\0'; return 0; }
    return n;
}

void scadable_config_on_change(scadable_config_cb_t cb, void *ctx) {
    s_cb_ctx = ctx;
    s_cb     = cb;
}

#endif /* CONFIG_SCD_CONFIG_ENABLE */
