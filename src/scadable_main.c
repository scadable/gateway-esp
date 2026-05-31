/*
 * scadable_main.c — library entry point + bootstrap orchestration.
 *
 * The library provides app_main as a WEAK symbol so customers don't
 * have to call any init function from their own app_main. The customer
 * defines scadable_user_main instead, which we call after NVS + log
 * init but BEFORE the network is up.
 *
 * If the customer DOES want to own app_main (advanced — e.g. they
 * need to do low-level peripheral init before anything else), they
 * just define their own app_main; the strong symbol overrides ours
 * via standard linker rules. They become responsible for calling
 * whatever bootstrap they want.
 *
 * Boot flow:
 *
 *   1. nvs_flash_init()
 *   2. scd_identity_load()       — cert/key/CA/cn from NVS
 *   3. esp_event_loop_create_default() + register IP_EVENT handler
 *   4. scadable_user_main()      — customer brings up network
 *   5. on IP_EVENT_*_GOT_IP:
 *        - scd_edge_route(cn)    — discover broker
 *        - scd_mqtt_start()      — connect TLS MQTT
 *        - scd_heartbeat_start() — periodic publish
 *        - scd_ota_start()       — subscribe + execute
 *
 * Single global identity is fine — there's exactly one device per
 * process. We keep it static so the heartbeat / OTA tasks read it
 * without an allocation.
 */

#include "scadable_internal.h"

#include <stdbool.h>
#include <string.h>           // strncpy in the NVS-fallback path

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "esp_system.h"   /* esp_reset_reason */
#include "nvs.h"          /* nvs_handle_t, nvs_open, nvs_get_u32, nvs_set_u32 */

static const char *TAG = "scadable";

#define SCD_NVS_NS    "scadable"
#define SCD_NVS_BOOT  "boot_count"

