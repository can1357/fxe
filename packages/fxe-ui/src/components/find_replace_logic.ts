type DocumentLike = Pick<
  InstanceType<typeof TextDocument>,
  'applyBatch' | 'revision' | 'searchLiteral' | 'text'
>;

export interface FindReplaceRange {
  start: number;
  end: number;
}

export interface FindReplaceActiveMatch extends FindReplaceRange {
  index: number;
  total: number;
}

export interface FindReplaceSearchState {
  revision: number;
  query: string;
  caseSensitive: boolean;
  useRegex: boolean;
  regexFlags: string;
  ranges: FindReplaceRange[];
  aborted: boolean;
  error: string | null;
}

export interface FindReplaceSearchOptions {
  query: string;
  caseSensitive?: boolean;
  useRegex?: boolean;
  regexFlags?: string;
  searchDeadlineMs?: number;
  searchMaxMatches?: number;
}

export interface FindReplaceEdit {
  start: number;
  removed: number;
  inserted: string;
}

export type FindReplaceDispatch = (
  edits: Array<FindReplaceEdit>,
  opts?: { origin?: string },
) => void;

const DEFAULT_SEARCH_DEADLINE_MS = 5000;
const DEFAULT_SEARCH_MAX_MATCHES = 50_000;

export function dedupeRegexFlags(flags: string): string {
  let out = '';
  const seen = new Set<string>();
  for (const ch of flags) {
    if (seen.has(ch)) continue;
    seen.add(ch);
    out += ch;
  }
  return out;
}

export function resolveFindReplaceState(
  document: DocumentLike,
  options: FindReplaceSearchOptions,
  cached?: FindReplaceSearchState | null,
): FindReplaceSearchState {
  const query = options.query;
  const caseSensitive = options.caseSensitive ?? false;
  const useRegex = options.useRegex ?? false;
  const regexFlags = options.regexFlags ?? '';
  const revision = document.revision();
  if (
    cached &&
    cached.revision === revision &&
    cached.query === query &&
    cached.caseSensitive === caseSensitive &&
    cached.useRegex === useRegex &&
    cached.regexFlags === regexFlags
  ) {
    return cached;
  }
  if (query.length === 0) {
    return {
      revision,
      query,
      caseSensitive,
      useRegex,
      regexFlags,
      ranges: [],
      aborted: false,
      error: null,
    };
  }
  return useRegex
    ? searchRegex(document, {
        query,
        caseSensitive,
        regexFlags,
        searchDeadlineMs: options.searchDeadlineMs,
        searchMaxMatches: options.searchMaxMatches,
      })
    : searchLiteral(document, { query, caseSensitive, regexFlags });
}

export function getActiveMatch(
  state: Pick<FindReplaceSearchState, 'ranges'>,
  activeIndex: number,
): FindReplaceActiveMatch | null {
  if (state.ranges.length === 0) return null;
  const index = clampActiveIndex(activeIndex, state.ranges.length);
  const range = state.ranges[index];
  return { ...range, index, total: state.ranges.length };
}

export function nextActiveIndex(activeIndex: number, total: number): number {
  if (total <= 0) return 0;
  const index = clampActiveIndex(activeIndex, total);
  return (index + 1) % total;
}

export function prevActiveIndex(activeIndex: number, total: number): number {
  if (total <= 0) return 0;
  const index = clampActiveIndex(activeIndex, total);
  return (index - 1 + total) % total;
}

export function clampActiveIndex(activeIndex: number, total: number): number {
  if (total <= 0) return 0;
  if (!Number.isFinite(activeIndex)) return 0;
  return Math.max(0, Math.min(Math.trunc(activeIndex), total - 1));
}

export function buildReplaceCurrentEdit(
  state: Pick<FindReplaceSearchState, 'ranges'>,
  activeIndex: number,
  replacement: string,
): FindReplaceEdit | null {
  const match = getActiveMatch(state, activeIndex);
  if (!match) return null;
  return { start: match.start, removed: match.end - match.start, inserted: replacement };
}

