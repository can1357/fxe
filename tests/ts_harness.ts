export type TestFn = () => void | Promise<void>;

declare global {
  interface PromiseConstructor {
    withResolvers<T>(): {
      promise: Promise<T>;
      resolve: (value: T | PromiseLike<T>) => void;
      reject: (reason?: unknown) => void;
    };
  }
}

type TestCase = {
  name: string;
  fn: TestFn;
};

const tests: TestCase[] = [];

export function test(name: string, fn: TestFn): void {
  if (typeof name !== 'string' || name.length === 0) {
    throw new Error('test name must be a non-empty string');
  }
  tests.push({ name, fn });
}

export function assert(condition: unknown, message?: string): asserts condition {
  if (!condition) {
    throw new Error(message ?? 'assertion failed');
  }
}

export function assertEqual<T>(actual: T, expected: T, message?: string): void {
  if (!Object.is(actual, expected)) {
    throw new Error(message ?? `expected ${formatValue(actual)} to equal ${formatValue(expected)}`);
  }
}

export function assertDeepEqual(actual: unknown, expected: unknown, message?: string): void {
  const actualJson = stableJson(actual);
  const expectedJson = stableJson(expected);
  if (actualJson !== expectedJson) {
    throw new Error(message ?? `expected ${actualJson} to deep equal ${expectedJson}`);
  }
}

export function assertThrows(fn: () => unknown, pattern?: RegExp | string): void {
  try {
    fn();
  } catch (error) {
    assertMatches(errorMessage(error), pattern);
    return;
  }
  throw new Error('expected function to throw');
}

export async function assertRejects(
  fn: () => Promise<unknown>,
  pattern?: RegExp | string,
): Promise<void> {
  try {
    await fn();
  } catch (error) {
    assertMatches(errorMessage(error), pattern);
    return;
  }
  throw new Error('expected promise to reject');
}

export function delay(ms: number): Promise<void> {
  const { promise, resolve } = Promise.withResolvers<void>();
  setTimeout(resolve, ms);
  return promise;
}

export async function run(): Promise<void> {
  let passed = 0;
  let failed = 0;

  for (const { name, fn } of tests) {
    try {
      await fn();
      ++passed;
      console.log(`bind-test-pass=${name}`);
    } catch (error) {
      ++failed;
      console.log(`bind-test-fail=${name}: ${errorMessage(error)}`);
    }
  }

  console.log(`bind-tests=${passed}:${failed}`);
  if (failed > 0) {
    throw new Error(`bind tests failed: ${failed}`);
  }
}

export function resetTests(): void {
  tests.length = 0;
}

function assertMatches(message: string, pattern?: RegExp | string): void {
  if (pattern === undefined) {
    return;
  }
  if (typeof pattern === 'string') {
    if (!message.includes(pattern)) {
      throw new Error(
        `expected error message ${formatValue(message)} to include ${formatValue(pattern)}`,
      );
    }
    return;
  }
  if (!pattern.test(message)) {
    throw new Error(`expected error message ${formatValue(message)} to match ${String(pattern)}`);
  }
}

function errorMessage(error: unknown): string {
  if (error instanceof Error) {
    return error.message;
  }
  return String(error);
}

function formatValue(value: unknown): string {
  if (typeof value === 'string') {
    return JSON.stringify(value);
  }
  return stableJson(value);
}

function stableJson(value: unknown): string {
  return stableStringify(value, []);
}

function stableStringify(value: unknown, seen: unknown[]): string {
  if (value === null || typeof value !== 'object') {
    return primitiveJson(value);
  }

  if (seen.includes(value)) {
    throw new Error('cannot compare circular structures');
  }
  const nextSeen = [...seen, value];

  if (Array.isArray(value)) {
    return `[${value.map((item) => stableStringify(item, nextSeen)).join(',')}]`;
  }

  const record = value as Record<string, unknown>;
  const keys = Object.keys(record).sort();
  return `{${keys
    .map((key) => `${JSON.stringify(key)}:${stableStringify(record[key], nextSeen)}`)
    .join(',')}}`;
}

function primitiveJson(value: unknown): string {
  if (value === undefined) {
    return 'undefined';
  }
  if (typeof value === 'bigint') {
    return `${value.toString()}n`;
  }
  if (typeof value === 'symbol' || typeof value === 'function') {
    return String(value);
  }
  return JSON.stringify(value);
}
