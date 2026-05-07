import { assert, assertEqual, test } from './ts_harness.ts';

const simpleApplicationMenu: MenuItem[] = [
  {
    id: 'file',
    label: 'File',
    type: 'submenu',
    submenu: [
      { id: 'new', label: 'New', accelerator: 'CmdOrCtrl+N', enabled: true },
      { type: 'separator' },
      { id: 'show-hidden', label: 'Show Hidden', type: 'checkbox', checked: false, enabled: true },
    ],
  },
];

if (
  (globalThis as typeof globalThis & { __FXE_TYPECHECK_ONLY__?: boolean })
    .__FXE_TYPECHECK_ONLY__ === true
) {
  const popupSignature: (items: MenuItem[], x: number, y: number) => Promise<string | null> =
    Menu.popup;
  const setApplicationMenuSignature: (items: MenuItem[]) => void = Menu.setApplicationMenu;
  void popupSignature;
  void setApplicationMenuSignature;
}

test('Menu exposes noninteractive functions', () => {
  assertEqual(typeof Menu.setApplicationMenu, 'function');
  assertEqual(typeof Menu.popup, 'function');
});

test('Menu.setApplicationMenu accepts empty and simple menus', () => {
  Menu.setApplicationMenu([]);
  Menu.setApplicationMenu(simpleApplicationMenu);
  Menu.setApplicationMenu([]);
});

test('Menu exposes mutable item lookup APIs', () => {
  assertEqual(Menu.findItem('non-existent'), null);
  assertEqual(Menu.updateItem('non-existent', { label: 'x' }), false);

  Menu.setApplicationMenu([
    {
      id: 'mfile',
      label: 'File',
      submenu: [{ id: 'mopen', label: 'Open' }],
    },
  ]);
  try {
    const handle = Menu.findItem('mopen');
    assert(handle !== null, 'Menu.findItem should return handle for existing item');
    assertEqual(handle.id, 'mopen');
    assertEqual(handle.setLabel('Open…'), undefined);
    assert(Menu.findItem('mopen') !== null, 'Menu.findItem should still work after setLabel');
    assertEqual(handle.setEnabled(false), undefined);

    if (process.platform === 'darwin') {
      assertEqual(Menu.updateItem('mopen', { checked: true }), true);
    }
  } finally {
    Menu.setApplicationMenu([]);
  }
});
