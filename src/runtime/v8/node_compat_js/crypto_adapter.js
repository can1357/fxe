
const randomNative = globalThis.__fxe_native?.random;
const hashNative = globalThis.__fxe_native?.hash;
const cipherNative = globalThis.__fxe_native?.cipher;
const kdfNative = globalThis.__fxe_native?.kdf;
const pkNative = globalThis.__fxe_native?.pk;
const pbkdf2Native = hashNative;

const requireRandomFill = () => {
  if (!randomNative || typeof randomNative.fill !== 'function') {
    throw new Error('host-backed node:crypto function unavailable: random.fill');
  }
  return randomNative.fill.bind(randomNative);
};

const requireHashCreate = () => {
  if (!hashNative || typeof hashNative.create !== 'function') {
    throw new Error('host-backed node:crypto function unavailable: hash.create');
  }
  return hashNative.create.bind(hashNative);
};

const requireHmacCreate = () => {
  if (!hashNative || typeof hashNative.createHmac !== 'function') {
    throw new Error('host-backed node:crypto function unavailable: hash.createHmac');
  }
  return hashNative.createHmac.bind(hashNative);
};

const requirePbkdf2Sync = () => {
  if (!pbkdf2Native || typeof pbkdf2Native.pbkdf2Sync !== 'function') {
    throw new Error('host-backed node:crypto function unavailable: hash.pbkdf2Sync');
  }
  return pbkdf2Native.pbkdf2Sync.bind(pbkdf2Native);
};

const requireCipherCreate = () => {
  if (!cipherNative || typeof cipherNative.createCipheriv !== 'function') {
    throw new Error('host-backed node:crypto function unavailable: cipher.createCipheriv');
  }
  return cipherNative.createCipheriv.bind(cipherNative);
};

const requireDecipherCreate = () => {
  if (!cipherNative || typeof cipherNative.createDecipheriv !== 'function') {
    throw new Error('host-backed node:crypto function unavailable: cipher.createDecipheriv');
  }
  return cipherNative.createDecipheriv.bind(cipherNative);
};

const requireScryptSync = () => {
  if (!kdfNative || typeof kdfNative.scryptSync !== 'function') {
    throw new Error('host-backed node:crypto function unavailable: kdf.scryptSync');
  }
  return kdfNative.scryptSync.bind(kdfNative);
};

const requirePkFn = (method) => {
  if (!pkNative || typeof pkNative[method] !== 'function') {
    throw new Error(`host-backed node:crypto function unavailable: pk.${method}`);
  }
  return pkNative[method].bind(pkNative);
};

const isArrayBufferView = (value) => value != null && ArrayBuffer.isView(value);

const assertIntegerSize = (size) => {
  const n = Number(size);
  if (!Number.isSafeInteger(n) || n < 0) {
    throw new RangeError('size must be a non-negative safe integer');
  }
  return n;
};

const toUint8Array = (value, encoding = 'utf8') => {
  if (typeof value === 'string') {
    const enc = String(encoding ?? 'utf8').toLowerCase();
    if (enc === 'utf8' || enc === 'utf-8') {
      return new TextEncoder().encode(value);
    }
    if (enc === 'hex') {
      if (value.length % 2 !== 0) {
        throw new TypeError('hex string must have an even number of characters');
      }
      const out = new Uint8Array(value.length / 2);
      for (let i = 0; i < out.length; ++i) {
        const byte = Number.parseInt(value.slice(i * 2, i * 2 + 2), 16);
        if (!Number.isFinite(byte)) {
          throw new TypeError('invalid hex string');
        }
        out[i] = byte;
      }
      return out;
    }
    if (enc === 'base64') {
      return Uint8Array.from(globalThis.atob(value), (c) => c.charCodeAt(0));
    }
    return new TextEncoder().encode(value);
  }
  if (value instanceof ArrayBuffer) {
    return new Uint8Array(value);
  }
  if (isArrayBufferView(value)) {
    return new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
  }
  throw new TypeError('data must be a string, ArrayBuffer, or typed array');
};

