/*
 * log_sink.c - see log_sink.h for design notes.
 *
 * Ported from firmware/main/log_sink.c. Dependencies on cJSON dropped
 * in favour of hand-rolled JSON to match the rest of libscadable.
 */
#include "log_sink.h"

#include "sdkconfig.h"

#if !CONFIG_SCD_LOGS_ENABLE

/* Subsystem disabled - stub out both entry points so callers compile
 * regardless of the flag. */
void scd_log_sink_install(void)            { /* noop */ }
void scd_log_sink_start_flush_task(void)   { /* noop */ }

#else

#include "scadable_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG_INTERNAL = "scd.log";

#define RING_SIZE             (CONFIG_SCD_LOGS_BUFFER_KB * 1024)
#define FLUSH_HIGH_WATERMARK  ((RING_SIZE * 3) / 4)
#define FLUSH_INTERVAL_MS     (CONFIG_SCD_LOGS_UPLOAD_INTERVAL_S * 1000)

/* Compile-time decision: minimum char that we capture. Drops V/D from
 * the upload batch when min_level >= I; gives back ~30% of the ring
 * for I/W/E-only fleets. UART forwarding (s_prev_vprintf) still gets
 * everything regardless - the user can still see V/D on serial. */
#if   CONFIG_SCD_LOGS_MIN_LEVEL_V
#define MIN_LEVEL_CHAR 'V'
#elif CONFIG_SCD_LOGS_MIN_LEVEL_D
#define MIN_LEVEL_CHAR 'D'
#elif CONFIG_SCD_LOGS_MIN_LEVEL_W
#define MIN_LEVEL_CHAR 'W'
#elif CONFIG_SCD_LOGS_MIN_LEVEL_E
#define MIN_LEVEL_CHAR 'E'
#else
#define MIN_LEVEL_CHAR 'I'
#endif

static char              s_ring[RING_SIZE];
static size_t            s_ring_head     = 0;
static SemaphoreHandle_t s_ring_mux      = NULL;
static SemaphoreHandle_t s_flush_signal  = NULL;
static vprintf_like_t    s_prev_vprintf  = NULL;
static volatile bool     s_in_sink       = false;

static bool should_capture(char level) {
    /* V < D < I < W < E - capture if level >= MIN_LEVEL_CHAR */
    static const char order[] = "VDIWE";
    const char *p_lvl = strchr(order, level);
    const char *p_min = strchr(order, MIN_LEVEL_CHAR);
    if (!p_lvl) return true; /* unknown levels go through */
    if (!p_min) return true;
    return (p_lvl - order) >= (p_min - order);
}

static int sink_vprintf(const char *fmt, va_list ap) {
    va_list ap_copy;
    va_copy(ap_copy, ap);
    int forwarded = s_prev_vprintf ? s_prev_vprintf(fmt, ap_copy) : 0;
    va_end(ap_copy);

    if (s_in_sink) return forwarded;
    s_in_sink = true;

    char line[512];
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    if (n > 0 && n < (int)sizeof(line)) {
        char level = line[0];
        if (should_capture(level) && xSemaphoreTake(s_ring_mux, pdMS_TO_TICKS(50)) == pdTRUE) {
            size_t free_space = RING_SIZE - s_ring_head;
            if ((size_t)n > free_space) n = (int)free_space;
            if (n > 0) {
                memcpy(s_ring + s_ring_head, line, n);
                s_ring_head += n;
            }
            bool need_flush = s_ring_head >= FLUSH_HIGH_WATERMARK;
            xSemaphoreGive(s_ring_mux);
            if (need_flush && s_flush_signal) xSemaphoreGive(s_flush_signal);
        }
    }

    s_in_sink = false;
    return forwarded;
}

void scd_log_sink_install(void) {
    s_ring_mux     = xSemaphoreCreateMutex();
    s_flush_signal = xSemaphoreCreateBinary();
    s_prev_vprintf = esp_log_set_vprintf(sink_vprintf);
}

/* Quick JSON string escape - handles ", \, control chars. Returns
 * bytes written excluding NUL, or -1 if it would have overflowed. */
static int json_escape(char *dst, int dst_len, const char *src, int src_len) {
    int o = 0;
    for (int i = 0; i < src_len; i++) {
        unsigned char c = (unsigned char)src[i];
        if (o + 6 >= dst_len) return -1; /* worst-case \uXXXX */
        if (c == '"')      { dst[o++] = '\\'; dst[o++] = '"'; }
        else if (c == '\\'){ dst[o++] = '\\'; dst[o++] = '\\'; }
        else if (c == '\n'){ dst[o++] = '\\'; dst[o++] = 'n';  }
        else if (c == '\r'){ dst[o++] = '\\'; dst[o++] = 'r';  }
        else if (c == '\t'){ dst[o++] = '\\'; dst[o++] = 't';  }
        else if (c < 0x20) { o += snprintf(dst + o, dst_len - o, "\\u%04x", c); }
        else               { dst[o++] = (char)c; }
    }
    dst[o] = '\0';
    return o;
}

