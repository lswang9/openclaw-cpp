# Native Plugin SDK (Linux)

openclaw-cpp ships a **C ABI** header `include/openclaw/plugins/sdk.h` (ABI 1).

| Symbol | Role |
|--------|------|
| `openclaw_plugin_abi` | Must return `OPENCLAW_PLUGIN_ABI` (1) |
| `openclaw_plugin_name` | Static name string |
| `openclaw_plugin_init` / `shutdown` | Optional lifecycle |
| `openclaw_plugin_invoke` | `method` + JSON params → malloc'd JSON (caller `free`s) |

## Sample

`plugins/sample_echo/` builds `libopenclaw_sample_echo.so`. Manifest `plugin.json`:

```json
{ "name": "sample_echo", "enabled": true, "native": "libopenclaw_sample_echo.so", "abi": 1 }
```

Gateway `plugins.Scan` + `LoadNatives()` (`dlopen`). RPC: `plugins.invoke` with `{id, method, params}`.

## Gap vs OpenClaw 2026.10.5

Upstream plugins are primarily **JS/TS** with a rich host API. This C ABI is a **Linux-native** subset for tools/extensions — not JS plugin parity.


## JS plugins

See [JS-PLUGIN-HOST.md](JS-PLUGIN-HOST.md) — Node stdio bridge + `plugins/sample_js`. Not full upstream JS API.
