import {
  assert,
  assertDeepEqual,
  assertEqual,
  assertRejects,
  assertThrows,
  test,
} from './ts_harness.ts';

test('Headers normalizes names and preserves combined values case-insensitively', () => {
  const headers = new Headers([
    ['Content-Type', 'text/plain'],
    ['X-Trace', 'first'],
  ]);

  assertEqual(headers.get('content-type'), 'text/plain');
  assertEqual(headers.get('CONTENT-TYPE'), 'text/plain');
  assert(headers.has('x-trace'));
  assert(headers.has('X-TRACE'));

  headers.append('x-trace', 'second');
  assertEqual(headers.get('X-Trace'), 'first, second');

  headers.set('X-TRACE', 'final');
  assertEqual(headers.get('x-trace'), 'final');

  headers.delete('CONTENT-type');
  assertEqual(headers.has('content-type'), false);
  assertEqual(headers.get('content-type'), null);
});

test('fetch.cookieJar exposes set get and clear', () => {
  assert(fetch.cookieJar);
  const jar = fetch.cookieJar();
  jar.clear();
  jar.set('example.test', 'sid', 'abc', '/');
  assertEqual(jar.get('http://example.test/path'), 'sid=abc');
  jar.clear();
  assertEqual(jar.get('http://example.test/path'), '');
});

test('Headers accepts records, arrays, and other Headers without aliasing', () => {
  const fromRecord = new Headers({ Accept: 'application/json', 'X-Mode': 'record' });
  assertEqual(fromRecord.get('accept'), 'application/json');
  assertEqual(fromRecord.get('x-mode'), 'record');

  const copied = new Headers(fromRecord);
  copied.set('accept', 'text/plain');

  assertEqual(fromRecord.get('ACCEPT'), 'application/json');
  assertEqual(copied.get('ACCEPT'), 'text/plain');
});

test('Headers forEach reports lower-case keys, values, and parent', () => {
  const headers = new Headers({ B: '2', A: '1' });
  const seen: string[] = [];
  const parents: Headers[] = [];

  headers.forEach((value, key, parent) => {
    seen.push(`${key}:${value}`);
    parents.push(parent);
  });

  assertDeepEqual(seen, ['b:2', 'a:1']);
  assertEqual(parents.length, 2);
  assert(
    parents.every((parent) => parent === headers),
    'forEach should pass the Headers instance',
  );
});

test('Headers constructor and methods reject/ignore invalid receiver shape predictably', () => {
  assertThrows(() => {
    (Headers as unknown as () => Headers)();
  }, 'Headers must be called with new');

  const detachedGet = Headers.prototype.get as unknown as (name: string) => string | null;
  assertEqual(detachedGet('missing'), undefined as unknown as string | null);
});

test('Request exposes url and method from string input and init', () => {
  const getRequest = new Request('http://127.0.0.1/resource');
  assertEqual(getRequest.url, 'http://127.0.0.1/resource');
  assertEqual(getRequest.method, 'GET');

  const postRequest = new Request('http://127.0.0.1/post', {
    method: 'POST',
    headers: { 'X-Test': 'yes' },
    body: 'payload',
  });
  assertEqual(postRequest.url, 'http://127.0.0.1/post');
  assertEqual(postRequest.method, 'POST');
});

test('Request validates construction and unsupported stream bodies', () => {
  assertThrows(() => {
    (Request as unknown as () => Request)();
  }, 'Request must be called with new');

  assertThrows(() => {
    new Request('http://127.0.0.1/upload', {
      body: { getReader() {} } as unknown as ArrayBuffer,
    });
  }, 'ReadableStream body is not supported');
});

test('AbortController exposes signal state and fires abort listeners once', () => {
  const controller = new AbortController();
  const signal = controller.signal;
  const calls: string[] = [];

  assertEqual(signal.aborted, false);
  assertEqual(signal.reason, undefined);

  signal.addEventListener('abort', () => calls.push(`first:${signal.reason}`));
  signal.addEventListener('abort', () => calls.push(`second:${signal.aborted}`));
  signal.addEventListener('ignored' as 'abort', () => calls.push('ignored'));

  controller.abort('test-reason');
  controller.abort('second-reason');

  assertEqual(signal.aborted, true);
  assertEqual(signal.reason, 'test-reason');
  assertDeepEqual(calls, ['first:test-reason', 'second:true']);
});

test('AbortSignal cannot be directly constructed and default abort reason is stable', () => {
  assertThrows(() => {
    new (AbortSignal as unknown as { new (): AbortSignal })();
  }, 'AbortSignal cannot be constructed directly');

  const controller = new AbortController();
  controller.abort();
  assertEqual(controller.signal.aborted, true);
  assertEqual(controller.signal.reason, 'aborted');
});

test('fetch rejects missing and empty URLs with TypeError messages', async () => {
  await assertRejects(() => (fetch as unknown as () => Promise<Response>)(), 'fetch: missing url');
  await assertRejects(() => fetch(''), 'fetch: empty url');
});

test('fetch rejects unsupported stream request bodies', async () => {
  await assertRejects(
    () =>
      fetch('http://127.0.0.1/upload', {
        body: { getReader() {} } as unknown as ArrayBuffer,
      }),
    'fetch: ReadableStream body is not supported in v0',
  );
});

test('fetch rejects pre-aborted signals before network submission', async () => {
  const controller = new AbortController();
  controller.abort('already-done');

  await assertRejects(
    () => fetch('http://127.0.0.1/pre-aborted', { signal: controller.signal }),
    'aborted: already-done',
  );
});

test('fetch accepts invalid local URLs for pre-submit rejection paths without network dependency', async () => {
  const controller = new AbortController();
  controller.abort('invalid-local-url');

  await assertRejects(
    () => fetch('http://127.0.0.1:0/', { signal: controller.signal }),
    'aborted: invalid-local-url',
  );
});

test('Response constructor state and body methods are verified when constructible', async () => {
  let response: Response;
  try {
    response = new (
      Response as unknown as {
        new (body?: string, init?: { status?: number; headers?: HeadersInit }): Response;
      }
    )('{"ok":true}', { status: 201, headers: [['Content-Type', 'application/json']] });
  } catch (error) {
    assert(error instanceof Error, 'Response constructor should throw an Error when unavailable');
    assert(
      error.message.includes('Response constructor is not implemented in v0'),
      `unexpected Response constructor error: ${error.message}`,
    );
    return;
  }

  assertEqual(response.status, 201);
  assertEqual(response.ok, true);
  assertEqual(response.bodyUsed, false);
  assertEqual(response.headers.get('content-type'), 'application/json');

  const text = await response.text();
  assertEqual(text, '{"ok":true}');
  assertEqual(response.bodyUsed, true);

  const jsonResponse = new (Response as unknown as { new (body?: string): Response })(
    '{"answer":42}',
  );
  assertDeepEqual(await jsonResponse.json(), { answer: 42 });

  const bufferResponse = new (Response as unknown as { new (body?: string): Response })('abc');
  const buffer = await bufferResponse.arrayBuffer();
  assertDeepEqual(Array.from(new Uint8Array(buffer)), [97, 98, 99]);
});
