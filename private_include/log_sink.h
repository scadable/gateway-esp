/*
 * log_sink.h - capture ESP_LOG output, batch-publish to MQTT.
 *
 * Ported from firmware/main/log_sink.c with these adaptations:
 *   - Gated behind CONFIG_SCD_LOGS_ENABLE (file compiles to stubs if off)
 *   - Topic built from scd_get_identity()->common_name (was hardcoded)
 *   - Ring buffer size + flush interval + min level from Kconfig
 *   - Hand-rolled JSON (no cJSON dep)
 *
 * Call install() once at app boot BEFORE any other ESP_LOG fires so
 * the boot log gets captured. The flush task is spawned by
 * start_flush_task() after MQTT is up.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void scd_log_sink_install(void);
void scd_log_sink_start_flush_task(void);

#ifdef __cplusplus
}
#endif