const bytesToHex = (bytes) => Array.prototype.map.call(bytes, (b) => b.toString(16).padStart(2, '0')).join('');
const bytesToBase64 = (bytes) => globalThis.btoa(String.fromCharCode(...bytes));
const toBuffer = (bytes) => typeof Buffer === 'function' ? Buffer.from(bytes) : bytes;
const bytesToArrayBuffer = (bytes) => {
  const view = new Uint8Array(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const copy = new Uint8Array(view.byteLength);
  copy.set(view);
  return copy.buffer;
};
const encodeOutput = (bytes, encoding) => {
  if (encoding === undefined) {
    return toBuffer(bytes);
  }
  const enc = String(encoding).toLowerCase();
  if (enc === 'hex') return bytesToHex(bytes);
  if (enc === 'base64') return bytesToBase64(bytes);
  if (enc === 'buffer') return toBuffer(bytes);
  if (enc === 'utf8' || enc === 'utf-8') return new TextDecoder().decode(bytes);
  throw new Error(`unsupported crypto output encoding: ${encoding}`);
};

export const randomFillSync = (buffer, offset = 0, size = undefined) => {
  if (!isArrayBufferView(buffer)) {
    throw new TypeError('randomFillSync buffer must be a typed array or DataView');
  }
  const byteOffset = assertIntegerSize(offset);
  const byteSize = size === undefined ? buffer.byteLength - byteOffset : assertIntegerSize(size);
  if (byteOffset > buffer.byteLength || byteSize > buffer.byteLength - byteOffset) {
    throw new RangeError('randomFillSync offset/size is out of range');
  }
  const view = new Uint8Array(buffer.buffer, buffer.byteOffset + byteOffset, byteSize);
  requireRandomFill()(view);
  return buffer;
};

export const randomBytes = (size) => {
  const out = new Uint8Array(assertIntegerSize(size));
  requireRandomFill()(out);
  return toBuffer(out);
};

export const getRandomValues = (typedArray) => {
  if (!isArrayBufferView(typedArray) || typedArray instanceof DataView) {
    throw new TypeError('getRandomValues requires an integer typed array');
  }
  if (typedArray.byteLength > 65536) {
    const error = new Error('The requested length exceeds 65,536 bytes');
    error.name = 'QuotaExceededError';
    throw error;
  }
  requireRandomFill()(new Uint8Array(typedArray.buffer, typedArray.byteOffset, typedArray.byteLength));
  return typedArray;
};

export const createHash = (algorithm) => {
  const native = requireHashCreate()(String(algorithm));
  return {
    update(data, inputEncoding = 'utf8') {
      native.update(toUint8Array(data, inputEncoding));
      return this;
    },
    digest(encoding) {
      const bytes = native.digest();
      if (encoding === undefined) {
        return toBuffer(bytes);
      }
      const enc = String(encoding).toLowerCase();
      if (enc === 'hex') return bytesToHex(bytes);
      if (enc === 'base64') return bytesToBase64(bytes);
      if (enc === 'buffer') return toBuffer(bytes);
      throw new Error(`unsupported hash digest encoding: ${encoding}`);
    },
  };
};

export const createHmac = (algorithm, key) => {
  const native = requireHmacCreate()(String(algorithm), toUint8Array(key));
  return {
    update(data, inputEncoding = 'utf8') {
      native.update(toUint8Array(data, inputEncoding));
      return this;
    },
    digest(encoding) {
      const bytes = native.digest();
      if (encoding === undefined) {
        return toBuffer(bytes);
      }
      const enc = String(encoding).toLowerCase();
      if (enc === 'hex') return bytesToHex(bytes);
      if (enc === 'base64') return bytesToBase64(bytes);
      if (enc === 'buffer') return toBuffer(bytes);
      throw new Error(`unsupported hmac digest encoding: ${encoding}`);
    },
  };
};

export const pbkdf2Sync = (password, salt, iterations, keylen, digest = 'sha1') => {
  const bytes = requirePbkdf2Sync()(
    toUint8Array(password),
    toUint8Array(salt),
    assertIntegerSize(iterations),
    assertIntegerSize(keylen),
    String(digest),
  );
  return toBuffer(bytes);
};

export const pbkdf2 = (password, salt, iterations, keylen, digest, callback) => {
  if (typeof digest === 'function') {
    callback = digest;
    digest = 'sha1';
  }
  if (typeof callback !== 'function') {
    throw new TypeError('pbkdf2 callback must be a function');
  }
  Promise.resolve().then(() => {
    try {
      callback(null, pbkdf2Sync(password, salt, iterations, keylen, digest));
    } catch (error) {
      callback(error);
    }
  });
};

const makeCipher = (native) => ({
  update(data, inputEncoding = 'utf8', outputEncoding = undefined) {
    return encodeOutput(native.update(toUint8Array(data, inputEncoding)), outputEncoding);
  },
  final(outputEncoding = undefined) {
    return encodeOutput(native.final(), outputEncoding);
  },
  setAutoPadding(enabled = true) {
    if (typeof native.setAutoPadding === 'function') {
      native.setAutoPadding(Boolean(enabled));
    }
    return this;
  },
  setAAD(data) {
    if (typeof native.setAAD === 'function') native.setAAD(toUint8Array(data));
    return this;
  },
  getAuthTag() {
    if (typeof native.getAuthTag !== 'function') throw new Error('cipher auth tag unavailable');
    return toBuffer(native.getAuthTag());
  },
  setAuthTag(tag) {
    if (typeof native.setAuthTag !== 'function') throw new Error('cipher auth tag unavailable');
    native.setAuthTag(toUint8Array(tag));
    return this;
  },
});

export const createCipheriv = (algorithm, key, iv) =>
  makeCipher(requireCipherCreate()(String(algorithm), toUint8Array(key), toUint8Array(iv)));

export const createDecipheriv = (algorithm, key, iv) =>
  makeCipher(requireDecipherCreate()(String(algorithm), toUint8Array(key), toUint8Array(iv)));


const assertPositiveInteger = (value, name) => {
  const n = Number(value);
  if (!Number.isSafeInteger(n) || n <= 0) {
    throw new RangeError(`${name} must be a positive safe integer`);
  }
  return n;
};

const assertPowerOfTwo = (value, name) => {
  if (value <= 1 || !Number.isInteger(Math.log2(value))) {
    throw new RangeError(`${name} must be a power of two greater than 1`);
  }
  return value;
};


const normalizeScryptOptions = (options = {}) => {
  const N = assertPowerOfTwo(assertPositiveInteger(options.N ?? options.cost ?? 16384, 'scrypt cost'), 'scrypt cost');
  const r = assertPositiveInteger(options.r ?? options.blockSize ?? 8, 'scrypt blockSize');
  const p = assertPositiveInteger(options.p ?? options.parallelization ?? 1, 'scrypt parallelization');
  const maxmem = assertPositiveInteger(options.maxmem ?? 32 * 1024 * 1024, 'scrypt maxmem');
  const memory = 128 * N * r;
  if (!Number.isSafeInteger(memory) || memory > maxmem) {
    throw new RangeError('scrypt memory limit exceeded');
  }
  if (!Number.isSafeInteger(p * 128 * r)) {
    throw new RangeError('scrypt parameters are too large');
  }
  return { N, r, p };
};

export const scryptSync = (password, salt, keylen, options = {}) => {
  const passwordBytes = toUint8Array(password);
  const saltBytes = toUint8Array(salt);
  const outputLength = assertIntegerSize(keylen);
  const { N, r, p } = normalizeScryptOptions(options);
  return toBuffer(requireScryptSync()(passwordBytes, saltBytes, N, r, p, outputLength));
};

export const scrypt = (password, salt, keylen, options, callback) => {
  if (typeof options === 'function') {
    callback = options;
    options = {};
  }
  if (typeof callback !== 'function') {
    throw new TypeError('scrypt callback must be a function');
  }
  Promise.resolve().then(() => {
    try {
      callback(null, scryptSync(password, salt, keylen, options));
    } catch (error) {
      callback(error);
    }
  });
};

const normalizeWebCryptoHash = (algorithm) => {
  const name = typeof algorithm === 'string' ? algorithm : algorithm?.name;
  const normalized = String(name ?? '').toLowerCase().replace(/[_\s]/g, '-');
  if (normalized === 'sha-1' || normalized === 'sha1') return 'sha1';
  if (normalized === 'sha-256' || normalized === 'sha256') return 'sha256';
  if (normalized === 'sha-384' || normalized === 'sha384') return 'sha384';
  if (normalized === 'sha-512' || normalized === 'sha512') return 'sha512';
  throw new Error(`unsupported SubtleCrypto digest algorithm: ${name}`);
};

const normalizeSubtleName = (algorithm) =>
  String((typeof algorithm === 'string' ? algorithm : algorithm?.name) ?? '').toUpperCase().replace(/_/g, '-');

const webCryptoHashName = (digest) => {
  if (digest === 'sha1') return 'SHA-1';
  if (digest === 'sha256') return 'SHA-256';
  if (digest === 'sha384') return 'SHA-384';
  if (digest === 'sha512') return 'SHA-512';
  throw new Error(`unsupported SubtleCrypto hash digest: ${digest}`);
};

const assertSubtleAlgorithm = (actual, expected, operation) => {
  if (actual !== expected) throw new Error(`unsupported SubtleCrypto ${operation} algorithm: ${actual || '<missing>'}`);
};

const normalizeAesAlgorithm = (algorithm, operation = 'AES operation') => {
  const name = normalizeSubtleName(algorithm);
  if (name !== 'AES-CBC' && name !== 'AES-GCM') {
    throw new Error(`unsupported SubtleCrypto ${operation} algorithm: ${name || '<missing>'}`);
  }
  const iv = toUint8Array(algorithm?.iv);
  if (name === 'AES-CBC') {
    if (iv.byteLength !== 16) throw new Error('SubtleCrypto AES-CBC requires a 16-byte iv');
    return { name, iv };
  }
  if (iv.byteLength === 0) throw new Error('SubtleCrypto AES-GCM requires a non-empty iv');
  const tagLengthBits = Number(algorithm?.tagLength ?? 128);
  const validTagLengths = [32, 64, 96, 104, 112, 120, 128];
  if (!validTagLengths.includes(tagLengthBits)) {
    throw new Error('SubtleCrypto AES-GCM tagLength must be one of 32, 64, 96, 104, 112, 120, or 128');
  }
  const additionalData = algorithm?.additionalData === undefined ? undefined : toUint8Array(algorithm.additionalData);
  return { name, iv, additionalData, tagLengthBytes: tagLengthBits / 8 };
};

const copyBytes = (data) => new Uint8Array(toUint8Array(data));

const subtleKey = (type, algorithm, extractable, usages, data, material = undefined) =>
  Object.freeze({
    type,
    algorithm,
    extractable: Boolean(extractable),
    usages: Array.from(usages ?? []),
    __fxeKeyData: data === undefined ? undefined : copyBytes(data),
    __fxeKeyMaterial: material,
  });

const normalizeKeyBytes = (key) => {
  if (key && key.__fxeKeyData !== undefined) return toUint8Array(key.__fxeKeyData);
  return toUint8Array(key);
};

const assertCryptoKey = (key, operation) => {
  if (!key || key.__fxeKeyData === undefined) throw new Error(`SubtleCrypto ${operation} requires a CryptoKey`);
};

const assertKeyAlgorithm = (key, expected, operation) => {
  assertCryptoKey(key, operation);
  const actual = normalizeSubtleName(key?.algorithm);
  if (actual && actual !== expected) throw new Error(`SubtleCrypto ${operation} requires a ${expected} key, got ${actual}`);
};

const assertKeyUsage = (key, usage, operation) => {
  assertCryptoKey(key, operation);
  if (!Array.isArray(key.usages) || !key.usages.includes(usage)) {
    throw new Error(`SubtleCrypto ${operation} requires a key with ${usage} usage`);
  }
};

const validateUsages = (usages, allowed, operation) => {
  for (const usage of usages ?? []) {
    if (!allowed.includes(usage)) throw new Error(`unsupported SubtleCrypto ${operation} key usage: ${usage}`);
  }
};

const aesCipherName = (name, keyBytes) => {
  const bits = keyBytes.byteLength * 8;
  if ((bits !== 128 && bits !== 192 && bits !== 256) || (name !== 'AES-CBC' && name !== 'AES-GCM')) {
    throw new Error(`unsupported AES key size for ${name}: ${bits}`);
  }
  return `aes-${bits}-${name === 'AES-GCM' ? 'gcm' : 'cbc'}`;
};

const normalizeHmacAlgorithm = (algorithm, key) => {
  const name = normalizeSubtleName(algorithm);
  assertSubtleAlgorithm(name, 'HMAC', 'HMAC');
  const hash = normalizeWebCryptoHash(algorithm?.hash ?? key?.algorithm?.hash);
  return { name, hash: { name: webCryptoHashName(hash) }, digest: hash };
};

const concatBytes = (...parts) => {
  const total = parts.reduce((sum, part) => sum + part.byteLength, 0);
  const out = new Uint8Array(total);
  let offset = 0;
  for (const part of parts) {
    out.set(part, offset);
    offset += part.byteLength;
  }
  return out;
};

const b64urlDecode = (value) => {
  const base64 = String(value).replace(/-/g, '+').replace(/_/g, '/');
  const padded = base64 + '='.repeat((4 - (base64.length % 4)) % 4);
  return Uint8Array.from(globalThis.atob(padded), (c) => c.charCodeAt(0));
};

const b64urlEncode = (bytes) => bytesToBase64(bytes).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/g, '');

