const native = (globalThis.__fxe_native && globalThis.__fxe_native.async_hooks) || null;
const STORE_SYMBOL = Symbol.for('fxe.als.stores');

class AsyncLocalStorage {
  run(store, fn, ...args) {
    if (!native) {
      const prev = this._sync;
      this._sync = store;
      try {
        return fn(...args);
      } finally {
        this._sync = prev;
      }
    }
    const prev = native.getCurrentResource();
    const next = {};
    const stores = (prev && prev[STORE_SYMBOL]) || new Map();
    const newStores = new Map(stores);
    newStores.set(this, store);
    next[STORE_SYMBOL] = newStores;
    native.setCurrentResource(next);
    try {
      return fn(...args);
    } finally {
      native.setCurrentResource(prev);
    }
  }

  getStore() {
    if (!native) return this._sync;
    const cur = native.getCurrentResource();
    const stores = cur && cur[STORE_SYMBOL];
    return stores ? stores.get(this) : undefined;
  }

  exit(fn, ...args) {
    if (!native) {
      const prev = this._sync;
      this._sync = undefined;
      try {
        return fn(...args);
      } finally {
        this._sync = prev;
      }
    }
    const prev = native.getCurrentResource();
    native.setCurrentResource({});
    try {
      return fn(...args);
    } finally {
      native.setCurrentResource(prev);
    }
  }

  enterWith(store) {
    if (!native) {
      this._sync = store;
      return;
    }
    const cur = native.getCurrentResource() || {};
    const stores = (cur[STORE_SYMBOL] && new Map(cur[STORE_SYMBOL])) || new Map();
    stores.set(this, store);
    cur[STORE_SYMBOL] = stores;
    native.setCurrentResource(cur);
  }

  disable() {
    if (!native) {
      this._sync = undefined;
      return;
    }
    const cur = native.getCurrentResource();
    const stores = cur && cur[STORE_SYMBOL];
    if (!stores) return;
    const nextStores = new Map(stores);
    nextStores.delete(this);
    cur[STORE_SYMBOL] = nextStores;
    native.setCurrentResource(cur);
  }
}

function executionAsyncId() {
  return native ? native.executionAsyncId() : 0;
}

function triggerAsyncId() {
  return native ? native.triggerAsyncId() : 0;
}

class AsyncResource {
  constructor(type) {
    this._t = type;
    this._id = native ? native.nextAsyncId() : 0;
  }

  asyncId() {
    return this._id;
  }

  triggerAsyncId() {
    return 0;
  }

  runInAsyncScope(fn, t, ...a) {
    return Reflect.apply(fn, t, a);
  }

  emitDestroy() {}

  bind(fn, t) {
    return (...a) => this.runInAsyncScope(fn, t, ...a);
  }
}

function createHook() {
  return {
    enable() {
      return this;
    },
    disable() {
      return this;
    },
  };
}

const api = {
  AsyncLocalStorage,
  AsyncResource,
  executionAsyncId,
  triggerAsyncId,
  executionAsyncResource: () => (native ? native.getCurrentResource() : {}),
  createHook,
};

export {
  AsyncLocalStorage,
  AsyncResource,
  executionAsyncId,
  triggerAsyncId,
  createHook,
};
export const executionAsyncResource = api.executionAsyncResource;
export default api;

if (typeof module !== 'undefined' && module && module.exports) {
  module.exports = api;
  module.exports.default = module.exports;
}
