/*
 * metrics.c - opt-in metric collection for the heartbeat payload.
 *
 * Counter definitions live here (referenced by hot-path code via
 * extern in metrics.h). Per-metric collectors are #ifdef-guarded by
 * their Kconfig flag, so disabled metrics generate zero code AND zero
 * runtime cost - the heartbeat just skips them entirely.
 */

#include "metrics.h"
#include "scadable_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_app_format.h"
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "sdkconfig.h"

/* ---------- Counter storage ---------- */

_Atomic uint_least64_t scd_bytes_out                = 0;
_Atomic uint_least64_t scd_bytes_in                 = 0;
_Atomic uint_least32_t scd_mqtt_publish_count       = 0;
_Atomic uint_least32_t scd_mqtt_publish_fail_count  = 0;
_Atomic uint_least32_t scd_mqtt_reconnect_count     = 0;
_Atomic uint_least32_t scd_wifi_disconnect_count    = 0;

/* ---------- Always-on stash (set from scadable_main.c) ---------- */

static const char *s_reset_reason = "unknown";
static uint32_t    s_boot_count   = 0;

#ifdef CONFIG_SCD_METRICS_CPU
/* CPU percent between heartbeats. Uses FreeRTOS runtime stats; reports
 * 100 - (idle_runtime_delta / total_runtime_delta * 100). On single-
 * core we read the IDLE task; on dual-core we sum IDLE0 + IDLE1 and
 * scale by 200%. Lifetime accumulators wrap eventually but the delta
 * within a heartbeat interval is always well-defined. */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static uint32_t s_prev_total_runtime = 0;
static uint32_t s_prev_idle_runtime  = 0;

static uint8_t scd_metrics_cpu_pct(void) {
    UBaseType_t n = uxTaskGetNumberOfTasks();
    TaskStatus_t *arr = pvPortMalloc(n * sizeof(TaskStatus_t));
    if (!arr) return 0;
    uint32_t total = 0;
    UBaseType_t got = uxTaskGetSystemState(arr, n, &total);
    uint32_t idle = 0;
    for (UBaseType_t i = 0; i < got; i++) {
        const char *name = arr[i].pcTaskName;
        if (name && (strcmp(name, "IDLE") == 0 ||
                     strcmp(name, "IDLE0") == 0 ||
                     strcmp(name, "IDLE1") == 0)) {
            idle += arr[i].ulRunTimeCounter;
        }
    }
    vPortFree(arr);
    uint32_t d_total = total - s_prev_total_runtime;
    uint32_t d_idle  = idle  - s_prev_idle_runtime;
    s_prev_total_runtime = total;
    s_prev_idle_runtime  = idle;
    if (d_total == 0) return 0;
    uint32_t idle_pct = (uint32_t)(((uint64_t)d_idle * 100) / d_total);
    if (idle_pct > 100) idle_pct = 100;
    return (uint8_t)(100 - idle_pct);
}
#endif

#ifdef CONFIG_SCD_METRICS_OTA_ROLLBACK
/* OTA rollback counter. NVS-persisted under the scadable namespace.
 * Bumped once during boot when esp_ota_check_rollback_is_possible() or
 * an ESP_OTA_IMG_INVALID partition state is detected. For v0.3.0 we
 * only READ it here - the increment lives in scadable_main.c next to
 * the boot_count increment. */
#include "nvs.h"
static uint32_t scd_ota_rollback_count_load(void) {
    nvs_handle_t h;
    if (nvs_open("scadable", NVS_READONLY, &h) != ESP_OK) return 0;
    uint32_t c = 0;
    (void)nvs_get_u32(h, "ota_rollback", &c);
    nvs_close(h);
    return c;
}
#endif

void scd_metrics_set_reset_reason(int r) {
    switch (r) {
        case ESP_RST_POWERON:  s_reset_reason = "poweron";  break;
        case ESP_RST_EXT:      s_reset_reason = "ext";      break;
        case ESP_RST_SW:       s_reset_reason = "sw";       break;
        case ESP_RST_PANIC:    s_reset_reason = "panic";    break;
        case ESP_RST_INT_WDT:  s_reset_reason = "int_wdt";  break;
        case ESP_RST_TASK_WDT: s_reset_reason = "task_wdt"; break;
        case ESP_RST_WDT:      s_reset_reason = "wdt";      break;
        case ESP_RST_DEEPSLEEP:s_reset_reason = "deepsleep";break;
        case ESP_RST_BROWNOUT: s_reset_reason = "brownout"; break;
        case ESP_RST_SDIO:     s_reset_reason = "sdio";     break;
        default:               s_reset_reason = "unknown";  break;
    }
}
const char *scd_reset_reason_str(void) { return s_reset_reason; }

