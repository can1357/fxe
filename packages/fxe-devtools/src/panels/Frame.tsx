/** @jsxImportSource fxe-ui */
import { ScrollView, Text, useEffect, useMemo, useState, View } from 'fxe-ui';
import type { CdpClient } from '../cdp_client.ts';

interface FrameSample {
  frameId: number;
  startMs: number;
  totalMs: number;
  dtMs: number;
  phases: {
    js: number;
    animations: number;
    reconcile: number;
    frameCallbacks: number;
  };
}

interface RuntimeEvaluateResponse {
  result?: {
    value?: unknown;
  };
}

interface FrameSnapshotResult {
  available: boolean;
  samples: FrameSample[];
}

type Timer = ReturnType<typeof setTimeout>;

const FRAME_BUDGET_MS = 16.6;
const POLL_INTERVAL_MS = 250;
const RING_SIZE = 240;
const MAX_SAMPLES = 60;
const TIMELINE_WIDTH_PX = 480;
const TRACK_COLOR = 0x0f172aff;
const BORDER_COLOR = 0x1f2937ff;
const TEXT_COLOR = 0xe2e8f0ff;
const MUTED_COLOR = 0x94a3b8ff;
const RECONCILE_COLOR = 0x3b82f6ff;
const ANIMATIONS_COLOR = 0x22c55eff;
const CALLBACKS_COLOR = 0xf59e0bff;
const UNACCOUNTED_COLOR = 0x64748bff;
const OVERFLOW_COLOR = 0xef4444ff;

async function evaluate<T>(cdp: CdpClient, expression: string): Promise<T | undefined> {
  const response = await cdp
    .send<RuntimeEvaluateResponse>('Runtime.evaluate', { expression, returnByValue: true })
    .catch(() => null);
  return response?.result?.value as unknown as T | undefined;
}

function msToWidth(ms: number): number {
  return Math.max(0, (ms / FRAME_BUDGET_MS) * TIMELINE_WIDTH_PX);
}

function formatMs(ms: number): string {
  return ms.toFixed(2);
}

function percentile95(samples: readonly FrameSample[]): number {
  if (samples.length === 0) return 0;
  const totals = samples.map((sample) => sample.totalMs).sort((a, b) => a - b);
  const index = Math.min(totals.length - 1, Math.ceil(totals.length * 0.95) - 1);
  return totals[index] ?? 0;
}

function sampleWindow(samples: readonly FrameSample[]): FrameSample[] {
  return samples.slice(-MAX_SAMPLES);
}

