// TypeScript entry: requires Node >= 22.6 (--experimental-strip-types) or the `typescript` package.
interface Api {
  registerTool(t: { name: string; description: string; parameters: object; execute: (id: string, p: Params) => Promise<unknown> }): void;
  registerCli(fn: (ctx: { program: any }) => void, opts?: { commands: string[] }): void;
}
type Params = { n: number };

const square = (n: number): number => n * n;

export default function register(api: Api): void {
  api.registerTool({
    name: 'ts_square',
    description: 'Square a number (TypeScript plugin)',
    parameters: { type: 'object', properties: { n: { type: 'number' } }, required: ['n'] },
    async execute(_id: string, p: Params) {
      return { content: [{ type: 'text', text: String(square(Number(p.n))) }] };
    },
  });
  api.registerCli(({ program }) => {
    program.command('tsdemo').description('TS plugin demo').action(() => console.log(`ts-square:${square(7)}`));
  }, { commands: ['tsdemo'] });
}
