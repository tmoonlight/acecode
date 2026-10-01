import assert from 'node:assert/strict';
import {
  createSideChatController,
  SIDE_CHAT_HISTORY_MAX_BYTES,
  sideChatToolVerb,
  sideChatTurnParts,
} from './sideChatController.js';

function run(name, fn) {
  try { fn(); console.log(`[pass] side chat controller: ${name}`); }
  catch (error) { console.error(`[fail] side chat controller: ${name}`); throw error; }
}

function setup() {
  const requests = [];
  const controller = createSideChatController({
    startStream(options) {
      const request = { ...options, stops: 0, disposals: 0 };
      requests.push(request);
      return {
        stop() { request.stops++; },
        dispose() { request.disposals++; },
      };
    },
  });
  return { controller, requests, state: () => controller.getSnapshot() };
}

// 场景:侧边回答先写一段、调用只读工具,再继续写;中途 provider 重试。
// 期望:工具行记下调用发生时回答的位置,reset 只丢弃工具之后这一步的正文;
// 渲染分段为「前文 / 工具行 / 后文」,完整回答进入后续追问的历史。
run('tool rows keep their position and reset discards only the current step', () => {
  const { controller, requests, state } = setup();
  controller.submit('question');
  requests[0].onDelta('before');
  requests[0].onTool({ callId: 'c1', name: 'file_read', target: 'a.txt', status: 'running' });
  assert.equal(state().turns[0].tools[0].offset, 6);
  assert.equal(state().turns[0].stepStart, 6);
  requests[0].onTool({ callId: 'c1', name: 'file_read', target: 'a.txt', status: 'success' });
  assert.equal(state().turns[0].tools.length, 1);
  assert.equal(state().turns[0].tools[0].status, 'success');
  const gap = String.fromCharCode(10).repeat(2);
  requests[0].onDelta(`${gap}draft`);
  requests[0].onReset();
  assert.equal(state().turns[0].answer, 'before');
  assert.equal(state().turns[0].status, 'streaming');
  requests[0].onDelta(`${gap}after`);
  assert.deepEqual(sideChatTurnParts(state().turns[0]).map((part) => part.kind), ['text', 'tools', 'text']);
  assert.equal(sideChatTurnParts(state().turns[0])[2].text, `${gap}after`);
  requests[0].onDone({ answer: `before${gap}after`, cancelled: false });
  controller.submit('follow-up');
  assert.deepEqual(requests[1].history, [
    { role: 'user', content: 'question' },
    { role: 'assistant', content: `before${gap}after` },
  ]);
});

// 场景:工具还在执行时用户最小化 / 停止。期望:界面不会留下一直转圈的工具行。
run('closing during a running tool marks the row cancelled', () => {
  const { controller, requests, state } = setup();
  controller.submit('question');
  requests[0].onTool({ callId: 'c1', name: 'grep', target: 'TODO', status: 'running' });
  controller.close();
  assert.equal(state().turns[0].tools[0].status, 'cancelled');
  assert.equal(state().turns[0].status, 'stopped');
});

run('turn parts clamp offsets, group adjacent tools and skip blank separators', () => {
  const parts = sideChatTurnParts({
    answer: `A${String.fromCharCode(10).repeat(2)}`,
    tools: [{ id: 'x', offset: 1 }, { id: 'y', offset: 1 }, { id: 'z', offset: 99 }],
  });
  assert.deepEqual(parts.map((part) => part.kind), ['text', 'tools']);
  assert.deepEqual(parts[1].tools.map((tool) => tool.id), ['x', 'y', 'z']);
  assert.equal(sideChatToolVerb('grep'), '搜索');
  assert.equal(sideChatToolVerb('unknown_tool'), '调用工具');
});

run('follow-ups include detached complete pairs and use the authoritative final answer', () => {
  const { controller, requests, state } = setup();
  controller.open();
  controller.setDraft('first question');
  assert.equal(controller.submit(), true);
  assert.deepEqual(requests[0].history, []);
  assert.equal(state().draft, '');
  assert.equal(state().turns[0].status, 'loading');
  requests[0].onDelta('first');
  assert.equal(state().turns[0].status, 'streaming');
  requests[0].onDone({ answer: 'first answer', cancelled: false });
  controller.submit('follow-up');
  assert.deepEqual(requests[1].history, [
    { role: 'user', content: 'first question' },
    { role: 'assistant', content: 'first answer' },
  ]);
  assert.notEqual(requests[0].requestId, requests[1].requestId);
});

