import type { TextDocument } from 'fxe';
import { Component, type Node, useEffect, useMemo, useRef, useState } from '../reconciler/fiber.ts';
import { StyleSheet } from '../style/index.ts';
import type { StyleValue } from '../style/types.ts';
import { useTheme } from '../theme/provider.ts';
import { Button } from './Button.ts';
import {
  applyFindReplaceEdits,
  buildReplaceAllEdits,
  buildReplaceCurrentEdit,
  type FindReplaceDispatch,
  type FindReplaceSearchState,
  findNextIndexAfterReplacement,
  getActiveMatch,
  nextActiveIndex,
  prevActiveIndex,
  resolveFindReplaceState,
} from './find_replace_logic.ts';
import { Text } from './Text.ts';
import { TextInput } from './TextInput.ts';
import { View } from './View.ts';

export interface FindReplaceBarProps {
  document: TextDocument;
  initialQuery?: string;
  initialReplacement?: string;
  caseSensitive?: boolean;
  useRegex?: boolean;
  regexFlags?: string;
  searchDeadlineMs?: number;
  searchMaxMatches?: number;
  showReplace?: boolean;
  onActiveMatchChange?: (
    range: { start: number; end: number; index: number; total: number } | null,
  ) => void;
  onReplaced?: (count: number) => void;
  onClose?: () => void;
  dispatch?: FindReplaceDispatch;
  style?: StyleValue;
}

const styles = StyleSheet.create({
  bar: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: 8,
    padding: 8,
  },
  section: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: 8,
  },
  grow: {
    flexGrow: 1,
    minWidth: 140,
  },
  compactButton: {
    minWidth: 36,
    paddingX: 10,
  },
  count: {
    minWidth: 72,
  },
});

