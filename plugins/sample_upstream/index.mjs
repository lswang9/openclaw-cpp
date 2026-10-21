// Upstream-style OpenClaw plugin: default export register(api).
export default function register(api) {
  const greeting = (api.pluginConfig && api.pluginConfig.greeting) || 'hello';

  api.registerTool({
    name: 'upstream_greet',
    description: 'Greet someone (sample upstream plugin tool)',
    parameters: { type: 'object', properties: { name: { type: 'string' } }, required: ['name'] },
    async execute(_id, params) {
      return { content: [{ type: 'text', text: `${greeting}, ${params.name}!` }] };
    },
  });

  api.registerCommand({
    name: 'ping',
    description: 'Reply pong without calling the model',
    handler: (ctx) => ({ text: `pong${ctx.args ? ' ' + ctx.args : ''}` }),
  });

  api.on('before_agent_start', async () => ({ prependContext: '[sample-upstream] context injected' }));
  api.on('message_sending', async (ev) =>
    ev.content && ev.content.includes('SECRET') ? { content: ev.content.replace(/SECRET/g, '[redacted]') } : undefined);

  const stats = { beforeTool: 0, afterTool: 0, sessionStart: 0, sessionEnd: 0, sent: [] };
  api.on('before_tool_call', (ev) => {
    stats.beforeTool++;
    if (ev.params && ev.params.text === 'forbidden') return { block: true, blockReason: 'sample-upstream blocked forbidden text' };
    if (ev.params && ev.params.text === 'rewrite-me') return { params: { ...ev.params, text: 'rewritten' } };
  });
  api.on('after_tool_call', () => { stats.afterTool++; });
  api.on('session_start', () => { stats.sessionStart++; });
  api.on('session_end', () => { stats.sessionEnd++; });

  api.registerGatewayMethod('sample.status', ({ respond }) => respond(true, { ok: true, plugin: api.id, ...stats }));
  api.registerGatewayMethod('sample.loginWithQrStart', ({ respond }) => respond(true, { qrDataUrl: 'data:image/png;base64,iVBORw0KGgo=', message: 'scan with sample app' }));
  api.registerGatewayMethod('sample.loginWithQrWait', ({ respond }) => respond(true, { connected: true, message: 'linked' }));

  api.registerHttpRoute({
    path: '/plugins/sample/hello',
    handler: (req, res) => {
      res.setHeader('content-type', 'application/json');
      res.end(JSON.stringify({ hello: (req.json && req.json.name) || 'world', method: req.method }));
    },
  });

  api.registerCli(({ program }) => {
    const cmd = program.command('sample').description('Sample upstream plugin CLI');
    cmd.command('greet <name>').option('--shout', 'uppercase').action((name, opts) => {
      const msg = `${greeting}, ${name}!`;
      console.log(opts.shout ? msg.toUpperCase() : msg);
    });
  }, { commands: ['sample'] });

  api.registerChannel({
    plugin: {
      id: 'echochan',
      meta: { label: 'Echo channel (sample)' },
      inbound: { parse: (body) => ({ from: body.user || 'anon', text: body.msg || '', chatType: 'direct' }) },
      outbound: { sendText: async ({ to, text }) => { stats.sent.push({ to, text }); return { ok: true }; } },
    },
  });

  api.registerProvider({
    id: 'samplellm',
    label: 'Sample LLM (deterministic)',
    models: ['echo-1'],
    complete: async ({ model, messages }) => {
      const last = [...messages].reverse().find((m) => m.role === 'user');
      return { content: `samplellm/${model}: ${last ? String(last.content).slice(-40) : ''}` };
    },
  });
}
