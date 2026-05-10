/** @jsxImportSource fxe-ui */
import { Pressable, ScrollView, Text, useEffect, useMemo, useRef, useState, View } from 'fxe-ui';
import type { CdpClient } from '../cdp_client.ts';

interface MemoTraceSlot {
  total: number;
  dirty: number;
  layout: number;
  noCache: number;
  noLastProps: number;
  epoch: number;
  propsDiff: number;
  hit: number;
}

interface MemoTracePropsDump {
  last: unknown;
  next: unknown;
  lastKeys: string[];
  nextKeys: string[];
}

interface MemoTraceSnapshot {
  totals: MemoTraceSlot;
  byName: Record<string, MemoTraceSlot>;
  propsDump: Record<string, MemoTracePropsDump>;
}

interface RuntimeEvaluateResponse {
  result?: {
    value?: unknown;
  };
}

interface MemoTraceRow {
  name: string;
  slot: MemoTraceSlot;
  rebuilds: number;
  dominantReason: string;
  diffKeys: string[];
}

type TimerHandle = ReturnType<typeof setTimeout>;

const POLL_INTERVAL_MS = 500;
const TRACK_COLOR = 0x0f172aff;
const BORDER_COLOR = 0x1f2937ff;
const TEXT_COLOR = 0xe2e8f0ff;
const MUTED_COLOR = 0x94a3b8ff;
const ACTIVE_BUTTON_COLOR = 0x1d4ed8ff;
const INACTIVE_BUTTON_COLOR = 0x1f2937ff;
const SUCCESS_COLOR = 0x22c55eff;
const WARN_COLOR = 0xf59e0bff;

const REBUILD_REASONS = [
  ['dirty', 'dirty'],
  ['layout', 'layout'],
  ['noCache', 'noCache'],
  ['noLastProps', 'noLastProps'],
  ['epoch', 'epoch'],
  ['propsDiff', 'propsDiff'],
] as const satisfies ReadonlyArray<readonly [keyof MemoTraceSlot, string]>;

async function evaluate<T>(cdp: CdpClient, expression: string): Promise<T | undefined> {
  const response = await cdp
    .send<RuntimeEvaluateResponse>('Runtime.evaluate', { expression, returnByValue: true })
    .catch(() => null);
  return response?.result?.value as T | undefined;
}

function rebuildCount(slot: MemoTraceSlot): number {
  return slot.dirty + slot.layout + slot.noCache + slot.noLastProps + slot.epoch + slot.propsDiff;
}

function dominantReason(slot: MemoTraceSlot): string {
  let winner = 'none';
  let max = 0;
  for (const [key, label] of REBUILD_REASONS) {
    const value = slot[key];
    if (value > max) {
      max = value;
      winner = label;
    }
  }
  return winner;
}

function summarizeTotals(slot: MemoTraceSlot): string {
  const rebuilds = rebuildCount(slot);
  return `samples ${slot.total} · hits ${slot.hit} · rebuilds ${rebuilds} · dirty ${slot.dirty} · layout ${slot.layout} · noCache ${slot.noCache} · noLastProps ${slot.noLastProps} · epoch ${slot.epoch} · propsDiff ${slot.propsDiff}`;
}

function diffKeysForDump(dump: MemoTracePropsDump | undefined): string[] {
  if (!dump) return [];
  const seen = new Set<string>();
  const keys: string[] = [];
  for (const key of dump.lastKeys) {
    if (seen.has(key)) continue;
    seen.add(key);
    keys.push(key);
  }
  for (const key of dump.nextKeys) {
    if (seen.has(key)) continue;
    seen.add(key);
    keys.push(key);
  }
  return keys;
}

