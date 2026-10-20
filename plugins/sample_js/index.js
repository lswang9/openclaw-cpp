/**
 * Minimal openclaw-cpp JS plugin (NOT full OpenClaw 2026.10.5 plugin API).
 * Exports: { name, tools: [{ name, description, parameters?, handler(args) }] }
 */
module.exports = {
  name: 'sample_js',
  tools: [
    {
      name: 'sample_echo',
      description: 'Echo text from the sample JS plugin (js.sample_echo)',
      parameters: {
        type: 'object',
        properties: { text: { type: 'string' } },
        required: ['text'],
      },
      handler(args) {
        const text = args && args.text != null ? String(args.text) : '';
        return { ok: true, content: 'js-echo:' + text, impl: 'real' };
      },
    },
    {
      name: 'sample_add',
      description: 'Add two numbers in JS (js.sample_add)',
      parameters: {
        type: 'object',
        properties: {
          a: { type: 'number' },
          b: { type: 'number' },
        },
      },
      handler(args) {
        const a = Number(args && args.a != null ? args.a : 0);
        const b = Number(args && args.b != null ? args.b : 0);
        return { ok: true, content: String(a + b), sum: a + b, impl: 'real' };
      },
    },
  ],
};
