import { dgram, dns, ipc, net, tcp, udp } from 'fxe:net';
import { assert, assertEqual, test } from './ts_harness.ts';

test('fxe:net exports native namespaces', () => {
  assert(typeof dns.lookup === 'function');
  assert(typeof dns.lookupService === 'function');
  assert(typeof dns.resolveRecord === 'function');
  assert(typeof net.listen === 'function');
  assert(typeof net.connect === 'function');
  assert(typeof ipc.listen === 'function');
  assert(typeof udp.bind === 'function');
  assertEqual(net, tcp);
  assertEqual(udp, dgram);
});

test('fxe:net dns.lookup resolves localhost', async () => {
  const { promise, resolve, reject } = Promise.withResolvers<void>();
  dns.lookup('localhost', { all: true }, (error, result) => {
    if (error) {
      reject(error);
      return;
    }
    try {
      assert(Array.isArray(result));
      assert(result.length > 0);
      assert(typeof result[0]?.address === 'string');
      assert(typeof result[0]?.family === 'number');
      resolve();
    } catch (caught) {
      reject(caught);
    }
  });
  await promise;
});
