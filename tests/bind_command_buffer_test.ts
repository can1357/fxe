import { assert, assertEqual, assertThrows, test } from './ts_harness.ts';

const TRIANGLE = 0 as FXE.VertexTopology;
const LINE = 1 as FXE.VertexTopology;
const FLOATS_PER_VERTEX = 8;

function translation(tx: number, ty: number, tz: number): Float32Array {
  return new Float32Array([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, tx, ty, tz, 1]);
}

function identity(): Float32Array {
  return translation(0, 0, 0);
}

function writeVertex(
  verts: Float32Array,
  vertex: number,
  x: number,
  y: number,
  z: number,
  w = 0,
): void {
  const base = vertex * FLOATS_PER_VERTEX;
  verts[base + 0] = x;
  verts[base + 1] = y;
  verts[base + 2] = z;
  verts[base + 3] = w;
}

function vertexComponent(verts: Float32Array, vertex: number, component: number): number {
  return verts[vertex * FLOATS_PER_VERTEX + component];
}

test('CommandBuffer constructor creates an empty buffer', () => {
  const cb = new CommandBuffer();

  assert(cb instanceof CommandBuffer, 'constructor should return CommandBuffer instance');
  assertEqual(cb.epoch(), 1);
  assertEqual(cb.vertexCount(), 0);
  assertEqual(cb.indexCount(), 0);
  assertEqual(cb.indexCount(TRIANGLE), 0);
  assertEqual(cb.indexCount(LINE), 0);
  assert(cb.isEmpty(), 'new CommandBuffer should be empty');

  const views = cb.buffers(TRIANGLE);
  assert(views.verts instanceof Float32Array, 'buffers().verts should be Float32Array');
  assert(views.idxs instanceof Uint32Array, 'buffers().idxs should be Uint32Array');
  assertEqual(views.verts.length, 0);
  assertEqual(views.idxs.length, 0);
  assertEqual(views.epoch, cb.epoch());
  assertEqual(cb.vertexBuffer().length, 0);
  assertEqual(cb.indexBuffer(TRIANGLE).length, 0);
});

test('CommandBuffer allocate returns aliased views, bases, counts, and epochs', () => {
  const cb = new CommandBuffer();
  const first = cb.allocate(3, 3, TRIANGLE);

  assertEqual(first.base, 0);
  assertEqual(first.indexBase, 0);
  assertEqual(first.verts.length, 3 * FLOATS_PER_VERTEX);
  assertEqual(first.idxs.length, 3);
  assertEqual(first.epoch, cb.epoch());
  assertEqual(cb.epoch(), 2);
  assertEqual(cb.vertexCount(), 3);
  assertEqual(cb.indexCount(TRIANGLE), 3);
  assertEqual(cb.indexCount(LINE), 0);
  assert(!cb.isEmpty(), 'allocated CommandBuffer should not be empty');

  writeVertex(first.verts, 0, 1, 2, 3);
  writeVertex(first.verts, 1, 4, 5, 6);
  writeVertex(first.verts, 2, 7, 8, 9);
  first.idxs.set([0, 1, 2]);

  const all = cb.buffers(TRIANGLE);
  assertEqual(all.epoch, cb.epoch());
  assertEqual(all.verts.length, 3 * FLOATS_PER_VERTEX);
  assertEqual(all.idxs.length, 3);
  assertEqual(vertexComponent(all.verts, 0, 0), 1);
  assertEqual(vertexComponent(all.verts, 1, 1), 5);
  assertEqual(all.idxs[2], 2);

  const second = cb.allocate(2, 2, TRIANGLE);
  assertEqual(second.base, 3);
  assertEqual(second.indexBase, 3);
  assertEqual(second.verts.length, 2 * FLOATS_PER_VERTEX);
  assertEqual(second.idxs.length, 2);
  assertEqual(second.epoch, cb.epoch());
  assertEqual(cb.epoch(), 3);
  assertEqual(cb.vertexCount(), 5);
  assertEqual(cb.indexCount(TRIANGLE), 5);

  second.idxs.set([3, 4]);
  const indices = cb.indexBuffer(TRIANGLE);
  assertEqual(indices.length, 5);
  assertEqual(indices[0], 0);
  assertEqual(indices[3], 3);
  assertEqual(indices[4], 4);
});

test('CommandBuffer buffers are separated by topology while vertices are shared', () => {
  const cb = new CommandBuffer();
  cb.allocate(1, 1, TRIANGLE).idxs[0] = 0;
  const line = cb.allocate(2, 2, LINE);
  line.idxs.set([1, 2]);

  assertEqual(cb.vertexCount(), 3);
  assertEqual(cb.indexCount(), 1);
  assertEqual(cb.indexCount(TRIANGLE), 1);
  assertEqual(cb.indexCount(LINE), 2);
  assertEqual(cb.buffers(TRIANGLE).verts.length, 3 * FLOATS_PER_VERTEX);
  assertEqual(cb.buffers(TRIANGLE).idxs.length, 1);
  assertEqual(cb.buffers(LINE).verts.length, 3 * FLOATS_PER_VERTEX);
  assertEqual(cb.buffers(LINE).idxs.length, 2);
  assertEqual(cb.indexBuffer(LINE)[1], 2);
  assertEqual(cb.vertexBuffer().length, 3 * FLOATS_PER_VERTEX);
});

