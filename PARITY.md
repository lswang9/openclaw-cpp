# OpenClaw 2026.10.5 → openclaw-cpp

**Version: 1.75.0-dev · 未宣称与上游全量对齐。**

## 执行摘要（今晚可用什么）

| 层级 | 内容 |
|------|------|
| **今天可当真用（`impl:real`，仍是子集）** | Gateway HTTP+WS；mock/DeepSeek/OpenAI-compat 路由；agent loop（tools、max_turns、timeout、cancel、session 锁）；sessions/CRUD/export；**workspace 文件工具 read/write/edit（沙箱化：拒绝 `..`/绝对路径/符号链接逃逸）** + **Markdown 记忆（MEMORY.md + memory/*.md，memory_search 为**混合检索**（0.7×本地向量 + 0.3×BM25，`mode` 可选 bm25/vector；向量为离线特征哈希嵌入：词 + 字符三元组 + CJK 单字，**非神经模型**）、memory_get 按行取、memory_set 持久化）**，`impl:real`；**系统提示词组装**（AGENTS/SOUL/TOOLS/IDENTITY/USER/HEARTBEAT/BOOTSTRAP/MEMORY.md 注入，单文件 20k 截断）+ **技能加载优先级**（workspace > ~/.openclaw/skills > bundled > extra）+ **gating**（requires.bins/anyBins/env、os、disable-model-invocation）+ `<available_skills>` 注入；**`skills.entries.<key>`**（enabled、env、apiKey→primaryEnv，满足 env gating，agent 运行期间注入且不覆盖已有变量）+ **技能斜杠命令**（`/<规范化名>` 或 `/skill <名> [参数]`，`user-invocable: false` 排除）+ **`command-dispatch: tool`**（`command-tool`/`command-arg-mode: raw`，斜杠命令直接调用工具，参数 `{command, commandName, skillName}`，不经模型），`impl:real`（env 注入为**每次运行线程局部覆盖层**，不修改进程环境，并发 run 互相隔离；子进程经 RunProcess 获得 environ+覆盖层）；`exec`/`shell` 工具默认真实执行（1.48 起，按上游 exec 审批/沙箱语义，见下文 1.48）；`skills list|check|info`、`context [prompt]` CLI；`chat` 支持 mock provider；**配置文件**：路径 `OPENCLAW_CONFIG_PATH` > `~/.openclaw/openclaw.json`（兼容旧 `~/.openclaw-cpp/config.json`），**JSON5 子集**解析（注释/尾逗号/无引号键/单引号/hex）、`${ENV}` 替换（缺失报错，`$${}` 转义）、`config get/set/unset/file/validate` 点路径 CLI、原子写+.bak，`impl:real`；**`$include`**（字符串/数组、相对路径、嵌套≤10、环检测、深合并后本地覆盖；写回时保留 `$include` 只写差异键）；**schema 校验**（已知根键 + gateway(port/bind/reload/auth)、agents.defaults(model/compaction/memorySearch/timeouts)、agents.list、bindings、session(reset*/dmScope/keyFormat)、channels.<id>(enabled/dmPolicy/groupPolicy/allowFrom/token 类)、tools(allow/deny/profile/exec)、plugins(entries/allow/deny)、models.providers、skills.entries、logging/cron/hooks 的类型与取值；类型错误拒绝加载、网关拒绝启动，未知键为警告）；**热重载**（网关每次 RPC/入站最多每秒检查 mtime，非法配置被拒且保留旧配置，`gateway.reload.mode: off` 关闭，host/port 变化提示需重启，`config.reload` RPC），`impl:real`（schema 覆盖主要子树但仍非上游完整 zod schema，渠道私有字段/深层嵌套多未校验；热重载现会**重扫技能并重建系统提示词**、**重启 channels.<id> 子树变化的运行中适配器**、plugins 子树变化时**重扫插件并重启 JS 宿主**（提示词重建有 smoke 覆盖；渠道/插件重启由 1.49 `scripts/e2e-hot-reload-channels.sh` 覆盖））；`OPENCLAW_MOCK_SCRIPT` 脚本化 mock provider 用于离线 e2e；**会话生命周期**：`sessions.json` 索引（sessionId/createdAt/updatedAt/resets）、会话键无损编码（修复 `a:b`≡`ab` 冲突，兼容旧文件）、`/new` `/reset`（可带后续消息）归档旧 transcript、`session.reset` 每日 atHour(默认 4 点)/idleMinutes 过期，`impl:real`（transcript 已为上游式 **JSONL**（session 头 + message 行 + parentId 链，旧 JSON 自动迁移为 `.migrated`）；网关只持久化**完整未裁剪**历史 + 本轮新消息（不再写入 pruned 视图/系统提示词）；**`session.resetByChannel` > `resetByType`(direct/group/thread) > `reset`** 已实现（chat_type 由 webhook `chat_type` 或会话键推断）；）；**compaction**：超过 `contextTokens - compaction.reserveTokens`（默认 64k/16k，按 chars/4 估算）时用模型把旧轮次总结成一条摘要消息，保留尾部且不从孤立 tool 结果开始，失败回退截断；`/compact [指示]` 手动压缩，`impl:real`；**compaction 前 memory flush**：自动压缩前跑一次静默 agent 轮（可用工具写 memory/*.md，输出不入 transcript，`compaction.memoryFlush.enabled/prompt/systemPrompt`）`impl:real`（`scripts/e2e-memory-flush.sh` 端到端：脚本化 mock provider 经真实网关触发 flush→write memory/flush.md→摘要压缩，flush 轮不入 transcript；已并入 smoke）；**tokenizer**：自研字节级 BPE + 手写 cl100k 预分词，加载 tiktoken 格式词表（`OPENCLAW_TOKENIZER_PATH` 或 `~/.openclaw/tokenizers/cl100k_base.tiktoken`，`tokenizer fetch` 下载），在样例与仓库文件上与 tiktoken 计数一致，`impl:real`（词表未随仓库分发，无词表时回退 chars/4；cl100k ≠ DeepSeek 自有分词器，仍是近似；Unicode 字母类为近似实现）；渠道会话键已改为上游格式（见下））；**上游会话键**：`agent:main:main`（`session.dmScope`=main 默认 / per-peer `agent:main:dm:<peer>` / per-channel-peer）、群 `agent:main:<ch>:group:<id>`、频道 `:channel:<id>`、线程/话题追加 `:thread:<id>`；Slack(channel_type/thread_ts)、Discord(guild_id)、Feishu(chat_type/thread_id)、Telegram 更新格式(chat.type/topic，经通用 webhook 适配器)、WhatsApp 适配器填充结构化字段；**修复**：渠道消息默认不再自动绑定到 ACP 会话（上游为显式绑定；`OPENCLAW_ACP_AUTO_BIND=1` 恢复旧行为）；旧键 transcript 首次自动迁移；显式 `session_key` 或 `session.keyFormat: legacy` 保持原样，`impl:real`（多 agent 路由 bindings 见 1.47）；**embedding provider 接口**：local（哈希）/ openai（任意 OpenAI 兼容 `/embeddings`，`memorySearch.remote.baseUrl/apiKey`，auto 有 key 时选 openai）+ **持久化向量索引/嵌入缓存**（按 sha256(chunk) 存于 `$OPENCLAW_DATA_DIR/memory/main.index.json`，未变 chunk 不重复嵌入，换 provider/model 自动全量重建），远程失败自动回退本地并标记 `fallback`；`memory search|status|index` CLI；`scripts/e2e-embeddings.sh` 用本地假服务端验证，`impl:real`（本地 ollama/gguf provider 见 1.64（Gemini/Voyage 见 1.60，SQLite 向量+FTS 见 1.59/1.64））；tools（含 `http_get` / `browser_*`）；**CDP browser**（Chrome debugging 就绪时 `impl:real`）；MCP mock/stdio bridge + `mcp.app` list/call/resources；cron/persist/pairing；Control UI 骨架；IM **webhook 入站**（Feishu/Slack/Discord/**WA Cloud**/iMessage）+ **Slack Socket Mode** + **Discord Gateway** + **Feishu 长连接**（mock/feed 或 app 凭证）与 **有 token 时的出站**；AES secrets（有 passphrase 时）；**JS 插件薄桥**（Node stdio / mock → `js.sample_echo`）；**ACP** `session/prompt`→AgentLoop + cancel + **channel bind** + **multimodal image**；`scripts/integration-smoke.sh`（mock，无活 LLM） |
| **仿真 / 轻量（勿当生产自动化）** | `impl:real` computer.screenshot/click/type/move（display+ffmpeg/xdotool）；canvas 本地磁盘 `impl:real` + A2UI v0.8 宿主（1.71）；desktop launch/windows/AT-SPI 树在 X11 下 `impl:real`（1.72），无 DISPLAY 时仍 `real-sim`；browser curl-session 回退 / CDP mock；**无** Playwright |
| **本地占位 store（API 面可测，不是上游 UI）** | **board** 已有磁盘 JSON + `/board` HTML CRUD；portal / wizard / openclaw.setup / mentions 仍偏占位 |
| **明确缺失** | 本地 GGUF embedding；（ClawHub 见 1.58，向量存储 1.59，Gemini/Voyage 1.60）ACP **acpx**/OAuth/音视频（通道绑定+多模态+session MCP+token 门禁子集已有）；完整 JS/TS 插件宿主（仅有 Node stdio 薄桥 + sample_js）；非 X11 的桌面/无障碍（X11+AT-SPI 见 1.72）；完整 Slack interactive；**Baileys**（仅有 Cloud API 子集）；Discord slash/voice；Feishu 专有 SDK 帧；Socket/Gateway/长连接仅为消息子集；上游级 board/canvas（A2UI v0.8 宿主+服务端渲染见 1.71；无 Lit 渲染器/节点 WebView）；上游云端设备链接/QR/scope token（仅有本地配对码流程 `impl:real`；ocd_ token 可用于网关鉴权，仅两种固定角色（operator/device 拒绝列表），非细粒度 scope；限流仅进程内；/login 为服务端会话 + CSRF token + WS Origin 检查；限流按 TCP 对端 IP 持久化，不信任代理头，无多实例共享；OIDC 授权码+PKCE（RS256/ES256、按组映射角色、刷新复验，1.74）；MFA 为按用户的 TOTP+恢复码，无 WebAuthn）；`.github/workflows` 直接推送（见 docs CI） |

