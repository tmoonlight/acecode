import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';
import { parseSync, traverse } from '@babel/core';
import { createEditor, Editor, Range, Transforms } from 'slate';
import * as composerModel from './richComposerModel.js';
import { formatSessionReferenceToken } from './sessionReference.js';

// Execute the production callback and deletion helpers with a real Slate model.
// Parsing JSX avoids adding a DOM dependency or test-only component exports.
const source = fs.readFileSync(new URL('../components/RichComposer.jsx', import.meta.url), 'utf8');
const ast = parseSync(source, { configFile: false, babelrc: false, parserOpts: { plugins: ['jsx'] } });
const functions = new Map();
traverse(ast, {
  FunctionDeclaration({ node }) {
    functions.set(node.id.name, source.slice(node.start, node.end));
  },
  VariableDeclarator({ node }) {
    if (node.id.name === 'handleKeyDown') {
      const callback = node.init.arguments[0];
      functions.set(node.id.name, source.slice(callback.start, callback.end));
    }
  },
});

function loadFunction(name, context) {
  assert.ok(functions.has(name), `Missing production function ${name}`);
  return vm.runInContext(`(${functions.get(name)})`, context);
}

function fixture({ kind = 'path', key = 'Backspace', selected = false } = {}) {
  const commands = [{ name: 'init', token: '/init', kind: 'builtin' }];
  const tokens = {
    path: '@main.js ',
    command: '/init ',
    session: formatSessionReferenceToken({ id: 'session-1', title: 'Context' }),
    attachment: '',
  };
  const attachments = kind === 'attachment' ? [{ id: 'file-1', name: 'main.js', kind: 'file' }] : [];
  const state = {
    active: false,
    settling: false,
    parentComposing: false,
    slateComposing: false,
    desktop: false,
    parentCalls: 0,
    submissions: 0,
    removed: [],
  };
  const context = vm.createContext({ ...composerModel, Editor, Range, Transforms });
  const editor = loadFunction('withComposerInlineTags', context)(createEditor());
  editor.children = composerModel.composerDocumentFromText(tokens[kind], commands, attachments);
  const offset = key === 'Delete' ? 0 : tokens[kind].length;
  editor.selection = composerModel.composerSelectionFromPlainTextRange(editor.children, offset, offset);
  if (kind === 'attachment' && key === 'Delete') {
    editor.selection = { anchor: { path: [0, 0], offset: 0 }, focus: { path: [0, 0], offset: 0 } };
  }
  if (selected) {
    editor.selection = composerModel.composerSelectionFromPlainTextRange(editor.children, 0, tokens[kind].length);
  }
  Object.assign(context, {
    editor,
    disabled: false,
    compositionStateRef: { current: state },
    isComposingKeyEvent: () => state.parentComposing,
    ReactEditor: { isComposing: () => state.slateComposing },
    isDesktopShell: () => state.desktop,
    onKeyDown: () => { state.parentCalls += 1; },
    onSubmit: () => { state.submissions += 1; },
    onRemoveAttachment: (attachmentKey) => state.removed.push(attachmentKey),
    deleteAdjacentTag: loadFunction('deleteAdjacentTag', context),
    deleteSelectedPlainText: loadFunction('deleteSelectedPlainText', context),
  });
  const handleKeyDown = loadFunction('handleKeyDown', context);
  const event = {
    key,
    keyCode: key === 'Backspace' ? 8 : key === 'Delete' ? 46 : 13,
    which: 0,
    isComposing: false,
    nativeEvent: { isComposing: false },
    defaultPrevented: false,
    propagationStopped: false,
    preventDefault() { this.defaultPrevented = true; },
    stopPropagation() { this.propagationStopped = true; },
  };
  return { state, context, editor, event, handleKeyDown };
}