const jwkBytes = (str) => b64urlDecode(str);
const jwkEncode = (bytes) => b64urlEncode(bytes);

const padToLength = (bytes, length) => {
  if (bytes.byteLength === length) return bytes;
  if (bytes.byteLength > length) return bytes.slice(bytes.byteLength - length);
  const out = new Uint8Array(length);
  out.set(bytes, length - bytes.byteLength);
  return out;
};

const trimLeadingZeros = (bytes) => {
  let i = 0;
  while (i < bytes.byteLength - 1 && bytes[i] === 0) ++i;
  return i === 0 ? bytes : bytes.slice(i);
};

const normalizeNamedCurve = (algorithm, key) => {
  const curve = String(algorithm?.namedCurve ?? key?.algorithm?.namedCurve ?? '').toUpperCase().replace(/_/g, '-');
  if (curve !== 'P-256') throw new Error(`unsupported SubtleCrypto ECDSA namedCurve: ${curve || '<missing>'}`);
  return 'P-256';
};

const normalizeEcdsaAlgorithm = (algorithm, key, operation) => {
  const name = normalizeSubtleName(algorithm);
  assertSubtleAlgorithm(name, 'ECDSA', operation);
  const hash = normalizeWebCryptoHash(algorithm?.hash ?? key?.algorithm?.hash ?? 'SHA-256');
  if (hash !== 'sha256') throw new Error(`unsupported SubtleCrypto ECDSA P-256 hash: ${webCryptoHashName(hash)}`);
  return { name, namedCurve: normalizeNamedCurve(algorithm, key), hash: { name: 'SHA-256' }, digest: 'sha256' };
};

