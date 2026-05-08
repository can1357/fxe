/** @jsxImportSource fxe-ui */

import { App, Window } from 'fxe';
import { Button, mount, type Node, StyleSheet, Text, TextInput, useState, View } from 'fxe-ui';

const s = StyleSheet.create({
  root: {
    width: '100%',
    height: '100%',
    alignItems: 'center',
    justifyContent: 'center',
    backgroundColor: 0x00000000,
  },
  // Outer glow / soft halo behind the card
  halo: {
    position: 'absolute',
    width: 620,
    height: 480,
    borderRadius: 32,
    backgroundColor: 0x6f7bff14,
  },
  haloAccent: {
    position: 'absolute',
    width: 280,
    height: 280,
    borderRadius: 140,
    backgroundColor: 0xd8b66a1a,
    top: 60,
    left: 60,
  },
  // Frosted glass card
  shell: {
    width: 540,
    height: 360,
    flexDirection: 'row',
    borderRadius: 20,
    borderWidth: 1,
    borderColor: 0xffffff1f,
    backgroundColor: 0x0c1018cc,
    overflow: 'hidden',
  },
  // Left rail — accent gradient-ish stack
  rail: {
    width: 180,
    height: 360,
    padding: 22,
    gap: 10,
    backgroundColor: 0x12182366,
    borderRightWidth: 1,
    borderRightColor: 0xffffff14,
  },
  railKicker: { height: 14, color: 0xd8b66aff, fontSize: 10, letterSpacing: 2 },
  railWordmark: { height: 38, color: 0xf5f0e8ff, fontSize: 30 },
  railRule: {
    width: 28,
    height: 2,
    marginTop: 4,
    marginBottom: 6,
    backgroundColor: 0xd8b66aff,
  },
  railBody: { color: 0x9ca8baff, fontSize: 12, lineHeight: 17 },
  railFooter: {
    marginTop: 'auto',
    color: 0x7a8699ff,
    fontSize: 10,
    letterSpacing: 1,
    height: 14,
  },
  railBadge: {
    width: 116,
    height: 24,
    marginTop: 12,
    paddingX: 10,
    borderRadius: 12,
    alignItems: 'center',
    justifyContent: 'center',
    flexDirection: 'row',
    gap: 6,
    backgroundColor: 0xd8b66a26,
    borderWidth: 1,
    borderColor: 0xd8b66a55,
  },
  railBadgeText: { height: 14, color: 0xefd591ff, fontSize: 10, letterSpacing: 1 },
  // Right panel
  panel: {
    width: 360,
    height: 360,
    paddingX: 28,
    paddingY: 30,
    gap: 8,
    justifyContent: 'center',
  },
  eyebrow: { height: 14, color: 0x8a93a8ff, fontSize: 10, letterSpacing: 2 },
  title: { height: 34, color: 0xf5f0e8ff, fontSize: 26 },
  copy: { color: 0x9da9bdff, fontSize: 12, lineHeight: 17, marginBottom: 4 },
  form: { gap: 8, marginTop: 6 },
  label: { height: 14, color: 0xb6c0d2ff, fontSize: 11, letterSpacing: 1 },
  input: {
    width: 304,
    height: 38,
    paddingX: 12,
    borderRadius: 10,
    borderWidth: 1,
    borderColor: 0xffffff1a,
    backgroundColor: 0x0a0f1980,
    color: 0xf3ecdeff,
    fontSize: 13,
  },
  button: {
    width: 304,
    height: 44,
    marginTop: 12,
    borderRadius: 10,
    backgroundColor: 0xd8b66aff,
    alignItems: 'center',
    justifyContent: 'center',
  },
  buttonText: { color: 0x17130aff, fontSize: 14, letterSpacing: 1 },
  foot: { height: 16, color: 0xd8b66aff, fontSize: 11, marginTop: 8 },
  // Custom traffic-light cluster (since we're frameless)
  dragBar: {
    position: 'absolute',
    top: 0,
    left: 0,
    width: 540,
    height: 28,
    flexDirection: 'row',
    alignItems: 'center',
    paddingX: 12,
    gap: 6,
  },
  dot: { width: 10, height: 10, borderRadius: 5 },
  dotClose: { backgroundColor: 0xff5f57ff },
  dotMin: { backgroundColor: 0xfebc2eff },
  dotMax: { backgroundColor: 0x28c840ff },
});

function LoginForm(): Node {
  const [email, setEmail] = useState('');
  const [password, setPassword] = useState('');
  const [message, setMessage] = useState('');

  const submit = (): void => {
    setMessage(
      email.includes('@') && password.length >= 6
        ? '✓ Credentials look ready.'
        : 'Use an email and a 6+ character password.',
    );
  };

  return (
    <View style={s.root}>
      <View style={s.halo} />
      <View style={s.haloAccent} />
      <View style={s.shell}>
        <View style={s.dragBar}>
          <View style={[s.dot, s.dotClose]} />
          <View style={[s.dot, s.dotMin]} />
          <View style={[s.dot, s.dotMax]} />
        </View>
        <View style={s.rail}>
          <Text style={s.railKicker}>FXE · ACCESS</Text>
          <Text style={s.railWordmark}>Sign in</Text>
          <View style={s.railRule} />
          <Text style={s.railBody}>
            A compact native login surface tuned for keyboard-first workflows.
          </Text>
          <View style={s.railBadge}>
            <Text style={s.railBadgeText}>🔒 ENCRYPTED</Text>
          </View>
          <Text style={s.railFooter}>v1 · LOCAL</Text>
        </View>
        <View style={s.panel}>
          <Text style={s.eyebrow}>ACCOUNT · SIGN IN</Text>
          <Text style={s.title}>Welcome back 👋</Text>
          <Text style={s.copy}>
            Pick up where you left off with the credentials you use every day.
          </Text>
          <View style={s.form}>
            <Text style={s.label}>EMAIL</Text>
            <TextInput
              style={s.input}
              value={email}
              placeholder="you@example.com"
              onChange={setEmail}
              onSubmit={submit}
            />
            <Text style={s.label}>PASSWORD</Text>
            <TextInput
              style={s.input}
              value={password}
              placeholder="minimum 6 characters"
              onChange={setPassword}
              onSubmit={submit}
            />
            <Button title="Continue →" style={s.button} textStyle={s.buttonText} onPress={submit} />
          </View>
          <Text style={s.foot}>{message || 'Tab between fields, then press Enter.'}</Text>
        </View>
      </View>
    </View>
  );
}

const win = new Window({
  width: 620,
  height: 480,
  title: 'fxe-ui login form',
  transparent: true,
  decorated: false,
});
mount(<LoginForm />, win);
App.run();
