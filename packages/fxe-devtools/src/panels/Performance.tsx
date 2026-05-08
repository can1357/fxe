/** @jsxImportSource fxe-ui */
import { Button, Text, useState, View } from 'fxe-ui';
import type { CdpClient } from '../cdp_client.ts';

interface CpuProfile {
  samples?: number[];
  nodes?: unknown[];
}

export function PerformancePanel({ cdp }: { cdp: CdpClient }) {
  const [recording, setRecording] = useState(false);
  const [profile, setProfile] = useState<CpuProfile | null>(null);
  const [status, setStatus] = useState('Idle');

  const toggleRecording = async (): Promise<void> => {
    if (!recording) {
      setStatus('Starting profiler…');
      await cdp.send('Profiler.enable').catch(() => {});
      await cdp.send('Profiler.start').catch(() => {});
      setRecording(true);
      setStatus('Recording…');
      return;
    }

    setStatus('Stopping profiler…');
    const result = (await cdp.send('Profiler.stop').catch(() => null)) as {
      profile?: CpuProfile;
    } | null;
    setProfile(result?.profile ?? null);
    setRecording(false);
    setStatus(result?.profile ? 'Profile captured' : 'Profiler stopped with no profile');
  };

  return (
    <View style={{ flexDirection: 'column', gap: 8, padding: 8 }}>
      <View style={{ flexDirection: 'row', gap: 8 }}>
        <Button title={recording ? 'Stop' : 'Start'} onPress={() => void toggleRecording()} />
      </View>
      <Text style={{ color: 0x94a3b8ff, fontSize: 13 }}>{status}</Text>
      <Text style={{ color: 0xe2e8f0ff, fontSize: 14 }}>
        {profile
          ? `Samples: ${profile.samples?.length ?? 0}, Nodes: ${profile.nodes?.length ?? 0}`
          : 'No profile yet.'}
      </Text>
    </View>
  );
}
