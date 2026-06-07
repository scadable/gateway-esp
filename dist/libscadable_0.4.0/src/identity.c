/*
 * identity.c — load the device cert/key/CA + identity from NVS.
 *
 * The SCADABLE backend provisioning flow writes these into NVS at
 * flash time (see backend/nvsgen.go + backend/provision.go). We just
 * read them.
 *
 * If anything required is missing, we log loudly and return an error.
 * Devices that reach the library without a valid bundle were flashed
 * with the wrong artifact — there is no recovery, only diagnostics.
 */

#include "scadable_internal.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "scd.identity";

/* Read a NUL-terminated string from NVS into a fixed-size buffer.
 * Returns ESP_OK on success; logs and returns the err on failure. */
static esp_err_t read_str(nvs_handle_t h, const char *key, char *buf, size_t cap) {
    size_t len = cap;
    esp_err_t err = nvs_get_str(h, key, buf, &len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_get_str(%s): %s", key, esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

esp_err_t scd_identity_load(scd_identity_t *out) {
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    nvs_handle_t h;
    const char *ns = CONFIG_SCD_NVS_NAMESPACE;
    esp_err_t err = nvs_open(ns, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
            "nvs_open(\"%s\") failed: %s — was this device provisioned via "
            "the SCADABLE dashboard? Expected the provisioner to populate "
            "the scadable_cfg namespace before first boot.",
            ns, esp_err_to_name(err));
        return err;
    }

    /* Pull each required field. First failure aborts and closes. */
    if ((err = read_str(h, "device_id",   out->device_id,   sizeof(out->device_id)))   != ESP_OK) goto out;
    if ((err = read_str(h, "common_name", out->common_name, sizeof(out->common_name))) != ESP_OK) goto out;
    if ((err = read_str(h, "cert_pem",    out->cert_pem,    sizeof(out->cert_pem)))    != ESP_OK) goto out;
    if ((err = read_str(h, "key_pem",     out->key_pem,     sizeof(out->key_pem)))     != ESP_OK) goto out;
    if ((err = read_str(h, "ca_pem",      out->ca_pem,      sizeof(out->ca_pem)))      != ESP_OK) goto out;
    if ((err = read_str(h, "mqtt_host",   out->mqtt_host,   sizeof(out->mqtt_host)))   != ESP_OK) goto out;

    char port_str[16] = {0};
    if ((err = read_str(h, "mqtt_port", port_str, sizeof(port_str))) != ESP_OK) goto out;
    out->mqtt_port = atoi(port_str);
    if (out->mqtt_port <= 0 || out->mqtt_port > 65535) {
        ESP_LOGE(TAG, "mqtt_port %s out of range", port_str);
        err = ESP_ERR_INVALID_STATE;
        goto out;
    }

    /* Pre-assemble the topics that depend on CN. Done once at boot so
     * the heartbeat task doesn't allocate or format every tick. */
    snprintf(out->topic_heartbeat,  sizeof(out->topic_heartbeat),
             "scadable/%s/heartbeat", out->common_name);
    snprintf(out->topic_ota_cmd,    sizeof(out->topic_ota_cmd),
             "scadable/%s/ota/command", out->common_name);
    snprintf(out->topic_ota_status, sizeof(out->topic_ota_status),
             "scadable/%s/ota/status", out->common_name);

    ESP_LOGI(TAG, "identity loaded: cn=%s device_id=%s default_broker=%s:%d",
        out->common_name, out->device_id, out->mqtt_host, out->mqtt_port);

out:
    nvs_close(h);
    return err;
}
