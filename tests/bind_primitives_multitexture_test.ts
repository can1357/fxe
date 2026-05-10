import { CommandBuffer, Image, Primitives, Spritesheet, VertexTopology } from 'fxe';
import { assert, assertDeepEqual, assertEqual, test } from './ts_harness.ts';

const TRIANGLE = VertexTopology.Triangle;

function textureIds(cb: CommandBuffer): number[] {
  const verts = cb.vertexBuffer();
  const words = new Uint32Array(verts.buffer, verts.byteOffset, verts.length);
  const out: number[] = [];
  for (let i = 0; i < verts.length; i += 8) out.push(words[i + 7]);
  return out;
}

test('Primitives.drawSprite emits textured quad vertices for external image handles', () => {
  const img = Image.fromBytes(
    new Uint8Array([255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255]),
    2,
    2,
  );
  const texId = img.textureId();
  assert(texId !== 0, 'ImageHandle.textureId() should register a texture handle');

  const sheet = new Spritesheet();
  const spriteId = sheet.add(img);
  const resolved = sheet.resolve(spriteId);
  assertEqual(resolved.textureId, texId);
  assertEqual(resolved.width, 2);
  assertEqual(resolved.height, 2);

  img.dispose();
  const retained = sheet.resolve(spriteId);
  assertEqual(retained.textureId, texId);

  const cb = new CommandBuffer();
  Primitives.drawSprite(cb, texId, 10, 20, 32, 24, 0.25, 0xffffffff);

  assert(cb.vertexCount() > 0, 'drawSprite should emit vertices');
  assert(cb.indexCount(TRIANGLE) > 0, 'drawSprite should emit triangle indices');
  assertDeepEqual(textureIds(cb), [texId, texId, texId, texId]);

  sheet.dispose();
});
