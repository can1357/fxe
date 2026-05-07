/** @jsxImportSource fxe-ui */

import { App, Window } from 'fxe';
import { Button, mount, type Node, ScrollView, StyleSheet, Text, useState, View } from 'fxe-ui';

const s = StyleSheet.create({
  root: { width: '100%', height: '100%', padding: 24, gap: 16, backgroundColor: 0x0f172aff },
  card: {
    padding: 18,
    gap: 12,
    borderWidth: 1,
    borderColor: 0x334155ff,
    backgroundColor: 0x111827ff,
  },
  title: { height: 34, color: 0xf8fafcff, fontSize: 24 },
  body: { height: 24, color: 0xcbd5e1ff, fontSize: 15 },
  row: { flexDirection: 'row', gap: 12, height: 42 },
  scroll: { height: 150, borderWidth: 1, borderColor: 0x334155ff, backgroundColor: 0x020617ff },
  item: { height: 28, paddingX: 10, paddingY: 5 },
});

function Demo(): Node {
  const [count, setCount] = useState(0);
  return (
    <View style={s.root}>
      <View style={s.card}>
        <Text style={s.title}>fxe-ui toolkit</Text>
        <Text style={s.body}>
          Flex layout, frozen styles, themed primitives, hit testing, and input.
        </Text>
        <View style={s.row}>
          <Button title={`Clicked ${count}`} onPress={() => setCount((n) => n + 1)} />
          <Button title="Reset" onPress={() => setCount(0)} />
        </View>
      </View>
      <ScrollView style={s.scroll}>
        {Array.from({ length: 12 }, (_, i) => (
          <View key={`row-${i}`} style={s.item}>
            <Text style={{ color: i % 2 ? 0x94a3b8ff : 0xe2e8f0ff, fontSize: 14 }}>
              Scrollable row {i + 1}
            </Text>
          </View>
        ))}
      </ScrollView>
    </View>
  );
}

const win = new Window({ width: 760, height: 520, title: 'fxe-ui toolkit demo' });
mount(<Demo />, win, { lazy: false });
App.run({ animate: true, fps: 60 });
