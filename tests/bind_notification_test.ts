import { assert, assertEqual, assertThrows, test } from './ts_harness.ts';

test('Notification exposes permission and requestPermission', async () => {
  assertEqual(typeof Notification, 'function');
  assertEqual(Notification.permission, 'granted');
  assertEqual(typeof Notification.requestPermission, 'function');
  assertEqual(await Notification.requestPermission(), 'granted');
});

test('Notification constructor creates instances with show method', () => {
  const notification = new Notification({
    title: 'Test title',
    body: 'Test body',
    icon: 'test-icon.png',
  });

  assert(notification instanceof Notification, 'constructor should create Notification instances');
  assertEqual(typeof notification.show, 'function');

  if (globalThis.__FXE_TYPECHECK_ONLY__ === true) {
    void notification.show();
  }
});

test('Notification constructor accepts action options', () => {
  const notification = new Notification({
    title: 'Action test',
    body: 'Choose an action',
    actions: [
      { id: 'accept', title: 'Accept', kind: 'button' },
      { id: 'reply', title: 'Reply', kind: 'input' },
    ],
    onAction(event) {
      assert(typeof event.id === 'string', 'action event should include an id');
      if (event.input !== undefined) {
        assert(typeof event.input === 'string', 'action input should be a string when present');
      }
    },
  });

  assert(notification instanceof Notification, 'constructor should create Notification instances');
  assertEqual(typeof notification.show, 'function');
});

test('Notification constructor rejects calls without new', () => {
  assertThrows(() => {
    (Notification as unknown as (opts: NotificationOptions) => Notification)({ title: 'bad call' });
  }, /new/);
});
