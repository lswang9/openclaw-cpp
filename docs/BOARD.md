# Control UI Board (openclaw-cpp)

Local **Kanban board** persisted as JSON. Served at **`/board`**. Not the full upstream OpenClaw board UX.

## Storage

Default path: `$OPENCLAW_SESSION_DIR/board.json` (fallback `/tmp/openclaw-cpp-sessions/board.json`).

Schema:

```json
{
  "version": 1,
  "columns": [{"id":"todo","title":"Todo","order":0}, ...],
  "cards": [{"id":"card-1","column_id":"todo","title":"...","body":"...","order":0,"updated_ms":0}]
}
```

## RPC

| Method | Role |
|--------|------|
| `board.get` / `board.data.read` / `board.status` | Read board |
| `board.update` | Replace whole board |
| `board.column.create` | Add column |
| `board.card.create` | Add card |
| `board.card.update` | Patch title/body/column |
| `board.card.move` | Move card to column |
| `board.card.delete` | Remove card |
| `board.action` | Thin alias (create/move/delete) |

`impl:real` for local disk state. Widget / prompt.authorize / appView remain **not implemented**.

## UI

- Control home: link **Board**
- `http://127.0.0.1:<port>/board` — create / move / delete cards (vanilla JS → `/v1/rpc`)
