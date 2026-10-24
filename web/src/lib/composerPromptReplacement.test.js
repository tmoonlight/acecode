import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';
import { parseSync, traverse } from '@babel/core';
import { createEditor, Editor, Transforms } from 'slate';
import { HistoryEditor, withHistory } from 'slate-history';
import * as model from './richComposerModel.js';

// Run the production replacement against real Slate history, including its
// selection snapshots; a parent-state-only test would miss cleared undo stacks.
const source = fs.readFileSync(new URL('../components/RichComposer.jsx', import.meta.url), 'utf8');
const functions = new Map();
traverse(parseSync(source, { configFile: false, babelrc: false, parserOpts: { plugins: ['jsx'] } }), {
  FunctionDeclaration({ node }) { functions.set(node.id.name, source.slice(node.start, node.end)); },
});
const context = vm.createContext({ ...model, Editor, Transforms, HistoryEditor });
for (const name of ['withComposerInlineTags', 'legalComposerDocument', 'safeDeselectEditor',
  'ensureLegalEditorDocument', 'replaceEditorDocument']) {
  context[name] = vm.runInContext(`(${functions.get(name)})`, context);
}

const flush = async () => { for (let i = 0; i < 4; i += 1) await Promise.resolve(); };
const clone = (value) => JSON.parse(JSON.stringify(value));
function fixture(text = 'Original draft', attachments = []) {
  const editor = withHistory(context.withComposerInlineTags(createEditor()));
  editor.children = model.composerDocumentFromText(text, [], attachments);
  editor.selection = model.composerSelectionFromPlainTextRange(editor.children, text.length, text.length);
  const replace = async (text, options = { undoable: true }) => {
    assert.equal(context.replaceEditorDocument(editor, model.composerDocumentFromText(text), options), true);
    await flush();
  };
  return { editor, replace, text: () => model.composerTextFromDocument(editor.children) };
}
async function test(name, run) { await run(); console.log(`[pass] prompt replacement: ${name}`); }

await test('replaces the full text and Ctrl+Z restores the draft without losing older edits', async () => {
  const f = fixture();
  Transforms.insertText(f.editor, ' edited'); await flush();
  const selection = clone(f.editor.selection);
  const before = f.text();
  const earlierBatches = f.editor.history.undos.length;
  await f.replace('Selected prompt');
  assert.equal(f.text(), 'Selected prompt');
  assert.equal(f.editor.history.undos.length, earlierBatches + 1);
  HistoryEditor.undo(f.editor); await flush();
  assert.equal(f.text(), before);
  assert.deepEqual(clone(f.editor.selection), selection);
  HistoryEditor.undo(f.editor); await flush();
  assert.equal(f.text(), 'Original draft');
});

await test('each card replacement is a separate undo step and can be redone', async () => {
  const f = fixture();
  await f.replace('First prompt'); await f.replace('Second prompt');
  HistoryEditor.undo(f.editor); await flush(); assert.equal(f.text(), 'First prompt');
  HistoryEditor.undo(f.editor); await flush(); assert.equal(f.text(), 'Original draft');
  HistoryEditor.redo(f.editor); await flush(); assert.equal(f.text(), 'First prompt');
});

await test('undo restores inline path and attachment nodes exactly', async () => {
  const f = fixture('Inspect @main.cpp please', [{ id: 'file1', kind: 'file', name: 'notes.md' }]);
  const before = clone(f.editor.children);
  assert.ok(model.composerContentFromDocument(f.editor.children).parts.some((part) => part.type === 'attachment'));
  await f.replace('Write a new plan');
  HistoryEditor.undo(f.editor); await flush();
  assert.deepEqual(clone(f.editor.children), before);
});

await test('workspace hydration stays outside undo while the subsequent prompt is undoable', async () => {
  const f = fixture('Workspace A');
  await f.replace('Workspace B draft', { clearHistory: true });
  assert.equal(f.editor.history.undos.length, 0);
  await f.replace('Suggested task for B');
  HistoryEditor.undo(f.editor); await flush();
  assert.equal(f.text(), 'Workspace B draft');
  assert.equal(f.editor.history.undos.length, 0);
});

await test('replacing an empty draft remains a single reversible operation', async () => {
  const f = fixture('');
  await f.replace('New prompt');
  assert.equal(f.editor.history.undos.length, 1);
  HistoryEditor.undo(f.editor); await flush();
  assert.equal(f.text(), '');
  assert.ok(f.editor.children.length > 0);
});
