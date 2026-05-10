/** @jsxImportSource fxe-ui */
import { Button, Pressable, ScrollView, Text, useEffect, useMemo, useState, View } from 'fxe-ui';
import type { CdpClient } from '../cdp_client.ts';

interface RuntimeEvaluateResponse {
  result?: {
    value?: unknown;
  };
}

interface LayoutResult {
  x: number;
  y: number;
  width: number;
  height: number;
  children: LayoutResult[];
}

type LayoutLength = number | `${number}%` | 'auto' | undefined;

interface LayoutTraceEntry {
  component: string;
  rect: LayoutResult;
  hasParentLayout: boolean;
  styleWidth?: LayoutLength;
  styleHeight?: LayoutLength;
  tag?: string;
}

interface LayoutEntryRecord {
  id: string;
  index: number;
  entry: LayoutTraceEntry;
}

interface LayoutSnapshot {
  available: boolean;
  entries: LayoutTraceEntry[];
}

interface LayoutGroup {
  component: string;
  entries: LayoutEntryRecord[];
  totalArea: number;
  maxWidth: number;
  maxHeight: number;
}

type Timer = ReturnType<typeof setTimeout>;

const POLL_INTERVAL_MS = 750;
const TRACE_LIMIT = 200;
const TRACK_COLOR = 0x0f172aff;
const BORDER_COLOR = 0x1f2937ff;
const TEXT_COLOR = 0xe2e8f0ff;
const MUTED_COLOR = 0x94a3b8ff;
const ACCENT_COLOR = 0x3b82f6ff;
const POSITIVE_COLOR = 0x22c55eff;
const WARNING_COLOR = 0xf59e0bff;

async function evaluate<T>(cdp: CdpClient, expression: string): Promise<T | undefined> {
  const response = await cdp
    .send<RuntimeEvaluateResponse>('Runtime.evaluate', { expression, returnByValue: true })
    .catch(() => null);
  return response?.result?.value as T | undefined;
}

function formatNumber(value: number): string {
  if (Number.isInteger(value)) return String(value);
  return value.toFixed(value >= 10 ? 1 : 2);
}

function formatRect(rect: LayoutResult): string {
  return `${formatNumber(rect.x)},${formatNumber(rect.y)} ${formatNumber(rect.width)}×${formatNumber(rect.height)}`;
}

function formatLength(value: LayoutLength): string {
  if (value === undefined) return '—';
  if (typeof value === 'number') return `${formatNumber(value)}px`;
  return value;
}

function formatTimestampLabel(timestamp: number | null): string {
  if (timestamp === null) return 'never';
  const date = new Date(timestamp);
  return `${date.toLocaleTimeString()}.${String(date.getMilliseconds()).padStart(3, '0')}`;
}

