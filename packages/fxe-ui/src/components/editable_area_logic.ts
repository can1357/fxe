import type { MultiRangeSelection, Range } from 'fxe-doc';

type DocumentLike = InstanceType<typeof TextDocument>;

type PendingEdit = {
  rangeIndex: number;
  start: number;
  removed: number;
  inserted: string;
};

type TextEdit = { start: number; removed: number; inserted: string };

type PendingRangeEdit = {
  rangeIndex: number;
  rangeStart: number;
  edit: TextEdit | null;
  nextRange: Range;
};

export const BRACKET_PAIRS: ReadonlyArray<readonly [string, string]> = [
  ['(', ')'],
  ['[', ']'],
  ['{', '}'],
  ['"', '"'],
  ["'", "'"],
  ['`', '`'],
];

export interface BracketContextProvider {
  /** Return true if offset `off` is inside a string or comment, where autoclose+match should be suppressed. Optional — when undefined, treated as code. */
  isStringOrComment?(off: number): boolean;
}

export interface AutocloseOutcome {
  /** Edits to dispatch through History; pre-sorted ascending. */
  edits: Array<{ start: number; removed: number; inserted: string }>;
  /** New selection after edits applied. */
  nextSel: MultiRangeSelection;
  /** True if the typed character was consumed (caller must NOT also dispatch the raw char). */
  handled: boolean;
}

export interface IndentInference {
  /** 'tab' or number of spaces. */
  unit: 'tab' | number;
  /** Confidence 0..1 — useful so caller can decide whether to override config. */
  confidence: number;
}

const OPEN_TO_CLOSE = new Map(BRACKET_PAIRS);
const CLOSE_TO_OPEN = new Map(BRACKET_PAIRS.map(([open, close]) => [close, open] as const));
const QUOTES = new Set(['"', "'", '`']);

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

/** Try to autoclose or step-over the typed character at every cursor.
 *  Behavior:
 *   - Typing the OPEN of a pair at a collapsed cursor inserts both halves and places caret between them.
 *   - Typing the OPEN with a non-empty selection wraps the selection in the pair.
 *   - Typing the CLOSE of a pair when the next char is already that close: step over (no insert; advance caret).
 *   - Quotes only autoclose at collapsed cursors when the next char is whitespace/EOL/punctuation OR the prior char is whitespace/SOL/punctuation; otherwise fall through to plain insert.
 *   - In string/comment context (per provider), autoclose is suppressed for ALL pairs except matching backticks inside template literals — keep simple: just suppress entirely. The plain character still types.
 *  Returns handled=false when no cursor needed pair handling — caller should fall back to plain `applyEditsAtRanges`.
 */
export function autocloseTyped(
  doc: DocumentLike,
  sel: MultiRangeSelection,
  ch: string,
  provider?: BracketContextProvider,
): AutocloseOutcome {
  if (ch.length !== 1) {
    return { edits: [], nextSel: sel, handled: false };
  }

  const open = OPEN_TO_CLOSE.get(ch);
  const close = CLOSE_TO_OPEN.get(ch);
  if (!open && !close) {
    return { edits: [], nextSel: sel, handled: false };
  }

  let handled = false;
  const pending: PendingRangeEdit[] = [];
  for (const [rangeIndex, range] of sel.ranges.entries()) {
    const { start, end } = orderedRange(range);
    const collapsed = start === end;
    const inSuppressedContext = provider?.isStringOrComment?.(start) === true;
    const nextChar = doc.slice(range.focus, range.focus + 1);

    if (!inSuppressedContext && close && collapsed && nextChar === ch) {
      handled = true;
      pending.push({
        rangeIndex,
        rangeStart: start,
        edit: null,
        nextRange: { anchor: range.focus + 1, focus: range.focus + 1 },
      });
      continue;
    }

    if (!inSuppressedContext && open) {
      if (!collapsed) {
        handled = true;
        pending.push(wrapRangeEdit(doc, rangeIndex, range, ch, open));
        continue;
      }
      if (!QUOTES.has(ch) || shouldAutocloseQuote(doc, range.focus)) {
        handled = true;
        pending.push(insertPairEdit(rangeIndex, range.focus, ch, open));
        continue;
      }
    }

    pending.push(plainTypeEdit(rangeIndex, range, ch));
  }

  if (!handled) {
    return { edits: [], nextSel: sel, handled: false };
  }

  const editsAsc = pending
    .filter((entry): entry is PendingRangeEdit & { edit: TextEdit } => entry.edit !== null)
    .sort((a, b) =>
      a.edit.start === b.edit.start ? a.rangeIndex - b.rangeIndex : a.edit.start - b.edit.start,
    )
    .map((entry) => entry.edit);

  const nextRanges: Range[] = new Array(sel.ranges.length);
  let shift = 0;
  for (const entry of pending
    .slice()
    .sort((a, b) => a.rangeStart - b.rangeStart || a.rangeIndex - b.rangeIndex)) {
    nextRanges[entry.rangeIndex] = shiftRange(entry.nextRange, shift);
    if (entry.edit) {
      shift += entry.edit.inserted.length - entry.edit.removed;
    }
  }

  return {
    edits: editsAsc,
    nextSel: sel.with(nextRanges, sel.primary),
    handled: true,
  };
}

