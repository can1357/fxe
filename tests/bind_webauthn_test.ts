import { Window } from 'fxe';
import { assert, assertEqual, assertRejects, test } from './ts_harness.ts';

test('navigator.credentials.create rejects without webauthn permission', async () => {
  const win = new Window({ width: 64, height: 64, visible: false });
  try {
    await assertRejects(
      () =>
        navigator.credentials.create({
          publicKey: {
            rp: { id: 'example.com', name: 'Example' },
            user: { id: new Uint8Array([1, 2, 3]), name: 'alice', displayName: 'Alice' },
            challenge: new Uint8Array(16),
            pubKeyCredParams: [{ type: 'public-key', alg: -7 }],
          },
        }),
      /NotAllowedError/,
    );
  } finally {
    win.close();
  }
});

test('register + assert via virtual authenticator', async () => {
  const win = new Window({
    width: 64,
    height: 64,
    visible: false,
    permissions: { webauthn: true },
  });
  try {
    const challenge1 = new Uint8Array(16);
    challenge1.fill(0xa5);
    const cred = (await navigator.credentials.create({
      publicKey: {
        rp: { id: 'example.com', name: 'Example' },
        user: { id: new Uint8Array([1, 2, 3]), name: 'alice', displayName: 'Alice' },
        challenge: challenge1,
        pubKeyCredParams: [{ type: 'public-key', alg: -7 }],
        attestation: 'none',
      },
    })) as PublicKeyCredential;
    assert(cred !== null);
    assertEqual(cred.type, 'public-key');
    const att = cred.response as AuthenticatorAttestationResponse;
    assert(att.attestationObject.byteLength > 0);
    assert(att.clientDataJSON.byteLength > 0);
    assertEqual(att.getPublicKeyAlgorithm(), -7);
    assert(att.getTransports().includes('internal'));

    const challenge2 = new Uint8Array(16);
    challenge2.fill(0xc3);
    const assertion = (await navigator.credentials.get({
      publicKey: {
        rpId: 'example.com',
        challenge: challenge2,
        allowCredentials: [{ type: 'public-key', id: cred.rawId }],
      },
    })) as PublicKeyCredential;
    assert(assertion !== null);
    const ar = assertion.response as AuthenticatorAssertionResponse;
    assert(ar.signature.byteLength > 0);
    assert(ar.authenticatorData.byteLength >= 37);
    assertEqual(assertion.id, cred.id);
  } finally {
    win.close();
  }
});

test('rp-id mismatch with explicit allowlist rejects', async () => {
  const win = new Window({
    width: 64,
    height: 64,
    visible: false,
    permissions: { webauthn: { rpIds: ['allowed.example'], allowVirtualAuthenticator: true } },
  });
  try {
    await assertRejects(
      () =>
        navigator.credentials.create({
          publicKey: {
            rp: { id: 'evil.example', name: 'Evil' },
            user: { id: new Uint8Array([1]), name: 'a', displayName: 'A' },
            challenge: new Uint8Array(16),
            pubKeyCredParams: [{ type: 'public-key', alg: -7 }],
          },
        }),
      /NotAllowedError/,
    );
  } finally {
    win.close();
  }
});

test('already-aborted signal rejects with AbortError', async () => {
  const win = new Window({
    width: 64,
    height: 64,
    visible: false,
    permissions: { webauthn: true },
  });
  try {
    const ac = new AbortController();
    ac.abort();
    await assertRejects(
      () =>
        navigator.credentials.create({
          publicKey: {
            rp: { id: 'example.com', name: 'Example' },
            user: { id: new Uint8Array([1]), name: 'a', displayName: 'A' },
            challenge: new Uint8Array(16),
            pubKeyCredParams: [{ type: 'public-key', alg: -7 }],
          },
          signal: ac.signal,
        }),
      /AbortError/,
    );
  } finally {
    win.close();
  }
});

test('isUserVerifyingPlatformAuthenticatorAvailable resolves', async () => {
  const result = await PublicKeyCredential.isUserVerifyingPlatformAuthenticatorAvailable();
  assertEqual(typeof result, 'boolean');
});