test('CommandBuffer clear empties all topologies and advances epoch', () => {
  const cb = new CommandBuffer();
  cb.allocate(1, 1, TRIANGLE);
  cb.allocate(1, 1, LINE);
  const before = cb.epoch();

  cb.clear();

  assertEqual(cb.epoch(), before + 1);
  assertEqual(cb.vertexCount(), 0);
  assertEqual(cb.indexCount(TRIANGLE), 0);
  assertEqual(cb.indexCount(LINE), 0);
  assertEqual(cb.vertexBuffer().length, 0);
  assertEqual(cb.indexBuffer(LINE).length, 0);
  assert(cb.isEmpty(), 'clear should restore empty state');
});

test('CommandBuffer transform mutates vertices and advances epoch', () => {
  const cb = new CommandBuffer();
  const allocation = cb.allocate(2, 0, TRIANGLE);
  writeVertex(allocation.verts, 0, 1, 2, 3);
  writeVertex(allocation.verts, 1, -1, -2, -3, 1);
  const before = cb.epoch();

  cb.transform(translation(10, 20, 30));

  const verts = cb.vertexBuffer();
  assertEqual(cb.epoch(), before + 1);
  assertEqual(vertexComponent(verts, 0, 0), 11);
  assertEqual(vertexComponent(verts, 0, 1), 22);
  assertEqual(vertexComponent(verts, 0, 2), 33);
  assertEqual(vertexComponent(verts, 0, 3), 0);
  assertEqual(vertexComponent(verts, 1, 0), 9);
  assertEqual(vertexComponent(verts, 1, 1), 18);
  assertEqual(vertexComponent(verts, 1, 2), 27);
  assertEqual(vertexComponent(verts, 1, 3), 1);
});

test('CommandBuffer queue appends transformed vertices and offset indices', () => {
  const dst = new CommandBuffer();
  const existing = dst.allocate(1, 1, TRIANGLE);
  writeVertex(existing.verts, 0, 100, 0, 0);
  existing.idxs[0] = 0;

  const src = new CommandBuffer();
  const srcAllocation = src.allocate(2, 2, TRIANGLE);
  writeVertex(srcAllocation.verts, 0, 1, 2, 3);
  writeVertex(srcAllocation.verts, 1, 4, 5, 6);
  srcAllocation.idxs.set([0, 1]);

  const before = dst.epoch();
  dst.queue(src, translation(10, 0, 0));

  assertEqual(dst.epoch(), before + 1);
  assertEqual(dst.vertexCount(), 3);
  assertEqual(dst.indexCount(TRIANGLE), 3);
  const verts = dst.vertexBuffer();
  const idxs = dst.indexBuffer(TRIANGLE);
  assertEqual(vertexComponent(verts, 0, 0), 100);
  assertEqual(vertexComponent(verts, 1, 0), 11);
  assertEqual(vertexComponent(verts, 2, 0), 14);
  assertEqual(idxs[0], 0);
  assertEqual(idxs[1], 1);
  assertEqual(idxs[2], 2);

  dst.queue(src, undefined, new Float32Array([1, 1, 1, 1]));
  assertEqual(dst.vertexCount(), 5);
  assertEqual(dst.indexCount(TRIANGLE), 5);
});

test('CommandBuffer clone copies data without sharing later mutations', () => {
  const cb = new CommandBuffer();
  const allocation = cb.allocate(1, 1, TRIANGLE);
  writeVertex(allocation.verts, 0, 3, 4, 5);
  allocation.idxs[0] = 0;

  const cloned = cb.clone();
  assert(cloned instanceof CommandBuffer, 'clone should return CommandBuffer');
  assertEqual(cloned.epoch(), cb.epoch());
  assertEqual(cloned.vertexCount(), 1);
  assertEqual(cloned.indexCount(TRIANGLE), 1);
  assertEqual(vertexComponent(cloned.vertexBuffer(), 0, 0), 3);
  assertEqual(cloned.indexBuffer(TRIANGLE)[0], 0);
  assert(!cloned.isEmpty(), 'clone of populated buffer should not be empty');

  cb.clear();
  assert(cb.isEmpty(), 'original should be clear');
  assertEqual(cloned.vertexCount(), 1);
  assertEqual(cloned.indexCount(TRIANGLE), 1);
  assertEqual(vertexComponent(cloned.vertexBuffer(), 0, 1), 4);

  cloned.transform(translation(1, 1, 1));
  assertEqual(cb.vertexCount(), 0);
  assertEqual(vertexComponent(cloned.vertexBuffer(), 0, 0), 4);
});

test('CommandBuffer rejects invalid arguments', () => {
  const cb = new CommandBuffer();

  assertThrows(() => (CommandBuffer as unknown as () => FXE.CommandBuffer)(), /new/);
  assertThrows(
    () => (cb.allocate as unknown as (v: number, i: number) => FXE.Allocation)(1, 1),
    /allocate/,
  );
  assertThrows(() => cb.allocate(1, 1, 2 as FXE.VertexTopology), /topology/);
  assertThrows(() => cb.indexCount(2 as FXE.VertexTopology), /topology/);
  assertThrows(() => cb.buffers(2 as FXE.VertexTopology), /topology/);
  assertThrows(() => cb.indexBuffer(2 as FXE.VertexTopology), /topology/);
  assertThrows(() => cb.transform(new Float32Array(15)), /Float32Array\(16\)/);
  assertThrows(() => cb.queue({} as FXE.CommandBuffer), /CommandBuffer/);
  assertThrows(() => cb.queue(new CommandBuffer(), new Float32Array(15)), /Float32Array\(16\)/);
  assertThrows(
    () => cb.queue(new CommandBuffer(), identity(), new Float32Array(3)),
    /Float32Array\(4\)/,
  );
});
