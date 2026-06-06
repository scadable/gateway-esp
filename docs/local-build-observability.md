# Local builds: applying `.scadable/config.yaml`

The SCADABLE **cloud** build pipeline translates your repo's
`.scadable/config.yaml` observability block (heartbeat / metrics / logs)
into `CONFIG_SCD_*` sdkconfig flags. A plain local **`idf.py build` does
not** — ESP-IDF has no idea `.scadable/` exists. So a locally-flashed
device silently ships with logs and opt-in metrics **compiled out**, and
nothing shows up in the dashboard even though `config.yaml` looks correct.

To make local builds match the cloud, run the same translation yourself
in your project's **top-level `CMakeLists.txt`, before `project()`**
(that's the only point early enough — IDF reads `SDKCONFIG_DEFAULTS` while
processing `project()`; a component can't inject them, it's already too
late).

## Setup

1. Copy `cmake/scadable_gen_defaults.py` from this component into your
   project (e.g. `cmake/scadable_gen_defaults.py`). It parses the
   `heartbeat` / `metrics` / `logs` blocks of `.scadable/config.yaml` into
   `CONFIG_SCD_*` lines — the exact same mapping the cloud build uses.

2. Add this above `include(... project.cmake)` in your top-level
   `CMakeLists.txt`:

   ```cmake
   set(_scd_cfg "${CMAKE_CURRENT_LIST_DIR}/.scadable/config.yaml")
   if(EXISTS "${_scd_cfg}")
     set(_scd_gen "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.scadable")
     execute_process(
       COMMAND python3 "${CMAKE_CURRENT_LIST_DIR}/cmake/scadable_gen_defaults.py"
               "${_scd_cfg}" "${_scd_gen}"
       RESULT_VARIABLE _scd_rc)
     if(_scd_rc EQUAL 0 AND EXISTS "${_scd_gen}")
       # Preserve IDF's implicit sdkconfig.defaults — otherwise listing
       # only the generated file makes IDF ignore your sdkconfig.defaults
       # entirely. Generated file goes first so explicit settings win.
       if(NOT SDKCONFIG_DEFAULTS)
         set(SDKCONFIG_DEFAULTS "sdkconfig.defaults")
       endif()
       list(PREPEND SDKCONFIG_DEFAULTS "${_scd_gen}")
     endif()
   endif()

   include($ENV{IDF_PATH}/tools/cmake/project.cmake)
   project(your_app)
   ```

3. Add `sdkconfig.scadable` to `.gitignore` (it's generated).

Now `idf.py reconfigure build` reads `.scadable/config.yaml` on every
build. `config.yaml` is the single source of truth; you don't hand-write
`CONFIG_SCD_LOGS_ENABLE` / `CONFIG_SCD_METRICS_*` anywhere. An explicit
value in your own `sdkconfig.defaults` still wins (it's applied after the
generated file), so you can override a single flag when you need to.

## What it sets

Same as the cloud translator:

| `.scadable/config.yaml` | sdkconfig |
|---|---|
| `heartbeat.interval_seconds` | `CONFIG_SCD_HEARTBEAT_INTERVAL_S` |
| `metrics.collect: [...]` | `CONFIG_SCD_METRICS_*=y` per metric |
| `logs.enabled: true` | `CONFIG_SCD_LOGS_ENABLE=y` (+ interval / buffer / min_level) |

Config *variables* (the `config:` block) are runtime-resolved from the
dashboard and are **not** part of this — they need no build flags beyond
`CONFIG_SCD_CONFIG_ENABLE` (on by default).

## Verify

```
idf.py reconfigure
grep CONFIG_SCD_ sdkconfig
```

You should see `CONFIG_SCD_LOGS_ENABLE=y` and your metrics. If not, check
that `python3` is on PATH and `.scadable/config.yaml` parses (the script
fails open — a bad file is skipped, not fatal).
