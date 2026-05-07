import {
  type BoundaryChild,
  Component,
  createContext,
  type Node,
  useContext,
} from '../reconciler/fiber.ts';
import { defaultTheme, type Theme } from './theme.ts';

export const ThemeContext = createContext<Theme>(defaultTheme);

export const ThemeProvider = Component(
  (props: { value?: Theme; theme?: Theme; children?: BoundaryChild; key?: string }): Node =>
    ThemeContext.Provider({
      value: props.value ?? props.theme ?? defaultTheme,
      children: props.children,
    }),
  'ThemeProvider',
);

export function useTheme(): Theme {
  return useContext(ThemeContext);
}