const normalizeRsaOaepAlgorithm = (algorithm, key, operation) => {
  const name = normalizeSubtleName(algorithm);
  assertSubtleAlgorithm(name, 'RSA-OAEP', operation);
  const hash = normalizeWebCryptoHash(algorithm?.hash ?? key?.algorithm?.hash ?? 'SHA-256');
  return { name, hash: { name: webCryptoHashName(hash) }, digest: hash, label: algorithm?.label === undefined ? new Uint8Array() : copyBytes(algorithm.label) };
};

const normalizeImportedAlgorithm = (algorithm, format, keyBytes, keyData, usages) => {
  const name = normalizeSubtleName(algorithm);
  if (name === 'HMAC') {
    if (format !== 'raw') throw new Error('SubtleCrypto HMAC importKey supports raw keys only');
    validateUsages(usages, ['sign', 'verify'], 'HMAC');
    const hmac = normalizeHmacAlgorithm(algorithm);
    return { type: 'secret', algorithm: { name: hmac.name, hash: hmac.hash, length: Number(algorithm?.length ?? keyBytes.byteLength * 8) }, data: keyBytes };
  }
  if (name === 'AES-CBC' || name === 'AES-GCM') {
    if (format !== 'raw') throw new Error(`SubtleCrypto ${name} importKey supports raw keys only`);
    validateUsages(usages, ['encrypt', 'decrypt', 'wrapKey', 'unwrapKey'], name);
    aesCipherName(name, keyBytes);
    return { type: 'secret', algorithm: { name, length: keyBytes.byteLength * 8 }, data: keyBytes };
  }
  if (name === 'PBKDF2') {
    if (format !== 'raw') throw new Error('SubtleCrypto PBKDF2 importKey supports raw keys only');
    validateUsages(usages, ['deriveBits', 'deriveKey'], 'PBKDF2');
    return { type: 'secret', algorithm: { name }, data: keyBytes };
  }
  if (name === 'RSA-OAEP') {
    validateUsages(usages, ['encrypt', 'decrypt', 'wrapKey', 'unwrapKey'], 'RSA-OAEP');
    const hash = normalizeWebCryptoHash(algorithm?.hash ?? 'SHA-256');
    let material;
    let type;
    if (format === 'spki') {
      material = requirePkFn('parsePublicKeyDer')(keyBytes);
      type = 'public';
    } else if (format === 'pkcs8') {
      material = requirePkFn('parsePrivateKeyDer')(keyBytes);
      type = 'private';
    } else if (format === 'jwk') {
      const jwk = keyData;
      if (jwk?.kty !== 'RSA' || !jwk.n || !jwk.e) throw new Error('invalid RSA JWK');
      material = { kind: 'rsa', n: jwkBytes(jwk.n), e: jwkBytes(jwk.e) };
      if (jwk.d) {
        material.d = jwkBytes(jwk.d);
        if (jwk.p) material.p = jwkBytes(jwk.p);
        if (jwk.q) material.q = jwkBytes(jwk.q);
        if (jwk.dp) material.dp = jwkBytes(jwk.dp);
        if (jwk.dq) material.dq = jwkBytes(jwk.dq);
        if (jwk.qi) material.qi = jwkBytes(jwk.qi);
        type = 'private';
      } else {
        type = 'public';
      }
    } else {
      throw new Error('SubtleCrypto RSA-OAEP importKey supports spki, pkcs8, or jwk');
    }
    if (material.kind !== 'rsa') throw new Error('SubtleCrypto RSA-OAEP importKey received a non-RSA key');
    if (type === 'public') validateUsages(usages, ['encrypt', 'wrapKey'], 'RSA-OAEP public');
    if (type === 'private') validateUsages(usages, ['decrypt', 'unwrapKey'], 'RSA-OAEP private');
    const modulusBytes = trimLeadingZeros(material.n);
    return {
      type,
      algorithm: {
        name,
        modulusLength: modulusBytes.byteLength * 8,
        publicExponent: trimLeadingZeros(material.e),
        hash: { name: webCryptoHashName(hash) },
      },
      data: keyBytes,
      material,
    };
  }
  if (name === 'ECDSA') {
    validateUsages(usages, ['sign', 'verify'], 'ECDSA');
    const namedCurve = normalizeNamedCurve(algorithm);
    let material;
    let type;
    if (format === 'raw') {
      if (keyBytes.byteLength !== 65 || keyBytes[0] !== 0x04) {
        throw new Error('SubtleCrypto ECDSA P-256 raw public keys must be 65-byte uncompressed points');
      }
      material = { kind: 'ec', curve: namedCurve, x: keyBytes.slice(1, 33), y: keyBytes.slice(33, 65) };
      requirePkFn('writePublicKeyDer')(material);
      type = 'public';
    } else if (format === 'spki') {
      material = requirePkFn('parsePublicKeyDer')(keyBytes);
      type = 'public';
    } else if (format === 'pkcs8') {
      material = requirePkFn('parsePrivateKeyDer')(keyBytes);
      type = 'private';
    } else if (format === 'jwk') {
      const jwk = keyData;
      if (jwk?.kty !== 'EC' || jwk.crv !== 'P-256' || !jwk.x || !jwk.y) throw new Error('invalid ECDSA P-256 JWK');
      material = { kind: 'ec', curve: 'P-256', x: padToLength(jwkBytes(jwk.x), 32), y: padToLength(jwkBytes(jwk.y), 32) };
      if (jwk.d) {
        material.d = padToLength(jwkBytes(jwk.d), 32);
        type = 'private';
      } else {
        type = 'public';
      }
      requirePkFn('writePublicKeyDer')({ kind: 'ec', curve: material.curve, x: material.x, y: material.y });
    } else {
      throw new Error('SubtleCrypto ECDSA importKey supports raw, spki, pkcs8, or jwk');
    }
    if (material.kind !== 'ec') throw new Error('SubtleCrypto ECDSA importKey received a non-EC key');
    if (material.curve !== namedCurve) throw new Error(`unsupported SubtleCrypto ECDSA namedCurve: ${material.curve || '<missing>'}`);
    if (type === 'public') validateUsages(usages, ['verify'], 'ECDSA public');
    if (type === 'private') validateUsages(usages, ['sign'], 'ECDSA private');
    return { type, algorithm: { name, namedCurve }, data: keyBytes, material };
  }
  throw new Error(`unsupported SubtleCrypto importKey algorithm: ${name || '<missing>'}`);
};

