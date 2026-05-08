/** @jsxImportSource fxe-ui */
import { ScrollView, Text, useEffect, useState, View } from 'fxe-ui';
import type { CdpClient } from '../cdp_client.ts';

type HttpEntry = {
  kind: 'request';
  id: string;
  url: string;
  method: string;
  type: string;
  status?: number;
  statusText?: string;
  mimeType?: string;
  encodedLength?: number;
  error?: string;
  startedAt: number;
  finishedAt?: number;
};

type WsEntry = {
  kind: 'websocket';
  id: string;
  url: string;
  status?: number;
  statusText?: string;
  closed?: boolean;
  framesSent: number;
  framesReceived: number;
  startedAt: number;
};

type NetworkEntry = HttpEntry | WsEntry;

type RequestWillBeSentEvent = {
  requestId?: string;
  timestamp?: number;
  request?: {
    url?: string;
    method?: string;
  };
  type?: string;
};

type ResponseReceivedEvent = {
  requestId?: string;
  timestamp?: number;
  type?: string;
  response?: {
    url?: string;
    status?: number;
    statusText?: string;
    mimeType?: string;
    encodedDataLength?: number;
  };
};

type LoadingFinishedEvent = {
  requestId?: string;
  timestamp?: number;
  encodedDataLength?: number;
};

type LoadingFailedEvent = {
  requestId?: string;
  timestamp?: number;
  type?: string;
  errorText?: string;
};

type WebSocketCreatedEvent = {
  requestId?: string;
  url?: string;
};

type WebSocketHandshakeResponseReceivedEvent = {
  requestId?: string;
  timestamp?: number;
  response?: {
    status?: number;
    statusText?: string;
  };
};

type WebSocketFrameEvent = {
  requestId?: string;
  timestamp?: number;
};

type WebSocketClosedEvent = {
  requestId?: string;
  timestamp?: number;
};

const CONTENT_COLOR = 0xddddddff;
const MUTED_COLOR = 0x94a3b8ff;
const ERROR_COLOR = 0xf87171ff;
const MAX_ENTRIES = 500;
const FALLBACK_TIMESTAMP = Number.POSITIVE_INFINITY;

function capAndSort(entries: NetworkEntry[]): NetworkEntry[] {
  const next = [...entries].sort((a, b) => a.startedAt - b.startedAt);
  return next.length > MAX_ENTRIES ? next.slice(next.length - MAX_ENTRIES) : next;
}

function defaultHttpEntry(id: string, startedAt: number): HttpEntry {
  return {
    kind: 'request',
    id,
    url: '',
    method: 'GET',
    type: 'Fetch',
    startedAt,
  };
}

function defaultWsEntry(id: string, startedAt: number): WsEntry {
  return {
    kind: 'websocket',
    id,
    url: '',
    framesSent: 0,
    framesReceived: 0,
    startedAt,
  };
}

function upsertRequest(
  entries: NetworkEntry[],
  id: string,
  startedAt: number,
  update: (entry: HttpEntry) => HttpEntry,
): NetworkEntry[] {
  const index = entries.findIndex((entry) => entry.id === id);
  const current =
    index >= 0 && entries[index]?.kind === 'request'
      ? (entries[index] as HttpEntry)
      : defaultHttpEntry(id, startedAt);
  const nextEntry = update(current);
  const next =
    index >= 0
      ? [...entries.slice(0, index), nextEntry, ...entries.slice(index + 1)]
      : [...entries, nextEntry];
  return capAndSort(next);
}

function upsertWebSocket(
  entries: NetworkEntry[],
  id: string,
  startedAt: number,
  update: (entry: WsEntry) => WsEntry,
): NetworkEntry[] {
  const index = entries.findIndex((entry) => entry.id === id);
  const current =
    index >= 0 && entries[index]?.kind === 'websocket'
      ? (entries[index] as WsEntry)
      : defaultWsEntry(id, startedAt);
  const nextEntry = update(current);
  const next =
    index >= 0
      ? [...entries.slice(0, index), nextEntry, ...entries.slice(index + 1)]
      : [...entries, nextEntry];
  return capAndSort(next);
}

