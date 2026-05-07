/** @jsxImportSource fxe-ui */

import { App, Window } from 'fxe';
import { mount, type Node, StyleSheet, Text, useState, View } from 'fxe-ui';

const COLS = 30;
const ROWS = 30;

const s = StyleSheet.create({
  root: { width: '100%', height: '100%', padding: 16, backgroundColor: 0x0f172aff },
  grid: { flexDirection: 'row', flexWrap: 'wrap', gap: 4 },
  cell: {
    width: 64,
    height: 48,
    padding: 6,
    borderRadius: 6,
    backgroundColor: 0x1e293bff,
  },
  title: { fontSize: 11, color: 0xe2e8f0ff },
  sub: { fontSize: 9, color: 0x94a3b8ff },
});

function Cell({ i, tick }: { i: number; tick: number }): Node {
  return (
    <View style={s.cell}>
      <Text style={s.title}>Item {i}</Text>
      <Text style={s.sub}>v{tick % 1000}</Text>
    </View>
  );
}

function App2(): Node {
  const [tick, setTick] = useState(0);
  // Animate so reconcile/paint actually runs every frame
  setTimeout(() => setTick((t) => t + 1), 0);
  const cells: Node[] = [];
  for (let i = 0; i < COLS * ROWS; i++) cells.push(<Cell key={i} i={i} tick={tick} />);
  return (
    <View style={s.root}>
      <View style={s.grid}>{cells}</View>
    </View>
  );
}

const win = new Window({ width: COLS * 70, height: ROWS * 54, title: 'stress grid' });
mount(<App2 />, win, { lazy: false });
App.run({ animate: true });
