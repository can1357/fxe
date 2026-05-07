import { assert, assertDeepEqual, assertEqual, run, test } from './ts_harness.ts';

type MessageEventLike<T = unknown> = {
  readonly type: 'message';
  readonly data: T;
};

type MessageListener = (event: MessageEventLike) => void;

type BroadcastChannelLike = {
  readonly name: string;
  onmessage: MessageListener | null;
  postMessage(value: unknown): void;
  addEventListener(type: 'message', listener: MessageListener): void;
  removeEventListener(type: 'message', listener: MessageListener): void;
  close(): void;
};

type MessagePortLike = {
  onmessage: MessageListener | null;
  postMessage(value: unknown): void;
  addEventListener(type: 'message', listener: MessageListener): void;
  removeEventListener(type: 'message', listener: MessageListener): void;
  start(): void;
  close(): void;
};

type MessageChannelLike = {
  readonly port1: MessagePortLike;
  readonly port2: MessagePortLike;
};

const globals = globalThis as unknown as {
  BroadcastChannel: new (name: string) => BroadcastChannelLike;
  MessageChannel: new () => MessageChannelLike;
  MessagePort: new () => MessagePortLike;
};

async function flushMessages(): Promise<void> {
  await Promise.resolve();
  await Promise.resolve();
}

test('BroadcastChannel asynchronously delivers to peers but not sender', async () => {
  assertEqual(typeof globals.BroadcastChannel, 'function');

  const sender = new globals.BroadcastChannel('fxe-messaging-broadcast');
  const peer = new globals.BroadcastChannel('fxe-messaging-broadcast');
  const listenerPeer = new globals.BroadcastChannel('fxe-messaging-broadcast');
  const isolated = new globals.BroadcastChannel('fxe-messaging-other');
  const closedBeforeDispatch = new globals.BroadcastChannel('fxe-messaging-broadcast');

  const senderMessages: unknown[] = [];
  const peerMessages: unknown[] = [];
  const listenerMessages: unknown[] = [];
  const isolatedMessages: unknown[] = [];
  const closedMessages: unknown[] = [];

  sender.onmessage = (event) => senderMessages.push(event.data);
  peer.onmessage = (event) => peerMessages.push(event.data);
  isolated.onmessage = (event) => isolatedMessages.push(event.data);
  closedBeforeDispatch.onmessage = (event) => closedMessages.push(event.data);

  const listener = (event: MessageEventLike) => listenerMessages.push(event.data);
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
  assertEqual(typeof globals.MessageChannel, 'function');
  assertEqual(typeof globals.MessagePort, 'function');

  const channel = new globals.MessageChannel();
  const received: unknown[] = [];
  const listened: unknown[] = [];

  channel.port2.onmessage = (event) => received.push(event.data);
  const listener = (event: MessageEventLike) => listened.push(event.data);
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