## Impl 标签

| `impl` | 含义 |
|--------|------|
| **real** | 本地状态和/或真实 HTTP（视 env） |
| **real-sim** | `SimulationBackend`，非生产 computer-use |
| **honest** | 少见的部分实现 |
| **implemented:false** | 故意不伪造（debugProxy / claws / nativeHook / web.login / attach 等） |

## 规模（计数 ≠ 对齐度）

| 指标 | 约值 |
|------|------|
| Catalog 广告 | 516 |
| `ImplementedMethods` | ~640（大量前缀桶/本地 store） |

详见里程碑 [SERIES.md](SERIES.md)；发版流程 [docs/RELEASE.md](docs/RELEASE.md)；上手 [STATUS.md](STATUS.md)。

## 1.75 WA Web: zlib frames + Signal protocol offline layers
- real: compressed WABinary frames (flag & 2) are inflated with zlib (conan `zlib` now explicit); self-test decodes a Node `zlib.deflateSync` frame byte-identically.
- real (offline, spec-shaped, not libsignal-verified): `waweb::signal`: X3DH (0xFF*32 prefix, DH1–DH4, HKDF "WhisperText"), Double Ratchet (root HKDF "WhisperRatchet", chain HMAC 0x01/0x02, "WhisperMessageKeys" → AES-256-CBC + HMAC-SHA256/8 over both identity keys), SignalMessage/PreKeySignalMessage protobuf with version 0x33, signed-prekey XEdDSA verify, one-time-prekey consumption, skipped keys (≤2000), atomic state on failure. Self-test: pkmsg×2, reply, out-of-order, bad MAC, replay, ratchet turns.
- gaps: no byte-level libsignal interop fixtures (npm unavailable offline); no XEdDSA signing (we cannot sign our own SPK yet); no sender keys (groups), session store persistence or WA pre-key upload; live login still missing (intentional).

## 1.74 OIDC ES256 + group→role + refresh re-validation; per-user MFA
- real: id_token ES256 (P-256 JWK, raw r||s) alongside RS256; groups claim (`OPENCLAW_OIDC_GROUPS_CLAIM`) → role via `OPENCLAW_OIDC_ROLE_MAP` (highest wins), `OPENCLAW_OIDC_ALLOWED_GROUPS` allowlist, `OPENCLAW_OIDC_REQUIRE_GROUP=1` fail-closed. OIDC sessions keep the refresh token and re-validate every `OPENCLAW_OIDC_RECHECK_SEC` (default 3600) via the refresh grant (new id_token re-verified, same sub, role re-derived); failure ends the session. e2e-oidc covers ES256, admin mapping, refresh-deny → 401, require_group → 403.
- real: per-user MFA: `auth mfa enroll|status|code|verify|disable --user <id>` stores `<state>/auth/mfa-users/<id>.json`; `/login` uses the principal's enrollment (gateway token → `gateway`, ocd_ token → its token_id), else the global one.
- gaps: refresh tokens are stored in the sessions file (0600 dir, not keychain); no back-channel logout; no EdDSA/PS256.