run('waiting, streaming and stop acknowledgement reject edits and duplicate submissions', () => {
  const { controller, requests, state } = setup();
  controller.submit('question');
  assert.equal(controller.submit('duplicate'), false);
  assert.equal(controller.setDraft('cannot edit while loading'), false);
  requests[0].onDelta('partial');
  assert.equal(controller.setDraft('cannot edit while streaming'), false);
  assert.equal(controller.stop(), true);
  assert.equal(controller.stop(), false);
  assert.equal(requests[0].stops, 1);
  assert.equal(state().busy, true);
  assert.equal(state().stopping, true);
  assert.equal(controller.submit('before ack'), false);
  requests[0].onDone({ answer: 'partial', cancelled: true });
  assert.equal(state().busy, false);
  assert.equal(state().stopping, false);
  assert.equal(state().turns[0].status, 'stopped');
  assert.equal(controller.setDraft('after ack'), true);
  assert.equal(controller.submit(), true);
  assert.deepEqual(requests[1].history, [
    { role: 'user', content: 'question' },
    { role: 'assistant', content: 'partial' },
  ]);
});

run('failed partial and empty cancelled turns stay visible but never enter history', () => {
  const { controller, requests, state } = setup();
  controller.submit('failed question');
  requests[0].onDelta('failed partial');
  requests[0].onError({ code: 'MODEL_ERROR', message: 'provider failed' });
  assert.equal(state().turns[0].answer, 'failed partial');
  assert.equal(state().turns[0].error, 'provider failed');
  assert.equal(state().turns[0].errorCode, 'MODEL_ERROR');
  assert.equal(state().busy, false);
  controller.submit('cancelled question');
  requests[1].onDone({ answer: ' \n ', cancelled: true });
  controller.submit('third question');
  assert.deepEqual(requests[2].history, []);
  assert.equal(state().turns.length, 3);
});

run('retry reset discards provisional output before the next attempt', () => {
  const { controller, requests, state } = setup();
  controller.submit('question');
  requests[0].onDelta('discarded attempt');
  requests[0].onReset();
  assert.equal(state().turns[0].answer, '');
  assert.equal(state().turns[0].status, 'loading');
  assert.equal(state().busy, true);
  requests[0].onDelta('second attempt');
  requests[0].onDone({ answer: 'second attempt' });
  assert.equal(state().turns[0].answer, 'second attempt');
});

run('whitespace-only completion is a visible failure and never becomes follow-up context', () => {
  const { controller, requests, state } = setup();
  controller.submit('question');
  requests[0].onDone({ answer: ' \n ' });
  assert.equal(state().turns[0].status, 'error');
  assert.equal(state().turns[0].errorCode, 'SIDE_CHAT_EMPTY_RESPONSE');
  assert.equal(state().busy, false);
  controller.submit('retry');
  assert.deepEqual(requests[1].history, []);
});

run('cancel acknowledgement cannot discard text already received', () => {
  const { controller, requests, state } = setup();
  controller.submit('question');
  requests[0].onDelta('partial answer');
  controller.stop();
  requests[0].onDone({ answer: '', cancelled: true });
  assert.equal(state().turns[0].answer, 'partial answer');
  assert.equal(state().turns[0].status, 'stopped');
});

run('closing cancels immediately and reopening preserves transcript and unsent draft', () => {
  const { controller, requests, state } = setup();
  controller.submit('question');
  requests[0].onDelta('partial');
  controller.close();
  assert.equal(requests[0].stops, 1);
  assert.equal(requests[0].disposals, 1);
  assert.equal(state().open, false);
  assert.equal(state().busy, false);
  assert.equal(state().turns[0].status, 'stopped');
  requests[0].onDelta('stale after close');
  requests[0].onDone({ answer: 'stale final' });
  controller.open();
  assert.equal(state().turns[0].answer, 'partial');
  controller.setDraft('unsent draft');
  controller.close();
  controller.open();
  assert.equal(state().draft, 'unsent draft');
});

run('clearing discards completed history and draft while keeping the window open', () => {
  const { controller, requests, state } = setup();
  controller.submit('old question');
  requests[0].onDone({ answer: 'old answer' });
  controller.setDraft('old draft');
  controller.clear();
  assert.deepEqual(state(), { open: true, turns: [], draft: '', busy: false, stopping: false });
  assert.equal(controller.setDraft('fresh question'), true);
  assert.equal(controller.submit(), true);
  assert.deepEqual(requests[1].history, []);
  controller.close();
  controller.clear();
  assert.equal(state().open, false);
});

for (const phase of ['loading', 'streaming', 'stopping']) {
  run(`clearing while ${phase} cancels the request and isolates the next conversation`, () => {
    const { controller, requests, state } = setup();
    controller.submit('old question');
    if (phase !== 'loading') requests[0].onDelta('old partial');
    if (phase === 'stopping') controller.stop();
    controller.clear();
    assert.ok(requests[0].stops > 0);
    assert.equal(requests[0].disposals, 1);
    assert.deepEqual(state(), { open: true, turns: [], draft: '', busy: false, stopping: false });
    assert.equal(controller.submit('fresh question'), true);
    assert.deepEqual(requests[1].history, []);
    const fresh = state();
    requests[0].onDelta('late delta');
    requests[0].onReset();
    requests[0].onError({ message: 'late error' });
    requests[0].onDone({ answer: 'late final', cancelled: true });
    assert.equal(state(), fresh);
    requests[1].onDone({ answer: 'fresh answer' });
    assert.equal(state().turns[0].answer, 'fresh answer');
    assert.equal(state().busy, false);
  });
}

