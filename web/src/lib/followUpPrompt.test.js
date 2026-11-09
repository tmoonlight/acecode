import assert from 'node:assert/strict';
import { createFollowUpPromptStore, followUpCompletionKey, followUpScope, validFollowUpPrompt, hasFollowUpDraft } from './followUpPrompt.js';

const complete = (user = 'u1', at = 200) => ({
  loadState: 'loaded', busy: false, status: 'idle', items: [{ kind: 'msg', role: 'user', messageId: user }],
  turnTimings: new Map([[user, { status: 'completed', userMessageUuid: user, completedAtMs: at }]]),
});
const tick = () => new Promise((resolve) => setImmediate(resolve));
const key = followUpCompletionKey(complete());
assert.equal(followUpScope(null), followUpScope({}));
assert.equal(key, 'u1:200');
for (const patch of [{ loadState: 'loading' }, { busy: true }, { abortPending: true }, { status: 'error' }, { items: [] }]) {
  assert.equal(followUpCompletionKey({ ...complete(), ...patch }), '');
}
for (const status of ['aborted', 'error', 'running']) {
  const state = complete(); state.turnTimings.get('u1').status = status;
  assert.equal(followUpCompletionKey(state), '');
}
const newer = complete(); newer.items.push({ kind: 'msg', role: 'user', messageId: 'u2' });
assert.equal(followUpCompletionKey(newer), '');
assert.notEqual(followUpScope({ sessionId: 's', workspaceHash: 'a' }), followUpScope({ sessionId: 's', workspaceHash: 'b' }));
assert.notEqual(followUpScope({ sessionId: 's', port: 1 }), followUpScope({ sessionId: 's', port: 2 }));

{
  const store = createFollowUpPromptStore();
  let calls = 0; let finish;
  const request = () => { calls++; return new Promise((resolve) => { finish = resolve; }); };
  const unsubscribe = store.subscribe('s1', key, () => {});
  store.ensure('s1', key, request); store.ensure('s1', key, request);
  unsubscribe(); // Leaving the session does not discard the in-flight request.
  store.ensure('s1', key, request);
  await tick(); assert.equal(calls, 1);
  finish({ completion_key: key, prompt: 'Run the tests.' }); await tick();
  assert.equal(store.get('s1', key).prompt, 'Run the tests.');
  store.ensure('s1', key, request, { hasDraft: true });
  store.ensure('s1', key, request); await tick(); assert.equal(calls, 1);
  assert.equal(store.get('s1', key).prompt, 'Run the tests.', 'clearing a draft reuses the ready prompt');
  assert.equal(store.get('s2', key).prompt, '');
  store.consume('s1', key);
  store.ensure('s1', key, request); await tick(); assert.equal(calls, 1);
  assert.equal(store.get('s1', key).prompt, '');
  store.ensure('s1', 'u2:300', async () => { calls++; return { completion_key: 'u2:300', prompt: 'Review the diff.' }; });
  await tick(); assert.equal(calls, 2); assert.equal(store.get('s1', 'u2:300').prompt, 'Review the diff.');
}
{
  const store = createFollowUpPromptStore(); let finish;
  store.ensure('s', key, () => new Promise((resolve) => { finish = resolve; }));
  await tick(); store.skipGeneration('s', key); // User edits while the request is pending.
  finish({ completion_key: key, prompt: 'Late result.' }); await tick();
  assert.equal(store.get('s', key).prompt, ''); assert.equal(store.get('s', key).consumed, true);
}
{
  const store = createFollowUpPromptStore(); let calls = 0;
  store.skipGeneration('s', key); // Existing draft effect can precede the parent's request effect.
  store.ensure('s', key, async () => { calls++; return { completion_key: key, prompt: 'Hidden result.' }; });
  await tick(); assert.equal(calls, 0); assert.equal(store.get('s', key).prompt, '');
}
{
  const store = createFollowUpPromptStore(); let calls = 0;
  const request = async () => { calls++; return { completion_key: key, prompt: 'Run tests.' }; };
  store.ensure('s', key, request, { draftReady: false });
  await tick(); assert.equal(calls, 0); assert.equal(store.get('s', key).status, 'idle');
  store.ensure('s', key, request, { hasDraft: true });
  await tick(); assert.equal(calls, 0); assert.equal(store.get('s', key).consumed, true);
  store.ensure('s', key, request); // Clearing and revisiting do not backfill a skipped trigger.
  await tick(); assert.equal(calls, 0);
  store.ensure('s', 'u2:300', request); await tick(); assert.equal(calls, 1);
}
{
  const store = createFollowUpPromptStore(); let calls = 0;
  store.ensure('s', key, async () => { calls++; return {}; });
  store.skipGeneration('s', key); // An edit before the queued request actually starts.
  await tick(); assert.equal(calls, 0);
}
assert.equal(hasFollowUpDraft(), false);
assert.equal(hasFollowUpDraft({ content: { version: 1, parts: [{ type: 'text', text: '' }] } }), false);
for (const draft of [{ text: 'draft' }, { text: ' ' }, { attachments: [{}] }, { contexts: [{}] },
  { content: { version: 1, parts: [{ type: 'pasted_text', text: 'paste' }] } },
  { content: { version: 1, parts: [{ type: 'attachment', id: 'file1' }] } }]) assert.equal(hasFollowUpDraft(draft), true);
{
  const store = createFollowUpPromptStore(); let calls = 0;
  const fail = async () => { calls++; throw new Error('offline'); };
  store.ensure('s', key, fail); await tick(); store.ensure('s', key, fail); await tick();
  assert.equal(calls, 1); assert.equal(store.get('s', key).status, 'failed');
  store.ensure('other', key, async () => ({ completion_key: 'old', prompt: 'Wrong turn.' })); await tick();
  assert.equal(store.get('other', key).prompt, '');
}
assert.equal(validFollowUpPrompt('  Run tests.  '), 'Run tests.');
for (const value of [null, {}, 'x'.repeat(161), 'a\nb', 'a\u0000b']) assert.equal(validFollowUpPrompt(value), '');
assert.equal(validFollowUpPrompt('测'.repeat(160)).length, 160);
console.log('followUpPrompt tests passed');
