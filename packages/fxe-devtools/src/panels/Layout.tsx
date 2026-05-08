/** @jsxImportSource fxe-ui */
import { Button, ScrollView, Text, useState, View } from 'fxe-ui';
import type { CdpClient } from '../cdp_client.ts';

interface RuntimeEvaluationResult {
  result?: {
    value?: unknown;
  };
}

async function evaluate<T>(cdp: CdpClient, expression: string): Promise<T | null> {
  const response = (await cdp
    .send('Runtime.evaluate', { expression, returnByValue: true })
    .catch(() => null)) as RuntimeEvaluationResult | null;
  return (response?.result?.value as T | undefined) ?? null;
}

export function LayoutPanel({ cdp }: { cdp: CdpClient }) {
  const [entries, setEntries] = useState<unknown[]>([]);
  const [status, setStatus] = useState('Idle');

  const enable = async (): Promise<void> => {
    setStatus('Enabling layout trace…');
    const enabled = await evaluate<boolean>(
      cdp,
      '(() => { const g = globalThis; const s = g.__fxeLayoutTrace ?? (g.__fxeLayoutTrace = { enabled: false, buffer: [], limit: 1000 }); s.enabled = true; s.buffer.length = 0; s.limit = 200; return true; })()',
    );
    setStatus(enabled ? 'Layout trace enabled' : 'Layout trace enable failed');
  };

  const drain = async (): Promise<void> => {
    setStatus('Draining layout trace…');
    const nextEntries = await evaluate<unknown[]>(
      cdp,
      '(() => { const s = globalThis.__fxeLayoutTrace; if (!s) return []; const out = Array.isArray(s.buffer) ? s.buffer.slice() : []; if (Array.isArray(s.buffer)) s.buffer.length = 0; return out; })()',
    );
    setEntries(nextEntries ?? []);
    setStatus(`Loaded ${nextEntries?.length ?? 0} layout trace entries`);
  };

  const disable = async (): Promise<void> => {
    setStatus('Disabling layout trace…');
    const disabled = await evaluate<boolean>(
      cdp,
      '(() => { const s = globalThis.__fxeLayoutTrace; if (!s) return false; s.enabled = false; return true; })()',
    );
    setStatus(disabled ? 'Layout trace disabled' : 'Layout trace disable failed');
  };

  return (
    <View style={{ flexDirection: 'column', gap: 8, padding: 8, width: '100%', height: '100%' }}>
      <View style={{ flexDirection: 'row', gap: 8 }}>
        <Button title="Enable" onPress={() => void enable()} />
        <Button title="Drain" onPress={() => void drain()} />
        <Button title="Disable" onPress={() => void disable()} />
      </View>
      <Text style={{ color: 0x94a3b8ff, fontSize: 13 }}>{status}</Text>
      <ScrollView style={{ flex: 1 }}>
        {entries.length === 0 ? (
          <Text style={{ color: 0x94a3b8ff, fontSize: 13 }}>No layout trace entries yet.</Text>
        ) : (
          entries.map((entry, index) => (
            <Text key={String(index)} style={{ fontSize: 12, color: 0xccccccff }}>
              {JSON.stringify(entry)}
            </Text>
          ))
        )}
      </ScrollView>
    </View>
  );
}
