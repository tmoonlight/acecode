import assert from 'node:assert/strict';
import { composerContentFromText } from './composerContent.js';
import {
  createSideChatController,
  SIDE_CHAT_HISTORY_MAX_BYTES,
  sideChatHandoffText,
  sideChatQuoteDraft,
  sideChatToolVerb,
  sideChatTurnParts,
} from './sideChatController.js';

function run(name, fn) {
  try { fn(); console.log(`[pass] side chat controller: ${name}`); }
  catch (error) { console.error(`[fail] side chat controller: ${name}`); throw error; }
}

async function runAsync(name, fn) {
  try { await fn(); console.log(`[pass] side chat controller: ${name}`); }
  catch (error) { console.error(`[fail] side chat controller: ${name}`); throw error; }
}

function setup({ sendToMain, uploadAttachment } = {}) {
  const requests = [];
  const controller = createSideChatController({
    uploadAttachment,
    startStream(options) {
      const request = { ...options, stops: 0, disposals: 0, decisions: [] };
      requests.push(request);
      return {
        stop() { request.stops++; },
        dispose() { request.disposals++; },
        respondSend(sendId, confirmed, message) {
          request.decisions.push({ sendId, confirmed, message });
          return true;
        },
      };
    },
    sendToMain,
  });
  return { controller, requests, state: () => controller.getSnapshot() };
}

const nl = String.fromCharCode(10);

// 场景:侧边模型调用「发到主会话」,窗口收到确认请求;用户改写后点发送。
// 期望:卡片挂在对应工具行上、先是可编辑;确认时把改写后的正文经侧边连接回给
// daemon,状态变为发送中;daemon 回报 steered 后卡片显示已插入。
// 回归:侧边对话没有任何和主会话通信的途径。
run('model send request waits for confirmation and reports the delivery', () => {
  const { controller, requests, state } = setup();
  controller.submit('告诉主会话改用 B 方案');
  requests[0].onTool({ callId: 'c1', name: 'send_to_main_session', target: '', status: 'running' });
  requests[0].onSendRequest({ sendId: 's1', callId: 'c1', message: '改用 B 方案' });
  const card = state().turns[0].sends[0];
  assert.equal(card.status, 'pending');
  assert.equal(card.callId, 'c1');
  assert.equal(card.origin, 'model');
  assert.equal(controller.editSend('s1', '请改用 B 方案并重跑测试'), true);
  assert.equal(controller.confirmSend('s1'), true);
  assert.deepEqual(requests[0].decisions, [{ sendId: 's1', confirmed: true, message: '请改用 B 方案并重跑测试' }]);
  assert.equal(state().turns[0].sends[0].status, 'sending');
  assert.equal(controller.editSend('s1', 'late edit'), false);
  requests[0].onSendResult({ sendId: 's1', status: 'steered', message: '' });
  assert.equal(state().turns[0].sends[0].status, 'steered');
  assert.equal(sideChatToolVerb('send_to_main_session'), '发到主会话');
});

// 场景:用户拒绝发送,或在确认卡片还没作答时停止 / 最小化侧边对话。
// 期望:拒绝时回给 daemon「不发送」并显示未发送;请求结束时未作答的卡片作废,
// 不会留下一张点了也没用的卡片。
run('declining or ending the request settles model send cards without sending', () => {
  const { controller, requests, state } = setup();
  controller.submit('q');
  requests[0].onSendRequest({ sendId: 's1', callId: 'c1', message: 'm1' });
  assert.equal(controller.cancelSend('s1'), true);
  assert.deepEqual(requests[0].decisions, [{ sendId: 's1', confirmed: false, message: undefined }]);
  assert.equal(state().turns[0].sends[0].status, 'cancelled');
  requests[0].onSendRequest({ sendId: 's2', callId: 'c2', message: 'm2' });
  controller.close();
  assert.equal(state().turns[0].sends[1].status, 'cancelled');
  assert.equal(controller.confirmSend('s2'), false);
});

