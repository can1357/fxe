import type { AccessibilityProps } from './types.ts';

export function extractA11yProps(p: Partial<AccessibilityProps>): AccessibilityProps {
  return {
    accessible: p.accessible,
    accessibilityRole: p.accessibilityRole,
    accessibilityLabel: p.accessibilityLabel,
    accessibilityHint: p.accessibilityHint,
    accessibilityState: p.accessibilityState,
    accessibilityValue: p.accessibilityValue,
    accessibilityLiveRegion: p.accessibilityLiveRegion,
    accessibilityLanguage: p.accessibilityLanguage,
    accessibilityHeadingLevel: p.accessibilityHeadingLevel,
    accessibilityId: p.accessibilityId,
    tabIndex: p.tabIndex,
    focusable: p.focusable,
    dir: p.dir,
  };
}
