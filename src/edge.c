/*
 * edge.c — HTTPS GET to the SCADABLE edge router (/v1/route).
 *
 * The edge call tells the device which MQTT broker to use. It also
 * promotes the device's provisioning record to "operational" on the
 * server side — that's a side effect we want even though we ignore
 * the response detail beyond mqtt_host / mqtt_port / region.
 *
 * If the call fails entirely (no network, DNS down, edge down), the
 * caller falls back to the NVS-baked mqtt_host / mqtt_port — which is
 * the value the backend wrote at provision time and is still likely
 * correct. Belt-and-suspenders.
 */

#include "scadable_internal.h"

#include <string.h>

#include "esp_crt_bundle.h"       // bundled CA store (Let's Encrypt et al.) for HTTPS edge call
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "scd.edge";

/* Tiny JSON field extractor — pulls "key":"value" or "key":number out
 * of a small, well-formed response. Avoids dragging cJSON into the
 * library hot path for one 80-byte payload.
 *
 * Returns the byte offset into src just past the value, or -1 if not
 * found. Does NOT validate strict JSON — assumes the server response
 * shape is what edge.go produces. */
static int extract_str(const char *src, int srclen, const char *key,
                       char *out, int outcap) {
    /* Build the search needle: "key": */
    char needle[64];
    int n = snprintf(needle, sizeof(needle), "\"%s\":", key);
    if (n <= 0 || n >= (int)sizeof(needle)) return -1;

    const char *p = strstr(src, needle);
    if (!p) return -1;
    p += n;
    /* skip whitespace */
    while (p < src + srclen && (*p == ' ' || *p == '\t')) p++;
    /* expect opening quote */
    if (p >= src + srclen || *p != '"') return -1;
    p++;
    const char *end = memchr(p, '"', src + srclen - p);
    if (!end) return -1;
    int len = end - p;
    if (len >= outcap) len = outcap - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    return (end - src) + 1;
}

static int extract_int(const char *src, int srclen, const char *key) {
    char needle[64];
    int n = snprintf(needle, sizeof(needle), "\"%s\":", key);
    if (n <= 0 || n >= (int)sizeof(needle)) return -1;

    const char *p = strstr(src, needle);
    if (!p) return -1;
    p += n;
    while (p < src + srclen && (*p == ' ' || *p == '\t')) p++;
    if (p >= src + srclen) return -1;

    int val = 0;
    int seen_digit = 0;
    while (p < src + srclen && *p >= '0' && *p <= '9') {
        val = val * 10 + (*p - '0');
        seen_digit = 1;
        p++;
    }
    return seen_digit ? val : -1;
}

/* Buffer the response body across multiple ESP_HTTP_CLIENT_ON_DATA
 * events. esp_http_client doesn't give us a single contiguous buffer
 * by default; we accumulate into a fixed-size scratch. */
typedef struct {
    char buf[512];
    int  used;
    bool overflowed;
} resp_buf_t;

static esp_err_t http_event(esp_http_client_event_t *e) {
    resp_buf_t *rb = (resp_buf_t *)e->user_data;
    if (e->event_id == HTTP_EVENT_ON_DATA && rb && e->data && e->data_len > 0) {
        int room = (int)sizeof(rb->buf) - 1 - rb->used;
        if (e->data_len > room) {
            rb->overflowed = true;
            return ESP_OK;
        }
        memcpy(rb->buf + rb->used, e->data, e->data_len);
        rb->used += e->data_len;
        rb->buf[rb->used] = '\0';
    }
    return ESP_OK;
}

static esp_err_t try_once(const char *url, const char *cn, scd_edge_route_t *out) {
    resp_buf_t rb = {0};

    esp_http_client_config_t cfg = {
        .url               = url,
        .method            = HTTP_METHOD_GET,
        .timeout_ms        = CONFIG_SCD_EDGE_TIMEOUT_MS,
        .event_handler     = http_event,
        .user_data         = &rb,
        /* Attach the bundled CA store (CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
         * defaults to y in any IDF v5+ project that doesn't explicitly
         * turn it off). edge.scadable.com is fronted by Caddy with a
         * Let's Encrypt cert; ISRG Root X1 is in the bundle. Setting
         * this to NULL (as we did before v0.2.3) caused esp-tls to
         * refuse the connection with "No server verification option
         * set in esp_tls_cfg_t structure". */
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_ERR_NO_MEM;

    esp_http_client_set_header(client, "X-Device-CN", cn);
    esp_http_client_set_header(client, "User-Agent", "libscadable/0.1.0");

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "perform: %s", esp_err_to_name(err));
        return err;
    }
    if (status != 200) {
        ESP_LOGW(TAG, "status=%d body=\"%.*s\"", status, rb.used, rb.buf);
        return ESP_FAIL;
    }
    if (rb.overflowed) {
        ESP_LOGW(TAG, "response > %d bytes; truncated", (int)sizeof(rb.buf));
        return ESP_ERR_INVALID_SIZE;
    }

    /* Parse mqtt_host, mqtt_port, region. */
    if (extract_str(rb.buf, rb.used, "mqtt_host", out->mqtt_host, sizeof(out->mqtt_host)) < 0) {
        ESP_LOGW(TAG, "no mqtt_host in response: %.*s", rb.used, rb.buf);
        return ESP_FAIL;
    }
    out->mqtt_port = extract_int(rb.buf, rb.used, "mqtt_port");
    if (out->mqtt_port <= 0) {
        ESP_LOGW(TAG, "no/bad mqtt_port in response: %.*s", rb.used, rb.buf);
        return ESP_FAIL;
    }
    /* region is informational — missing is fine. */
    extract_str(rb.buf, rb.used, "region", out->region, sizeof(out->region));

    return ESP_OK;
}

esp_err_t scd_edge_route(const char *common_name, scd_edge_route_t *out) {
    if (!common_name || !out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));

    const char *url = CONFIG_SCD_EDGE_URL;
    int max_attempts = CONFIG_SCD_EDGE_MAX_RETRIES + 1;
    int backoff_ms = 1000;

    for (int i = 1; i <= max_attempts; i++) {
        ESP_LOGI(TAG, "GET %s (attempt %d/%d)", url, i, max_attempts);
        esp_err_t err = try_once(url, common_name, out);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "broker resolved: %s:%d (region=%s)",
                out->mqtt_host, out->mqtt_port,
                out->region[0] ? out->region : "?");
            return ESP_OK;
        }
        if (i < max_attempts) {
            vTaskDelay(pdMS_TO_TICKS(backoff_ms));
            backoff_ms = backoff_ms * 2 < 30000 ? backoff_ms * 2 : 30000;
        }
    }
    return ESP_FAIL;
}