run('clear invalidates synchronous cancellation callbacks before clearing the transcript', () => {
  let disposed = false;
  const controller = createSideChatController({ startStream({ onDelta, onDone }) {
    return {
      stop() {
        onDelta('late cancellation text');
        onDone({ answer: 'late cancellation answer', cancelled: true });
      },
      dispose() { disposed = true; },
    };
  } });
  controller.submit('old question');
  controller.clear();
  assert.equal(disposed, true);
  assert.deepEqual(controller.getSnapshot(), { open: true, turns: [], draft: '', busy: false, stopping: false });
});

run('session reset and dispose invalidate all previous callbacks', () => {
  const { controller, requests, state } = setup();
  controller.submit('old session');
  controller.reset();
  assert.equal(requests[0].stops, 1);
  assert.deepEqual(state(), { open: false, turns: [], draft: '', busy: false, stopping: false });
  controller.submit('new session');
  requests[0].onDelta('stale');
  requests[0].onReset();
  requests[0].onError({ message: 'stale error' });
  requests[0].onDone({ answer: 'stale final' });
  assert.equal(state().turns.length, 1);
  assert.equal(state().turns[0].answer, '');
  assert.equal(state().busy, true);
  controller.dispose();
  requests[1].onDone({ answer: 'stale after dispose' });
  assert.equal(state().turns.length, 0);
  assert.equal(requests[1].disposals, 1);
});

run('completed callbacks cannot overwrite the next active turn', () => {
  const { controller, requests, state } = setup();
  controller.submit('first');
  requests[0].onDone({ answer: 'answer' });
  controller.submit('second');
  requests[0].onDelta('late first delta');
  requests[0].onReset();
  requests[0].onDone({ answer: 'late first done' });
  assert.equal(state().turns[0].answer, 'answer');
  assert.equal(state().turns[1].answer, '');
  assert.equal(state().busy, true);
});

run('synchronous transport failure unlocks input and disposes its returned handle', () => {
  let disposals = 0;
  const controller = createSideChatController({ startStream({ onError }) {
    onError({ code: 'CONNECT', message: 'cannot connect' });
    return { dispose() { disposals++; } };
  } });
  controller.submit('question');
  assert.equal(controller.getSnapshot().busy, false);
  assert.equal(controller.getSnapshot().turns[0].error, 'cannot connect');
  assert.equal(disposals, 1);
});

run('construction exceptions become visible errors instead of leaving a busy turn', () => {
  const controller = createSideChatController({ startStream() { throw new Error('startup failed'); } });
  controller.submit('question');
  assert.equal(controller.getSnapshot().busy, false);
  assert.equal(controller.getSnapshot().turns[0].status, 'error');
  assert.equal(controller.getSnapshot().turns[0].error, 'startup failed');
});

run('question size is bounded in UTF8 bytes and preserves an invalid draft for editing', () => {
  const { controller, requests, state } = setup();
  controller.setDraft('中'.repeat(5334));
  assert.equal(controller.submit(), false);
  assert.equal(requests.length, 0);
  assert.equal(state().turns[0].errorCode, 'SIDE_CHAT_QUESTION_TOO_LONG');
  assert.equal(state().draft, '中'.repeat(5334));
  controller.setDraft('中'.repeat(5333));
  assert.equal(controller.submit(), true);
});

run('history size errors neither truncate successful pairs nor start a model request', () => {
  const { controller, requests, state } = setup();
  controller.submit('q');
  requests[0].onDone({ answer: 'a'.repeat(SIDE_CHAT_HISTORY_MAX_BYTES) });
  assert.equal(controller.submit('follow-up'), false);
  assert.equal(requests.length, 1);
  assert.equal(state().turns[0].answer.length, SIDE_CHAT_HISTORY_MAX_BYTES);
  assert.equal(state().turns[1].errorCode, 'SIDE_CHAT_HISTORY_TOO_LONG');
  assert.equal(state().busy, false);
});

run('history accepts 200 messages and rejects the next complete pair', () => {
  const { controller, requests, state } = setup();
  for (let i = 0; i < 101; i++) {
    assert.equal(controller.submit(`question ${i}`), true);
    requests[i].onDone({ answer: 'answer' });
  }
  assert.equal(requests[100].history.length, 200);
  assert.equal(controller.submit('over limit'), false);
  assert.equal(state().turns.at(-1).errorCode, 'SIDE_CHAT_HISTORY_TOO_LONG');
});

run('subscribers observe immutable snapshots and unsubscribe cleanly', () => {
  const { controller, state } = setup();
  const before = state();
  let notifications = 0;
  const unsubscribe = controller.subscribe(() => notifications++);
  controller.open();
  assert.equal(notifications, 1);
  assert.notEqual(state(), before);
  assert.equal(before.open, false);
  unsubscribe();
  controller.setDraft('draft');
  assert.equal(notifications, 1);
});
