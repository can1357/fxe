// Undo/redo over a native TextDocument. Edits made via `dispatch()` push an
// inverse snapshot onto the undo stack; History suppresses its own
// undo/redo dispatches so listener-driven re-pushes don't loop.
//
// Undo-coalescing policy: edits made within `mergeWindowMs` of the previous
// edit and tagged with the same `origin` (e.g. `'type'` for keystrokes) fold
// into the previous undo entry, so one Cmd-Z reverts a whole word instead
// of one character.

import type { TextDocumentEdit } from 'fxe';

// `TextDocument` is exported as both a value (constructor) and a type;
// re-export under an alias so we can refer to the instance type without
// importing the constructor at runtime.
type TextDocumentLike = InstanceType<typeof TextDocument>;

export interface HistoryOptions {
  /** Max undo entries retained. Older entries drop off the bottom. */
  limit?: number;
  /** Edits within this window with the same origin merge. */
  mergeWindowMs?: number;
}

export interface DispatchOptions {
  /** Logical group (e.g. `'type'`, `'paste'`, `'replace'`). Coalescing key. */
  origin?: string;
  /** Force a new undo entry regardless of the merge window. */
  break?: boolean;
}

export interface TransactOptions {
  /** Origin recorded on the transaction's undo entry. */
  origin?: string;
}

interface Entry {
  /** Edits to *replay* this group's mutation. */
  forward: Array<{ start: number; removed: number; inserted: string }>;
  /** Edits to *undo* this group's mutation. */
  inverse: Array<{ start: number; removed: number; inserted: string }>;
  forwardBatches: Array<Array<{ start: number; removed: number; inserted: string }>>;
  inverseBatches: Array<Array<{ start: number; removed: number; inserted: string }>>;
  origin: string;
  timestamp: number;
  mergeable: boolean;
}

export class History {
  private undo_: Entry[] = [];
  private redo_: Entry[] = [];
  private limit: number;
  private mergeWindowMs: number;
  private suppress = false;
  private txDepth = 0;
  private txEntry: Entry | null = null;
  private txOrigin = 'transaction';

  constructor(
    private readonly doc: TextDocumentLike,
    opts: HistoryOptions = {},
  ) {
    this.limit = opts.limit ?? 256;
    this.mergeWindowMs = opts.mergeWindowMs ?? 500;
  }

  canUndo(): boolean {
    return this.undo_.length > 0;
  }
  canRedo(): boolean {
    return this.redo_.length > 0;
  }
  /**
   * Group every `dispatch()` made inside `fn` into a single undo entry.
   * Nested transactions flatten into the outermost group. If `fn` throws,
   * already-applied edits are still committed as one undo step so the caller
   * can undo the partial change. Async transactions stay open across awaits;
   * callers must avoid interleaving unrelated dispatches while one is open.
   */
  transact<T>(fn: () => T, opts: TransactOptions = {}): T {
    if (this.txDepth === 0) {
      this.redo_.length = 0;
      this.txOrigin = opts.origin ?? 'transaction';
      this.txEntry = {
        forward: [],
        inverse: [],
        forwardBatches: [],
        inverseBatches: [],
        origin: this.txOrigin,
        timestamp: Date.now(),
        mergeable: false,
      };
    }
    ++this.txDepth;
    let result: T;
    try {
      result = fn();
    } catch (error) {
      this.finishTransaction();
      throw error;
    }
    if (result instanceof Promise) {
      return result.finally(() => this.finishTransaction()) as T;
    }
    this.finishTransaction();
    return result;
  }

