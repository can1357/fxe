// FXE resolves these package names through the host loader rather than node_modules.
import { Component, Draw, ErrorBoundary, Layer, Suspense } from 'fxe-ui';
import { jsx, jsxs } from 'fxe-ui/jsx-runtime';

import { assert, assertEqual, run, test } from './ts_harness.ts';

test('fxe-ui package exports construct basic nodes', () => {
  const draw = Draw(() => undefined, ['deps']);
  assertEqual(draw.type, 'draw');
  if (draw.type !== 'draw') throw new Error('expected draw node');
  assertEqual(draw.props.deps?.[0], 'deps');

  const layer = Layer({ children: [draw], key: 'root' });
  assertEqual(layer.type, 'layer');
  assertEqual(layer.key, 'root');
  if (layer.type !== 'layer') throw new Error('expected layer node');
  assertEqual(layer.props.children[0], draw);

  const View = Component(
    (props: { value: number }) =>
      Draw(() => {
        void props.value;
      }),
    'View',
  );
  const component = View({ value: 7, key: 'view' });
  assertEqual(component.type, 'component');
  if (component.type !== 'component') throw new Error('expected component node');
  assertEqual(component.displayName, 'View');
  assertEqual(component.key, 'view');

  assertEqual(ErrorBoundary({ children: draw }).type, 'error-boundary');
  assertEqual(Suspense({ children: draw }).type, 'suspense');
});

test('fxe-ui jsx-runtime exports jsx/jsxs and Fragment', () => {
  const view = jsxs('view', {
    children: [jsx('text', { children: 'Hello' }), null, false],
    key: 'multi',
  });
  assert(!Array.isArray(view), 'view jsxs should produce a single node');
  assertEqual(view.type, 'component');
  if (view.type !== 'component') throw new Error('expected component node from jsxs');
  assertEqual(view.displayName, 'View');
  assertEqual(view.key, 'multi');

  const text = jsx('text', { children: 'World', key: 'jsx-text' });
  assert(!Array.isArray(text), 'text JSX should produce a single node');
  assertEqual(text.type, 'component');
  if (text.type !== 'component') throw new Error('expected component node from jsx');
  assertEqual(text.displayName, 'Text');
  assertEqual(text.key, 'jsx-text');
});

await run();
