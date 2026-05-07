/** @jsxImportSource fxe-ui */

import { App, Window } from 'fxe';
import { Button, mount, type Node, StyleSheet, Text, TextInput, useState, View } from 'fxe-ui';

const s = StyleSheet.create({
  root: {
    width: '100%',
    height: '100%',
    alignItems: 'center',
    justifyContent: 'center',
    backgroundColor: 0x080b10ff,
  },
  shell: {
    width: 500,
    height: 320,
    flexDirection: 'row',
    gap: 18,
    padding: 18,
    borderWidth: 1,
    borderColor: 0x31384aff,
    backgroundColor: 0x101620ff,
  },
  rail: {
    width: 138,
    height: 284,
    padding: 16,
    gap: 10,
    backgroundColor: 0x171f30ff,
  },
  railKicker: { height: 16, color: 0x90a6c7ff, fontSize: 11, letterSpacing: 1 },
  railWordmark: { height: 34, color: 0xf3ecdeff, fontSize: 28 },
  railBody: { color: 0x9ca8baff, fontSize: 13, lineHeight: 18 },
  railBadge: {
    width: 106,
    height: 26,
    marginTop: 10,
    alignItems: 'center',
    justifyContent: 'center',
    backgroundColor: 0xd8b66aff,
  },
  railBadgeText: { height: 16, color: 0x17130aff, fontSize: 11 },
  panel: { width: 308, height: 284, gap: 10, justifyContent: 'center' },
  title: { height: 30, color: 0xf5f0e8ff, fontSize: 24 },
  copy: { color: 0x9da9bdff, fontSize: 13, lineHeight: 18 },
  form: { height: 172, gap: 8, marginTop: 10 },
  label: { height: 16, color: 0xc7d1e2ff, fontSize: 12 },
  input: {
    width: 308,
    height: 40,
    paddingX: 12,
    borderWidth: 1,
    borderColor: 0x384255ff,
    backgroundColor: 0x1a2130ff,
  },
  button: { width: 308, height: 50, marginTop: 10, backgroundColor: 0xd8b66aff },
  buttonText: { color: 0x17130aff, fontSize: 16 },
  foot: { height: 18, color: 0xd8b66aff, fontSize: 12 },
});

function LoginForm(): Node {
  const [email, setEmail] = useState('');
  const [password, setPassword] = useState('');
  const [message, setMessage] = useState('');

  const submit = (): void => {
    setMessage(
      email.includes('@') && password.length >= 6
        ? 'Credentials look ready.'
        : 'Use an email and a 6+ character password.',
    );
  };

  return (
    <View style={s.root}>
      <View style={s.shell}>
        <View style={s.rail}>
          <Text style={s.railKicker}>FXE ACCESS</Text>
          <Text style={s.railWordmark}>Sign in</Text>
          <Text style={s.railBody}>
            A compact native login surface tuned for keyboard-first workflows.
          </Text>
          <View style={s.railBadge}>
            <Text style={s.railBadgeText}>🔒 ENCRYPTED</Text>
          </View>
        </View>
        <View style={s.panel}>
          <Text style={s.title}>Welcome back 👋</Text>
          <Text style={s.copy}>
            Continue to your workspace with the credentials you use every day.
          </Text>
          <View style={s.form}>
            <Text style={s.label}>Email address</Text>
            <TextInput
              style={s.input}
              value={email}
              placeholder="you@example.com"
              onChange={setEmail}
              onSubmit={submit}
            />
            <Text style={s.label}>Password</Text>
            <TextInput
              style={s.input}
              value={password}
              placeholder="minimum 6 characters"
              onChange={setPassword}
              onSubmit={submit}
            />
            <Button title="Continue" style={s.button} textStyle={s.buttonText} onPress={submit} />
          </View>
          <Text style={s.foot}>{message || 'Tab between fields, then press Enter.'}</Text>
        </View>
      </View>
    </View>
  );
}

const win = new Window({ width: 560, height: 440, title: 'fxe-ui login form' });
mount(<LoginForm />, win, { lazy: false });
App.run({ animate: true, fps: 60 });
