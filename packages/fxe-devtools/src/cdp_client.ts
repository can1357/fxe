export interface CdpClient {
  send<T = unknown>(method: string, params?: unknown): Promise<T>;
  on(event: string, cb: (params: unknown) => void): () => void;
  close(): void;
}

interface PendingRequest {
  res: (value: unknown) => void;
  rej: (error: unknown) => void;
}

export function connectCdp(url: string): Promise<CdpClient> {
  const { promise, resolve, reject } = Promise.withResolvers<CdpClient>();
  const ws = new WebSocket(url);
  let settled = false;
  let nextId = 1;
  const pending = new Map<number, PendingRequest>();
  const listeners = new Map<string, Set<(params: unknown) => void>>();

  const failAll = (error: unknown): void => {
    for (const entry of pending.values()) entry.rej(error);
    pending.clear();
  };

  ws.onopen = () => {
    if (settled) return;
    settled = true;
    resolve({
      send<T = unknown>(method: string, params?: unknown) {
        if (ws.readyState !== WebSocket.OPEN) {
          return Promise.reject(new Error(`CDP socket is not open for ${method}`));
        }
        const id = nextId++;
        const req = Promise.withResolvers<T>();
        pending.set(id, { res: req.resolve as (value: unknown) => void, rej: req.reject });
        ws.send(JSON.stringify({ id, method, params: params ?? {} }));
        return req.promise;
      },
      on(event, cb) {
        let set = listeners.get(event);
        if (!set) {
          set = new Set();
          listeners.set(event, set);
        }
        set.add(cb);
        return () => {
          set.delete(cb);
          if (set.size === 0) listeners.delete(event);
        };
      },
      close() {
        ws.close();
      },
    });
  };

  ws.onerror = (event) => {
    const error = new Error(event.message || 'CDP WebSocket error');
    if (!settled) {
      settled = true;
      reject(error);
    }
    failAll(error);
  };

  ws.onclose = (event) => {
    const error = new Error(
      `CDP WebSocket closed (${event.code}${event.reason ? `: ${event.reason}` : ''})`,
    );
    if (!settled) {
      settled = true;
      reject(error);
    }
    failAll(error);
  };

  ws.onmessage = (event) => {
    const data = typeof event.data === 'string' ? event.data : String(event.data);
    const msg = JSON.parse(data) as {
      id?: number;
      method?: string;
      params?: unknown;
      result?: unknown;
      error?: unknown;
    };
    if (typeof msg.id === 'number') {
      const entry = pending.get(msg.id);
      if (!entry) return;
      pending.delete(msg.id);
      if (msg.error !== undefined) entry.rej(msg.error);
      else entry.res(msg.result);
      return;
    }
    if (!msg.method) return;
    const set = listeners.get(msg.method);
    if (!set) return;
    for (const cb of set) cb(msg.params);
  };

  return promise;
}
