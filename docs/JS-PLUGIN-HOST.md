# JS Plugin Host (openclaw-cpp)

Thin **Node.js stdio JSON-RPC bridge** for `.js` plugins. Complements the native `.so` C ABI (`docs/PLUGIN-SDK.md`).

**Not** the full OpenClaw 2026.10.5 JS/TS plugin API (no rich hooks, channel bindings, or Control UI extensions).

## Layout

| Path | Role |
|------|------|
| `plugins/js_bridge/host.mjs` | Node host process (newline JSON-RPC) |
| `plugins/sample_js/` | Demo plugin (`index.js` + `plugin.json`) |

`plugin.json` for JS plugins:

```json
{ "name": "sample_js", "enabled": true, "js": "index.js" }
```

Minimal plugin module:

```js
module.exports = {
  name: 'sample_js',
  tools: [{
    name: 'sample_echo',
    description: '...',
    parameters: { type: 'object', properties: { text: { type: 'string' } } },
    handler(args) { return { ok: true, content: 'js-echo:' + args.text }; },
  }],
};
```

Tools are exposed as `js.<name>` (e.g. `js.sample_echo`).

## Env

| Variable | Role |
|----------|------|
| `OPENCLAW_JS_FORCE_MOCK=1` | In-process mock (no Node). CI / offline. |

Live mode requires `node` on `PATH` and `plugins/js_bridge/host.mjs`.

## RPC

- `plugins.js.status` — host status (`impl: real` vs `real-sim` mock)
- `plugins.js.start` — (re)start bridge + register tools
- `plugins.js.list_tools` — tools from JS plugins
- `plugins.js.call` — `{ name, arguments }`

Gateway auto-starts the host at boot and registers `js.*` into the tools registry.

## Protocol (stdio)

Newline-delimited JSON-RPC 2.0: `initialize`, `list_tools`, `call_tool`, `list_plugins`, `shutdown`, `ping`.

## Gaps vs upstream OpenClaw

- No TypeScript / bundler / full plugin SDK
- No lifecycle hooks, HTTP routes, or channel adapters from JS
- No sandboxed QuickJS embed (Node external process only)
- Mock mode mirrors `sample_js` only
