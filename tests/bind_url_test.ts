import { assert, assertDeepEqual, assertEqual, assertThrows, test } from './ts_harness.ts';

test('URL parses and serializes absolute special URLs', () => {
  const url = new URL('HTTPS://User:Pass@Example.COM:443/a/b?x=1&x=2#top');

  assertEqual(url.href, 'https://User:Pass@example.com/a/b?x=1&x=2#top');
  assertEqual(url.toString(), url.href);
  assertEqual(url.protocol, 'https:');
  assertEqual(url.host, 'example.com');
  assertEqual(url.hostname, 'example.com');
  assertEqual(url.port, '');
  assertEqual(url.pathname, '/a/b');
  assertEqual(url.search, '?x=1&x=2');
  assertEqual(url.hash, '#top');
  assertEqual(url.origin, 'https://example.com');
  assertEqual(url.username, 'User');
  assertEqual(url.password, 'Pass');
  assertEqual(url.searchParams.get('x'), '1');
  assertDeepEqual(url.searchParams.getAll('x'), ['1', '2']);
  assert(url.searchParams.has('x'));
});

test('URL preserves non-default ports and computes origins', () => {
  const http = new URL('http://example.com:8080/path');
  assertEqual(http.host, 'example.com:8080');
  assertEqual(http.port, '8080');
  assertEqual(http.origin, 'http://example.com:8080');
  assertEqual(http.href, 'http://example.com:8080/path');

  const ws = new URL('ws://socket.example:80/channel');
  assertEqual(ws.port, '');
  assertEqual(ws.origin, 'ws://socket.example');

  const custom = new URL('custom:opaque/path?x=1#h');
  assertEqual(custom.protocol, 'custom:');
  assertEqual(custom.host, '');
  assertEqual(custom.pathname, 'opaque/path');
  assertEqual(custom.origin, 'null');
  assertEqual(custom.href, 'custom:opaque/path?x=1#h');
});

test('URL resolves relative URLs against a base', () => {
  const child = new URL('child?x=1#frag', 'https://example.com/dir/page.html?old=1#old');
  assertEqual(child.href, 'https://example.com/dir/child?x=1#frag');
  assertEqual(child.protocol, 'https:');
  assertEqual(child.host, 'example.com');
  assertEqual(child.pathname, '/dir/child');
  assertEqual(child.search, '?x=1');
  assertEqual(child.hash, '#frag');
  assertEqual(child.origin, 'https://example.com');

  const absolutePath = new URL('/rooted?q=yes', 'https://example.com/dir/page.html');
  assertEqual(absolutePath.href, 'https://example.com/rooted?q=yes');
  assertEqual(absolutePath.pathname, '/rooted');

  const queryOnly = new URL('?fresh=1', 'https://example.com/dir/page.html?old=1#old');
  assertEqual(queryOnly.href, 'https://example.com/dir/page.html?fresh=1');
  assertEqual(queryOnly.search, '?fresh=1');
  assertEqual(queryOnly.hash, '');

  const fragmentOnly = new URL('#new', 'https://example.com/dir/page.html?old=1#old');
  assertEqual(fragmentOnly.href, 'https://example.com/dir/page.html#new');
  assertEqual(fragmentOnly.search, '');
  assertEqual(fragmentOnly.hash, '#new');

  const protocolRelative = new URL(
    '//cdn.example.com/lib.js',
    'https://example.com/app/index.html',
  );
  assertEqual(protocolRelative.href, 'https://cdn.example.com/lib.js');
  assertEqual(protocolRelative.origin, 'https://cdn.example.com');
});

