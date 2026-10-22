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
| Agent tools (core) | ~57 | 46 native (+JS/MCP bridged) | 46 | browser/desktop are local-runtime variants | ~19 (§3) |
| Config root keys | 46 | 46 accepted | ~20 deeply validated | 26 shallow (key accepted, sub-shape not validated) | 0 |
| Channel extensions | 27 messaging | 6 working | telegram (1.126), discord, slack, feishu, whatsapp (Cloud API), imessage (bridge inbound) | whatsapp-web (partial Noise/Baileys) | 21 (§5) |
| Model-provider extensions | ~85 | 28 built-in ids + any `models.providers.*` | 4 wire formats (chat completions, Responses, Anthropic Messages, Gemini) offline-tested | no streaming for Anthropic/Gemini/Responses (single delta); no OAuth/Bedrock/Vertex signing | Bedrock, Vertex, Copilot, Codex OAuth, … |
| Plugin API (`api.register*`) | 59 | 10 (JS host) + C ABI | registerTool/Provider/GatewayMethod/HttpRoute/Hook/Command/Cli/Service/Channel/HttpHandler | Channel/Service are thin | 49 (§6) |

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

**Native (46):**
- agents_list, automations, acp_agent, browser(+evaluate/open_url/screenshot), canvas
- create_goal/get_goal/update_goal, desktop, discord_voice, docs_search, echo, edit, exec, process, shell, read, write
- github_publish, http_get, image, image_generate, music_generate, video_generate
- memory_get/search/set, message, nodes, pdf, presence, reminder, secrets, session_status
- sessions_history/list/search/send/spawn/yield, skills_read/search, subagents, time_now, transcripts, tts, web_fetch

There are also JS-plugin tools and MCP-bridged tools.

**Missing compared with upstream core tools:**
- agents_wait, ask_user, computer (only `desktop` exists), conversations_list/send/turn
- dashboard, decision_evaluate, gateway, github_identity_status, mobile_ui
- personal_instructions, plugins, portal, progress_card, screen, skill_workshop, terminal, view_image
- web_search (as a tool; webSearch RPC exists)

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

**Missing channel extensions (21):**
- a2a, buzz, clickclack, googlechat, irc, line, matrix, mattermost, msteams, nextcloud-talk, nostr
- raft, reef, signal, sms, synology-chat, tlon, twitch, x, zalo, zalouser

**Other extension families with no or only partial C++ counterpart:**
- memory-lancedb, memory-wiki
- diagnostics-otel/prometheus
- voice-call, google-meet/teams/zoom meetings, slack-huddles
- image/video/music provider packs (only generic generate tools)
- onepassword/vault secret backends
- tokenjuice, lobster, workboard, etc.

## 6. Plugin API

The JS host (`plugins/js_bridge/host.mjs`) implements 10 of 59 `api.register*` entry points:
- registerTool, registerProvider, registerGatewayMethod, registerHttpRoute, registerHttpHandler
- registerHook, registerCommand, registerCli, registerService, registerChannel (thin)

Not implemented:
- the agent harness/executor/middleware APIs
- the context engine, compaction, embedding, memory capability/corpus/prompt APIs
- the speech/realtime/transcription/image/video/music generation providers
- web search, storage, session catalog/extension/action, config migration, Control UI descriptors
- security audit collectors, trusted tool policy, node host commands, worker providers, etc.

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
- **Streaming** is token-level only for chat-completions providers. Anthropic/Gemini/Responses deliver one final delta.
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
2. Signal, Matrix, MS Teams, Google Chat, Mattermost channel extensions.
3. Native streaming for Anthropic/Gemini/Responses, plus Bedrock/Vertex auth.
4. Missing core tools: web_search, ask_user, view_image, agents_wait, conversations_*.
5. Plugin API breadth: provider families, context engine, memory capability.
6. SQLite-backed session and transcript storage, for on-disk parity.

## 10. Test-suite notes

The smoke step "device tools.list allowed" failed intermittently. Root cause: the smoke's second gateway shared the
default data dir (`~/.openclaw-cpp/data`), whose device-link store persists a per-IP claim rate limit (30/min). Earlier
runs in the same minute (and `connect`, which polled `pairing.link.claim` every 500 ms while pending) used up the
budget, so the device-token claim was rate-limited and the token came back empty. Fixed in 1.126: the smoke gateway uses
its own data dir, polls with the correct secret on a still-pending link no longer consume the budget (bad secrets
still do), and `connect` polls once a second.
