import { CommandBuffer, Primitives, VertexTopology } from 'fxe';

function rate(name: string, count: number, fn: () => void): void {
  const begin = performance.now();
  fn();
  const elapsed = performance.now() - begin;
  console.log(`${name}/sec=${Math.round(count / (elapsed / 1000))}`);
}

const allocateCount = 250_000;
const allocateBuffer = new CommandBuffer();
rate('allocate', allocateCount, () => {
  for (let i = 0; i < allocateCount; ++i) {
    allocateBuffer.allocate(3, 3, VertexTopology.Triangle);
  }
});
const view = allocateBuffer.buffers(VertexTopology.Triangle);
console.log(
  `allocate vertices=${allocateBuffer.vertexCount()} indices=${allocateBuffer.indexCount(VertexTopology.Triangle)} epoch=${view.epoch}`,
);

const drainCount = 250_000;
const drainBuffer = new CommandBuffer();
const opcodes = new Uint32Array(drainCount);
const params = new Float32Array(drainCount * 9);
opcodes.fill(Primitives.OP_FILL_RECT);
for (let i = 0, p = 0; i < drainCount; ++i) {
  params[p++] = i & 255;
  params[p++] = (i >> 8) & 255;
  params[p++] = 8;
  params[p++] = 8;
  params[p++] = 0;
  params[p++] = 1;
  params[p++] = 1;
  params[p++] = 1;
  params[p++] = 1;
}
rate('drainFillRect', drainCount, () => {
  const executed = Primitives.drain(drainBuffer, opcodes, params);
  if (executed !== drainCount) {
    throw new Error(`drained ${executed}, expected ${drainCount}`);
  }
});
const drained = drainBuffer.buffers(VertexTopology.Triangle);
console.log(
  `drain vertices=${drainBuffer.vertexCount()} indices=${drainBuffer.indexCount(VertexTopology.Triangle)} epoch=${drained.epoch}`,
);
