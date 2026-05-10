import { MultiRangeSelection, type Range } from 'fxe-doc';

type DocumentLike = InstanceType<typeof TextDocument>;

type LineCol = {
  line: number;
  col: number;
};

export function clampLineCol(doc: DocumentLike, line: number, col: number): LineCol {
  const maxLine = Math.max(0, doc.lineCount() - 1);
  const nextLine = Math.max(0, Math.min(maxLine, line));
  const lineRange = doc.lineRange(nextLine);
  const lineLength = lineRange.end - lineRange.start;
  return {
    line: nextLine,
    col: Math.max(0, Math.min(lineLength, col)),
  };
}

export function blockSelectionFromAnchorFocus(
  doc: DocumentLike,
  points: { anchor: LineCol; focus: LineCol },
): MultiRangeSelection {
  const anchorLine = clampLineCol(doc, points.anchor.line, 0).line;
  const focusLine = clampLineCol(doc, points.focus.line, 0).line;
  const anchorCol = Math.max(0, points.anchor.col);
  const focusCol = Math.max(0, points.focus.col);
  const lineMin = Math.min(anchorLine, focusLine);
  const lineMax = Math.max(anchorLine, focusLine);
  const colMin = Math.min(anchorCol, focusCol);
  const colMax = Math.max(anchorCol, focusCol);
  const anchorUsesMinCol = anchorCol <= focusCol;
  const candidates: Array<{ line: number; range: Range; isEmptyDocLine: boolean }> = [];
  let nonEmptyCursorRows = 0;

  for (let line = lineMin; line <= lineMax; ++line) {
    const lineRange = doc.lineRange(line);
    const lineLength = lineRange.end - lineRange.start;
    const startCol = Math.min(colMin, lineLength);
    const endCol = Math.min(colMax, lineLength);
    const start = doc.lineColToOffset(line, startCol);
    const end = doc.lineColToOffset(line, endCol);
    const range = anchorUsesMinCol ? { anchor: start, focus: end } : { anchor: end, focus: start };
    if (lineLength > 0) nonEmptyCursorRows += 1;
    candidates.push({ line, range, isEmptyDocLine: lineLength === 0 });
  }

  // Columns are UTF-16 code-unit offsets within the line, not visual columns.
  // When block-selecting a zero-width column across empty lines, keep the focus
  // row but drop other empty-document rows once the gesture already covers more
  // than one non-empty row; this avoids noisy extra cursors while preserving at
  // least one cursor in fully-empty selections.
  const ranges =
    colMin === colMax && nonEmptyCursorRows > 1
      ? candidates
          .filter((candidate) => !candidate.isEmptyDocLine || candidate.line === focusLine)
          .map((candidate) => candidate.range)
      : candidates.map((candidate) => candidate.range);

  const clampedFocus = clampLineCol(doc, focusLine, focusCol);
  const primary = Math.max(
    0,
    ranges.findIndex(
      (range) => range.focus === doc.lineColToOffset(clampedFocus.line, clampedFocus.col),
    ),
  );
  return new MultiRangeSelection(ranges, primary);
}
