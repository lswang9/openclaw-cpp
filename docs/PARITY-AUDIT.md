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
| Agent tools (core) | ~57 | 66 native (+JS/MCP bridged) | 66 (13 added in 1.134) | computer/mobile_ui need a node implementing the commands; web_search partial | 0 by name (§3) |
| Config root keys | 46 | 46 accepted | ~20 deeply validated | 26 shallow (key accepted, sub-shape not validated) | 0 |
| Channel extensions | 27 messaging | 27 answer (text) | telegram (1.126), discord, slack, feishu, whatsapp (Cloud API), imessage (bridge inbound); signal, matrix, msteams, googlechat, mattermost (1.129); 16 misc (1.138) | whatsapp-web (partial Noise/Baileys); many channels text-only (no live WS/daemon) | 0 by name (§5) |
| Model-provider extensions | ~85 | 31 built-in ids + any `models.providers.*` | 7 wire formats (chat completions, Responses, Anthropic Messages, Gemini, Bedrock Converse, Anthropic Vertex, Google Vertex) offline-tested | token streaming over SSE; Bedrock AWS SigV4 / bearer + Vertex Google ADC (1.137) | Copilot, Codex OAuth, Mantle, … |
| Plugin API (`api.register*`) | 60 | 60 accepted (JS host) + C ABI | 12 deeply consumed (1.130) + decision/web_fetch/media/reload/lifecycle hooks (1.139); all 48 formerly inert kinds invocable via `plugins.invokeRegistration` | Channel/Service thin; many kinds still need domain wiring beyond generic invoke | 0 rejected (§6) |

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

**1.134: the remaining upstream core tools** (src/gateway/core_tools.cpp, e2e `scripts/e2e-core-tools.sh`):
- gateway (config.get with path, config.schema.lookup, update.run gated to owner sessions; update.run itself still answers UNAVAILABLE in this runtime)
- plugins (list/inspect/search/install/enable/disable/uninstall/reload/reloadAndResume over plugins.* RPCs)
- progress_card (progressCard.put), screen (ui.command mapping), portal (portal.open/list/close; owner sessions only)
- terminal (list/read/resize/close/input on operator-opened terminals scoped to the session; input requires `tools.exec.security: "full"` because there is no interactive approval route for shared-terminal input yet)
- dashboard (per-session tabs + plugin widgets with read/tab_*/widget_*/focus_tab/presentation; stored under `<state>/dashboards/`, widgets mirrored to board.widget.put, `board.changed`/`board.command` events)
- computer and mobile_ui forward to a connected node declaring `computer.act` / `mobile.ui.observe|act` (screenshots become image input); `node run` only declares them with the `OPENCLAW_NODE_SIMULATE_UI=1` test fixture — no real Linux input injection or Android bridge
- decision_evaluate (boolean/choice/score questions to `agents.defaults.decisionModel` or a per-agent `decisionModel`; output normalized to probabilities; unavailable when unset; no provider capability manifests)
- personal_instructions (USER.md get/set with sha256 expectedHash), github_identity_status (tools.github.status + reconnect hint)
- skill_workshop (list/view/create/patch/write_file/remove_file/archive/restore with per-change version snapshots under `<state>/agents/<id>/agent/workshop-skills`, loaded as a "workshop" skill root; no personal-library target)
- Owner detection: sessions that did not come from an external channel are treated as owner; channel sessions need `commands.ownerAllowFrom`.

- 1.133: view_image (and any tool result carrying a `data:image/*` `dataUrl`) is fed back as real image input: the base64 is stripped from the tool-result text (`imageAttached: true`) and a user message with the image follows the tool results, serialized as an Anthropic `image` block, Gemini `inlineData`, Responses `input_image` or chat-completions `image_url` (e2e `scripts/e2e-view-image.sh`). Not yet: per-model `input` capability gating / routing to `imageModel` when the primary lacks vision, remote URL fetch, and resizing large images.
- Partial: web_search supports Brave and Tavily only (upstream also has Perplexity/Gemini/Grok/etc.). ask_user has no interactive buttons (options are a numbered list).

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

