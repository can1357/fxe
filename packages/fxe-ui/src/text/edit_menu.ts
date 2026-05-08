export type EditMenuAction = 'undo' | 'redo' | 'cut' | 'copy' | 'paste' | 'selectAll';

export interface EditMenuOptions {
  /** Disable Cut/Copy when no selection. */
  hasSelection?: boolean;
  /** Disable Undo/Redo independently. */
  canUndo?: boolean;
  canRedo?: boolean;
  /** Disable Paste (for read-only / disabled inputs). */
  canPaste?: boolean;
  /** Disable everything except Copy/Select All (for read-only fields). */
  readOnly?: boolean;
  /** Disable absolutely everything (for fully disabled inputs — prefer not popping menu at all in caller). */
  disabled?: boolean;
}

interface MenuItem {
  id?: string;
  label?: string;
  accelerator?: string;
  enabled?: boolean;
  type?: 'normal' | 'separator' | 'checkbox' | 'submenu';
  submenu?: MenuItem[];
  checked?: boolean;
  visible?: boolean;
}

// Internal: deterministic ids matching EditMenuAction names so popup result maps cleanly.
const EDIT_IDS: Record<EditMenuAction, string> = {
  undo: 'fxe.edit.undo',
  redo: 'fxe.edit.redo',
  cut: 'fxe.edit.cut',
  copy: 'fxe.edit.copy',
  paste: 'fxe.edit.paste',
  selectAll: 'fxe.edit.selectAll',
};

const EDIT_ACTIONS: EditMenuAction[] = ['undo', 'redo', 'cut', 'copy', 'paste', 'selectAll'];

export function buildEditMenuItems(opts: EditMenuOptions = {}): MenuItem[] {
  const ro = opts.readOnly === true;
  const dis = opts.disabled === true;
  const sel = opts.hasSelection === true;

  const canUndo = !dis && !ro && opts.canUndo === true;
  const canRedo = !dis && !ro && opts.canRedo === true;
  const canCut = !dis && !ro && sel;
  const canCopy = !dis && sel;
  const canPaste = !dis && !ro && (opts.canPaste ?? true);

  return [
    { id: EDIT_IDS.undo, label: 'Undo', accelerator: 'CmdOrCtrl+Z', enabled: canUndo },
    { id: EDIT_IDS.redo, label: 'Redo', accelerator: 'Shift+CmdOrCtrl+Z', enabled: canRedo },
    { type: 'separator' },
    { id: EDIT_IDS.cut, label: 'Cut', accelerator: 'CmdOrCtrl+X', enabled: canCut },
    { id: EDIT_IDS.copy, label: 'Copy', accelerator: 'CmdOrCtrl+C', enabled: canCopy },
    { id: EDIT_IDS.paste, label: 'Paste', accelerator: 'CmdOrCtrl+V', enabled: canPaste },
    { type: 'separator' },
    { id: EDIT_IDS.selectAll, label: 'Select All', accelerator: 'CmdOrCtrl+A', enabled: !dis },
  ];
}

export async function popupEditMenu(
  x: number,
  y: number,
  opts: EditMenuOptions = {},
): Promise<EditMenuAction | null> {
  const menu = (
    globalThis as {
      Menu?: { popup: (items: MenuItem[], x: number, y: number) => Promise<string | null> };
    }
  ).Menu;

  if (!menu?.popup) return null;

  const id = await menu.popup(buildEditMenuItems(opts), x, y);
  if (id === null || id === undefined) return null;

  for (const action of EDIT_ACTIONS) {
    if (EDIT_IDS[action] === id) return action;
  }
  return null;
}