export function MemoPanel({ cdp }: { cdp: CdpClient }) {
  const [enabled, setEnabled] = useState(true);
  const [available, setAvailable] = useState(false);
  const [snapshot, setSnapshot] = useState<MemoTraceSnapshot | null>(null);
  const enabledRef = useRef(true);

  useEffect(() => {
    let active = true;
    let timer: TimerHandle | null = null;

    const syncEnabled = async (next: boolean): Promise<boolean> => {
      const result = await evaluate<boolean>(
        cdp,
        `(() => {
          const devtools = globalThis.__fxe_devtools;
          if (!devtools) return false;
          devtools.setMemoTrace(${next ? 'true' : 'false'});
          return true;
        })()`,
      );
      if (!active) return false;
      if (next) {
        enabledRef.current = result === true;
        setEnabled(result === true);
      }
      setAvailable(result === true);
      if (result !== true) setSnapshot(null);
      return result === true;
    };

    const poll = async (): Promise<void> => {
      const result = await evaluate<MemoTraceSnapshot | null>(
        cdp,
        'globalThis.__fxe_devtools?.memoTraceSnapshot() ?? null',
      );
      if (!active) return;
      const nextSnapshot = result ?? null;
      setSnapshot(nextSnapshot);
      if (enabledRef.current) setAvailable(nextSnapshot !== null);
      timer = setTimeout(() => {
        void poll();
      }, POLL_INTERVAL_MS);
    };

    void syncEnabled(true).then((ok) => {
      if (!active || !ok) return;
      void poll();
    });

    return () => {
      active = false;
      if (timer !== null) clearTimeout(timer);
      void evaluate(
        cdp,
        `(() => {
          const devtools = globalThis.__fxe_devtools;
          if (!devtools) return false;
          devtools.setMemoTrace(false);
          return true;
        })()`,
      );
    };
  }, [cdp]);

  const setTracingEnabled = async (next: boolean): Promise<void> => {
    const result = await evaluate<boolean>(
      cdp,
      `(() => {
        const devtools = globalThis.__fxe_devtools;
        if (!devtools) return false;
        devtools.setMemoTrace(${next ? 'true' : 'false'});
        return true;
      })()`,
    );
    const ok = result === true;
    if (!ok) {
      enabledRef.current = false;
      setEnabled(false);
      setAvailable(false);
      setSnapshot(null);
      return;
    }
    enabledRef.current = next;
    setEnabled(next);
    if (!next) {
      setAvailable(true);
      setSnapshot(null);
      return;
    }
    const nextSnapshot = await evaluate<MemoTraceSnapshot | null>(
      cdp,
      'globalThis.__fxe_devtools?.memoTraceSnapshot() ?? null',
    );
    setSnapshot(nextSnapshot ?? null);
    setAvailable((nextSnapshot ?? null) !== null);
  };

  const reset = async (): Promise<void> => {
    const result = await evaluate<boolean>(
      cdp,
      `(() => {
        const devtools = globalThis.__fxe_devtools;
        if (!devtools) return false;
        devtools.resetMemoTrace();
        return true;
      })()`,
    );
    if (result !== true) {
      setAvailable(false);
      setSnapshot(null);
      return;
    }
    const nextSnapshot = await evaluate<MemoTraceSnapshot | null>(
      cdp,
      'globalThis.__fxe_devtools?.memoTraceSnapshot() ?? null',
    );
    setSnapshot(nextSnapshot ?? null);
    setAvailable((nextSnapshot ?? null) !== null || !enabledRef.current);
  };

  const rows = useMemo(() => {
    if (!snapshot) return [];
    return Object.entries(snapshot.byName)
      .map(([name, slot]) => ({
        name,
        slot,
        rebuilds: rebuildCount(slot),
        dominantReason: dominantReason(slot),
        diffKeys: diffKeysForDump(snapshot.propsDump[name]),
      }))
      .sort((a, b) => {
        const aScore = a.rebuilds + a.slot.hit;
        const bScore = b.rebuilds + b.slot.hit;
        if (bScore !== aScore) return bScore - aScore;
        return a.name.localeCompare(b.name);
      });
  }, [snapshot]);

  return (
    <View style={{ width: '100%', height: '100%', padding: 8, gap: 8 }}>
      <View style={{ flexDirection: 'row', gap: 8 }}>
        <Pressable
          onPress={() => void setTracingEnabled(true)}
          style={{
            paddingX: 10,
            paddingY: 6,
            backgroundColor: enabled ? ACTIVE_BUTTON_COLOR : INACTIVE_BUTTON_COLOR,
          }}
        >
          <Text style={{ color: enabled ? TEXT_COLOR : MUTED_COLOR, fontSize: 13 }}>Enable</Text>
        </Pressable>
        <Pressable
          onPress={() => void setTracingEnabled(false)}
          style={{
            paddingX: 10,
            paddingY: 6,
            backgroundColor: enabled ? INACTIVE_BUTTON_COLOR : ACTIVE_BUTTON_COLOR,
          }}
        >
          <Text style={{ color: enabled ? MUTED_COLOR : TEXT_COLOR, fontSize: 13 }}>Disable</Text>
        </Pressable>
        <Pressable
          onPress={() => void reset()}
          style={{ paddingX: 10, paddingY: 6, backgroundColor: INACTIVE_BUTTON_COLOR }}
        >
          <Text style={{ color: TEXT_COLOR, fontSize: 13 }}>Reset</Text>
        </Pressable>
      </View>
      <Text style={{ color: !enabled || available ? MUTED_COLOR : WARN_COLOR, fontSize: 13 }}>
        {!enabled
          ? 'Tracing disabled; enable to collect memo decisions.'
          : available
            ? `Polling every ${POLL_INTERVAL_MS}ms`
            : 'Memo trace is unavailable. This app may not have imported fxe-ui or the devtools surface is missing.'}
      </Text>
      <Text style={{ color: TEXT_COLOR, fontSize: 13 }}>
        Totals: {snapshot ? summarizeTotals(snapshot.totals) : 'no snapshot'}
      </Text>
      <View
        style={{
          flexDirection: 'row',
          paddingX: 8,
          paddingY: 6,
          backgroundColor: TRACK_COLOR,
          borderWidth: 1,
          borderColor: BORDER_COLOR,
        }}
      >
        <Text style={{ color: MUTED_COLOR, fontSize: 12, flex: 3 }}>Component</Text>
        <Text style={{ color: MUTED_COLOR, fontSize: 12, flex: 1 }}>Hits</Text>
        <Text style={{ color: MUTED_COLOR, fontSize: 12, flex: 1 }}>Rebuilds</Text>
        <Text style={{ color: MUTED_COLOR, fontSize: 12, flex: 2 }}>Dominant reason</Text>
      </View>
      <ScrollView style={{ flex: 1, width: '100%' }}>
        <View style={{ gap: 6, paddingBottom: 8 }}>
          {!enabled ? (
            <Text style={{ color: MUTED_COLOR, fontSize: 13 }}>Tracing is disabled.</Text>
          ) : !available ? (
            <Text style={{ color: MUTED_COLOR, fontSize: 13 }}>
              memoTraceSnapshot() returned null, so there is nothing to display.
            </Text>
          ) : rows.length === 0 ? (
            <Text style={{ color: MUTED_COLOR, fontSize: 13 }}>
              Waiting for memo trace samples…
            </Text>
          ) : (
            rows.map((row) => (
              <View
                key={row.name}
                style={{
                  gap: 4,
                  paddingX: 8,
                  paddingY: 6,
                  backgroundColor: TRACK_COLOR,
                  borderWidth: 1,
                  borderColor: BORDER_COLOR,
                }}
              >
                <View style={{ flexDirection: 'row' }}>
                  <Text style={{ color: TEXT_COLOR, fontSize: 13, flex: 3 }}>{row.name}</Text>
                  <Text style={{ color: SUCCESS_COLOR, fontSize: 13, flex: 1 }}>
                    {String(row.slot.hit)}
                  </Text>
                  <Text style={{ color: TEXT_COLOR, fontSize: 13, flex: 1 }}>
                    {String(row.rebuilds)}
                  </Text>
                  <Text
                    style={{
                      color: row.dominantReason === 'none' ? MUTED_COLOR : WARN_COLOR,
                      fontSize: 13,
                      flex: 2,
                    }}
                  >
                    {row.dominantReason}
                  </Text>
                </View>
                {row.diffKeys.length > 0 ? (
                  <Text style={{ color: MUTED_COLOR, fontSize: 12 }}>
                    props keys: {row.diffKeys.join(', ')}
                  </Text>
                ) : null}
              </View>
            ))
          )}
        </View>
      </ScrollView>
    </View>
  );
}
