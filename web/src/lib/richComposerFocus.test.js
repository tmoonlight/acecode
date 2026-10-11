import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';
import { parseSync, traverse } from '@babel/core';

const source = fs.readFileSync(new URL('../components/RichComposer.jsx', import.meta.url), 'utf8');
const ast = parseSync(source, { configFile: false, babelrc: false, parserOpts: { plugins: ['jsx'] } });
let method;
traverse(ast, {
  ObjectMethod({ node }) {
    if (node.key.name === 'focus') method = source.slice(node.start, node.end);
  },
});
assert.ok(method, 'The production composer must expose imperative focus');

function fixture({ pending = false, renderPending = false, composing = false, settling = false, selection = true, domFailure = false } = {}) {
  const state = { focused: 0, repairs: 0, frames: [] };
  const editor = { children: [], selection: selection ? {} : null, operations: pending ? [{}] : [] };
  const editableRef = { current: { isConnected: true } };
  const renderPendingRef = { current: renderPending };
  const compositionStateRef = { current: { active: composing, settling } };
  const scope = {
    editor, editableRef, renderPendingRef, compositionStateRef, latestTextRef: { current: '' },
    composerSelectionFromPlainTextRange: () => ({}),
    Transforms: { select: (target, value) => {
      target.selection = value;
      target.operations.push({ type: 'set_selection' });
    } },
    ReactEditor: { isComposing: () => composing, focus: () => {
      state.focused += 1;
      if (domFailure) { domFailure = false; throw new Error('DOM mapping pending'); }
    } },
    ensureLegalEditorDocument: () => { state.repairs += 1; },
    window: { requestAnimationFrame: (callback) => { state.frames.push(callback); } },
  };
  const focus = vm.runInNewContext(`({ ${method} }).focus`, scope);
  return { state, editor, editableRef, compositionStateRef, focus,
    flush() {
      editor.operations = [];
      renderPendingRef.current = false;
      const frames = state.frames.splice(0);
      frames.forEach((callback) => callback());
    },
  };
}

function run(name, check) {
  check();
  console.log(`[pass] ${name}`);
}

run('focus waits for pending Slate operations without starting an unmanaged native retry', () => {
  const test = fixture({ pending: true });
  test.focus();
  assert.equal(test.state.focused, 0);
  assert.equal(test.state.frames.length, 1);
  test.flush();
  assert.equal(test.state.focused, 1);
  assert.equal(test.state.frames.length, 0);
});

run('an empty selection is committed before attempting DOM focus', () => {
  const test = fixture({ selection: false });
  test.focus();
  assert.ok(test.editor.selection);
  assert.equal(test.state.focused, 0);
  test.flush();
  assert.equal(test.state.focused, 1);
});

run('unmounting the input cancels the effect of an already scheduled focus retry', () => {
  const test = fixture({ pending: true });
  test.focus();
  test.editableRef.current = null;
  test.flush();
  assert.equal(test.state.focused, 0);
});

run('a disconnected editor is never focused', () => {
  const test = fixture();
  test.editableRef.current.isConnected = false;
  test.focus();
  assert.equal(test.state.focused, 0);
  assert.equal(test.state.frames.length, 0);
});

run('a cancelled window activation cannot focus after a pending Slate commit', () => {
  const test = fixture({ pending: true });
  let active = true;
  test.focus({ canFocus: () => active });
  active = false;
  test.flush();
  assert.equal(test.state.focused, 0);
});

run('a missing DOM mapping gets one guarded retry after a frame', () => {
  const test = fixture({ domFailure: true });
  test.focus();
  assert.equal(test.state.repairs, 1);
  test.flush();
  assert.equal(test.state.focused, 2);
});

run('an already synchronized editor focuses immediately', () => {
  const test = fixture();
  test.focus();
  assert.equal(test.state.focused, 1);
  assert.equal(test.state.frames.length, 0);
});

for (const options of [{composing:true}, {settling:true}]) {
  run('focus cannot change selection during IME composition or its commit microtask: '+JSON.stringify(options), () => {
    const test = fixture({...options, selection:false});
    test.focus();
    assert.equal(test.editor.selection, null);
    assert.equal(test.state.focused, 0);
    assert.equal(test.state.frames.length, 0);
  });
}

run('focus waits for React to render even after Slate operations have flushed', () => {
  const test = fixture({renderPending:true});
  test.focus();
  assert.equal(test.state.focused, 0);
  test.flush();
  assert.equal(test.state.focused, 1);
});

run('composition starting during a pending focus retry retains the IME range', () => {
  const test = fixture({pending:true});
  test.focus();
  test.compositionStateRef.current.active = true;
  test.flush();
  assert.equal(test.state.focused, 0);
});
