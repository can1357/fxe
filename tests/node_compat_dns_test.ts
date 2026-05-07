// @ts-ignore FXE host-backed builtin
import dns, { lookup, resolve4, resolve6 } from 'node:dns';

import { assert, run, test } from './ts_harness.ts';

const enabled = process.env.FXE_DNS_TEST === '1';

function dnsCallback<T>(fn: (cb: (err: Error | null, value?: T) => void) => void): Promise<T> {
  const { promise, resolve, reject } = Promise.withResolvers<T>();
  fn((err, value) => {
    if (err) reject(err);
    else resolve(value as T);
  });
  return promise;
}

test('node:dns offline exports expanded resolver API', () => {
  assert(typeof dns.resolveTxt === 'function', 'resolveTxt should be exported');
  assert(typeof dns.resolveMx === 'function', 'resolveMx should be exported');
  assert(typeof dns.resolveSrv === 'function', 'resolveSrv should be exported');
  assert(typeof dns.resolveCname === 'function', 'resolveCname should be exported');
  assert(typeof dns.resolveNs === 'function', 'resolveNs should be exported');
  assert(typeof dns.reverse === 'function', 'reverse should be exported');
  assert(typeof dns.lookupService === 'function', 'lookupService should be exported');
});

test('node:dns online resolve4/resolve6 when FXE_DNS_TEST=1', async () => {
  if (!enabled) return;
  const v4 = await dnsCallback<string[]>((cb) => resolve4('one.one.one.one', cb));
  assert(
    v4.some((address) => /^\d+\.\d+\.\d+\.\d+$/.test(address)),
    'resolve4 should return IPv4 addresses',
  );

  const v6 = await dnsCallback<string[]>((cb) => resolve6('one.one.one.one', cb));
  assert(
    v6.some((address) => address.includes(':')),
    'resolve6 should return IPv6 addresses',
  );
});

test('node:dns online lookupService when FXE_DNS_TEST=1', async () => {
  if (!enabled) return;
  const { promise, resolve, reject } = Promise.withResolvers<{
    hostname: string;
    service: string;
  }>();
  lookup('1.1.1.1', { family: 4 }, (lookupError: Error | null, address?: string) => {
    if (lookupError) {
      reject(lookupError);
      return;
    }
    dns.lookupService(
      String(address),
      53,
      (serviceError: Error | null, hostname?: string, service?: string) => {
        if (serviceError) reject(serviceError);
        else resolve({ hostname: String(hostname), service: String(service) });
      },
    );
  });
  const result = await promise;
  assert(result.hostname.length > 0, 'lookupService should return a hostname');
  assert(result.service.length > 0, 'lookupService should return a service');
});

await run();
