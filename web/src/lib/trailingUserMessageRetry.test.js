import assert from 'node:assert/strict';
import fs from 'node:fs';
import { trailingUserMessageRetryId } from './trailingUserMessageRetry.js';
import { createTranscriptState, loadTranscriptHistory, reduceTranscriptEvent } from './sessionTranscript.js';

const user = { role: 'user', content: 'original', id: 'user-1' };
const loaded = (...messages) => loadTranscriptHistory(createTranscriptState(), { messages }).state;
const retryId = (state) => trailingUserMessageRetryId({ ...state, sessionId: 'session-1' });

assert.equal(retryId(loaded(user)), 'user-1');
assert.equal(retryId(loaded()), '');
assert.equal(retryId(loaded({ ...user, id: '' })), '');
for (const role of ['assistant', 'tool', 'system', 'error']) {
  for (const content of ['', 'later content']) {
    assert.equal(retryId(loaded(user, { role, content })), '', `${role} must block even when empty`);
  }
}
const history = [user, { role: 'assistant', content: 'done' }, { ...user, id: 'user-2' }];
assert.equal(retryId(loaded(...history)), 'user-2');
assert.equal(retryId(loaded(...history, { role: 'assistant', content: '', tool_calls: [
  { id: 'call-1', type: 'function', function: { name: 'bash', arguments: '{}' } },
] })), '');

for (const blocker of [
  { loadState: 'loading' }, { loadState: 'error' }, { busy: true },
  { status: 'running' }, { streamingId: 0 }, { disabled: true },
  { abortPending: true },
]) assert.equal(retryId({ ...loaded(user), ...blocker }), '');
assert.equal(trailingUserMessageRetryId(loaded(user)), '');
for (const extra of [{ queued: {} }, { streaming: true }, { messageId: '' }]) {
  assert.equal(retryId({ ...loaded(user), items: [{ ...loaded(user).items[0], ...extra }] }), '');
}
for (const event of [
  { type: 'token', payload: { text: 'answer' } },
  { type: 'reasoning', payload: { text: 'thinking' } },
  { type: 'tool_start', payload: { tool: 'bash', tool_call_id: 'call-1' } },
  { type: 'message', payload: { role: 'system', content: 'later notice' } },
]) {
  const next = reduceTranscriptEvent(loaded(user), event).state;
  assert.equal(retryId(next), '', `${event.type} must invalidate the previous tail`);
}

const stopped = {
  role: 'system', content: '[Interrupted]', id: 'stop-1',
  metadata: { transcript_only: true, user_aborted: true, retry_user_message_id: 'user-1' },
};
for (const output of [[], [{ role: 'assistant', content: 'partial reply' }], [
  { role: 'assistant', content: '', tool_calls: [
    { id: 'call-1', type: 'function', function: { name: 'bash', arguments: '{}' } },
  ] },
  { role: 'tool', content: 'tool result', tool_call_id: 'call-1' },
]]) {
  const restored = loaded(user, ...output, stopped);
  assert.equal(retryId(restored), 'user-1', 'a persisted stop restores retry after output');
  assert.equal(retryId({ ...restored, abortPending: true }), '');
  for (const role of ['assistant', 'tool', 'system', 'error']) {
    assert.equal(retryId(loaded(user, ...output, stopped, { role, content: 'later' })), '');
  }
}
assert.equal(retryId(loaded(user, { ...stopped, metadata: undefined })), '');
assert.equal(retryId(loaded(user, { ...stopped, metadata: { turn_interrupt: true } })), '');
assert.equal(retryId(loaded(user, { ...stopped, metadata: { ...stopped.metadata, retry_user_message_id: 'stale' } })), '');
assert.equal(retryId(loaded(user, { ...user, id: 'user-2' }, stopped)), '');
const completedAfterStop = loaded(user, stopped, { role: 'assistant', content: 'completed retry' });
const oldStopReplay = reduceTranscriptEvent(completedAfterStop, {
  type: 'message', payload: stopped, replayed: true,
}).state;
assert.equal(oldStopReplay.items.length, completedAfterStop.items.length);
assert.equal(retryId(oldStopReplay), '', 'an old stop replay must not grant a completed turn another retry');
const partial = {
  id: 'partial-1', role: 'assistant', content: 'partial reply',
  metadata: { transcript_only: true, interrupted_output: true },
};
const restoredWithReplay = loadTranscriptHistory(createTranscriptState(), {
  messages: [user, partial, stopped], busy: false,
  events: [
    { type: 'busy_changed', payload: { busy: true }, seq: 1 },
    { type: 'token', payload: { text: 'partial reply' }, seq: 2 },
    { type: 'message', payload: partial, seq: 3 },
    { type: 'message', payload: stopped, seq: 4 },
    { type: 'done', payload: { outcome: 'aborted' }, seq: 5 },
  ],
}).state;
assert.equal(retryId(restoredWithReplay), 'user-1');
assert.deepEqual(restoredWithReplay.items.map((item) => item.content), [
  'original', 'partial reply', '用户已终止本轮任务',
]);

let abortState = loaded(user);
const applyAbortEvent = (type, payload = {}) => {
  abortState = reduceTranscriptEvent(abortState, { type, payload }).state;
};
applyAbortEvent('busy_changed', { busy: true, turn_id: 'user-1' });
applyAbortEvent('token', { text: 'partial re' });
applyAbortEvent('turn_aborted');
assert.equal(retryId(abortState), '', 'a local stop must wait for the daemon');
applyAbortEvent('token', { text: 'ply' });
applyAbortEvent('message', partial);
applyAbortEvent('message', stopped);
assert.equal(retryId(abortState), '', 'the stop notice alone does not finish the worker');
assert.equal(abortState.items.filter((item) => item.kind === 'termination_notice').length, 1);
applyAbortEvent('busy_changed', { busy: false, outcome: 'aborted' });
applyAbortEvent('done', { outcome: 'aborted' });
assert.equal(retryId(abortState), 'user-1');
assert.deepEqual(abortState.items.map((item) => item.content), [
  'original', 'partial reply', '用户已终止本轮任务',
]);
applyAbortEvent('error', { reason: 'later error' });
assert.equal(retryId(abortState), '', 'a later failure invalidates the stop');

// Submit clicks and Enter must share a gate; only the empty busy stop action
// bypasses submission. The send path must inspect the
// latest complete store rather than composer history or a rendered window.
const inputBar = fs.readFileSync(new URL('../components/InputBar.jsx', import.meta.url), 'utf8');
assert.match(inputBar, /const submit = \(\) => \{\s*if \(!actionState\.canSubmit\) return;/);
assert.match(inputBar, /onSubmit=\{submit\}/);
assert.match(inputBar, /const isStopAction = actionState\.mode === 'stop';/);
assert.match(inputBar, /onClick=\{isStopAction \? onAbort : submit\}/);
const chat = fs.readFileSync(new URL('../components/ChatView.jsx', import.meta.url), 'utf8');
assert.match(chat, /trailingUserMessageRetryId\(\{\s*sessionId: sid, items, loadState:/);
assert.match(chat, /const latest = transcript\.getState\(\);/);
assert.match(chat, /if \(latestRetryId !== retryUserMessageId\) return;/);
assert.match(chat, /api\.retryLastUserMessage\(sid, latestRetryId\)/);
console.log('[pass] trailing user message retry uses the complete transcript and rejects stale or non-user tails');
