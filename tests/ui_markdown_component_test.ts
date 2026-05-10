import { CommandBuffer } from 'fxe';
import {
  Markdown as MarkdownComponent,
  clearHitTargets,
  hitTest,
  render,
  resetEventPipeline,
  snapshotFiberTree,
  type FiberNode,
  type HitTarget,
  type MarkdownTheme,
} from 'fxe-ui';

import { assert, assertEqual, run, test } from './ts_harness.ts';

Font.load('/System/Library/Fonts/Monaco.ttf', 32);

type MarkdownApi = typeof Markdown;

function collectDisplayNames(nodes: readonly FiberNode[]): string[] {
  const out: string[] = [];
  const visit = (node: FiberNode): void => {
    if (node.displayName) out.push(node.displayName);
    for (const child of node.children) visit(child);
  };
  for (const node of nodes) visit(node);
  return out;
}
function vertexBufferSignature(cb: CommandBuffer): string {
  return Array.from(cb.vertexBuffer()).join(',');
}

function findPressableTarget(): HitTarget | null {
  for (const point of [
    { x: 24, y: 24 },
    { x: 32, y: 24 },
    { x: 40, y: 24 },
    { x: 24, y: 32 },
    { x: 40, y: 40 },
    { x: 56, y: 24 },
  ]) {
    const target = hitTest(point.x, point.y);
    if (target?.componentType === 'Pressable') return target;
  }
  return null;
}

test('Markdown returns a component node for simple content', () => {
  const node = MarkdownComponent({ source: '# Hello\n\nworld' });
  assertEqual(node.type, 'component');
  if (node.type !== 'component') throw new Error('expected component node');
  assertEqual(node.displayName, 'Markdown');
});

test('Markdown renders simple content without throwing', () => {
  const cb = new CommandBuffer();
  render(MarkdownComponent({ source: '# Hello\n\nworld', style: { width: 240, height: 160 } }), cb);
  const names = collectDisplayNames(snapshotFiberTree().tree);
  assert(names.includes('Markdown'), 'expected Markdown component in snapshot');
  assert(names.includes('MdHeading'), 'expected heading block in snapshot');
  assert(names.includes('MdParagraph'), 'expected paragraph block in snapshot');
  assert(cb.vertexCount() > 0, 'expected rendered markdown to emit geometry');
});

test('Markdown empty source produces an empty container', () => {
  const cb = new CommandBuffer();
  render(MarkdownComponent({ source: '', style: { width: 200, height: 100 } }), cb);
  const names = collectDisplayNames(snapshotFiberTree().tree);
  assert(names.includes('Markdown'), 'expected Markdown component in snapshot');
  assert(names.includes('View'), 'expected root container view in snapshot');
  assert(!names.includes('MdParagraph'), 'empty markdown should not render paragraph blocks');
});

test('Markdown parse failure falls back to empty container', () => {
  const markdownGlobal = globalThis as typeof globalThis & { Markdown: MarkdownApi };
  const originalMarkdown = markdownGlobal.Markdown;
  const throwingMarkdown: MarkdownApi = {
    ...originalMarkdown,
    parse(): FXEMarkdown.DocumentNode {
      throw new Error('boom');
    },
  };
  markdownGlobal.Markdown = throwingMarkdown;
  try {
    const cb = new CommandBuffer();
    render(MarkdownComponent({ source: '# broken', style: { width: 200, height: 100 } }), cb);
    const names = collectDisplayNames(snapshotFiberTree().tree);
    assert(names.includes('Markdown'), 'expected Markdown component in snapshot');
    assert(!names.includes('MdHeading'), 'parse failure should not render markdown blocks');
  } finally {
    markdownGlobal.Markdown = originalMarkdown;
  }
});

test('Markdown custom onLinkPress is invoked from rendered link pressable', () => {
  clearHitTargets();
  resetEventPipeline();
  let seen: string | null = null;
  try {
    render(
      MarkdownComponent({
        source: '[docs](https://example.com)',
        style: { width: 240, height: 120 },
        onLinkPress: (href) => {
          seen = href;
        },
      }),
      new CommandBuffer(),
    );
    const target = findPressableTarget();
    assert(target !== null, 'expected rendered markdown link hit target');
    target.onPress?.({} as never);
    assertEqual(seen, 'https://example.com');
  } finally {
    clearHitTargets();
    resetEventPipeline();
  }
});

test('Markdown theme link override propagates into rendered output', () => {
  const makeTheme = (link: number): Partial<MarkdownTheme> => ({
    colors: { link } as MarkdownTheme['colors'],
  });
  const source = '[docs](https://example.com)';
  const first = new CommandBuffer();
  const second = new CommandBuffer();
  render(
    MarkdownComponent({ source, style: { width: 240, height: 120 }, theme: makeTheme(0x12ab34ff) }),
    first,
  );
  render(
    MarkdownComponent({ source, style: { width: 240, height: 120 }, theme: makeTheme(0xcd3412ff) }),
    second,
  );
  assert(first.vertexCount() > 0, 'expected markdown link render to emit geometry');
  assertEqual(first.vertexCount(), second.vertexCount());
  assert(
    vertexBufferSignature(first) !== vertexBufferSignature(second),
    'expected link theme override to change rendered output',
  );
});

test('Markdown code block renders highlighted block shape without throwing', () => {
  const cb = new CommandBuffer();
  render(
    MarkdownComponent({
      source: '```ts\nconst a = 1;\n```',
      style: { width: 320, height: 180 },
    }),
    cb,
  );
  const names = collectDisplayNames(snapshotFiberTree().tree);
  assert(names.includes('MdCodeBlock'), 'expected code block component in snapshot');
  assert(cb.vertexCount() > 0, 'expected code block to emit geometry');
});

await run();