void     scd_metrics_set_boot_count(uint32_t n) { s_boot_count = n; }
uint32_t scd_boot_count(void)                   { return s_boot_count; }

/* ---------- Helper: bounded append ----------
 * Tries to append a printf-style fragment to buf+pos. Returns the new
 * pos, or buf_len (saturated) if it would have overflowed. Caller can
 * detect overflow by checking pos == buf_len after the chain. */
static int appendf(char *buf, int buf_len, int pos, const char *fmt, ...) {
    if (pos >= buf_len) return buf_len;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + pos, buf_len - pos, fmt, ap);
    va_end(ap);
    if (n < 0)              return buf_len;
    if (n >= buf_len - pos) return buf_len;
    return pos + n;
}

int scd_metrics_collect_json(char *buf, int buf_len) {
    if (!buf || buf_len < 16) return -1;
    int p = 0;

#ifdef CONFIG_SCD_METRICS_BYTES_OUT
    p = appendf(buf, buf_len, p, ",\"bytes_out\":%llu",
                (unsigned long long)scd_bytes_out);
#endif

#ifdef CONFIG_SCD_METRICS_BYTES_IN
    p = appendf(buf, buf_len, p, ",\"bytes_in\":%llu",
                (unsigned long long)scd_bytes_in);
#endif

#ifdef CONFIG_SCD_METRICS_CPU
    p = appendf(buf, buf_len, p, ",\"cpu_pct\":%u",
                (unsigned)scd_metrics_cpu_pct());
#endif

#ifdef CONFIG_SCD_METRICS_MEM_PERCENT
    {
        size_t total = heap_caps_get_total_size(MALLOC_CAP_DEFAULT);
        size_t free  = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
        unsigned pct = total ? (unsigned)(100 - (100 * (uint64_t)free / total)) : 0;
        p = appendf(buf, buf_len, p, ",\"mem_pct\":%u", pct);
    }
#endif

#ifdef CONFIG_SCD_METRICS_MEM_KB
    p = appendf(buf, buf_len, p, ",\"mem_kb\":%u",
                (unsigned)(heap_caps_get_free_size(MALLOC_CAP_DEFAULT) / 1024));
#endif

#ifdef CONFIG_SCD_METRICS_STORAGE_KB
    {
        uint64_t free_bytes = 0;
        esp_partition_iterator_t it = esp_partition_find(
            ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, NULL);
        while (it) {
            const esp_partition_t *p_ = esp_partition_get(it);
            if (p_) free_bytes += p_->size; /* coarse: total size of data partitions */
            it = esp_partition_next(it);
        }
        esp_partition_iterator_release(it);
        p = appendf(buf, buf_len, p, ",\"storage_kb\":%llu",
                    (unsigned long long)(free_bytes / 1024));
    }
#endif

#ifdef CONFIG_SCD_METRICS_SERVER_LATENCY
    /* No counter to emit - client_ts_ms is already written by the
     * heartbeat task itself in the always-on prefix. This flag just
     * exists for YAML symmetry so users can list "server_latency_ms"
     * in collect: and have something to enable. */
#endif

#ifdef CONFIG_SCD_METRICS_WIFI_RSSI
    {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            p = appendf(buf, buf_len, p, ",\"wifi_rssi\":%d", (int)ap.rssi);
        }
    }
#endif

#ifdef CONFIG_SCD_METRICS_MQTT_RECONNECT
    p = appendf(buf, buf_len, p, ",\"mqtt_reconnect_count\":%u",
                (unsigned)scd_mqtt_reconnect_count);
#endif

#ifdef CONFIG_SCD_METRICS_OTA_ROLLBACK
    p = appendf(buf, buf_len, p, ",\"ota_rollback_count\":%u",
                (unsigned)scd_ota_rollback_count_load());
#endif

#ifdef CONFIG_SCD_METRICS_PARTITION
    {
        const esp_partition_t *run = esp_ota_get_running_partition();
        p = appendf(buf, buf_len, p, ",\"current_partition\":\"%s\"",
                    run ? run->label : "unknown");
    }
#endif

    /* If overflow happened mid-write, we still return what we wrote.
     * The heartbeat task will close the JSON with "}" using the
     * remaining capacity. Saturation here is preferable to dropping
     * the whole heartbeat. */
    return p;
}