// 场景:用户在一条已完成的侧边回答上点「发到主会话」。
// 期望:卡片预填「问 + 答」(主会话看不到侧边对话),确认后经 REST 发送并显示
// daemon 给出的送达方式;失败显示错误;取消直接移除卡片;同一回答不重复开卡片。
await runAsync('user handoff prefills the question and answer and reports the delivery', async () => {
  const sent = [];
  let fail = false;
  const { controller, requests, state } = setup({
    sendToMain: async (message) => {
      sent.push(message);
      if (fail) throw new Error('daemon offline');
      return { delivery: 'queued' };
    },
  });
  controller.submit('为什么用互斥锁?');
  requests[0].onDelta('它保护快照。');
  requests[0].onDone({ answer: '它保护快照。', cancelled: false });
  const turnId = state().turns[0].id;
  assert.equal(controller.startSendToMain(turnId), true);
  assert.equal(controller.startSendToMain(turnId), false);
  const card = state().turns[0].sends[0];
  assert.equal(card.origin, 'user');
  assert.equal(card.text, sideChatHandoffText({ question: '为什么用互斥锁?', answer: '它保护快照。' }));
  assert.ok(card.text.includes('问:为什么用互斥锁?') && card.text.includes('答:“它保护快照。”'));
  assert.equal(controller.confirmSend(card.id), true);
  assert.equal(state().turns[0].sends[0].status, 'sending');
  await new Promise((resolve) => setTimeout(resolve, 0));
  assert.equal(state().turns[0].sends[0].status, 'queued');
  assert.deepEqual(sent, [card.text]);

  fail = true;
  assert.equal(controller.startSendToMain(turnId), true);
  const second = state().turns[0].sends[1];
  controller.confirmSend(second.id);
  await new Promise((resolve) => setTimeout(resolve, 0));
  assert.equal(state().turns[0].sends[1].status, 'failed');
  assert.equal(state().turns[0].sends[1].error, 'daemon offline');

  assert.equal(controller.startSendToMain(turnId), true);
  const third = state().turns[0].sends[2];
  assert.equal(controller.cancelSend(third.id), true);
  assert.equal(state().turns[0].sends.length, 2);
});

// 场景:主会话里选中文字点「发到侧边对话」。期望:以引用块追加到侧边输入框末尾,
// 打开窗口;侧边回答进行中也能继续编辑引用草稿。
run('quoting main session text into the side chat draft', () => {
  const { controller, state } = setup();
  assert.equal(controller.insertQuote(`第一行${nl}第二行`), true);
  assert.equal(state().open, true);
  assert.equal(state().draft, `> 第一行${nl}> 第二行${nl}${nl}`);
  assert.equal(sideChatQuoteDraft('已有问题  ', 'x'), `已有问题${nl}${nl}> x${nl}${nl}`);
  assert.equal(sideChatQuoteDraft('草稿', '   '), '草稿');
  controller.submit('q');
  assert.equal(controller.insertQuote('y'), true);
  assert.equal(state().draft, '> y\n\n');
});

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

run('waiting and stopping allow drafts but reject direct concurrent requests', () => {
  const { controller, requests, state } = setup();
  controller.submit('question');
  assert.equal(controller.submit('duplicate'), false);
  assert.equal(controller.setDraft('draft while loading'), true);
  requests[0].onDelta('partial');
  assert.equal(controller.setDraft('draft while streaming'), true);
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
  assert.deepEqual(state(), { open: true, turns: [], draft: '', busy: false, stopping: false, queued: [], queuePaused: false, composerContent: null, attachments: [], inputHistory: [], modelName: '', reasoningEffort: null });
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
    assert.deepEqual(state(), { open: true, turns: [], draft: '', busy: false, stopping: false, queued: [], queuePaused: false, composerContent: null, attachments: [], inputHistory: [], modelName: '', reasoningEffort: null });
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
  assert.deepEqual(controller.getSnapshot(), { open: true, turns: [], draft: '', busy: false, stopping: false, queued: [], queuePaused: false, composerContent: null, attachments: [], inputHistory: [], modelName: '', reasoningEffort: null });
});

