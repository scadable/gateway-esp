/*
 * config_store.h - config variables (docs: "Config Variables (v0.4.0)").
 *
 * The backend publishes the device's resolved config map as a single
 * RETAINED message on scadable/devices/<cn>/cmd/config (QoS 1):
 *
 *   { "version": 7,
 *     "config": { "sample_rate_hz": 25, "pump_enabled": true,
 *                 "mode": "boost", "sys.heartbeat_interval_ms": 30000 } }
 *
 * Retained delivery means the broker replays the current map on every
 * (re)connect — devices that were offline converge automatically, no
 * fetch call. The library caches the map in NVS so scadable_config_*
 * reads return last-known values before the network is up, and echoes
 * the applied version in the heartbeat as "config_version" so the
 * dashboard can show synced / pending per device.
 *
 * Value types are whatever JSON the backend resolved from the schema
 * declared in .scadable/config.yaml — numbers, booleans, strings. The
 * library is schema-agnostic: validation happens in the build pipeline
 * and the dashboard; the device just reads the resolved map with typed
 * getters that fall back on missing/mistyped keys.
 *
 * Customer-facing getters (scadable_config_*) live in scadable.h.
 * All entry points below are no-op stubs when CONFIG_SCD_CONFIG_ENABLE=n.
 */

#pragma once

#include <stdint.h>

#include "scadable_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Load the cached config map from NVS into RAM. Called once from
 * app_main BEFORE scadable_user_main so last-known values are readable
 * from the very first line of customer code, network or not. */
void scd_config_boot_load(void);

/* Register the inbound data handler + subscribe to the config topic.
 * Called from bootstrap_online_task once MQTT is starting. */
void scd_config_start(const scd_identity_t *id);

/* Applied map version for the heartbeat's "config_version" field.
 * 0 = no config ever received (or feature compiled out). */
uint32_t scd_config_version(void);

#ifdef __cplusplus
}
#endif
