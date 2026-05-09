// Native node:fs/crypto imports are provided by FXE at runtime.
import { createHash } from 'node:crypto';
import { existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs';

import { assertEqual, run, test } from './ts_harness.ts';

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

function toArrayBuffer(bytes: Uint8Array): ArrayBuffer {
  const copy = new Uint8Array(bytes.byteLength);
  copy.set(bytes);
  return copy.buffer;
}

function sha256Hex(bytes: Uint8Array): string {
  return createHash('sha256').update(bytes).digest('hex');
}

test('App.installUpdate stages a fetched artifact from a checked manifest', async () => {
  const dir = join(process.cwd(), `.fxe-auto-update-apply-${process.pid}`);
  mkdirSync(dir, { recursive: true });
  const bundleBytes = new Uint8Array([
    102, 120, 101, 32, 115, 116, 97, 103, 101, 100, 32, 98, 117, 110, 100, 108, 101, 32, 98, 121,
    116, 101, 115,
  ]);
  const bundlePath = join(dir, 'bundle.bin');
  writeFileSync(bundlePath, bundleBytes);

  const manifestUrl = `file://${join(dir, 'manifest.json')}`;
  const artifactUrl = `file://${bundlePath}`;
  const manifest = {
    version: '99.99.99',
    url: artifactUrl,
    sha256: sha256Hex(bundleBytes),
    signature: 'signed-feed',
  };
  writeFileSync(join(dir, 'manifest.json'), JSON.stringify(manifest));

  stubFetch((url) => {
    if (url === manifestUrl) {
      return {
        ok: true,
        async json() {
          return manifest;
        },
      };
    }
    if (url === artifactUrl) {
      return {
        ok: true,
        async arrayBuffer() {
          return toArrayBuffer(bundleBytes);
        },
      };
    }
    throw new Error(`unexpected fetch URL ${url}`);
  });
  trustSignedManifest();

  try {
    const checked = await App.checkForUpdates(manifestUrl, {
      expectedFeedPublicKey: 'feed-public-key',
    });
    assertEqual(checked.available, true);
    assertEqual(checked.canInstall, true);

    const installed = await App.installUpdate();
    assertEqual(installed.installed, true);
    const pendingPath = installed.pendingPath;
    if (typeof pendingPath !== 'string' || pendingPath.length === 0) {
      throw new Error('installUpdate should return pendingPath');
    }
    assertEqual(existsSync(pendingPath), true);
    assertEqual(readFileSync(pendingPath, 'utf8'), 'fxe staged bundle bytes');
  } finally {
    globalThis.fetch = originalFetch;
    restoreUpdateSignatureVerifier();
    rmSync(dir, { recursive: true, force: true });
    // Clean up the persisted updates/99.99.99 directory + history entry +
    // first-launch.flag so subsequent tests don't see stale state.
    const updatesRoot = `${App.getPath('userData')}/updates`;
    rmSync(`${updatesRoot}/99.99.99`, { recursive: true, force: true });
    rmSync(`${updatesRoot}/first-launch.flag`, { force: true });
    const histPath = `${updatesRoot}/history.txt`;
    if (existsSync(histPath)) {
      const next = readFileSync(histPath, 'utf8')
        .split('\n')
        .filter((line) => line.length > 0 && line !== '99.99.99')
        .join('\n');
      writeFileSync(histPath, next.length > 0 ? `${next}\n` : '');
    }
  }
});

await run();
