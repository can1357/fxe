import type { MultiRangeSelection, Range } from 'fxe-doc';

type DocumentLike = InstanceType<typeof TextDocument>;

type PendingEdit = {
  rangeIndex: number;
  start: number;
  removed: number;
  inserted: string;
};

export function applyEditsAtRanges(
  doc: DocumentLike,
  sel: MultiRangeSelection,
  text: string,
  origin: string,
): {
  edits: Array<{ start: number; removed: number; inserted: string }>;
  nextSel: MultiRangeSelection;
} {
  const pending: PendingEdit[] = [];
  for (const [rangeIndex, range] of sel.ranges.entries()) {
    const { start, end } = orderedRange(range);
    if (origin === 'delete-backward') {
      if (start !== end) {
        pending.push({ rangeIndex, start, removed: end - start, inserted: '' });
      } else if (start > 0) {
        pending.push({ rangeIndex, start: start - 1, removed: 1, inserted: '' });
      }
      continue;
    }
    if (origin === 'delete-forward') {
      if (start !== end) {
        pending.push({ rangeIndex, start, removed: end - start, inserted: '' });
      } else if (start < doc.length()) {
        pending.push({ rangeIndex, start, removed: 1, inserted: '' });
      }
      continue;
    }
    pending.push({ rangeIndex, start, removed: end - start, inserted: text });
  }

  const editsAsc = pending
    .slice()
    .sort((a, b) => (a.start === b.start ? a.rangeIndex - b.rangeIndex : a.start - b.start))
    .map(({ start, removed, inserted }) => ({
      start,
      removed,
      inserted,
      deleted: removed > 0 ? doc.slice(start, start + removed) : '',
    }));
  return {
    edits: editsAsc.map(({ start, removed, inserted }) => ({ start, removed, inserted })),
    // History.dispatch() and TextDocument.applyBatch() both require ascending
    // edits. Mapping the original selection through that batch with
    // `bias: 'right'` lands each edited range at the replacement end while
    // accounting for cumulative shift from earlier edits in the batch.
    nextSel: sel.map(editsAsc, 'right'),
  };
}

export function expandToWord(doc: DocumentLike, sel: MultiRangeSelection): MultiRangeSelection {
  const text = doc.text();
  const nextRanges = sel.ranges.map((range) => {
    const { start, end } = orderedRange(range);
    if (start !== end) return range;
    const expanded = wordRangeAt(text, start);
    return expanded ?? range;
  });
  return sel.with(nextRanges, sel.primary);
}

export function expandLines(doc: DocumentLike, sel: MultiRangeSelection): MultiRangeSelection {
  const lastLine = Math.max(0, doc.lineCount() - 1);
  const nextRanges = sel.ranges.map((range) => {
    const { start, end } = orderedRange(range);
    const startLine = doc.offsetToLine(start);
    const inclusiveEnd = end > start ? end - 1 : end;
    const endLine = doc.offsetToLine(Math.min(doc.length(), inclusiveEnd));
    const lineStart = doc.lineToOffset(startLine);
    const lineEnd = endLine >= lastLine ? doc.length() : doc.lineToOffset(endLine + 1);
    return { anchor: lineStart, focus: lineEnd };
  });
  return sel.with(nextRanges, sel.primary);
}

export function addNextOccurrence(
  doc: DocumentLike,
  sel: MultiRangeSelection,
): MultiRangeSelection {
  const primary = sel.primaryRange();
  let base = sel;
  let primaryRange = orderedRange(primary);
  if (primaryRange.start === primaryRange.end) {
    base = expandPrimaryToWord(doc, sel);
    primaryRange = orderedRange(base.primaryRange());
    if (primaryRange.start === primaryRange.end) return sel;
  }

  const needle = doc.slice(primaryRange.start, primaryRange.end);
  if (needle.length === 0) return base;
  const existing = new Set(
    base.ranges.map((range) => `${orderedRange(range).start}:${orderedRange(range).end}`),
  );
  for (const match of findLiteralMatches(doc, needle, primaryRange.end)) {
    const key = `${match.start}:${match.end}`;
    if (existing.has(key)) continue;
    return base.add({ anchor: match.start, focus: match.end });
  }
  return base;
}

function expandPrimaryToWord(doc: DocumentLike, sel: MultiRangeSelection): MultiRangeSelection {
  const text = doc.text();
  const nextRanges = sel.ranges.map((range, index) => {
    if (index !== sel.primary) return range;
    const expanded = wordRangeAt(text, range.focus);
    return expanded ?? range;
  });
  return sel.with(nextRanges, sel.primary);
}

function findLiteralMatches(
  doc: DocumentLike,
  needle: string,
  from: number,
): Array<{ start: number; end: number }> {
  const searchLiteral = (
    doc as {
      searchLiteral?: (
        needle: string,
        opts?: { from?: number; limit?: number; caseInsensitive?: boolean },
      ) => Array<{ start: number; end: number }>;
    }
  ).searchLiteral;
  if (typeof searchLiteral === 'function') {
    const matches = doc.searchLiteral(needle, { from });
    if (matches.length > 0) return matches;
  }
  const text = doc.text();
  const out: Array<{ start: number; end: number }> = [];
  let offset = from;
  while (offset <= text.length - needle.length) {
    const at = text.indexOf(needle, offset);
    if (at < 0) break;
    out.push({ start: at, end: at + needle.length });
    offset = at + Math.max(needle.length, 1);
  }
  return out;
}

function wordRangeAt(text: string, offset: number): Range | null {
  if (text.length === 0) return null;
  let cursor = Math.max(0, Math.min(text.length, offset));
  if (!isWordChar(text[cursor]) && cursor > 0 && isWordChar(text[cursor - 1])) {
    cursor -= 1;
  }
  if (!isWordChar(text[cursor])) return null;
  let start = cursor;
  let end = cursor + 1;
  while (start > 0 && isWordChar(text[start - 1])) start -= 1;
  while (end < text.length && isWordChar(text[end])) end += 1;
  return { anchor: start, focus: end };
}

function isWordChar(ch: string | undefined): boolean {
  return ch !== undefined && /\w/.test(ch);
}

function orderedRange(range: Range): { start: number; end: number } {
  return range.anchor <= range.focus
    ? { start: range.anchor, end: range.focus }
    : { start: range.focus, end: range.anchor };
}