const exportJwk = (key) => {
  const material = key.__fxeKeyMaterial;
  if (material?.kind === 'rsa') {
    const jwk = {
      kty: 'RSA',
      n: jwkEncode(trimLeadingZeros(material.n)),
      e: jwkEncode(trimLeadingZeros(material.e)),
      key_ops: key.usages,
      ext: key.extractable,
    };
    if (key.type === 'private') {
      jwk.d = jwkEncode(trimLeadingZeros(material.d));
      if (material.p) jwk.p = jwkEncode(trimLeadingZeros(material.p));
      if (material.q) jwk.q = jwkEncode(trimLeadingZeros(material.q));
      if (material.dp) jwk.dp = jwkEncode(trimLeadingZeros(material.dp));
      if (material.dq) jwk.dq = jwkEncode(trimLeadingZeros(material.dq));
      if (material.qi) jwk.qi = jwkEncode(trimLeadingZeros(material.qi));
    }
    return jwk;
  }
  if (material?.kind === 'ec') {
    const jwk = {
      kty: 'EC',
      crv: material.curve,
      x: jwkEncode(padToLength(material.x, 32)),
      y: jwkEncode(padToLength(material.y, 32)),
      key_ops: key.usages,
      ext: key.extractable,
    };
    if (key.type === 'private') {
      jwk.d = jwkEncode(padToLength(material.d, 32));
    }
    return jwk;
  }
  return { kty: 'oct', k: b64urlEncode(normalizeKeyBytes(key)), alg: key.algorithm?.name, key_ops: key.usages, ext: key.extractable };
};

