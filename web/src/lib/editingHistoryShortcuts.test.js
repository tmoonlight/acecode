import assert from 'node:assert/strict';
import { createEditor, Transforms, Node } from 'slate';
import { withHistory } from 'slate-history';
import { captureShortcut, createShortcutStore, effectiveShortcut, bindingLabel, validateShortcut } from './shortcutBindings.js';
import { editingHistoryAction, handleEditorHistoryKey, handleNativeHistoryKey } from './editingHistoryShortcuts.js';

const key = (code, extra = {}) => ({ code, ctrlKey: true, preventDefault() { this.defaultPrevented = true; }, stopPropagation() {}, ...extra });
const custom = captureShortcut(key('KeyU', { altKey: true }));
const redo = captureShortcut(key('KeyU', { altKey: true, shiftKey: true }));
assert.equal(editingHistoryAction(key('KeyZ'), {}), 'undo');
assert.equal(editingHistoryAction(key('KeyZ', { shiftKey: true }), {}), 'redo');
assert.equal(editingHistoryAction(key('KeyZ', { ctrlKey: false, metaKey: true }), {}), 'undo');
assert.equal(bindingLabel(effectiveShortcut('redo', {}), true), 'Cmd+Shift+Z');
assert.equal(validateShortcut('undo', captureShortcut(key('KeyZ'))), null);
assert.equal(validateShortcut('redo', captureShortcut(key('KeyZ', { shiftKey: true }))), null);
assert.equal(validateShortcut('search', captureShortcut(key('KeyZ')))?.reason, 'reserved');
assert.equal(validateShortcut('undo', captureShortcut(key('KeyY')))?.reason, 'reserved');
assert.equal(validateShortcut('undo', redo, { redo })?.conflict, 'redo');
const overrides = { undo: custom, redo };
assert.equal(editingHistoryAction(key('KeyZ'), overrides), 'ignore');
assert.equal(editingHistoryAction(key('KeyZ', { shiftKey: true }), overrides), 'ignore');
for (const extra of [{ isComposing: true }, { keyCode: 229 }, { getModifierState: () => true },
  { target: { closest: () => ({}) } }]) {
  assert.equal(editingHistoryAction(key('KeyU', { altKey: true, ...extra }), overrides), null);
}
const editor = withHistory(createEditor());
editor.children = [{ type: 'paragraph', children: [{ text: 'before' }] }];
Transforms.select(editor, { path: [0, 0], offset: 6 });
Transforms.insertText(editor, ' after');
assert.equal(handleEditorHistoryKey(key('KeyZ'), editor, overrides), true);
assert.equal(Node.string(editor), 'before after');
handleEditorHistoryKey(key('KeyU', { altKey: true }), editor, overrides);
assert.equal(Node.string(editor), 'before');
handleEditorHistoryKey(key('KeyU', { altKey: true, shiftKey: true }), editor, overrides);
assert.equal(Node.string(editor), 'before after');
handleEditorHistoryKey(key('KeyU', { altKey: true, repeat: true }), editor, overrides);
assert.equal(Node.string(editor), 'before after');
for (const target of [null, { closest: () => ({}) }]) {
  assert.equal(handleNativeHistoryKey(key('KeyZ', { target })), false);
}
const data = new Map();
const storage = { getItem: k => data.get(k), setItem: (k, v) => data.set(k, v) };
const store = createShortcutStore(storage);
assert.deepEqual(store.save('undo', custom), { ok: true });
assert.deepEqual(store.save('redo', redo), { ok: true });
assert.equal(editingHistoryAction(key('KeyU', { altKey: true }), createShortcutStore(storage).getSnapshot()), 'undo');
assert.deepEqual(store.save('undo', null), { ok: true });
assert.equal(editingHistoryAction(key('KeyZ'), store.getSnapshot()), 'undo');
console.log('[pass] history shortcuts: defaults, remapping, Slate history, conflicts, IME, repeat, persistence and reset');