## 1.73 Desktop agent tool; A2UI clicks into agent session + writable inputs
- real: agent tool `desktop` (status|windows|observe|focus|close|launch|release|a11y_tree|a11y_action; requires_approval; launch still allowlist-gated) over the 1.72 controller; e2e via `tools.invoke`.
- real: A2UI TextField/CheckBox are writable: `/__openclaw__/a2ui/input` writes into the bound data-model path (type-checked; non-inputs 400), so Button `userAction.context` resolves user-entered values. Button clicks are now delivered into the agent as a `canvas`-channel inbound turn (`[A2UI userAction] {...}`), routed by session rules (default main session); `OPENCLAW_A2UI_DELIVER=0` or `deliver:false` only records the event.
- gaps: delivery is synchronous in the click request; no Slider/DateTimeInput/MultipleChoice.

## 1.72 Desktop control: real launch/windows/AT-SPI accessibility tree
- real (when DISPLAY + xdotool): `desktop.launch` (fail-closed policy: `OPENCLAW_DESKTOP_APPS` basename allowlist or `OPENCLAW_DESKTOP_LAUNCH=1`; setsid, tracked pids, waits for a window), `desktop.windows|focus|close` (xdotool), `desktop.release` (SIGTERM→SIGKILL on the process group), `desktop.observe` (windows + a11y summary). `desktop.launch` and `desktop.a11y.action` are forbidden for the device role.
- real (when python3-gi + Atspi + a11y bus): `desktop.a11y.tree` (role/name/states/bounds/actions/text, capped node count, filter by app) and `desktop.a11y.action` (do_action by name/index, EditableText set_text).
- real: `scripts/e2e-desktop.sh` (smoke; SKIPs when deps are missing) runs Xvfb + dbus-run-session with a GTK fixture app: deny by default, launch, window list, tree, set_text, a11y click changes the label, release kills the process.
- gaps: X11/Linux only (no Wayland, macOS AX, or Windows UIA); without DISPLAY/xdotool the sim path remains (`real-sim`); no agent tool wrapper yet (RPC only).

## 1.71 Canvas: A2UI v0.8 host (validate, push JSONL, render, userAction)
- real: A2UI v0.8 server→client messages (`surfaceUpdate`, `dataModelUpdate`, `beginRendering`, `deleteSurface`) validated like upstream nodes-cli (one message per line, v0.9 `createSurface` rejected, line-numbered errors); RPC `canvas.a2ui.validate|push|pushJSONL|reset|state|render`; text helper builds surfaceUpdate+beginRendering. Surface state is persisted in `<canvas>/a2ui-state.json`.
- real: gateway-hosted renderer at `/__openclaw__/a2ui/` (auth required, CSP, HTML-escaped) for the v0.8 standard-catalog subset (Text with h1–h5/caption, Image http(s)/data, Icon, Row, Column, List with explicitList or template+dataBinding, Card, Divider, Button, CheckBox, TextField, Tabs) with path data binding; reloads on revision change. Button clicks POST `/__openclaw__/a2ui/action`; the server resolves the stored action into an upstream-shaped `userAction` {name, surfaceId, sourceComponentId, timestamp, context} recorded as a `canvas.events` entry (`canvas_id=a2ui`). Agent tool `canvas` (a2ui_push|a2ui_reset|state|actions).
- real: `scripts/e2e-a2ui.sh` (smoke).
- gaps: not the upstream Lit renderer/WKWebView node surfaces (no node-scoped plugin-surface URLs, no `canvas.present/navigate/eval/snapshot` on nodes); inputs are read-only (no two-way TextField/CheckBox binding); Modal/Slider/DateTimeInput/Video/Audio unsupported; (1.73: userAction delivered into the agent session; inputs writable).

## 1.70 Gateway OIDC login (auth code + PKCE)
- real: `/login/oidc` → discovery (`.well-known/openid-configuration`, issuer checked) → authorize redirect with state, nonce, PKCE S256; `/login/oidc/callback` checks the state cookie binding (single-use, 10 min), exchanges the code, verifies the RS256 id_token via JWKS (≥2048-bit; iss/aud/azp/exp/iat/nonce), and enforces a fail-closed allowlist (`OPENCLAW_OIDC_ALLOWED_EMAILS|SUBS|DOMAINS`; email needs email_verified). Creates an identity session (role `operator` by default or `admin` via `OPENCLAW_OIDC_ROLE`, 12 h TTL) that stops working when OIDC is unconfigured. `/login/methods` is public; the login UI shows an SSO link.
- real: `scripts/e2e-oidc.sh` (smoke) runs against `scripts/fake-oidc-idp.py` (PKCE-checking fake IdP): allowed user → operator session; missing state cookie → 401; disallowed email → 403; bad signature → 403.
- gaps: RS256 only (no ES256/EdDSA), no refresh/back-channel logout, no per-user MFA (the IdP handles MFA), no group/claim→role mapping beyond one configured role; not tested against a real IdP.

