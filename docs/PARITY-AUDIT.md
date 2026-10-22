# Parity audit — openclaw-cpp vs OpenClaw v2026.10.5

Audited 2026-10-22, at openclaw-cpp 1.125.0. Upstream was re-checked: `npm view openclaw version` still returns
**2026.10.5**, so no retarget was needed. Upstream source is the v2026.10.5 tree.

This audit tries to be honest. "Answers" means the command or method responds without "not implemented". It does
**not** mean the behaviour matches upstream end to end. Each section below separates four kinds of thing:
- what is real;
- what answers but is a simulation or partial;
- what is missing;
- what only live credentials can verify.

Reproduce the numbers with:
- `python3 /tmp/climiss.py` (CLI);
- `scripts/probe-unimplemented.py` and `scripts/parity-audit-rpc.py <rpc-url> out.json` (RPC);
- the e2e suites wired into `scripts/integration-smoke.sh`.

## Summary counts

| Area | Upstream | C++ answers | Real (offline-verified) | Partial / sim | Missing |
|---|---|---|---|---|---|
| CLI command groups / subcommands (upstream tree) | 45 / 131 | 45 / 131 | most; e2e for every 1.12x addition | see §1 | 0 |
| Gateway RPC methods (catalog) | 493 | 493 (0 unimplemented) | 267 ok on `{}` + 185 param-validating handlers | 1 real-sim (`computer.invoke`), 31 precondition-only (need node/creds/session) | 0 |
| Agent tools (core) | ~57 | 53 native (+JS/MCP bridged) | 53 (7 added in 1.128) | browser/desktop are local-runtime variants; view_image/web_search partial | ~12 (§3) |
| Config root keys | 46 | 46 accepted | ~20 deeply validated | 26 shallow (key accepted, sub-shape not validated) | 0 |
| Channel extensions | 27 messaging | 11 working | telegram (1.126), discord, slack, feishu, whatsapp (Cloud API), imessage (bridge inbound); signal, matrix, msteams, googlechat, mattermost (1.129, text) | whatsapp-web (partial Noise/Baileys); 1.129 channels lack live listeners/inbound signature checks | 16 (§5) |
| Model-provider extensions | ~85 | 28 built-in ids + any `models.providers.*` | 4 wire formats (chat completions, Responses, Anthropic Messages, Gemini) offline-tested | token streaming over SSE for all four formats (1.127); no OAuth/Bedrock/Vertex signing | Bedrock, Vertex, Copilot, Codex OAuth, … |
| Plugin API (`api.register*`) | 60 | 60 accepted (JS host) + C ABI | 12 consumed: Tool/Provider/GatewayMethod/HttpRoute/Hook/Command/Cli/Service/Channel + WebSearchProvider/TextTransforms/AgentToolResultMiddleware (1.130) | Channel/Service thin; 48 captured but not consumed | 0 rejected; 48 unconsumed (§6) |

## 1. CLI tree

The upstream tree (`src/cli/upstream_cli_tree.inc`, 45 groups and 131 subcommands) now fully answers. `climiss` went
from 27 to 0 in 1.124. Gateway-backed subcommands route to the matching RPC, with upstream flag validation and
camelCase params. Every command prints help on `--help`.

**Real, with an e2e test** (`e2e-cli-local*.sh`, `e2e-cli-routed.sh`):
- connect: setup code, `oc-pair://` and `/j/` join URL → pairing.link.claim → `node run`, or `--service` to install a
  systemd unit
- mcp list/show/add/set/unset/tools/status/probe/doctor/login/logout/serve/configure
- acp client: drives `acp` over stdio and handles permission prompts
- promos list/claim (ClawHub API)
- sandbox list/recreate (docker labels)
- dns setup (report only)
- secrets audit/apply/configure (file provider, 0600)
- backup sqlite (sqlite3 backup API + manifest + integrity check) and backup git (deterministic SQL dumps)
- webhooks gmail setup/run (gcloud + gog)
- node install
- hooks relay/install/update
- agents team create
- claws dev/build/migrate/update
- devices clear
- agent exec (isolated gateway child)
- reset, uninstall, docs, proxy, database, directory, update wizard, qr, etc.

