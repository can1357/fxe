import { type BoundaryChild, createContext, type Node, useContext } from '../reconciler/fiber.ts';
import type { TextStyle } from '../style/types.ts';

export const TextStyleContext = createContext<TextStyle>({ color: 0xf4f6fbff, fontSize: 16 });

export function useTextStyle(): TextStyle {
  return useContext(TextStyleContext);
}

export function TextStyleProvider(props: { value: TextStyle; children?: BoundaryChild }): Node {
  return TextStyleContext.Provider({ value: props.value, children: props.children });
}
