import { CommandBuffer } from 'fxe';
import { assertEqual, run, test } from '../../../../tests/ts_harness.ts';
import { View } from '../components/View.ts';
import { Component, createContext, Draw, render, useContext } from './fiber.ts';

test('custom Context.Provider is visible to useContext on the first render', () => {
  const NumberContext = createContext(0);
  const seen: number[] = [];
  const Consumer = Component(() => {
    seen.push(useContext(NumberContext));
    return Draw(() => undefined);
  }, 'FirstRenderContextConsumer');

  render(
    View({
      style: { width: 120, height: 40 },
      children: NumberContext.Provider({ value: 42, children: Consumer({ key: 'consumer' }) }),
    }),
    new CommandBuffer(),
  );

  assertEqual(seen[0], 42);
  assertEqual(seen.join(','), '42,42');
});

await run();
