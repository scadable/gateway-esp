#!/usr/bin/env python3
# Translate a project's .scadable/config.yaml observability block into
# CONFIG_SCD_* sdkconfig defaults, so a LOCAL `idf.py build` enables the
# same heartbeat / metrics / logs as the SCADABLE cloud build pipeline.
#
# Invoked from project_include.cmake. Mirrors the translator in the
# backend build script (backend/repos.go). Config *variables* (the
# `config:` block) are runtime-only and intentionally NOT emitted here.
#
#   usage: scadable_gen_defaults.py <config.yaml> <out_sdkconfig_defaults>
#
# Writes the out file (possibly empty) and always exits 0 — a missing or
# malformed config must never break the consumer's build.

import sys

METRIC_FLAGS = {
    "bytes_out":            "CONFIG_SCD_METRICS_BYTES_OUT",
    "bytes_in":             "CONFIG_SCD_METRICS_BYTES_IN",
    "cpu_percent":          "CONFIG_SCD_METRICS_CPU",
    "mem_percent":          "CONFIG_SCD_METRICS_MEM_PERCENT",
    "mem_kb":               "CONFIG_SCD_METRICS_MEM_KB",
    "storage_kb":           "CONFIG_SCD_METRICS_STORAGE_KB",
    "server_latency_ms":    "CONFIG_SCD_METRICS_SERVER_LATENCY",
    "wifi_rssi":            "CONFIG_SCD_METRICS_WIFI_RSSI",
    "mqtt_reconnect_count": "CONFIG_SCD_METRICS_MQTT_RECONNECT",
    "ota_rollback_count":   "CONFIG_SCD_METRICS_OTA_ROLLBACK",
    "current_partition":    "CONFIG_SCD_METRICS_PARTITION",
}


def main():
    if len(sys.argv) != 3:
        return 0
    src, out = sys.argv[1], sys.argv[2]

    lines = []
    try:
        import yaml
        with open(src) as f:
            cfg = yaml.safe_load(f) or {}
    except Exception as e:
        # No yaml module or unreadable/garbage file: emit nothing rather
        # than guess. Local dev can still set flags by hand.
        sys.stderr.write(f"scadable: skipping config.yaml translation: {e}\n")
        open(out, "w").close()
        return 0

    hb = cfg.get("heartbeat", {}) or {}
    if isinstance(hb, dict) and hb.get("interval_seconds"):
        lines.append(f"CONFIG_SCD_HEARTBEAT_INTERVAL_S={int(hb['interval_seconds'])}")

    metrics = cfg.get("metrics", {}) or {}
    for name in (metrics.get("collect", []) or []):
        flag = METRIC_FLAGS.get(name)
        if flag:
            lines.append(f"{flag}=y")

    logs = cfg.get("logs", {}) or {}
    if isinstance(logs, dict) and logs.get("enabled"):
        lines.append("CONFIG_SCD_LOGS_ENABLE=y")
        if logs.get("upload_interval_seconds"):
            lines.append(f"CONFIG_SCD_LOGS_UPLOAD_INTERVAL_S={int(logs['upload_interval_seconds'])}")
        if logs.get("buffer_kb"):
            lines.append(f"CONFIG_SCD_LOGS_BUFFER_KB={int(logs['buffer_kb'])}")
        lvl = str(logs.get("min_level", "")).upper()
        if lvl in ("V", "D", "I", "W", "E"):
            lines.append(f"CONFIG_SCD_LOGS_MIN_LEVEL_{lvl}=y")

    with open(out, "w") as f:
        if lines:
            f.write("# Generated from .scadable/config.yaml by libscadable\n")
            f.write("\n".join(lines) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