export const subtle = {
  async digest(algorithm, data) {
    const digest = createHash(normalizeWebCryptoHash(algorithm)).update(toUint8Array(data)).digest();
    return bytesToArrayBuffer(digest);
  },
  async importKey(format, keyData, algorithm, extractable = true, keyUsages = []) {
    const fmt = String(format).toLowerCase();
    const bytes = fmt === 'jwk' ? new Uint8Array() : copyBytes(keyData);
    const imported = normalizeImportedAlgorithm(algorithm, fmt, bytes, keyData, keyUsages);
    return subtleKey(imported.type, imported.algorithm, extractable, keyUsages, imported.data, imported.material);
  },
  async exportKey(format, key) {
    assertCryptoKey(key, 'exportKey');
    if (!key.extractable) throw new Error('SubtleCrypto exportKey requires an extractable key');
    const fmt = String(format).toLowerCase();
    if (fmt === 'raw') {
      const material = key.__fxeKeyMaterial;
      if (material?.kind === 'ec' && key.type === 'public') {
        return bytesToArrayBuffer(concatBytes(new Uint8Array([0x04]), padToLength(material.x, 32), padToLength(material.y, 32)));
      }
      if (key.type !== 'secret') throw new Error('SubtleCrypto raw exportKey requires a secret key or ECDSA public key');
      return bytesToArrayBuffer(normalizeKeyBytes(key));
    }
    if (fmt === 'jwk') return exportJwk(key);
    if (fmt === 'spki') {
      if (key.type !== 'public') throw new Error('SubtleCrypto spki exportKey requires a public key');
      return bytesToArrayBuffer(requirePkFn('writePublicKeyDer')(key.__fxeKeyMaterial));
    }
    if (fmt === 'pkcs8') {
      if (key.type !== 'private') throw new Error('SubtleCrypto pkcs8 exportKey requires a private key');
      return bytesToArrayBuffer(requirePkFn('writePrivateKeyDer')(key.__fxeKeyMaterial));
    }
    throw new Error(`unsupported SubtleCrypto exportKey format: ${format}`);
  },
  async generateKey(algorithm, extractable = true, keyUsages = []) {
    const name = normalizeSubtleName(algorithm);
    if (name === 'AES-CBC' || name === 'AES-GCM') {
      validateUsages(keyUsages, ['encrypt', 'decrypt', 'wrapKey', 'unwrapKey'], name);
      const length = Number(algorithm?.length ?? 256);
      aesCipherName(name, new Uint8Array(length / 8));
      return subtleKey('secret', { name, length }, extractable, keyUsages, randomBytes(length / 8));
    }
    if (name === 'HMAC') {
      validateUsages(keyUsages, ['sign', 'verify'], 'HMAC');
      const hmac = normalizeHmacAlgorithm(algorithm);
      const length = Number(algorithm?.length ?? 256);
      if (!Number.isSafeInteger(length) || length <= 0 || length % 8 !== 0) {
        throw new Error('SubtleCrypto HMAC generateKey length must be a positive byte-aligned bit length');
      }
      return subtleKey('secret', { name, hash: hmac.hash, length }, extractable, keyUsages, randomBytes(length / 8));
    }
    if (name === 'ECDSA') {
      validateUsages(keyUsages, ['sign', 'verify'], 'ECDSA');
      const namedCurve = normalizeNamedCurve(algorithm);
      const material = requirePkFn('ecdsaGenerate')(namedCurve);
      const publicMaterial = { kind: 'ec', curve: material.curve, x: material.x, y: material.y };
      return {
        privateKey: subtleKey('private', { name, namedCurve }, extractable, keyUsages.filter((u) => u === 'sign'), new Uint8Array(0), material),
        publicKey: subtleKey('public', { name, namedCurve }, true, keyUsages.filter((u) => u === 'verify'), new Uint8Array(0), publicMaterial),
      };
    }
    throw new Error(`unsupported SubtleCrypto generateKey algorithm: ${name || '<missing>'}`);
  },
  async encrypt(algorithm, key, data) {
    const requestedName = normalizeSubtleName(algorithm);
    if (requestedName === 'RSA-OAEP') {
      assertKeyAlgorithm(key, 'RSA-OAEP', 'encrypt');
      assertKeyUsage(key, 'encrypt', 'encrypt');
      if (key.type !== 'public') throw new Error('SubtleCrypto RSA-OAEP encrypt requires a public key');
      const oaep = normalizeRsaOaepAlgorithm(algorithm, key, 'encrypt');
      return bytesToArrayBuffer(requirePkFn('rsaOaepEncrypt')(key.__fxeKeyMaterial, oaep.digest, oaep.label, toUint8Array(data)));
    }
    const { name, iv, additionalData, tagLengthBytes } = normalizeAesAlgorithm(algorithm, 'encrypt');
    assertKeyAlgorithm(key, name, 'encrypt');
    assertKeyUsage(key, 'encrypt', 'encrypt');
    const keyBytes = normalizeKeyBytes(key);
    const cipher = createCipheriv(aesCipherName(name, keyBytes), keyBytes, iv);
    if (additionalData !== undefined) cipher.setAAD(additionalData);
    const encrypted = concatBytes(toUint8Array(cipher.update(toUint8Array(data))), toUint8Array(cipher.final()));
    if (name === 'AES-GCM') return bytesToArrayBuffer(concatBytes(encrypted, toUint8Array(cipher.getAuthTag()).slice(0, tagLengthBytes)));
    return bytesToArrayBuffer(encrypted);
  },
  async decrypt(algorithm, key, data) {
    const requestedName = normalizeSubtleName(algorithm);
    if (requestedName === 'RSA-OAEP') {
      assertKeyAlgorithm(key, 'RSA-OAEP', 'decrypt');
      assertKeyUsage(key, 'decrypt', 'decrypt');
      if (key.type !== 'private') throw new Error('SubtleCrypto RSA-OAEP decrypt requires a private key');
      const oaep = normalizeRsaOaepAlgorithm(algorithm, key, 'decrypt');
      return bytesToArrayBuffer(requirePkFn('rsaOaepDecrypt')(key.__fxeKeyMaterial, oaep.digest, oaep.label, toUint8Array(data)));
    }
    const { name, iv, additionalData, tagLengthBytes } = normalizeAesAlgorithm(algorithm, 'decrypt');
    assertKeyAlgorithm(key, name, 'decrypt');
    assertKeyUsage(key, 'decrypt', 'decrypt');
    const keyBytes = normalizeKeyBytes(key);
    const input = toUint8Array(data);
    const decipher = createDecipheriv(aesCipherName(name, keyBytes), keyBytes, iv);
    if (additionalData !== undefined) decipher.setAAD(additionalData);
    if (name === 'AES-GCM') {
      if (input.byteLength < tagLengthBytes) throw new Error('SubtleCrypto AES-GCM ciphertext is shorter than its tag');
      decipher.setAuthTag(input.slice(input.byteLength - tagLengthBytes));
      const ciphertext = input.slice(0, input.byteLength - tagLengthBytes);
      return bytesToArrayBuffer(concatBytes(toUint8Array(decipher.update(ciphertext)), toUint8Array(decipher.final())));
    }
    return bytesToArrayBuffer(concatBytes(toUint8Array(decipher.update(input)), toUint8Array(decipher.final())));
  },
  async sign(algorithm, key, data) {
    const requestedName = normalizeSubtleName(algorithm);
    if (requestedName === 'ECDSA') {
      assertKeyAlgorithm(key, 'ECDSA', 'sign');
      assertKeyUsage(key, 'sign', 'sign');
      if (key.type !== 'private') throw new Error('SubtleCrypto ECDSA sign requires a private key');
      const ecdsa = normalizeEcdsaAlgorithm(algorithm, key, 'sign');
      return bytesToArrayBuffer(requirePkFn('ecdsaSign')(key.__fxeKeyMaterial, ecdsa.digest, toUint8Array(data)));
    }
    assertKeyAlgorithm(key, 'HMAC', 'sign');
    assertKeyUsage(key, 'sign', 'sign');
    const hmac = normalizeHmacAlgorithm(algorithm, key);
    return bytesToArrayBuffer(createHmac(hmac.digest, normalizeKeyBytes(key)).update(toUint8Array(data)).digest());
  },
  async verify(algorithm, key, signature, data) {
    const requestedName = normalizeSubtleName(algorithm);
    if (requestedName === 'ECDSA') {
      assertKeyAlgorithm(key, 'ECDSA', 'verify');
      assertKeyUsage(key, 'verify', 'verify');
      const ecdsa = normalizeEcdsaAlgorithm(algorithm, key, 'verify');
      return Boolean(requirePkFn('ecdsaVerify')(key.__fxeKeyMaterial, ecdsa.digest, toUint8Array(data), copyBytes(signature)));
    }
    assertKeyAlgorithm(key, 'HMAC', 'verify');
    assertKeyUsage(key, 'verify', 'verify');
    const hmac = normalizeHmacAlgorithm(algorithm, key);
    const actual = toUint8Array(createHmac(hmac.digest, normalizeKeyBytes(key)).update(toUint8Array(data)).digest());
    const expected = toUint8Array(signature);
    if (actual.byteLength !== expected.byteLength) return false;
    return requirePkFn('timingSafeEqual')(actual, expected);
  },
  async deriveBits(algorithm, baseKey, length) {
    const name = normalizeSubtleName(algorithm);
    assertSubtleAlgorithm(name, 'PBKDF2', 'deriveBits');
    assertKeyAlgorithm(baseKey, 'PBKDF2', 'deriveBits');
    assertKeyUsage(baseKey, 'deriveBits', 'deriveBits');
    const bits = Number(length);
    if (!Number.isSafeInteger(bits) || bits < 0 || bits % 8 !== 0) {
      throw new Error('SubtleCrypto PBKDF2 deriveBits length must be a non-negative byte-aligned bit length');
    }
    const iterations = Number(algorithm?.iterations);
    if (!Number.isSafeInteger(iterations) || iterations <= 0) throw new Error('SubtleCrypto PBKDF2 iterations must be a positive safe integer');
    return bytesToArrayBuffer(pbkdf2Sync(normalizeKeyBytes(baseKey), toUint8Array(algorithm?.salt), iterations, bits / 8, normalizeWebCryptoHash(algorithm?.hash)));
  },
  async deriveKey(algorithm, baseKey, derivedKeyType, extractable = true, keyUsages = []) {
    assertKeyUsage(baseKey, 'deriveKey', 'deriveKey');
    const bits = Number(derivedKeyType?.length ?? 256);
    if (!Number.isSafeInteger(bits) || bits <= 0 || bits % 8 !== 0) throw new Error('SubtleCrypto deriveKey length must be a positive byte-aligned bit length');
    const keyBytes = bits / 8;
    const raw = new Uint8Array(await this.deriveBits(algorithm, { ...baseKey, usages: [...baseKey.usages, 'deriveBits'] }, bits));
    return this.importKey('raw', raw.slice(0, keyBytes), derivedKeyType, extractable, keyUsages);
  },
  async wrapKey(format, key, wrappingKey, wrapAlgorithm) {
    assertKeyUsage(wrappingKey, 'wrapKey', 'wrapKey');
    const exported = await this.exportKey(format, key);
    const data = exported instanceof ArrayBuffer ? exported : new TextEncoder().encode(JSON.stringify(exported));
    return this.encrypt(wrapAlgorithm, { ...wrappingKey, usages: [...wrappingKey.usages, 'encrypt'] }, data);
  },
  async unwrapKey(format, wrappedKey, unwrappingKey, unwrapAlgorithm, unwrappedKeyAlgorithm, extractable = true, keyUsages = []) {
    assertKeyUsage(unwrappingKey, 'unwrapKey', 'unwrapKey');
    const raw = await this.decrypt(unwrapAlgorithm, { ...unwrappingKey, usages: [...unwrappingKey.usages, 'decrypt'] }, wrappedKey);
    const fmt = String(format).toLowerCase();
    const keyData = fmt === 'jwk' ? JSON.parse(new TextDecoder().decode(raw)) : raw;
    return this.importKey(format, keyData, unwrappedKeyAlgorithm, extractable, keyUsages);
  },
};

export const webcrypto = { getRandomValues, subtle };

const cryptoGlobal = { getRandomValues, subtle };
if (typeof globalThis.crypto !== 'object' || globalThis.crypto === null) {
  Object.defineProperty(globalThis, 'crypto', {
    value: cryptoGlobal,
    configurable: true,
    enumerable: false,
    writable: true,
  });
} else {
  if (typeof globalThis.crypto.getRandomValues !== 'function') {
    Object.defineProperty(globalThis.crypto, 'getRandomValues', {
      value: getRandomValues,
      configurable: true,
      enumerable: true,
      writable: true,
    });
  }
  if (typeof globalThis.crypto.subtle !== 'object' || globalThis.crypto.subtle === null) {
    Object.defineProperty(globalThis.crypto, 'subtle', {
      value: subtle,
      configurable: true,
      enumerable: true,
      writable: true,
    });
  }
}

export default {
  randomFillSync,
  randomBytes,
  createHash,
  createHmac,
  createCipheriv,
  createDecipheriv,
  pbkdf2,
  pbkdf2Sync,
  scrypt,
  scryptSync,
  getRandomValues,
  webcrypto,
  subtle,
};