**Answers, but partial:**
- `dns setup --apply` → "supported on macOS only". This matches upstream on Linux.
- `webhooks gmail run` needs the `gog` binary.
- `sandbox` lists only docker containers. bwrap sandboxes are per-command and leave nothing running.
- `backup sqlite/git` cover the SQLite files the port has (exec approvals, memory). Most C++ state is JSON; see §7.
- `update` cannot self-update an npm install. It manages the C++ binary layout.

## 2. Gateway RPC

There are 493 upstream methods; probing each with `{}` params gives:

| Result | Count |
|---|---|
| Answers ok | 267 |
| Rejects with a validation error (handler exists) | 185 |
| Precondition errors | 31 |
| `real-sim` | 1 |
| Skipped by the probe as destructive (restart/stop/update) | 9 |
| Unimplemented | 0 |

The 31 precondition errors need a connected node, a live talk or MCP-app session, a board ticket, GitHub OAuth config, a
web-push subscription, a WhatsApp QR-login plugin, or a search API key. The handlers exist, but **behaviour behind them
is only as real as the subsystem**. Notable caveats:
- **`computer.invoke`** is `real-sim`: a simulated desktop unless a node provides one.
- **`talk.*` / realtime voice:** the session plumbing exists. There is no realtime model provider.
- **`web.login.*`:** needs a channel plugin that provides QR login. WhatsApp-Web is partial.
- **`usage.*`:** token counts come from a local tokenizer over transcripts. Cost is computed only when
  `models.providers.*.models[].cost` is configured.
- **`webSearch.*`:** Brave/Tavily only.

Fixed during this audit: `usage.status`, `usage.cost`, `sessions.usage.logs`, `sessions.catalog.list` (they crashed on
sessions with no index entry) and `agents.files.get` (it read a directory).

## 3. Agent tools

