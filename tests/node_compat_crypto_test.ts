// Native Node compatibility smoke tests intentionally import host-backed builtins
// that are provided by FXE at runtime rather than by @types/node.
import {
  createCipheriv,
  createDecipheriv,
  createHash,
  createHmac,
  getRandomValues,
  pbkdf2,
  pbkdf2Sync,
  randomBytes,
  randomFillSync,
  scrypt,
  scryptSync,
  webcrypto,
} from 'node:crypto';

import { assert, assertEqual, assertRejects, run, test } from './ts_harness.ts';

declare const TextEncoder: { new (): { encode(input?: string): Uint8Array } };
declare const TextDecoder: { new (): { decode(input?: ArrayBuffer | ArrayBufferView): string } };

const toHex = (bytes: Uint8Array): string =>
  Array.prototype.map.call(bytes, (b: number) => b.toString(16).padStart(2, '0')).join('');

const globalCrypto = crypto;

test('randomFillSync mutates a Uint8Array', () => {
  const bytes = new Uint8Array(32);
  randomFillSync(bytes);
  assert(
    bytes.some((value) => value !== 0),
    'expected randomFillSync to write non-zero data',
  );
});

test('randomBytes returns requested length', () => {
  assertEqual(randomBytes(17).length, 17);
});

test('createHash computes SHA-256 for abc', () => {
  const digest = createHash('sha256').update('abc').digest();
  assert(digest instanceof Uint8Array, 'digest must be Uint8Array-compatible');
  assertEqual(toHex(digest), 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad');
  assertEqual(
    createHash('sha256').update('abc').digest('hex'),
    'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad',
  );
});

test('createHmac computes HMAC-SHA256 for known vectors', () => {
  assertEqual(
    createHmac('sha256', 'key').update('The quick brown fox jumps over the lazy dog').digest('hex'),
    'f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8',
  );
});

test('createCipheriv and createDecipheriv round-trip AES-256-CBC', () => {
  const key = new Uint8Array(32);
  key.fill(1);
  const iv = new Uint8Array(16);
  iv.fill(2);
  const cipher = createCipheriv('aes-256-cbc', key, iv);
  const encrypted = `${cipher.update('hello world', 'utf8', 'hex')}${cipher.final('hex')}`;
  assertEqual(encrypted, 'f563737a376afbed282274255a7fcabd');

  const decipher = createDecipheriv('aes-256-cbc', key, iv);
  const decrypted = `${decipher.update(encrypted, 'hex', 'utf8')}${decipher.final('utf8')}`;
  assertEqual(decrypted, 'hello world');
});

test('createCipheriv and createDecipheriv round-trip AES-256-GCM', () => {
  const key = new Uint8Array(32);
  key.fill(3);
  const iv = new Uint8Array(12);
  iv.fill(4);
  const cipher = createCipheriv('aes-256-gcm', key, iv);
  const encrypted = cipher.update('hello gcm');
  const final = cipher.final();
  const tag = cipher.getAuthTag();

  const decipher = createDecipheriv('aes-256-gcm', key, iv);
  decipher.setAuthTag(tag);
  const decrypted = `${decipher.update(encrypted, undefined, 'utf8')}${decipher.update(final, undefined, 'utf8')}${decipher.final('utf8')}`;
  assertEqual(decrypted, 'hello gcm');
});

test('pbkdf2Sync derives deterministic SHA-256 keys', () => {
  assertEqual(
    toHex(pbkdf2Sync('password', 'salt', 1, 32, 'sha256')),
    '120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b',
  );
});

test('pbkdf2 callback receives derived key', async () => {
  const { promise, resolve, reject } = Promise.withResolvers<Uint8Array>();
  pbkdf2('password', 'salt', 1, 32, 'sha256', (error: unknown, derivedKey: Uint8Array) => {
    if (error) {
      reject(error);
    } else {
      resolve(derivedKey);
    }
  });
  assertEqual(
    toHex(await promise),
    '120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b',
  );
});

test('scryptSync derives RFC 7914 test vector', () => {
  assertEqual(
    toHex(scryptSync('', '', 64, { N: 16, r: 1, p: 1 })),
    '77d6576238657b203b19ca42c18a0497f16b4844e3074ae8dfdffa3fede21442fcd0069ded0948f8326a753a0fc81f17e8d3e0fb2e0d3628cf35e20c38d18906',
  );
});

test('scrypt callback receives derived key', async () => {
  const { promise, resolve, reject } = Promise.withResolvers<Uint8Array>();
  scrypt('', '', 64, { N: 16, r: 1, p: 1 }, (error: unknown, derivedKey: Uint8Array) => {
    if (error) {
      reject(error);
    } else {
      resolve(derivedKey);
    }
  });
  assertEqual(
    toHex(await promise),
    '77d6576238657b203b19ca42c18a0497f16b4844e3074ae8dfdffa3fede21442fcd0069ded0948f8326a753a0fc81f17e8d3e0fb2e0d3628cf35e20c38d18906',
  );
});

test('crypto getRandomValues fills typed arrays', () => {
  const bytes = new Uint8Array(16);
  assertEqual(getRandomValues(bytes), bytes);
  assert(
    bytes.some((value) => value !== 0),
    'expected getRandomValues export to write non-zero data',
  );

  const globalBytes = new Uint8Array(16);
  assertEqual(globalCrypto.getRandomValues(globalBytes), globalBytes);
  assert(
    globalBytes.some((value) => value !== 0),
    'expected global crypto.getRandomValues to write non-zero data',
  );
});

test('webcrypto getRandomValues fills typed arrays', () => {
  const bytes = new Uint8Array(16);
  assertEqual(webcrypto.getRandomValues(bytes), bytes);
  assert(
    bytes.some((value) => value !== 0),
    'expected webcrypto.getRandomValues to write non-zero data',
  );
});

test('webcrypto subtle.digest computes SHA-256', async () => {
  const digest = new Uint8Array(
    await webcrypto.subtle.digest('SHA-256', new Uint8Array([0x61, 0x62, 0x63])),
  );
  assertEqual(toHex(digest), 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad');
});

test('webcrypto subtle.importKey and exportKey copy raw AES-CBC key material', async () => {
  const keyBytes = new Uint8Array(16);
  keyBytes.fill(7);
  const key = await webcrypto.subtle.importKey('raw', keyBytes, { name: 'AES-CBC' }, true, [
    'encrypt',
    'decrypt',
  ]);
  keyBytes.fill(9);

  const exported = new Uint8Array(await webcrypto.subtle.exportKey('raw', key));
  assertEqual(toHex(exported), '07070707070707070707070707070707');
  exported[0] = 0xff;

  const exportedAgain = new Uint8Array(await webcrypto.subtle.exportKey('raw', key));
  assertEqual(toHex(exportedAgain), '07070707070707070707070707070707');
});

test('webcrypto subtle.sign and verify compute HMAC-SHA256 vectors', async () => {
  const key = await webcrypto.subtle.importKey(
    'raw',
    new TextEncoder().encode('key'),
    { name: 'HMAC', hash: 'SHA-256' },
    true,
    ['sign', 'verify'],
  );
  const data = new TextEncoder().encode('The quick brown fox jumps over the lazy dog');
  const signature = new Uint8Array(await webcrypto.subtle.sign('HMAC', key, data));

  assertEqual(toHex(signature), 'f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8');
  assert(
    await webcrypto.subtle.verify('HMAC', key, signature, data),
    'expected matching HMAC signature to verify',
  );
  assert(
    !(await webcrypto.subtle.verify('HMAC', key, signature, new TextEncoder().encode('tampered'))),
    'expected tampered data to fail HMAC verification',
  );
});

test('webcrypto subtle.deriveBits derives PBKDF2-SHA256 vector', async () => {
  const key = await webcrypto.subtle.importKey(
    'raw',
    new TextEncoder().encode('password'),
    { name: 'PBKDF2' },
    false,
    ['deriveBits', 'deriveKey'],
  );
  const bits = await webcrypto.subtle.deriveBits(
    { name: 'PBKDF2', salt: new TextEncoder().encode('salt'), iterations: 1, hash: 'SHA-256' },
    key,
    256,
  );

  assertEqual(
    toHex(new Uint8Array(bits)),
    '120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b',
  );
});

test('webcrypto subtle.deriveKey produces exportable AES-CBC key material', async () => {
  const baseKey = await webcrypto.subtle.importKey(
    'raw',
    new TextEncoder().encode('password'),
    { name: 'PBKDF2' },
    false,
    ['deriveKey'],
  );
  const aesKey = await webcrypto.subtle.deriveKey(
    { name: 'PBKDF2', salt: new TextEncoder().encode('salt'), iterations: 1, hash: 'SHA-256' },
    baseKey,
    { name: 'AES-CBC', length: 128 },
    true,
    ['encrypt', 'decrypt'],
  );

  const exported = new Uint8Array(await webcrypto.subtle.exportKey('raw', aesKey));
  assertEqual(exported.byteLength, 16);
  assertEqual(toHex(exported), '120fb6cffcf8b32c43e7225256c4f837');
});

test('webcrypto subtle.deriveKey respects AES-CBC bit lengths exactly', async () => {
  const baseKey = await webcrypto.subtle.importKey(
    'raw',
    new TextEncoder().encode('password'),
    { name: 'PBKDF2' },
    false,
    ['deriveKey'],
  );

  const vectors: Array<[number, number, string]> = [
    [128, 16, '120fb6cffcf8b32c43e7225256c4f837'],
    [192, 24, '120fb6cffcf8b32c43e7225256c4f837a86548c92ccc3548'],
    [256, 32, '120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b'],
  ];

  for (const [length, bytes, expectedHex] of vectors) {
    const key = await webcrypto.subtle.deriveKey(
      { name: 'PBKDF2', salt: new TextEncoder().encode('salt'), iterations: 1, hash: 'SHA-256' },
      baseKey,
      { name: 'AES-CBC', length },
      true,
      ['encrypt', 'decrypt'],
    );
    const exported = new Uint8Array(await webcrypto.subtle.exportKey('raw', key));
    assertEqual(exported.byteLength, bytes);
    assertEqual(toHex(exported), expectedHex);
  }
});

test('webcrypto subtle.deriveKey rejects invalid AES-CBC lengths clearly', async () => {
  const baseKey = await webcrypto.subtle.importKey(
    'raw',
    new TextEncoder().encode('password'),
    { name: 'PBKDF2' },
    false,
    ['deriveKey'],
  );

  await assertRejects(
    () =>
      webcrypto.subtle.deriveKey(
        { name: 'PBKDF2', salt: new TextEncoder().encode('salt'), iterations: 1, hash: 'SHA-256' },
        baseKey,
        { name: 'AES-CBC', length: 129 },
        true,
        ['encrypt', 'decrypt'],
      ),
    /positive byte-aligned bit length/,
  );

  await assertRejects(
    () =>
      webcrypto.subtle.deriveKey(
        { name: 'PBKDF2', salt: new TextEncoder().encode('salt'), iterations: 1, hash: 'SHA-256' },
        baseKey,
        { name: 'AES-CBC', length: 120 },
        true,
        ['encrypt', 'decrypt'],
      ),
    /unsupported AES key size for AES-CBC: 120/,
  );
});

test('webcrypto subtle.encrypt and decrypt round-trip AES-CBC vectors', async () => {
  const keyBytes = new Uint8Array(32);
  keyBytes.fill(1);
  const iv = new Uint8Array(16);
  iv.fill(2);
  const key = await webcrypto.subtle.importKey('raw', keyBytes, { name: 'AES-CBC' }, true, [
    'encrypt',
    'decrypt',
  ]);
  const encrypted = new Uint8Array(
    await webcrypto.subtle.encrypt(
      { name: 'AES-CBC', iv },
      key,
      new TextEncoder().encode('hello world'),
    ),
  );

  assertEqual(toHex(encrypted), 'f563737a376afbed282274255a7fcabd');
  const decrypted = new TextDecoder().decode(
    await webcrypto.subtle.decrypt({ name: 'AES-CBC', iv }, key, encrypted),
  );
  assertEqual(decrypted, 'hello world');
});

test('webcrypto subtle.encrypt and decrypt round-trip AES-GCM vectors', async () => {
  const key = await webcrypto.subtle.importKey(
    'raw',
    new Uint8Array(32).fill(5),
    { name: 'AES-GCM' },
    true,
    ['encrypt', 'decrypt'],
  );
  const iv = new Uint8Array(12).fill(6);
  const encrypted = await webcrypto.subtle.encrypt(
    { name: 'AES-GCM', iv },
    key,
    new TextEncoder().encode('subtle-gcm'),
  );
  const decrypted = new TextDecoder().decode(
    await webcrypto.subtle.decrypt({ name: 'AES-GCM', iv }, key, encrypted),
  );
  assertEqual(decrypted, 'subtle-gcm');
});

const ecP256PrivateJwk = {
  kty: 'EC',
  crv: 'P-256',
  x: 'axfR8uEsQkf4vOblY6RA8ncDfYEt6zOg9KE5RdiYwpY',
  y: 'T-NC4v4af5uO5-tKfA-eFivOM1drMV7Oy7ZAaDe_UfU',
  d: 'AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAE',
};

const ecP256PublicJwk = {
  kty: 'EC',
  crv: 'P-256',
  x: 'axfR8uEsQkf4vOblY6RA8ncDfYEt6zOg9KE5RdiYwpY',
  y: 'T-NC4v4af5uO5-tKfA-eFivOM1drMV7Oy7ZAaDe_UfU',
};

const rsaOaepPublicJwk = {
  kty: 'RSA',
  n: '5qse6m-1XglHYsGUmTS1UfuBq77oD6chaRuTdKEGjnyUtExMmf98ij6pW79KAVyR7iIquic2xlDazsjHocUpgTkqJ5lyE0xpLh6YcweNP5uHsygws-Gpsg4IlmkcOobyUovjdS2nS1s2YhhFzdLWQfbe89PxpRTw7aH7t_sHm_c',
  e: 'AQAB',
};

const rsaOaepPrivateJwk = {
  ...rsaOaepPublicJwk,
  d: '4WngMGEx789Jf2yE9qLXfmI1ecx5orJEqB5WnuatLZj5CKh3Qxa0cbSCdDIe8-2uffPNpVSk5nAlI2Z6D9BImTAAUB_RPYVkr8-l5Znlk-413ruyL-BevxKLY_6mkUQ1-_PI6bQZeOK6D5Tf8ubfReD5VWXjH7dGuivXpEIE6CE',
};

test('webcrypto subtle.sign and verify support ECDSA P-256 JWK keys', async () => {
  const privateKey = await webcrypto.subtle.importKey(
    'jwk',
    ecP256PrivateJwk,
    { name: 'ECDSA', namedCurve: 'P-256' },
    true,
    ['sign'],
  );
  const publicKey = await webcrypto.subtle.importKey(
    'jwk',
    ecP256PublicJwk,
    { name: 'ECDSA', namedCurve: 'P-256' },
    true,
    ['verify'],
  );
  const data = new TextEncoder().encode('ecdsa message');
  const signature = new Uint8Array(
    await webcrypto.subtle.sign({ name: 'ECDSA', hash: 'SHA-256' }, privateKey, data),
  );

  assertEqual(signature.byteLength, 64);
  assert(
    await webcrypto.subtle.verify({ name: 'ECDSA', hash: 'SHA-256' }, publicKey, signature, data),
    'expected ECDSA P-256 signature to verify',
  );
  assert(
    !(await webcrypto.subtle.verify(
      { name: 'ECDSA', hash: 'SHA-256' },
      publicKey,
      signature,
      new TextEncoder().encode('tampered'),
    )),
    'expected tampered ECDSA data to fail verification',
  );
});

test('webcrypto subtle.encrypt and decrypt support RSA-OAEP JWK keys', async () => {
  const publicKey = await webcrypto.subtle.importKey(
    'jwk',
    rsaOaepPublicJwk,
    { name: 'RSA-OAEP', hash: 'SHA-1' },
    true,
    ['encrypt'],
  );
  const privateKey = await webcrypto.subtle.importKey(
    'jwk',
    rsaOaepPrivateJwk,
    { name: 'RSA-OAEP', hash: 'SHA-1' },
    false,
    ['decrypt'],
  );
  const data = new TextEncoder().encode('rsa oaep');
  const encrypted = await webcrypto.subtle.encrypt({ name: 'RSA-OAEP' }, publicKey, data);
  const decrypted = new TextDecoder().decode(
    await webcrypto.subtle.decrypt({ name: 'RSA-OAEP' }, privateKey, encrypted),
  );

  assertEqual(decrypted, 'rsa oaep');
});

test('webcrypto subtle.wrapKey and unwrapKey round-trip extractable keys only', async () => {
  const keyToWrap = await webcrypto.subtle.importKey(
    'raw',
    new Uint8Array([1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16]),
    { name: 'AES-CBC' },
    true,
    ['encrypt'],
  );
  const wrappingKey = await webcrypto.subtle.importKey(
    'raw',
    new Uint8Array(32).fill(0x5a),
    { name: 'AES-GCM' },
    false,
    ['wrapKey', 'unwrapKey'],
  );
  const iv = new Uint8Array(12).fill(0xa5);
  const wrapped = await webcrypto.subtle.wrapKey('raw', keyToWrap, wrappingKey, {
    name: 'AES-GCM',
    iv,
  });
  const unwrapped = await webcrypto.subtle.unwrapKey(
    'raw',
    wrapped,
    wrappingKey,
    { name: 'AES-GCM', iv },
    { name: 'AES-CBC' },
    false,
    ['encrypt'],
  );

  await assertRejects(() => webcrypto.subtle.exportKey('raw', unwrapped), /extractable/);
  await assertRejects(
    () => webcrypto.subtle.wrapKey('raw', unwrapped, wrappingKey, { name: 'AES-GCM', iv }),
    /extractable/,
  );
});

await run();