/** Find the matching bracket position for the bracket at `offset`, or null.
 *  Walks forward for opens, backward for closes, balancing same-type pairs. Skips brackets inside strings/comments per provider.
 *  Off must point AT a bracket character (use `findBracketAt` helper or pass offset of the char). */
export function findMatchingBracket(
  doc: DocumentLike,
  offset: number,
  provider?: BracketContextProvider,
): { open: number; close: number } | null {
  if (offset < 0 || offset >= doc.length()) return null;
  if (provider?.isStringOrComment?.(offset) === true) return null;
  const ch = doc.slice(offset, offset + 1);
  const open = OPEN_TO_CLOSE.get(ch);
  const close = CLOSE_TO_OPEN.get(ch);
  if (!open && !close) return null;

  if (QUOTES.has(ch)) {
    return findMatchingQuote(doc, offset, ch, provider);
  }
  if (open) {
    let depth = 1;
    for (let cursor = offset + 1; cursor < doc.length(); ++cursor) {
      if (provider?.isStringOrComment?.(cursor) === true) continue;
      const here = doc.slice(cursor, cursor + 1);
      if (here === ch) {
        depth += 1;
      } else if (here === open) {
        depth -= 1;
        if (depth === 0) return { open: offset, close: cursor };
      }
    }
    return null;
  }
  if (!close) return null;
  let depth = 1;
  for (let cursor = offset - 1; cursor >= 0; --cursor) {
    if (provider?.isStringOrComment?.(cursor) === true) continue;
    const here = doc.slice(cursor, cursor + 1);
    if (here === ch) {
      depth += 1;
    } else if (here === close) {
      depth -= 1;
      if (depth === 0) return { open: cursor, close: offset };
    }
  }
  return null;
}

/** Sample up to `maxLines` non-empty lines and infer the dominant indent unit.
 *  Algorithm:
 *   - For each line: count leading tabs (T) and leading spaces (S).
 *   - If T > 0 and S == 0 → vote 'tab'.
 *   - If S > 0 and T == 0 → record S; the dominant gcd of {2,4,8} that divides the smallest non-zero S is the spaces width (prefer 2 over 4 over 8).
 *   - Confidence = winning_votes / total_voted_lines, clamped to [0,1].
 *  Returns { unit: 2, confidence: 0 } when no votes. */
