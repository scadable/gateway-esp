# File uploads

`libscadable` v0.2.0 adds an opt-in streaming upload API. Devices can
PUT files of any size to `uploads.scadable.com`, the server stores them
in Valkey (later S3), and the dashboard lists them per organization.

## Opt-in

The feature is gated by a Kconfig flag, **off by default**:

```
idf.py menuconfig
  → Component config
    → SCADABLE Gateway
      → [*] Enable file upload feature
```

(or in `sdkconfig.defaults`: `CONFIG_SCD_UPLOAD_ENABLE=y`)

When off: the four `scadable_upload_*` symbols don't ship in your
binary, and `src/upload.c` collapses to an empty translation unit. Zero
code, zero RAM, zero TLS handshakes. You opt in only if you need it.

## Public API

```c
#include "scadable.h"

#if defined(CONFIG_SCD_UPLOAD_ENABLE)

typedef struct scd_upload* scd_upload_handle_t;

esp_err_t scadable_upload_begin(const char *filename,
                                const char *content_type,
                                scd_upload_handle_t *out);

esp_err_t scadable_upload_chunk(scd_upload_handle_t h,
                                const void *bytes, size_t len);

esp_err_t scadable_upload_end  (scd_upload_handle_t h,
                                char *file_id_out, size_t file_id_max);

void scadable_upload_abort(scd_upload_handle_t h);

#endif
```

Pattern is begin → chunk × N → end. On any error from `chunk`, call
`abort` to free the handle. `end` finalizes the request and parses the
server-assigned `file_id` out of the response.

## Customer code (minimal)

```c
#include "scadable.h"

void upload_crashdump(void) {
    scd_upload_handle_t up;
    if (scadable_upload_begin("crash-0001.bin",
                              "application/octet-stream",
                              &up) != ESP_OK) return;

    uint8_t buf[4096];
    size_t  n;
    while (read_crashdump_chunk(buf, sizeof(buf), &n)) {
        if (scadable_upload_chunk(up, buf, n) != ESP_OK) {
            scadable_upload_abort(up);
            return;
        }
    }

    char file_id[64];
    if (scadable_upload_end(up, file_id, sizeof(file_id)) == ESP_OK) {
        ESP_LOGI("crash", "uploaded as %s", file_id);
    }
}
```

You can also do a one-shot upload of an in-memory buffer:

```c
scd_upload_handle_t up;
scadable_upload_begin("hello.txt", "text/plain", &up);
scadable_upload_chunk(up, "hello world", 11);
scadable_upload_end(up, NULL, 0);
```

## Auth

The library reads the device certificate's Common Name from NVS at boot
and sends it as `X-Device-CN: SC-<device_id>` on every upload. The
server resolves the CN through the provisioning record to find the
device's namespace and org, and stamps all three on the stored
metadata. Devices that aren't provisioned get a `403`; devices outside
an org get a `403` ("device has no namespace; cannot resolve org").

You don't pass org or namespace IDs from firmware — they're derived
server-side from the cert. That's the whole identity story for uploads.

## Wire protocol (for reference / testing without the library)

```
PUT https://uploads.scadable.com/v1/upload HTTP/1.1
X-Device-CN:       SC-a3f8b2c4e6d8
X-Filename:        crash-0001.bin
X-Content-Type:    application/octet-stream
Transfer-Encoding: chunked

<chunked body>

HTTP/1.1 200 OK
Content-Type: application/json

{
  "file_id":      "f_abc123...",
  "org_id":       "org_...",
  "namespace_id": "ns_...",
  "device_cn":    "SC-a3f8b2c4e6d8",
  "filename":     "crash-0001.bin",
  "content_type": "application/octet-stream",
  "uploaded_at":  1748678400,
  "size_bytes":   524288,
  "sha256":       "..."
}
```

Equivalent curl:

```bash
curl -i -X PUT \
  -H "X-Device-CN: SC-<your_device_id>" \
  -H "X-Filename: crash-0001.bin" \
  -H "X-Content-Type: application/octet-stream" \
  --data-binary @./crash-0001.bin \
  https://uploads.scadable.com/v1/upload
```

## Limits (v0.2.0)

| Limit | Default | Override |
|---|---|---|
| Per-request body size | 50 MB | `UPLOAD_MAX_BYTES` env (backend) |
| Cloudflare edge cap | 100 MB | n/a — grey-cloud DNS-only |
| Per-org storage quota | 500 MB | `UPLOAD_ORG_QUOTA_BYTES` env (backend) |
| Per-upload HTTP timeout | 60 s | `CONFIG_SCD_UPLOAD_TIMEOUT_MS` (Kconfig) |

Exceeding the per-request limit → `413 Payload Too Large`.
Exceeding the per-org quota → `507 Insufficient Storage`.

## Storage today, S3 tomorrow

v0.2.0 stores file bytes directly in Valkey (`scadable:upload:<id>:bytes`).
That's fine for the size range we're in. v0.3.0+ swaps to S3 — the
device API doesn't change; only the backend implementation behind
`uploads.scadable.com/v1/upload` does.

When the swap happens, customers running v0.2.0 continue working without
re-flashing.

## Listing what got uploaded (dashboard-side)

```
GET /api/orgs/{org_id}/uploads?limit=50&from=<unix>&to=<unix>&ns=<ns_id>&device=<cn>
```

Newest-first metadata list. Excludes the bytes (use the planned
`GET /api/orgs/{org_id}/uploads/{file_id}/download` in v0.3.0 to fetch
the actual content).

```
GET /api/orgs/{org_id}/uploads/stats
```

Returns `{ count, bytes_used, bytes_quota, last_upload_at }` for a
quick dashboard tile.

## Memory cost on the device

When `CONFIG_SCD_UPLOAD_ENABLE=y`:

| Item | RAM |
|---|---|
| Resident library code | ~3 KB additional |
| Per-active-upload handle | ~100 bytes (`struct scd_upload`) + esp_http_client internals (~4 KB while open) |
| TLS session (shared with edge.scadable.com TLS) | ~14 KB (no extra cost since TLS is already up for edge calls) |

You only pay the per-upload cost while an upload is in flight; closing
the handle returns it.

## What's NOT in v0.2.0

- **Download/fetch from device.** The upload is one-way (device → cloud).
- **Resumable uploads.** A drop mid-upload requires starting over.
- **Multiple concurrent uploads per device.** Pattern is begin/chunk/end serially.
- **Server-side compression.** Bytes are stored as-is.

All of those are candidates for v0.3.0+ if real use justifies them.
