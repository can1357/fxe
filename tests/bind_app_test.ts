import { assert, assertEqual, assertThrows, test } from './ts_harness.ts';

type AppPathKind = 'userData' | 'documents' | 'downloads' | 'temp' | 'home';

const pathKinds: AppPathKind[] = ['userData', 'documents', 'downloads', 'temp', 'home'];

function assertNonEmptyString(value: unknown, label: string): asserts value is string {
  assert(typeof value === 'string', `${label} should be a string`);
  assert(value.length > 0, `${label} should not be empty`);
}

test('App exposes name and version strings', () => {
  assertEqual(App.getName(), 'fxe');
  assertEqual(App.getVersion(), '0.0.0');
});

test('App.getPath returns strings for supported path kinds', () => {
  for (const kind of pathKinds) {
    assertNonEmptyString(App.getPath(kind), `App.getPath(${kind})`);
  }
});

test('App.bookmark persists, resolves, and scopes access', () => {
  const path = '/tmp';
  const blob = App.bookmark.persist(path);
  assertNonEmptyString(blob, 'App.bookmark.persist(/tmp)');
  if (process.platform !== 'darwin') {
    assertEqual(blob, path);
  }

  const resolved = App.bookmark.resolve(blob);
  assertEqual(resolved.path, path);
  assertEqual(resolved.isStale, false);
  assertEqual(App.bookmark.startAccessing(blob), true);
  assertEqual(App.bookmark.stopAccessing(blob), undefined);
});

test('App.session.cookies exposes in-memory cookie controls', () => {
  App.session.cookies.clear();
  App.session.cookies.set({
    name: 'sid',
    value: 'abc',
    url: 'http://example.test/account/login',
    httpOnly: true,
  });

  const cookies = App.session.cookies.getAll({ url: 'http://example.test/account/profile' });
  assertEqual(cookies.length, 1);
  assertEqual(cookies[0].name, 'sid');
  assertEqual(cookies[0].value, 'abc');
  assertEqual(cookies[0].sameSite, 'Lax');
  assertEqual(cookies[0].httpOnly, true);

  assertThrows(() => {
    App.session.cookies.set({
      name: 'thirdParty',
      value: '1',
      url: 'http://example.test/',
      sameSite: 'None',
    });
  }, 'rejected invalid cookie');

  App.session.cookies.remove('sid', 'http://example.test/account/profile');
  assertEqual(App.session.cookies.getAll({ url: 'http://example.test/account/profile' }).length, 0);
});

test('App.requestSingleInstanceLock accepts a unique id', () => {
  assertEqual(App.requestSingleInstanceLock('fxe.bind_app_test.single_instance'), true);
});

test('App.setBadgeCount accepts zero', () => {
  assertEqual(App.setBadgeCount(0), undefined);
});

test('App.whenReady resolves', async () => {
  assertEqual(await App.whenReady(), undefined);
});

test('App.windows returns an empty snapshot when no windows exist', () => {
  const windows = App.windows();
  assert(Array.isArray(windows), 'App.windows should return an array');
  assertEqual(windows.length, 0, 'bind_app_test must run without creating windows');
});

const appRunTypeOnly = (): void => {
  App.run();
  App.run({ animate: false });
  App.run({ fps: 0 });
  App.run({ animate: true, fps: 60 });
};
void appRunTypeOnly;