export function inferIndent(doc: DocumentLike, maxLines: number = 200): IndentInference {
  let tabVotes = 0;
  const spaceVotes = new Map<number, number>([
    [2, 0],
    [4, 0],
    [8, 0],
  ]);
  let totalVotes = 0;
  const limit = Math.min(doc.lineCount(), Math.max(0, maxLines));

  for (let line = 0; line < limit; ++line) {
    const text = lineText(doc, line);
    if (text.trim().length === 0) continue;
    const { tabs, spaces } = leadingIndent(text);
    if (tabs > 0 && spaces === 0) {
      tabVotes += 1;
      totalVotes += 1;
      continue;
    }
    if (spaces > 0 && tabs === 0) {
      totalVotes += 1;
      for (const width of [2, 4, 8] as const) {
        if (spaces % width === 0) {
          spaceVotes.set(width, (spaceVotes.get(width) ?? 0) + 1);
        }
      }
    }
  }

  if (totalVotes === 0) {
    return { unit: 2, confidence: 0 };
  }

  let unit: 'tab' | number = 'tab';
  let winningVotes = tabVotes;
  for (const width of [2, 4, 8] as const) {
    const votes = spaceVotes.get(width) ?? 0;
    if (votes > winningVotes) {
      unit = width;
      winningVotes = votes;
    }
  }
  if (winningVotes === 0) {
    return { unit: 2, confidence: 0 };
  }

  return {
    unit,
    confidence: Math.max(0, Math.min(1, winningVotes / totalVotes)),
  };
}

/** Indent every line touched by every range by one unit (`unit`).
 *  - With `unit === 'tab'`: prefix each line with '\t'.
 *  - With `unit === N` (spaces): prefix each line with N space characters.
 *  Returns a batch of edits in ASCENDING start order plus the remapped selection. */
export function indentSelection(
  doc: DocumentLike,
  sel: MultiRangeSelection,
  unit: 'tab' | number,
): {
  edits: Array<{ start: number; removed: number; inserted: string }>;
  nextSel: MultiRangeSelection;
} {
  const indentText = unit === 'tab' ? '\t' : ' '.repeat(unit);
  const mapped = touchedLines(doc, sel).map((line) => ({
    start: doc.lineToOffset(line),
    removed: 0,
    inserted: indentText,
    deleted: '',
  }));
  return {
    edits: mapped.map(({ start, removed, inserted }) => ({ start, removed, inserted })),
    nextSel: sel.map(mapped, 'right'),
  };
}

/** Outdent: opposite of indentSelection. For each touched line, strip up to one unit of leading indentation:
 *  - 'tab': remove a single leading '\t' (no-op if line starts with spaces).
 *  - N spaces: remove up to N leading spaces (stop at first non-space). Lines with mixed leading whitespace are normalized: only strip the first contiguous run of matching whitespace. */
