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

  api.registerGatewayMethod('sample.status', ({ respond }) => respond(true, { ok: true, plugin: api.id }));
}
