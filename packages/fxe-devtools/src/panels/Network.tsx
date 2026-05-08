/** @jsxImportSource fxe-ui */
import { ScrollView, Text, View, useEffect, useState } from 'fxe-ui';
import type { CdpClient } from '../cdp_client.ts';

interface RequestEntry {
  id: string;
  url: string;
  method: string;
  status?: number;
}

export function NetworkPanel({ cdp }: { cdp: CdpClient }) {
  const [requests, setRequests] = useState<RequestEntry[]>([]);

  useEffect(() => {
    void cdp.send('Fetch.subscribe').catch(() => {});
    const offRequest = cdp.on('Fetch.requestWillBeSent', (params) => {
      const event = params as { requestId?: string; request?: { url?: string; method?: string } };
      const requestId = event.requestId ?? '';
      setRequests((prev) => {
        const next = prev.filter((entry) => entry.id !== requestId);
        next.push({
          id: requestId,
          url: event.request?.url ?? '',
          method: event.request?.method ?? 'GET',
        });
        return next.slice(-200);
      });
    });
    const offResponse = cdp.on('Fetch.responseReceived', (params) => {
      const event = params as { requestId?: string; response?: { status?: number } };
      setRequests((prev) =>
        prev.map((entry) =>
          entry.id === event.requestId ? { ...entry, status: event.response?.status } : entry,
        ),
      );
    });
    return () => {
      offRequest();
      offResponse();
    };
  }, [cdp]);

  return (
    <ScrollView style={{ width: '100%', height: '100%' }}>
      {requests.length === 0 ? (
        <View style={{ padding: 8 }}>
          <Text style={{ color: 0x94a3b8ff, fontSize: 14 }}>
            Waiting for Fetch.requestWillBeSent…
          </Text>
        </View>
      ) : (
        requests.map((request) => (
          <Text key={request.id} style={{ color: 0xddddddff, fontSize: 13 }}>
            [{request.method}] {request.url} {'->'} {request.status ?? 'pending'}
          </Text>
        ))
      )}
    </ScrollView>
  );
}
