#!/usr/bin/env node
/**
 * openclaw-cpp JS/TS plugin host — newline-delimited JSON-RPC over stdio.
 *
 * Loads upstream-style OpenClaw plugins:
 *   - manifest `openclaw.plugin.json` ({id, name, description, configSchema}) or package.json `openclaw.extensions`
 *   - entry exports `default function register(api)` or `{ id, register(api) }` (ESM or CJS; .ts via Node's
 *     --experimental-strip-types when available, else the `typescript` package if resolvable)
 *   - legacy openclaw-cpp `plugin.json` + `module.exports = { tools: [...] }`
 *
 * Plugin API subset: api.id/name/config/pluginConfig/logger/resolvePath, api.runtime.{config,state,logging,system,channel,agent,media},
 *   registerTool(tool|factory), registerCommand, on(hook)/registerHook, registerGatewayMethod, registerService.
 * RPC: initialize, list_plugins, list_tools, call_tool, list_commands, run_command, emit_hook,
 *      list_gateway_methods, gateway_method, shutdown, ping
 */
import fs from 'fs';
import path from 'path';
import os from 'os';
import { createRequire } from 'module';
import { fileURLToPath, pathToFileURL } from 'url';
import readline from 'readline';

const require = createRequire(import.meta.url);
const __dirname = path.dirname(fileURLToPath(import.meta.url));
// stdout is the RPC channel: route plugin console output to stderr.
for (const k of ['log', 'info', 'debug']) console[k] = (...a) => process.stderr.write(a.map(String).join(' ') + '\n');

const plugins = new Map();      // id -> {id, name, path, manifest, kind, status, error}
const tools = new Map();        // js.<name> -> {plugin, tool}
const commands = new Map();     // name -> {plugin, cmd}
const hooks = new Map();        // event -> [{plugin, fn, priority}]
const gwMethods = new Map();    // name -> {plugin, fn}
const services = [];            // {plugin, svc}
const channels = new Map();     // id -> {plugin, ch}
const providers = new Map();    // id -> {plugin, p}
const httpRoutes = [];          // {plugin, path, match, auth, handler}
const registrations = new Map(); // register* kind -> [{plugin, value, opts}] (1.130: every upstream api.register* is accepted)
const CAPTURED_KINDS = ['registerAgentEventSubscription', 'registerAgentExecutorController', 'registerAgentHarness', 'registerAgentToolResultMiddleware',
  'registerAutoEnableProbe', 'registerBoardWidgetContentKind', 'registerCliBackend', 'registerCodexAppServerExtensionFactory', 'registerCompactionProvider',
  'registerConfigMigration', 'registerContextEngine', 'registerControlUiDescriptor', 'registerDecisionProvider', 'registerEmbeddingProvider',
  'registerGatewayAccessPolicy', 'registerGatewayDiscoveryService', 'registerHostedMediaResolver', 'registerImageGenerationProvider',
  'registerInteractiveHandler', 'registerMcpServerConnectionResolver', 'registerMediaUnderstandingProvider', 'registerMemoryCapability',
  'registerMemoryCorpusSupplement', 'registerMemoryPromptPreparation', 'registerMemoryPromptSupplement', 'registerMigrationProvider',
  'registerModelCatalogProvider', 'registerMusicGenerationProvider', 'registerNodeCliFeature', 'registerNodeHostCommand', 'registerNodeInvokePolicy',
  'registerRealtimeTranscriptionProvider', 'registerRealtimeVoiceProvider', 'registerReload', 'registerRuntimeLifecycle', 'registerSecurityAuditCollector',
  'registerSessionAction', 'registerSessionCatalog', 'registerSessionExtension', 'registerSessionSchedulerJob', 'registerSpeechProvider',
  'registerStorageProvider', 'registerTextTransforms', 'registerToolMetadata', 'registerTranscriptSourceProvider', 'registerTrustedToolPolicy',
  'registerVideoGenerationProvider', 'registerWebFetchProvider', 'registerWebSearchProvider', 'registerWidgetPresenter', 'registerWorkerProvider'];
function regs(kind) { return registrations.get(kind) || []; }
// 1.142: domain consumers for context engine / memory prompt supplements / agent harness / speech providers.
const engineCache = new Map();
async function contextEngine(id) {
  const list = regs('registerContextEngine');
  const entry = id ? list.find((r) => r.value && r.value.id === id) : null;
  if (!entry) return null;
  if (engineCache.has(id)) return engineCache.get(id);
  const v = entry.value;
  let eng = null;
  if (typeof v.factory === 'function') eng = await v.factory({ config: hostConfig, runtime: makeRuntime({ id: entry.plugin }) });
  else if (v && typeof v.assemble === 'function') eng = v;
  engineCache.set(id, eng);
  return eng;
}
function assistantText(r) {
  if (r == null) return null;
  if (typeof r === 'string') return r;
  if (Array.isArray(r.assistantTexts)) return r.assistantTexts.join('\n');
  for (const k of ['finalText', 'text', 'output', 'reply']) if (typeof r[k] === 'string') return r[k];
  if (r.result !== undefined) return assistantText(r.result);
  return null;
}

function toolResultText(r) {
  if (typeof r === 'string') return r;
  if (r && Array.isArray(r.content)) return r.content.filter((c) => c && c.type === 'text').map((c) => c.text).join('\n');
  return JSON.stringify(r);
}
function applyReplacements(list, text) {
  for (const { from, to } of list) {
    if (from instanceof RegExp) text = text.replace(from.global ? from : new RegExp(from.source, from.flags + 'g'), to);
    else if (typeof from === 'string' && from) text = text.split(from).join(to);
  }
  return text;
}
const cliCommands = new Map();  // top-level name -> {plugin, cmd}
let hostConfig = {};
const events = [];            // queued host-side effects drained by the gateway (drain_events)
const channelRuns = new Map();  // `${channel}:${account}` -> {abort, promise}
const catalogCache = new Map(); // provider id -> resolved catalog
let loadSeq = 0;

function readJson(p) { try { return JSON.parse(fs.readFileSync(p, 'utf8')); } catch { return null; } }

