// fxe-doc: editor-grade document helpers built on top of the native
// `TextDocument` exposed by fxe. The native document owns the rope and
// per-line index; these helpers provide the JS-side glue an editor needs:
// undo/redo with merge windows, multi-cursor selections, and range-keyed
// decorations (diagnostics, find matches, gutter marks).

export { History } from './history.ts';
export type { HistoryOptions, DispatchOptions } from './history.ts';
export { MultiRangeSelection } from './selection.ts';
export type { Range } from './selection.ts';
export { Decorations } from './decorations.ts';
export type { Decoration } from './decorations.ts';
