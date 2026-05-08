/** @jsxImportSource fxe-ui */
import { Button, Pressable, ScrollView, Text, View, useEffect, useMemo, useState } from 'fxe-ui';
import type { CdpClient } from '../cdp_client.ts';

interface ConsoleMessage {
  level: string;
  text: string;
  ts: number;
}

type FilterLevel = 'log' | 'info' | 'warn' | 'error';

type FilterState = Record<FilterLevel, boolean>;

const DEFAULT_FILTERS: FilterState = {
  log: true,
  info: true,
  warn: true,
  error: true,
};

const LEVEL_COLORS: Record<FilterLevel, number> = {
  log: 0xe2e8f0ff,
  info: 0x66aaffff,
  warn: 0xfbbf24ff,
  error: 0xff6b6bff,
};

export function ConsolePanel({ cdp }: { cdp: CdpClient }) {
  const [messages, setMessages] = useState<ConsoleMessage[]>([]);
  const [filters, setFilters] = useState<FilterState>(DEFAULT_FILTERS);

  useEffect(() => {
    void cdp.send('Console.enable').catch(() => {});
    return cdp.on('Console.messageAdded', (params) => {
      const message = params as { level?: string; text?: string; ts?: number };
      setMessages((prev) =>
        [
          ...prev,
          { level: message.level ?? 'log', text: message.text ?? '', ts: message.ts ?? 0 },
        ].slice(-1000),
      );
    });
  }, [cdp]);

  const visibleMessages = useMemo(
    () =>
      messages.filter((message) => {
        if (message.level in filters) return filters[message.level as FilterLevel];
        return true;
      }),
    [messages, filters],
  );

  const toggleLevel = (level: FilterLevel): void => {
    setFilters((prev) => ({ ...prev, [level]: !prev[level] }));
  };

  return (
    <View style={{ width: '100%', height: '100%', gap: 8 }}>
      <View style={{ flexDirection: 'row', gap: 8, paddingTop: 4, paddingBottom: 4 }}>
        {(['log', 'info', 'warn', 'error'] as const).map((level) => (
          <Pressable
            key={level}
            onPress={() => toggleLevel(level)}
            style={{
              paddingX: 10,
              paddingY: 6,
              backgroundColor: filters[level] ? 0x1d4ed8ff : 0x1f2937ff,
            }}
          >
            <Text style={{ color: LEVEL_COLORS[level], fontSize: 13 }}>{level}</Text>
          </Pressable>
        ))}
        <Button title="Clear" onPress={() => setMessages([])} />
      </View>
      <ScrollView style={{ width: '100%', height: '100%' }}>
        {messages.length === 0 ? (
          <Text style={{ color: 0x94a3b8ff, fontSize: 14 }}>Waiting for Console.messageAdded…</Text>
        ) : visibleMessages.length === 0 ? (
          <Text style={{ color: 0x94a3b8ff, fontSize: 14 }}>
            No messages match the active filters.
          </Text>
        ) : (
          visibleMessages.map((message, index) => {
            const level = message.level in LEVEL_COLORS ? (message.level as FilterLevel) : null;
            return (
              <Text
                key={`${message.ts}-${index}`}
                style={{ color: level ? LEVEL_COLORS[level] : 0xe2e8f0ff, fontSize: 13 }}
              >
                [{message.level}] {message.text}
              </Text>
            );
          })
        )}
      </ScrollView>
    </View>
  );
}