function pluginEntries(dir) {
  const up = readJson(path.join(dir, 'openclaw.plugin.json'));
  const pkg = readJson(path.join(dir, 'package.json'));
  const legacy = readJson(path.join(dir, 'plugin.json'));
  if (up) {
    let entries = [];
    if (pkg && pkg.openclaw && Array.isArray(pkg.openclaw.extensions)) entries = pkg.openclaw.extensions;
    else for (const c of ['index.ts', 'index.mjs', 'index.js', 'src/index.ts', 'dist/index.js']) if (fs.existsSync(path.join(dir, c))) { entries = [c]; break; }
    return { kind: 'openclaw', manifest: up, id: up.id || path.basename(dir), entries };
  }
  if (pkg && pkg.openclaw && Array.isArray(pkg.openclaw.extensions))
    return { kind: 'openclaw', manifest: pkg, id: (pkg.name || path.basename(dir)).replace(/^@[^/]+\//, ''), entries: pkg.openclaw.extensions };
  if (legacy && legacy.js) return { kind: 'legacy', manifest: legacy, id: legacy.name || path.basename(dir), entries: [legacy.js || legacy.main || 'index.js'] };
  return null;
}

function stripTypesSupported() {
  const [maj, min] = process.versions.node.split('.').map(Number);
  return maj > 22 || (maj === 22 && min >= 6);
}

async function importEntry(file) {
  if (file.endsWith('.ts') || file.endsWith('.mts')) {
    if (!stripTypesSupported() || !process.execArgv.includes('--experimental-strip-types')) {
      let ts;
      try { ts = require('typescript'); } catch {
        throw new Error('TypeScript plugin needs Node >= 22.6 (--experimental-strip-types) or the `typescript` package');
      }
      const out = ts.transpileModule(fs.readFileSync(file, 'utf8'), {
        compilerOptions: { module: ts.ModuleKind.ESNext, target: ts.ScriptTarget.ES2022 }, fileName: file,
      }).outputText;
      const tmp = path.join(os.tmpdir(), `oc-plugin-${process.pid}-${++loadSeq}.mjs`);
      fs.writeFileSync(tmp, out);
      try { return await import(pathToFileURL(tmp).href); } finally { fs.rmSync(tmp, { force: true }); }
    }
  }
  if (file.endsWith('.cjs') || (file.endsWith('.js') && !isEsmPackage(path.dirname(file)))) {
    try { delete require.cache[require.resolve(file)]; } catch {}
    try { return { default: require(file) }; } catch (e) {
      if (!String(e).includes('ERR_REQUIRE_ESM')) throw e;
    }
  }
  return await import(pathToFileURL(file).href + `?v=${++loadSeq}`);
}
function isEsmPackage(dir) {
  for (let d = dir; d !== path.dirname(d); d = path.dirname(d)) {
    const p = readJson(path.join(d, 'package.json'));
    if (p) return p.type === 'module';
  }
  return false;
}

// Minimal commander-compatible shim for api.registerCli.
function makeProgram(pluginId) {
  const mk = (name, parent) => {
    const c = { _name: name, _desc: '', _opts: [], _action: null, _subs: [], _args: [] };
    c.command = (spec) => {
      const [n, ...rest] = String(spec).trim().split(/\s+/);
      const sub = mk(n, c); sub._args = rest; c._subs.push(sub);
      if (!parent) cliCommands.set(n, { plugin: pluginId, cmd: sub });
      return sub;
    };
    c.description = (d) => { c._desc = d; return c; };
    c.alias = () => c;
    c.argument = (a) => { c._args.push(a); return c; };
    c.option = (flags, desc, def) => { c._opts.push({ flags, desc, def }); return c; };
    c.requiredOption = c.option;
    c.action = (fn) => { c._action = fn; return c; };
    return c;
  };
  return mk('openclaw', null);
}

function parseCliArgs(cmd, argv) {
  const opts = {}, pos = [];
  for (const o of cmd._opts) {
    const long = (o.flags.match(/--([\w-]+)/) || [])[1];
    if (long && o.def !== undefined) opts[long.replace(/-(\w)/g, (_, x) => x.toUpperCase())] = o.def;
  }
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a.startsWith('--')) {
      const [k, v] = a.slice(2).split('=');
      const key = k.replace(/-(\w)/g, (_, x) => x.toUpperCase());
      const spec = cmd._opts.find((o) => o.flags.includes('--' + k));
      if (v !== undefined) opts[key] = v;
      else if (spec && /[<[]/.test(spec.flags) && i + 1 < argv.length) opts[key] = argv[++i];
      else opts[key] = true;
    } else pos.push(a);
  }
  return { opts, pos };
}


// api.runtime.* subset. Host side effects are queued and drained by the gateway (drain_events RPC):
//   system.enqueueSystemEvent, channel.sendMessage (outbound), agent.dispatchInbound (agent turn).
// Plugin → host reverse calls: {"hostCall":id,method,params} on stdout; host answers {"hostReply":id,result|error}.
// Streaming: {"hostEvent":id,type,...} may arrive before the final hostReply (e.g. reply.block).
const pendingHost = new Map();
let hostSeq = 0;
function hostCall(rec, method, params, onEvent) {
  return new Promise((resolve, reject) => {
    const id = ++hostSeq;
    pendingHost.set(id, { resolve, reject, onEvent: typeof onEvent === 'function' ? onEvent : null });
    process.stdout.write(JSON.stringify({ hostCall: id, method, params: { ...(params || {}), plugin: rec.id } }) + '\n');
  });
}

function makeRuntime(rec) {
  const stateDir = () => process.env.OPENCLAW_STATE_DIR || path.join(os.homedir(), '.openclaw');
  const push = (e) => { events.push({ plugin: rec.id, at: Date.now(), ...e }); return { queued: true }; };
  const child = (name) => {
    const log = (lvl) => (...a) => process.stderr.write(`[plugin:${rec.id}:${name}] ${lvl}: ${a.map(String).join(' ')}\n`);
    return { info: log('info'), warn: log('warn'), error: log('error'), debug: log('debug') };
  };
  return {
    version: '2026.10.5-cpp', host: 'openclaw-cpp',
    config: {
      loadConfig: () => hostConfig, current: () => hostConfig,
      writeConfigFile: (cfg) => hostCall(rec, 'config.write', { config: cfg }),
    },
    state: { resolveStateDir: stateDir, resolvePluginStateDir: () => { const d = path.join(stateDir(), 'plugins', rec.id); fs.mkdirSync(d, { recursive: true }); return d; } },
    logging: { getChildLogger: (opts) => child((opts && opts.name) || 'child'), shouldLogVerbose: () => !!process.env.OPENCLAW_VERBOSE },
    system: { enqueueSystemEvent: (text, opts) => push({ type: 'system', text: String(text), sessionKey: opts && opts.sessionKey }) },
    channel: {
      sendMessage: ({ channel, to, text, accountId }) => push({ type: 'outbound', channel, to, text: String(text ?? ''), accountId }),
      reply: {
        // Streamed block dispatcher: host emits reply.block events as paragraphs become available, then a final hostReply.
        // Safe to call from tools/commands (deferred on a gateway thread; write lock is separate from the RPC lock).
        dispatchReplyWithBufferedBlockDispatcher: async ({ ctx = {}, dispatcherOptions = {} } = {}) => {
          const d = dispatcherOptions;
          const counts = { block: 0, final: 0 };
          try {
            if (typeof d.onReplyStart === 'function') await d.onReplyStart();
            const r = await hostCall(rec, 'agent.reply', {
              channel: ctx.channel || ctx.Provider, from: ctx.from || ctx.From || ctx.SenderId, text: ctx.text ?? ctx.Body ?? '',
              conversationId: ctx.conversationId || ctx.To, sessionKey: ctx.sessionKey || ctx.SessionKey, accountId: ctx.accountId || ctx.AccountId,
              chatType: ctx.chatType || ctx.ChatType, blockLimit: d.blockLimit || 2000, stream: d.stream !== false },
              async (ev) => {
                if (ev.type === 'reply.delta') { if (typeof d.onPartialReply === 'function') await d.onPartialReply({ text: ev.text || '' }); return; }
                if (ev.type === 'reply.typing') { if (typeof d.onTyping === 'function') await d.onTyping(); return; }
                if (ev.type !== 'reply.block') return;
                const kind = ev.kind || 'block';
                await d.deliver({ text: ev.text || '' }, { kind });
                if (kind === 'final') counts.final++; else counts.block++;
              });
            // Fallback: non-streaming host (or empty stream) delivers from the final payload.
            if (!r.streamed && counts.block + counts.final === 0) {
              const blocks = r.blocks || [];
              for (let i = 0; i < blocks.length; i++) {
                const kind = i === blocks.length - 1 ? 'final' : 'block';
                await d.deliver({ text: blocks[i] }, { kind });
                if (kind === 'final') counts.final++; else counts.block++;
              }
            }
            if (typeof d.onIdle === 'function') await d.onIdle();
            return { queuedFinal: counts.final > 0, counts, streamed: !!(r && r.streamed) };
          } catch (e) {
            if (typeof d.onError === 'function') { await d.onError(e, { kind: 'final' }); return { queuedFinal: false, counts: { block: 0, final: 0 } }; }
            throw e;
          }
        },
      },
      text: { chunkText: (t, limit = 2000) => { const out = []; for (let i = 0; i < t.length; i += limit) out.push(t.slice(i, i + limit)); return out; } },
    },
    agent: { dispatchInbound: (m) => push({ type: 'inbound', ...m }) },
    tts: {
      textToSpeech: ({ text, voice } = {}) => hostCall(rec, 'tts.synthesize', { text: String(text ?? ''), voice }),
    },
    stt: {
      transcribeAudioFile: async ({ filePath, buffer, mime, language } = {}) => {
        const buf = buffer ? Buffer.from(buffer) : fs.readFileSync(filePath);
        return hostCall(rec, 'stt.transcribe', { base64: buf.toString('base64'), mime, fileName: filePath ? path.basename(filePath) : undefined, language });
      },
    },
    media: {
      fetchRemoteMedia: async ({ url, maxBytes } = {}) => {
        const r = await hostCall(rec, 'media.fetch', { url, maxBytes });
        return { buffer: Buffer.from(r.base64 || '', 'base64'), contentType: r.contentType, fileName: r.fileName, url: r.finalUrl };
      },
      saveMediaBuffer: (buffer, contentType, _direction, maxBytes, fileName) => {
        const buf = Buffer.from(buffer);
        if (maxBytes && buf.length > maxBytes) return Promise.reject(new Error('media exceeds maxBytes'));
        return hostCall(rec, 'media.save', { base64: buf.toString('base64'), contentType, fileName });
      },
      detectMime: (name) => ({ png: 'image/png', jpg: 'image/jpeg', jpeg: 'image/jpeg', gif: 'image/gif', pdf: 'application/pdf', txt: 'text/plain' })[String(name).split('.').pop().toLowerCase()] || 'application/octet-stream' },
  };
}

function channelAccounts(ch) {
  try {
    if (ch.config && typeof ch.config.listAccountIds === 'function') { const ids = ch.config.listAccountIds(hostConfig); if (Array.isArray(ids) && ids.length) return ids; }
  } catch {}
  return ['default'];
}

function makeApi(rec) {
  const entryCfg = ((hostConfig.plugins || {}).entries || {})[rec.id] || {};
  const log = (lvl) => (...a) => process.stderr.write(`[plugin:${rec.id}] ${lvl}: ${a.map(String).join(' ')}\n`);
  const api = {
    id: rec.id,
    name: rec.manifest.name || rec.id,
    config: hostConfig,
    pluginConfig: entryCfg.config || {},
    logger: { info: log('info'), warn: log('warn'), error: log('error'), debug: log('debug') },
    runtime: makeRuntime(rec),
    resolvePath: (p) => (p.startsWith('~') ? path.join(os.homedir(), p.slice(1)) : path.resolve(rec.path, p)),
    registerTool(tool, opts) {
      const list = typeof tool === 'function' ? [].concat(tool({ config: hostConfig, pluginConfig: api.pluginConfig }) || []) : [tool];
      for (const t of list) {
        if (!t || !t.name || (typeof t.execute !== 'function' && typeof t.handler !== 'function')) continue;
        tools.set(`js.${t.name}`, { plugin: rec.id, tool: t, optional: !!(opts && opts.optional) });
      }
    },
    registerCommand(cmd) {
      if (!cmd || !cmd.name || typeof cmd.handler !== 'function') return;
      commands.set(String(cmd.name).replace(/^\//, '').toLowerCase(), { plugin: rec.id, cmd });
    },
    on(event, fn, opts) {
      if (typeof fn !== 'function') return;
      if (!hooks.has(event)) hooks.set(event, []);
      hooks.get(event).push({ plugin: rec.id, fn, priority: (opts && opts.priority) || 0 });
      hooks.get(event).sort((a, b) => b.priority - a.priority);
    },
    registerHook(events, fn, opts) { for (const e of [].concat(events)) api.on(e, fn, opts); },
    registerGatewayMethod(name, fn) { if (typeof fn === 'function') gwMethods.set(name, { plugin: rec.id, fn }); },
    registerService(svc) { if (svc && svc.id) services.push({ plugin: rec.id, svc }); },
    registerChannel(reg) {
      const ch = reg && reg.plugin ? reg.plugin : reg;
      if (!ch || !ch.id) return api.logger.warn('registerChannel: missing id');
      channels.set(ch.id, { plugin: rec.id, ch });
    },
    registerProvider(p) {
      if (!p || !p.id) return api.logger.warn('registerProvider: missing id');
      providers.set(p.id, { plugin: rec.id, p });
    },
    registerHttpRoute(r) {
      if (!r || !r.path || typeof r.handler !== 'function') return api.logger.warn('registerHttpRoute: need {path, handler}');
      httpRoutes.push({ plugin: rec.id, path: r.path, match: r.match || 'exact', auth: r.auth || 'gateway', handler: r.handler });
    },
    registerHttpHandler(fn) { if (typeof fn === 'function') httpRoutes.push({ plugin: rec.id, path: '/', match: 'any', auth: 'gateway', handler: fn }); },
    ...Object.fromEntries(CAPTURED_KINDS.map((kind) => [kind, (value, opts) => {
      if (value === undefined || value === null) return api.logger.warn(`${kind}: missing registration`);
      if (!registrations.has(kind)) registrations.set(kind, []);
      // registerContextEngine(id, factory): keep both so the engine can be instantiated on demand (1.142).
      if (kind === 'registerContextEngine' && typeof value === 'string') { value = { id: value, factory: opts }; opts = undefined; }
      registrations.get(kind).push({ plugin: rec.id, value, opts });
    }])),
    registerCli(fn, opts) {
      if (typeof fn !== 'function') return;
      const program = makeProgram(rec.id);
      try { fn({ program, config: hostConfig, logger: api.logger, workspaceDir: process.cwd() }); }
      catch (e) { api.logger.error('registerCli failed: ' + e); }
    },
  };
  return api;
}

async function loadPluginDir(dir) {
  const info = pluginEntries(dir);
  if (!info) return;
  const pcfg = hostConfig.plugins || {};
  const entryCfg = (pcfg.entries || {})[info.id] || {};
  const rec = { id: info.id, name: info.manifest.name || info.id, path: dir, manifest: info.manifest, kind: info.kind, status: 'loaded' };
  if (plugins.has(rec.id)) return;  // first discovered wins (precedence order)
  plugins.set(rec.id, rec);
  if (pcfg.enabled === false || entryCfg.enabled === false || info.manifest.enabled === false ||
      (Array.isArray(pcfg.deny) && pcfg.deny.includes(rec.id)) ||
      (Array.isArray(pcfg.allow) && pcfg.allow.length && !pcfg.allow.includes(rec.id))) { rec.status = 'disabled'; return; }
  try {
    for (const e of info.entries) {
      const mod = await importEntry(path.resolve(dir, e));
      const ex = mod.default !== undefined ? mod.default : mod;
      if (info.kind === 'legacy') {
        for (const t of Array.isArray(ex.tools) ? ex.tools : []) if (t && t.name && typeof t.handler === 'function') tools.set(`js.${t.name}`, { plugin: rec.id, tool: t });
        continue;
      }
      const register = typeof ex === 'function' ? ex : (ex && typeof ex.register === 'function' ? ex.register.bind(ex) : null);
      if (!register) throw new Error('plugin entry exports no register(api)');
      await register(makeApi(rec));
    }
  } catch (e) {
    rec.status = 'error';
    rec.error = String(e && e.message ? e.message : e);
    if (!/needs Node >= 22\.6/.test(rec.error) || process.env.OPENCLAW_PLUGIN_DEBUG) process.stderr.write(`js_bridge: plugin ${rec.id} failed: ${rec.error}\n`);
  }
}

async function scan(roots) {
  plugins.clear(); tools.clear(); commands.clear(); hooks.clear(); gwMethods.clear(); services.length = 0;
  channels.clear(); providers.clear(); httpRoutes.length = 0; cliCommands.clear(); registrations.clear();
  for (const root of roots) {
    if (!root || !fs.existsSync(root)) continue;
    if (pluginEntries(root)) { await loadPluginDir(root); continue; }
    for (const ent of fs.readdirSync(root, { withFileTypes: true })) {
      if (!ent.isDirectory() || ent.name === 'js_bridge' || ent.name === 'node_modules') continue;
      await loadPluginDir(path.join(root, ent.name));
    }
  }
  for (const { plugin, svc } of services) {
    try { if (typeof svc.start === 'function') await svc.start({ config: hostConfig }); }
    catch (e) { process.stderr.write(`js_bridge: service ${plugin}/${svc.id} start failed: ${e}\n`); }
  }
  for (const r of regs('registerRuntimeLifecycle')) {
    try { if (r.value && typeof r.value.start === 'function') await r.value.start({ config: hostConfig }); }
    catch (e) { process.stderr.write(`js_bridge: runtime lifecycle ${r.plugin} start failed: ${e}\n`); }
  }
}

function toText(r) {
  if (r == null) return '';
  if (typeof r === 'string') return r;
  if (Array.isArray(r.content)) return r.content.filter((c) => c && c.type === 'text').map((c) => c.text).join('\n');
  if (typeof r.content === 'string') return r.content;
  if (typeof r.text === 'string') return r.text;
  return JSON.stringify(r);
}

function respond(id, result, error) {
  const msg = { jsonrpc: '2.0', id };
  if (error) msg.error = error; else msg.result = result;
  process.stdout.write(JSON.stringify(msg) + '\n');
}

async function handle(req) {
  const { id, method, params = {} } = req || {};
  try {
    switch (method) {
      case 'ping': return respond(id, { ok: true, pong: true });
      case 'initialize': {
        hostConfig = params.config || {};
        const roots = Array.isArray(params.plugins_dirs) ? params.plugins_dirs : [params.plugins_dir || path.resolve(__dirname, '..')];
        await scan(roots);
        return respond(id, {
          ok: true, host: 'openclaw-cpp-js-host', version: '2', node: process.versions.node,
          plugins: [...plugins.keys()], tools: [...tools.keys()], commands: [...commands.keys()],
          hooks: [...hooks.keys()], gatewayMethods: [...gwMethods.keys()], services: services.map((s) => `${s.plugin}/${s.svc.id}`),
          channels: [...channels.keys()], providers: [...providers.keys()], cli: [...cliCommands.keys()],
          httpRoutes: httpRoutes.map((r) => ({ path: r.path, match: r.match, auth: r.auth, plugin: r.plugin })),
          registrations: Object.fromEntries([...registrations].map(([k, v]) => [k, v.length])),
        });
      }
      case 'list_plugins':
        return respond(id, { plugins: [...plugins.values()].map((p) => ({
          id: p.id, name: p.id, displayName: p.name, path: p.path, kind: p.kind, status: p.status, error: p.error,
          tools: [...tools.entries()].filter(([, v]) => v.plugin === p.id).map(([k]) => k.slice(3)),
          commands: [...commands.entries()].filter(([, v]) => v.plugin === p.id).map(([k]) => k),
        })) });
      case 'list_tools':
        return respond(id, { tools: [...tools.entries()].map(([name, v]) => ({
          name, description: v.tool.description || '', parameters: v.tool.parameters || { type: 'object' }, plugin: v.plugin,
        })) });
      case 'call_tool': {
        const ent = tools.get(params.name);
        if (!ent) return respond(id, null, { code: -32601, message: 'unknown tool: ' + params.name });
        const args = params.arguments || {};
        const out = typeof ent.tool.execute === 'function'
          ? await ent.tool.execute(params.toolCallId || `call_${Date.now()}`, args)
          : await ent.tool.handler(args);
        if (ent.tool.execute) return respond(id, { content: toText(out), raw: out });
        return respond(id, out);
      }
      case 'list_commands':
        return respond(id, { commands: [...commands.entries()].map(([name, v]) => ({ name, description: v.cmd.description || '', plugin: v.plugin, acceptsArgs: v.cmd.acceptsArgs !== false })) });
      case 'run_command': {
        const ent = commands.get(String(params.name || '').toLowerCase());
        if (!ent) return respond(id, null, { code: -32601, message: 'unknown command: ' + params.name });
        const ctx = { args: params.args || '', senderId: params.senderId, channel: params.channel, sessionKey: params.sessionKey,
                      config: hostConfig, isAuthorizedSender: params.authorized !== false, commandBody: `/${params.name} ${params.args || ''}`.trim() };
        const out = await ent.cmd.handler(ctx);
        return respond(id, { text: toText(out), raw: out ?? null });
      }
      case 'emit_hook': {
        const list = hooks.get(params.event) || [];
        const results = [];
        let payload = params.payload || {};
        for (const h of list) {
          try {
            const r = await h.fn(payload, params.ctx || {});
            if (r && typeof r === 'object') { results.push({ plugin: h.plugin, result: r }); payload = { ...payload, ...r }; }
          } catch (e) { results.push({ plugin: h.plugin, error: String(e) }); }
        }
        return respond(id, { event: params.event, handlers: list.length, results, merged: payload });
      }
      case 'list_gateway_methods': return respond(id, { methods: [...gwMethods.keys()] });
      case 'gateway_method': {
        const ent = gwMethods.get(params.method);
        if (!ent) return respond(id, null, { code: -32601, message: 'unknown gateway method: ' + params.method });
        let resolved = null, failed = null;
        const r = await ent.fn({ params: params.params || {}, respond: (ok, payload, err) => { if (ok) resolved = payload; else failed = err || payload; } });
        if (failed) return respond(id, null, { code: -32000, message: typeof failed === 'string' ? failed : JSON.stringify(failed) });
        return respond(id, resolved ?? r ?? {});
      }
      case 'http_request': {
        const p = String(params.path || '/');
        const route = httpRoutes.find((r) => r.match === 'any' ? false : (r.match === 'prefix' ? p.startsWith(r.path) : p === r.path))
          || httpRoutes.find((r) => r.match === 'any');
        if (!route) return respond(id, { handled: false });
        const res = { statusCode: 200, headers: {}, body: '', ended: false,
          setHeader(k, v) { this.headers[String(k).toLowerCase()] = String(v); },
          writeHead(code, h) { this.statusCode = code; if (h) for (const k in h) this.setHeader(k, h[k]); return this; },
          write(d) { this.body += String(d); return true; },
          end(d) { if (d !== undefined) this.body += String(d); this.ended = true; } };
        const req = { method: params.method || 'GET', url: p + (params.query ? '?' + params.query : ''), path: p,
          headers: params.headers || {}, body: params.body ?? '' };
        try { req.json = JSON.parse(req.body); } catch {}
        const r = await route.handler(req, res);
        if (route.match === 'any' && r === false && !res.ended) return respond(id, { handled: false });
        return respond(id, { handled: true, status: res.statusCode, headers: res.headers, body: res.body, plugin: route.plugin });
      }
      case 'list_cli':
        return respond(id, { commands: [...cliCommands.entries()].map(([n, v]) => ({ name: n, description: v.cmd._desc, plugin: v.plugin,
          subcommands: v.cmd._subs.map((s) => s._name) })) });
      case 'run_cli': {
        const argv = Array.isArray(params.argv) ? params.argv.map(String) : [];
        const ent = cliCommands.get(argv[0]);
        if (!ent) return respond(id, null, { code: -32601, message: 'unknown cli command: ' + argv[0] });
        let cmd = ent.cmd, rest = argv.slice(1);
        while (rest.length && cmd._subs.find((s) => s._name === rest[0])) { cmd = cmd._subs.find((s) => s._name === rest[0]); rest = rest.slice(1); }
        if (!cmd._action) {
          const help = [`Usage: openclaw ${argv[0]} <command>`, ...cmd._subs.map((s) => `  ${s._name}  ${s._desc}`)].join('\n');
          return respond(id, { output: help + '\n', exitCode: 1 });
        }
        const { opts, pos } = parseCliArgs(cmd, rest);
        let out = '';
        const orig = console.log;
        console.log = (...a) => { out += a.map((x) => (typeof x === 'string' ? x : JSON.stringify(x))).join(' ') + '\n'; };
        let exitCode = 0;
        try { await cmd._action(...pos, opts, cmd); } catch (e) { out += String(e && e.message ? e.message : e) + '\n'; exitCode = 1; }
        finally { console.log = orig; }
        return respond(id, { output: out, exitCode });
      }
      case 'list_channels':
        return respond(id, { channels: [...channels.entries()].map(([cid, v]) => ({ id: cid, plugin: v.plugin,
          label: (v.ch.meta && v.ch.meta.label) || cid, meta: v.ch.meta || {}, capabilities: v.ch.capabilities || {},
          outbound: !!(v.ch.outbound && v.ch.outbound.sendText), media: !!(v.ch.outbound && v.ch.outbound.sendMedia),
          inbound: !!(v.ch.inbound && typeof v.ch.inbound.parse === 'function'),
          gateway: !!(v.ch.gateway && typeof v.ch.gateway.startAccount === 'function'),
          accounts: channelAccounts(v.ch), running: [...channelRuns.keys()].filter((k) => k.startsWith(cid + ':')).map((k) => k.slice(cid.length + 1)) })) });
      case 'channel_gateway_start': {
        const ent = channels.get(params.channel);
        if (!ent || !ent.ch.gateway || typeof ent.ch.gateway.startAccount !== 'function')
          return respond(id, null, { code: -32601, message: 'channel has no gateway.startAccount: ' + params.channel });
        const started = [];
        for (const accountId of (params.accountId ? [params.accountId] : channelAccounts(ent.ch))) {
          const key = `${params.channel}:${accountId}`;
          if (channelRuns.has(key)) continue;
          const ac = new AbortController();
          let account = {};
          try { if (ent.ch.config && ent.ch.config.resolveAccount) account = ent.ch.config.resolveAccount(hostConfig, accountId) || {}; } catch {}
          const emit = (m) => events.push({ plugin: ent.plugin, type: 'inbound', channel: params.channel, accountId, at: Date.now(), ...m });
          const ctx = { accountId, account, cfg: hostConfig, abortSignal: ac.signal, log: makeRuntime({ id: ent.plugin }).logging.getChildLogger({ name: params.channel }),
                        onMessage: emit, emit, setStatus: (st) => events.push({ type: 'channel.status', channel: params.channel, accountId, status: st }) };
          const promise = Promise.resolve().then(() => ent.ch.gateway.startAccount(ctx)).catch((e) =>
            events.push({ type: 'channel.status', channel: params.channel, accountId, status: { error: String(e) } }));
          channelRuns.set(key, { abort: ac, promise });
          started.push(accountId);
        }
        return respond(id, { ok: true, started });
      }
      case 'channel_gateway_stop': {
        const stopped = [];
        for (const [key, run] of [...channelRuns.entries()]) {
          if (params.channel && !key.startsWith(params.channel + ':')) continue;
          run.abort.abort();
          const [cid, acc] = [key.slice(0, key.indexOf(':')), key.slice(key.indexOf(':') + 1)];
          const ent = channels.get(cid);
          try { if (ent && ent.ch.gateway && ent.ch.gateway.stopAccount) await ent.ch.gateway.stopAccount({ accountId: acc, cfg: hostConfig }); } catch {}
          channelRuns.delete(key); stopped.push(key);
        }
        return respond(id, { ok: true, stopped });
      }
      case 'channel_send_media': {
        const ent = channels.get(params.channel);
        if (!ent || !ent.ch.outbound || typeof ent.ch.outbound.sendMedia !== 'function')
          return respond(id, null, { code: -32601, message: 'channel has no outbound.sendMedia: ' + params.channel });
        const r = await ent.ch.outbound.sendMedia({ to: params.to, text: params.text || '', mediaUrl: params.mediaUrl, cfg: hostConfig, accountId: params.accountId });
        return respond(id, { ok: true, result: r ?? null });
      }
      case 'drain_events': {
        const out = events.splice(0, events.length);
        return respond(id, { events: out });
      }
      case 'channel_send': {
        const ent = channels.get(params.channel);
        if (!ent || !ent.ch.outbound || typeof ent.ch.outbound.sendText !== 'function')
          return respond(id, null, { code: -32601, message: 'channel has no outbound.sendText: ' + params.channel });
        const r = await ent.ch.outbound.sendText({ to: params.to, text: params.text, cfg: hostConfig, accountId: params.accountId });
        return respond(id, { ok: true, result: r ?? null });
      }
      case 'channel_inbound': {
        const ent = channels.get(params.channel);
        if (!ent || !ent.ch.inbound || typeof ent.ch.inbound.parse !== 'function')
          return respond(id, null, { code: -32601, message: 'channel has no inbound.parse: ' + params.channel });
        const r = await ent.ch.inbound.parse(params.body ?? {}, { cfg: hostConfig });
        const list = r == null ? [] : [].concat(r);
        return respond(id, { messages: list.filter((m) => m && typeof m.text === 'string') });
      }
      case 'list_providers': {
        const out = [];
        for (const [pid, v] of providers.entries()) {
          let models = (v.p.models || []).map((m) => (typeof m === 'string' ? { id: m } : m)).filter((m) => m && m.id);
          let catalog = null;
          if (v.p.catalog && typeof v.p.catalog.run === 'function') {
            try {
              if (!catalogCache.has(pid) || params.refresh) catalogCache.set(pid, await v.p.catalog.run({ config: hostConfig, env: process.env }));
              catalog = catalogCache.get(pid);
              const pm = catalog && catalog.provider && Array.isArray(catalog.provider.models) ? catalog.provider.models : (catalog && catalog.models) || [];
              for (const m of pm) { const mm = typeof m === 'string' ? { id: m } : m; if (mm && mm.id && !models.find((x) => x.id === mm.id)) models.push(mm); }
            } catch (e) { catalog = { error: String(e) }; }
          }
          out.push({ id: pid, plugin: v.plugin, label: v.p.label || pid, docsPath: v.p.docsPath,
            auth: (v.p.auth || []).map((a) => ({ id: a.id, label: a.label, kind: a.kind })),
            models: models.map((m) => m.id), modelInfo: models, catalog: catalog && catalog.error ? { error: catalog.error } : (catalog ? { ok: true } : null),
            chat: typeof v.p.complete === 'function' });
        }
        return respond(id, { providers: out });
      }
      case 'provider_complete': {
        const ent = providers.get(params.provider);
        if (!ent || typeof ent.p.complete !== 'function')
          return respond(id, null, { code: -32601, message: 'provider has no complete(): ' + params.provider });
        const r = await ent.p.complete({ model: params.model, messages: params.messages || [], tools: params.tools || [], config: hostConfig, auth: params.auth || null });
        if (typeof r === 'string') return respond(id, { content: r, tool_calls: [] });
        return respond(id, { content: (r && r.content) || '', tool_calls: (r && r.tool_calls) || [] });
      }
      case 'provider_auth_run': {
        const ent = providers.get(params.provider);
        if (!ent) return respond(id, null, { code: -32601, message: 'unknown provider: ' + params.provider });
        const methods = ent.p.auth || [];
        const m = params.method ? methods.find((a) => a.id === params.method) : methods[0];
        if (!m) return respond(id, null, { code: -32602, message: 'provider has no auth method' + (params.method ? ': ' + params.method : '') });
        const rec = { id: ent.plugin };
        const prompter = {
          text: (o) => hostCall(rec, 'prompt.text', { message: (o && o.message) || String(o), secret: !!(o && (o.secret || o.mask)) }),
          confirm: (o) => hostCall(rec, 'prompt.confirm', { message: (o && o.message) || String(o) }),
          select: (o) => hostCall(rec, 'prompt.select', { message: o.message, options: (o.options || []).map((x) => (typeof x === 'string' ? { value: x, label: x } : x)) }),
          note: (msg, title) => hostCall(rec, 'prompt.note', { message: String(msg), title }),
        };
        const ctx = { config: hostConfig, prompter, env: process.env, isRemote: false,
          openUrl: (url) => hostCall(rec, 'open.url', { url: String(url) }), runtime: makeRuntime(rec) };
        let result;
        if (typeof m.run === 'function') result = await m.run(ctx);
        else if (m.kind === 'api_key' || m.kind === 'token') {
          const key = await prompter.text({ message: `${ent.p.label || params.provider} API key`, secret: true });
          result = { profiles: [{ profileId: `${params.provider}:default`, credential: { type: 'api_key', provider: params.provider, key } }] };
        } else return respond(id, null, { code: -32602, message: 'auth method has no run(): ' + m.id });
        return respond(id, { method: m.id, kind: m.kind, ...(result || {}) });
      }
      case 'provider_auth_refresh': {
        const ent = providers.get(params.provider);
        if (!ent) return respond(id, null, { code: -32601, message: 'unknown provider: ' + params.provider });
        const fn = ent.p.refreshOAuth || (ent.p.oauth && ent.p.oauth.refresh) || ((ent.p.auth || []).find((a) => typeof a.refresh === 'function') || {}).refresh;
        if (typeof fn !== 'function') return respond(id, null, { code: -32601, message: 'provider has no OAuth refresh' });
        const cred = await fn(params.credential || {}, { config: hostConfig });
        return respond(id, { credential: cred });
      }
      case 'list_registrations':
        return respond(id, { registrations: Object.fromEntries([...registrations].map(([k, v]) => [k, v.map((r) => ({
          plugin: r.plugin, id: r.value && (r.value.id || r.value.name || r.value.kind) || null, label: r.value && r.value.label || null,
          type: typeof r.value }))])) });
      case 'web_search': {
        const want = params.provider;
        const entry = regs('registerWebSearchProvider').find((r) => !want || r.value.id === want);
        if (!entry) return respond(id, { ok: false, error: want ? `no plugin web search provider ${want}` : 'no plugin web search provider' });
        try {
          const tool = typeof entry.value.createTool === 'function' ? entry.value.createTool({ config: hostConfig }) : null;
          if (!tool || typeof tool.execute !== 'function') return respond(id, { ok: false, error: `provider ${entry.value.id} has no tool` });
          const result = await tool.execute(params.args || {}, {});
          return respond(id, { ok: true, provider: entry.value.id, plugin: entry.plugin, result });
        } catch (e) { return respond(id, { ok: false, provider: entry.value.id, error: String(e && e.message || e) }); }
      }
      case 'text_transform': {
        const dir = params.direction === 'input' ? 'input' : 'output';
        let text = String(params.text ?? '');
        for (const r of regs('registerTextTransforms')) if (Array.isArray(r.value[dir])) text = applyReplacements(r.value[dir], text);
        return respond(id, { text });
      }
      case 'tool_result_middleware': {
        let result = { content: [{ type: 'text', text: String(params.result ?? '') }] };
        const event = { toolCallId: params.toolCallId || '', toolName: params.toolName, args: params.args || {}, isError: !!params.isError, result };
        const ctx = { runtime: 'openclaw', agentId: params.agentId, sessionKey: params.sessionKey };
        let changed = false;
        for (const r of regs('registerAgentToolResultMiddleware')) {
          const m = r.opts && r.opts.matcher;
          if (m && ((typeof m === 'string' && m !== params.toolName) || (Array.isArray(m) && !m.includes(params.toolName)) ||
                    (m instanceof RegExp && !m.test(params.toolName)) || (typeof m === 'function' && !m(params.toolName)))) continue;
          try {
            const out = await r.value({ ...event, result }, ctx);
            if (out && out.result) { result = out.result; changed = true; }
          } catch (e) { process.stderr.write(`[plugin:${r.plugin}] tool result middleware failed: ${e}\n`); }
        }
        return respond(id, { changed, result: toolResultText(result) });
      }
      case 'context_engine_assemble': {
        const eng = await contextEngine(params.engineId);
        if (!eng || typeof eng.assemble !== 'function') return respond(id, { ok: false, error: `no context engine ${params.engineId}` });
        try {
          const r = await eng.assemble({ sessionId: params.sessionKey || 'main', sessionKey: params.sessionKey, messages: params.messages || [], tokenBudget: params.tokenBudget });
          return respond(id, { ok: true, messages: Array.isArray(r && r.messages) ? r.messages : null, systemPromptAddition: r && r.systemPromptAddition || null, estimatedTokens: r && r.estimatedTokens || null });
        } catch (e) { return respond(id, { ok: false, error: String(e && e.message || e) }); }
      }
      case 'context_engine_after_turn': {
        const eng = await contextEngine(params.engineId);
        try {
          if (eng && typeof eng.afterTurn === 'function') await eng.afterTurn({ sessionId: params.sessionKey || 'main', sessionKey: params.sessionKey, messages: params.messages || [], prePromptMessageCount: params.prePromptMessageCount || 0 });
          else if (eng && typeof eng.ingest === 'function') for (const m of (params.messages || []).slice(params.prePromptMessageCount || 0)) await eng.ingest({ sessionId: params.sessionKey || 'main', sessionKey: params.sessionKey, message: m });
          return respond(id, { ok: true });
        } catch (e) { return respond(id, { ok: false, error: String(e && e.message || e) }); }
      }
      case 'memory_prompt_supplements': {
        const out = [];
        for (const r of regs('registerMemoryPromptSupplement')) {
          const v = r.value; const fn = typeof v === 'function' ? v : v && (v.build || v.execute || v.supplement);
          try {
            const res = typeof fn === 'function' ? await fn.call(v, { agentId: params.agentId, sessionKey: params.sessionKey, config: hostConfig }) : null;
            const lines = Array.isArray(res) ? res : typeof res === 'string' ? [res] : res && typeof res.text === 'string' ? [res.text] : res && Array.isArray(res.lines) ? res.lines : [];
            for (const l of lines) if (typeof l === 'string' && l.trim()) out.push(l);
          } catch (e) { process.stderr.write(`js_bridge: memory prompt supplement ${r.plugin} failed: ${e}\n`); }
        }
        return respond(id, { ok: true, text: out.join('\n') });
      }
      case 'harness_run': {
        const entry = regs('registerAgentHarness').find((r) => r.value && r.value.id === params.harnessId);
        if (!entry) return respond(id, { ok: false, error: `no agent harness ${params.harnessId}` });
        const h = entry.value;
        try {
          const ctx = { provider: params.provider, modelId: params.model, agentId: params.agentId };
          if (typeof h.supports === 'function') {
            const sup = await h.supports(ctx);
            if (sup === false || (sup && sup.supported === false)) return respond(id, { ok: false, unsupported: true, reason: sup && sup.reason || 'unsupported' });
          }
          const run = h.runAttempt || h.run || h.execute;
          if (typeof run !== 'function') return respond(id, { ok: false, error: 'harness has no runAttempt' });
          const r = await run.call(h, { prompt: params.prompt, messages: params.messages || [], provider: params.provider, modelId: params.model, agentId: params.agentId, sessionKey: params.sessionKey, config: hostConfig });
          const text = assistantText(r);
          return respond(id, { ok: text !== null, text, error: text === null ? 'harness returned no assistant text' : undefined });
        } catch (e) { return respond(id, { ok: false, error: String(e && e.message || e) }); }
      }
      case 'speech_synthesize': {
        const list = regs('registerSpeechProvider');
        const entry = params.providerId ? list.find((r) => r.value && r.value.id === params.providerId) : list[0];
        if (!entry) return respond(id, { ok: false, error: 'no speech provider' });
        const v = entry.value; const fn = v.synthesize || v.execute;
        try {
          const r = await fn.call(v, { text: params.text, voice: params.voice, cfg: hostConfig, config: hostConfig });
          const buf = r && (r.audioBuffer || r.audio || r.buffer);
          const b64 = buf == null ? null : (typeof buf === 'string' ? buf : Buffer.from(buf).toString('base64'));
          if (!b64) return respond(id, { ok: false, error: 'speech provider returned no audio' });
          return respond(id, { ok: true, provider: v.id || null, plugin: entry.plugin, audioBase64: b64, format: r.outputFormat || r.format || 'mp3', fileExtension: r.fileExtension || ('.' + (r.outputFormat || 'mp3')) });
        } catch (e) { return respond(id, { ok: false, error: String(e && e.message || e) }); }
      }
      case 'invoke_registration': {
        // 1.139: generic consumer for any captured api.register* (kind + optional id + method + args).
        const kind = params.kind, want = params.id, method = params.method || 'execute';
        const list = regs(kind);
        if (!list.length) return respond(id, { ok: false, error: `no registrations for ${kind}` });
        const entry = want ? list.find((r) => (r.value && (r.value.id || r.value.name)) === want) : list[0];
        if (!entry) return respond(id, { ok: false, error: `no registration ${kind}/${want}` });
        let v = entry.value;
        if (kind === 'registerContextEngine' && v && typeof v.factory === 'function') v = await contextEngine(v.id) || v;
        const ctx = { config: hostConfig, plugin: entry.plugin, opts: entry.opts || {}, runtime: makeRuntime({ id: entry.plugin }) };
        try {
          let result;
          if (typeof v === 'function') result = await v(params.args || {}, ctx);
          else if (v && typeof v[method] === 'function') result = await v[method](params.args || {}, ctx);
          else if (v && typeof v.execute === 'function') result = await v.execute(params.args || {}, ctx);
          else if (v && typeof v.handle === 'function') result = await v.handle(params.args || {}, ctx);
          else if (v && typeof v.run === 'function') result = await v.run(params.args || {}, ctx);
          else if (v && typeof v.collect === 'function') result = await v.collect(params.args || {}, ctx);
          else if (v && typeof v.create === 'function') {
            const created = await v.create(params.args || {}, ctx);
            result = created && typeof created.execute === 'function' ? await created.execute(params.args || {}, ctx) : created;
          } else result = { registration: { id: v && (v.id || v.name) || null, label: v && v.label || null, keys: v && typeof v === 'object' ? Object.keys(v) : [] } };
          return respond(id, { ok: true, kind, plugin: entry.plugin, id: v && (v.id || v.name) || null, result });
        } catch (e) { return respond(id, { ok: false, kind, plugin: entry.plugin, error: String(e && e.message || e) }); }
      }
      case 'list_capability_providers': {
        // Summaries for provider-like registrations the C++ runtime prefers over built-ins when present.
        const kinds = [].concat(params.kinds || CAPTURED_KINDS);
        const out = {};
        for (const kind of kinds) out[kind] = regs(kind).map((r) => ({
          plugin: r.plugin, id: r.value && (r.value.id || r.value.name) || null, label: r.value && r.value.label || null,
          methods: r.value && typeof r.value === 'object' ? Object.keys(r.value).filter((k) => typeof r.value[k] === 'function') : [],
        }));
        return respond(id, { providers: out });
      }
      case 'shutdown': {
        for (const run of channelRuns.values()) { try { run.abort.abort(); } catch {} }
        for (const { svc } of services) { try { if (typeof svc.stop === 'function') await svc.stop(); } catch {} }
        // 1.139: notify lifecycle registrations on shutdown
        for (const r of regs('registerRuntimeLifecycle')) {
          try { if (r.value && typeof r.value.stop === 'function') await r.value.stop({ config: hostConfig }); } catch {}
        }
        respond(id, { ok: true });
        process.exit(0);
      }
      default: return respond(id, null, { code: -32601, message: 'unknown method: ' + method });
    }
  } catch (e) {
    return respond(id, null, { code: -32000, message: String(e && e.message ? e.message : e) });
  }
}

// Serialize requests so responses stay ordered.
let chain = Promise.resolve();
const rl = readline.createInterface({ input: process.stdin, crlfDelay: Infinity });
rl.on('line', (line) => {
  const s = line.trim();
  if (!s) return;
  let req;
  try { req = JSON.parse(s); } catch (e) {
    process.stdout.write(JSON.stringify({ jsonrpc: '2.0', id: null, error: { code: -32700, message: String(e) } }) + '\n');
    return;
  }
  if (req && req.hostEvent !== undefined) {
    const p = pendingHost.get(req.hostEvent);
    if (p && p.onEvent) {
      p._events = (p._events || Promise.resolve()).then(() => p.onEvent(req)).catch(() => {});
      p._streamed = true;
    }
    return;
  }
  if (req && req.hostReply !== undefined) {  // answer to a plugin→host call: never queued behind the chain
    const p = pendingHost.get(req.hostReply);
    if (p) {
      pendingHost.delete(req.hostReply);
      const finish = () => req.error ? p.reject(new Error(String(req.error))) : p.resolve(req.result);
      if (p._events) p._events.then(finish, finish); else finish();
    }
    return;
  }
  chain = chain.then(() => handle(req));
});
rl.on('close', () => { chain.then(() => process.exit(0)); });