  /**
   * Apply edits and record the inverse on the undo stack. Edits MUST be
   * sorted ascending by `start` and non-overlapping (same contract as
   * TextDocument.applyBatch).
   */
  dispatch(
    edits: ReadonlyArray<{ start: number; removed: number; inserted: string }>,
    opts: DispatchOptions = {},
  ): TextDocumentEdit[] {
    if (edits.length === 0) return [];
    const origin = opts.origin ?? 'edit';
    const now = Date.now();
    this.suppress = true;
    let applied: TextDocumentEdit[];
    try {
      applied = this.doc.applyBatch(edits);
    } finally {
      this.suppress = false;
    }
    const inverse = inverseEditsFromApplied(applied);
    const entry: Entry = {
      forward: edits.map((e) => ({ ...e })),
      inverse,
      forwardBatches: [edits.map((e) => ({ ...e }))],
      inverseBatches: [inverse.map((e) => ({ ...e }))],
      origin,
      timestamp: now,
      mergeable: true,
    };
    if (this.txDepth > 0) {
      const txEntry = this.txEntry ?? {
        forward: [],
        inverse: [],
        forwardBatches: [],
        inverseBatches: [],
        origin: this.txOrigin,
        timestamp: now,
        mergeable: false,
      };
      txEntry.forward.push(...entry.forward);
      txEntry.inverse.unshift(...entry.inverse);
      txEntry.forwardBatches.push(entry.forward);
      txEntry.inverseBatches.unshift(entry.inverse.map((e) => ({ ...e })));
      txEntry.timestamp = now;
      this.txEntry = txEntry;
      return applied;
    }
    this.redo_.length = 0;
    const top = this.undo_[this.undo_.length - 1];
    if (
      !opts.break &&
      top &&
      top.mergeable &&
      top.origin === origin &&
      now - top.timestamp < this.mergeWindowMs
    ) {
      // Merge: append forward batches and keep inverse batches in reverse
      // application order so one undo reverts the whole group.
      top.forward.push(...entry.forward);
      top.inverse.unshift(...entry.inverse);
      top.forwardBatches.push(entry.forward);
      top.inverseBatches.unshift(entry.inverse.map((e) => ({ ...e })));
      top.timestamp = now;
    } else {
      this.undo_.push(entry);
      while (this.undo_.length > this.limit) this.undo_.shift();
    }
    return applied;
  }

  undo(): boolean {
    const e = this.undo_.pop();
    if (!e) return false;
    this.applyBatches(e.inverseBatches);
    this.redo_.push(e);
    return true;
  }

  redo(): boolean {
    const e = this.redo_.pop();
    if (!e) return false;
    this.applyBatches(e.forwardBatches);
    this.undo_.push(e);
    return true;
  }

  private finishTransaction(): void {
    --this.txDepth;
    if (this.txDepth !== 0) return;
    const entry = this.txEntry;
    this.txEntry = null;
    if (!entry || entry.forward.length === 0) return;
    entry.timestamp = Date.now();
    this.undo_.push(entry);
    while (this.undo_.length > this.limit) this.undo_.shift();
  }

  private applyBatches(
    batches: ReadonlyArray<ReadonlyArray<{ start: number; removed: number; inserted: string }>>,
  ): void {
    this.suppress = true;
    try {
      for (const batch of batches) this.doc.applyBatch(batch);
    } finally {
      this.suppress = false;
    }
  }

  /** Force the next dispatch to start a new undo entry. */
  breakCoalescing(): void {
    if (this.undo_.length > 0) {
      this.undo_[this.undo_.length - 1].timestamp = 0;
    }
  }

  /** Returns true while History is performing its own undo/redo, so external
   *  listeners can avoid re-recording these edits. */
  get isReplaying(): boolean {
    return this.suppress;
  }
}

function inverseEditsFromApplied(
  applied: ReadonlyArray<TextDocumentEdit>,
): Array<{ start: number; removed: number; inserted: string }> {
  // Inverse must be sorted ascending by post-edit start. Walk applied edits
  // left-to-right while accumulating the running shift; for each, the
  // inverse runs at (start + shift) and replaces inserted.length chars with
  // the previously-deleted text.
  const out: Array<{ start: number; removed: number; inserted: string }> = [];
  let shift = 0;
  for (const e of applied) {
    out.push({ start: e.start + shift, removed: e.inserted.length, inserted: e.deleted });
    shift += e.inserted.length - e.removed;
  }
  return out;
}