export function FramePanel({ cdp }: { cdp: CdpClient }) {
  const [samples, setSamples] = useState<FrameSample[]>([]);
  const [available, setAvailable] = useState(true);

  useEffect(() => {
    let active = true;
    let timer: Timer | null = null;

    const enable = async (): Promise<boolean> => {
      const result = await evaluate<boolean>(
        cdp,
        `(() => {
          const profile = globalThis.__fxeFrameProfile;
          if (!profile) return false;
          profile.enable({ ringSize: ${RING_SIZE} });
          return true;
        })()`,
      );
      if (!active) return false;
      const nextAvailable = result === true;
      setAvailable(nextAvailable);
      if (!nextAvailable) setSamples([]);
      return nextAvailable;
    };

    const poll = async (): Promise<void> => {
      const result = await evaluate<FrameSnapshotResult>(
        cdp,
        `(() => {
          const profile = globalThis.__fxeFrameProfile;
          if (!profile) return { available: false, samples: [] };
          return { available: true, samples: profile.snapshot().slice(-${MAX_SAMPLES}) };
        })()`,
      );
      if (!active) return;
      setAvailable(result?.available !== false);
      setSamples(Array.isArray(result?.samples) ? sampleWindow(result.samples) : []);
      timer = setTimeout(() => {
        void poll();
      }, POLL_INTERVAL_MS);
    };

    void enable().then((enabled) => {
      if (!active || !enabled) return;
      void poll();
    });

    return () => {
      active = false;
      if (timer !== null) clearTimeout(timer);
      void evaluate(
        cdp,
        `(() => {
          const profile = globalThis.__fxeFrameProfile;
          if (!profile) return true;
          profile.disable();
          return true;
        })()`,
      );
    };
  }, [cdp]);

  const stats = useMemo(() => {
    const count = samples.length;
    const total = samples.reduce((sum, sample) => sum + sample.totalMs, 0);
    const max = samples.reduce((current, sample) => Math.max(current, sample.totalMs), 0);
    return {
      count,
      avg: count === 0 ? 0 : total / count,
      max,
      p95: percentile95(samples),
    };
  }, [samples]);

  if (!available) {
    return (
      <View style={{ width: '100%', height: '100%', padding: 8 }}>
        <Text style={{ color: MUTED_COLOR, fontSize: 13 }}>
          This app has not imported fxe-ui; frame profiling is unavailable.
        </Text>
      </View>
    );
  }

  return (
    <View style={{ width: '100%', height: '100%', padding: 8, gap: 8 }}>
      <Text style={{ color: TEXT_COLOR, fontSize: 14 }}>
        Frame timeline (last {stats.count} samples)
      </Text>
      <ScrollView style={{ flex: 1, width: '100%' }}>
        <View style={{ gap: 6, paddingBottom: 8 }}>
          {samples.length === 0 ? (
            <Text style={{ color: MUTED_COLOR, fontSize: 13 }}>Waiting for frame samples…</Text>
          ) : (
            samples.map((sample) => {
              const reconcileMs = Math.max(0, sample.phases.reconcile);
              const animationsMs = Math.max(0, sample.phases.animations);
              const frameCallbacksMs = Math.max(0, sample.phases.frameCallbacks);
              const unaccountedMs = Math.max(
                0,
                sample.phases.js - reconcileMs - animationsMs - frameCallbacksMs,
              );
              const overflowWidth = Math.max(0, msToWidth(sample.totalMs) - TIMELINE_WIDTH_PX);
              const segments = [
                { key: 'reconcile', ms: reconcileMs, color: RECONCILE_COLOR },
                { key: 'animations', ms: animationsMs, color: ANIMATIONS_COLOR },
                { key: 'frameCallbacks', ms: frameCallbacksMs, color: CALLBACKS_COLOR },
                { key: 'unaccounted', ms: unaccountedMs, color: UNACCOUNTED_COLOR },
              ];
              let consumedWidth = 0;
              return (
                <View key={String(sample.frameId)} style={{ gap: 3 }}>
                  <Text style={{ color: MUTED_COLOR, fontSize: 11 }}>
                    #{sample.frameId} · {formatMs(sample.totalMs)} ms
                  </Text>
                  <View style={{ flexDirection: 'row', alignItems: 'center', gap: 4 }}>
                    <View
                      style={{
                        width: TIMELINE_WIDTH_PX,
                        height: 6,
                        flexDirection: 'row',
                        backgroundColor: TRACK_COLOR,
                        borderWidth: 1,
                        borderColor: BORDER_COLOR,
                      }}
                    >
                      {segments.map((segment) => {
                        const rawWidth = msToWidth(segment.ms);
                        const width = Math.max(
                          0,
                          Math.min(rawWidth, TIMELINE_WIDTH_PX - consumedWidth),
                        );
                        consumedWidth += width;
                        if (width <= 0) return null;
                        return (
                          <View
                            key={segment.key}
                            style={{
                              flexBasis: width,
                              width,
                              height: 6,
                              backgroundColor: segment.color,
                            }}
                          />
                        );
                      })}
                    </View>
                    {overflowWidth > 0 ? (
                      <View
                        style={{
                          flexBasis: overflowWidth,
                          width: overflowWidth,
                          height: 6,
                          backgroundColor: OVERFLOW_COLOR,
                        }}
                      />
                    ) : null}
                  </View>
                </View>
              );
            })
          )}
        </View>
      </ScrollView>
      <Text style={{ color: MUTED_COLOR, fontSize: 13 }}>
        avg {formatMs(stats.avg)} ms · max {formatMs(stats.max)} ms · p95 {formatMs(stats.p95)} ms
      </Text>
    </View>
  );
}
