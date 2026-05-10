// Type-only smoke test for the Audio binding. Guarded by `if (false)` so it
// never executes — a `just ts-check` run is enough to verify the public
// surface declared in `types/fxe.d.ts` (Audio + Sound) compiles.

if (globalThis.__FXE_TYPECHECK_ONLY__ === true) {
  const s = await Audio.load('a.wav');
  s.play({ volume: 0.5 });
  s.play({ volume: 1.0, loop: true, rate: 1.25 });
  s.stop();
  s.dispose();

  const bytes = new Uint8Array(0);
  const s2 = await Audio.loadFromBytes(bytes);
  s2.play({});
  s2.dispose();

  Audio.setMasterVolume(0.75);
}

export {};
