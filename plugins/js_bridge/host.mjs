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
 * Plugin API subset: api.id/name/config/pluginConfig/logger/runtime/resolvePath,
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
let hostConfig = {};
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

function makeApi(rec) {
  const entryCfg = ((hostConfig.plugins || {}).entries || {})[rec.id] || {};
  const log = (lvl) => (...a) => process.stderr.write(`[plugin:${rec.id}] ${lvl}: ${a.map(String).join(' ')}\n`);
  const api = {
    id: rec.id,
    name: rec.manifest.name || rec.id,
    config: hostConfig,
    pluginConfig: entryCfg.config || {},
    logger: { info: log('info'), warn: log('warn'), error: log('error'), debug: log('debug') },
    runtime: { version: '2026.10.5-cpp', host: 'openclaw-cpp' },
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
    registerChannel() { api.logger.warn('registerChannel not supported by openclaw-cpp host'); },
    registerProvider() { api.logger.warn('registerProvider not supported by openclaw-cpp host'); },
    registerHttpRoute() { api.logger.warn('registerHttpRoute not supported by openclaw-cpp host'); },
    registerCli() {},
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
    process.stderr.write(`js_bridge: plugin ${rec.id} failed: ${rec.error}\n`);
  }
}

async function scan(roots) {
  plugins.clear(); tools.clear(); commands.clear(); hooks.clear(); gwMethods.clear(); services.length = 0;
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
      case 'shutdown': {
        for (const { svc } of services) { try { if (typeof svc.stop === 'function') await svc.stop(); } catch {} }
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
  chain = chain.then(() => handle(req));
});
rl.on('close', () => { chain.then(() => process.exit(0)); });
