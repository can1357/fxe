/** @jsxImportSource fxe-ui */
import { Pressable, Text, View, useEffect, useState } from 'fxe-ui';
import { connectCdp, type CdpClient } from './cdp_client.ts';
import { ConsolePanel } from './panels/Console.tsx';
import { HeapPanel } from './panels/Heap.tsx';
import { LayoutPanel } from './panels/Layout.tsx';
import { NetworkPanel } from './panels/Network.tsx';
import { PerformancePanel } from './panels/Performance.tsx';
import { ReconcilerPanel } from './panels/Reconciler.tsx';
import { SourcePanel } from './panels/Source.tsx';

type TabId = 'console' | 'reconciler' | 'performance' | 'heap' | 'network' | 'layout' | 'source';

const TABS: ReadonlyArray<{ id: TabId; label: string }> = [
  { id: 'console', label: 'Console' },
  { id: 'reconciler', label: 'Reconciler' },
  { id: 'performance', label: 'Performance' },
  { id: 'heap', label: 'Heap' },
  { id: 'network', label: 'Network' },
  { id: 'layout', label: 'Layout' },
  { id: 'source', label: 'Source' },
];

export function Shell({ url }: { url: string }) {
  const [cdp, setCdp] = useState<CdpClient | null>(null);
  const [tab, setTab] = useState<TabId>('console');
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    let active = true;
    let client: CdpClient | null = null;
    setCdp(null);
    setError(null);
    void connectCdp(url)
      .then((nextClient) => {
        if (!active) {
          nextClient.close();
          return;
        }
        client = nextClient;
        setCdp(nextClient);
      })
      .catch((cause) => {
        if (!active) return;
        const message = cause instanceof Error ? cause.message : String(cause);
        setError(message);
      });
    return () => {
      active = false;
      client?.close();
    };
  }, [url]);

  if (error) {
    return (
      <View style={{ width: '100%', height: '100%', padding: 16, backgroundColor: 0x0f172aff }}>
        <Text style={{ color: 0xff6b6bff, fontSize: 15 }}>Connection failed: {error}</Text>
      </View>
    );
  }

  if (!cdp) {
    return (
      <View style={{ width: '100%', height: '100%', padding: 16, backgroundColor: 0x0f172aff }}>
        <Text style={{ color: 0xe2e8f0ff, fontSize: 15 }}>Connecting to {url}…</Text>
      </View>
    );
  }

  return (
    <View
      style={{
        width: '100%',
        height: '100%',
        flexDirection: 'column',
        backgroundColor: 0x020617ff,
      }}
    >
      <View style={{ flexDirection: 'row', padding: 8, gap: 8, backgroundColor: 0x111827ff }}>
        {TABS.map((item) => (
          <Pressable
            key={item.id}
            onPress={() => setTab(item.id)}
            style={{
              paddingX: 10,
              paddingY: 6,
              backgroundColor: tab === item.id ? 0x1d4ed8ff : 0x1f2937ff,
            }}
          >
            <Text style={{ color: 0xffffffff, fontSize: 14 }}>{item.label}</Text>
          </Pressable>
        ))}
      </View>
      <View style={{ flex: 1, padding: 8 }}>
        {tab === 'console' ? <ConsolePanel cdp={cdp} /> : null}
        {tab === 'reconciler' ? <ReconcilerPanel cdp={cdp} /> : null}
        {tab === 'performance' ? <PerformancePanel cdp={cdp} /> : null}
        {tab === 'heap' ? <HeapPanel cdp={cdp} /> : null}
        {tab === 'network' ? <NetworkPanel cdp={cdp} /> : null}
        {tab === 'layout' ? <LayoutPanel cdp={cdp} /> : null}
        {tab === 'source' ? <SourcePanel cdp={cdp} /> : null}
      </View>
    </View>
  );
}