test('URL setters update serialized components', () => {
  const url = new URL('http://example.com/a?x=1#old');

  url.protocol = 'https';
  url.hostname = 'Api.EXAMPLE.com';
  url.port = '8443';
  url.pathname = 'v1/items';
  url.search = 'q=hello';
  url.hash = 'section';

  assertEqual(url.protocol, 'https:');
  assertEqual(url.hostname, 'api.example.com');
  assertEqual(url.host, 'api.example.com:8443');
  assertEqual(url.pathname, '/v1/items');
  assertEqual(url.search, '?q=hello');
  assertEqual(url.hash, '#section');
  assertEqual(url.href, 'https://api.example.com:8443/v1/items?q=hello#section');
  assertEqual(url.origin, 'https://api.example.com:8443');

  url.search = '';
  url.hash = '';
  assertEqual(url.href, 'https://api.example.com:8443/v1/items');

  url.href = 'ws://Example.org:123/socket?room=a#open';
  assertEqual(url.protocol, 'ws:');
  assertEqual(url.hostname, 'example.org');
  assertEqual(url.port, '123');
  assertEqual(url.pathname, '/socket');
  assertEqual(url.searchParams.get('room'), 'a');
  assertEqual(url.href, 'ws://example.org:123/socket?room=a#open');
});

test('URL rejects invalid inputs and invalid bases', () => {
  assertThrows(() => new URL('relative/path'), /invalid url/);
  assertThrows(() => new URL('https:path-without-authority'), /invalid url/);
  assertThrows(() => new URL('/path', 'not a base'), /invalid base/);
  assertThrows(() => new URL('http://[broken/path'), /invalid url/);

  const url = new URL('https://example.com/');
  assertThrows(() => {
    url.href = '/relative';
  }, /URL\.href: invalid url/);
});

test('URLSearchParams reads strings with decoding and repeated keys', () => {
  const params = new URLSearchParams('?a=1&b=two+words&a=3&empty=&bare&encoded=%7Bok%7D&&bad=%ZZ');

  assertEqual(params.get('a'), '1');
  assertDeepEqual(params.getAll('a'), ['1', '3']);
  assertEqual(params.get('b'), 'two words');
  assertEqual(params.get('empty'), '');
  assertEqual(params.get('bare'), '');
  assertEqual(params.get('encoded'), '{ok}');
  assertEqual(params.get('bad'), '%ZZ');
  assertEqual(params.get('missing'), null);
  assert(params.has('a'));
  assert(!params.has('missing'));
  assertEqual(params.toString(), 'a=1&b=two+words&a=3&empty=&bare=&encoded=%7Bok%7D&bad=%25ZZ');
});

test('URLSearchParams mutators preserve order and replace duplicates', () => {
  const params = new URLSearchParams('a=1&a=2&b=3');

  params.append('a', '4');
  params.append('space key', 'value/with?punct');
  assertDeepEqual(params.getAll('a'), ['1', '2', '4']);
  assertEqual(params.toString(), 'a=1&a=2&b=3&a=4&space+key=value%2Fwith%3Fpunct');

  params.set('a', 'only');
  assertDeepEqual(params.getAll('a'), ['only']);
  assertEqual(params.toString(), 'a=only&b=3&space+key=value%2Fwith%3Fpunct');

  params.set('new', 'added');
  assertEqual(params.get('new'), 'added');
  assertEqual(params.toString(), 'a=only&b=3&space+key=value%2Fwith%3Fpunct&new=added');

  params.delete('b');
  assert(!params.has('b'));
  assertEqual(params.toString(), 'a=only&space+key=value%2Fwith%3Fpunct&new=added');
});

test('URLSearchParams constructors accept arrays and records', () => {
  const fromArray = new URLSearchParams([
    ['x', '1'],
    ['x', '2'],
    ['y', 'space value'],
  ]);
  assertEqual(fromArray.toString(), 'x=1&x=2&y=space+value');

  const fromRecord = new URLSearchParams({ a: 'alpha', b: 'two words' });
  assertEqual(fromRecord.get('a'), 'alpha');
  assertEqual(fromRecord.get('b'), 'two words');
  assertEqual(fromRecord.toString(), 'a=alpha&b=two+words');
});

test('URLSearchParams forEach receives value, key, and parent in order', () => {
  const params = new URLSearchParams('a=1&b=2&a=3');
  const seen: string[] = [];
  const parents: boolean[] = [];

  params.forEach((value, key, parent) => {
    seen.push(`${key}:${value}`);
    parents.push(parent === params);
  });

  assertDeepEqual(seen, ['a:1', 'b:2', 'a:3']);
  assertDeepEqual(parents, [true, true, true]);
});

