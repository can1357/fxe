/** @jsxImportSource fxe-ui */

import { App, Window } from 'fxe';
import {
  Animated,
  Button,
  defaultTheme,
  mount,
  Pressable,
  ScrollView,
  StyleSheet,
  Text,
  TextInput,
  ThemeProvider,
  type Theme,
  type Node,
  useEffect,
  useMemo,
  useState,
  View,
} from 'fxe-ui';

const NAV = ['Overview', 'Activity', 'Orders', 'Team', 'Settings'] as const;
const STATS = [
  ['Uptime', '99.98%', '+0.04%'],
  ['Throughput', '184/s', '+12 today'],
  ['Queue', '07', '2 waiting'],
] as const;
const FEED = [
  'Inbound checks cleared for west region',
  'Asset sync completed without drift',
  'Three pending approvals assigned',
  'Shadow deploy reached 25% traffic',
  'Cache pressure returned to baseline',
  'Design review bundle published',
  'Audit snapshot sealed for export',
  'Scheduler lane utilization normalized',
  'Primary replica applied 18 commits',
  'Keyboard shortcuts synced to profile',
  'Synthetic health probes all green',
  'Backup rotation completed on time',
  'Release window opened for staging',
  'Notifications digest queued',
] as const;

const lightTheme: Theme = {
  ...defaultTheme,
  colors: {
    ...defaultTheme.colors,
    background: 0xf4f7fbff,
    surface: 0xffffffff,
    text: 0x132033ff,
    accent: 0x2954f3ff,
  },
};

const palette = {
  dark: {
    app: 0x09111fff,
    panel: 0x0f172aff,
    panelAlt: 0x111c33ff,
    panelSoft: 0x162238ff,
    card: 0x142035ff,
    cardAlt: 0x182740ff,
    text: 0xf8fafcff,
    muted: 0x94a3b8ff,
    line: 0x27344dff,
    lineStrong: 0x334563ff,
    accent: 0x60a5faff,
    accentSoft: 0x173153ff,
    danger: 0xf97316ff,
    success: 0x34d399ff,
    shadow: 0x02061788,
    input: 0x09111fff,
  },
  light: {
    app: 0xeef3faff,
    panel: 0xf8fbffff,
    panelAlt: 0xffffffff,
    panelSoft: 0xf1f5f9ff,
    card: 0xffffffff,
    cardAlt: 0xf8fbffff,
    text: 0x142033ff,
    muted: 0x526277ff,
    line: 0xd6e0eeff,
    lineStrong: 0xc4d0e2ff,
    accent: 0x2954f3ff,
    accentSoft: 0xe2ebffff,
    danger: 0xdc2626ff,
    success: 0x059669ff,
    shadow: 0x0f172a1f,
    input: 0xffffffff,
  },
} as const;

const s = StyleSheet.create({
  root: { width: '100%', height: '100%', padding: 20, backgroundColor: 0x00000000 },
  shell: { flex: 1, flexDirection: 'row', borderRadius: 24, overflow: 'hidden' },
  sidebar: { width: 236, padding: 20, gap: 14, borderRightWidth: 1 },
  brand: { gap: 4, marginBottom: 8 },
  eyebrow: { height: 14, fontSize: 11, letterSpacing: 1.4 },
  brandTitle: { height: 32, fontSize: 28 },
  brandBody: { fontSize: 13, lineHeight: 18 },
  nav: { gap: 10 },
  navItem: {
    height: 46,
    paddingX: 14,
    borderRadius: 14,
    borderWidth: 1,
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
  },
  navText: { height: 20, fontSize: 14 },
  content: { flex: 1, padding: 20, gap: 16 },
  header: { height: 56, flexDirection: 'row', alignItems: 'center', gap: 12 },
  headerTitle: { height: 30, fontSize: 26 },
  chip: {
    height: 28,
    paddingX: 12,
    borderRadius: 14,
    alignItems: 'center',
    justifyContent: 'center',
    borderWidth: 1,
  },
  chipText: { height: 16, fontSize: 12, fontWeight: 600 },
  spacer: { flex: 1 },
  toggle: {
    width: 108,
    height: 40,
    borderRadius: 20,
    borderWidth: 1,
    alignItems: 'center',
    justifyContent: 'center',
  },
  panel: {
    borderRadius: 20,
    borderWidth: 1,
    padding: 18,
    gap: 14,
    shadowOffsetY: 12,
    shadowBlur: 28,
    shadowSpread: 0,
  },
  panelTitle: { height: 22, fontSize: 18 },
  panelBody: { fontSize: 13, lineHeight: 18 },
  statsRow: { flexDirection: 'row', gap: 16 },
  statCard: { flex: 1, minHeight: 132 },
  statValue: { height: 44, fontSize: 34 },
  statMeta: { height: 18, fontSize: 13 },
  lower: { flex: 1, flexDirection: 'row', gap: 16 },
  listCard: { flex: 1 },
  controlsCard: { width: 360 },
  scroll: { flex: 1, minHeight: 360 },
  scrollContent: { gap: 10 },
  feedItem: {
    minHeight: 56,
    paddingX: 14,
    paddingY: 12,
    borderRadius: 14,
    borderWidth: 1,
    flexDirection: 'row',
    alignItems: 'center',
    gap: 12,
  },
  feedDot: { width: 8, height: 8, borderRadius: 4 },
  feedText: { flex: 1, fontSize: 13, lineHeight: 18 },
  input: { height: 42, paddingX: 12, borderRadius: 12, borderWidth: 1, fontSize: 14 },
  buttonRow: { flexDirection: 'row', gap: 10, height: 42 },
  action: { flex: 1, height: 42, borderRadius: 12, alignItems: 'center', justifyContent: 'center' },
  actionText: { fontSize: 13, fontWeight: 600 },
  footer: { height: 32, flexDirection: 'row', alignItems: 'center', gap: 10, marginTop: 'auto' },
  statusDot: { width: 10, height: 10, borderRadius: 5 },
  footerText: { height: 18, fontSize: 13 },
});

