import assert from 'node:assert/strict';
import {
  createTranscriptState,
  loadTranscriptHistory,
  mergeTurnTimingsFromMessages,
  pendingTurnTimingUserId,
  reduceTranscriptEvent,
} from './sessionTranscript.js';
import { followUpCompletionKey } from './followUpPrompt.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

function reduceMany(events, initial) {
  return events.reduce((state, event) => reduceTranscriptEvent(state, event).state, initial);
}

// 已加载、实时订阅中的会话,对话为空。
function liveLoadedState() {
  return createTranscriptState({ loadState: 'loaded', isLive: true });
}

// daemon 落盘的 [Turn timing] 记录,REST 历史页里原样返回。
function timingRecord(userId, completedAtMs, status = 'completed') {
  return {
    id: `timing-${userId}`,
    role: 'system',
    content: '[Turn timing]',
    metadata: {
      transcript_only: true,
      turn_timing: {
        user_message_uuid: userId,
        started_at_ms: 1000,
        completed_at_ms: completedAtMs,
        duration_ms: completedAtMs - 1000,
        status,
      },
    },
  };
}

// 与 daemon 实时推送的顺序一致:先推 user 消息,再 busy_changed(true),回合结束时
// busy_changed(false) 之后跟 done。turn_timing 只写进 JSONL,不在这串事件里。
function liveTurnEvents(userId, { outcome = 'completed', assistantText = '好的,已完成。', seq = 1 } = {}) {
  const events = [
    { type: 'message', seq, payload: { id: userId, role: 'user', content: '帮我看看' } },
    { type: 'busy_changed', seq: seq + 1, payload: { busy: true, turn_id: userId } },
  ];
  if (assistantText) {
    events.push({ type: 'token', seq: seq + 2, payload: { text: assistantText } });
    events.push({ type: 'message', seq: seq + 3, payload: { id: `a-${userId}`, role: 'assistant', content: assistantText } });
  }
  events.push({ type: 'busy_changed', seq: seq + 4, payload: { busy: false, outcome, turn_id: userId } });
  events.push({ type: 'done', seq: seq + 5, payload: { outcome, turn_id: userId } });
  return events;
}

run('回归:实时回合正常结束后补拉 turn_timing,预测消息的 completion key 随之可算', () => {
  // 触发场景:页面开着,用户发一条消息,回合正常结束(WS 只送达 user / assistant /
  // busy_changed / done,daemon 不推 turn_timing)。
  // bug 表现:turnTimings 里查不到这条 user,followUpCompletionKey 恒为空,
  // 前端一次预测请求都不发,只有刷新页面重新打开会话(历史里带 turn_timing)才出现灰字。
  // 期望行为:pendingTurnTimingUserId 指出要补拉的 user;补拉到的 REST 尾页并入后,
  // completion key 与服务端按 JSONL 算出的 `${user}:${completed_at_ms}` 一致。
  const ended = reduceMany(liveTurnEvents('u1'), liveLoadedState());
  assert.equal(ended.busy, false);
  assert.equal(ended.lastTurnOutcome, 'completed');
  assert.equal(followUpCompletionKey(ended), '', '实时事件里没有 turn_timing,补拉前算不出 key');
  assert.equal(pendingTurnTimingUserId(ended), 'u1');

  // limit 1 的尾页:服务端倒着收到 user 为止,这一页的 user / assistant 已经由 WS 送达。
  const page = [
    { id: 'u1', role: 'user', content: '帮我看看' },
    { id: 'a-u1', role: 'assistant', content: '好的,已完成。' },
    timingRecord('u1', 6000),
  ];
  const merged = mergeTurnTimingsFromMessages(ended, page);
  assert.equal(followUpCompletionKey(merged), 'u1:6000');
  assert.equal(pendingTurnTimingUserId(merged), '', '补到之后不再重复拉');
  // 只并 turn_timing,不把这一页的消息再追加一遍。
  assert.equal(merged.items.length, ended.items.length);
  const assistant = merged.items.find((item) => item.kind === 'msg' && item.role === 'assistant');
  assert.equal(assistant.turnDurationMs, 5000, '本回合条目拿到持久化的耗时,与重新打开会话时一致');
});

