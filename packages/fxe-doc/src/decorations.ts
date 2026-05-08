// Range-keyed decorations (diagnostics, find matches, highlights, gutter
// marks). Stored as a sorted-by-start array of {start, end, payload};
// queries use binary search for the first overlapping range and walk
// forward. Edits shift offsets through `map()` similar to selections.

import type { TextDocumentEdit } from 'fxe';

export interface Decoration<T> {
  start: number;
  end: number;
  payload: T;
}

export class Decorations<T> {
  private items: ReadonlyArray<Decoration<T>>;

  constructor(items: ReadonlyArray<Decoration<T>> = []) {
    this.items = sortAndCheck(items);
  }

  get size(): number {
    return this.items.length;
  }

  all(): ReadonlyArray<Decoration<T>> {
    return this.items;
  }

  add(start: number, end: number, payload: T): Decorations<T> {
    return new Decorations<T>([...this.items, { start, end, payload }]);
  }

  remove(predicate: (d: Decoration<T>) => boolean): Decorations<T> {
    return new Decorations<T>(this.items.filter((d) => !predicate(d)));
  }

  /** All decorations whose [start, end) intersects [start, end). */
  intersecting(start: number, end: number): Decoration<T>[] {
    if (this.items.length === 0 || start >= end) return [];
    // Binary-search the first item whose `end > start` (i.e. could intersect).
    let lo = 0;
    let hi = this.items.length;
    while (lo < hi) {
      const mid = (lo + hi) >>> 1;
      if (this.items[mid].end <= start) lo = mid + 1;
      else hi = mid;
    }
    const out: Decoration<T>[] = [];
    for (let i = lo; i < this.items.length; ++i) {
      const d = this.items[i];
      if (d.start >= end) break;
      if (d.end > start && d.start < end) out.push(d);
    }
    return out;
  }

  /** Shift decorations through edits. Decorations fully covered by a
   *  deletion are dropped; partially overlapping ones contract. */
  map(edits: ReadonlyArray<TextDocumentEdit>): Decorations<T> {
    if (edits.length === 0 || this.items.length === 0) return this;
    const out: Decoration<T>[] = [];
    for (const d of this.items) {
      const r = mapRange(d.start, d.end, edits);
      if (r) out.push({ start: r.start, end: r.end, payload: d.payload });
    }
    return new Decorations<T>(out);
  }
}

function sortAndCheck<T>(items: ReadonlyArray<Decoration<T>>): Decoration<T>[] {
  const sorted = [...items].sort((a, b) => a.start - b.start || a.end - b.end);
  for (const d of sorted) {
    if (d.end < d.start) throw new RangeError('Decoration end < start');
  }
  return sorted;
}

function mapRange(
  start: number,
  end: number,
  edits: ReadonlyArray<TextDocumentEdit>,
): { start: number; end: number } | null {
  let s = start;
  let e = end;
  let shift = 0;
  for (const ed of edits) {
    const replEnd = ed.start + ed.removed;
    // Apply shift accumulated from edits strictly before this one.
    if (replEnd <= start) {
      shift += ed.inserted.length - ed.removed;
      continue;
    }
    if (ed.start >= end) break;
    // The edit overlaps the decoration. Map start and end independently.
    s = mapOffsetThrough(start, ed, shift, 'left');
    e = mapOffsetThrough(end, ed, shift, 'right');
    shift += ed.inserted.length - ed.removed;
  }
  if (e <= s) return null; // collapsed by deletion
  return { start: s + (start === s ? shift : 0), end: e + (end === e ? shift : 0) };
}

function mapOffsetThrough(
  offset: number,
  e: TextDocumentEdit,
  shift: number,
  side: 'left' | 'right',
): number {
  const replEnd = e.start + e.removed;
  if (offset <= e.start) return offset + shift;
  if (offset >= replEnd) return offset + shift + (e.inserted.length - e.removed);
  // Inside replaced region: collapse to the boundary on the chosen side.
  return side === 'left' ? e.start + shift : e.start + e.inserted.length + shift;
}
