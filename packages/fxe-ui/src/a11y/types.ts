export type AccessibilityRole =
  | 'none'
  | 'group'
  | 'text'
  | 'button'
  | 'link'
  | 'image'
  | 'textbox'
  | 'searchbox'
  | 'checkbox'
  | 'switch'
  | 'radio'
  | 'slider'
  | 'progressbar'
  | 'heading'
  | 'list'
  | 'listitem'
  | 'scrollview'
  | 'dialog'
  | 'alert'
  | 'status'
  | 'tab'
  | 'tablist'
  | 'menu'
  | 'menuitem';

export type AccessibilityLiveRegion = 'off' | 'polite' | 'assertive';
export type AccessibilityCheckedState = boolean | 'mixed';

export interface AccessibilityState {
  disabled?: boolean;
  selected?: boolean;
  checked?: AccessibilityCheckedState;
  expanded?: boolean;
  busy?: boolean;
  required?: boolean;
  invalid?: boolean | 'grammar' | 'spelling';
  readOnly?: boolean;
  pressed?: boolean;
}

export interface AccessibilityValue {
  text?: string;
  now?: number;
  min?: number;
  max?: number;
}

export interface AccessibilityProps {
  accessible?: boolean;
  accessibilityRole?: AccessibilityRole;
  accessibilityLabel?: string;
  accessibilityHint?: string;
  accessibilityState?: AccessibilityState;
  accessibilityValue?: string | number | AccessibilityValue;
  accessibilityLiveRegion?: AccessibilityLiveRegion;
  accessibilityLanguage?: string;
  accessibilityHeadingLevel?: 1 | 2 | 3 | 4 | 5 | 6;
  accessibilityId?: string;
  tabIndex?: number;
  focusable?: boolean;
  dir?: 'ltr' | 'rtl' | 'auto';
}

export interface AccessibilityRect {
  x: number;
  y: number;
  width: number;
  height: number;
}

export interface AccessibilityNodeSnapshot {
  id: string;
  parentId: string | null;
  role: AccessibilityRole;
  label: string;
  hint?: string;
  value?: AccessibilityValue;
  state: AccessibilityState;
  rect: AccessibilityRect;
  focusable: boolean;
  tabIndex?: number;
  liveRegion: AccessibilityLiveRegion;
  language?: string;
  headingLevel?: number;
  children: AccessibilityNodeSnapshot[];
}

export interface AccessibilityTreeSnapshot {
  rootId: string;
  generation: number;
  focusedId: string | null;
  nodesById: Record<string, AccessibilityNodeSnapshot>;
  childrenById: Record<string, string[]>;
}
