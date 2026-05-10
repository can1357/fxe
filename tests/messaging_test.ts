import { assertDeepEqual, assertEqual, run, test } from './ts_harness.ts';

async function flushMessages(): Promise<void> {
  await Promise.resolve();
  await Promise.resolve();
}

test('BroadcastChannel asynchronously delivers to peers but not sender', async () => {
  assertEqual(typeof BroadcastChannel, 'function');

  const sender = new BroadcastChannel('fxe-messaging-broadcast');
  const peer = new BroadcastChannel('fxe-messaging-broadcast');
  const listenerPeer = new BroadcastChannel('fxe-messaging-broadcast');
  const isolated = new BroadcastChannel('fxe-messaging-other');
  const closedBeforeDispatch = new BroadcastChannel('fxe-messaging-broadcast');

  const senderMessages: unknown[] = [];
  const peerMessages: unknown[] = [];
  const listenerMessages: unknown[] = [];
  const isolatedMessages: unknown[] = [];
  const closedMessages: unknown[] = [];

  sender.onmessage = (event) => senderMessages.push(event.data);
  peer.onmessage = (event) => peerMessages.push(event.data);
  isolated.onmessage = (event) => isolatedMessages.push(event.data);
  closedBeforeDispatch.onmessage = (event) => closedMessages.push(event.data);

  const listener = (event: { data: unknown }) => listenerMessages.push(event.data);
  listenerPeer.addEventListener('message', listener);

  sender.postMessage({ nested: { value: 7 } });
  closedBeforeDispatch.close();
  await flushMessages();

  assertDeepEqual(senderMessages, []);
  assertDeepEqual(peerMessages, [{ nested: { value: 7 } }]);
  assertDeepEqual(listenerMessages, [{ nested: { value: 7 } }]);
  assertDeepEqual(isolatedMessages, []);
  assertDeepEqual(closedMessages, []);

  listenerPeer.removeEventListener('message', listener);
  sender.postMessage('after-remove');
  await flushMessages();
  assertDeepEqual(listenerMessages, [{ nested: { value: 7 } }]);
  assertDeepEqual(peerMessages, [{ nested: { value: 7 } }, 'after-remove']);

  peer.close();
  sender.postMessage('after-close');
  await flushMessages();
  assertDeepEqual(peerMessages, [{ nested: { value: 7 } }, 'after-remove']);

  sender.close();
  listenerPeer.close();
  isolated.close();
});

test('MessageChannel delivers cloned messages and respects close', async () => {
  assertEqual(typeof MessageChannel, 'function');
  assertEqual(typeof MessagePort, 'function');

  const channel = new MessageChannel();
  const received: unknown[] = [];
  const listened: unknown[] = [];

  channel.port2.onmessage = (event) => received.push(event.data);
  const listener = (event: { data: unknown }) => listened.push(event.data);
  channel.port2.addEventListener('message', listener);
  channel.port2.start();

  const payload = { nested: { value: 11 } };
  channel.port1.postMessage(payload);
  payload.nested.value = 99;
  await flushMessages();

  assertDeepEqual(received, [{ nested: { value: 11 } }]);
  assertDeepEqual(listened, [{ nested: { value: 11 } }]);

  channel.port2.removeEventListener('message', listener);
  channel.port1.postMessage('after-remove');
  await flushMessages();
  assertDeepEqual(received, [{ nested: { value: 11 } }, 'after-remove']);
  assertDeepEqual(listened, [{ nested: { value: 11 } }]);

  channel.port1.postMessage('pending-close');
  channel.port2.close();
  await flushMessages();
  assertDeepEqual(received, [{ nested: { value: 11 } }, 'after-remove']);

  channel.port1.close();
  channel.port1.postMessage('closed-sender');
  await flushMessages();
  assertDeepEqual(received, [{ nested: { value: 11 } }, 'after-remove']);
});

await run();
