/** @jsxImportSource fxe-ui */

import { App, Window } from 'fxe';
import {
  Button,
  defaultTheme,
  mount,
  Pressable,
  ScrollView,
  StyleSheet,
  Text,
  TextInput,
  type Node,
  View,
} from 'fxe-ui';

const WINDOW_WIDTH = 320;
const WINDOW_HEIGHT = 240;
const BACKGROUND = 0xe6d2b5ff;
const CARD = 0xf6efe4ff;
const BORDER = 0x8f6f49ff;
const TEXT = 0x2f2419ff;
const ACCENT = 0x8e5c2dff;
const ACCENT_HOVER = 0xb87c45ff;
const ACCENT_PRESSED = 0x6e451eff;

const s = StyleSheet.create({
  root: {
    width: '100%',
    height: '100%',
    alignItems: 'center',
    justifyContent: 'center',
    backgroundColor: BACKGROUND,
  },
  card: {
    width: 240,
    padding: 16,
    gap: 12,
    borderWidth: 1,
    borderColor: BORDER,
    borderRadius: 12,
    backgroundColor: CARD,
  },
  title: {
    color: TEXT,
    fontSize: 14,
    height: 18,
  },
  button: {
    minWidth: 168,
  },
  input: {
    width: 220,
  },
  pressable: {
    minWidth: 180,
    minHeight: 72,
    borderWidth: 1,
    borderColor: BORDER,
    borderRadius: 12,
    paddingX: 16,
    paddingY: 14,
    alignItems: 'center',
    justifyContent: 'center',
  },
  pressableText: {
    color: TEXT,
    fontSize: 16,
    height: 20,
  },
  scroll: {
    width: 224,
    height: 132,
    borderWidth: 1,
    borderColor: BORDER,
    borderRadius: 12,
    backgroundColor: CARD,
  },
  scrollContent: {
    padding: 12,
    gap: 8,
  },
  scrollItem: {
    paddingX: 10,
    paddingY: 8,
    borderWidth: 1,
    borderColor: 0xd3b693ff,
    borderRadius: 8,
    backgroundColor: 0xfff8edff,
  },
  scrollItemText: {
    color: TEXT,
    fontSize: 13,
    height: 16,
  },
  borderedView: {
    width: 224,
    padding: 18,
    gap: 8,
    borderWidth: 2,
    borderColor: BORDER,
    borderRadius: 14,
    backgroundColor: CARD,
  },
  body: {
    color: TEXT,
    fontSize: 16,
    height: 20,
  },
  muted: {
    color: 0x6c5a48ff,
    fontSize: 12,
    height: 16,
  },
});

function panel(title: string, child: Node): Node {
  return (
    <View style={s.root}>
      <View style={s.card}>
        <Text style={s.title}>{title}</Text>
        {child}
      </View>
    </View>
  );
}

function buttonVariant(pressed: boolean): Node {
  return panel(
    pressed ? 'Button · pressed' : 'Button · default',
    <Button
      title="Deploy"
      onPress={() => {}}
      style={(state) => ({
        ...s.button,
        backgroundColor: state.pressed ? ACCENT_PRESSED : state.hovered ? ACCENT_HOVER : ACCENT,
        borderWidth: 1,
        borderColor: state.pressed ? 0x563311ff : 0x734821ff,
      })}
      textStyle={{ color: 0xfff9f2ff }}
    />,
  );
}

function textInputVariant(value: string | undefined, label: string): Node {
  return panel(
    label,
    <TextInput
      style={s.input}
      value={value}
      placeholder="name@example.com"
      caretBlinkMs={0}
      focusRing
    />,
  );
}

function pressableHoverVariant(): Node {
  return panel(
    'Pressable · hover',
    <Pressable
      onPress={() => {}}
      style={(state) => ({
        ...s.pressable,
        backgroundColor: state.hovered ? 0xfff1dcff : CARD,
        borderColor: state.hovered ? ACCENT_HOVER : BORDER,
      })}
    >
      {(state) => (
        <Text style={[s.pressableText, { color: state.hovered ? ACCENT_PRESSED : TEXT }]}>
          Hover target
        </Text>
      )}
    </Pressable>,
  );
}

function scrollViewVariant(): Node {
  return (
    <View style={s.root}>
      <ScrollView style={s.scroll} contentStyle={s.scrollContent}>
        {Array.from({ length: 8 }, (_, index) => (
          <View key={`item-${index}`} style={s.scrollItem}>
            <Text style={s.scrollItemText}>Row {index + 1} · component golden</Text>
          </View>
        ))}
      </ScrollView>
    </View>
  );
}

function viewBorderedVariant(): Node {
  return (
    <View style={s.root}>
      <View style={s.borderedView}>
        <Text style={s.body}>Bordered container</Text>
        <Text style={s.muted}>Static layout baseline</Text>
      </View>
    </View>
  );
}

const variants: Record<string, () => Node> = {
  'button-default': () => buttonVariant(false),
  'button-pressed': () => buttonVariant(true),
  'textinput-empty': () => textInputVariant(undefined, 'TextInput · empty'),
  'textinput-focused': () => textInputVariant('Ada Lovelace', 'TextInput · focused'),
  'pressable-hover': pressableHoverVariant,
  'scrollview-default': scrollViewVariant,
  'view-bordered': viewBorderedVariant,
};

function componentFromInputs(argv: readonly string[]): string {
  const fromFlag = argv.find((arg) => arg.startsWith('--component='));
  if (fromFlag) return fromFlag.slice('--component='.length);
  const fromPositional = argv[2];
  if (fromPositional?.startsWith('--component='))
    return fromPositional.slice('--component='.length);
  const fromEnv = process.env.FXE_GOLDEN_COMPONENT;
  if (fromEnv) return fromEnv;
  throw new Error(
    `missing component selector; pass --component=... or set FXE_GOLDEN_COMPONENT. argv=${argv.join(' ')}`,
  );
}

const component = componentFromInputs(process.argv);
const renderVariant = variants[component];
if (!renderVariant) {
  throw new Error(
    `unknown component variant ${component}; expected one of ${Object.keys(variants).join(', ')}`,
  );
}

const win = new Window({
  width: WINDOW_WIDTH,
  height: WINDOW_HEIGHT,
  title: `fxe-ui golden · ${component}`,
});
mount(renderVariant(), win, {
  theme: defaultTheme,
  backgroundColor: BACKGROUND,
  lazy: false,
  devTools: false,
});
App.run();
