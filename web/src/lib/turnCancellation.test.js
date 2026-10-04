import assert from 'node:assert/strict';
import { createTranscriptState, reduceTranscriptEvent, preserveLiveRuntimeOnLoad, loadTranscriptHistory } from './sessionTranscript.js';
import { acceptedUserInputEvent, createChatInputQueueState, enqueueQueuedInput, pauseQueuedInput, resumeQueuedInput, shouldPauseQueuedInputAfterAbort, shouldDrainQueuedInput } from './chatInputQueue.js';

let state;
const apply = (type, payload = {}) => {
  state = reduceTranscriptEvent(state, { type, payload }).state;
};
const begin = (id) => {
  apply('message', { id: `user-${id}`, role: 'user', content: '继续' });
  apply('busy_changed', { busy: true, turn_id: id });
};
const notices = () => state.items.filter((item) => item.kind === 'termination_notice');
const confirm = (id) => apply('message', { id: `stop-${id}`, role: 'system', content: '[Interrupted]', metadata: { transcript_only: true, user_aborted: true, turn_id: id } });

state = createTranscriptState();
begin('t1');
apply('turn_abort_requested', { turn_id: 't1' });
apply('turn_abort_requested', { turn_id: 't1' });
assert.equal(state.busy, true);
assert.equal(state.activeTurnId, 't1');
assert.equal(state.status, 'stopping');
assert.equal(notices().length, 1);
assert.equal(notices()[0].pending, true);
for (const tool of ['vision_analyze', 'web_search', 'bash', 'mcp_example']) {
  apply('agent_progress', { phase: 'tool_running', tool });
  apply('message', { role: 'tool_call', content: tool });
  apply('message', { role: 'tool_result', content: 'late result' });
  assert.equal(state.status, 'stopping');
  assert.equal(state.abortPending, true);
}
confirm('t1');
assert.equal(notices().length, 1);
assert.equal(notices()[0].messageId, 'stop-t1');
assert.equal(notices()[0].content, '用户已终止本轮任务');
assert.equal(state.busy, true, '持久化提示不抢先结束运行状态');
apply('busy_changed', { busy: false, outcome: 'aborted', turn_id: 't1' });
assert.equal(state.abortPending, false);
assert.equal(state.busy, false);
confirm('t1');
assert.equal(notices().length, 1);

// HTTP submission can begin before the preceding terminal done is delivered.
apply('busy_changed', { busy: true });
apply('done', { outcome: 'aborted', turn_id: 't1' });
assert.equal(state.busy, true);
begin('t2');
apply('done', { outcome: 'aborted', turn_id: 't1' });
assert.equal(state.activeTurnId, 't2');
apply('turn_abort_requested', { turn_id: 't2' });
confirm('t2');
assert.equal(notices().length, 2);
apply('done', { outcome: 'aborted', turn_id: 't2' });
assert.equal(state.busy, false);

for (const terminal of ['done', 'busy_changed']) {
  state = createTranscriptState();
  begin('legacy-terminal');
  apply('token', { text: 'partial answer' });
  apply('turn_abort_requested');
  const ended = reduceTranscriptEvent(state, { type: terminal, payload: { busy: false } });
  assert.equal(ended.state.lastTurnOutcome, 'aborted');
  assert.equal(ended.state.lastTerminalTurnId, 'legacy-terminal');
  assert.deepEqual(ended.effects, [], 'legacy stop acknowledgement is not a completion notification');
}

// Legacy confirmation without a turn id still merges across hidden tool rows.
state = createTranscriptState();
apply('busy_changed', { busy: true });
apply('turn_abort_requested');
apply('message', { role: 'tool_result', content: 'late' });
apply('message', { id: 'old-stop', role: 'system', metadata: { transcript_only: true, user_aborted: true } });
assert.equal(notices().length, 1);

// A current busy snapshot must not erase a local stop that has no event seq.
state = createTranscriptState();
begin('t3');
apply('turn_abort_requested', { turn_id: 't3' });
const restored = preserveLiveRuntimeOnLoad(createTranscriptState({ busy: true, activeTurnId: 't3' }), state);
assert.equal(restored.abortPending, true);
assert.equal(restored.status, 'stopping');
assert.equal(loadTranscriptHistory(state, { messages: [], busy: true, active_turn_id: 't3' }).state.abortPending, true);
apply('turn_abort_failed');
assert.equal(state.abortPending, false);
assert.equal(state.busy, true);
assert.equal(notices().length, 0);
apply('turn_abort_requested', { turn_id: 't3' });
assert.equal(state.abortPending, true, '断线后可重试停止');

// Accepted plain submissions reconcile by client id, including identical text.
state = createTranscriptState();
for (const id of ['send-1', 'send-2']) {
  const event = acceptedUserInputEvent({ text: '继续', client_message_id: id });
  state = reduceTranscriptEvent(state, event).state;
  const refresh = loadTranscriptHistory(state, { messages: [], busy: true }).state;
  assert.ok(refresh.items.some((item) => item.metadata?.client_message_id === id));
  const replacement = reduceTranscriptEvent(state, { type: 'transcript_replace', payload: { messages: [] } }).state;
  assert.ok(replacement.items.some((item) => item.metadata?.client_message_id === id));
  apply('message', { id, role: 'user', content: '继续', metadata: { client_message_id: id } });
  state = reduceTranscriptEvent(state, event).state;
}
assert.equal(state.items.length, 2);
assert.deepEqual(state.items.map((item) => item.messageId), ['send-1', 'send-2']);

let queue = enqueueQueuedInput(createChatInputQueueState(), { sessionId: 's1', text: '旧消息' });
queue = pauseQueuedInput(queue, 's1');
queue = resumeQueuedInput(queue, 's1', { afterAbortTurnId: 't1' });
queue = enqueueQueuedInput(queue, { sessionId: 's1', text: '新消息' });
assert.equal(shouldDrainQueuedInput({ sessionId: 's1', busy: true }), false);
assert.equal(shouldPauseQueuedInputAfterAbort({ state: queue, sessionId: 's1', lastTurnOutcome: 'aborted', turnId: 't1' }), false);
assert.equal(shouldPauseQueuedInputAfterAbort({ state: queue, sessionId: 's1', lastTurnOutcome: 'aborted', turnId: 't2' }), true);
queue = pauseQueuedInput(queue, 's1');
assert.equal(queue.resumedAbortTurns.s1, undefined);
console.log('[pass] turn cancellation, late tools, terminal identity, accepted input and queue intent');
