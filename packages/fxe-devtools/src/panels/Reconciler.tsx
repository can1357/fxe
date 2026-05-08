/** @jsxImportSource fxe-ui */
import { Button, ScrollView, Text, TextInput, View, useEffect, useMemo, useState } from 'fxe-ui';
import type { CdpClient } from '../cdp_client.ts';

interface ReconcilerSnapshot {
  tree?: unknown[];
}

interface RuntimeEvaluationResult {
  result?: {
    value?: unknown;
  };
}

export function ReconcilerPanel({ cdp }: { cdp: CdpClient }) {
  const [snapshotText, setSnapshotText] = useState('Loading Reconciler.snapshot…');
  const [refreshIntervalText, setRefreshIntervalText] = useState('1000');
  const [paintFlash, setPaintFlash] = useState(false);
  const [paintFlashStatus, setPaintFlashStatus] = useState('Paint flash off');

  const refreshIntervalMs = useMemo(() => {
    const parsed = Number(refreshIntervalText);
    if (!Number.isFinite(parsed) || parsed < 100) return 1000;
    return Math.round(parsed);
  }, [refreshIntervalText]);

  useEffect(() => {
    let alive = true;

    const refresh = (): void => {
      void cdp
        .send<ReconcilerSnapshot>('Reconciler.snapshot')
        .then((snapshot) => {
          if (!alive) return;
          setSnapshotText(JSON.stringify(snapshot, null, 2));
        })
        .catch((error) => {
          if (!alive) return;
          const message = error instanceof Error ? error.message : String(error);
          setSnapshotText(`Reconciler.snapshot failed: ${message}`);
        });
    };

    refresh();
    const timer = setInterval(refresh, refreshIntervalMs);
    return () => {
      alive = false;
      clearInterval(timer);
    };
  }, [cdp, refreshIntervalMs]);

  const togglePaintFlash = async (): Promise<void> => {
    const next = !paintFlash;
    setPaintFlashStatus(next ? 'Enabling paint flash…' : 'Disabling paint flash…');
    const response = (await cdp
      .send('Runtime.evaluate', {
        expression: `(() => { globalThis.__fxe_devtools?.setPaintFlash(${next ? 'true' : 'false'}); return Boolean(globalThis.__fxe_devtools); })()`,
        returnByValue: true,
      })
      .catch(() => null)) as RuntimeEvaluationResult | null;
    if (response?.result?.value) {
      setPaintFlash(next);
      setPaintFlashStatus(next ? 'Paint flash on' : 'Paint flash off');
      return;
    }
    setPaintFlashStatus('Paint flash toggle failed');
  };

  return (
    <View style={{ width: '100%', height: '100%', gap: 8 }}>
      <View style={{ flexDirection: 'row', gap: 8, paddingTop: 4, paddingBottom: 4 }}>
        <View style={{ width: 180, gap: 4 }}>
          <Text style={{ color: 0x94a3b8ff, fontSize: 12 }}>Refresh Interval (ms)</Text>
          <TextInput
            value={refreshIntervalText}
            onChange={setRefreshIntervalText}
            style={{ width: '100%' }}
          />
        </View>
        <Button
          title={paintFlash ? 'Paint Flash: On' : 'Paint Flash: Off'}
          onPress={() => void togglePaintFlash()}
        />
      </View>
      <Text style={{ color: 0x94a3b8ff, fontSize: 13 }}>
        {paintFlashStatus} · polling every {refreshIntervalMs}ms
      </Text>
      <ScrollView style={{ width: '100%', height: '100%' }}>
        <Text style={{ color: 0xe2e8f0ff, fontSize: 13 }}>{snapshotText}</Text>
      </ScrollView>
    </View>
  );
}