function reveal(progress: number, index: number): { opacity: number; marginTop: number } {
  const start = index * 0.14;
  const t = Math.max(0, Math.min(1, (progress - start) / 0.4));
  return { opacity: t, marginTop: (1 - t) * 18 };
}

function Showcase(): Node {
  const [mode, setMode] = useState<'dark' | 'light'>('dark');
  const [nav, setNav] = useState<(typeof NAV)[number]>('Overview');
  const [query, setQuery] = useState('');
  const [progress, setProgress] = useState(0);
  const entry = useMemo(() => new Animated.Value(0), []);
  const colors = palette[mode];

  useEffect(() => {
    const unlisten = entry.addListener((value) => setProgress(value));
    const animation = Animated.timing(entry, {
      to: 1,
      duration: 680,
      easing: Animated.Easings.materialDecelerate,
    });
    animation.start();
    return () => {
      unlisten();
      animation.stop();
    };
  }, [entry]);

  return (
    <ThemeProvider value={mode === 'dark' ? defaultTheme : lightTheme}>
      <View style={[s.root, { backgroundColor: colors.app }]}>
        <View style={[s.shell, { backgroundColor: colors.panel, borderColor: colors.line }]}>
          <View
            style={[s.sidebar, { backgroundColor: colors.panelAlt, borderRightColor: colors.line }]}
          >
            <View style={s.brand}>
              <Text style={[s.eyebrow, { color: colors.accent }]}>DESIGN SYSTEM</Text>
              <Text style={[s.brandTitle, { color: colors.text }]}>fxe showcase</Text>
              <Text style={[s.brandBody, { color: colors.muted }]}>
                A single-window demo for layout, input, motion, and interaction polish.
              </Text>
            </View>
            <View style={s.nav}>
              {NAV.map((item) => {
                const active = item === nav;
                return (
                  <Pressable
                    key={item}
                    onPress={() => setNav(item)}
                    style={(state) => [
                      s.navItem,
                      {
                        backgroundColor: active
                          ? colors.accentSoft
                          : state.pressed
                            ? colors.panelSoft
                            : state.hovered
                              ? colors.cardAlt
                              : colors.panelAlt,
                        borderColor: active || state.hovered ? colors.lineStrong : colors.line,
                      },
                    ]}
                  >
                    {(state) => (
                      <>
                        <Text
                          style={[
                            s.navText,
                            { color: active || state.hovered ? colors.text : colors.muted },
                          ]}
                        >
                          {item}
                        </Text>
                        <Text style={[s.navText, { color: active ? colors.accent : colors.muted }]}>
                          {active ? '•' : '›'}
                        </Text>
                      </>
                    )}
                  </Pressable>
                );
              })}
            </View>
            <View style={s.footer}>
              <View style={[s.statusDot, { backgroundColor: colors.success }]} />
              <Text style={[s.footerText, { color: colors.muted }]}>Ready for review</Text>
            </View>
          </View>
          <View style={s.content}>
            <View style={s.header}>
              <Text style={[s.headerTitle, { color: colors.text }]}>Operations overview</Text>
              <View
                style={[
                  s.chip,
                  { backgroundColor: colors.accentSoft, borderColor: colors.lineStrong },
                ]}
              >
                <Text style={[s.chipText, { color: colors.accent }]}>v0.4 preview</Text>
              </View>
              <View style={s.spacer} />
              <Pressable
                onPress={() => setMode((value) => (value === 'dark' ? 'light' : 'dark'))}
                style={(state) => [
                  s.toggle,
                  {
                    backgroundColor: state.pressed ? colors.panelSoft : colors.panelAlt,
                    borderColor: state.hovered ? colors.lineStrong : colors.line,
                  },
                ]}
              >
                {(state) => (
                  <Text style={[s.chipText, { color: state.hovered ? colors.text : colors.muted }]}>
                    {mode === 'dark' ? 'Light theme' : 'Dark theme'}
                  </Text>
                )}
              </Pressable>
            </View>
            <View style={s.statsRow}>
              {STATS.map(([label, value, meta], index) => {
                const motion = reveal(progress, index);
                return (
                  <View
                    key={label}
                    style={[
                      s.panel,
                      s.statCard,
                      {
                        backgroundColor: colors.card,
                        borderColor: colors.line,
                        shadowColor: colors.shadow,
                        opacity: motion.opacity,
                        marginTop: motion.marginTop,
                      },
                    ]}
                  >
                    <Text style={[s.eyebrow, { color: colors.muted }]}>{label}</Text>
                    <Text style={[s.statValue, { color: colors.text }]}>{value}</Text>
                    <Text style={[s.statMeta, { color: colors.accent }]}>{meta}</Text>
                  </View>
                );
              })}
            </View>
            <View style={s.lower}>
              <View
                style={[
                  s.panel,
                  s.listCard,
                  {
                    backgroundColor: colors.card,
                    borderColor: colors.line,
                    shadowColor: colors.shadow,
                    ...reveal(progress, 3),
                  },
                ]}
              >
                <Text style={[s.panelTitle, { color: colors.text }]}>Activity feed</Text>
                <ScrollView style={s.scroll} contentStyle={s.scrollContent}>
                  {FEED.map((item, index) => (
                    <View
                      key={item}
                      style={[
                        s.feedItem,
                        { backgroundColor: colors.cardAlt, borderColor: colors.line },
                      ]}
                    >
                      <View
                        style={[
                          s.feedDot,
                          { backgroundColor: index % 3 === 0 ? colors.accent : colors.success },
                        ]}
                      />
                      <Text style={[s.feedText, { color: colors.text }]}>{item}</Text>
                    </View>
                  ))}
                </ScrollView>
              </View>
              <View
                style={[
                  s.panel,
                  s.controlsCard,
                  {
                    backgroundColor: colors.card,
                    borderColor: colors.line,
                    shadowColor: colors.shadow,
                    ...reveal(progress, 4),
                  },
                ]}
              >
                <Text style={[s.panelTitle, { color: colors.text }]}>Quick actions</Text>
                <Text style={[s.panelBody, { color: colors.muted }]}>
                  Filter the queue, stage an action, or clear the draft state without leaving the
                  screen.
                </Text>
                <TextInput
                  style={[
                    s.input,
                    {
                      backgroundColor: colors.input,
                      borderColor: colors.lineStrong,
                      color: colors.text,
                    },
                  ]}
                  value={query}
                  placeholder="Search tickets, owners, or release tags"
                  onChange={setQuery}
                />
                <View style={s.buttonRow}>
                  <Button
                    title="Deploy"
                    style={[s.action, { backgroundColor: colors.accent }]}
                    textStyle={[s.actionText, { color: 0xffffffff }]}
                  />
                  <Button
                    title="Review"
                    style={[
                      s.action,
                      {
                        backgroundColor: colors.panelSoft,
                        borderWidth: 1,
                        borderColor: colors.lineStrong,
                      },
                    ]}
                    textStyle={[s.actionText, { color: colors.text }]}
                  />
                  <Button
                    title="Archive"
                    style={[s.action, { backgroundColor: colors.danger }]}
                    textStyle={[s.actionText, { color: 0xffffffff }]}
                  />
                </View>
                <View
                  style={[
                    s.feedItem,
                    { backgroundColor: colors.panelSoft, borderColor: colors.line },
                  ]}
                >
                  <View style={[s.statusDot, { backgroundColor: colors.success }]} />
                  <Text style={[s.feedText, { color: colors.text }]}>
                    {query
                      ? `Prepared filter: ${query}`
                      : 'No filter applied. Keyboard focus lands in the search field first.'}
                  </Text>
                </View>
                <View style={s.footer}>
                  <View style={[s.statusDot, { backgroundColor: colors.success }]} />
                  <Text style={[s.footerText, { color: colors.muted }]}>
                    All services nominal · 12 ms input latency
                  </Text>
                </View>
              </View>
            </View>
          </View>
        </View>
      </View>
    </ThemeProvider>
  );
}

const win = new Window({ width: 1280, height: 800, title: 'fxe — showcase' });
mount(<Showcase />, win);
App.run();