export function outdentSelection(
  doc: DocumentLike,
  sel: MultiRangeSelection,
  unit: 'tab' | number,
): {
  edits: Array<{ start: number; removed: number; inserted: string }>;
  nextSel: MultiRangeSelection;
} {
  const mapped = touchedLines(doc, sel)
    .map((line) => {
      const start = doc.lineToOffset(line);
      const text = lineText(doc, line);
      const removed = unit === 'tab' ? removeLeadingTab(text) : removeLeadingSpaces(text, unit);
      return {
        start,
        removed,
        inserted: '',
        deleted: removed > 0 ? doc.slice(start, start + removed) : '',
      };
    })
    .filter((edit) => edit.removed > 0);
  return {
    edits: mapped.map(({ start, removed, inserted }) => ({ start, removed, inserted })),
    nextSel: sel.map(mapped, 'right'),
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

function wrapRangeEdit(
  doc: DocumentLike,
  rangeIndex: number,
  range: Range,
  open: string,
  close: string,
): PendingRangeEdit {
  const { start, end } = orderedRange(range);
  const middle = doc.slice(start, end);
  const innerStart = start + open.length;
  const innerEnd = innerStart + middle.length;
  return {
    rangeIndex,
    rangeStart: start,
    edit: { start, removed: end - start, inserted: `${open}${middle}${close}` },
    nextRange:
      range.anchor <= range.focus
        ? { anchor: innerStart, focus: innerEnd }
        : { anchor: innerEnd, focus: innerStart },
  };
}

function insertPairEdit(
  rangeIndex: number,
  offset: number,
  open: string,
  close: string,
): PendingRangeEdit {
  return {
    rangeIndex,
    rangeStart: offset,
    edit: { start: offset, removed: 0, inserted: `${open}${close}` },
    nextRange: { anchor: offset + open.length, focus: offset + open.length },
  };
}

function plainTypeEdit(rangeIndex: number, range: Range, ch: string): PendingRangeEdit {
  const { start, end } = orderedRange(range);
  const nextOffset = start + ch.length;
  return {
    rangeIndex,
    rangeStart: start,
    edit: { start, removed: end - start, inserted: ch },
    nextRange: { anchor: nextOffset, focus: nextOffset },
  };
}

function shiftRange(range: Range, shift: number): Range {
  return {
    anchor: range.anchor + shift,
    focus: range.focus + shift,
  };
}

function shouldAutocloseQuote(doc: DocumentLike, offset: number): boolean {
  const prev = offset > 0 ? doc.slice(offset - 1, offset) : '';
  const next = offset < doc.length() ? doc.slice(offset, offset + 1) : '';
  return isBoundaryChar(prev) || isBoundaryChar(next);
}

function isBoundaryChar(ch: string): boolean {
  if (ch.length === 0) return true;
  if (ch === '\n' || ch === '\r' || ch === '\t' || ch === ' ') return true;
  return isAsciiPunctuation(ch);
}

function isAsciiPunctuation(ch: string): boolean {
  if (ch.length !== 1) return false;
  const code = ch.charCodeAt(0);
  if (code >= 48 && code <= 57) return false;
  if (code >= 65 && code <= 90) return false;
  if (code >= 97 && code <= 122) return false;
  if (code === 95) return false;
  return code >= 33 && code <= 126;
}

function findMatchingQuote(
  doc: DocumentLike,
  offset: number,
  quote: string,
  provider?: BracketContextProvider,
): { open: number; close: number } | null {
  const positions: number[] = [];
  for (let cursor = 0; cursor < doc.length(); ++cursor) {
    if (provider?.isStringOrComment?.(cursor) === true) continue;
    if (doc.slice(cursor, cursor + 1) === quote) positions.push(cursor);
  }
  const index = positions.indexOf(offset);
  if (index < 0) return null;
  if (index % 2 === 0) {
    const close = positions[index + 1];
    return close === undefined ? null : { open: offset, close };
  }
  const open = positions[index - 1];
  return open === undefined ? null : { open, close: offset };
}

function touchedLines(doc: DocumentLike, sel: MultiRangeSelection): number[] {
  const lines = new Set<number>();
  for (const range of sel.ranges) {
    const { start, end } = orderedRange(range);
    const startLine = doc.offsetToLine(start);
    let inclusiveEnd = end;
    if (end > start) {
      const endLine = doc.offsetToLine(end);
      if (doc.lineToOffset(endLine) === end) {
        inclusiveEnd = end - 1;
      }
    }
    const endLine = doc.offsetToLine(Math.max(start, Math.min(doc.length(), inclusiveEnd)));
    for (let line = startLine; line <= endLine; ++line) {
      lines.add(line);
    }
  }
  return [...lines].sort((a, b) => a - b);
}

function lineText(doc: DocumentLike, line: number): string {
  const range = doc.lineRange(line);
  return doc.slice(range.start, range.end);
}

function leadingIndent(text: string): { tabs: number; spaces: number } {
  let tabs = 0;
  let spaces = 0;
  let cursor = 0;
  while (cursor < text.length && text[cursor] === '\t') {
    tabs += 1;
    cursor += 1;
  }
  while (cursor < text.length && text[cursor] === ' ') {
    spaces += 1;
    cursor += 1;
  }
  return { tabs, spaces };
}

function removeLeadingTab(text: string): number {
  return text.startsWith('\t') ? 1 : 0;
}

function removeLeadingSpaces(text: string, unit: number): number {
  let removed = 0;
  while (removed < unit && removed < text.length && text[removed] === ' ') {
    removed += 1;
  }
  return removed;
}
