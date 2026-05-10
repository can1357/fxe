import { focusTarget } from '../mount/focus.ts';

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
  const id = await Menu.popup(buildEditMenuItems(opts), x, y);
  if (id === null || id === undefined) return null;

  for (const action of EDIT_ACTIONS) {
    if (EDIT_IDS[action] === id) return action;
  }
  return null;
}

// ----------------------------------------------------------------------------
// Application-menu (menu-bar) integration.
//
// Native menus reach edit actions via global accelerators (Cmd+Z, Cmd+X, ...)
// and dispatch them by `MenuItem.id` through the C++ menu callback bridge.
// `installApplicationEditMenu` wires that bridge to the focused TextInput /
// TextArea via the `onEditCommand` HitTarget hook.
//
// macOS apps gain an "Edit" menu with first-responder-style accelerators;
// Windows + Linux apps that explicitly install a menu bar (none by default)
// pick up the same routing.
// ----------------------------------------------------------------------------

/**
 * Build the standard "Edit" submenu (Undo / Redo / Cut / Copy / Paste /
 * Select All) suitable for installation under the application menu bar via
 * `Menu.setApplicationMenu`. Items are always enabled — actions no-op when
 * the focused target cannot service them, matching native menu convention.
 */
export function buildApplicationEditSubmenu(): MenuItem {
  return {
    label: 'Edit',
    type: 'submenu',
    submenu: [
      { id: EDIT_IDS.undo, label: 'Undo', accelerator: 'CmdOrCtrl+Z' },
      { id: EDIT_IDS.redo, label: 'Redo', accelerator: 'Shift+CmdOrCtrl+Z' },
      { type: 'separator' },
      { id: EDIT_IDS.cut, label: 'Cut', accelerator: 'CmdOrCtrl+X' },
      { id: EDIT_IDS.copy, label: 'Copy', accelerator: 'CmdOrCtrl+C' },
      { id: EDIT_IDS.paste, label: 'Paste', accelerator: 'CmdOrCtrl+V' },
      { type: 'separator' },
      { id: EDIT_IDS.selectAll, label: 'Select All', accelerator: 'CmdOrCtrl+A' },
    ],
  };
}

/**
 * Map an application-menu `MenuItem.id` to its EditMenuAction, or `null`
 * if it is not an edit action.
 */
export function editActionFromMenuId(id: string): EditMenuAction | null {
  for (const action of EDIT_ACTIONS) {
    if (EDIT_IDS[action] === id) return action;
  }
  return null;
}

export interface InstallApplicationEditMenuOptions {
  /**
   * Extra top-level menu items rendered alongside the Edit submenu. The
   * Edit submenu is always inserted first so platform menu-bar conventions
   * (macOS App > Edit > Window) remain navigable.
   */
  extras?: MenuItem[];
  /**
   * Override the dispatch target. Defaults to routing each EditMenuAction
   * to the focused HitTarget's `onEditCommand` hook (`TextInput` /
   * `TextArea` implement it). Use this to bridge into a custom command
   * router, e.g. when an app wants Undo/Redo to drive a document model
   * outside any text input.
   */
  dispatch?: (action: EditMenuAction) => void;
}

/**
 * Install an Edit submenu into the application menu bar and route
 * activations to the focused text component. Returns a disposer that
 * clears both the menu and the command handler.
 *
 * Idempotent: calling twice replaces the previous installation.
 */
export function installApplicationEditMenu(
  opts: InstallApplicationEditMenuOptions = {},
): () => void {
  const extras = opts.extras ?? [];
  const items: MenuItem[] = [buildApplicationEditSubmenu(), ...extras];
  Menu.setApplicationMenu(items);

  const dispatch =
    opts.dispatch ??
    ((action: EditMenuAction) => {
      const target = focusTarget();
      target?.onEditCommand?.(action);
    });

  Menu.onCommand((id) => {
    const action = editActionFromMenuId(id);
    if (action) dispatch(action);
  });

  return () => {
    Menu.onCommand(null);
    Menu.setApplicationMenu([]);
  };
}
