declare module 'node:crypto' {
  type Binary = string | ArrayBuffer | ArrayBufferView;
  interface HashLike {
    update(data: Binary, inputEncoding?: string): HashLike;
    digest(): Uint8Array;
    digest(encoding: 'hex'): string;
  }
  interface CipherLike {
    update(data: Binary, inputEncoding?: string, outputEncoding?: string): Uint8Array | string;
    final(outputEncoding?: string): Uint8Array | string;
    getAuthTag(): Uint8Array;
    setAuthTag(tag: Uint8Array): void;
  }
  export function createHash(algorithm: string): HashLike;
  export function createHmac(algorithm: string, key: Binary): HashLike;
  export function createCipheriv(algorithm: string, key: Binary, iv: Binary): CipherLike;
  export function createDecipheriv(algorithm: string, key: Binary, iv: Binary): CipherLike;
  export function randomBytes(size: number): Uint8Array;
  export function randomFillSync<T extends ArrayBufferView>(buffer: T): T;
  export function getRandomValues<T extends ArrayBufferView>(array: T): T;
  export function pbkdf2Sync(
    password: Binary,
    salt: Binary,
    iterations: number,
    keylen: number,
    digest: string,
  ): Uint8Array;
  export function pbkdf2(
    password: Binary,
    salt: Binary,
    iterations: number,
    keylen: number,
    digest: string,
    callback: (error: unknown, derivedKey: Uint8Array) => void,
  ): void;
  export function scryptSync(
    password: Binary,
    salt: Binary,
    keylen: number,
    options?: unknown,
  ): Uint8Array;
  export function scrypt(
    password: Binary,
    salt: Binary,
    keylen: number,
    options: unknown,
    callback: (error: unknown, derivedKey: Uint8Array) => void,
  ): void;
}

declare module 'node:https' {
  import type { EventEmitter } from 'events';

  type ResponseLike = EventEmitter & { statusCode: number; headers: Record<string, string> };
  type RequestLike = EventEmitter & { end(data?: string): void };
  type RequestCallback = (res: ResponseLike) => void;
  export class Agent {}
  export function request(url: string, options: unknown, callback: RequestCallback): RequestLike;
  export function request(url: string, callback?: RequestCallback): RequestLike;
  export function get(url: string, callback?: RequestCallback): RequestLike;
  export function createServer(options?: unknown): unknown;
  const httpsDefault: {
    Agent: typeof Agent;
    globalAgent: Agent;
    request: typeof request;
    get: typeof get;
    createServer: typeof createServer;
  };
  export default httpsDefault;
}

declare module 'node:http2' {
  interface StreamLike {
    end(data?: string): void;
    on(event: 'response', listener: (headers: Record<string, number>) => void): StreamLike;
    on(event: 'error', listener: (error: Error) => void): StreamLike;
    on(event: string, listener: (...args: unknown[]) => void): StreamLike;
  }
  export const constants: Record<string, string | number> & {
    HTTP2_HEADER_METHOD: string;
    HTTP2_HEADER_PATH: string;
    HTTP2_HEADER_STATUS: string;
    HTTP_STATUS_OK: number;
  };
  export function connect(url: string): {
    request(headers: Record<string, string>): StreamLike;
    close(): void;
  };
  export function createSecureServer(options?: unknown): unknown;
  const http2Default: {
    connect: typeof connect;
    constants: typeof constants;
    createSecureServer: typeof createSecureServer;
  };
  export default http2Default;
}

declare module 'node:tls' {
  export class SecureContext {}
  export const rootCertificates: readonly string[];
  export function createSecureContext(options?: unknown): SecureContext;
  export function connect(options?: unknown): {
    on(name: string, listener: (...args: unknown[]) => void): unknown;
  };
  const tlsDefault: {
    SecureContext: typeof SecureContext;
    rootCertificates: typeof rootCertificates;
    createSecureContext: typeof createSecureContext;
    connect: typeof connect;
  };
  export default tlsDefault;
}

declare module 'node:net' {
  export interface Socket {
    on(event: 'data', listener: (chunk: Uint8Array) => void): Socket;
    on(event: 'end', listener: () => void): Socket;
    on(event: 'error', listener: (error: Error) => void): Socket;
    on(event: string, listener: (...args: unknown[]) => void): Socket;
    once(event: string, listener: (...args: unknown[]) => void): Socket;
    write(data: Uint8Array | string): void;
    end(data?: Uint8Array | string): void;
    destroy(): void;
  }
  export class Server {
    listen(port?: number, host?: string, callback?: () => void): Server;
    close(callback?: () => void): void;
    address(): { address: string; family: string; port: number } | null;
    on(event: string, listener: (...args: unknown[]) => void): Server;
  }
  export function connect(port: number, host?: string): Socket;
  export function connect(options: { port: number; host?: string }, callback?: () => void): Socket;
  export function createConnection(port: number, host?: string): Socket;
  export function createConnection(
    options: { port: number; host?: string },
    callback?: () => void,
  ): Socket;
  export function createServer(listener?: (socket: Socket) => void): Server;
  export function isIP(input: string): number;
  const netDefault: {
    connect: typeof connect;
    createConnection: typeof createConnection;
    createServer: typeof createServer;
    isIP: typeof isIP;
  };
  export default netDefault;
}

declare module 'node:dgram' {
  export interface DgramSocket {
    bind(port?: number, address?: string, callback?: () => void): void;
    bind(options: { port?: number; address?: string }, callback?: () => void): void;
    address(): { address: string; family: string; port: number };
    send(
      data: Uint8Array | string,
      port: number,
      address: string,
      callback?: (err: Error | null) => void,
    ): void;
    close(callback?: () => void): void;
    on(
      event: 'message',
      listener: (message: Uint8Array, info: Record<string, unknown>) => void,
    ): DgramSocket;
    on(event: string, listener: (...args: unknown[]) => void): DgramSocket;
    once(
      event: 'message',
      listener: (message: Uint8Array, info: Record<string, unknown>) => void,
    ): DgramSocket;
    once(event: string, listener: (...args: unknown[]) => void): DgramSocket;
  }
  export function createSocket(type: 'udp4' | 'udp6' | { type: 'udp4' | 'udp6' }): DgramSocket;
  const dgramDefault: { createSocket: typeof createSocket };
  export default dgramDefault;
}
