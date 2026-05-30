/*
 * mqtt_session.c — MQTT client lifecycle + publish primitive.
 *
 * Wraps esp_mqtt_client with:
 *   • a single global handle (one session per device)
 *   • subscribe-replay on reconnect (so OTA listener survives drops)
 *   • a single data-handler callback registered by ota_machine
 *
 * TLS is one-way: we trust the broker's Let's Encrypt cert via the
 * CA chain loaded from NVS. The library carries the device cert + key
 * for future mTLS-at-broker work (V2), but does not present them at
 * the TLS handshake today — the backend doesn't enforce mTLS at the
 * broker yet, so doing so would just bloat the handshake.
 */

#include "scadable_internal.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mqtt_client.h"
#include "sdkconfig.h"

static const char *TAG = "scd.mqtt";

#define MAX_SUBS 4
#define MAX_TOPIC_LEN 128

static esp_mqtt_client_handle_t s_client = NULL;
static atomic_bool              s_connected = ATOMIC_VAR_INIT(false);

static SemaphoreHandle_t s_subs_mux = NULL;
static char              s_topics[MAX_SUBS][MAX_TOPIC_LEN];
static int               s_topic_count = 0;
static void            (*s_data_cb)(const char *, const char *, int) = NULL;

/* Stash CA + (eventually) cert/key so reconnect doesn't need them
 * re-passed. Pointers into the identity struct, which is resident
 * for the process lifetime — so this is safe. */
static const char *s_ca_pem  = NULL;
static const char *s_crt_pem = NULL;
static const char *s_key_pem = NULL;

static void replay_subscriptions(void) {
    if (!s_client) return;
    xSemaphoreTake(s_subs_mux, portMAX_DELAY);
    for (int i = 0; i < s_topic_count; i++) {
        esp_mqtt_client_subscribe_single(s_client, s_topics[i], 0);
        ESP_LOGI(TAG, "(re)subscribed: %s", s_topics[i]);
    }
    xSemaphoreGive(s_subs_mux);
}

static void event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "connected");
        atomic_store(&s_connected, true);
        replay_subscriptions();
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "disconnected");
        atomic_store(&s_connected, false);
        break;
    case MQTT_EVENT_DATA: {
        if (!s_data_cb || event->topic_len <= 0) break;
        /* Topic + data are not NUL-terminated in the event. */
        char topic[MAX_TOPIC_LEN];
        int  tn = event->topic_len < MAX_TOPIC_LEN - 1 ? event->topic_len : MAX_TOPIC_LEN - 1;
        memcpy(topic, event->topic, tn);
        topic[tn] = '\0';
        s_data_cb(topic, event->data, event->data_len);
        break;
    }
    case MQTT_EVENT_ERROR:
        if (event && event->error_handle) {
            ESP_LOGE(TAG, "error: type=%d", event->error_handle->error_type);
        }
        break;
    default:
        break;
    }
}

esp_err_t scd_mqtt_start(const scd_identity_t *id, const scd_edge_route_t *route) {
    if (!id || !route) return ESP_ERR_INVALID_ARG;

    if (s_subs_mux == NULL) {
        s_subs_mux = xSemaphoreCreateMutex();
        if (!s_subs_mux) return ESP_ERR_NO_MEM;
    }

    /* Idempotent: already running -> leave it. The reconnect-on-drop
     * is handled by esp_mqtt's internal loop. */
    if (s_client) {
        ESP_LOGD(TAG, "scd_mqtt_start: already running");
        return ESP_OK;
    }

    /* Stash for potential future reconfigure. */
    s_ca_pem  = id->ca_pem;
    s_crt_pem = id->cert_pem;
    s_key_pem = id->key_pem;

    /* Build "mqtts://host:port" — TLS implied by the scheme. */
    char uri[SCD_HOST_MAX + 32];
    snprintf(uri, sizeof(uri), "mqtts://%s:%d", route->mqtt_host, route->mqtt_port);

    /* Client_id = common_name so the broker sees a stable identity
     * matching the cert CN (useful even without mTLS for ACL setup). */
    esp_mqtt_client_config_t cfg = {
        .broker = {
            .address.uri              = uri,
            .verification.certificate = s_ca_pem,   /* validate broker cert */
        },
        .credentials = {
            .client_id = id->common_name,
        },
        .session = {
            .keepalive = 30,
        },
        .network = {
            .reconnect_timeout_ms = CONFIG_SCD_MQTT_RECONNECT_TIMEOUT_MS,
        },
        .buffer = {
            .size     = 2048,
            .out_size = 1024,
        },
    };

    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) {
        ESP_LOGE(TAG, "esp_mqtt_client_init failed");
        return ESP_FAIL;
    }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, event_handler, NULL);

    esp_err_t err = esp_mqtt_client_start(s_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_mqtt_client_start: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "started -> %s as cn=%s", uri, id->common_name);
    return ESP_OK;
}

bool scd_mqtt_is_connected(void) {
    return atomic_load(&s_connected);
}

esp_err_t scd_mqtt_publish_raw(const char *topic, const void *payload, int len,
                               int qos, bool retain) {
    if (!topic || !payload) return ESP_ERR_INVALID_ARG;
    if (!s_client || !atomic_load(&s_connected)) return ESP_ERR_INVALID_STATE;
    if (len < 0) len = strlen((const char *)payload);
    int msg_id = esp_mqtt_client_publish(s_client, topic, (const char *)payload,
                                         len, qos, retain ? 1 : 0);
    return msg_id >= 0 ? ESP_OK : ESP_ERR_NO_MEM;
}

int scd_mqtt_subscribe(const char *topic) {
    if (!topic) return -1;
    xSemaphoreTake(s_subs_mux, portMAX_DELAY);
    bool already = false;
    for (int i = 0; i < s_topic_count; i++) {
        if (strcmp(s_topics[i], topic) == 0) { already = true; break; }
    }
    if (!already && s_topic_count < MAX_SUBS) {
        strncpy(s_topics[s_topic_count], topic, MAX_TOPIC_LEN - 1);
        s_topics[s_topic_count][MAX_TOPIC_LEN - 1] = '\0';
        s_topic_count++;
    }
    xSemaphoreGive(s_subs_mux);

    if (s_client && atomic_load(&s_connected)) {
        return esp_mqtt_client_subscribe_single(s_client, topic, 0);
    }
    return 0;  /* deferred to next connect */
}

void scd_mqtt_set_data_handler(void (*cb)(const char *, const char *, int)) {
    s_data_cb = cb;
}

/* ───── Public API wrappers ───── */

bool scadable_mqtt_connected(void) {
    return scd_mqtt_is_connected();
}

esp_err_t scadable_mqtt_publish(const char *topic, const void *payload, int len,
                                scd_qos_t qos, bool retain) {
    return scd_mqtt_publish_raw(topic, payload, len, (int)qos, retain);
}
