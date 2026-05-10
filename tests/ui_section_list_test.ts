import { SectionList, Text } from 'fxe-ui';
import {
  __sectionListLayout,
  __sectionListStickyState,
} from '../packages/fxe-ui/src/components/SectionList.ts';
import { assert, assertDeepEqual, assertEqual, run, test } from './ts_harness.ts';

test('section list exposes internal layout helper and flattens interleaved rows', () => {
  assert(typeof __sectionListLayout === 'function');

  const layout = __sectionListLayout({
    sections: [
      { key: 'recent', data: ['a', 'b'] },
      { key: 'archived', data: [] },
      { key: 'later', data: ['c'] },
    ],
    itemHeight: 10,
    sectionHeaderHeight: 20,
  });

  assertDeepEqual(layout, {
    rows: [
      { kind: 'header', sectionIndex: 0, itemIndex: -1, top: 0, height: 20 },
      { kind: 'item', sectionIndex: 0, itemIndex: 0, top: 20, height: 10 },
      { kind: 'item', sectionIndex: 0, itemIndex: 1, top: 30, height: 10 },
      { kind: 'header', sectionIndex: 1, itemIndex: -1, top: 40, height: 20 },
      { kind: 'header', sectionIndex: 2, itemIndex: -1, top: 60, height: 20 },
      { kind: 'item', sectionIndex: 2, itemIndex: 0, top: 80, height: 10 },
    ],
    totalHeight: 90,
    sectionStartY: [0, 40, 60],
  });
});

test('section list sticky state advances and hands off to the next header', () => {
  const layout = __sectionListLayout({
    sections: [
      { key: 'alpha', data: ['a', 'b'] },
      { key: 'beta', data: ['c'] },
    ],
    itemHeight: 12,
    sectionHeaderHeight: 20,
  });

  assertDeepEqual(
    __sectionListStickyState({
      sectionStartY: layout.sectionStartY,
      sectionHeaderHeight: 20,
      offsetY: 0,
    }),
    {
      activeSectionIndex: 0,
      pinnedY: 0,
    },
  );

  assertDeepEqual(
    __sectionListStickyState({
      sectionStartY: layout.sectionStartY,
      sectionHeaderHeight: 20,
      offsetY: 27,
    }),
    {
      activeSectionIndex: 0,
      pinnedY: -3,
    },
  );

  assertDeepEqual(
    __sectionListStickyState({
      sectionStartY: layout.sectionStartY,
      sectionHeaderHeight: 20,
      offsetY: 44,
    }),
    {
      activeSectionIndex: 1,
      pinnedY: 0,
    },
  );
});

test('section list returns a component node when sticky headers are disabled', () => {
  const node = SectionList({
    sections: [{ key: 'alpha', data: ['a'] }],
    itemHeight: 12,
    sectionHeaderHeight: 20,
    stickyHeaders: false,
    renderItem: (item) => Text({ children: item }),
  });

  assertEqual(node.type, 'component');
  if (node.type !== 'component') throw new Error('expected component node');
  assertEqual(node.displayName, 'SectionList');
});

void run();