/* Parse one line of the form "X (12345) tag: message" and append a
 * JSON object to out at position p. Returns new p, or -1 on overflow. */
static int append_log_entry(char *out, int out_len, int p, const char *line, size_t len) {
    if (len == 0) return p;
    char level = line[0];

    const char *tag_start = NULL; size_t tag_len = 0;
    const char *msg_start = NULL; size_t msg_len = 0;
    long ts_ms = 0;
    bool parsed = false;

    if (len > 4 && line[1] == ' ' && line[2] == '(') {
        const char *paren_close = memchr(line + 3, ')', len - 3);
        if (paren_close) {
            ts_ms = strtol(line + 3, NULL, 10);
            const char *p_ = paren_close + 1;
            size_t rem = len - (p_ - line);
            if (rem > 0 && *p_ == ' ') { p_++; rem--; }
            const char *colon = memchr(p_, ':', rem);
            if (colon) {
                tag_start = p_; tag_len = colon - p_;
                msg_start = colon + 1;
                msg_len   = rem - (msg_start - p_);
                while (msg_len > 0 && (*msg_start == ' ' || *msg_start == '\t')) { msg_start++; msg_len--; }
                parsed = true;
            }
        }
    }

    /* {"level":"X","tag":"...","message":"...","uptime_ms":N} */
    int n = snprintf(out + p, out_len - p, "%s{\"level\":\"%c\",\"tag\":\"",
                     (p > 1 ? "," : ""), parsed ? level : 'I');
    if (n < 0 || n >= out_len - p) return -1;
    p += n;

    if (parsed) {
        int e = json_escape(out + p, out_len - p, tag_start, (int)tag_len);
        if (e < 0) return -1;
        p += e;
    } else {
        n = snprintf(out + p, out_len - p, "raw");
        if (n < 0 || n >= out_len - p) return -1;
        p += n;
    }

    n = snprintf(out + p, out_len - p, "\",\"message\":\"");
    if (n < 0 || n >= out_len - p) return -1;
    p += n;

    {
        int e = json_escape(out + p, out_len - p,
                            parsed ? msg_start : line,
                            parsed ? (int)msg_len : (int)len);
        if (e < 0) return -1;
        p += e;
    }

    n = snprintf(out + p, out_len - p, "\",\"uptime_ms\":%ld}",
                 parsed ? ts_ms : (long)(esp_timer_get_time() / 1000));
    if (n < 0 || n >= out_len - p) return -1;
    p += n;
    return p;
}

static void flush_task(void *arg) {
    (void)arg;
    static char snapshot[RING_SIZE];
    static char payload[RING_SIZE * 2 + 256]; /* JSON-escaped grows up to 6x; assume 2x typical */
    char topic[96];

    /* Build topic once - identity is stable. */
    const scd_identity_t *id = scd_get_identity();
    if (!id) { vTaskDelete(NULL); return; }
    snprintf(topic, sizeof(topic), "scadable/%s/logs", id->common_name);

    while (1) {
        xSemaphoreTake(s_flush_signal, pdMS_TO_TICKS(FLUSH_INTERVAL_MS));

        if (!scd_mqtt_is_connected()) continue;

        size_t snap_len = 0;
        if (xSemaphoreTake(s_ring_mux, pdMS_TO_TICKS(200)) == pdTRUE) {
            snap_len = s_ring_head;
            if (snap_len > 0) {
                memcpy(snapshot, s_ring, snap_len);
                s_ring_head = 0;
            }
            xSemaphoreGive(s_ring_mux);
        }
        if (snap_len == 0) continue;

        /* Build {"device_id":"...","logs":[...]} */
        int p = snprintf(payload, sizeof(payload),
                         "{\"device_id\":\"%s\",\"logs\":[",
                         id->common_name);
        if (p < 0) continue;

        const char *cursor = snapshot;
        size_t left = snap_len;
        while (left > 0) {
            const char *nl = memchr(cursor, '\n', left);
            size_t line_len = nl ? (size_t)(nl - cursor) : left;
            while (line_len > 0 && cursor[line_len - 1] == '\r') line_len--;
            if (line_len > 0) {
                int np = append_log_entry(payload, sizeof(payload), p, cursor, line_len);
                if (np < 0) break; /* overflow - drop remainder this round */
                p = np;
            }
            if (!nl) break;
            size_t advance = (size_t)(nl - cursor) + 1;
            cursor += advance;
            left   -= advance;
        }

        if (p + 2 < (int)sizeof(payload)) {
            payload[p++] = ']';
            payload[p++] = '}';
            payload[p]   = '\0';
            (void)scd_mqtt_publish_raw(topic, payload, p, 0 /* QoS 0 */, false);
        }
    }
}

void scd_log_sink_start_flush_task(void) {
    xTaskCreate(flush_task, "scd_log_flush", 6144, NULL, 3, NULL);
}

#endif /* CONFIG_SCD_LOGS_ENABLE */