run('session reset and dispose invalidate all previous callbacks', () => {
  const { controller, requests, state } = setup();
  controller.submit('old session');
  controller.reset();
  assert.equal(requests[0].stops, 1);
  assert.deepEqual(state(), { open: false, turns: [], draft: '', busy: false, stopping: false, queued: [], queuePaused: false, composerContent: null, attachments: [], inputHistory: [], modelName: '', reasoningEffort: null });
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

await runAsync('queued follow-ups run serially with completed history and preserve the next draft', async () => {
  const { controller, requests, state } = setup();
  controller.setDraft('first');
  controller.submitDraft();
  controller.setDraft('second\nline');
  assert.equal(controller.submitDraft(), true);
  assert.equal(controller.submitDraft(), false, 'an empty repeat cannot duplicate a queue item');
  controller.setDraft('third');
  controller.submitDraft();
  controller.setDraft('still writing');
  assert.equal(requests.length, 1);
  assert.deepEqual(state().queued.map((item) => item.text), ['second\nline', 'third']);
  requests[0].onDone({ answer: 'answer one' });
  await Promise.resolve();
  assert.equal(requests.length, 2);
  assert.equal(requests[1].question, 'second\nline');
  assert.deepEqual(requests[1].history, [{ role: 'user', content: 'first' }, { role: 'assistant', content: 'answer one' }]);
  assert.equal(state().draft, 'still writing');
  requests[0].onDone({ answer: 'late completion' });
  requests[1].onDone({ answer: 'answer two' });
  await Promise.resolve();
  assert.equal(requests.length, 3);
  assert.equal(requests[2].question, 'third');
  assert.equal(requests[2].history.length, 4);
  assert.equal(state().draft, 'still writing');
  assert.equal(state().queued.length, 0);
});

for (const action of ['stop', 'close', 'suspend', 'error']) {
  await runAsync(`${action} pauses queued messages until explicit resume`, async () => {
    const { controller, requests, state } = setup();
    controller.submit('first');
    controller.setDraft('queued');
    controller.submitDraft();
    requests[0].onDelta('partial');
    if (action === 'error') requests[0].onError({ message: 'offline' });
    else controller[action]();
    if (action === 'stop') requests[0].onDone({ answer: 'partial', cancelled: true });
    await Promise.resolve();
    assert.equal(state().queuePaused, true);
    assert.equal(requests.length, 1);
    controller.open();
    await Promise.resolve();
    assert.equal(requests.length, 1, 'reopening does not resume a paused queue');
    assert.equal(controller.submitDraft(), true, 'empty Enter resumes the queue');
    await Promise.resolve();
    assert.equal(requests.length, 2);
    assert.equal(requests[1].question, 'queued');
    assert.equal(requests[1].history.length, action === 'error' ? 0 : 2);
  });
}

await runAsync('queue cancellation, clear and suspend invalidate scheduled dispatch', async () => {
  for (const action of ['cancel', 'clear', 'reset', 'dispose', 'suspend']) {
    const { controller, requests, state } = setup();
    controller.submit('first');
    controller.setDraft('queued');
    controller.submitDraft();
    const queuedId = state().queued[0].id;
    requests[0].onDone({ answer: 'done' });
    if (action === 'cancel') controller.cancelQueued(queuedId);
    else controller[action]();
    await Promise.resolve();
    assert.equal(requests.length, 1, action);
    assert.equal(state().queued.length, action === 'suspend' ? 1 : 0);
  }
});

await runAsync('queue validation keeps invalid drafts and rechecks history at dispatch', async () => {
  const { controller, requests, state } = setup();
  controller.submit('first');
  controller.setDraft('中'.repeat(5334));
  assert.equal(controller.submitDraft(), false);
  assert.equal(state().queued.length, 0);
  assert.equal(state().draft.length, 5334);
  assert.equal(state().turns.at(-1).errorCode, 'SIDE_CHAT_QUESTION_TOO_LONG');
  controller.setDraft('next');
  controller.submitDraft();
  requests[0].onDone({ answer: 'a'.repeat(SIDE_CHAT_HISTORY_MAX_BYTES) });
  await Promise.resolve();
  assert.equal(requests.length, 1);
  assert.equal(state().queued[0].text, 'next');
  assert.equal(state().queuePaused, true);
  assert.equal(state().turns.at(-1).errorCode, 'SIDE_CHAT_HISTORY_TOO_LONG');
});

await runAsync('explicit submission during stop acknowledgement resumes after acknowledgement', async () => {
  const { controller, requests, state } = setup();
  controller.submit('first');
  controller.stop();
  controller.setDraft('queued during stop');
  controller.submitDraft();
  requests[0].onDone({ answer: 'partial', cancelled: true });
  await Promise.resolve();
  assert.equal(requests.length, 2);
  assert.equal(requests[1].question, 'queued during stop');
  assert.equal(state().queuePaused, false);
});

await runAsync('a new idle submission runs before paused messages and resumes the queue', async () => {
  const { controller, requests, state } = setup();
  controller.submit('first');
  controller.setDraft('old queued');
  controller.submitDraft();
  controller.stop();
  requests[0].onDone({ answer: 'partial', cancelled: true });
  await Promise.resolve();
  controller.setDraft('new message');
  controller.submitDraft();
  assert.equal(requests[1].question, 'new message');
  assert.equal(state().queuePaused, false);
  requests[1].onDone({ answer: 'new answer' });
  await Promise.resolve();
  assert.equal(requests[2].question, 'old queued');
});

await runAsync('rich drafts, images and selected models survive hiding and queue independently', async () => {
  const { controller, requests, state } = setup({ uploadAttachment: async file => ({
    attachment: { id: 'image-1', name: file.name, kind: 'image', mime_type: file.type, session_id: 'main' },
  }) });
  controller.open();
  controller.setModel('vision-a', 'high');
  await controller.addFiles([new File(['image'], 'photo.png', { type: 'image/png' })]);
  controller.setDraft('describe', composerContentFromText('describe', state().attachments));
  controller.close();
  controller.open();
  assert.equal(state().attachments[0].id, 'image-1');
  assert.equal(state().modelName, 'vision-a');
  assert.equal(controller.submitDraft(), true);
  assert.equal(requests[0].modelName, 'vision-a');
  assert.equal(requests[0].reasoningEffort, 'high');
  assert.deepEqual(requests[0].input.attachments, [{ id: 'image-1' }]);
  controller.setModel('text-b');
  controller.setDraft('follow-up', composerContentFromText('follow-up'));
  controller.submitDraft();
  controller.setModel('text-c');
  controller.setDraft('unsent draft');
  requests[0].onDone({ answer: 'a photo' });
  await Promise.resolve();
  assert.equal(requests[1].modelName, 'text-b', 'queued model is a submission snapshot');
  assert.deepEqual(requests[1].history[0].input.attachments, [{ id: 'image-1' }]);
  assert.equal(state().draft, 'unsent draft');
  assert.equal(state().modelName, 'text-c');
  assert.equal(state().inputHistory[0].composer_content.parts[0].id, 'image-1');
  controller.dispose();
});

await runAsync('attachment-only input submits and remains in follow-up history', async () => {
  const { controller, requests } = setup({ uploadAttachment: async () => ({ attachment: { id: 'image', kind: 'image', name: 'p.png' } }) });
  await controller.addFiles([new File(['png'], 'p.png', { type: 'image/png' })]);
  assert.equal(controller.submitDraft(), true);
  assert.equal(requests[0].question, '');
  requests[0].onDone({ answer: 'image description' });
  controller.setDraft('more');
  controller.submitDraft();
  assert.equal(requests[1].history[0].input.attachments[0].id, 'image');
  controller.dispose();
});

await runAsync('clear invalidates late uploads and failures preserve retryable files', async () => {
  let finish;
  const { controller, state } = setup({ uploadAttachment: () => new Promise(resolve => { finish = resolve; }) });
  const pending = controller.addFiles([new File(['data'], 'file.txt')]);
  await Promise.resolve();
  controller.clear();
  finish({ attachment: { id: 'stale', name: 'file.txt' } });
  await pending;
  assert.deepEqual(state().attachments, []);
  assert.equal(state().composerContent, null);
  let calls = 0;
  const retry = setup({ uploadAttachment: async () => {
    if (++calls === 1) throw new Error('offline');
    return { attachment: { id: 'recovered', name: 'file.txt' } };
  } });
  await retry.controller.addFiles([new File(['data'], 'file.txt')]);
  assert.equal(retry.state().attachments[0].upload_error, 'offline');
  assert.equal(retry.controller.submitDraft(), false);
  await retry.controller.retryUpload(retry.state().attachments[0].local_id);
  assert.equal(retry.controller.submitDraft(), true);
  assert.equal(retry.requests[0].input.attachments[0].id, 'recovered');
});

await runAsync('removing an attachment keeps its resource for undo without blocking a plain draft', async () => {
  const { controller, state, requests } = setup({ uploadAttachment: async () => { throw new Error('offline'); } });
  await controller.addFiles([new File(['data'], 'file.txt')]);
  const before = state().composerContent;
  const key = state().attachments[0].local_id;
  controller.removeAttachment(key);
  assert.equal(state().attachments.length, 1, 'resource survives removal for editor undo');
  controller.setDraft('restored', before);
  assert.equal(state().composerContent.parts[0].key, key);
  assert.equal(controller.submitDraft(), false, 'restored failed upload still requires retry');
  controller.removeAttachment(key);
  controller.setDraft('plain', composerContentFromText('plain'));
  assert.equal(controller.submitDraft(), true);
  assert.equal(requests[0].input.attachments.length, 0);
});
