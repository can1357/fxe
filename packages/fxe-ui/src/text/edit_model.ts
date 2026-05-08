export const MOD_SHIFT = 1;
export const MOD_CONTROL = 2;
export const MOD_ALT = 4;
export const MOD_SUPER = 8;

const MERGE_WINDOW_MS = 500;
const MAX_HISTORY = 200;

const WORD_CHAR_RE = /[\w]/u;
const WHITESPACE_RE = /\s/u;

export type Platform = 'macos' | 'win' | 'linux' | 'other';

export interface TextSelection {
  anchor: number;
  focus: number;
}

export interface TextEditState {
  value: string;
  selection: TextSelection;
}

export type EditKind = 'type' | 'delete' | 'paste' | 'cut' | 'ime' | 'replace';

export interface TextEditTransaction {
  before: TextEditState;
  after: TextEditState;
  kind: EditKind;
  timestamp: number;
}

export interface TextHistory {
  past: TextEditTransaction[];
  future: TextEditTransaction[];
  lastKind: EditKind | null;
  lastTimestamp: number;
}

function isWordChar(ch: string | undefined): boolean {
  return ch !== undefined && WORD_CHAR_RE.test(ch);
}

function isWhitespace(ch: string | undefined): boolean {
  return ch !== undefined && WHITESPACE_RE.test(ch);
}

export function orderedRange(start: number, end: number): [number, number] {
  return start <= end ? [start, end] : [end, start];
}

export function clampIndex(value: number, text: string): number {
  return Math.max(0, Math.min(Math.trunc(value), text.length));
}

export function wordRangeAt(text: string, idx: number): [number, number] {
  const clamped = clampIndex(idx, text);
  const atWord = isWordChar(text[clamped]);

  let start = clamped;
  while (start > 0 && isWordChar(text[start - 1]) === atWord) start -= 1;

  let end = clamped;
  while (end < text.length && isWordChar(text[end]) === atWord) end += 1;

  return [start, end];
}

export function lineRangeAt(text: string, idx: number): [number, number] {
  const clamped = clampIndex(idx, text);

  let start = 0;
  for (let i = clamped - 1; i >= 0; i--) {
    if (text[i] === '\n') {
      start = i + 1;
      break;
    }
  }

  let end = text.length;
  for (let i = clamped; i < text.length; i++) {
    if (text[i] === '\n') {
      end = i;
      break;
    }
  }

  return [start, end];
}

export function moveByWord(text: string, idx: number, direction: 1 | -1): number {
  let pos = clampIndex(idx, text);

  if (direction === 1) {
    while (pos < text.length && isWhitespace(text[pos])) pos += 1;
    while (pos < text.length && isWordChar(text[pos])) pos += 1;
    return pos;
  }

  if (pos > 0) pos -= 1;
  while (pos > 0 && isWhitespace(text[pos])) pos -= 1;
  while (pos > 0 && isWordChar(text[pos - 1])) pos -= 1;
  return clampIndex(pos, text);
}

export function moveToLineEdge(text: string, idx: number, edge: 'start' | 'end'): number {
  let pos = clampIndex(idx, text);

  if (edge === 'start') {
    while (pos > 0 && text[pos - 1] !== '\n') pos -= 1;
    return pos;
  }

  while (pos < text.length && text[pos] !== '\n') pos += 1;
  return pos;
}

export function replaceRange(state: TextEditState, insert: string): TextEditState {
  const [start, end] = orderedRange(state.selection.anchor, state.selection.focus);
  const next = state.value.slice(0, start) + insert + state.value.slice(end);
  const caret = start + insert.length;
  return {
    value: next,
    selection: { anchor: caret, focus: caret },
  };
}

export function createHistory(): TextHistory {
  return { past: [], future: [], lastKind: null, lastTimestamp: 0 };
}

export function pushTransaction(history: TextHistory, tx: TextEditTransaction): TextHistory {
  const canMerge =
    (tx.kind === 'type' || tx.kind === 'delete') &&
    history.past.length > 0 &&
    history.lastKind === tx.kind &&
    tx.timestamp - history.lastTimestamp < MERGE_WINDOW_MS;

  if (canMerge) {
    const past = history.past.slice();
    const last = past[past.length - 1];
    past[past.length - 1] = { ...last, after: tx.after };
    return {
      past,
      future: [],
      lastKind: history.lastKind,
      lastTimestamp: tx.timestamp,
    };
  }

  const appended = [...history.past, tx];
  const past =
    appended.length > MAX_HISTORY ? appended.slice(appended.length - MAX_HISTORY) : appended;

  return {
    past,
    future: [],
    lastKind: tx.kind,
    lastTimestamp: tx.timestamp,
  };
}

export function undo(
  state: TextEditState,
  history: TextHistory,
): { state: TextEditState; history: TextHistory } | null {
  void state;
  if (history.past.length === 0) return null;

  const past = history.past.slice(0, -1);
  const tx = history.past[history.past.length - 1];
  const future = [...history.future, tx];

  return {
    state: tx.before,
    history: {
      past,
      future,
      lastKind: null,
      lastTimestamp: 0,
    },
  };
}

export function redo(
  state: TextEditState,
  history: TextHistory,
): { state: TextEditState; history: TextHistory } | null {
  void state;
  if (history.future.length === 0) return null;

  const future = history.future.slice(0, -1);
  const tx = history.future[history.future.length - 1];
  const past = [...history.past, tx];

  return {
    state: tx.after,
    history: {
      past,
      future,
      lastKind: tx.kind,
      lastTimestamp: tx.timestamp,
    },
  };
}

export function detectPlatform(): Platform {
  const processPlatform = (globalThis as { process?: { platform?: string } }).process?.platform;
  if (processPlatform === 'darwin') return 'macos';
  if (processPlatform === 'win32') return 'win';
  if (processPlatform === 'linux') return 'linux';

  const navPlatform = (globalThis as { navigator?: { platform?: string } }).navigator?.platform;
  if (typeof navPlatform === 'string') {
    const lower = navPlatform.toLowerCase();
    if (lower.includes('mac')) return 'macos';
    if (lower.includes('win')) return 'win';
    if (lower.includes('linux')) return 'linux';
  }

  return 'other';
}

export function isPrimaryModifier(
  modifiers: number,
  platform: Platform = detectPlatform(),
): boolean {
  if (platform === 'macos') return (modifiers & MOD_SUPER) !== 0;
  return (modifiers & MOD_CONTROL) !== 0;
}

export function isWordJumpModifier(
  modifiers: number,
  platform: Platform = detectPlatform(),
): boolean {
  if (platform === 'macos') return (modifiers & MOD_ALT) !== 0;
  return (modifiers & MOD_CONTROL) !== 0;
}

export function isLineJumpModifier(
  modifiers: number,
  platform: Platform = detectPlatform(),
): boolean {
  if (platform === 'macos') return (modifiers & MOD_SUPER) !== 0;
  return (modifiers & MOD_CONTROL) !== 0;
}
