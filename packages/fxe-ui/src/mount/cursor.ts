import type { CursorKind, Window } from 'fxe';

let current: CursorKind | null = null;

export function setCursor(win: Pick<Window, 'setCursor'>, cursor: CursorKind): void {
  if (cursor === current) return;
  current = cursor;
  win.setCursor(cursor);
}
