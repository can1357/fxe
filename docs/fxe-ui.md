# fxe-ui — UI Toolkit

`fxe-ui` is the single JSX/TSX UI package shipped under `packages/fxe-ui/`. It owns the reconciler (`Layer`, `Draw`, hooks, scheduler, signals, external-store), a TypeScript flexbox layout solver, CSS-like `Style` objects, theme context, components, paint pipeline, mount pipeline, and `Animated` timing/spring helpers.

## Conventions

- Import from `fxe-ui` and use `/** @jsxImportSource fxe-ui */` for TSX.
- Default layout is Yoga / React Native style: `flexDirection: 'column'`, not CSS `row`.
- `StyleSheet.create()` freezes stable style objects. Prefer it for styles captured in hook dependency arrays.
- Core components: `View`, `Text`, `Image`, `Pressable`, `Button`, `ScrollView`, `TextInput`, `VirtualList`.
- Hooks: `useState`, `useReducer`, `useRef`, `useEffect`, `useMemo`, `useContext`, `useId`, `useFrame`, `useEvent`, `useDeferredValue`, `useTransition`.
- `mount(root, window)` wires layout, paint, hit-testing, hover/press/focus, cursor, and keyboard dispatch. It returns a disposer for listener cleanup.
- Object-identity memoization (layouts keyed on props object identity, AST handles in demos/tests, etc.) should use `Symbol` + `Reflect.get` / `Reflect.set`, not `WeakMap` — see [JS/TS object-identity caches](code-style.md#jsts-object-identity-caches).
- Layout primitives push entries to a built-in `recordLayout` sink when layout tracing is enabled — see the SDK's `page.layout_trace_*` helpers in [the Python SDK guide](python-sdk.md#layout-tracing-fxe-ui).

## Example

```tsx
/** @jsxImportSource fxe-ui */
import { Window } from 'fxe';
import { Button, StyleSheet, Text, View, mount, useState } from 'fxe-ui';

const s = StyleSheet.create({
  root: { width: '100%', height: '100%', padding: 24, gap: 12, backgroundColor: 0x0f172aff },
  title: { height: 28, color: 0xffffffff, fontSize: 22 },
});

function App() {
  const [count, setCount] = useState(0);
  return (
    <View style={s.root}>
      <Text style={s.title}>Count {count}</Text>
      <Button title="Increment" onPress={() => setCount((n) => n + 1)} />
    </View>
  );
}

mount(<App />, new Window({ width: 480, height: 320 }));
```
