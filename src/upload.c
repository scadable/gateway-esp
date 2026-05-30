/*
 * upload.c — streaming file upload to uploads.scadable.com.
 *
 * Compile-time gated by CONFIG_SCD_UPLOAD_ENABLE; when the flag is
 * off this file's contents are an empty translation unit and the
 * scadable_upload_* symbols don't ship.
 *
 * Design:
 *   esp_http_client supports chunked transfer encoding via the
 *   open() / write() / fetch_headers() / read_response() flow. We
 *   wrap one client handle per scd_upload_handle_t. The library
 *   sends Transfer-Encoding: chunked, so the device never has to
 *   know the total size up front.
 *
 *   Identity for the upload comes from NVS — we ship X-Device-CN
 *   identical to what /v1/route uses. No extra config required.
 */

#include "sdkconfig.h"

#if defined(CONFIG_SCD_UPLOAD_ENABLE)

#include "scadable.h"
#include "scadable_internal.h"

#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_log.h"

static const char *TAG = "scd.upload";

/* The opaque handle we hand the customer. Holds the http_client + a
 * tiny bit of bookkeeping so abort() works even mid-chunk. */
struct scd_upload {
    esp_http_client_handle_t client;
    bool                     opened;   /* esp_http_client_open succeeded */
};

/* Forward decl — identity is loaded once at boot in scadable_main.c
 * and exposed via this private extern so upload.c doesn't need to
 * re-read NVS. */
extern const scd_identity_t *scd_get_identity(void);

esp_err_t scadable_upload_begin(const char *filename,
                                const char *content_type,
                                scd_upload_handle_t *out) {
    if (!out) return ESP_ERR_INVALID_ARG;
    *out = NULL;

    const scd_identity_t *id = scd_get_identity();
    if (!id) {
        ESP_LOGE(TAG, "identity not loaded; library not bootstrapped yet?");
        return ESP_ERR_INVALID_STATE;
    }

    scd_upload_handle_t h = calloc(1, sizeof(*h));
    if (!h) return ESP_ERR_NO_MEM;

    esp_http_client_config_t cfg = {
        .url               = CONFIG_SCD_UPLOAD_URL,
        .method            = HTTP_METHOD_PUT,
        .timeout_ms        = CONFIG_SCD_UPLOAD_TIMEOUT_MS,
        .crt_bundle_attach = NULL,   /* use system cert bundle */
    };
    h->client = esp_http_client_init(&cfg);
    if (!h->client) {
        free(h);
        return ESP_ERR_NO_MEM;
    }

    /* Auth + metadata headers. The server resolves org + namespace
     * from the device CN; we don't need to send them. */
    esp_http_client_set_header(h->client, "X-Device-CN", id->common_name);
    esp_http_client_set_header(h->client, "Transfer-Encoding", "chunked");
    esp_http_client_set_header(h->client, "User-Agent", "libscadable/0.2.0");
    if (filename && *filename) {
        esp_http_client_set_header(h->client, "X-Filename", filename);
    }
    if (content_type && *content_type) {
        esp_http_client_set_header(h->client, "X-Content-Type", content_type);
    } else {
        esp_http_client_set_header(h->client, "X-Content-Type",
                                   "application/octet-stream");
    }

    /* Open the connection. Pass -1 for write_len → chunked. */
    esp_err_t err = esp_http_client_open(h->client, -1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_http_client_open: %s", esp_err_to_name(err));
        esp_http_client_cleanup(h->client);
        free(h);
        return err;
    }
    h->opened = true;
    *out = h;
    return ESP_OK;
}

esp_err_t scadable_upload_chunk(scd_upload_handle_t h,
                                const void *bytes, size_t len) {
    if (!h || !h->opened) return ESP_ERR_INVALID_STATE;
    if (!bytes || len == 0) return ESP_ERR_INVALID_ARG;

    int written = esp_http_client_write(h->client, (const char *)bytes,
                                        (int)len);
    if (written < 0 || (size_t)written != len) {
        ESP_LOGW(TAG, "write failed: wrote %d of %u", written, (unsigned)len);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t scadable_upload_end(scd_upload_handle_t h,
                              char *file_id_out, size_t file_id_max) {
    if (!h) return ESP_ERR_INVALID_ARG;
    if (!h->opened) {
        /* Already aborted or never opened — free + bail. */
        if (h->client) esp_http_client_cleanup(h->client);
        free(h);
        return ESP_ERR_INVALID_STATE;
    }

    /* fetch_headers commits the chunked stream and reads the response
     * status + headers. read_response then pulls the body. */
    int content_length = esp_http_client_fetch_headers(h->client);
    int status = esp_http_client_get_status_code(h->client);

    char body[256] = {0};
    if (content_length > 0) {
        int to_read = content_length < (int)sizeof(body) - 1
                          ? content_length
                          : (int)sizeof(body) - 1;
        int n = esp_http_client_read(h->client, body, to_read);
        if (n > 0 && n < (int)sizeof(body)) body[n] = '\0';
    }

    esp_err_t close_err = esp_http_client_close(h->client);
    esp_http_client_cleanup(h->client);
    h->opened = false;
    h->client = NULL;

    if (status != 200) {
        ESP_LOGE(TAG, "upload failed: status=%d body=%s", status, body);
        free(h);
        return ESP_FAIL;
    }
    if (close_err != ESP_OK) {
        ESP_LOGW(TAG, "close: %s", esp_err_to_name(close_err));
    }

    /* Parse "file_id":"f_..." from the response body. Same tiny
     * extractor pattern as edge.c — no cJSON in the hot path. */
    if (file_id_out && file_id_max > 0) {
        file_id_out[0] = '\0';
        const char *key = "\"file_id\":\"";
        const char *p = strstr(body, key);
        if (p) {
            p += strlen(key);
            const char *end = strchr(p, '"');
            if (end) {
                size_t n = (size_t)(end - p);
                if (n >= file_id_max) n = file_id_max - 1;
                memcpy(file_id_out, p, n);
                file_id_out[n] = '\0';
            }
        }
    }

    ESP_LOGI(TAG, "upload ok: %s", body);
    free(h);
    return ESP_OK;
}

void scadable_upload_abort(scd_upload_handle_t h) {
    if (!h) return;
    if (h->client) {
        if (h->opened) (void)esp_http_client_close(h->client);
        esp_http_client_cleanup(h->client);
    }
    free(h);
}

#endif /* CONFIG_SCD_UPLOAD_ENABLE */