test('URL searchParams reflects the current search string', () => {
  const url = new URL('https://example.com/?a=1&a=2');

  assertDeepEqual(url.searchParams.getAll('a'), ['1', '2']);

  url.search = '?b=3&space=two+words';
  assertEqual(url.searchParams.get('a'), null);
  assertEqual(url.searchParams.get('b'), '3');
  assertEqual(url.searchParams.get('space'), 'two words');
});

test('URL exposes WHATWG-compatible getters for supported components', () => {
  const url = new URL('https://user:pw@example.com:8443/path/sub?q=1#h');

  assertEqual(url.protocol, 'https:');
  assertEqual(url.username, 'user');
  assertEqual(url.password, 'pw');
  assertEqual(url.hostname, 'example.com');
  assertEqual(url.port, '8443');
  assertEqual(url.host, 'example.com:8443');
  assertEqual(url.pathname, '/path/sub');
  assertEqual(url.search, '?q=1');
  assertEqual(url.hash, '#h');
  assertEqual(url.origin, 'https://example.com:8443');
  assertEqual(url.href, 'https://user:pw@example.com:8443/path/sub?q=1#h');
  assertEqual(url.toString(), url.href);
});

test('URL resolves relatives and keeps search/searchParams in sync', () => {
  assertEqual(new URL('../a', 'https://x.com/b/c/').href, 'https://x.com/b/a');
  assertThrows(() => new URL('a'));

  const url = new URL('https://example.com/root/start?q=1');
  const params = url.searchParams;

  assert(params === url.searchParams);

  url.pathname = '/next';
  assertEqual(url.href, 'https://example.com/next?q=1');

  url.search = '?name=fxe&name=url';
  assertDeepEqual(params.getAll('name'), ['fxe', 'url']);

  params.set('name', 'bound');
  params.append('extra', '1');
  assertEqual(url.search, '?name=bound&extra=1');
  assertEqual(url.href, 'https://example.com/next?name=bound&extra=1');
});

test('URLSearchParams iterators preserve insertion order and spec sort stability', () => {
  const params = new URLSearchParams('b=1&a=first&a=second&c=3');
  const seen: [string, string][] = [];

  for (const entry of params) {
    seen.push(entry);
  }

  assertDeepEqual(seen, [
    ['b', '1'],
    ['a', 'first'],
    ['a', 'second'],
    ['c', '3'],
  ]);
  assertDeepEqual([...params.entries()], seen);
  assertDeepEqual([...params.keys()], ['b', 'a', 'a', 'c']);
  assertDeepEqual([...params.values()], ['1', 'first', 'second', '3']);

  params.sort();
  assertDeepEqual(
    [...params.entries()],
    [
      ['a', 'first'],
      ['a', 'second'],
      ['b', '1'],
      ['c', '3'],
    ],
  );
});

test('URLSearchParams handles deletion, size, encoding, and parsing edges', () => {
  const params = new URLSearchParams('dup=1&dup=2&space+key=a+b&encoded=%E2%98%83');
  params.delete('dup');
  assertEqual(params.has('dup'), false);

  const expectedSize = params.getAll('space key').length + params.getAll('encoded').length;
  assertEqual('size' in params ? params.size : [...params].length, expectedSize);

  const special = new URLSearchParams([
    ['a&b', '1=2'],
    ['plus+', 'x+y'],
    ['space key', 'two words'],
    ['unicode', '雪'],
  ]);
  const reparsed = new URLSearchParams(special.toString());
  assertDeepEqual(
    [...reparsed.entries()],
    [
      ['a&b', '1=2'],
      ['plus+', 'x+y'],
      ['space key', 'two words'],
      ['unicode', '雪'],
    ],
  );

  assertEqual(new URLSearchParams('?a=1').get('a'), '1');
  assertEqual(new URLSearchParams('a').get('a'), '');
});
