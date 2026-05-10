/** @jsxImportSource fxe-ui */

import { App, Window } from 'fxe';
import { mount, StyleSheet, Text, View, VirtualList } from 'fxe-ui';

const ROW_HEIGHT = 28;
const ROW_COUNT = 100_000;

const rows = Array.from({ length: ROW_COUNT }, (_, i) => ({ id: i, label: `row #${i}` }));

const styles = StyleSheet.create({
  root: { width: '100%', height: '100%', backgroundColor: 0x0f172aff },
  list: { width: '100%', height: '100%' },
  row: { height: ROW_HEIGHT, paddingX: 12, justifyContent: 'center' },
  rowAlt: {
    height: ROW_HEIGHT,
    paddingX: 12,
    justifyContent: 'center',
    backgroundColor: 0x1e293bff,
  },
  label: { color: 0xffffffff, fontSize: 14 },
});

function AppView() {
  return (
    <View style={styles.root}>
      <VirtualList
        style={styles.list}
        data={rows}
        itemHeight={ROW_HEIGHT}
        keyExtractor={(item) => String(item.id)}
        renderItem={(item, index) => (
          <View style={index % 2 === 0 ? styles.row : styles.rowAlt}>
            <Text style={styles.label}>{item.label}</Text>
          </View>
        )}
      />
    </View>
  );
}

mount(
  <AppView />,
  new Window({ width: 720, height: 600, title: 'fxe — virtual list stress (100k rows)' }),
  { lazy: false },
);
App.run();