## 1.69 Gateway MFA: TOTP + recovery codes on /login
- real: RFC 6238 TOTP (HMAC-SHA1, 30 s, 6 digits, ±1 step) with replay protection (last accepted counter persisted) and 8 single-use recovery codes (SHA-256 hashed) in `<state>/auth/mfa.json` (0600) or `OPENCLAW_MFA_TOTP_SECRET`. CLI `auth mfa enroll|status|code|verify|disable` (enroll prints otpauth:// URI). `/login` returns 401 `mfa_required` until a valid `otp` is supplied; login UI has a one-time-code field.
- real: self-test RFC 4226/6238 vectors; `scripts/e2e-mfa.sh` (smoke) covers missing/bad/good/replayed OTP and recovery reuse against a live gateway.
- gaps: MFA applies to the cookie /login only (bearer-token API calls are not second-factor gated, as upstream tokens are machine creds); no WebAuthn; single shared secret, not per-user.

## 1.68 WA Web: WABinary token dictionary v3 + XEdDSA cert chain (offline)
- real: `EncodeBinaryNode`/`DecodeBinaryNode` wire-compatible with Baileys (token dictionary v3 single/double-byte tables generated from Baileys constants.ts, MIT; nibble/hex packing; JID_PAIR/AD_JID/FB_JID/INTEROP_JID; LIST_8/16; BINARY_8/20/32; frame flag byte). Self-test decodes 5 Baileys-generated fixtures (scripts/fixtures/wabinary-baileys.json) and re-encodes byte-identically.
- real: `VerifyXEdDsa` (libsignal curve25519_verify semantics via OpenSSL BN + Ed25519) and `VerifyCertChain` following whatsmeow verifyServerCert: intermediate signed by the WA root (`WaRootPublicKey`), issuerSerial 0, leaf signed by intermediate, serial linkage, leaf key == server static, validity window. Self-test uses a synthetic chain and checks rejects (real WA root, wrong static, tamper).
- gaps: zlib-compressed frames rejected (no zlib linked); not tested against a real WA ServerHello (needs live socket); Signal/libsignal, ADV pairing, live QR login remain missing.

## 1.67 Token-level streaming + typing keep-alive
- real: `Provider::CompleteStreaming(messages, tools, on_delta)`. A shared OpenAI-compatible SSE parser (`StreamChatCompletion`) handles content deltas, **tool_call deltas** (index/id/name/argument fragments concatenated), `finish_reason`, and servers that ignore `stream:true` (falls back to a plain completion object). DeepSeek and OpenAI-compat use it. Router and AuthRotating forward it, and rotation never happens after partial output. The default for other providers is non-streaming with a single delta. Mock streams word by word, with `delayMs` per script entry.
- real: the agent loop streams every model turn when `LoopOptions.on_delta` is set. The gateway enables it only when someone consumes it: a connected WS client (`agent.delta` events), a plugin reply sink, or `agents.defaults.streaming: true`.
- real: plugin `dispatchReplyWithBufferedBlockDispatcher` receives `reply.delta` → `dispatcherOptions.onPartialReply({text})` and `reply.typing` → `onTyping()` while the turn runs, on top of the 1.64 paragraph blocks.
- real: **typing keep-alive** during agent turns: Discord `POST /channels/{id}/typing` and Telegram `sendChatAction typing`, sent immediately and then every `channels.<id>.typingIntervalSeconds` (default 6). `channels.<id>.typing:false` disables it. Other channels record the typing event and emit `agent.typing` on WS. New `TELEGRAM_API_BASE` / `channels.telegram.apiBase` setting for the outbound base URL.
- tests: `scripts/e2e-streaming-typing.sh` (smoke) uses a fake SSE server that splits a tool call's arguments across chunks, slow content chunks, a plugin that observes partial replies and typing, and Discord typing pings (≥2) with Bot auth.
- gaps: no live "draft" message editing (Telegram/Discord/Slack partial-message edit streaming). Slack has no bot typing API, so that is record only. Feishu/WhatsApp typing is record only.

## 1.66 Per-agent auth profiles + OS keychain (libsecret)
- real: built-in providers (`AuthRotatingProvider`) and plugin providers resolve credentials from the **running agent's** store, `<state>/agents/<agentId>/agent/auth-profiles.json`, using `agents.list[].auth.order` when set and global `auth.order` otherwise. If the agent's store has no profile for the provider, they fall back to `main`'s store. CLI: `models auth paste-token|list|remove … --agent <id>`.
- real (Linux, when a Secret Service is reachable): profile secrets (`key`/`access`/`refresh`/`token`) are stored through **libsecret** (dlopen `libsecret-1.so.0`, schema `ai.openclaw.AuthProfile`, attributes profile+field scoped by store path). The JSON file keeps only `keychain:{backend:"libsecret",fields}`. Reads hydrate from the keychain, and `remove` clears the keychain entries. `models auth keychain` reports the backend and the reason when it is unavailable.
- honest fallback: with no session bus, no libsecret, or a failed probe write, secrets stay in the 0600 JSON file (`secretBackend:"file"`). `OPENCLAW_KEYCHAIN=off` forces the file backend; smoke uses off by default.
- tests: `scripts/e2e-auth-agents-keychain.sh` (smoke). The work agent is routed by a binding and uses its own key, while main uses main's key. The libsecret half runs gnome-keyring under `dbus-run-session` and verifies the secret is absent from the file. It reports SKIP when the keyring is unavailable.
- gaps: no macOS Keychain or Windows Credential Manager. Existing plaintext profiles are not migrated automatically (re-paste to move them). A locked keyring that needs an interactive prompt makes the keychain unusable and the file backend is used instead.

## 1.65 Vendor-shaped TTS/STT clients (OpenAI / ElevenLabs / Deepgram)
- real: `OPENCLAW_TTS_PROVIDER=openai|elevenlabs|deepgram|http`. OpenAI `POST /v1/audio/speech` `{model,input,voice,response_format}` Bearer. ElevenLabs `POST /v1/text-to-speech/{voice_id}` `{text,model_id}` `xi-api-key`. Deepgram `POST /v1/speak?model=aura-…` `{text}` `Authorization: Token`. Keys come from `OPENCLAW_TTS_API_KEY` or the vendor env (`OPENAI_API_KEY`/`ELEVENLABS_API_KEY`/`DEEPGRAM_API_KEY`), and the base URL defaults to the vendor host. `OPENCLAW_TTS_OUT_DIR` saves the audio, with the extension taken from the content type.
- real: `OPENCLAW_STT_PROVIDER=openai|elevenlabs|deepgram`. OpenAI multipart `/v1/audio/transcriptions`. ElevenLabs multipart `/v1/speech-to-text` (`model_id=scribe_v1`, `language_code`). Deepgram raw-body `/v1/listen?model=nova-3` with the transcript taken from `results.channels[0].alternatives[0].transcript`.
- real: CLI `voice tts [--voice V] <text>` / `voice stt <file> [--language L]`.
- tests: `scripts/e2e-voice-providers.sh` (smoke) runs a fake server that asserts each vendor's path, headers and body. These are shape tests only: no live vendor calls.
- gaps: no streaming TTS (chunked/websocket), no Deepgram live STT websocket, and no automatic voice-note transcription on channel inbound.

## 1.64 Streaming reply blocks, deadlock-free plugin agent.reply, SavePreserving, auth rotation, autoReview.model, PTY resize, Ollama/GGUF embeddings, FTS, WA Web offline stubs
- real: plugin `dispatchReplyWithBufferedBlockDispatcher` receives **streamed** `hostEvent` `reply.block` events (kind `block`/`final`) before the final `hostReply` (`streamed:true`). Events are chained before resolve so delivery order is stable.
- real: plugin **commands/tools may initiate** `agent.reply` without deadlock: `JsHost` splits `write_mu_` (SendHostReply/Event) from the request mutex, `state_mu_` for `HasCommand`/`HasHook` during an in-flight Request, `EmitHook` skips when `RequestInFlight()` (re-entrancy from nested DeliverInbound, e.g. `sample_upstream` `session_start`), and DeliverInbound drops `deps_mu_` before `RunCommand`.
- real: `writeConfigFile` / `config::SavePreserving` keeps leading `//` comments and top-level `$include`, writes JSON5-ish with unquoted `$include`, validates schema, atomic `.bak`. Plugin install records also go through SavePreserving.
- real: built-in DeepSeek / OpenAI-compatible providers wrap `AuthRotatingProvider` — keys from `auth-profiles.json` + `auth.order.<provider>`, rotate on HTTP 401/429, env fallback. Offline: `scripts/e2e-auth-rotation.sh`.
- real: `tools.exec.autoReview.model` (`provider` or `provider/model`) is honored via `ReviewWithProviderRoute` on the gateway router.
- real: PTY `ResizePty` (TIOCSWINSZ) + process tool `resize` action (`pty_rows`/`pty_cols` on background sessions).
- real: embedding providers `ollama` (`/api/embeddings`) and `gguf` (magic+version check, then LocalEmbedder neural stand-in). Offline: `scripts/e2e-embeddings-ollama-gguf.sh`.
- real: memory SQLite **FTS5** (`chunk_fts` + `bm25()`); hybrid search can use FTS text scores when enabled.
- real (offline stubs): WA Web `DecodeCertChain`/`VerifyCertChain` (structural), (superseded by 1.68: Baileys-compatible WABinary v3 + XEdDSA chain verify), Signal identity/X3DH/ratchet HKDF stubs — **not** interoperable with WhatsApp. Live Baileys login still missing (needs user phone; not requested).
- gaps: TTS/STT still generic HTTP only (no OpenAI/ElevenLabs/Deepgram-specific clients offline). libsignal byte-level interop (Signal X3DH+ratchet offline since 1.75), sender keys, and live QR login still missing. Token-level streaming (vs paragraph blocks) still missing.

## 1.63 Provider auth: OAuth / API-key flows + auth profiles
- real: `openclaw models auth login --provider <id> [--method <authId>]` runs the provider plugin's `auth[].run(ctx)`. The `ctx.prompter.text/confirm/select/note` and `ctx.openUrl` calls are host calls answered by the CLI on the terminal. Methods of kind `api_key`/`token` with no `run` get a built-in key prompt. The result's `profiles[]` go to `auth-profiles.json`, its `configPatch` is merge-patched into config (schema-validated), and `auth.profiles.<id> = {provider, mode}` is recorded. Also `models auth paste-token --provider <id> [--profile-id]`, `models auth list` (secrets masked), and `models auth remove <profileId>`.
- real: `auth-profiles.json` lives at `<OPENCLAW_STATE_DIR|~/.openclaw>/agents/<agent>/agent/auth-profiles.json`, mode 0600, written atomically, with `api_key` and `oauth` credential shapes.
- real: plugin providers receive `complete({…, auth: {profileId, type, apiKey | accessToken}})`, preferring `<provider>:default` and otherwise the first `<provider>:*` profile. An OAuth credential within 60 s of expiry is refreshed first through `provider.refreshOAuth(cred)` (or `oauth.refresh` / `auth[].refresh`) and persisted. If refresh fails, the turn fails with a clear error.
- gaps: the OAuth flow itself (authorize URL, PKCE, callback or paste) belongs to the plugin, and the host provides no loopback callback server. Built-in DeepSeek/OpenAI-compat use auth-profiles + rotation as of 1.64 (env still fallback). Per-agent profiles exist only for `main` in the CLI. No keychain storage. Test: `scripts/e2e-provider-auth.sh` (smoke).

## 1.62 Plugin runtime: reply dispatcher, media fetch/store, TTS/STT, writeConfigFile
- real: plugin → host **reverse calls** over the existing stdio JSON-RPC (`{"hostCall"}` / `{"hostReply"}`; replies bypass the host's request chain). They are served while any host request is in flight, including the 200 ms event pump; agent turns are deferred to a gateway thread so the plugin host lock is never held across them.
- real: `runtime.channel.reply.dispatchReplyWithBufferedBlockDispatcher({ctx, dispatcherOptions:{deliver, onReplyStart, onIdle, onError, blockLimit}})` runs an agent turn for the inbound ctx and delivers paragraph-aware blocks (`block`… then `final`). As of 1.64 it is safe from tools/commands (deferred + split locks); 1.62 refused nested calls to avoid deadlock.
- real: `runtime.media.fetchRemoteMedia({url, maxBytes})` uses a new SSRF guard (`net::SafeFetch`: resolve + pin via CURLOPT_RESOLVE, blocks loopback/RFC1918/link-local/CGNAT/ULA/multicast/v4-mapped, manual redirects re-checked, byte cap). Private hosts need `plugins.entries.<id>.allowPrivateNetwork`. `runtime.media.saveMediaBuffer(buf, type, dir, maxBytes, name)` writes to the gateway media store. The `http_get` tool now uses the same guard; its old substring check is gone.
- real: `runtime.tts.textToSpeech` goes through the gateway TTS engine (`OPENCLAW_TTS_URL`). New STT interface `voice::SttEngine` with an OpenAI-compatible multipart backend (`OPENCLAW_STT_URL`, `OPENCLAW_STT_MODEL`, `OPENCLAW_STT_API_KEY`), exposed as `runtime.stt.transcribeAudioFile({filePath|buffer, mime, language})`.
- real: `runtime.config.writeConfigFile(cfg)` is schema-validated (invalid config rejected), written atomically with `.bak`, and picked up by hot reload.
- gaps: paragraph blocks stream via hostEvent (1.64); still no token-level streaming or typing keep-alive; TTS/STT have only the generic HTTP backends (no OpenAI/ElevenLabs/Deepgram presets, no telephony format); writeConfigFile writes plain JSON (comments and `$include` structure are not preserved); no `mutateConfigFile`. Tests: `scripts/e2e-plugin-runtime.sh` (smoke) and self-test (SSRF).

## 1.61 WhatsApp Web (Baileys-equivalent): assessment + first primitives
- assessment: `docs/whatsapp-web-gap.md` (layer-by-layer gap, ordered plan, what needs the user).
- real (offline, in-process tested): `channels::waweb` — `Noise_XX_25519_AESGCM_SHA256` symmetric state with the WA prologue (`WA` 6 3), both handshake roles (client = Baileys side, server = test responder), Baileys ordering (ClientFinish framed before `FinishInit`), transport framing (intro header, 3-byte length, split/merged frames), minimal protobuf `HandshakeMessage` codec, RFC 5869 HKDF (test vector), WhatsApp media encryption (HKDF type keys, AES-256-CBC, HMAC-SHA256/10), and the pairing QR payload.
- missing: live socket login, live ServerHello cert check (offline XEdDSA verify real since 1.68), ClientPayload/WAProto, ADV pairing signatures, SenderKeys, libsignal interop, pre-key upload, app-state/history sync, media hosts, auth persistence. Not interoperability-tested with WhatsApp servers.

## 1.60 Gemini + Voyage embedding providers
- real (fake-server tested): `memorySearch.provider: "gemini"` — `POST {baseUrl}/models/{model}:batchEmbedContents` with `x-goog-api-key`, `taskType` `RETRIEVAL_DOCUMENT` for chunks / `RETRIEVAL_QUERY` for queries, optional `memorySearch.outputDimensionality`, batches of 100; default `gemini-embedding-001` on `generativelanguage.googleapis.com/v1beta`, key `remote.apiKey` → `GEMINI_API_KEY` → `GOOGLE_API_KEY`.
- real (fake-server tested): `provider: "voyage"` — `POST {baseUrl}/embeddings` Bearer, `input_type` `document`/`query`, batches of 128; default `voyage-3.5` on `api.voyageai.com/v1`, key `remote.apiKey` → `VOYAGE_API_KEY`.
- real: `provider: "auto"` picks openai → gemini → voyage by available key, else local. New `EmbedQuery` hook for query-side task types. Failures still fall back to local with `fallback` reported.
- gaps: never run against live Gemini/Voyage (no creds; request/response shapes from public API docs); no Gemini batch-API async jobs, no Voyage multimodal/contextualized embeddings, no per-provider rate-limit backoff; mistral/ollama/local GGUF providers still missing. Test: `scripts/e2e-embeddings-gemini-voyage.sh` (smoke).

## 1.59 Persisted vector store (sqlite-vec equivalent)
- real: memory embeddings now persist in SQLite (`$OPENCLAW_DATA_DIR/memory/main.sqlite`, per-agent `agents/<id>/memory/index.sqlite`; supersedes the 1.47 `index.json` paths): table `embedding_cache(provider, model, hash, dims, embedding BLOB float32, updated_at)`, several provider/models side by side (switching models no longer wipes the cache), WAL, stale rows of the active model pruned when chunks change, legacy `*.index.json` auto-imported then renamed `.migrated`. SQL function `vec_distance_cosine(a, b)` + `VectorIndex::Knn` (ORDER BY distance LIMIT k). `memory search` reports `index.{hits,misses,pruned}`; `memory status` lists models/dims/entries.
- real-sim: "equivalent", not the sqlite-vec extension — KNN is a brute-force scan through a C++ UDF (no `vec0` virtual table / ANN index); search still scores every chunk in-process; chunks/FTS tables are not persisted (BM25 recomputed per query). Tests: self-test (KNN), `scripts/e2e-embeddings.sh` (sqlite rows, prune, legacy import).

## 1.58 ClawHub client (skills + plugins)
- real (fake-server tested): ClawHub `/api/v1` client — `CLAWHUB_REGISTRY` (legacy `CLAWDHUB_REGISTRY`), optional `CLAWHUB_TOKEN` Bearer, 429 `Retry-After`/`RateLimit-Reset` retry. `openclaw skills search|install <@owner/slug> [--version] [--force]|update [<slug>|--all]|uninstall|sync|installed|inspect` (also `openclaw clawhub …`). Skills land in `<workdir>/skills/<slug>` (`CLAWHUB_WORKDIR` or workspace), versions in `.clawhub/lock.json`, per-skill `.clawhub/origin.json`; moderation `isMalwareBlocked` refused; zip symlinks refused; update refuses to clobber locally modified installs.
- real: `plugins install clawhub:<pkg>[@ver]` — package resolve → version `/security` (`trust.blockedFromDownload` refused) → `/artifact` → `/artifact/download`; npm-pack `.tgz` verified against `artifactSha256`/`X-ClawHub-Artifact-Sha256`; legacy ZIP (`package/` root) extracted. Install record `source: clawhub` + artifact metadata; `plugins update` re-resolves latest. `plugins search <q>` (`/api/v1/plugins/search`).
- real-sim / gaps: `sync` uses our fingerprint (sha256 of sorted `path:sha256` lines) against `/api/v1/resolve` — upstream's exact bundle-fingerprint algorithm is not verified, so real-registry matches may differ; no publish/login/whoami/delete (registry-auth workflows belong to the separate `clawhub` CLI upstream); GitHub source-handoff skills not supported; `pluginApi`/`minGatewayVersion` compat checks not enforced; no install telemetry. Not run against live clawhub.ai. Tests: `scripts/e2e-clawhub.sh` + `scripts/fake_clawhub.py` (smoke).

## 1.57 exec mode=auto reviewer + PTY/send-keys
- real (mock-tested): `tools.exec.mode: "auto"` = allowlist + on-miss, but a miss first goes to a model reviewer through the provider interface (`ReviewWithProvider`, gateway router/default provider, no tools). Strict JSON verdict `allow|deny|escalate`; `allow` with `risk=high` (or inline-eval without `risk=low`) → escalate; malformed/failed reply → escalate (or deny with `autoReview.onError: "deny"`); escalate → normal human approval/askFallback. Event `exec.autoreview`. Operand SHA revalidation still applies.
- real: `exec {pty:true}` runs in a pseudo-terminal (posix_openpt, setsid, controlling TTY, 80x24) as a background session; `process` actions `write`, `send-keys` (tmux-style names: Enter, C-x, M-x, arrows, F1–F12…), `submit`, `paste` (bracketed). Tests: self-test (reviewer verdicts, isatty + line input), `scripts/e2e-exec-auto.sh` (smoke).
- real-sim / gaps: reviewer prompt is ours, not upstream's verbatim; `autoReview.model` is parsed but the reviewer uses the session's default route (no per-call model override); no live-LLM reviewer quality evaluation; PTY resize not exposed; sandbox host ignores pty.

## 1.43 JS/TS plugin host (upstream API)
- real: host.mjs v2 discovers openclaw.plugin.json / package.json `openclaw.extensions` / legacy plugin.json across repo plugins/, ~/.openclaw/extensions, <workspace>/.openclaw/extensions, plugins.load.paths; honours plugins.enabled/allow/deny and entries.<id>.enabled/config (api.pluginConfig).
- real: api.registerTool (execute/handler, factory), registerCommand (gateway runs before skills, no model turn), api.on hooks: message_received, before_agent_start (prependContext/systemPrompt), message_sending (content rewrite / cancel), agent_end; registerGatewayMethod (DispatchRpc fallback); registerService (start/stop).
- (1.46: TS + channel/provider/http/CLI/tool & session hooks became real — see below.)

## 1.44 Slack interactive
- real: reply directives `[[buttons: L|v,...]]`, `[[select: P|a,b]]`, `[[modal: Title|F1,F2]]` → Block Kit (section + actions); plain text keeps a fallback.
- real: block_actions (button/static_select), view_submission, shortcut/message_action over HTTP (`payload` form field or JSON) and Socket Mode `interactive` envelopes → agent turn with upstream-style session key; reply posted with blocks/thread_ts via chat.postMessage.
- real: modal button → views.open with plain_text_input per field (live when SLACK_BOT_TOKEN, SLACK_API_BASE overridable; recorded otherwise).
- real-sim: no live Slack test (no creds); response_url replies + signing covered in 1.50/1.54 (offline e2e).

## 1.45 Discord slash commands + native chat commands
- real: application command specs built from native (/new /reset /status /help /compact /model /stop) + user-invocable skills + plugin commands (sanitized, dedup, ≤100, optional `args` string option); `discord.commands` / `discord.commands.sync` RPC; PUT /applications/{app}[/guilds/{g}]/commands live with DISCORD_BOT_TOKEN+DISCORD_APPLICATION_ID (auto on adapter start), recorded otherwise.
- real: APPLICATION_COMMAND + message-component interactions via HTTP (type 4 response) and Gateway INTERACTION_CREATE (deferred type 5 ack + PATCH @original follow-up) → agent pipeline with upstream session keys.
- real: native /status, /help, /stop, /model handled in gateway without a model turn.
- real-sim: no live Discord test; Ed25519 in 1.50; /stop aborts in-flight session run and /model session override in 1.54.

## 1.46 Plugin API completion
- real: TypeScript plugin entries on Node >= 22.6 (`--experimental-strip-types` auto-added; OPENCLAW_NODE selects the binary) or with the `typescript` package; tested by scripts/e2e-ts-plugin.sh with Node 22.20 (skips honestly when no Node >= 22.6).
- real: api.registerHttpRoute({path, handler, match: exact|prefix, auth: gateway|plugin}) / registerHttpHandler served by the gateway HTTP server (Node-style req/res shim).
- real: api.registerCli (commander-compatible shim: command/description/option/argument/action) → `openclaw-cpp <cmd> ...` and `plugins run|cli`.
- real: before_tool_call (block / blockReason / params rewrite) and after_tool_call (result, durationMs, error) in the agent loop; session_start / session_end on new, reset and policy reset.
- real-sim: api.registerChannel — openclaw-cpp contract is `inbound.parse(body)` → messages + `outbound.sendText({to,text})`; upstream ChannelPlugin's full adapter surface (gateway start/stop, pairing, directory, actions, threading) is missing.
- real-sim: api.registerProvider with `complete({model, messages, tools})` → router route `<id>`; upstream provider plugins are auth/catalog descriptors (OAuth flows, model catalogs) — not implemented; a route uses the provider's first model.
- missing (install landed in 1.55): plugin runtime helpers (api.runtime.*), tool_result_persist / before_compaction hooks.

## 1.47 Multi-agent bindings
- real: agents.list (id, name, default, workspace, model string|{primary}, identity.name, systemPrompt) + bindings[{agentId, match:{channel, accountId, peer:{kind,id}, guildId, teamId}}]; most-specific wins (peer > guild > team > account > channel), first listed on ties; unknown agentIds ignored; missing accountId only matches the default account.
- real: inbound session keys use the routed agent (`agent:<id>:...`); per-agent system prompt (agent workspace bootstrap files + identity/systemPrompt, cached, cleared on reload) and per-agent model route via the provider router; `agents.list` / `agents.route` RPC.
- real-sim (superseded by 1.51): per-agent sessions/workspace/memory/tools; skills + JS host still process-global.

## 1.48 exec tool: real by default + approvals/sandbox semantics
- real: `exec` tool (legacy `shell` alias) runs for real by default (gateway host, security=full, ask=off — upstream gateway defaults); params command/workdir/env/timeout/host/ask; host env overrides of PATH/LD_*/DYLD_*/NODE_OPTIONS rejected. OPENCLAW_EXEC_MODE=mock keeps a mock for tests.
- real: policy = stricter of config (tools.exec.mode deny|allowlist|ask|auto|full or security/ask, per-agent agents.list[].tools.exec) and the approvals document (defaults + agents.<id>: security/ask/askFallback/allowlist); askFallback deny|allowlist|full when no approval UI.
- real: allowlist per agent — resolved-path globs (`**`), bare names only via PATH, argPattern (ECMAScript), every shell segment (&&, ||, ;, |) must match; $(…)/backticks/heredocs never allowlisted; safe bins (stdin-only) ; strictInlineEval (python -c, node -e, …).
- real: approval flow — exec.approval.requested event + prompt posted to the originating chat; resolve via `exec.approval.resolve` RPC, WS, or chat `/approve <id> allow-once|allow-always|deny` (requester or tools.exec.approvers only); allow-always persists exact-argv + cwd-bound entries; timeout → askFallback. Gateway HTTP now serves each client on its own thread so approvals can arrive mid-run.
- real: sandbox host — agents.defaults.sandbox.mode off|non-main|all (per-agent override) picks host; docker backend (`docker run --network none -v ws:/workspace`) or bwrap (read-only root, writable workspace, no network) when docker is absent; `openclaw-cpp approvals get|set`, `exec-policy show|preset yolo|cautious|deny-all`.
- real-sim (superseded in part by 1.53): see 1.53 for SQLite store, binary-hash binding, background/process; bwrap remains the local substitute when docker is absent; no node host; no LLM auto-reviewer for mode=auto (still allowlist+on-miss like ask).

## 1.49 Hot-reload channel/plugin restart coverage
- real: scripts/e2e-hot-reload-channels.sh (part of smoke) proves a reload restarts only adapters whose channels.<id> subtree changed, restarts nothing on unrelated edits, adapters keep serving afterwards, and a plugins.* change restarts the JS host with the **new** plugin config (bug fixed: the host previously restarted with the stale config) and re-registers plugin channels/providers.
- fix: `tools.invoke` RPC accepts plain-text tool results (JS plugin tools) instead of failing JSON parse.
- real-sim (superseded by 1.54): adapters SetConfig + ChannelCred; Start exports config tokens into env for Socket/Gateway children.
- note (superseded by 1.52): the “config reload swaps `deps.config` without a reader lock” gap is closed in **1.52** (exclusive `deps_mu_` + bounded HTTP workers + e2e).

## 1.50 Webhook authenticity + form bodies
- real: Slack signing secret (SLACK_SIGNING_SECRET or channels.slack.signingSecret): HMAC-SHA256 `v0:ts:body`, constant-time compare, 5-minute replay window → 401 otherwise.
- real: Discord interactions Ed25519 (DISCORD_PUBLIC_KEY or channels.discord.publicKey) over `X-Signature-Timestamp + body` → 401 otherwise.
- real: application/x-www-form-urlencoded inbound bodies (Slack slash commands and interactive `payload=`); HTTP Slack slash commands answer with `response_type`/`text`/`blocks`.
- unchanged: other webhook channels still use shared verify tokens only.

## 1.51 Per-agent isolation
- real: each agents.list entry gets its own sessions root (`$OPENCLAW_AGENTS_DIR|<state|data>/agents/<id>/sessions`; `main` keeps `OPENCLAW_SESSION_DIR` when set for back-compat), workspace (`agents.list[].workspace` or `agents/<id>/workspace`), memory index (`agents/<id>/memory/index.json`; main keeps `$OPENCLAW_DATA_DIR/memory/main.index.json`), and agent files under `AgentsRoot()/<id>/`.
- real: tools.allow / tools.deny / tools.profile (`minimal|coding|messaging|full`) from global `tools.*`, then `agents.defaults.tools`, then `agents.list[]|entries.<id>.tools` — deny wins; profile supplies a base allow set (coding includes exec/read/write/edit/http_get/browser_*).
- real: per-agent sandbox object (mode/backend/…) still flows into exec ResolvePolicy; agent Pool rebuilds tool registries on config reload and after JS plugin host restart (re-RegisterTools into the shared base).
- real-sim: skills library and JS plugin host remain process-global (not per-agent); MCP tool list is shared then filtered by allow/deny.

## 1.56 Plugin contracts: api.runtime, ChannelPlugin gateway/accounts, provider catalog
- real: `api.runtime` subset — `config.loadConfig/current`, `state.resolveStateDir/resolvePluginStateDir`, `logging.getChildLogger`, `system.enqueueSystemEvent`, `channel.sendMessage` + `channel.text.chunkText`, `agent.dispatchInbound`, `media.detectMime`. Host side effects are queued in the Node host and drained by a gateway pump (`drain_events`, ~200 ms) → system events / outbound sends / agent turns.
- real: ChannelPlugin surface — `meta`, `capabilities`, `config.listAccountIds/resolveAccount`, `gateway.startAccount(ctx{accountId, account, cfg, abortSignal, onMessage, setStatus, log})` / `stopAccount`, `outbound.sendText` + `sendMedia`, HTTP `inbound.parse`. Gateway starts every account on load and aborts on plugin refresh/shutdown; inbound → agent → `outbound.sendText`.
- real: provider plugins — `models` + `catalog.run()` (models merged, cached, `refresh`), `auth[]` descriptors listed; each catalog model routable as `<provider>/<model>` (router exact match first). RPC `plugins.providers`, `plugins.channels`, `plugins.events.drain`; `chat.abort` now aborts active session runs.
- real: `scripts/e2e-plugin-contracts.sh` (smoke, installed plugin, offline).
- real-sim / missing: runtime is a subset (no runtime.tts/stt, media fetch/store, reply dispatcher with typing/blocks, pairing/directory/actions/threading adapters, `config.writeConfigFile`); provider `auth` methods are listed but OAuth/API-key flows are not run; pump processes inbound sequentially.

## 1.55 plugins install / uninstall / update
- real: `openclaw-cpp plugins install <dir|.tgz|npm-spec> [--force] [--link] [--no-deps]` → `$OPENCLAW_EXTENSIONS_DIR|~/.openclaw/extensions/<id>` (id from openclaw.plugin.json, else package.json name; requires openclaw.plugin.json or package.json#openclaw). npm specs go through `npm pack` (so registry, `file:`, git specs work when npm can reach them); deps installed with `npm install --omit=dev --ignore-scripts` only when the package has dependencies and no node_modules. Staged copy + atomic rename; symlinks escaping the archive rejected.
- real: install record `plugins.installs.<id>` {source, spec, installPath, version, installedAt} + `plugins.entries.<id>.enabled=true`; `plugins update <id>` re-installs from the recorded spec; `plugins uninstall <id>` removes dir + records; `plugins installs` lists. JS host scans the extensions dir so installed tools load.
- real: `scripts/e2e-plugins-install.sh` (smoke, offline: dir, npm-pack tarball, `file:` spec, update, uninstall, non-plugin rejection).
- missing: registry-backed integrity pinning / signature checks, ClawHub plugin source, `plugins enable/disable` CLI parity flags.

## 1.54 Slack response_url · Discord /stop /model · config credentials
- real: Slack interactive + slash replies POST to Slack `response_url` when present (Block Kit preserved); covered by `scripts/e2e-stop-model-response-url.sh`.
- real: `/stop` sets a per-session cancel flag that aborts an in-flight agent loop (Discord slash `/stop` and chat); `/model [name|clear|list|status]` sets a session model override used via the provider router.
- real: built-in adapters call `SetConfig` on gateway start / hot-reload / channel restart and resolve tokens via env **or** `channels.<id>.*` (botToken/token/appToken/…). Start exports missing env from config so Socket Mode / Discord Gateway children also see credentials.
- real-sim: Socket/Gateway still primarily read process env after export; no per-account multi-token rotation.

## 1.53 Exec: SQLite approvals, binary revalidation, background/process
- real: approvals policy stored in `$OPENCLAW_STATE_DIR|$OPENCLAW_DATA_DIR/state/openclaw.sqlite` key `exec_approvals_config` (upstream locator); legacy `exec-approvals.json` migrated once to `.migrated`. `OPENCLAW_EXEC_APPROVALS_PATH` still forces JSON for tests.
- real: before approval wait, snapshot SHA-256 of resolved executables + existing script/file operands; after allow-once/always, revalidate and deny with `operand changed after approval` if bytes differ. `allow-always` with mutable script operands is downgraded to one-shot.
- real: `yieldMs` / `background` return `{status:running, sessionId}`; companion `process` tool (`list|poll|log|kill`) scoped per agent. Docker sandbox used when `docker` is on PATH, else bwrap (honest when neither available).
- real: `scripts/e2e-exec-sqlite-bg.sh` (smoke) covers migrate + binary-change denial + background list.
- real-sim / remaining: no LLM `tools.exec.reviewer` for mode=auto (auto ≡ ask = allowlist + on-miss); no PTY/send-keys/paste; no node host; WaitBackground overall timeout still SIGKILLs.

## 1.52 Config reload vs concurrent HTTP (exclusive lock + worker pool)
- real: `Server` holds exclusive `deps_mu_` (`std::mutex`). Request handlers and `ReloadConfigNow` serialize on it for config/tools/sessions swaps; adapter Stop/Start and the agent model/tool loop run **after** releasing the lock so reload cannot deadlock against a long run. `config.reload` RPC is handled without holding the lock across adapters.
- real: earlier `shared_mutex` + unbounded `std::thread().detach()` per HTTP client OOMed under reload hammers; HTTP is now a **bounded worker pool** (default 8, `OPENCLAW_GATEWAY_WORKERS`, queue cap 256 → HTTP 503). WebSocket upgrades still detach (long-lived).
- real: per-agent `tools::Registry` is a `shared_ptr` so in-flight agent runs keep a live registry across SetConfig rebuilds (`tools.reset()` + lazy rebuild on next Get).
- real: `scripts/e2e-config-lock.sh` (part of smoke) hammers concurrent `config.reload` + Slack inbound + `tools.list` and asserts the gateway stays healthy.
- real-sim / remaining: not a full formal proof of every deps_ field; long agent loops may still observe a mix of old tools + new config if they re-read `deps_` without the lock for the whole run. Channel credential env re-read semantics unchanged from 1.49.
