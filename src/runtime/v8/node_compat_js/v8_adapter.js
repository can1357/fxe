const native = globalThis.__fxe_native?.v8;

if (!native || typeof native !== 'object') {
  throw new Error('fxe: __fxe_native.v8 is not installed');
}

function unsupported(method) {
  return () => {
    throw new Error(`fxe: node:v8.${method} is not supported`);
  };
}

class V8Serializer {
  constructor() {
    this._headerWritten = false;
    this._valueSet = false;
    this._value = undefined;
  }

  writeHeader() {
    this._headerWritten = true;
  }

  writeValue(value) {
    this._value = value;
    this._valueSet = true;
  }

  releaseBuffer() {
    if (!this._headerWritten || !this._valueSet) {
      throw new Error('Serializer.releaseBuffer requires writeHeader() and writeValue() first');
    }
    return native.serialize(this._value);
  }
}

class V8Deserializer {
  constructor(bytes) {
    this._bytes = bytes;
    this._headerRead = false;
  }

  readHeader() {
    this._headerRead = true;
  }

  readValue() {
    if (!this._headerRead) {
      throw new Error('Deserializer.readValue requires readHeader() first');
    }
    return native.deserialize(this._bytes);
  }
}

class V8DefaultSerializer extends V8Serializer {}
class V8DefaultDeserializer extends V8Deserializer {}

function createPromiseHook() {
  return {
    enable() {},
    disable() {},
  };
}

const api = {
  getHeapStatistics: () => native.getHeapStatistics(),
  getHeapSpaceStatistics: () => native.getHeapSpaceStatistics(),
  getHeapCodeStatistics: () => native.getHeapCodeStatistics(),
  writeHeapSnapshot: (filePath) => native.writeHeapSnapshot(filePath),
  cachedDataVersionTag: () => native.cachedDataVersionTag(),
  serialize: (value) => native.serialize(value),
  deserialize: (bytes) => native.deserialize(bytes),
  Serializer: V8Serializer,
  Deserializer: V8Deserializer,
  DefaultSerializer: V8DefaultSerializer,
  DefaultDeserializer: V8DefaultDeserializer,
  promiseHooks: {
    createHook: createPromiseHook,
    onInit: createPromiseHook,
    onSettled: createPromiseHook,
    onBefore: createPromiseHook,
    onAfter: createPromiseHook,
  },
  GCKind: { MAJOR: 1, MINOR: 2, INCREMENTAL: 3 },
  GCType: { ALL: 0xff },
  startupSnapshot: {
    setDeserializeMainFunction: unsupported('startupSnapshot.setDeserializeMainFunction'),
  },
};

export default api;
export const getHeapStatistics = api.getHeapStatistics;
export const getHeapSpaceStatistics = api.getHeapSpaceStatistics;
export const getHeapCodeStatistics = api.getHeapCodeStatistics;
export const writeHeapSnapshot = api.writeHeapSnapshot;
export const cachedDataVersionTag = api.cachedDataVersionTag;
export const serialize = api.serialize;
export const deserialize = api.deserialize;
export const Serializer = api.Serializer;
export const Deserializer = api.Deserializer;
export const DefaultSerializer = api.DefaultSerializer;
export const DefaultDeserializer = api.DefaultDeserializer;
export const promiseHooks = api.promiseHooks;
export const GCKind = api.GCKind;
export const GCType = api.GCType;
export const startupSnapshot = api.startupSnapshot;

if (typeof module !== 'undefined' && module && module.exports) {
  module.exports = api;
  module.exports.default = module.exports;
}
