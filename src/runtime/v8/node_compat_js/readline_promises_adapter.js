import * as rl from 'node:readline';

export class Interface extends rl.Interface {
  question(query, _opts) {
    return super.question(query);
  }
}

export function createInterface(opts) {
  return new Interface(opts);
}

const api = { Interface, createInterface };
export default api;

if (typeof module !== 'undefined' && module && module.exports) {
  module.exports = api;
  module.exports.default = module.exports;
}