run('只有工具收尾、没有正文的回合同样要补拉', () => {
  // 触发场景:回合只以 task_complete 等工具结束,没有 assistant 正文。
  // 期望行为:busy_changed(false) 的 completed 照样写入 lastTurnOutcome,
  // 补拉判定不依赖 turn_completed 效果(那个效果要求本回合有正文)。
  const ended = reduceMany(liveTurnEvents('u-tool', { assistantText: '' }), liveLoadedState());
  assert.equal(ended.lastTurnOutcome, 'completed');
  assert.equal(pendingTurnTimingUserId(ended), 'u-tool');
});

run('中止、出错、仍在忙的回合不补拉', () => {
  // 期望行为:服务端只对 status=completed 的回合给预测,中止或出错时补拉也用不上;
  // 回合还在跑时更不能拉(此时 JSONL 里还没有完成记录)。
  const aborted = reduceMany(liveTurnEvents('u2', { outcome: 'aborted' }), liveLoadedState());
  assert.equal(pendingTurnTimingUserId(aborted), '');
  const failed = reduceMany(liveTurnEvents('u3', { outcome: 'error' }), liveLoadedState());
  assert.equal(pendingTurnTimingUserId(failed), '');
  const running = reduceMany(liveTurnEvents('u4').slice(0, 3), liveLoadedState());
  assert.equal(running.busy, true);
  assert.equal(pendingTurnTimingUserId(running), '');
});

run('打开会话时不额外请求:历史里已经带着 turn_timing', () => {
  // 触发场景:刷新或从侧栏打开会话,历史首屏里已有最后一回合的 turn_timing。
  // 期望行为:没有观察到实时收尾(lastTurnOutcome 为空),不发补拉请求;
  // completion key 直接由历史算出,原有的打开会话即请求预测的路径不变。
  const loaded = loadTranscriptHistory(createTranscriptState({ title: 's' }), {
    messages: [
      { id: 'u5', role: 'user', content: '你好' },
      { id: 'a5', role: 'assistant', content: '你好!' },
      timingRecord('u5', 3000),
    ],
  }).state;
  assert.equal(followUpCompletionKey(loaded), 'u5:3000');
  assert.equal(pendingTurnTimingUserId(loaded), '');
  // 即便实时收尾已记下 completed,turnTimings 已有这条 user 时也不拉。
  assert.equal(pendingTurnTimingUserId({ ...loaded, lastTurnOutcome: 'completed' }), '');
});

run('最后一条 user 还没拿到持久 id 时不补拉', () => {
  // 触发场景:排队输入刚被接受,乐观插入的 user 条目只有 client_message_id。
  // 期望行为:服务端按持久 uuid 记 turn_timing,没有 id 就对不上,不发请求。
  const ended = reduceMany(liveTurnEvents('u6'), liveLoadedState());
  const queued = reduceTranscriptEvent(ended, {
    type: 'queued_input_accepted', seq: 100, payload: { client_message_id: 'c-1', content: '下一条' },
  }).state;
  assert.equal(pendingTurnTimingUserId(queued), '');
});

run('尾页里没有新记录时原样返回 state,不触发重渲染', () => {
  // 触发场景:补拉回来的尾页只有已知的 turn_timing,或者根本没有(例如插话后的回合)。
  // 期望行为:返回同一个 state 引用,单写者 store 不广播无意义的更新。
  const ended = reduceMany(liveTurnEvents('u7'), liveLoadedState());
  assert.equal(mergeTurnTimingsFromMessages(ended, []), ended);
  assert.equal(mergeTurnTimingsFromMessages(ended, [{ id: 'u7', role: 'user', content: '帮我看看' }]), ended);
  const merged = mergeTurnTimingsFromMessages(ended, [timingRecord('u7', 4000)]);
  assert.notEqual(merged, ended);
  assert.equal(mergeTurnTimingsFromMessages(merged, [timingRecord('u7', 4000)]), merged);
});

console.log('turnTimingCatchup tests passed');