function requestSummary(entry: HttpEntry): string {
  if (entry.error) return `[${entry.method}] ${entry.url} -> ⚠ ${entry.error}`;
  if (entry.status === undefined) return `[${entry.method}] ${entry.url} -> pending`;
  const details: string[] = [];
  if (entry.mimeType) details.push(entry.mimeType);
  if (entry.encodedLength !== undefined) details.push(`${entry.encodedLength} bytes`);
  const suffix = details.length > 0 ? ` (${details.join(', ')})` : '';
  return `[${entry.method}] ${entry.url} -> ${entry.status} ${entry.statusText ?? ''}${suffix}`;
}

function websocketSummary(entry: WsEntry): string {
  if (entry.closed) return `[WS-closed] ${entry.url}`;
  const status =
    entry.status !== undefined ? `${entry.status} ${entry.statusText ?? ''}` : 'pending';
  return `[WS] ${entry.url} -> ${status} (sent: ${entry.framesSent}, recv: ${entry.framesReceived})`;
}

function entryColor(entry: NetworkEntry): number {
  if (entry.kind === 'request') {
    if (entry.error || (entry.status !== undefined && entry.status >= 400)) return ERROR_COLOR;
    return CONTENT_COLOR;
  }
  if (entry.status !== undefined && entry.status >= 400) return ERROR_COLOR;
  return entry.closed ? MUTED_COLOR : CONTENT_COLOR;
}

