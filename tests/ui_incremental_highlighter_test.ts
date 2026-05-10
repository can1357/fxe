import {
  createIncrementalHighlighter,
  defaultHighlightTheme,
  defaultTheme,
  type HighlightTheme,
  type LineDecorations,
} from 'fxe-ui';

import { assert, assertEqual, run, test } from './ts_harness.ts';

const supported = Markdown.highlightLanguages();
const hasTypescript = supported.includes('typescript');

const baseTheme: HighlightTheme = {
  comment: { color: 0x586e75ff },
  function: { color: 0x268bd2ff },
  keyword: { color: 0x859900ff },
  number: { color: 0xd33682ff },
  string: { color: 0x2aa198ff },
  type: { color: 0xb58900ff },
};

test('createIncrementalHighlighter returns spans for a highlighted line', () => {
  if (!hasTypescript) return;

  const document = new TextDocument('const x = 1;\nconst y = 2;');
  const highlighter = createIncrementalHighlighter({
    document,
    language: 'ts',
    theme: defaultHighlightTheme(defaultTheme),
  });

  try {
    const decorations = highlighter.getLineDecorations(0);
    assert(decorations !== null, 'line 0 should produce decorations');
    assert((decorations.spans?.length ?? 0) > 0, 'line 0 should have at least one span');
    assertEqual(highlighter.revision(), document.revision());
  } finally {
    highlighter.dispose();
  }
});

test('repeated reads hit the cache and edits invalidate downstream lines', () => {
  if (!hasTypescript) return;

  type MutableMarkdown = {
    highlight(source: string, language: string): FXEMarkdown.HighlightResult | null;
  };
  const mutableMarkdown = Markdown as MutableMarkdown;
  const originalHighlight = mutableMarkdown.highlight;
  let calls = 0;
  mutableMarkdown.highlight = (source, language) => {
    ++calls;
    return originalHighlight(source, language);
  };

  const document = new TextDocument('const x = 1; /* ok */\nconst y = 2;');
  const highlighter = createIncrementalHighlighter({
    document,
    language: 'typescript',
    theme: baseTheme,
  });

  try {
    const before = spanSignature(highlighter.getLineDecorations(1));
    const beforeEditedLine = spanSignature(highlighter.getLineDecorations(0));
    assert(before.length > 0, 'line 1 should start highlighted');
    assert(beforeEditedLine.length > 0, 'line 0 should start highlighted');
    assertEqual(calls, 1, 'initial highlight should run once');

    spanSignature(highlighter.getLineDecorations(1));
    spanSignature(highlighter.getLineDecorations(0));
    assertEqual(calls, 1, 'cached reads must not re-run Markdown.highlight');

    const closeComment = document.text().indexOf('*/');
    assert(closeComment >= 0, 'expected closing block comment');
    document.replace(closeComment, closeComment + 2, '');

    spanSignature(highlighter.getLineDecorations(1));
    assertEqual(calls, 2, 'downstream line cache miss should re-run Markdown.highlight once');
    const afterEditedLine = spanSignature(highlighter.getLineDecorations(0));
    assert(
      afterEditedLine !== beforeEditedLine,
      'edited line decorations should change after breaking the comment',
    );

    spanSignature(highlighter.getLineDecorations(1));
    assertEqual(calls, 2, 'post-edit repeated reads must hit the cache');
  } finally {
    highlighter.dispose();
    mutableMarkdown.highlight = originalHighlight;
  }
});

test('multi-line tokens split per line when the grammar emits them', () => {
  if (!hasTypescript) return;

  const source = 'const tpl = `hello\nworld`;';
  const baseline = Markdown.highlight(source, 'ts');
  assert(baseline !== null, 'typescript highlight should be available when supported');
  const multilineToken = baseline.tokens.find(
    (token) =>
      token.name.startsWith('string') && source.slice(token.start, token.end).includes('\n'),
  );
  if (!multilineToken) {
    console.log('bind-test-skip=multi-line token split: grammar did not emit a multi-line token');
    return;
  }

  const document = new TextDocument(source);
  const highlighter = createIncrementalHighlighter({
    document,
    language: 'ts',
    theme: { string: { color: 0x111111ff } },
  });

  try {
    const line0 = document.lineText(0);
    const line1 = document.lineText(1);
    const dec0 = highlighter.getLineDecorations(0);
    const dec1 = highlighter.getLineDecorations(1);
    assert(dec0 !== null && dec1 !== null, 'both lines should have string decorations');

    const stringStart0 = line0.indexOf('`');
    assert(stringStart0 >= 0, 'line 0 should contain opening template tick');
    assert(hasCoveredRange(dec0, stringStart0, line0.length, 0x111111ff));

    const stringEnd1 = line1.indexOf('`') + 1;
    assert(stringEnd1 > 0, 'line 1 should contain closing template tick');
    assert(hasCoveredRange(dec1, 0, stringEnd1, 0x111111ff));
  } finally {
    highlighter.dispose();
  }
});

