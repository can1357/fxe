import { assert, assertEqual, test } from './ts_harness.ts';

if (globalThis.__FXE_TYPECHECK_ONLY__ === true) {
  const openResult: Promise<OpenDialogResult> = dialog.showOpenDialog({
    title: 'Open file',
    defaultPath: '/tmp',
    multiple: true,
    directories: false,
    filters: [{ name: 'Text', extensions: ['txt', 'md'] }],
  });

  const saveResult: Promise<SaveDialogResult> = dialog.showSaveDialog({
    title: 'Save file',
    defaultPath: '/tmp/out.txt',
    filters: [{ name: 'Text', extensions: ['txt'] }],
  });

  const messageResult: Promise<MessageBoxResult> = dialog.showMessageBox({
    title: 'Question',
    message: 'Continue?',
    detail: 'Dialog type-level coverage only.',
    buttons: ['Yes', 'No'],
    type: 'question',
  });

  void openResult;
  void saveResult;
  void messageResult;

  // @ts-expect-error showOpenDialog requires an options object.
  dialog.showOpenDialog();
  // @ts-expect-error filter extensions must be strings.
  dialog.showOpenDialog({ filters: [{ name: 'Bad', extensions: [1] }] });
  // @ts-expect-error showSaveDialog defaultPath must be a string.
  dialog.showSaveDialog({ defaultPath: 123 });
  // @ts-expect-error message box type is restricted to supported values.
  dialog.showMessageBox({ type: 'fatal' });
  // @ts-expect-error message box buttons must be strings.
  dialog.showMessageBox({ buttons: [0] });
}

test('dialog global exposes callable methods', () => {
  assert(typeof dialog === 'object' && dialog !== null, 'dialog global should exist');
  assertEqual(typeof dialog.showOpenDialog, 'function');
  assertEqual(typeof dialog.showSaveDialog, 'function');
  assertEqual(typeof dialog.showMessageBox, 'function');
});
