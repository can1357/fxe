import { assertDeepEqual, assertEqual, test } from './ts_harness.ts';

const ZERO_STATS = {
  verticesSubmitted: 0,
  indicesSubmitted: 0,
  queueCalls: 0,
  cacheHits: 0,
  cacheMisses: 0,
  rebuilds: 0,
  frames: 0,
};

const SNAPSHOT_KEYS = Object.keys(ZERO_STATS).sort();

test('RenderStats.snapshot exposes the complete numeric counter shape', () => {
  RenderStats.reset();

  const snapshot = RenderStats.snapshot();

  assertDeepEqual(Object.keys(snapshot).sort(), SNAPSHOT_KEYS);
  for (const key of SNAPSHOT_KEYS) {
    assertEqual(
      typeof snapshot[key as keyof typeof snapshot],
      'number',
      `${key} should be numeric`,
    );
  }
  assertDeepEqual(snapshot, ZERO_STATS);
});

test('RenderStats record methods and beginFrame increment only their counters', () => {
  RenderStats.reset();

  RenderStats.recordCacheHit();
  RenderStats.recordCacheHit();
  RenderStats.recordCacheMiss();
  RenderStats.recordRebuild();
  RenderStats.recordQueueCall();
  RenderStats.beginFrame();
  RenderStats.beginFrame();
  RenderStats.beginFrame();

  assertDeepEqual(RenderStats.snapshot(), {
    ...ZERO_STATS,
    queueCalls: 1,
    cacheHits: 2,
    cacheMisses: 1,
    rebuilds: 1,
    frames: 3,
  });
});

test('RenderStats.reset clears all counters after activity', () => {
  RenderStats.reset();

  RenderStats.recordCacheHit();
  RenderStats.recordCacheMiss();
  RenderStats.recordRebuild();
  RenderStats.recordQueueCall();
  RenderStats.beginFrame();

  RenderStats.reset();

  assertDeepEqual(RenderStats.snapshot(), ZERO_STATS);
});

test('CommandBuffer.queue updates RenderStats queue submission counters', () => {
  RenderStats.reset();

  const source = new CommandBuffer();
  source.allocate(3, 3, 0);
  assertEqual(source.vertexCount(), 3);
  assertEqual(source.indexCount(0), 3);

  const target = new CommandBuffer();
  target.queue(source);

  assertDeepEqual(RenderStats.snapshot(), {
    ...ZERO_STATS,
    verticesSubmitted: 3,
    indicesSubmitted: 3,
    queueCalls: 1,
  });

  target.queue(new CommandBuffer());

  assertDeepEqual(RenderStats.snapshot(), {
    ...ZERO_STATS,
    verticesSubmitted: 3,
    indicesSubmitted: 3,
    queueCalls: 2,
  });
});
