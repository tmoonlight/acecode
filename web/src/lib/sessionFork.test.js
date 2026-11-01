import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';
import { setTimeout as nextTask } from 'node:timers/promises';
import { parseSync } from '@babel/core';
import { forkRestoredPrompt } from './sessionFork.js';
import { normalizeComposerContent, reconcileComposerContentAttachments } from './composerContent.js';
import { composerDraftFingerprint } from './composerDraft.js';
import { legacyFoldUploadPending } from './pastedText.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('restores the prompt when the fork target was a user message', () => {
  assert.equal(forkRestoredPrompt({ restored_prompt: 'reword me' }), 'reword me');
});

run('restores nothing when the fork target was not a user message', () => {
  assert.equal(forkRestoredPrompt({}), '');
  assert.equal(forkRestoredPrompt({ restored_prompt: '' }), '');
  assert.equal(forkRestoredPrompt({ restored_prompt: 42 }), '');
  assert.equal(forkRestoredPrompt(null), '');
  assert.equal(forkRestoredPrompt(undefined), '');
});

// Execute the actual callbacks in commit order: render updates shared refs,
// then React cleans up the old effects before setting up the new effects.
const source = fs.readFileSync(new URL('../components/ChatView.jsx', import.meta.url), 'utf8');
const ast = parseSync(source, { configFile: false, babelrc: false, parserOpts: { plugins: ['jsx'] } });
const component = ast.program.body.find((node) => node.declaration?.id?.name === 'ChatView').declaration;
const statements = component.body.body;
const callbackSource = (node) => source.slice(node.start, node.end);
const forkNode = statements.flatMap((node) => node.declarations || [])
  .find((node) => node.id.name === 'forkAndSwitch').init.arguments[0];
const effects = statements.filter((node) => node.expression?.callee?.name === 'useEffect')
  .map((node) => node.expression.arguments[0]);
const loadNode = effects.find((node) => callbackSource(node).includes('api.getSessionDraft(targetSid'));
const cleanupNode = effects.find((node) => callbackSource(node).includes('if (!targetSid || !targetKey || !composerDirtyRef.current)'));

async function verifyForkDraftLifecycle(dirty, activateDestination = true) {
  const saved = [];
  const loaded = [];
  let composer = 'source draft';
  const context = {
    sid: 'source', draftSessionKey: 'workspace:source', draftWorkspaceHash: 'workspace',
    ref: { workspaceHash: 'workspace' },
    api: {
      forkSession: async () => ({ session_id: 'fork', restored_prompt: 'historical prompt' }),
      getSessionDraft: async (sid) => { loaded.push(sid); return { text: 'other draft' }; },
    },
    forkActionGuardRef: { current: { acquire: () => true, release() {} } },
    FORK_ACTION_KEY: 'fork', setForkingMessageId() {},
    pendingForkComposerRef: { current: null },
    preserveComposerInputOnSessionChangeRef: { current: false },
    composerDirtyRef: { current: dirty }, composerValueRef: { current: composer },
    composerContentRef: { current: null }, composerAttachmentsRef: { current: [] },
    draftEditVersionRef: { current: 0 }, draftLastSavedRef: { current: {} },
    draftSaveQueueRef: { current: new Map() },
    draftSessionKeyRef: { current: 'workspace:source' },
    setDraftReadyKey() {}, setDraftLoadFailedKey() {}, setComposerSubmitting() {},
    setComposerValue(text) { composer = text; },
    restoreComposerDraft(draft) { composer = draft.text; },
    normalizeComposerContent, reconcileComposerContentAttachments, composerDraftFingerprint,
    // 收尾保存在旧长文本折叠的文件块上传完成前跳过;这里没有折叠(guard 为空),照常保存。
    legacyFoldUploadPending, legacyFoldGuardRef: { current: null },
    persistDraftValue: (sid, workspace, key, text) => saved.push({ sid, text }),
    forkRestoredPrompt, newSessionRefFrom: (ref, sid) => ({ ...ref, sessionId: sid }),
    onSessionPromoted() {}, notifySessionListChanged() {}, toast() {},
  };
  const evaluate = (node) => vm.runInNewContext(`(${callbackSource(node)})`, context);
  const cleanup = evaluate(cleanupNode)();
  await evaluate(forkNode)('message');
  assert.equal(composer, 'source draft', 'fork response must not mutate the outgoing composer');
  context.sid = activateDestination ? 'fork' : 'other';
  context.draftSessionKey = `workspace:${context.sid}`;
  context.draftSessionKeyRef.current = context.draftSessionKey;
  context.composerValueRef.current = composer;
  cleanup();
  assert.deepEqual(saved, dirty ? [{ sid: 'source', text: 'source draft' }] : []);
  evaluate(loadNode)();
  await nextTask();
  assert.equal(composer, activateDestination ? 'historical prompt' : 'other draft');
  assert.deepEqual(loaded, activateDestination ? [] : ['other']);
  assert.equal(context.pendingForkComposerRef.current, null);
  if (activateDestination) {
    context.composerValueRef.current = composer;
    evaluate(cleanupNode)()();
    assert.deepEqual(saved.at(-1), { sid: 'fork', text: 'historical prompt' });
  }
}

await verifyForkDraftLifecycle(true);
await verifyForkDraftLifecycle(false);
await verifyForkDraftLifecycle(true, false);
console.log('[pass] fork draft lifecycle preserves source drafts and scopes refill to the destination');