function assertImeOwnsKey(test) {
  const before = JSON.stringify(test.editor.children);
  const selection = JSON.stringify(test.editor.selection);
  const handled = test.handleKeyDown(test.event);
  assert.equal(JSON.stringify(test.editor.children), before, 'uncommitted IME key must preserve tags and text');
  assert.equal(JSON.stringify(test.editor.selection), selection, 'IME key must preserve the Slate selection');
  assert.deepEqual(test.state.removed, [], 'IME key must not remove attachments');
  assert.equal(test.state.parentCalls, 0, 'IME key must not invoke parent history navigation');
  assert.equal(test.state.submissions, 0, 'IME key must not submit');
  assert.equal(handled, true, 'Slate keyboard fallthrough must be skipped');
  assert.equal(test.event.defaultPrevented, false, 'native IME default must remain available');
  assert.equal(test.event.propagationStopped, false);
}

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

for (const kind of ['path', 'command', 'session', 'attachment']) {
  for (const key of ['Backspace', 'Delete']) {
    run(`IME ${key} preserves the ${kind} tag`, () => {
      const test = fixture({ kind, key });
      test.state.active = true;
      test.event.nativeEvent.isComposing = true;
      assertImeOwnsKey(test);
    });

    run(`normal ${key} still deletes the ${kind} tag`, () => {
      const test = fixture({ kind, key });
      test.handleKeyDown(test.event);
      assert.equal(test.event.defaultPrevented, true);
      if (kind === 'attachment') {
        assert.deepEqual(test.state.removed, ['file-1']);
      } else {
        assert.equal(composerModel.composerTextFromDocument(test.editor.children), '');
        assert.equal(test.editor.children.some((paragraph) => paragraph.children.some(composerModel.isComposerInlineTag)), false);
      }
    });
  }
}

const signals = {
  'local composition start': ({ state }) => { state.active = true; },
  'composition end settling': ({ state }) => { state.settling = true; },
  'parent composition guard': ({ state }) => { state.parentComposing = true; },
  'Slate composition flag': ({ state }) => { state.slateComposing = true; },
  'synthetic event flag': ({ event }) => { event.isComposing = true; },
  'native event flag': ({ event }) => { event.nativeEvent.isComposing = true; },
  'keyCode 229': ({ event }) => { event.keyCode = 229; },
  'which 229': ({ event }) => { event.which = 229; },
  'native keyCode 229': ({ event }) => { event.nativeEvent.keyCode = 229; },
  'native which 229': ({ event }) => { event.nativeEvent.which = 229; },
};

for (const [name, applySignal] of Object.entries(signals)) {
  run(`IME keyboard protection honors ${name} independently`, () => {
    for (const key of ['Backspace', 'Delete', 'Enter', 'ArrowUp', 'ArrowDown']) {
      const test = fixture({ key });
      applySignal(test);
      assertImeOwnsKey(test);
    }
  });
}

for (const key of ['Backspace', 'Delete']) {
  run(`IME ${key} preserves an expanded committed-text selection`, () => {
    const test = fixture({ key, selected: true });
    test.state.active = true;
    assertImeOwnsKey(test);
    test.state.active = false;
    test.handleKeyDown(test.event);
    assert.equal(composerModel.composerTextFromDocument(test.editor.children), '');
    assert.equal(test.event.defaultPrevented, true);
  });
}

run('ordinary tag deletion resumes after composition settles', () => {
  const test = fixture();
  test.state.active = true;
  assertImeOwnsKey(test);
  test.state.active = false;
  test.state.settling = true;
  assertImeOwnsKey(test);
  test.state.settling = false;
  test.handleKeyDown(test.event);
  assert.equal(composerModel.composerTextFromDocument(test.editor.children), '');
  assert.equal(test.event.defaultPrevented, true);
});

run('IME Enter is protected while normal Enter and desktop line breaks retain their behavior', () => {
  for (const desktop of [false, true]) {
    const test = fixture({ key: 'Enter' });
    test.state.desktop = desktop;
    test.event.ctrlKey = desktop;
    test.state.active = true;
    assertImeOwnsKey(test);
    test.state.active = false;
    test.handleKeyDown(test.event);
    assert.equal(test.event.defaultPrevented, true);
    assert.equal(test.state.submissions, desktop ? 0 : 1);
    assert.equal(test.editor.children.length, desktop ? 2 : 1);
  }
  const test = fixture({ key: 'Enter' });
  test.event.shiftKey = true;
  assert.equal(test.handleKeyDown(test.event), undefined);
  assert.equal(test.event.defaultPrevented, false);
  assert.equal(test.state.submissions, 0);
});