static uint32_t bump_boot_count_in_nvs(void) {
    nvs_handle_t h;
    if (nvs_open(SCD_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return 1;
    uint32_t count = 0;
    (void)nvs_get_u32(h, SCD_NVS_BOOT, &count);
    count++;
    (void)nvs_set_u32(h, SCD_NVS_BOOT, count);
    (void)nvs_commit(h);
    nvs_close(h);
    return count;
}

#ifdef CONFIG_SCD_METRICS_OTA_ROLLBACK
#include "esp_ota_ops.h"
/* Increment the OTA rollback counter when the running image came up
 * in PENDING_VERIFY state - that means the bootloader picked it as
 * the rollback target. Called once per boot. */
static void bump_ota_rollback_in_nvs(void) {
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (!run || esp_ota_get_state_partition(run, &st) != ESP_OK) return;
    if (st != ESP_OTA_IMG_PENDING_VERIFY && st != ESP_OTA_IMG_INVALID) return;
    nvs_handle_t h;
    if (nvs_open(SCD_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    uint32_t c = 0;
    (void)nvs_get_u32(h, "ota_rollback", &c);
    (void)nvs_set_u32(h, "ota_rollback", c + 1);
    (void)nvs_commit(h);
    nvs_close(h);
}
#endif

static scd_identity_t s_identity;
static volatile bool  s_identity_loaded = false;
static volatile bool  s_online_bootstrapped = false;

/* Accessor for sibling translation units (e.g. upload.c) that need
 * the device CN without re-reading NVS. */
const scd_identity_t *scd_get_identity(void) {
    return s_identity_loaded ? &s_identity : NULL;
}

/* Public helper — see include/scadable.h for full doc. Builds
 * "scadable/{cn}/{suffix}" so customer publishes land in the
 * per-tenant namespace that api.scadable.com forwards. */
int scadable_topic(char *buf, size_t buf_len, const char *suffix) {
    if (!buf || buf_len < 16 || !suffix) return -1;
    if (!s_identity_loaded) return -1;
    int n = snprintf(buf, buf_len, "scadable/%s/%s",
                     s_identity.common_name, suffix);
    if (n < 0 || (size_t)n >= buf_len) return -1;
    return n;
}

/* Customer MUST define scadable_user_main in their own code. We used to
 * provide a weak fallback here that just logged "running headless", but
 * in practice the ESP-IDF build system was sometimes resolving the weak
 * fallback over the customer's strong definition (the WHOLE_ARCHIVE
 * treatment of main isn't enough on its own to make strong-over-weak
 * deterministic in this codebase). Forcing the customer to define
 * scadable_user_main turns a silent runtime "no Wi-Fi ever" into a loud
 * link-time "undefined reference to scadable_user_main", which is the
 * right semantics — libscadable does nothing useful without customer
 * code to bring up the network. Burned hours on this 2026-05-31. */
extern void scadable_user_main(void);

/* Bring up the SCADABLE plumbing once we have an IP. Runs in a task
 * spawned from the IP event handler so we can do blocking HTTP +
 * MQTT calls without holding up the event loop. */
static void bootstrap_online_task(void *arg) {
    (void)arg;

    /* Edge route — discover which broker to use. Falls back to the
     * NVS-baked default if the edge call fails entirely. */
    scd_edge_route_t route = {0};
    esp_err_t err = scd_edge_route(s_identity.common_name, &route);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "edge route failed; falling back to NVS default %s:%d",
            s_identity.mqtt_host, s_identity.mqtt_port);
        strncpy(route.mqtt_host, s_identity.mqtt_host, sizeof(route.mqtt_host) - 1);
        route.mqtt_port = s_identity.mqtt_port;
    }

    /* MQTT + supporting tasks. */
    if (scd_mqtt_start(&s_identity, &route) != ESP_OK) {
        ESP_LOGE(TAG, "mqtt start failed; aborting bootstrap");
        vTaskDelete(NULL);
        return;
    }
    scd_heartbeat_start(&s_identity);
    scd_ota_start(&s_identity);

    ESP_LOGI(TAG, "online bootstrap complete");
    vTaskDelete(NULL);
}

/* IP event handler — fires on IP_EVENT_*_GOT_IP for STA, AP, ETH,
 * PPP, etc. Whichever interface brought up an IP, we go. Guard against
 * re-bootstrapping if multiple interfaces come up (only the first
 * wins; subsequent events are noops). */
static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)base; (void)id; (void)data;
    if (s_online_bootstrapped) return;
    s_online_bootstrapped = true;
    ESP_LOGI(TAG, "IP acquired; bringing up SCADABLE plumbing");
    xTaskCreate(bootstrap_online_task, "scd_boot", 6144, NULL, 5, NULL);
}

__attribute__((weak)) void app_main(void) {
    ESP_LOGI(TAG, "libscadable v0.1.0 starting");

    /* NVS — required by esp_wifi for calibration even though we don't
     * use Wi-Fi directly, and required by us for the credential bundle. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    /* v0.3.0: persist boot count + stash reset reason so the heartbeat
     * payload can include both. boot_count is NVS-persisted across
     * reboots; reset_reason is per-boot. */
    scd_metrics_set_boot_count(bump_boot_count_in_nvs());
    scd_metrics_set_reset_reason((int)esp_reset_reason());
#ifdef CONFIG_SCD_METRICS_OTA_ROLLBACK
    bump_ota_rollback_in_nvs();
#endif

    if (scd_identity_load(&s_identity) != ESP_OK) {
        ESP_LOGE(TAG,
            "identity load failed — device is unprovisioned or was flashed "
            "with the wrong NVS bundle. Halting library bootstrap; customer "
            "code will still run.");
        scadable_user_main();
        while (1) vTaskDelay(portMAX_DELAY);
    }
    s_identity_loaded = true;

    /* esp_netif + event loop are prerequisites for receiving IP events.
     * Customer's network code (esp_wifi_init etc.) will also expect
     * these to exist — we set them up before calling user_main. Both
     * are idempotent if customer also calls them. */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                               on_ip_event, NULL));

    /* Hand off to customer. Their code brings up the network; our IP
     * event handler fires once they succeed. */
    scadable_user_main();

    /* If user_main returns, we still need to keep the task alive so
     * the IP event handler can fire and bootstrap_online_task can
     * spawn. */
    while (1) vTaskDelay(portMAX_DELAY);
}
