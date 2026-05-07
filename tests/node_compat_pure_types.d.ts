interface ImportMeta {
  readonly url: string;
}

declare module 'node:assert/strict' {
  const assertStrict: {
    equal(actual: unknown, expected: unknown, message?: string): void;
  };
  export default assertStrict;
}

declare module 'node:async_hooks' {
  export class AsyncLocalStorage<T> {
    run<R>(store: T, callback: () => R): R;
    getStore(): T | undefined;
  }
}

declare module 'node:stream/consumers' {
  import { Readable } from 'node:stream';

  export function text(stream: Readable): Promise<string>;
}

declare module 'node:module' {
  export function createRequire(filename: string): (specifier: string) => unknown;
}
