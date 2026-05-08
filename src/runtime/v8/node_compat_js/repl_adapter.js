import * as rl from 'node:readline';
import { EventEmitter } from 'node:events';

export class REPLServer extends EventEmitter {
  constructor(opts = {}) {
    super();
    this._rl = rl.createInterface({
      input: opts.input || globalThis.process?.stdin,
      output: opts.output || globalThis.process?.stdout,
      prompt: opts.prompt ?? '> ',
    });
    this._eval = opts.eval || ((cmd, _ctx, _file, cb) => {
      try {
        // eslint-disable-next-line no-new-func
        const result = new Function(`return (${cmd})`)();
        cb(null, result);
      } catch (error) {
        cb(error);
      }
    });
    this.context = {};
    this._rl.on('line', (line) => {
      this._eval(line, this.context, '<repl>', (err, result) => {
        if (err) this._rl.write(String(err) + '\n');
        else if (result !== undefined) this._rl.write(String(result) + '\n');
        this._rl.prompt();
      });
    });
    this._rl.on('close', () => this.emit('exit'));
  }

  defineCommand(_name, _fn) {}

  displayPrompt() {
    this._rl.prompt();
  }

  close() {
    this._rl.close();
  }
}

export function start(opts) {
  const server = new REPLServer(opts);
  server.displayPrompt();
  return server;
}

const api = { REPLServer, start };
export default api;

if (typeof module !== 'undefined' && module && module.exports) {
  module.exports = api;
  module.exports.default = module.exports;
}
