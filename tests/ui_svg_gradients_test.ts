import { parseSvg } from 'fxe-ui';

import { assert, assertDeepEqual, assertEqual, run, test } from './ts_harness.ts';

test('parseSvg materializes linear gradients from defs', () => {
  const svg = `
    <svg width="10" height="10" viewBox="0 0 10 10">
      <defs>
        <linearGradient id="g1">
          <stop offset="0" stop-color="red" />
          <stop offset="1" stop-color="#0000ff" />
        </linearGradient>
      </defs>
      <rect width="10" height="10" fill="url(#g1)" />
    </svg>
  `;
  const doc = parseSvg(svg);
  assertEqual(doc.shapes.length, 1);
  assertDeepEqual(doc.shapes[0].fill, {
    kind: 'linear-gradient',
    x1: 0,
    y1: 0,
    x2: 1,
    y2: 0,
    stops: [
      { offset: 0, color: 0xff0000ff },
      { offset: 1, color: 0x0000ffff },
    ],
    spread: 'pad',
    gradientUnits: 'objectBoundingBox',
    transform: [1, 0, 0, 1, 0, 0],
  });
});

test('parseSvg materializes radial gradients', () => {
  const svg = `
    <svg width="10" height="10" viewBox="0 0 10 10">
      <defs>
        <radialGradient id="rg" cx="5" cy="5" r="5">
          <stop offset="0%" stop-color="#ffffff" />
          <stop offset="100%" stop-color="#000000" stop-opacity="0.5" />
        </radialGradient>
      </defs>
      <circle cx="5" cy="5" r="5" fill="url(#rg)" />
    </svg>
  `;
  const doc = parseSvg(svg);
  assertEqual(doc.shapes.length, 1);
  assertDeepEqual(doc.shapes[0].fill, {
    kind: 'radial-gradient',
    cx: 5,
    cy: 5,
    r: 5,
    fx: 5,
    fy: 5,
    stops: [
      { offset: 0, color: 0xffffffff },
      { offset: 1, color: 0x00000080 },
    ],
    spread: 'pad',
    gradientUnits: 'objectBoundingBox',
    transform: [1, 0, 0, 1, 0, 0],
  });
});

test('parseSvg resolves xlink href inheritance chains', () => {
  const svg = `
    <svg width="10" height="10" viewBox="0 0 10 10" xmlns:xlink="http://www.w3.org/1999/xlink">
      <defs>
        <linearGradient id="g1" x1="0.25" x2="0.75">
          <stop offset="0" stop-color="#123456" />
          <stop offset="1" stop-color="#abcdef" />
        </linearGradient>
        <linearGradient id="g2" xlink:href="#g1" y1="0.1" y2="0.9" />
        <linearGradient id="g3" href="#g2" gradientTransform="translate(2 3)" />
      </defs>
      <rect width="10" height="10" fill="url(#g3)" />
    </svg>
  `;
  const doc = parseSvg(svg);
  assertEqual(doc.shapes.length, 1);
  assertDeepEqual(doc.shapes[0].fill, {
    kind: 'linear-gradient',
    x1: 0.25,
    y1: 0.1,
    x2: 0.75,
    y2: 0.9,
    stops: [
      { offset: 0, color: 0x123456ff },
      { offset: 1, color: 0xabcdefff },
    ],
    spread: 'pad',
    gradientUnits: 'objectBoundingBox',
    transform: [1, 0, 0, 1, 2, 3],
  });
});

test('parseSvg leaves shapes with missing gradients unfilled', () => {
  const svg = `
    <svg width="10" height="10" viewBox="0 0 10 10">
      <rect width="10" height="10" fill="url(#missing)" />
    </svg>
  `;
  const doc = parseSvg(svg);
  assertEqual(doc.shapes.length, 1);
  assertEqual(doc.shapes[0].fill, undefined);
});

test('parseSvg keeps solid fills on the numeric fast path', () => {
  const svg = `
    <svg width="10" height="10" viewBox="0 0 10 10">
      <rect width="10" height="10" fill="#ff0000" stroke="url(#missing)" />
    </svg>
  `;
  const doc = parseSvg(svg);
  assertEqual(doc.shapes.length, 1);
  assertEqual(doc.shapes[0].fill, 0xff0000ff);
  assertEqual(doc.shapes[0].stroke, undefined);
  assert(typeof doc.shapes[0].fill === 'number');
});

void run();
