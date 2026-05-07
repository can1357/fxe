import { assert, assertEqual, assertRejects, test } from './ts_harness.ts';

const missingAudioPath = '__fxe_missing_audio_file__.wav';

// Type-only coverage for the declared Sound surface. This function is never
// executed; it keeps the test deterministic and avoids requiring audio output.
function acceptsSoundShape(sound: Sound): void {
  sound.play();
  sound.play({ volume: 0.5, loop: true, rate: 1.25 });
  sound.stop();
  sound.dispose();
}

void acceptsSoundShape;

function acceptsCaptureShape(session: CaptureSession, device: AudioDeviceInfo): void {
  const id: string = device.id;
  const name: string = device.name;
  const isDefault: boolean = device.isDefault;
  void id;
  void name;
  void isDefault;
  Audio.startCapture({ sampleRate: 48000, channels: 1, deviceId: device.id }, (samples, info) => {
    const view: Float32Array = samples;
    const frameCount: number = info.frameCount;
    const channels: number = info.channels;
    const sampleRate: number = info.sampleRate;
    void view;
    void frameCount;
    void channels;
    void sampleRate;
  });
  session.stop();
}

void acceptsCaptureShape;

test('Audio.setMasterVolume accepts finite volume values', () => {
  assertEqual(typeof Audio.setMasterVolume, 'function');
  Audio.setMasterVolume(0);
  Audio.setMasterVolume(0.5);
  Audio.setMasterVolume(1);
});

test('Audio.enumerateDevices returns input device array', () => {
  assertEqual(typeof Audio.enumerateDevices, 'function');
  const devices = Audio.enumerateDevices('input');
  assert(Array.isArray(devices), 'expected input devices to be an array');
  if (devices.length === 0) {
    return;
  }
  const first = devices[0];
  assertEqual(typeof first.id, 'string');
  assertEqual(typeof first.name, 'string');
  assertEqual(typeof first.isDefault, 'boolean');
});

test('Audio.load rejects a missing file', async () => {
  await assertRejects(() => Audio.load(missingAudioPath), /Audio\.load: failed to load sound/);
});

test('Audio.loadFromBytes rejects invalid bytes', async () => {
  const invalidBytes = new Uint8Array([0x66, 0x78, 0x65, 0x00, 0xff]);
  await assertRejects(
    () => Audio.loadFromBytes(invalidBytes),
    /Audio\.loadFromBytes: failed to decode/,
  );
});

test('Sound type is available in type-only code', () => {
  type SoundShape = {
    play(options?: SoundPlayOptions): void;
    stop(): void;
    dispose(): void;
  };

  const assertSoundShape = (sound: Sound): SoundShape => sound;
  assert(typeof assertSoundShape === 'function');
});