export const FindReplaceBar = Component((props: FindReplaceBarProps): Node => {
  const theme = useTheme();
  const [query, setQuery] = useState(props.initialQuery ?? '');
  const [replacement, setReplacement] = useState(props.initialReplacement ?? '');
  const [caseSensitive, setCaseSensitive] = useState(props.caseSensitive ?? false);
  const [useRegex, setUseRegex] = useState(props.useRegex ?? false);
  const [, setDocTick] = useState(0);
  const [activeIndex, setActiveIndex] = useState(0);
  const cacheRef = useRef<FindReplaceSearchState | null>(null);

  const ensureMatches = (): FindReplaceSearchState => {
    const next = resolveFindReplaceState(
      props.document,
      {
        query,
        caseSensitive,
        useRegex,
        regexFlags: props.regexFlags,
        searchDeadlineMs: props.searchDeadlineMs,
        searchMaxMatches: props.searchMaxMatches,
      },
      cacheRef.current,
    );
    cacheRef.current = next;
    return next;
  };

  useEffect(() => {
    cacheRef.current = null;
    setDocTick((tick) => tick + 1);
    const subId = props.document.subscribe(() => {
      cacheRef.current = null;
      setDocTick((tick) => tick + 1);
    });
    return () => {
      props.document.unsubscribe(subId);
    };
  }, [props.document]);

  const matches = ensureMatches();
  const activeMatch = useMemo(() => getActiveMatch(matches, activeIndex), [matches, activeIndex]);

  useEffect(() => {
    props.onActiveMatchChange?.(activeMatch);
  }, [props.onActiveMatchChange, activeMatch]);

  const showReplace = props.showReplace !== false;
  const countLabel = useMemo(() => {
    if (query.length === 0) return '';
    if (matches.error) return matches.error;
    if (matches.ranges.length === 0) return matches.aborted ? '0… (more)' : '0 / 0';
    const prefix = `${(activeMatch?.index ?? 0) + 1} / ${matches.ranges.length}`;
    return matches.aborted ? `${prefix}… (more)` : prefix;
  }, [activeMatch?.index, matches.aborted, matches.error, matches.ranges.length, query.length]);

  const refreshAfterInputChange = (): void => {
    cacheRef.current = null;
    setActiveIndex(0);
  };

  const goNext = (): void => {
    const state = ensureMatches();
    if (state.ranges.length === 0) return;
    setActiveIndex((current) => nextActiveIndex(current, state.ranges.length));
  };

  const goPrev = (): void => {
    const state = ensureMatches();
    if (state.ranges.length === 0) return;
    setActiveIndex((current) => prevActiveIndex(current, state.ranges.length));
  };

  const replaceCurrent = (): void => {
    const before = ensureMatches();
    const edit = buildReplaceCurrentEdit(before, activeIndex, replacement);
    if (!edit) return;
    applyFindReplaceEdits(props.document, [edit], props.dispatch);
    cacheRef.current = null;
    const after = ensureMatches();
    setActiveIndex(findNextIndexAfterReplacement(after, edit.start, replacement));
    props.onReplaced?.(1);
  };

  const replaceAll = (): void => {
    const before = ensureMatches();
    const edits = buildReplaceAllEdits(before, replacement);
    if (edits.length === 0) return;
    applyFindReplaceEdits(props.document, edits, props.dispatch);
    cacheRef.current = null;
    const after = ensureMatches();
    setActiveIndex(after.ranges.length === 0 ? 0 : Math.min(activeIndex, after.ranges.length - 1));
    props.onReplaced?.(edits.length);
  };

  return View({
    style: [
      styles.bar,
      {
        backgroundColor: theme.colors.surface,
        borderColor: theme.colors.border,
        borderWidth: 1,
        borderRadius: theme.radii.md,
      },
      props.style,
    ],
    accessibilityRole: 'group',
    accessibilityLabel: 'Find and replace toolbar',
    children: [
      View({
        style: [styles.section, styles.grow],
        children: [
          TextInput({
            style: [styles.grow, { minHeight: 36 }],
            value: query,
            placeholder: 'Find…',
            accessibilityLabel: 'Find query',
            inputMode: 'search',
            onChange: (value) => {
              setQuery(value);
              refreshAfterInputChange();
            },
            onSubmit: () => {
              goNext();
            },
          }),
          Button({
            title: '↑',
            style: styles.compactButton,
            accessibilityLabel: 'Previous match',
            disabled: matches.ranges.length === 0,
            onPress: goPrev,
          }),
          Button({
            title: '↓',
            style: styles.compactButton,
            accessibilityLabel: 'Next match',
            disabled: matches.ranges.length === 0,
            onPress: goNext,
          }),
          Text({
            style: [
              styles.count,
              { color: matches.error ? theme.colors.danger : theme.colors.mutedText, fontSize: 13 },
            ],
            accessibilityLabel:
              countLabel.length > 0 ? `Match count ${countLabel}` : 'No active query',
            children: countLabel,
          }),
          Button({
            title: caseSensitive ? 'Aa' : 'aa',
            style: styles.compactButton,
            accessibilityLabel: 'Toggle case sensitive search',
            onPress: () => {
              setCaseSensitive((current) => !current);
              refreshAfterInputChange();
            },
          }),
          Button({
            title: '.*',
            style: styles.compactButton,
            accessibilityLabel: 'Toggle regular expression search',
            onPress: () => {
              setUseRegex((current) => !current);
              refreshAfterInputChange();
            },
          }),
        ],
      }),
      ...(showReplace
        ? [
            View({
              style: [styles.section, styles.grow],
              children: [
                TextInput({
                  style: [styles.grow, { minHeight: 36 }],
                  value: replacement,
                  placeholder: 'Replace…',
                  accessibilityLabel: 'Replace with',
                  onChange: setReplacement,
                  onSubmit: () => {
                    goNext();
                  },
                }),
                Button({
                  title: 'Replace',
                  accessibilityLabel: 'Replace current match',
                  disabled: activeMatch === null,
                  onPress: replaceCurrent,
                }),
                Button({
                  title: 'Replace All',
                  accessibilityLabel: 'Replace all matches',
                  disabled: matches.ranges.length === 0,
                  onPress: replaceAll,
                }),
              ],
            }),
          ]
        : []),
      ...(props.onClose
        ? [
            Button({
              title: 'Close',
              accessibilityLabel: 'Close find and replace',
              onPress: props.onClose,
            }),
          ]
        : []),
    ],
  });
}, 'FindReplaceBar');
