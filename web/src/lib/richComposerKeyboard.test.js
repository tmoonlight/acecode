import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';
import { parseSync, traverse } from '@babel/core';
import { createEditor, Editor, Range, Transforms } from 'slate';
import { HistoryEditor, withHistory } from 'slate-history';
import * as contentModel from './composerContent.js';
import { composerDraftEditFingerprint } from './composerDraft.js';
import { withComposerImageAttachments } from './composerImagePresentation.js';
import { isUserComposerEdit } from './inputHistoryNavigation.js';
import * as composerModel from './richComposerModel.js';
import * as composerSelection from './composerSelection.js';
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
    adoptCalls: 0,
    // 模拟用户看到的 DOM 光标;为 null 表示 DOM 与 Slate 一致、无需采纳。
    domCaret: null,
  };
  const context = vm.createContext({ ...composerModel, ...contentModel, ...composerSelection, Editor, Range, Transforms, HistoryEditor, COMPOSER_CLIPBOARD_TYPE: 'application/x-acecode-composer-content' });
  const editor = withHistory(loadFunction('withComposerInlineTags', context)(createEditor()));
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
    submitOnEnter: true,
    compositionStateRef: { current: state },
    isComposingKeyEvent: () => state.parentComposing,
    ReactEditor: { isComposing: () => state.slateComposing, toDOMNode: () => ({ dir: state.rtl ? 'rtl' : '' }) },
    isDesktopShell: () => state.desktop,
    onKeyDown: () => { state.parentCalls += 1; },
    onSubmit: () => { state.submissions += 1; },
    onRemoveAttachment: (attachmentKey) => state.removed.push(attachmentKey),
    adoptDomSelection: () => {
      state.adoptCalls += 1;
      if (!state.domCaret) return false;
      Transforms.select(editor, state.domCaret);
      return true;
    },
    deleteAdjacentTag: loadFunction('deleteAdjacentTag', context),
    removeAttachmentReference: loadFunction('removeAttachmentReference', context),
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
  // 合成期间 DOM 里有未提交的拼音,此时的 DOM 选区不能写回 Slate(由 compositionstart 负责)。
  assert.equal(test.state.adoptCalls, 0, 'IME key must not adopt the DOM selection');
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
  run(`collapsed ${kind} void selection copies its canonical content and cuts only that tag`, () => {
    const test = fixture({ kind });
    const before = structuredClone(test.editor.children);
    const [tag, path] = [...Editor.nodes(test.editor, { at: [], match: composerModel.isComposerInlineTag })][0];
    Transforms.select(test.editor, Editor.start(test.editor, path));
    const formats = new Map();
    const event = { clipboardData: { setData: (type, value) => formats.set(type, value) }, preventDefault() {} };
    assert.equal(loadFunction('writeSelectedPlainText', test.context)(event, test.editor), true);
    assert.equal(formats.get('text/plain'), tag.token || `[${tag.name}]`);
    assert.ok(formats.has('application/x-acecode-composer-content'));
    assert.equal(loadFunction('deleteSelectedPlainText', test.context)(test.editor), true);
    assert.equal([...Editor.nodes(test.editor, { at: [], match: composerModel.isComposerInlineTag })].length, 0);
    HistoryEditor.undo(test.editor);
    assert.deepEqual(test.editor.children, before);
  });

  run(`pasting text replaces a collapsed ${kind} void selection and undo restores it`, () => {
    const test = fixture({ kind });
    const before = structuredClone(test.editor.children);
    const [, path] = [...Editor.nodes(test.editor, { at: [], match: composerModel.isComposerInlineTag })][0];
    Transforms.select(test.editor, Editor.start(test.editor, path));
    loadFunction('insertPlainText', test.context)(test.editor, 'replacement');
    assert.equal([...Editor.nodes(test.editor, { at: [], match: composerModel.isComposerInlineTag })].length, 0);
    assert.ok(composerModel.composerTextFromDocument(test.editor.children).startsWith('replacement'));
    HistoryEditor.undo(test.editor);
    assert.deepEqual(test.editor.children, before);
  });

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
        assert.equal(composerModel.composerContentFromDocument(test.editor.children).parts.some((part) => part.type === 'attachment'), false);
        HistoryEditor.undo(test.editor);
        assert.equal(composerModel.composerContentFromDocument(test.editor.children).parts.some((part) => part.type === 'attachment'), true);
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
    for (const key of ['Backspace', 'Delete', 'Enter', 'ArrowUp', 'ArrowDown', 'ArrowLeft', 'ArrowRight']) {
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

function orderedFixture() {
  const test = fixture();
  const content = { version: 1, parts: [
    { type: 'text', text: 'Review ' },
    { type: 'skill', name: 'review', token: '$review' },
    { type: 'text', text: ' with ' },
    { type: 'attachment', key: 'local-file', id: 'att-file', name: 'notes.md', kind: 'file' },
    { type: 'text', text: ' then compare ' },
    { type: 'path', path: 'src/main.cpp', token: '@src/main.cpp', directory: false },
  ] };
  test.editor.children = composerModel.composerDocumentFromContent(content);
  test.editor.selection = Editor.range(test.editor, []);
  test.editor.history.undos = [];
  test.editor.history.redos = [];
  test.content = content;
  test.resources = [{ local_id: 'local-file', id: 'att-file', name: 'notes.md', kind: 'file' }];
  test.context.currentPlainSelection = loadFunction('currentPlainSelection', test.context);
  return test;
}

run('mixed text/skill/file selection deletes atomically and undo restores exact reference order', () => {
  const test = orderedFixture();
  loadFunction('deleteSelectedPlainText', test.context)(test.editor);
  assert.deepEqual(composerModel.composerContentFromDocument(test.editor.children).parts, []);
  HistoryEditor.undo(test.editor);
  assert.deepEqual(composerModel.composerContentFromDocument(test.editor.children), test.content);
});

run('copy/paste preserves mixed references while external text retains readable filenames', () => {
  const test = orderedFixture();
  const formats = new Map();
  const event = { clipboardData: { setData: (type, value) => formats.set(type, value) }, preventDefault() {} };
  assert.equal(loadFunction('writeSelectedPlainText', test.context)(event, test.editor), true);
  assert.equal(formats.get('text/plain'), 'Review $review with [notes.md] then compare @src/main.cpp');
  const copied = JSON.parse(formats.get('application/x-acecode-composer-content'));
  assert.deepEqual(copied, test.content);
  test.editor.children = composerModel.composerDocumentFromText('');
  test.editor.selection = Editor.range(test.editor, []);
  assert.equal(loadFunction('insertComposerContent', test.context)(test.editor, copied, [], test.resources), true);
  assert.deepEqual(composerModel.composerContentFromDocument(test.editor.children), test.content);
});

run('clipboard references from a different resource registry use the safe plain-text fallback', () => {
  const test = orderedFixture();
  const before = JSON.stringify(test.editor.children);
  assert.equal(loadFunction('insertComposerContent', test.context)(test.editor, test.content, [], []), false);
  assert.equal(JSON.stringify(test.editor.children), before);
});

run('programmatic file/path insertion preserves existing skill and attachment positions', () => {
  const test = orderedFixture();
  const previous = composerModel.composerTextFromDocument(test.editor.children);
  const next = 'Please ' + previous;
  const changed = loadFunction('replaceComposerTextPreservingReferences', test.context)(test.editor, next, []);
  assert.equal(contentModel.composerContentText(changed), next);
  assert.deepEqual(changed.parts.filter((part) => part.type !== 'text'), test.content.parts.filter((part) => part.type !== 'text'));
  HistoryEditor.undo(test.editor);
  assert.deepEqual(composerModel.composerContentFromDocument(test.editor.children), test.content);
});

for (const completion of [
  { query: '@', token: formatSessionReferenceToken({ id: 'referenced-task', title: 'Referenced task' }, { trailingSpace: false }), type: composerModel.COMPOSER_SESSION_TAG },
  { query: '@src/m', token: '@src/main.cpp', type: composerModel.COMPOSER_PATH_TAG },
  { query: '@docs/', token: '@"docs/my notes.md"', type: composerModel.COMPOSER_PATH_TAG },
]) {
  run(`mention completion replaces the entire ${completion.query} query with an atomic reference`, () => {
    const test = orderedFixture();
    const original = { ...test.content, parts: [
      ...test.content.parts.slice(0, 4),
      { type: 'text', text: ` before ${completion.query} after` },
    ] };
    test.editor.children = composerModel.composerDocumentFromContent(original);
    const previous = composerModel.composerTextFromDocument(test.editor.children);
    const begin = previous.indexOf(completion.query);
    const end = begin + completion.query.length;
    Transforms.select(test.editor, composerModel.composerSelectionFromPlainTextRange(test.editor.children, end, end));
    const next = previous.slice(0, begin) + completion.token + ' ' + previous.slice(end);
    const changed = loadFunction('replaceComposerTextPreservingReferences', test.context)(test.editor, next, [], { begin, end });
    assert.equal(contentModel.composerContentText(changed), next);
    assert.ok(test.editor.children[0].children.some((node) => node.type === completion.type && node.token === completion.token));
    assert.deepEqual(changed.parts.filter((part) => part.type === 'attachment'), original.parts.filter((part) => part.type === 'attachment'));
    assert.deepEqual(changed.parts.filter((part) => part.type === 'skill'), original.parts.filter((part) => part.type === 'skill'));
    HistoryEditor.undo(test.editor);
    assert.deepEqual(composerModel.composerContentFromDocument(test.editor.children), original);
  });
}

run('completing a query between two adjacent file occurrences preserves both and undo', () => {
  const test = orderedFixture();
  const file = test.content.parts.find((part) => part.type === 'attachment');
  const original = { version: 1, parts: [{ type: 'text', text: 'before ' }, file, { type: 'text', text: '@' }, file, { type: 'text', text: ' after' }] };
  test.editor.children = composerModel.composerDocumentFromContent(original);
  const next = 'before @src/main.cpp  after';
  const changed = loadFunction('replaceComposerTextPreservingReferences', test.context)(test.editor, next, [], { begin: 7, end: 8 });
  assert.equal(changed.parts.filter((part) => part.type === 'attachment').length, 2);
  assert.equal(contentModel.composerContentText(changed), next);
  HistoryEditor.undo(test.editor);
  assert.deepEqual(composerModel.composerContentFromDocument(test.editor.children), original);
});

run('inserting a new path before an existing path retains both complete references', () => {
  const test = orderedFixture();
  test.editor.children = composerModel.composerDocumentFromText('@old.js ');
  Transforms.select(test.editor, Editor.start(test.editor, []));
  const changed = loadFunction('replaceComposerTextPreservingReferences', test.context)(test.editor, '@new.js @old.js ', [], { begin: 0, end: 0 });
  assert.equal(contentModel.composerContentText(changed), '@new.js @old.js ');
  assert.deepEqual(changed.parts.filter((part) => part.type === 'path').map((part) => part.path), ['new.js', 'old.js']);
  HistoryEditor.undo(test.editor);
  assert.equal(composerModel.composerTextFromDocument(test.editor.children), '@old.js ');
});

for (const directory of ['@src/', '@"my docs/"']) run('entering a directory keeps the next path query editable instead of committing a tag', () => {
  const test = orderedFixture();
  test.editor.children = composerModel.composerDocumentFromContent({ version: 1, parts: [{ type: 'text', text: 'before @sr after' }] });
  const changed = loadFunction('replaceComposerTextPreservingReferences', test.context)(test.editor, `before ${directory} after`, [], { begin: 7, end: 10, plainText: true });
  assert.equal(contentModel.composerContentText(changed), `before ${directory} after`);
  assert.equal(changed.parts.some((part) => part.type === 'path'), false);
});

// 场景:Slate 的 selectionchange 同步被卡住(例如拖拽结束事件丢失),editor.selection 停在
// 行首(标签之前),用户看到的光标却在 "@main.js " 末尾。按普通 Backspace。
// 期望:handleKeyDown 先采纳 DOM 光标,父组件 onKeyDown 读到的是采纳后的选区,退格按用户
// 看到的位置删除前面的标签。
// 修复前:退格在过期的行首选区上执行,标签不删(同一根因下输入法文字也会落到过期位置)。
run('ordinary keys adopt the visible DOM caret before parent and deletion handlers read the selection', () => {
  const test = fixture({ kind: 'path', key: 'Backspace' });
  const visibleCaret = structuredClone(test.editor.selection);
  Transforms.select(test.editor, Editor.start(test.editor, []));
  test.state.domCaret = visibleCaret;
  let parentSelection = null;
  test.context.onKeyDown = () => { parentSelection = structuredClone(test.editor.selection); };
  test.handleKeyDown(test.event);
  assert.equal(test.state.adoptCalls, 1);
  assert.deepEqual(parentSelection, visibleCaret);
  assert.equal(test.event.defaultPrevented, true);
  assert.equal(composerModel.composerTextFromDocument(test.editor.children), '');
});

run('queue editor lets Enter reach Slate when submitOnEnter is disabled', () => {
  const test = fixture({ key: 'Enter' });
  test.context.submitOnEnter = false;
  test.handleKeyDown(test.event);
  assert.equal(test.state.submissions, 0);
  assert.equal(test.event.defaultPrevented, false);
});

for (const kind of ['path', 'command', 'session', 'attachment']) {
  for (const reverse of [false, true]) {
    run(`plain ${reverse ? 'left' : 'right'} arrow skips one ${kind} without selecting it`, () => {
      const test = fixture({ kind, key: reverse ? 'ArrowLeft' : 'ArrowRight' });
      const [, path] = [...Editor.nodes(test.editor, { at: [], match: composerModel.isComposerInlineTag })][0];
      const range = composerSelection.composerTagSelection(test.editor, path);
      const before = structuredClone(test.editor.children);
      Transforms.select(test.editor, reverse ? range.focus : range.anchor);
      test.handleKeyDown(test.event);
      const expected = reverse ? range.anchor : range.focus;
      assert.deepEqual(test.editor.selection, { anchor: expected, focus: expected });
      assert.equal(test.event.defaultPrevented, true);
      assert.equal(composerSelection.composerSelectedTag(test.editor), null);
      assert.deepEqual(test.editor.children, before);
      assert.equal(test.editor.history.undos.length, 0);
    });
  }
}

run('plain arrow tag crossing follows the Slate paragraph RTL direction', () => {
  for (const key of ['ArrowLeft', 'ArrowRight']) {
    const test = fixture({ key });
    test.state.rtl = true;
    const range = composerSelection.composerTagSelection(test.editor, [0, 1]);
    Transforms.select(test.editor, key === 'ArrowLeft' ? range.anchor : range.focus);
    test.handleKeyDown(test.event);
    const expected = key === 'ArrowLeft' ? range.focus : range.anchor;
    assert.deepEqual(test.editor.selection, { anchor: expected, focus: expected });
    assert.equal(test.event.defaultPrevented, true);
  }
});

run('modified arrows and expanded selections stay with existing keyboard handlers', () => {
  for (const modifier of ['shiftKey', 'ctrlKey', 'altKey', 'metaKey', 'expanded']) {
    const test = fixture({ key: 'ArrowRight' });
    const range = composerSelection.composerTagSelection(test.editor, [0, 1]);
    if (modifier === 'expanded') Transforms.select(test.editor, range);
    else { Transforms.select(test.editor, range.anchor); test.event[modifier] = true; }
    const before = structuredClone(test.editor.selection);
    test.handleKeyDown(test.event);
    assert.deepEqual(test.editor.selection, before);
    assert.equal(test.event.defaultPrevented, false);
  }
});

run('ordinary text and a parent-consumed arrow are not intercepted', () => {
  for (const parentConsumed of [false, true]) {
    const test = fixture({ key: 'ArrowLeft' });
    test.context.onKeyDown = event => { if (parentConsumed) event.preventDefault(); };
    Transforms.select(test.editor, Editor.end(test.editor, []));
    const before = structuredClone(test.editor.selection);
    test.handleKeyDown(test.event);
    assert.deepEqual(test.editor.selection, before);
    assert.equal(test.event.defaultPrevented, parentConsumed);
  }
});

run('repeated attachment references paste twice and Backspace removes only the selected occurrence', () => {
  const test = orderedFixture();
  const one = { version: 1, parts: [test.content.parts.find((part) => part.type === 'attachment')] };
  test.editor.children = composerModel.composerDocumentFromText('');
  test.editor.selection = Editor.range(test.editor, []);
  const insert = loadFunction('insertComposerContent', test.context);
  assert.equal(insert(test.editor, one, [], test.resources), true);
  assert.equal(insert(test.editor, one, [], test.resources), true);
  assert.equal(composerModel.composerContentFromDocument(test.editor.children).parts.length, 2);
  const before = composerModel.composerContentFromDocument(test.editor.children);
  Transforms.select(test.editor, Editor.end(test.editor, []));
  const path = composerModel.composerAdjacentAttachmentPath(test.editor.children, test.editor.selection, 'backward');
  assert.ok(path);
  loadFunction('removeAttachmentReference', test.context)(test.editor, null, path);
  assert.equal(composerModel.composerContentFromDocument(test.editor.children).parts.length, 1);
  HistoryEditor.undo(test.editor);
  assert.deepEqual(composerModel.composerContentFromDocument(test.editor.children), before);
});

const inputBarSource = fs.readFileSync(new URL('../components/InputBar.jsx', import.meta.url), 'utf8');
const inputBarAst = parseSync(inputBarSource, { configFile: false, babelrc: false, parserOpts: { plugins: ['jsx'] } });
let inputChangeSource = '';
traverse(inputBarAst, {
  VariableDeclarator({ node }) {
    if (node.id.name === 'handleComposerChange') inputChangeSource = inputBarSource.slice(node.init.start, node.init.end);
  },
});

run('resuming edits cancels an earlier picker caret restore before upload completion', () => {
  const test = orderedFixture();
  const oldText = contentModel.composerContentText(test.content);
  const state = { cleared: false, edited: false, updated: null };
  const context = vm.createContext({
    ...contentModel, composerDraftEditFingerprint, isUserComposerEdit,
    valueRef: { current: oldText }, contentRef: { current: test.content },
    mergeEditorContent: (content) => withComposerImageAttachments(content, test.content),
    caretRestoreUntilRef: { current: 999999 }, caretRestoreSelectionRef: { current: { start: 1, end: 1 } },
    clearCaretRestoreSchedule: () => { state.cleared = true; },
    updateValue: (text, content) => { state.updated = { text, content }; },
    setEditedSinceHistory: (value) => { state.edited = value; },
  });
  const handler = vm.runInContext(`(${inputChangeSource})`, context);
  const edited = { ...test.content, parts: [{ type: 'text', text: 'Typed ' }, ...test.content.parts] };
  handler(contentModel.composerContentText(edited), edited);
  assert.equal(context.caretRestoreUntilRef.current, 0);
  assert.equal(context.caretRestoreSelectionRef.current, null);
  assert.equal(state.cleared, true);
  assert.equal(state.edited, true);
});

run('legacy path tokenization and upload metadata echoes preserve input history navigation', () => {
  const source = { version: 1, parts: [{ type: 'text', text: '@src/main.cpp' }] };
  const tokenized = composerModel.composerContentFromDocument(composerModel.composerDocumentFromText('@src/main.cpp'));
  const state = { edits: 0, updates: 0 };
  const context = vm.createContext({
    ...contentModel, composerDraftEditFingerprint, isUserComposerEdit,
    valueRef: { current: '@src/main.cpp' }, contentRef: { current: source },
    mergeEditorContent: (content) => withComposerImageAttachments(content, source),
    caretRestoreUntilRef: { current: 100 }, caretRestoreSelectionRef: { current: { start: 1, end: 1 } },
    clearCaretRestoreSchedule: () => { throw new Error('semantic echo must not clear caret state'); },
    updateValue: () => { state.updates += 1; },
    setEditedSinceHistory: () => { state.edits += 1; },
  });
  vm.runInContext(`(${inputChangeSource})`, context)('@src/main.cpp', tokenized);
  assert.equal(state.updates, 1);
  assert.equal(state.edits, 0);
});

run('deleting a skill beside a zero-text file preserves that adjacent file occurrence', () => {
  const test = orderedFixture();
  const skill = test.content.parts.find((part) => part.type === 'skill');
  const attachment = test.content.parts.find((part) => part.type === 'attachment');
  const content = { version: 1, parts: [skill, attachment] };
  test.editor.children = composerModel.composerDocumentFromContent(content);
  const children = test.editor.children[0].children;
  const index = children.findIndex(composerModel.isComposerSkillTag);
  test.editor.selection = {
    anchor: { path: [0, index + 1], offset: 0 },
    focus: { path: [0, index + 1], offset: 0 },
  };
  assert.equal(loadFunction('deleteAdjacentTag', test.context)(test.editor, 'backward'), true);
  assert.deepEqual(composerModel.composerContentFromDocument(test.editor.children).parts, [attachment]);
  HistoryEditor.undo(test.editor);
  assert.deepEqual(composerModel.composerContentFromDocument(test.editor.children), content);
});
