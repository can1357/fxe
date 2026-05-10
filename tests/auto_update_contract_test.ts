import { createHash } from 'node:crypto';
import { existsSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { assertEqual, assertRejects, run, test } from './ts_harness.ts';

type FetchStub = (url: string, opts?: unknown) => Promise<unknown> | unknown;

const originalFetch = globalThis.fetch;
const originalVerifyUpdateSignature = (App as unknown as { __fxeVerifyUpdateSignature?: unknown })
  .__fxeVerifyUpdateSignature;

function trustSignedManifest(): void {
  (App as unknown as { __fxeVerifyUpdateSignature: () => boolean }).__fxeVerifyUpdateSignature =
    () => true;
}

function restoreUpdateSignatureVerifier(): void {
  if (typeof originalVerifyUpdateSignature === 'function') {
    (App as unknown as { __fxeVerifyUpdateSignature: unknown }).__fxeVerifyUpdateSignature =
      originalVerifyUpdateSignature;
  }
}

function stubFetch(fn: FetchStub): void {
  globalThis.fetch = fn as typeof globalThis.fetch;
}

function manifestResponse(manifest: unknown, ok = true): unknown {
  return {
    ok,
    status: ok ? 200 : 503,
    async json() {
      return manifest;
    },
  };
}

function join(...parts: string[]): string {
  let out = parts[0] ?? '';
  for (const raw of parts.slice(1)) {
    const part = String(raw);
    out = out.endsWith('/')
      ? out + (part.startsWith('/') ? part.slice(1) : part)
      : out + (part.startsWith('/') ? part : `/${part}`);
  }
  return out;
}

function sha256Hex(bytes: Uint8Array): string {
  return createHash('sha256').update(bytes).digest('hex');
}

function cleanupUpdateVersions(versions: string[]): void {
  const root = join(App.getPath('userData'), 'updates');
  for (const version of versions) {
    rmSync(join(root, version), { recursive: true, force: true });
  }
  const historyPath = join(root, 'history.txt');
  if (existsSync(historyPath)) {
    const next = readFileSync(historyPath, 'utf8')
      .split('\n')
      .filter((line) => line.length > 0 && !versions.includes(line))
      .join('\n');
    writeFileSync(historyPath, next.length > 0 ? `${next}\n` : '');
  }
}

function stageUpdateForRollback(version: string, body: string): void {
  const bytes = new TextEncoder().encode(body);
  const api = App as unknown as {
    __fxeStageUpdate(descriptor: unknown, artifact: Uint8Array): { ok: boolean };
    __fxeApplyPendingUpdate(): boolean;
  };
  const staged = api.__fxeStageUpdate(
    {
      version,
      url: `file:///tmp/${version}.bin`,
      sha256: sha256Hex(bytes),
    },
    bytes,
  );
  assertEqual(staged.ok, true);
  assertEqual(api.__fxeApplyPendingUpdate(), true);
}

test('App.checkForUpdates fetches and validates update manifest', async () => {
  let seenUrl = '';
  let seenOpts: unknown;
  trustSignedManifest();
  stubFetch((url, opts) => {
    seenUrl = url;
    seenOpts = opts;
    return manifestResponse({
      version: '1.2.3',
      url: 'https://updates.example/fxe.zip',
      sha256: '0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef',
      signature: 'signed-feed',
    });
  });

  try {
    const result = await App.checkForUpdates('https://updates.example/manifest.json', {
      headers: { accept: 'application/json' },
      expectedFeedPublicKey: 'feed-public-key',
    });

    assertEqual(seenUrl, 'https://updates.example/manifest.json');
    assertEqual(typeof seenOpts, 'object');
    assertEqual(result.available, true);
    assertEqual(result.version, '1.2.3');
    assertEqual(result.url, 'https://updates.example/fxe.zip');
    assertEqual(result.sha256, '0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef');
    assertEqual(result.canInstall, true);
    assertEqual(result.reason, undefined);
    assertEqual(result.installUnavailableReason, undefined);
    assertEqual(result.missingCapabilities.length, 0);
  } finally {
    globalThis.fetch = originalFetch;
    restoreUpdateSignatureVerifier();
  }
});

test('App.checkForUpdates reports current version as unavailable without installing', async () => {
  trustSignedManifest();
  stubFetch(() =>
    manifestResponse({
      version: App.getVersion(),
      url: 'https://updates.example/fxe-current.zip',
      sha256: 'abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789',
      signature: 'signed-feed',
    }),
  );

  try {
    const result = await App.checkForUpdates('https://updates.example/current.json', {
      expectedFeedPublicKey: 'feed-public-key',
    });
    assertEqual(result.available, false);
    assertEqual(result.version, App.getVersion());
    assertEqual(result.url, 'https://updates.example/fxe-current.zip');
    assertEqual(result.sha256, 'abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789');
    assertEqual(result.canInstall, false);
    assertEqual(result.reason, 'update version is already installed');
    assertEqual(result.installUnavailableReason, result.reason);
  } finally {
    globalThis.fetch = originalFetch;
    restoreUpdateSignatureVerifier();
  }
});

test('App.checkForUpdates rejects unavailable fetch', async () => {
  try {
    Reflect.deleteProperty(globalThis, 'fetch');
    await assertRejects(
      () => App.checkForUpdates('https://updates.example/manifest.json'),
      /requires globalThis\.fetch/,
    );
  } finally {
    globalThis.fetch = originalFetch;
  }
});

test('App.checkForUpdates rejects malformed manifests', async () => {
  const malformed = [
    null,
    [],
    { url: 'https://updates.example/fxe.zip', sha256: 'a'.repeat(64) },
    { version: '1.2.3', sha256: 'a'.repeat(64) },
    { version: '1.2.3', url: 'https://updates.example/fxe.zip' },
    { version: '1.2.3', url: 'https://updates.example/fxe.zip', sha256: 'not-a-valid-sha256' },
    {
      version: '1.2.3',
      url: 'https://updates.example/fxe.zip',
      sha256: 'a'.repeat(64),
      signature: 42,
    },
  ];

  try {
    for (const manifest of malformed) {
      stubFetch(() => manifestResponse(manifest));
      await assertRejects(
        () => App.checkForUpdates('https://updates.example/manifest.json'),
        /manifest/,
      );
    }
  } finally {
    globalThis.fetch = originalFetch;
  }
});

test('App.checkForUpdates rejects failed fetch responses', async () => {
  stubFetch(() =>
    manifestResponse(
      {
        version: '1.2.3',
        url: 'https://updates.example/fxe.zip',
        sha256: 'abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789',
      },
      false,
    ),
  );

  try {
    await assertRejects(
      () => App.checkForUpdates('https://updates.example/manifest.json'),
      /failed to fetch manifest/,
    );
  } finally {
    globalThis.fetch = originalFetch;
  }
});

test('App.checkForUpdates reports invalid signed manifests as non-installable', async () => {
  stubFetch(() =>
    manifestResponse({
      version: '1.2.3',
      url: 'https://updates.example/fxe.zip',
      sha256: '0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef',
      signature: 'signed-manifest',
    }),
  );

  try {
    const result = await App.checkForUpdates('https://updates.example/manifest.json', {
      expectedPublicKey: 'test-public-key',
      publicKey: 'test-public-key',
    });

    assertEqual(result.available, true);
    assertEqual(result.canInstall, false);
    assertEqual(result.reason, 'signature verification failed');
    assertEqual(result.installUnavailableReason, result.reason);
    assertEqual(result.missingCapabilities.includes('trusted signed manifest'), true);
  } finally {
    globalThis.fetch = originalFetch;
  }
});

test('App.checkForUpdates validates public key options for signed manifests', async () => {
  stubFetch(() =>
    manifestResponse({
      version: '1.2.3',
      url: 'https://updates.example/fxe.zip',
      sha256: '0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef',
      signature: 'signed-manifest',
    }),
  );

  try {
    await assertRejects(
      () =>
        App.checkForUpdates('https://updates.example/manifest.json', {
          expectedPublicKey: '',
        }),
      /expectedPublicKey/,
    );
  } finally {
    globalThis.fetch = originalFetch;
  }
});

test('App.update.setChannel substitutes feed URL channel templates', async () => {
  let seenUrl = '';
  trustSignedManifest();
  App.update.setChannel('beta');
  stubFetch((url) => {
    seenUrl = url;
    return manifestResponse({
      version: '7.7.7',
      url: 'https://updates.example/{channel}/fxe.zip',
      sha256: '0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef',
      signature: 'signed-feed',
      channel: 'beta',
      rollout_percent: 100,
    });
  });

  try {
    const result = await App.checkForUpdates('https://updates.example/{channel}/manifest.json', {
      expectedFeedPublicKey: 'feed-public-key',
    });
    assertEqual(seenUrl, 'https://updates.example/beta/manifest.json');
    assertEqual(result.channel, 'beta');
    assertEqual(result.url, 'https://updates.example/beta/fxe.zip');
    assertEqual(result.canInstall, true);
  } finally {
    App.update.setChannel('stable');
    restoreUpdateSignatureVerifier();
    globalThis.fetch = originalFetch;
  }
});

test('App.checkForUpdates applies rollout eligibility from stable device hash', async () => {
  const rollout = App as unknown as {
    __fxeUpdateRolloutEligible(percent: number, deviceId: string): boolean;
  };
  assertEqual(rollout.__fxeUpdateRolloutEligible(0, 'device-a'), false);
  assertEqual(rollout.__fxeUpdateRolloutEligible(100, 'device-a'), true);
  trustSignedManifest();
  stubFetch(() =>
    manifestResponse({
      version: '8.8.8',
      url: 'https://updates.example/fxe.zip',
      sha256: '0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef',
      signature: 'signed-feed',
      rollout_percent: 0,
    }),
  );

  try {
    const result = await App.checkForUpdates('https://updates.example/manifest.json', {
      expectedFeedPublicKey: 'feed-public-key',
      deviceId: 'device-a',
    });
    assertEqual(result.available, true);
    assertEqual(result.canInstall, false);
    assertEqual(result.rolloutPercent, 0);
    assertEqual(result.reason, 'device is not eligible for rollout');
  } finally {
    restoreUpdateSignatureVerifier();
    globalThis.fetch = originalFetch;
  }
});

test('App.update.rollback swaps history back to the previous installed version', () => {
  const versions = [`rollback-${process.pid}-1`, `rollback-${process.pid}-2`];
  cleanupUpdateVersions(versions);

  try {
    stageUpdateForRollback(versions[0], 'rollback body one');
    stageUpdateForRollback(versions[1], 'rollback body two');
    let history = App.update.history();
    assertEqual(history[0], versions[1]);
    assertEqual(history[1], versions[0]);

    assertEqual(App.update.rollback(), true);
    history = App.update.history();
    assertEqual(history[0], versions[0]);
    assertEqual(history[1], versions[1]);
  } finally {
    cleanupUpdateVersions(versions);
  }
});

await run();
