#!/usr/bin/env node
/**
 * openclaw-cpp JS plugin host — newline-delimited JSON-RPC over stdio.
 * Subset host only; not the full OpenClaw Node plugin runtime.
 *
 * Methods: initialize, list_tools, call_tool, list_plugins, shutdown, ping
 */
import fs from 'fs';
import path from 'path';
import { createRequire } from 'module';
import { fileURLToPath } from 'url';
import readline from 'readline';

const require = createRequire(import.meta.url);
const __dirname = path.dirname(fileURLToPath(import.meta.url));

/** @type {Map<string, any>} */
const plugins = new Map();
/** @type {Map<string, {plugin: string, tool: any}>} */
const tools = new Map();

function loadPluginDir(dir) {
  const manifestPath = path.join(dir, 'plugin.json');
  if (!fs.existsSync(manifestPath)) return;
  let manifest;
  try {
    manifest = JSON.parse(fs.readFileSync(manifestPath, 'utf8'));
  } catch {
    return;
  }
  if (manifest.enabled === false) return;
  const entry = manifest.js || manifest.main || 'index.js';
  const entryPath = path.join(dir, entry);
  if (!fs.existsSync(entryPath)) return;
  // Clear cache for reload
  try {
    delete require.cache[require.resolve(entryPath)];
  } catch {
    /* first load */
  }
  const abs = path.resolve(entryPath);
  let mod;
  try {
    mod = require(abs);
  } catch (e) {
    process.stderr.write(`js_bridge: failed to load ${abs}: ${e}\n`);
    return;
  }
  const name = mod.name || manifest.name || path.basename(dir);
  plugins.set(name, { mod, path: dir, manifest });
  const list = Array.isArray(mod.tools) ? mod.tools : [];
  for (const t of list) {
    if (!t || !t.name || typeof t.handler !== 'function') continue;
    const full = `js.${t.name}`;
    tools.set(full, { plugin: name, tool: t });
  }
}

function scan(root) {
  plugins.clear();
  tools.clear();
  if (!root || !fs.existsSync(root)) return;
  for (const ent of fs.readdirSync(root, { withFileTypes: true })) {
    if (!ent.isDirectory()) continue;
    if (ent.name === 'js_bridge') continue;
    loadPluginDir(path.join(root, ent.name));
  }
}

function respond(id, result, error) {
  const msg = { jsonrpc: '2.0', id };
  if (error) msg.error = error;
  else msg.result = result;
  process.stdout.write(JSON.stringify(msg) + '\n');
}

function handle(req) {
  const { id, method, params } = req || {};
  try {
    if (method === 'ping') {
      return respond(id, { ok: true, pong: true });
    }
    if (method === 'initialize') {
      const root = (params && params.plugins_dir) || path.resolve(__dirname, '..');
      scan(root);
      return respond(id, {
        ok: true,
        plugins: [...plugins.keys()],
        tools: [...tools.keys()],
        host: 'openclaw-cpp-js-bridge',
        version: '1.12.0-dev',
      });
    }
    if (method === 'list_plugins') {
      return respond(id, {
        plugins: [...plugins.entries()].map(([name, p]) => ({
          name,
          path: p.path,
          tools: (p.mod.tools || []).map((t) => t.name),
        })),
      });
    }
    if (method === 'list_tools') {
      return respond(id, {
        tools: [...tools.entries()].map(([name, v]) => ({
          name,
          description: v.tool.description || '',
          parameters: v.tool.parameters || { type: 'object' },
          plugin: v.plugin,
        })),
      });
    }
    if (method === 'call_tool') {
      const name = params && params.name;
      const args = (params && params.arguments) || {};
      const ent = tools.get(name);
      if (!ent) {
        return respond(id, null, { code: -32601, message: 'unknown tool: ' + name });
      }
      const out = ent.tool.handler(args);
      return respond(id, out);
    }
    if (method === 'shutdown') {
      respond(id, { ok: true });
      process.exit(0);
      return;
    }
    return respond(id, null, { code: -32601, message: 'unknown method: ' + method });
  } catch (e) {
    return respond(id, null, { code: -32000, message: String(e && e.message ? e.message : e) });
  }
}

const rl = readline.createInterface({ input: process.stdin, crlfDelay: Infinity });
rl.on('line', (line) => {
  const s = line.trim();
  if (!s) return;
  try {
    handle(JSON.parse(s));
  } catch (e) {
    process.stdout.write(
      JSON.stringify({ jsonrpc: '2.0', id: null, error: { code: -32700, message: String(e) } }) +
        '\n',
    );
  }
});
rl.on('close', () => process.exit(0));
