import { assertEqual, test } from './ts_harness.ts';

test('storage: localStorage set/get/property/delete roundtrip', () => {
  const key = `fxe-storage-test-${Date.now()}`;
  localStorage.removeItem(key);
  assertEqual(localStorage.getItem(key), null);

  localStorage.setItem(key, '1');
  assertEqual(localStorage.getItem(key), '1');
  assertEqual(localStorage[key], '1');

  localStorage[key] = '2';
  assertEqual(localStorage.getItem(key), '2');

  delete localStorage[key];
  assertEqual(localStorage.getItem(key), null);
});

test('storage: sessionStorage length key clear and string coercion', () => {
  sessionStorage.clear();
  assertEqual(sessionStorage.length, 0);
  assertEqual(sessionStorage.key(0), null);

  sessionStorage.setItem('alpha', '10');
  sessionStorage.beta = true;
  assertEqual(sessionStorage.length, 2);
  assertEqual(sessionStorage.getItem('alpha'), '10');
  assertEqual(sessionStorage.getItem('beta'), 'true');
  assertEqual(sessionStorage.key(0), 'alpha');
  assertEqual(sessionStorage.key(1), 'beta');
  assertEqual(sessionStorage.key(2), null);

  sessionStorage.removeItem('alpha');
  assertEqual(sessionStorage.length, 1);
  sessionStorage.clear();
  assertEqual(sessionStorage.length, 0);
});