export function buildReplaceAllEdits(
  state: Pick<FindReplaceSearchState, 'ranges'>,
  replacement: string,
): Array<FindReplaceEdit> {
  return state.ranges.map((range) => ({
    start: range.start,
    removed: range.end - range.start,
    inserted: replacement,
  }));
}

export function findNextIndexAfterReplacement(
  state: Pick<FindReplaceSearchState, 'ranges'>,
  replacementStart: number,
  replacement: string,
): number {
  if (state.ranges.length === 0) return 0;
  const targetOffset = replacementStart + replacement.length;
  const next = state.ranges.findIndex((range) => range.start >= targetOffset);
  if (next >= 0) return next;
  return state.ranges.length - 1;
}

export function applyFindReplaceEdits(
  document: DocumentLike,
  edits: Array<FindReplaceEdit>,
  dispatch?: FindReplaceDispatch,
): void {
  if (edits.length === 0) return;
  const ascending = edits
    .slice()
    .sort((a, b) => (a.start === b.start ? a.removed - b.removed : a.start - b.start));
  if (dispatch) {
    dispatch(ascending, { origin: 'find-replace' });
    return;
  }
  document.applyBatch(ascending);
}

function searchLiteral(
  document: DocumentLike,
  options: Required<Pick<FindReplaceSearchOptions, 'query'>> &
    Pick<FindReplaceSearchOptions, 'caseSensitive' | 'regexFlags'>,
): FindReplaceSearchState {
  return {
    revision: document.revision(),
    query: options.query,
    caseSensitive: options.caseSensitive ?? false,
    useRegex: false,
    regexFlags: options.regexFlags ?? '',
    ranges: document.searchLiteral(options.query, {
      from: 0,
      limit: 0xffffffff,
      caseInsensitive: !(options.caseSensitive ?? false),
    }),
    aborted: false,
    error: null,
  };
}

function searchRegex(
  document: DocumentLike,
  options: Required<Pick<FindReplaceSearchOptions, 'query'>> &
    Pick<
      FindReplaceSearchOptions,
      'caseSensitive' | 'regexFlags' | 'searchDeadlineMs' | 'searchMaxMatches'
    >,
): FindReplaceSearchState {
  const revision = document.revision();
  const caseSensitive = options.caseSensitive ?? false;
  const flags = dedupeRegexFlags(`g${caseSensitive ? '' : 'i'}${options.regexFlags ?? ''}`);
  let re: RegExp;
  try {
    re = new RegExp(options.query, flags);
  } catch (error) {
    return {
      revision,
      query: options.query,
      caseSensitive,
      useRegex: true,
      regexFlags: options.regexFlags ?? '',
      ranges: [],
      aborted: false,
      error: error instanceof Error ? error.message : String(error),
    };
  }
  const text = document.text();
  const deadlineMs = options.searchDeadlineMs ?? DEFAULT_SEARCH_DEADLINE_MS;
  const maxMatches = options.searchMaxMatches ?? DEFAULT_SEARCH_MAX_MATCHES;
  const startedAt = performance.now();
  const ranges: FindReplaceRange[] = [];
  let aborted = false;
  while (true) {
    if (ranges.length >= maxMatches || performance.now() - startedAt >= deadlineMs) {
      aborted = true;
      break;
    }
    const match = re.exec(text);
    if (!match) break;
    const start = match.index;
    const consumed = match[0]?.length ?? 0;
    const end = start + consumed;
    ranges.push({ start, end });
    if (consumed === 0) {
      if (re.lastIndex >= text.length) break;
      re.lastIndex = re.lastIndex + 1;
    }
  }
  return {
    revision,
    query: options.query,
    caseSensitive,
    useRegex: true,
    regexFlags: options.regexFlags ?? '',
    ranges,
    aborted,
    error: null,
  };
}
