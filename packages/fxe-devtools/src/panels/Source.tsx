/** @jsxImportSource fxe-ui */
import { Pressable, ScrollView, Text, useEffect, useState, View } from 'fxe-ui';
import type { CdpClient } from '../cdp_client.ts';

interface ScriptEntry {
  id: string;
  url: string;
}

export function SourcePanel({ cdp }: { cdp: CdpClient }) {
  const [scripts, setScripts] = useState<ScriptEntry[]>([]);
  const [body, setBody] = useState('');
  const [selectedScriptId, setSelectedScriptId] = useState<string | null>(null);

  useEffect(() => {
    void cdp.send('Debugger.enable').catch(() => {});
    return cdp.on('Debugger.scriptParsed', (params) => {
      const event = params as { scriptId?: string; url?: string };
      const scriptId = event.scriptId;
      if (!scriptId) return;
      setScripts((prev) => {
        const next = prev.filter((entry) => entry.id !== scriptId);
        next.push({ id: scriptId, url: event.url ?? '<anon>' });
        return next.slice(-200);
      });
    });
  }, [cdp]);

  const loadSource = async (script: ScriptEntry): Promise<void> => {
    setSelectedScriptId(script.id);
    const response = (await cdp
      .send('Debugger.getScriptSource', { scriptId: script.id })
      .catch(() => null)) as { scriptSource?: string } | null;
    setBody(response?.scriptSource ?? '');
  };

  return (
    <View style={{ flexDirection: 'row', flex: 1 }}>
      <ScrollView style={{ width: 240, height: '100%', backgroundColor: 0x111827ff }}>
        {scripts.length === 0 ? (
          <Text style={{ color: 0x94a3b8ff, fontSize: 13, padding: 8 }}>
            Waiting for Debugger.scriptParsed…
          </Text>
        ) : (
          scripts.map((script) => (
            <Pressable
              key={script.id}
              onPress={() => void loadSource(script)}
              style={{
                padding: 4,
                backgroundColor: selectedScriptId === script.id ? 0x1d4ed8ff : 0x111827ff,
              }}
            >
              <Text style={{ color: 0xddddddff, fontSize: 12 }}>{script.url}</Text>
            </Pressable>
          ))
        )}
      </ScrollView>
      <ScrollView style={{ flex: 1, height: '100%', backgroundColor: 0x020617ff }}>
        <Text style={{ color: 0xeeeeeeff, fontSize: 11 }}>{body || 'Select a script.'}</Text>
      </ScrollView>
    </View>
  );
}
