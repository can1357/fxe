/** @jsxImportSource fxe-ui */

// Visual smoke-test for the embedded JetBrainsMono Nerd Font cascade.
// Renders a row of common Nerd Font glyphs inline with regular Latin text;
// if the cascade is wired correctly each codepoint shows as an icon (rather
// than a tofu rectangle) at the same baseline as the surrounding text.

import { App, Window } from 'fxe';
import { mount, StyleSheet, Text, View } from 'fxe-ui';

const s = StyleSheet.create({
  root: {
    width: '100%',
    height: '100%',
    padding: 32,
    backgroundColor: 0x0f172aff,
    flexDirection: 'column',
    gap: 16,
  },
  title: { fontSize: 22, color: 0xe2e8f0ff },
  subtitle: { fontSize: 13, color: 0x94a3b8ff },
  row: { flexDirection: 'row', alignItems: 'center', gap: 18 },
  glyph: { fontSize: 32, color: 0xf8fafcff },
  label: { fontSize: 12, color: 0x64748bff },
  cell: { flexDirection: 'column', alignItems: 'center', gap: 4, minWidth: 56 },
});

interface Icon {
  cp: string;
  label: string;
}

const ICONS: Icon[] = [
  { cp: '\uE0B0', label: 'powerline' },
  { cp: '\uE5FA', label: 'seti-ui' },
  { cp: '\uE702', label: 'devicon' },
  { cp: '\uE7C5', label: 'devicon-2' },
  { cp: '\uEA60', label: 'codicon' },
  { cp: '\uF00C', label: 'fa-check' },
  { cp: '\uF02D', label: 'fa-book' },
  { cp: '\uF07B', label: 'fa-folder' },
  { cp: '\uF15B', label: 'fa-file' },
  { cp: '\uF1C0', label: 'fa-db' },
  { cp: '\uF2DB', label: 'fa-cpu' },
  { cp: '\uF49B', label: 'octicon' },
];

function Demo() {
  return (
    <View style={s.root}>
      <Text style={s.title}>Nerd Font cascade — embedded JetBrainsMono</Text>
      <Text style={s.subtitle}>
        Each glyph below is in the Private-Use-Area; primary font is the system default. If the
        cascade is wired, every cell renders an icon (not tofu).
      </Text>
      <View style={s.row}>
        {ICONS.map((icon) => (
          <View key={icon.label} style={s.cell}>
            <Text style={s.glyph}>{icon.cp}</Text>
            <Text style={s.label}>{icon.label}</Text>
          </View>
        ))}
      </View>
      <Text style={{ fontSize: 18, color: 0xcbd5e1ff }}>
        {`Inline:  branch \uE725  \uE702 main  \uF00C ok  \uF188 bug  \uE7B5 vim  \uF02D docs`}
      </Text>
    </View>
  );
}

const win = new Window({
  width: 920,
  height: 320,
  title: 'fxe — nerd font cascade',
});
mount(<Demo />, win);
App.run();
