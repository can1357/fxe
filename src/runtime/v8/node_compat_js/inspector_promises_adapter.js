const { Session: SyncSession } = await import('node:inspector');
class Session extends SyncSession {
  post(method, params) { return new Promise((res, rej) => { try { super.post(method, params, (err, r) => err ? rej(err) : res(r)); } catch (e) { rej(e); } }); }
}
module.exports = { Session, open: async () => {} };
module.exports.default = module.exports;
