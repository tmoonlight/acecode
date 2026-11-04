import { CONTEXT_SHORTCUTS, effectiveShortcut, getShortcutOverrides, isShortcutComposing,
  isShortcutRecording, matchesShortcutBinding } from './shortcutBindings.js';

// Each editor owns its history. 'ignore' blocks obsolete native/Slate defaults.
export function editingHistoryAction(event, overrides = getShortcutOverrides()) {
  if (isShortcutComposing(event) || isShortcutRecording(event)) return null;
  for (const id of ['undo', 'redo']) {
    if (matchesShortcutBinding(event, effectiveShortcut(id, overrides))) return id;
  }
  for (const id of ['undo', 'redo']) {
    if (Object.hasOwn(overrides, id) && matchesShortcutBinding(event, CONTEXT_SHORTCUTS[id])) return 'ignore';
  }
  return null;
}

export function handleEditorHistoryKey(event, editor, overrides) {
  const action = editingHistoryAction(event, overrides);
  if (!action) return false;
  event.preventDefault();
  event.stopPropagation();
  if (!event.repeat && action !== 'ignore') editor[action]();
  return true;
}

export function handleNativeHistoryKey(event, doc = globalThis.document) {
  if (event.defaultPrevented) return false;
  const target = event.target;
  if (target?.closest?.('.xterm, [data-slate-editor], [data-shortcut-recorder]')) return false;
  const input = target?.closest?.('input, textarea');
  if (!input || input.readOnly || input.disabled || (input.tagName === 'INPUT'
      && !['text', 'search', 'url', 'tel', 'email', 'password'].includes(input.type))) return false;
  const action = editingHistoryAction(event);
  if (!action) return false;
  // Preserve the browser's native path when this is already its normal key.
  if (action !== 'ignore' && matchesShortcutBinding(event, CONTEXT_SHORTCUTS[action])) return false;
  event.preventDefault();
  event.stopPropagation();
  if (!event.repeat && action !== 'ignore') doc?.execCommand?.(action);
  return true;
}
