/** @jsxImportSource fxe-ui */
import { Button, ScrollView, Text, View, useEffect, useState } from 'fxe-ui';
import type { CdpClient } from '../cdp_client.ts';

interface HeapSnapshotRoot {
  snapshot?: {
    node_count?: number;
    meta?: {
      node_fields?: string[];
    };
  };
  nodes?: unknown[];
  strings?: string[];
}

interface HeapNodeSummary {
  name: string;
  size: number;
}

function extractSnapshotDetails(snapshotText: string): {
  nodeCount: number | null;
  topNodes: HeapNodeSummary[];
} {
  try {
    const parsed = JSON.parse(snapshotText) as HeapSnapshotRoot;
    const nodeCount =
      typeof parsed.snapshot?.node_count === 'number'
        ? parsed.snapshot.node_count
        : Array.isArray(parsed.nodes)
          ? parsed.nodes.length
          : null;

    if (!Array.isArray(parsed.nodes)) return { nodeCount, topNodes: [] };

    const fields = parsed.snapshot?.meta?.node_fields;
    if (Array.isArray(fields) && parsed.nodes.every((entry) => typeof entry === 'number')) {
      const stride = fields.length;
      const nameIndex = fields.indexOf('name');
      const shallowIndex = fields.indexOf('self_size');
      const retainedIndex = fields.indexOf('retained_size');
      if (stride > 0 && nameIndex >= 0 && (shallowIndex >= 0 || retainedIndex >= 0)) {
        const numericNodes = parsed.nodes as number[];
        const strings = Array.isArray(parsed.strings) ? parsed.strings : [];
        const topNodes: HeapNodeSummary[] = [];
        for (let offset = 0; offset + stride <= numericNodes.length; offset += stride) {
          const stringIndex = numericNodes[offset + nameIndex];
          const name =
            typeof stringIndex === 'number' ? (strings[stringIndex] ?? '<unknown>') : '<unknown>';
          const retainedSize =
            retainedIndex >= 0 ? numericNodes[offset + retainedIndex] : undefined;
          const shallowSize = shallowIndex >= 0 ? numericNodes[offset + shallowIndex] : undefined;
          const size = typeof retainedSize === 'number' ? retainedSize : (shallowSize ?? 0);
          topNodes.push({ name, size });
        }
        topNodes.sort((a, b) => b.size - a.size);
        return { nodeCount, topNodes: topNodes.slice(0, 20) };
      }
    }

    if (parsed.nodes.every((entry) => typeof entry === 'object' && entry !== null)) {
      const topNodes = (parsed.nodes as Array<Record<string, unknown>>)
        .map((entry) => ({
          name:
            typeof entry.name === 'string'
              ? entry.name
              : typeof entry.type === 'string'
                ? entry.type
                : '<unknown>',
          size:
            typeof entry.retainedSize === 'number'
              ? entry.retainedSize
              : typeof entry.retained_size === 'number'
                ? entry.retained_size
                : typeof entry.self_size === 'number'
                  ? entry.self_size
                  : typeof entry.shallowSize === 'number'
                    ? entry.shallowSize
                    : 0,
        }))
        .sort((a, b) => b.size - a.size)
        .slice(0, 20);
      return { nodeCount, topNodes };
    }

    return { nodeCount, topNodes: [] };
  } catch {
    return { nodeCount: null, topNodes: [] };
  }
}

export function HeapPanel({ cdp }: { cdp: CdpClient }) {
  const [nodeCount, setNodeCount] = useState<number | null>(null);
  const [topNodes, setTopNodes] = useState<HeapNodeSummary[]>([]);
  const [status, setStatus] = useState('Idle');

  useEffect(() => {
    void cdp.send('HeapProfiler.enable').catch(() => {});
  }, [cdp]);

  const takeSnapshot = (): void => {
    const chunks: string[] = [];
    const offChunk = cdp.on('HeapProfiler.addHeapSnapshotChunk', (params) => {
      const chunk = (params as { chunk?: string }).chunk;
      if (typeof chunk === 'string') chunks.push(chunk);
    });

    const progress = Promise.withResolvers<void>();
    const offProgress = cdp.on('HeapProfiler.reportHeapSnapshotProgress', (params) => {
      const progressEvent = params as { finished?: boolean };
      if (progressEvent.finished) progress.resolve();
    });

    setStatus('Taking heap snapshot…');
    void cdp
      .send('HeapProfiler.takeHeapSnapshot')
      .then(() => progress.promise)
      .then(() => {
        const details = extractSnapshotDetails(chunks.join(''));
        setNodeCount(details.nodeCount);
        setTopNodes(details.topNodes);
        setStatus(
          details.nodeCount === null
            ? 'Snapshot received but nodeCount was unavailable'
            : 'Snapshot complete',
        );
      })
      .catch((error) => {
        const message = error instanceof Error ? error.message : String(error);
        setStatus(`Heap snapshot failed: ${message}`);
      })
      .finally(() => {
        offChunk();
        offProgress();
      });
  };

  return (
    <View style={{ width: '100%', height: '100%', gap: 12 }}>
      <Button title="Take Heap Snapshot" onPress={takeSnapshot} />
      <Text style={{ color: 0xe2e8f0ff, fontSize: 14 }}>Status: {status}</Text>
      <Text style={{ color: 0x66aaffff, fontSize: 16 }}>
        nodeCount: {nodeCount === null ? 'n/a' : String(nodeCount)}
      </Text>
      <ScrollView style={{ flex: 1 }}>
        {topNodes.length === 0 ? (
          <Text style={{ color: 0x94a3b8ff, fontSize: 13 }}>
            No retained-node summaries in the last snapshot.
          </Text>
        ) : (
          topNodes.map((node, index) => (
            <Text key={`${node.name}-${index}`} style={{ color: 0xe2e8f0ff, fontSize: 13 }}>
              {index + 1}. {node.name} — {node.size}
            </Text>
          ))
        )}
      </ScrollView>
    </View>
  );
}
