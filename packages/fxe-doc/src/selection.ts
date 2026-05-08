// Multi-range selection over a TextDocument. Each range carries an anchor
// and focus offset (UTF-16 code units). `primary` indexes the "real" cursor
// — the one whose viewport must be kept visible after edits.

import type { TextDocumentEdit } from 'fxe';

export interface Range {
  anchor: number;
  focus: number;
}

export class MultiRangeSelection {
  readonly ranges: ReadonlyArray<Range>;
  readonly primary: number;

  constructor(ranges: ReadonlyArray<Range>, primary: number = ranges.length - 1) {
    if (ranges.length === 0) {
      this.ranges = [{ anchor: 0, focus: 0 }];
      this.primary = 0;
    } else {
      this.ranges = normalize(ranges);
      this.primary = Math.min(Math.max(primary, 0), this.ranges.length - 1);
    }
  }

  static cursor(offset: number): MultiRangeSelection {
    return new MultiRangeSelection([{ anchor: offset, focus: offset }], 0);
  }

  primaryRange(): Range {
    return this.ranges[this.primary];
  }

  with(ranges: ReadonlyArray<Range>, primary?: number): MultiRangeSelection {
    return new MultiRangeSelection(ranges, primary ?? this.primary);
  }

  /** Add a cursor; returns a new selection with the new range as primary. */
  add(range: Range): MultiRangeSelection {
    return new MultiRangeSelection([...this.ranges, range], this.ranges.length);
  }

  /** Collapse all but the primary cursor. */
  collapseToPrimary(): MultiRangeSelection {
    return new MultiRangeSelection([this.primaryRange()], 0);
  }

  /**
   * Shift this selection through a batch of applied edits. `bias` controls
   * what happens to a cursor that lies exactly at the start of a replaced
   * region: `'right'` keeps it after the inserted text (typing behaviour);
   * `'left'` keeps it at the original position (bookmark behaviour).
   */
  map(
    edits: ReadonlyArray<TextDocumentEdit>,
    bias: 'left' | 'right' = 'right',
  ): MultiRangeSelection {
    if (edits.length === 0) return this;
    const next: Range[] = this.ranges.map((r) => ({
      anchor: mapOffset(r.anchor, edits, bias),
      focus: mapOffset(r.focus, edits, bias),
    }));
    return new MultiRangeSelection(next, this.primary);
  }
}

function normalize(ranges: ReadonlyArray<Range>): Range[] {
  // Keep insertion order (so `primary` stays stable) but merge ranges that
  // touch or overlap. Two ranges overlap when their min/max intervals
  // intersect.
  const sorted = ranges
    .map((r, i) => ({ r, i }))
    .sort((a, b) => Math.min(a.r.anchor, a.r.focus) - Math.min(b.r.anchor, b.r.focus));
  const out: Range[] = [];
  for (const { r } of sorted) {
    const lo = Math.min(r.anchor, r.focus);
    const hi = Math.max(r.anchor, r.focus);
    const last = out[out.length - 1];
    if (last) {
      const llo = Math.min(last.anchor, last.focus);
      const lhi = Math.max(last.anchor, last.focus);
      if (lo <= lhi) {
        // Merge — preserve direction of the later range.
        const newLo = Math.min(llo, lo);
        const newHi = Math.max(lhi, hi);
        if (r.anchor <= r.focus) {
          out[out.length - 1] = { anchor: newLo, focus: newHi };
        } else {
          out[out.length - 1] = { anchor: newHi, focus: newLo };
        }
        continue;
      }
    }
    out.push({ anchor: r.anchor, focus: r.focus });
  }
  return out;
}

function mapOffset(
  offset: number,
  edits: ReadonlyArray<TextDocumentEdit>,
  bias: 'left' | 'right',
): number {
  let shift = 0;
  for (const e of edits) {
    if (offset < e.start) break;
    const replEnd = e.start + e.removed;
    if (offset === e.start) {
      return bias === 'left' ? offset + shift : e.start + e.inserted.length + shift;
    }
    if (offset <= replEnd) {
      return bias === 'left' ? e.start + shift : e.start + e.inserted.length + shift;
    }
    shift += e.inserted.length - e.removed;
  }
  return offset + shift;
}