test('unsupported languages return null for every line without throwing', () => {
  assertEqual(Markdown.highlight(' ', 'this-language-does-not-exist'), null);

  const document = new TextDocument('');
  const highlighter = createIncrementalHighlighter({
    document,
    language: 'this-language-does-not-exist',
    theme: baseTheme,
  });

  try {
    assertEqual(highlighter.getLineDecorations(0), null);
    document.replace(0, 0, 'still plain');
    assertEqual(highlighter.getLineDecorations(0), null);
  } finally {
    highlighter.dispose();
  }
});

test('empty documents, invalidate, and refill do not throw', () => {
  if (!hasTypescript) return;

  const document = new TextDocument('const x = 1;');
  const highlighter = createIncrementalHighlighter({
    document,
    language: 'typescript',
    theme: baseTheme,
  });

  try {
    assert(highlighter.getLineDecorations(0) !== null);
    document.replace(0, document.length(), '');
    assertEqual(document.lineCount(), 1);
    assertEqual(highlighter.getLineDecorations(0), null);
    assertEqual(highlighter.revision(), document.revision());

    highlighter.invalidate();
    assertEqual(highlighter.revision(), -1);
    document.replace(0, 0, 'const y = 2;');
    assert(highlighter.getLineDecorations(0) !== null);
    assertEqual(highlighter.revision(), document.revision());
  } finally {
    highlighter.dispose();
  }
});

test('dispose unsubscribes cleanly and later reads return null', () => {
  if (!hasTypescript) return;

  const document = new TextDocument('const x = 1;');
  const highlighter = createIncrementalHighlighter({
    document,
    language: 'typescript',
    theme: baseTheme,
  });

  assert(highlighter.getLineDecorations(0) !== null);
  highlighter.dispose();
  document.replace(0, 0, '// ');
  assertEqual(highlighter.getLineDecorations(0), null);
});

test('dotted capture themes prefer exact matches and fall back to the head capture', () => {
  if (!hasTypescript) return;

  const source = String.raw`const s = "\n";`;
  const baseline = Markdown.highlight(source, 'ts');
  assert(baseline !== null, 'typescript highlight should be available when supported');
  const escapeToken = baseline.tokens.find((token) => token.name === 'string.escape');
  if (!escapeToken) {
    console.log('bind-test-skip=dotted capture theme: grammar did not emit string.escape');
    return;
  }

  const document = new TextDocument(source);
  const exactHighlighter = createIncrementalHighlighter({
    document,
    language: 'ts',
    theme: {
      string: { color: 0x111111ff },
      'string.escape': { color: 0x222222ff },
    },
  });
  const fallbackHighlighter = createIncrementalHighlighter({
    document,
    language: 'ts',
    theme: {
      string: { color: 0x111111ff },
    },
  });

  try {
    const escapeColumn = escapeToken.start - document.lineRange(0).start;
    assertEqual(colorAt(exactHighlighter.getLineDecorations(0), escapeColumn), 0x222222ff);
    assertEqual(colorAt(fallbackHighlighter.getLineDecorations(0), escapeColumn), 0x111111ff);
  } finally {
    exactHighlighter.dispose();
    fallbackHighlighter.dispose();
  }
});

await run();

function spanSignature(decorations: LineDecorations | null): string {
  if (!decorations?.spans || decorations.spans.length === 0) return 'null';
  return decorations.spans
    .map(
      (span) =>
        `${span.start}:${span.end}:${span.color ?? 'none'}:${span.bold ? 'b' : ''}:${span.italic ? 'i' : ''}`,
    )
    .join('|');
}

function hasCoveredRange(
  decorations: LineDecorations | null,
  start: number,
  end: number,
  color: number,
): boolean {
  if (!decorations?.spans) return false;
  return decorations.spans.some(
    (span) => span.start <= start && span.end >= end && span.color === color,
  );
}

function colorAt(decorations: LineDecorations | null, column: number): number | undefined {
  return decorations?.spans?.find((span) => span.start <= column && column < span.end)?.color;
}