**Native (53):**
- agents_list, automations, acp_agent, browser(+evaluate/open_url/screenshot), canvas
- create_goal/get_goal/update_goal, desktop, discord_voice, docs_search, echo, edit, exec, process, shell, read, write
- github_publish, http_get, image, image_generate, music_generate, video_generate
- memory_get/search/set, message, nodes, pdf, presence, reminder, secrets, session_status
- sessions_history/list/search/send/spawn/yield, skills_read/search, subagents, time_now, transcripts, tts, web_fetch
- 1.128: web_search (Brave/Tavily), ask_user (question.request/get/list/resolve/waitAnswer RPCs; prompt mirrored to the session's channel; next inbound message answers), view_image (png/jpeg/gif/webp metadata + data URL), agents_wait, conversations_list/send/turn (conversationRef = conv_<32 hex>, recorded from inbound channel messages; turn replies are consumed instead of starting an agent turn)

There are also JS-plugin tools and MCP-bridged tools.

**Missing compared with upstream core tools:**
- computer (only `desktop` exists)
- dashboard, decision_evaluate, gateway, github_identity_status, mobile_ui
- personal_instructions, plugins, portal, progress_card, screen, skill_workshop, terminal
- Partial: view_image returns the image as a data URL in the tool result text; it is not attached as a native vision content block. web_search supports Brave and Tavily only (upstream also has Perplexity/Gemini/Grok/etc.). ask_user has no interactive buttons (options are a numbered list).

## 4. Config schema

All 46 upstream root keys are accepted. In 1.125, 14 keys were added:
- accessGroups, attachments, cloudWorkers, desktop, proxy, security, storage
- surfaces, telemetry, transcripts, tts, worktree*

Deep validation covers:
- gateway.*, agents.defaults.*
- session.reset*, skills.entries.*
- SecretRefs (env/file/exec, with allowlists)

Other subtrees are accepted without sub-shape validation. Upstream is a strict zod schema, so typos inside those
subtrees warn upstream but pass here. SecretRefs are resolved at load time into typed config, and now also by the
provider factory. Some raw-config consumers still read raw values.

## 5. Channels / extensions

**Working:**
- **Discord:** webhook, Gateway WS, slash commands, voice/DAVE.
- **Slack:** Events API, Socket Mode, interactive.
- **Feishu:** webhook and long-connection WS.
- **WhatsApp:** Cloud API in/out. WhatsApp-Web (Baileys-equivalent) is partial; see `docs/whatsapp-web-gap.md`.
- **iMessage:** bridge inbound.
- **Generic:** webhook, HTTP, WS, CLI.

**Telegram (1.126, real):** getUpdates long polling (deleteWebhook first, offset persisted across restarts, backoff, 409 conflict logged) or webhook inbound with secret header; replies chunked at 4096 chars, group replies use `reply_parameters`, forum topics keep `message_thread_id`. Offline e2e: `scripts/e2e-telegram.sh`. Not yet: media send (except voice), inline keyboards/callback actions, reactions, native commands menu.

**Signal, Matrix, MS Teams, Google Chat, Mattermost (1.129, text-only transports):** offline e2e `scripts/e2e-ext-channels.sh` against `scripts/fake_chatops.py`.
- Signal: signal-cli REST `POST /v2/send` (DM + `group.<id>`); inbound receive envelopes are pushed to `/api/channels/signal/inbound`. Not yet: polling `/v1/receive` or the JSON-RPC/SSE daemon, attachments, reactions.
- Matrix: `PUT rooms/{room}/send/m.room.message/{txn}` with thread (`m.thread`) replies; inbound app-service transactions or single events; `dmRooms` marks 1:1 rooms. Not yet: `/sync` long polling, E2EE, invites/auto-join.
- MS Teams: Bot Framework client-credentials token (cached) and `POST {serviceUrl}/v3/conversations/{id}/activities[/replyToId]`; `<at>` mentions stripped. Not yet: inbound JWT validation, Adaptive Cards, files, proactive install flows.
- Google Chat: synchronous reply in the event response, or `spaces.messages.create` (thread reply) when `accessToken` is configured. Not yet: service-account JWT minting, inbound bearer verification, cards.
- Mattermost: `POST /api/v4/posts` (thread `root_id`); inbound outgoing webhooks verified by `webhookToken`. Not yet: WebSocket events, DMs via WebSocket, slash commands.
- All five record a conversationRef, so conversations_* and ask_user work over them. Idle adapters now pick up hot-reloaded config.

**Missing channel extensions (16):**
- a2a, buzz, clickclack, irc, line, nextcloud-talk, nostr
- raft, reef, sms, synology-chat, tlon, twitch, x, zalo, zalouser

**Other extension families with no or only partial C++ counterpart:**
- memory-lancedb, memory-wiki
- diagnostics-otel/prometheus
- voice-call, google-meet/teams/zoom meetings, slack-huddles
- image/video/music provider packs (only generic generate tools)
- onepassword/vault secret backends
- tokenjuice, lobster, workboard, etc.

## 6. Plugin API

The JS host (`plugins/js_bridge/host.mjs`) accepts all 60 upstream `api.register*` entry points (1.130; recount — the earlier audit said 59). A plugin that calls any of them keeps loading; before 1.130 an unimplemented call threw and the plugin failed.

**Consumed by the C++ runtime (12 upstream + the non-upstream registerHttpHandler):**
- registerTool, registerProvider, registerGatewayMethod, registerHttpRoute, registerHttpHandler
- registerHook, registerCommand, registerCli, registerService, registerChannel (thin)
- 1.130: registerWebSearchProvider (feeds `web_search` when no built-in Brave/Tavily key, or by provider id), registerTextTransforms (input rewrites on the newest user message before the provider call; output rewrites on the final assistant text — already-streamed deltas are not rewritten), registerAgentToolResultMiddleware (string/array/RegExp/function matcher; rewrites tool results before the model sees them)

**Captured only (48):** recorded and counted in the host's `initialize`/`list_registrations` answer, but nothing in the C++ runtime calls them yet:
- the agent harness/executor/event-subscription APIs
- the context engine, compaction, embedding, memory capability/corpus/prompt APIs
- the speech/realtime/transcription/media-understanding/image/video/music generation providers
- web fetch, storage, session catalog/extension/action/scheduler, config migration, Control UI descriptors, widget presenters
- security audit collectors, trusted tool policy, node host commands/invoke policy, worker providers, gateway access policy/discovery, etc.

Offline e2e: `scripts/e2e-plugin-registrations.sh`.

The native C ABI (`docs/PLUGIN-SDK.md`) is a Linux-only subset with no upstream equivalent.

## 7. Behaviour

**Real:**
- the agent loop with tools, sub-agents, compaction, session reset policies, exec approvals (SQLite)
- sandboxing (bwrap/docker, labelled)
- pairing (device links, setup codes in upstream format since 1.124), OIDC/cookie auth
- cron, skills, MCP client (stdio + streamable-http + OAuth), ACP server/client
- **providers routed per `provider/model`** (1.125): config `models.providers.*` plus 28 built-in ids
  (openai→Responses, anthropic→Messages, google→Gemini, and OpenAI-compatible for the rest)
- `agents.defaults.model.primary` as the default route, per-agent `model`
- local `chat` uses the same factory
- **webhook ingress** (1.125): `/hooks/wake`, `/hooks/agent` (async 202 or `waitForCompletion`), mappings with
  templates, the `gmail` preset with forEach, token header rules, body limit, session-key prefix policy

**Partial:**
- **State is JSON files**, where upstream uses SQLite for sessions, transcripts and most stores. Behaviour matches,
  but the on-disk format and `database`/`storage` semantics differ.
- **Streaming** (1.127): token-level SSE for chat completions, Anthropic Messages (text + tool_use input_json deltas), Gemini `streamGenerateContent?alt=sse` and Responses (`output_text.delta`, `output_item.done`), offline-tested. Reasoning/thinking blocks are dropped, not surfaced.
- **Hooks:** there is no idempotency replay cache across restarts, no per-mapping `transform` modules, and
  `next-heartbeat` wakes queue to a file that heartbeat consumes.
- **Control UI:** the bundled UI is served. Plugin UI descriptors are not.

## 8. What only live credentials can verify

All provider calls against the real Anthropic, OpenAI, Gemini, DeepSeek and OpenRouter APIs are tested only against
offline fakes that copy the documented wire formats. The same applies to:
- Discord, Slack, Feishu and WhatsApp Cloud live delivery
- Gmail Pub/Sub push (gcloud/gog)
- ClawHub production (promos, packages)
- GitHub OAuth / publish
- OIDC IdPs
- Brave/Tavily search, ElevenLabs/TTS
- Tailscale wide-area DNS
- docker on a real host
- systemd user units (`OPENCLAW_SKIP_SYSTEMCTL` is used in tests)

## 9. Largest remaining gaps (priority order)

1. ~~Telegram live Bot API~~ (done in 1.126; media/keyboards remain).
2. ~~Signal, Matrix, MS Teams, Google Chat, Mattermost~~ (text transports in 1.129; live listeners/verification listed in §5).
3. ~~Native streaming~~ (done in 1.127); Bedrock/Vertex auth remain.
4. ~~Core tools web_search, ask_user, view_image, agents_wait, conversations_*~~ (done in 1.128, with the partials in §3).
5. Plugin API: all register* accepted (1.130); still unconsumed: context engine, memory capability, media/speech providers, harness APIs.
6. SQLite-backed session and transcript storage, for on-disk parity.

## 10. Test-suite notes

The smoke step "device tools.list allowed" failed intermittently. Root cause: the smoke's second gateway shared the
default data dir (`~/.openclaw-cpp/data`), whose device-link store persists a per-IP claim rate limit (30/min). Earlier
runs in the same minute (and `connect`, which polled `pairing.link.claim` every 500 ms while pending) used up the
budget, so the device-token claim was rate-limited and the token came back empty. Fixed in 1.126: the smoke gateway uses
its own data dir, polls with the correct secret on a still-pending link no longer consume the budget (bad secrets
still do), and `connect` polls once a second.

- 1.130: the smoke `tools.list` checks piped large RPC output into `grep -q` under `pipefail`; once the tool list grew, the writer occasionally got SIGPIPE (rc 141). They now use a here-string / `grep >/dev/null`.
