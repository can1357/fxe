import { StickyScroll, createIndentOutlineProvider, createTreeSitterOutlineProvider } from 'fxe-ui';
import { resolveStickyScrollEntries } from '../packages/fxe-ui/src/components/StickyScroll.ts';
import { getIndentStickyEntries } from '../packages/fxe-ui/src/components/sticky_scroll_outline.ts';
import { assertDeepEqual, assertEqual, run, test } from './ts_harness.ts';

test('indent outline returns no entries at top of document', () => {
  const doc = new TextDocument('function outer() {\n  body();\n}');
  const outline = createIndentOutlineProvider();
  assertDeepEqual(outline.getStickyEntries(doc, 0, 4), []);
});

test('indent outline returns enclosing function header for a body line', () => {
  const doc = new TextDocument('function outer() {\n  body();\n}');
  assertDeepEqual(getIndentStickyEntries(doc, 1, 4), [{ line: 0, depth: 0 }]);
});

test('indent outline respects maxDepth and orders by depth ascending', () => {
  const doc = new TextDocument(
    [
      'class Box {',
      '  function outer() {',
      '    if (ready) {',
      '      work();',
      '    }',
      '  }',
      '}',
    ].join('\n'),
  );
  assertDeepEqual(getIndentStickyEntries(doc, 3, 2), [
    { line: 1, depth: 0 },
    { line: 2, depth: 1 },
  ]);
});

test('indent outline skips blank lines as ancestors', () => {
  const doc = new TextDocument('function outer() {\n\n  body();\n}');
  assertDeepEqual(getIndentStickyEntries(doc, 2, 4), [{ line: 0, depth: 0 }]);
});

test('tree-sitter provider returns [] when no callback supplied', () => {
  const doc = new TextDocument('function outer() {}');
  const outline = createTreeSitterOutlineProvider();
  assertDeepEqual(outline.getStickyEntries(doc, 0, 4), []);
});

test('tree-sitter provider forwards to callback when present', () => {
  const doc = new TextDocument('function outer() {}');
  const outline = createTreeSitterOutlineProvider((seenDoc, topVisibleLine, maxDepth) => {
    assertEqual(seenDoc, doc);
    assertEqual(topVisibleLine, 3);
    assertEqual(maxDepth, 2);
    return [{ line: 1, depth: 0, label: 'outer' }];
  });
  assertDeepEqual(outline.getStickyEntries(doc, 3, 2), [{ line: 1, depth: 0, label: 'outer' }]);
});

test('StickyScroll renders nothing when topVisibleLine is 0', () => {
  const doc = new TextDocument('function outer() {\n  body();\n}');
  const outline = createIndentOutlineProvider();
  assertDeepEqual(resolveStickyScrollEntries(doc, outline, 0, 20, 4), []);

  const node = StickyScroll({
    doc,
    scrollOffset: 0,
    lineHeight: 20,
    width: 320,
    outline,
  });
  assertEqual(node.type, 'component');
  if (node.type !== 'component') throw new Error('expected component node');
  assertEqual(node.displayName, 'StickyScroll');
});

await run();
