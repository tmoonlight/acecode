import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import vm from 'node:vm';
import { parseSync, traverse } from '@babel/core';
import { composerDraftFingerprint } from './composerDraft.js';
import { homeComposerDraft } from './homeComposerDrafts.js';

function effectSource(path, marker, hook = 'useEffect') {
  const source = readFileSync(new URL(path, import.meta.url), 'utf8');
  const ast = parseSync(source, { configFile: false, babelrc: false, parserOpts: { plugins: ['jsx'] } });
  let found;
  traverse(ast, {
    CallExpression({ node }) {
      if (node.callee?.name !== hook) return;
      const setup = node.arguments[0];
      const text = source.slice(setup.start, setup.end);
      if (text.includes(marker)) found = text;
    },
  });
  assert.ok(found, `production effect must contain ${marker}`);
  return found;
}

const restoration = effectSource('../components/ChatView.jsx', 'api.getSessionDraft(targetSid');
const lifecycle = effectSource('../App.jsx', "window.addEventListener('pagehide', flush)");
const selectWorkspace = effectSource('../components/ChatView.jsx',
  'const selected = workspace?.noWorkspace', 'useStableCallback');

{
  let release;
  const navigation = new Promise(resolve => { release = resolve; });
  const selected = [], localChanges = [];
  const choose = vm.runInNewContext(`(${selectWorkspace})`, {
    noHomeWorkspaceOption: () => ({ hash: '', noWorkspace: true }),
    onHomeWorkspaceChange: workspace => { selected.push(workspace); return navigation; },
    setProjectDropdownOpen() {},
    setHomeWorkspaceHash: value => localChanges.push(value),
    persistHomeWorkspaceHash: value => localChanges.push(value),
  });
  const result = choose({ hash: 'b' });
  assert.deepEqual(selected, [{ hash: 'b' }]);
  assert.deepEqual(localChanges, [], 'a pending navigation must not change the visible workspace or preference');
  release(false);
  assert.equal(await result, false);
  assert.deepEqual(localChanges, [], 'cancelled navigation retains the workspace that owns the current draft');
  console.log('[pass] the workspace dropdown waits for navigation instead of separating the target from its draft');
}

function fixture() {
  let resolve;
  const ready = new Promise((yes) => { resolve = yes; });
  const values = [];
  const context = {
    api: {}, sid: '', draftWorkspaceHash: 'a', draftSessionKey: '',
    homeDraftWorkspaceHash: 'a', homeComposerDrafts: {},
    draftEditVersionRef: { current: 0 },
    preserveComposerInputOnSessionChangeRef: { current: false },
    pendingForkComposerRef: { current: null },
    composerDirtyRef: { current: false }, draftLastSavedRef: { current: null },
    draftSessionKeyRef: { current: '' }, draftSaveQueueRef: { current: new Map() },
    setDraftReadyKey() {}, setComposerSubmitting() {}, homeComposerDraft, composerDraftFingerprint,
    onHomeComposerDraftLoad: () => ({ draft: { text: 'memory draft' }, ready }),
    restoreComposerDraft(draft) { values.push(draft.text); },
  };
  const setup = vm.runInNewContext(`(${restoration})`, context);
  return { context, values, setup, resolve, ready };
}

{
  const test = fixture();
  test.setup();
  assert.deepEqual(test.values, ['memory draft']);
  test.resolve({ text: 'saved workspace draft' });
  await test.ready;
  assert.deepEqual(test.values, ['memory draft', 'saved workspace draft']);
  console.log('[pass] the real home restoration effect hydrates the durable workspace draft');
}

{
  const test = fixture();
  test.setup();
  test.context.draftEditVersionRef.current += 1;
  test.resolve({ text: 'stale draft' });
  await test.ready;
  assert.deepEqual(test.values, ['memory draft']);
  console.log('[pass] the real hydration callback cannot replace typed or staged input');
}

{
  const test = fixture();
  const leave = test.setup();
  leave();
  test.resolve({ text: 'workspace A after navigating to B' });
  await test.ready;
  assert.deepEqual(test.values, ['memory draft']);
  console.log('[pass] leaving a workspace cancels its pending editor hydration');
}

{
  const test = fixture();
  let homeLoads = 0;
  Object.assign(test.context, {
    sid: 'existing', draftSessionKey: 'a:existing',
    draftSessionKeyRef: { current: 'a:existing' },
    api: { getSessionDraft: async () => ({ text: 'existing session draft' }) },
    onHomeComposerDraftLoad() { homeLoads += 1; },
  });
  test.setup();
  for (let i = 0; i < 5; i += 1) await Promise.resolve();
  assert.equal(homeLoads, 0);
  assert.deepEqual(test.values, ['', 'existing session draft']);
  console.log('[pass] existing sessions still restore through their own draft API');
}

{
  const events = () => {
    const listeners = new Map();
    return {
      listeners,
      addEventListener(name, callback) { listeners.set(name, callback); },
      removeEventListener(name, callback) { assert.equal(listeners.get(name), callback); listeners.delete(name); },
    };
  };
  const window = events(), document = { ...events(), visibilityState: 'visible' };
  let flushes = 0;
  const dispose = vm.runInNewContext(`(${lifecycle})`, {
    window, document, homeDraftStore: { flush() { flushes += 1; } },
  })();
  window.listeners.get('beforeunload')();
  window.listeners.get('pagehide')();
  document.listeners.get('visibilitychange')();
  assert.equal(flushes, 2);
  document.visibilityState = 'hidden';
  document.listeners.get('visibilitychange')();
  dispose();
  assert.equal(flushes, 4);
  assert.equal(window.listeners.size + document.listeners.size, 0);
  console.log('[pass] App flushes workspace drafts on page exit, hiding, and unmount');
}