**Telegram (1.126, real):** getUpdates long polling (deleteWebhook first, offset persisted across restarts, backoff, 409 conflict logged) or webhook inbound with secret header; replies chunked at 4096 chars, group replies use `reply_parameters`, forum topics keep `message_thread_id`. Offline e2e: `scripts/e2e-telegram.sh`. **1.135:** outbound media (sendPhoto/Animation/Video/Voice/Audio/Document; local files multipart, URLs by reference; `MEDIA:` reply directives), inline keyboards from `buttons` (message tool + ask_user options), `callback_query` → answerCallbackQuery + user turn, inbound photo/document/video/animation/sticker downloaded via getFile (images reach the model as image input, other files as attachment notes). Offline e2e: `scripts/e2e-telegram-media.sh`. Not yet: reactions, native commands menu (setMyCommands), message edits/streaming edits.

**Signal, Matrix, MS Teams, Google Chat, Mattermost (1.129 text transports; 1.132 live listeners + inbound auth):** offline e2e `scripts/e2e-ext-channels.sh` and `scripts/e2e-ext-live.sh` against `scripts/fake_chatops.py`.
- Signal: signal-cli REST `POST /v2/send` (DM + `group.<id>`); inbound receive envelopes are pushed to `/api/channels/signal/inbound`. 1.132: polls `/v1/receive/{number}` (signal-cli REST normal mode) unless `receiveMode: "webhook"`; webhook pushes need `X-Webhook-Secret` when `webhookSecret` is set. Not yet: the JSON-RPC/SSE daemon transport, attachments, reactions.
- Matrix: `PUT rooms/{room}/send/m.room.message/{txn}` with thread (`m.thread`) replies; inbound app-service transactions or single events; `dmRooms` marks 1:1 rooms. 1.132: `/sync` long polling (initial backlog skipped, `next_batch` persisted in `<data>/matrix/sync.json`, own messages ignored) and invite auto-join (`autoJoin: false` disables); app-service pushes require the `hsToken` (Bearer or `?access_token=`). Not yet: E2EE, media.
- MS Teams: Bot Framework client-credentials token (cached) and `POST {serviceUrl}/v3/conversations/{id}/activities[/replyToId]`; `<at>` mentions stripped. 1.132: inbound Bot Framework JWTs are verified when `appId` is set (RS256 via the OpenID metadata `jwks_uri`, issuer `https://api.botframework.com`, audience = appId, exp/nbf with 5 min skew, JWKS cached 1 h and refetched on unknown `kid`; `verifyJwt: false` opts out). Not yet: Adaptive Cards, files, proactive install flows, the emulator/channel-service issuer variants.
- Google Chat: synchronous reply in the event response, or `spaces.messages.create` (thread reply) when `accessToken` is configured. 1.132: inbound bearer JWTs are verified when `audience` is set (issuer `chat@system.gserviceaccount.com`, Google's JWKS). Not yet: service-account JWT minting for outbound, cards.
- Mattermost: `POST /api/v4/posts` (thread `root_id`); inbound outgoing webhooks verified by `webhookToken`. 1.132: polls the posts of channels listed in `channels.mattermost.channels` (bot's own posts and system posts ignored). Not yet: the WebSocket event stream (so DMs need the channel id listed), slash commands.
- All five record a conversationRef, so conversations_* and ask_user work over them. Idle adapters now pick up hot-reloaded config.

**Misc channel extensions (1.138 — text transports for the former 16):**
`a2a`, `buzz`, `clickclack`, `irc`, `line`, `nextcloud-talk`, `nostr`, `raft`, `reef`, `sms`, `synology-chat`, `tlon`, `twitch`, `x`, `zalo`, `zalouser`.
Webhook inbound on `/api/channels/<id>/inbound` + HTTP outbound when credentials are set:
LINE reply/push, Zalo sendMessage, Twilio SMS, Twitch Helix chat, X DM API, Synology incoming webhook, Nextcloud Talk OCS chat, and HTTP bridges for IRC/Nostr/a2a/buzz/clickclack/raft/reef/tlon/zalouser (`apiBase` + optional bearer).
Offline e2e: `scripts/e2e-misc-channels.sh` against `scripts/fake_misc_channels.py`.
Not yet: native IRC sockets, Nostr NIP-04 crypto in-process, Twitch IRC, X Account Activity CRC, LINE rich menus/flex, Zalo OA stickers.

**Other extension families with no or only partial C++ counterpart:**
- memory-lancedb, memory-wiki
- diagnostics-otel/prometheus
- voice-call, google-meet/teams/zoom meetings, slack-huddles
- image/video/music provider packs (only generic generate tools)
- onepassword/vault secret backends
- tokenjuice, lobster, workboard, etc.

## 6. Plugin API

The JS host (`plugins/js_bridge/host.mjs`) accepts all 60 upstream `api.register*` entry points (1.130; recount — the earlier audit said 59). A plugin that calls any of them keeps loading; before 1.130 an unimplemented call threw and the plugin failed.

**Deeply consumed by the C++ runtime (12 upstream + the non-upstream registerHttpHandler):**
- registerTool, registerProvider, registerGatewayMethod, registerHttpRoute, registerHttpHandler
- registerHook, registerCommand, registerCli, registerService, registerChannel (thin)
- 1.130: registerWebSearchProvider (feeds `web_search` when no built-in Brave/Tavily key, or by provider id), registerTextTransforms (input rewrites on the newest user message before the provider call; output rewrites on the final assistant text — already-streamed deltas are not rewritten), registerAgentToolResultMiddleware (string/array/RegExp/function matcher; rewrites tool results before the model sees them)

**1.139 — formerly inert kinds are now callable:**
- Host: `invoke_registration` + `list_capability_providers`; C++ `plugins::InvokeRegistration` / `HasCapability` / `NotifyReload` / `NotifyLifecycle`.
- RPCs: `plugins.listCapabilities`, `plugins.invokeRegistration` (any captured `register*` kind + method + args).
- Prefer-plugin hooks: `decision_evaluate` → `registerDecisionProvider`; `web_fetch` → `registerWebFetchProvider` (direct JsHost call, not nested GatewayRpc); `image_generate` / `video_generate` / `music_generate` → matching generation providers; config hot-reload → `registerReload`; host start/stop → `registerRuntimeLifecycle`.
- Remaining kinds (context engine, memory, speech, embedding, policies, harness, …) stay generic-invoke only until a domain path wires them.

Offline e2e: `scripts/e2e-plugin-registrations.sh`, `scripts/e2e-plugin-caps.sh`.

The native C ABI (`docs/PLUGIN-SDK.md`) is a Linux-only subset with no upstream equivalent.

## 7. Behaviour

**Real:**
- the agent loop with tools, sub-agents, compaction, session reset policies, exec approvals (SQLite)
- sandboxing (bwrap/docker, labelled)
- pairing (device links, setup codes in upstream format since 1.124), OIDC/cookie auth
- cron, skills, MCP client (stdio + streamable-http + OAuth), ACP server/client
- **providers routed per `provider/model`** (1.125): config `models.providers.*` plus 31 built-in ids
  (openai→Responses, anthropic→Messages, google→Gemini, amazon-bedrock→Converse, anthropic-vertex / google-vertex→ADC, and OpenAI-compatible for the rest)
- **Bedrock / Vertex (1.137):** `auth: "aws-sdk"` signs Converse with AWS SigV4 (env access key / session token) or `AWS_BEARER_TOKEN_BEDROCK`; `auth: "google-adc"` refreshes ADC (`authorized_user` refresh_token or `service_account` JWT) against `OPENCLAW_GOOGLE_TOKEN_URL` / oauth2 and sends `Authorization: Bearer` to Vertex Anthropic `:rawPredict` / Gemini `:generateContent`. Offline fake: `scripts/fake_cloud_llm.py`.
- `agents.defaults.model.primary` as the default route, per-agent `model`
- local `chat` uses the same factory
- **webhook ingress** (1.125): `/hooks/wake`, `/hooks/agent` (async 202 or `waitForCompletion`), mappings with
  templates, the `gmail` preset with forEach, token header rules, body limit, session-key prefix policy

**Partial:**
- **Session storage (1.131):** `session.store: "sqlite"` (or `OPENCLAW_SESSION_STORE=sqlite`) stores sessions in `<sessions dir>/openclaw-agent.sqlite`, created from the verbatim upstream v2026.10.5 schema (all 57 tables, `user_version` 25, `schema_meta` primary row with role `agent`). The index entry goes in `session_nodes.entry_json`, each sessionId gets a `session_windows` row (`initial`/`reset`), and transcript lines go in `transcript_events.event_json` (same JSONL line shape as the file store). On first open, existing JSON/JSONL sessions are imported once; the files are left in place and a `sessions.migrated-to-sqlite.json` receipt is written. `sessions.reset` now starts a new window and archives the old transcript, as upstream does (it used to truncate). Default is still JSON. Partial: only the session/transcript tables are written; the other upstream tables (conversations, memory index, standing intents, pending inputs, …) exist but the C++ stores for those features remain JSON files. Events are stored as plain `event_json` (no zstd tier, no `navigation_json`), and upstream's own legacy-schema upgrade path (v<25 → 25) is not replicated — older upstream databases are refused rather than migrated. E2e: `scripts/e2e-sqlite-sessions.sh`.
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

1. ~~Telegram live Bot API~~ (1.126; media/keyboards/callbacks 1.135; 1.140: reactions (setMessageReaction out, `message_reaction` in → transcript note without a turn), edit (editMessageText / editMessageReplyMarkup), delete/pin/unpin via `message.action`, `setMyCommands` menu from `channels.telegram.commands` or defaults, inbound edits marked; offline `scripts/e2e-telegram-actions.sh`). Remaining: polls, forum-topic management, business/payments APIs.
2. ~~Signal, Matrix, MS Teams, Google Chat, Mattermost~~ (text transports in 1.129; live listeners/verification listed in §5). ~~16 misc channels~~ (1.138 text transports).
3. ~~Native streaming~~ (done in 1.127); ~~Bedrock/Vertex auth~~ (1.137: AWS SigV4 + `AWS_BEARER_TOKEN_BEDROCK`, Google ADC authorized_user/service_account → Vertex Anthropic rawPredict + Gemini generateContent; offline `scripts/e2e-bedrock-vertex.sh`).
4. ~~Core tools web_search, ask_user, view_image, agents_wait, conversations_*~~ (done in 1.128, with the partials in §3).
5. ~~Plugin API inert registrations~~ (1.139: invoke + decision/web_fetch/media/reload/lifecycle hooks). Still thin: context engine, memory, speech, harness need domain wiring beyond generic invoke.
6. ~~SQLite session/transcript storage~~ (1.131, opt-in); other stores still JSON.

## 10. Test-suite notes

The smoke step "device tools.list allowed" failed intermittently. Root cause: the smoke's second gateway shared the
default data dir (`~/.openclaw-cpp/data`), whose device-link store persists a per-IP claim rate limit (30/min). Earlier
runs in the same minute (and `connect`, which polled `pairing.link.claim` every 500 ms while pending) used up the
budget, so the device-token claim was rate-limited and the token came back empty. Fixed in 1.126: the smoke gateway uses
its own data dir, polls with the correct secret on a still-pending link no longer consume the budget (bad secrets
still do), and `connect` polls once a second.

- 1.130: the smoke `tools.list` checks piped large RPC output into `grep -q` under `pipefail`; once the tool list grew, the writer occasionally got SIGPIPE (rc 141). They now use a here-string / `grep >/dev/null`.


**Test-suite offline guard (1.135–1.136):** integration-smoke unsets host provider keys. Every standalone `scripts/e2e-*.sh` that can start a gateway now pins `OPENCLAW_DEFAULT_PROVIDER=mock` (or keeps its own fake) and unsets host LLM API keys, so running one script alone cannot reach DeepSeek/OpenAI/etc. Before 1.135, scripts that didn't pin `mock` could reach a live provider when the host had a key exported. Also fixed in 1.135: plugin/MCP tool names with `.` are sanitized to `[A-Za-z0-9_-]` for providers and mapped back on invoke.