export function NetworkPanel({ cdp }: { cdp: CdpClient }) {
  const [entries, setEntries] = useState<NetworkEntry[]>([]);

  useEffect(() => {
    void cdp.send('Network.enable').catch(() => {});

    const offHandlers = [
      cdp.on('Network.requestWillBeSent', (params) => {
        const event = params as RequestWillBeSentEvent;
        const requestId = event.requestId ?? '';
        if (!requestId) return;
        const timestamp = event.timestamp ?? FALLBACK_TIMESTAMP;
        setEntries((prev) =>
          upsertRequest(prev, requestId, timestamp, (entry) => ({
            ...entry,
            url: event.request?.url ?? entry.url,
            method: event.request?.method ?? entry.method,
            type: event.type ?? entry.type,
            startedAt: Math.min(entry.startedAt, timestamp),
          })),
        );
      }),
      cdp.on('Network.responseReceived', (params) => {
        const event = params as ResponseReceivedEvent;
        const requestId = event.requestId ?? '';
        if (!requestId) return;
        const timestamp = event.timestamp ?? FALLBACK_TIMESTAMP;
        setEntries((prev) =>
          upsertRequest(prev, requestId, timestamp, (entry) => ({
            ...entry,
            url: event.response?.url ?? entry.url,
            type: event.type ?? entry.type,
            status: event.response?.status ?? entry.status,
            statusText: event.response?.statusText ?? entry.statusText,
            mimeType: event.response?.mimeType ?? entry.mimeType,
            encodedLength: event.response?.encodedDataLength ?? entry.encodedLength,
            startedAt: Math.min(entry.startedAt, timestamp),
          })),
        );
      }),
      cdp.on('Network.loadingFinished', (params) => {
        const event = params as LoadingFinishedEvent;
        const requestId = event.requestId ?? '';
        if (!requestId) return;
        const timestamp = event.timestamp ?? FALLBACK_TIMESTAMP;
        setEntries((prev) =>
          upsertRequest(prev, requestId, timestamp, (entry) => ({
            ...entry,
            encodedLength: event.encodedDataLength ?? entry.encodedLength,
            startedAt: Math.min(entry.startedAt, timestamp),
            finishedAt: timestamp,
          })),
        );
      }),
      cdp.on('Network.loadingFailed', (params) => {
        const event = params as LoadingFailedEvent;
        const requestId = event.requestId ?? '';
        if (!requestId) return;
        const timestamp = event.timestamp ?? FALLBACK_TIMESTAMP;
        setEntries((prev) =>
          upsertRequest(prev, requestId, timestamp, (entry) => ({
            ...entry,
            type: event.type ?? entry.type,
            error: event.errorText ?? entry.error,
            startedAt: Math.min(entry.startedAt, timestamp),
            finishedAt: timestamp,
          })),
        );
      }),
      cdp.on('Network.webSocketCreated', (params) => {
        const event = params as WebSocketCreatedEvent;
        const requestId = event.requestId ?? '';
        if (!requestId) return;
        setEntries((prev) =>
          upsertWebSocket(prev, requestId, FALLBACK_TIMESTAMP, (entry) => ({
            ...entry,
            url: event.url ?? entry.url,
          })),
        );
      }),
      cdp.on('Network.webSocketWillSendHandshakeRequest', (params) => {
        const event = params as RequestWillBeSentEvent;
        const requestId = event.requestId ?? '';
        if (!requestId) return;
        const timestamp = event.timestamp ?? FALLBACK_TIMESTAMP;
        setEntries((prev) =>
          upsertWebSocket(prev, requestId, timestamp, (entry) => ({
            ...entry,
            url: event.request?.url ?? entry.url,
            startedAt: Math.min(entry.startedAt, timestamp),
          })),
        );
      }),
      cdp.on('Network.webSocketHandshakeResponseReceived', (params) => {
        const event = params as WebSocketHandshakeResponseReceivedEvent;
        const requestId = event.requestId ?? '';
        if (!requestId) return;
        const timestamp = event.timestamp ?? FALLBACK_TIMESTAMP;
        setEntries((prev) =>
          upsertWebSocket(prev, requestId, timestamp, (entry) => ({
            ...entry,
            status: event.response?.status ?? entry.status,
            statusText: event.response?.statusText ?? entry.statusText,
            startedAt: Math.min(entry.startedAt, timestamp),
          })),
        );
      }),
      cdp.on('Network.webSocketFrameSent', (params) => {
        const event = params as WebSocketFrameEvent;
        const requestId = event.requestId ?? '';
        if (!requestId) return;
        const timestamp = event.timestamp ?? FALLBACK_TIMESTAMP;
        setEntries((prev) =>
          upsertWebSocket(prev, requestId, timestamp, (entry) => ({
            ...entry,
            framesSent: entry.framesSent + 1,
            startedAt: Math.min(entry.startedAt, timestamp),
          })),
        );
      }),
      cdp.on('Network.webSocketFrameReceived', (params) => {
        const event = params as WebSocketFrameEvent;
        const requestId = event.requestId ?? '';
        if (!requestId) return;
        const timestamp = event.timestamp ?? FALLBACK_TIMESTAMP;
        setEntries((prev) =>
          upsertWebSocket(prev, requestId, timestamp, (entry) => ({
            ...entry,
            framesReceived: entry.framesReceived + 1,
            startedAt: Math.min(entry.startedAt, timestamp),
          })),
        );
      }),
      cdp.on('Network.webSocketClosed', (params) => {
        const event = params as WebSocketClosedEvent;
        const requestId = event.requestId ?? '';
        if (!requestId) return;
        const timestamp = event.timestamp ?? FALLBACK_TIMESTAMP;
        setEntries((prev) =>
          upsertWebSocket(prev, requestId, timestamp, (entry) => ({
            ...entry,
            closed: true,
            startedAt: Math.min(entry.startedAt, timestamp),
          })),
        );
      }),
    ];

    return () => {
      void cdp.send('Network.disable').catch(() => {});
      for (const off of offHandlers) off();
    };
  }, [cdp]);

  return (
    <ScrollView style={{ width: '100%', height: '100%' }}>
      {entries.length === 0 ? (
        <View style={{ padding: 8 }}>
          <Text style={{ color: MUTED_COLOR, fontSize: 14 }}>
            Waiting for Network.* events… (call Network.enable)
          </Text>
        </View>
      ) : (
        entries.map((entry) => (
          <Text
            key={`${entry.kind}:${entry.id}`}
            style={{ color: entryColor(entry), fontSize: 13 }}
          >
            {entry.kind === 'request' ? requestSummary(entry) : websocketSummary(entry)}
          </Text>
        ))
      )}
    </ScrollView>
  );
}
