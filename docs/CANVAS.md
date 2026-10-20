# Canvas (openclaw-cpp)

Local **HTML + JSON widget** documents on disk. Served at **`/canvas`**.

**Not** full upstream OpenClaw canvas / A2UI compositor.

## Storage

Default: `$OPENCLAW_SESSION_DIR/canvas/` (gateway) or `sim/canvas/` under SimulationBackend data dir.

- `index.json` — doc list + `active_id`
- `{id}.json` — document (`title`, `html`, `widgets`, `meta`, timestamps)
- `{id}.html` — HTML sibling for inspection
- `events.json` — capped A2UI-ish event log

## RPC

| Method | Role |
|--------|------|
| `canvas.status` | Store status |
| `canvas.list` | List documents |
| `canvas.create` | Create doc |
| `canvas.get` | Get by id (empty → active) |
| `canvas.patch` | Patch title/html/widgets |
| `canvas.delete` | Remove doc |
| `canvas.events` | Event log |
| `canvas.event` | Append event |
| `canvas.document.view` / `preview` / `put` | Active-doc helpers (via sim + store) |

`impl:real` for local disk state. Still **not** upstream A2UI / remote canvas host.

## UI

- Control home → **Canvas**
- `http://127.0.0.1:<port>/canvas` — list / edit HTML / event log (vanilla JS → `/v1/rpc`)