export function LayoutPanel({ cdp }: { cdp: CdpClient }) {
  const [entries, setEntries] = useState<LayoutEntryRecord[]>([]);
  const [enabled, setEnabled] = useState(false);
  const [available, setAvailable] = useState(true);
  const [status, setStatus] = useState('Initializing layout trace…');
  const [lastDrainAt, setLastDrainAt] = useState<number | null>(null);
  const [expanded, setExpanded] = useState<Record<string, boolean>>({});

  const groups = useMemo(() => {
    const grouped = new Map<string, LayoutGroup>();
    for (const record of entries) {
      const component = record.entry.component;
      let group = grouped.get(component);
      if (!group) {
        group = {
          component,
          entries: [],
          totalArea: 0,
          maxWidth: 0,
          maxHeight: 0,
        };
        grouped.set(component, group);
      }
      group.entries.push(record);
      group.totalArea += record.entry.rect.width * record.entry.rect.height;
      group.maxWidth = Math.max(group.maxWidth, record.entry.rect.width);
      group.maxHeight = Math.max(group.maxHeight, record.entry.rect.height);
    }
    return Array.from(grouped.values());
  }, [entries]);

  const enable = async () => {
    setStatus('Enabling layout trace…');
    const result = await evaluate<boolean>(
      cdp,
      `(() => {
        const trace = globalThis.__fxeLayoutTrace;
        if (!trace) return false;
        trace.enabled = true;
        trace.buffer.length = 0;
        trace.limit = ${TRACE_LIMIT};
        return true;
      })()`,
    );
    const nextEnabled = result === true;
    setAvailable(nextEnabled);
    setEnabled(nextEnabled);
    if (nextEnabled) {
      setEntries([]);
      setExpanded({});
      setLastDrainAt(null);
    }
    setStatus(nextEnabled ? 'Layout trace enabled' : 'Layout trace unavailable');
  };

  const drain = async () => {
    setStatus('Draining layout trace…');
    const snapshot = await evaluate<LayoutSnapshot>(
      cdp,
      `(() => {
        const trace = globalThis.__fxeLayoutTrace;
        if (!trace) return { available: false, entries: [] };
        const entries = Array.isArray(trace.buffer) ? trace.buffer.slice() : [];
        if (Array.isArray(trace.buffer)) trace.buffer.length = 0;
        return { available: true, entries };
      })()`,
    );
    const nextAvailable = snapshot?.available !== false;
    const nextEntries = Array.isArray(snapshot?.entries) ? snapshot.entries : [];
    setAvailable(nextAvailable);
    setEntries(
      nextEntries.map((entry, index) => ({ entry, index, id: `${entry.component}:${index}` })),
    );
    setLastDrainAt(Date.now());
    setStatus(
      nextAvailable
        ? nextEntries.length === 0
          ? 'Layout trace drained: no entries'
          : `Layout trace drained: ${nextEntries.length} entries`
        : 'Layout trace unavailable',
    );
  };

  const disable = async () => {
    setStatus('Disabling layout trace…');
    const result = await evaluate<boolean>(
      cdp,
      `(() => {
        const trace = globalThis.__fxeLayoutTrace;
        if (!trace) return false;
        trace.enabled = false;
        return true;
      })()`,
    );
    const nextAvailable = result === true;
    setAvailable(nextAvailable);
    setEnabled(false);
    setStatus(nextAvailable ? 'Layout trace disabled' : 'Layout trace disable failed');
  };

  useEffect(() => {
    void enable();

    return () => {
      void evaluate(
        cdp,
        `(() => {
          const trace = globalThis.__fxeLayoutTrace;
          if (!trace) return false;
          trace.enabled = false;
          return true;
        })()`,
      );
    };
  }, [cdp]);

  useEffect(() => {
    if (!enabled) return;
    let active = true;
    let timer: Timer | null = null;

    const tick = async () => {
      await drain();
      if (!active) return;
      timer = setTimeout(() => {
        void tick();
      }, POLL_INTERVAL_MS);
    };

    timer = setTimeout(() => {
      void tick();
    }, POLL_INTERVAL_MS);

    return () => {
      active = false;
      if (timer !== null) clearTimeout(timer);
    };
  }, [cdp, enabled]);

  const toggleGroup = (component: string) => {
    setExpanded((current) => ({
      ...current,
      [component]: !current[component],
    }));
  };

  if (!available) {
    return (
      <View style={{ width: '100%', height: '100%', padding: 8 }}>
        <View style={{ flexDirection: 'row', gap: 8, marginBottom: 8 }}>
          <Button title="Enable" onPress={() => void enable()} />
          <Button title="Drain" onPress={() => void drain()} />
          <Button title="Disable" onPress={() => void disable()} />
        </View>
        <Text style={{ color: MUTED_COLOR, fontSize: 13 }}>
          Layout trace is unavailable. Import fxe-ui and ensure __fxeLayoutTrace is present.
        </Text>
      </View>
    );
  }

  return (
    <View style={{ width: '100%', height: '100%', padding: 8, gap: 8 }}>
      <View style={{ flexDirection: 'row', gap: 8 }}>
        <Button title="Enable" onPress={() => void enable()} />
        <Button title="Drain" onPress={() => void drain()} />
        <Button title="Disable" onPress={() => void disable()} />
      </View>
      <Text style={{ color: MUTED_COLOR, fontSize: 13 }}>{status}</Text>
      <ScrollView style={{ flex: 1, width: '100%' }}>
        {entries.length === 0 ? (
          <Text style={{ color: MUTED_COLOR, fontSize: 13 }}>
            Waiting for layout trace entries. Interact with the app or trigger a rerender to
            populate the timeline.
          </Text>
        ) : (
          <View style={{ gap: 8, paddingBottom: 8 }}>
            {groups.map((group) => {
              const isExpanded = expanded[group.component] === true;
              return (
                <View
                  key={group.component}
                  style={{
                    borderWidth: 1,
                    borderColor: BORDER_COLOR,
                    backgroundColor: TRACK_COLOR,
                    padding: 8,
                    gap: 6,
                  }}
                >
                  <Pressable onPress={() => toggleGroup(group.component)}>
                    <View style={{ gap: 4 }}>
                      <Text style={{ color: ACCENT_COLOR, fontSize: 14 }}>
                        {isExpanded ? '▾' : '▸'} {group.component}
                      </Text>
                      <Text style={{ color: MUTED_COLOR, fontSize: 12 }}>
                        {group.entries.length} entries · area {formatNumber(group.totalArea)} · max{' '}
                        {formatNumber(group.maxWidth)}×{formatNumber(group.maxHeight)}
                      </Text>
                    </View>
                  </Pressable>
                  {isExpanded ? (
                    <View style={{ gap: 6 }}>
                      {group.entries.map((record) => (
                        <View
                          key={record.id}
                          style={{
                            borderWidth: 1,
                            borderColor: BORDER_COLOR,
                            backgroundColor: 0x111827ff,
                            padding: 8,
                            gap: 3,
                          }}
                        >
                          <Text style={{ color: TEXT_COLOR, fontSize: 12 }}>
                            {record.entry.tag ?? `#${record.index}`}
                          </Text>
                          <Text style={{ color: MUTED_COLOR, fontSize: 12 }}>
                            rect {formatRect(record.entry.rect)}
                          </Text>
                          <Text style={{ color: MUTED_COLOR, fontSize: 12 }}>
                            style {formatLength(record.entry.styleWidth)} ×{' '}
                            {formatLength(record.entry.styleHeight)}
                          </Text>
                          <Text
                            style={{
                              color: record.entry.hasParentLayout ? POSITIVE_COLOR : WARNING_COLOR,
                              fontSize: 12,
                            }}
                          >
                            parent layout: {record.entry.hasParentLayout ? 'yes' : 'no'}
                          </Text>
                        </View>
                      ))}
                    </View>
                  ) : null}
                </View>
              );
            })}
          </View>
        )}
      </ScrollView>
      <Text style={{ color: MUTED_COLOR, fontSize: 13 }}>
        total {entries.length} · components {groups.length} · last drain{' '}
        {formatTimestampLabel(lastDrainAt)}
      </Text>
    </View>
  );
}
