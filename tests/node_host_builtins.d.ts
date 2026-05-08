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
    request(
      headers: Record<string, string>,
      options?: { signal?: AbortSignal; timeout?: number; timeoutMs?: number },
    ): StreamLike;
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
