import assert from 'node:assert/strict';
import {
  QUEUED_INPUT_STATE,
  QUEUE_PAUSE_REASON,
  acceptedQueuedInputEvent,
  beginQueuedGuidance,
  buildQueuedMessageItems,
  cancelQueuedInput,
  completeQueuedInputForMessage,
  createChatInputQueueState,
  enqueueQueuedInput,
  finishQueuedGuidance,
  isQueuedInputPaused,
  markQueuedGuidanceAccepted,
  markQueuedInputFailed,
  markQueuedInputSending,
  nextQueuedInput,
  pauseQueuedInput,
  queuedInputPause,
  queuedInputRequestPayload,
  queuedInputsForSession,
  resumeQueuedInput,
  retryQueuedInput,
  shouldDrainQueuedInput,
  shouldPauseQueuedInputAfterAbort,
  updateQueuedInputContent,
} from './chatInputQueue.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('enqueue 创建稳定本地消息并按 session 隔离', () => {
  let state = createChatInputQueueState();
  state = enqueueQueuedInput(state, { sessionId: 's1', text: 'first', now: 100 });
  state = enqueueQueuedInput(state, { sessionId: 's2', text: 'second', now: 101 });

  const s1 = queuedInputsForSession(state, 's1');
  const s2 = queuedInputsForSession(state, 's2');
  assert.equal(s1.length, 1);
  assert.equal(s2.length, 1);
  assert.equal(s1[0].kind, 'msg');
  assert.equal(s1[0].role, 'user');
  assert.equal(s1[0].content, 'first');
  assert.equal(s1[0].queued.state, QUEUED_INPUT_STATE.QUEUED);
  assert.match(s1[0].queued.id, /^queued-s1-1$/);
  assert.match(s2[0].queued.id, /^queued-s2-2$/);
});

run('nextQueuedInput 保持 FIFO 且发送中时不取下一条', () => {
  let state = createChatInputQueueState();
  state = enqueueQueuedInput(state, { sessionId: 's1', text: 'one', now: 100 });
  state = enqueueQueuedInput(state, { sessionId: 's1', text: 'two', now: 101 });
  const first = nextQueuedInput(state, 's1');
  assert.equal(first.content, 'one');

  state = markQueuedInputSending(state, first.queued.id, { now: 200 });
  assert.equal(nextQueuedInput(state, 's1'), null);
});

run('插话中项目同步暂停 FIFO，HTTP 接受后等待消息事件，失败恢复原状态', () => {
  let state = createChatInputQueueState();
  state = enqueueQueuedInput(state, { sessionId: 's1', text: 'side question', now: 100 });
  state = enqueueQueuedInput(state, { sessionId: 's1', text: 'later question', now: 101 });
  const id = nextQueuedInput(state, 's1').queued.id;

  state = beginQueuedGuidance(state, id, { turnId: 'turn-1', now: 150 });
  assert.equal(queuedInputsForSession(state, 's1')[0].queued.state, QUEUED_INPUT_STATE.GUIDING);
  assert.equal(queuedInputsForSession(state, 's1')[0].queued.steerTurnId, 'turn-1');
  assert.equal(nextQueuedInput(state, 's1'), null);

  state = finishQueuedGuidance(state, id, { succeeded: false });
  assert.equal(queuedInputsForSession(state, 's1')[0].queued.state, QUEUED_INPUT_STATE.QUEUED);
  assert.equal(nextQueuedInput(state, 's1').queued.id, id);

  state = beginQueuedGuidance(state, id, { turnId: 'turn-1', now: 200 });
  const duplicateBegin = beginQueuedGuidance(state, id, { turnId: 'turn-1', now: 201 });
  assert.equal(duplicateBegin, state);
  state = markQueuedGuidanceAccepted(state, id, { turnId: 'turn-1', now: 220 });
  assert.equal(queuedInputsForSession(state, 's1').length, 2);
  assert.equal(queuedInputsForSession(state, 's1')[0].queued.acceptedAt, 220);
  assert.equal(nextQueuedInput(state, 's1'), null);
  state = completeQueuedInputForMessage(state, {
    sessionId: 's1',
    content: 'expanded guidance',
    clientMessageId: id,
    ts: 240,
  });
  assert.equal(queuedInputsForSession(state, 's1').length, 1);
  assert.equal(nextQueuedInput(state, 's1').content, 'later question');
});

run('引导只能按 client id 完成，不按相同文本误配', () => {
  let state = createChatInputQueueState();
  state = enqueueQueuedInput(state, { sessionId: 's1', text: 'same', now: 100 });
  const id = nextQueuedInput(state, 's1').queued.id;
  state = beginQueuedGuidance(state, id, { turnId: 'turn-1' });
  const unchanged = completeQueuedInputForMessage(state, {
    sessionId: 's1',
    content: 'same',
    ts: 200,
  });
  assert.equal(
    queuedInputsForSession(unchanged, 's1')[0].queued.state,
    QUEUED_INPUT_STATE.GUIDING,
  );
});

run('附件 payload 可以在空文本时排队并保留发送体', () => {
  let state = createChatInputQueueState();
  state = enqueueQueuedInput(state, {
    sessionId: 's1',
    payload: { text: '', attachments: [{ id: 'att_1' }], contexts: [] },
    now: 100,
  });
  const first = nextQueuedInput(state, 's1');
  assert.equal(first.content, '');
  assert.deepEqual(first.queued.payload.attachments, [{ id: 'att_1' }]);
});

run('排队提交复用稳定 id 作为请求关联键和接受后投影', () => {
  let state = createChatInputQueueState();
  state = enqueueQueuedInput(state, {
    sessionId: 's1',
    payload: { text: '', attachments: [{ id: 'att_1' }], contexts: [] },
    now: 100,
  });
  const first = nextQueuedInput(state, 's1');

  assert.deepEqual(queuedInputRequestPayload(first), {
    text: '',
    attachments: [{ id: 'att_1' }],
    contexts: [],
    client_message_id: first.queued.id,
  });
  assert.deepEqual(acceptedQueuedInputEvent(first, { now: 250 }), {
    type: 'queued_input_accepted',
    payload: {
      client_message_id: first.queued.id,
      content: '附件消息',
    },
    timestamp_ms: 250,
  });
});

run('蜂群模式在排队、重试和请求重建时保持，普通消息不产生该字段', () => {
  let state = createChatInputQueueState();
  state = enqueueQueuedInput(state, {
    sessionId: 's1',
    payload: {
      text: '并行检查实现和测试',
      attachments: [],
      contexts: [],
      swarm_mode: true,
    },
    now: 100,
  });
  const swarm = nextQueuedInput(state, 's1');
  assert.equal(swarm.queued.payload.swarm_mode, true);
  assert.equal(queuedInputRequestPayload(swarm).swarm_mode, true);

  state = markQueuedInputSending(state, swarm.queued.id, { now: 150 });
  state = markQueuedInputFailed(state, swarm.queued.id, 'network');
  state = retryQueuedInput(state, swarm.queued.id);
  const retried = nextQueuedInput(state, 's1');
  assert.equal(retried.queued.payload.swarm_mode, true);
  assert.equal(queuedInputRequestPayload(retried).swarm_mode, true);

  state = enqueueQueuedInput(state, {
    sessionId: 's2',
    payload: { text: '普通消息', attachments: [], contexts: [] },
    now: 200,
  });
  const ordinary = nextQueuedInput(state, 's2');
  assert.equal(Object.hasOwn(ordinary.queued.payload, 'swarm_mode'), false);
  assert.equal(Object.hasOwn(queuedInputRequestPayload(ordinary), 'swarm_mode'), false);
});

// 场景:蜂群模式升级为会话级的 off | star | mesh(add-mesh-swarm-mode),排队消息携带
// 要写给服务端的模式;关闭也是一次显式切换。
// 期望:三个字符串原样保留到请求里;无法识别的值丢弃,不把垃圾值发给服务端。
run('排队消息保留字符串形式的蜂群模式,未知值丢弃', () => {
  let state = createChatInputQueueState();
  for (const [index, mode] of ['mesh', 'off', 'grid'].entries()) {
    state = enqueueQueuedInput(state, {
      sessionId: `m${index}`,
      payload: { text: `模式 ${mode}`, attachments: [], contexts: [], swarm_mode: mode },
      now: 100 + index,
    });
  }
  assert.equal(queuedInputRequestPayload(nextQueuedInput(state, 'm0')).swarm_mode, 'mesh');
  assert.equal(queuedInputRequestPayload(nextQueuedInput(state, 'm1')).swarm_mode, 'off');
  assert.equal(Object.hasOwn(queuedInputRequestPayload(nextQueuedInput(state, 'm2')), 'swarm_mode'), false);
});

run('cancelled 项不会出现在可见队列也不会被发送', () => {
  let state = createChatInputQueueState();
  state = enqueueQueuedInput(state, { sessionId: 's1', text: 'one' });
  const first = nextQueuedInput(state, 's1');
  state = cancelQueuedInput(state, first.queued.id);

  assert.equal(queuedInputsForSession(state, 's1').length, 0);
  assert.equal(nextQueuedInput(state, 's1'), null);
});

run('编辑排队项会更新可见文本和发送 payload，发送中不可改', () => {
  let state = createChatInputQueueState();
  state = enqueueQueuedInput(state, { sessionId: 's1', text: 'plain', now: 90 });
  const textOnlyId = nextQueuedInput(state, 's1').queued.id;
  const emptyTextOnly = updateQueuedInputContent(state, textOnlyId, '   ', { now: 95 });
  assert.equal(emptyTextOnly, state);

  state = enqueueQueuedInput(state, {
    sessionId: 's1',
    payload: {
      text: 'old',
      attachments: [{ id: 'att_1' }],
      contexts: [],
      swarm_mode: true,
    },
    now: 100,
  });
  const id = queuedInputsForSession(state, 's1')[1].queued.id;

  state = updateQueuedInputContent(state, id, 'revised question', { now: 150 });
  const edited = queuedInputsForSession(state, 's1')[1];
  assert.equal(edited.content, 'revised question');
  assert.equal(edited.queued.payload.text, 'revised question');
  assert.deepEqual(edited.queued.payload.attachments, [{ id: 'att_1' }]);
  assert.equal(edited.queued.payload.swarm_mode, true);
  assert.equal(queuedInputRequestPayload(edited).text, 'revised question');

  state = updateQueuedInputContent(state, id, '   ', { now: 160 });
  assert.equal(queuedInputsForSession(state, 's1')[1].content, '   ');

  state = markQueuedInputSending(state, id, { now: 200 });
  const sendingUnchanged = updateQueuedInputContent(state, id, 'too late', { now: 210 });
  assert.equal(sendingUnchanged, state);
});

run('failed 项保留可见状态并可重试', () => {
  let state = createChatInputQueueState();
  state = enqueueQueuedInput(state, { sessionId: 's1', text: 'one' });
  const first = nextQueuedInput(state, 's1');
  state = markQueuedInputSending(state, first.queued.id, { now: 200 });
  state = markQueuedInputFailed(state, first.queued.id, 'network');

  let visible = buildQueuedMessageItems(state, 's1');
  assert.equal(visible.length, 1);
  assert.equal(visible[0].queued.state, QUEUED_INPUT_STATE.FAILED);
  assert.equal(visible[0].queued.error, 'network');
  assert.equal(nextQueuedInput(state, 's1'), null);

  state = retryQueuedInput(state, first.queued.id);
  assert.equal(nextQueuedInput(state, 's1').content, 'one');
});

run('backend user message 到达后完成对应 sending 占位', () => {
  let state = createChatInputQueueState();
  state = enqueueQueuedInput(state, { sessionId: 's1', text: 'same text', now: 100 });
  const first = nextQueuedInput(state, 's1');
  state = markQueuedInputSending(state, first.queued.id, { now: 200 });
  state = completeQueuedInputForMessage(state, { sessionId: 's1', content: 'same text', ts: 250 });

  assert.equal(queuedInputsForSession(state, 's1').length, 0);
  assert.equal(queuedInputsForSession(state, 's1', { includeDone: true })[0].queued.state, QUEUED_INPUT_STATE.COMPLETED);
});

run('backend user message 优先按 client id 完成而不是误配相同文本', () => {
  let state = createChatInputQueueState();
  state = enqueueQueuedInput(state, { sessionId: 's1', text: 'same text', now: 100 });
  state = enqueueQueuedInput(state, { sessionId: 's1', text: 'same text', now: 101 });
  const [first, second] = queuedInputsForSession(state, 's1', { includeDone: true });
  state = markQueuedInputSending(state, first.queued.id, { now: 200 });

  const unchanged = completeQueuedInputForMessage(state, {
    sessionId: 's1',
    content: 'same text',
    ts: 250,
    clientMessageId: second.queued.id,
  });
  assert.equal(
    queuedInputsForSession(unchanged, 's1', { includeDone: true })[0].queued.state,
    QUEUED_INPUT_STATE.SENDING,
  );

  state = completeQueuedInputForMessage(state, {
    sessionId: 's1',
    content: 'expanded text can differ',
    ts: 250,
    clientMessageId: first.queued.id,
  });
  assert.equal(
    queuedInputsForSession(state, 's1', { includeDone: true })[0].queued.state,
    QUEUED_INPUT_STATE.COMPLETED,
  );
});

run('shouldDrainQueuedInput: 切会话 loading 期间禁止 drain', () => {
  assert.equal(shouldDrainQueuedInput({
    sessionId: 's1',
    busy: false,
    loadState: 'loading',
  }), false);
  assert.equal(shouldDrainQueuedInput({
    sessionId: 's1',
    busy: false,
    loadState: 'idle',
  }), false);
  assert.equal(shouldDrainQueuedInput({
    sessionId: 's1',
    busy: false,
    loadState: 'error',
  }), false);
});

run('shouldDrainQueuedInput: loaded 且非 busy 才允许 drain', () => {
  assert.equal(shouldDrainQueuedInput({
    sessionId: 's1',
    busy: false,
    loadState: 'loaded',
  }), true);
  assert.equal(shouldDrainQueuedInput({
    sessionId: 's1',
    busy: true,
    loadState: 'loaded',
  }), false);
  assert.equal(shouldDrainQueuedInput({
    sessionId: '',
    busy: false,
    loadState: 'loaded',
  }), false);
});

// ---- 队列暂停(用户中断回合) ------------------------------------------------
// 回归背景:用户排了一堆消息后点停止,busy 一翻 false 自动 drain 就把下一条排队
// 消息发了出去 —— 用户刚说「停」,界面却替他继续。修复后中断收尾 → 队列暂停,
// 只有用户明确操作(继续 / 再次发送 / 重试)才恢复。

// 触发场景:会话 s1 排了两条消息,回合被中断。
// 期望行为:pauseQueuedInput 记下 {reason, pausedAt};isQueuedInputPaused 为 true;
// shouldDrainQueuedInput 带 paused 时返回 false;resume 后恢复 drain 并保留全部卡片。
run('pauseQueuedInput / resumeQueuedInput:暂停期间禁止 drain,恢复后卡片仍在', () => {
  let state = createChatInputQueueState();
  state = enqueueQueuedInput(state, { sessionId: 's1', text: '第一条', now: 100 });
  state = enqueueQueuedInput(state, { sessionId: 's1', text: '第二条', now: 101 });
  assert.equal(isQueuedInputPaused(state, 's1'), false);

  state = pauseQueuedInput(state, 's1', { now: 200 });
  assert.equal(isQueuedInputPaused(state, 's1'), true);
  assert.deepEqual(queuedInputPause(state, 's1'), {
    reason: QUEUE_PAUSE_REASON.INTERRUPTED,
    pausedAt: 200,
  });
  assert.equal(shouldDrainQueuedInput({
    sessionId: 's1', busy: false, loadState: 'loaded', paused: isQueuedInputPaused(state, 's1'),
  }), false, '暂停期间即使 idle + loaded 也不能自动出队');
  // 暂停不动卡片本身:两条仍是 QUEUED,nextQueuedInput 仍能取到第一条(由调用方决定不取)
  assert.deepEqual(queuedInputsForSession(state, 's1').map((item) => item.content), ['第一条', '第二条']);
  assert.equal(nextQueuedInput(state, 's1')?.content, '第一条');

  const resumed = resumeQueuedInput(state, 's1');
  assert.equal(isQueuedInputPaused(resumed, 's1'), false);
  assert.equal(queuedInputPause(resumed, 's1'), null);
  assert.equal(shouldDrainQueuedInput({
    sessionId: 's1', busy: false, loadState: 'loaded', paused: isQueuedInputPaused(resumed, 's1'),
  }), true);
  assert.deepEqual(queuedInputsForSession(resumed, 's1').map((item) => item.content), ['第一条', '第二条']);
});

// 触发场景:没有待发送消息的会话调用 pause;已暂停的会话再次 pause;
// 别的会话(s2)有排队消息但没被暂停。
// 期望行为:无卡片 → 返回原对象(store 不会触发多余渲染);重复 pause 保留首次的
// pausedAt;暂停按会话隔离,s2 不受影响。
run('pauseQueuedInput:无排队消息时是 no-op,重复暂停保留首次时间,按会话隔离', () => {
  const empty = createChatInputQueueState();
  assert.equal(pauseQueuedInput(empty, 's1'), empty, '没有卡片就没有可暂停的东西');
  assert.equal(resumeQueuedInput(empty, 's1'), empty, '本来就没暂停,resume 也是 no-op');

  let state = enqueueQueuedInput(empty, { sessionId: 's1', text: 'a', now: 1 });
  state = enqueueQueuedInput(state, { sessionId: 's2', text: 'b', now: 2 });
  const paused = pauseQueuedInput(state, 's1', { now: 10 });
  const pausedAgain = pauseQueuedInput(paused, 's1', { now: 20 });
  assert.equal(pausedAgain, paused, '重复暂停返回原对象');
  assert.equal(queuedInputPause(pausedAgain, 's1').pausedAt, 10);
  assert.equal(isQueuedInputPaused(pausedAgain, 's2'), false, '暂停只作用于中断的那个会话');
  assert.equal(shouldDrainQueuedInput({ sessionId: 's2', busy: false, loadState: 'loaded', paused: false }), true);
});

// 触发场景:队列暂停期间用户把卡片逐条删除。
// 期望行为:删到最后一条时暂停标记一并清除 —— 否则横幅因为没有卡片而消失,
// 一个看不见的暂停态却还在,之后(别的客户端启动回合、用户再排新消息)会被它卡住。
run('cancelQueuedInput:删掉最后一条待发送消息时顺带解除暂停', () => {
  let state = createChatInputQueueState();
  state = enqueueQueuedInput(state, { sessionId: 's1', text: 'a', now: 1 });
  state = enqueueQueuedInput(state, { sessionId: 's1', text: 'b', now: 2 });
  state = pauseQueuedInput(state, 's1', { now: 3 });
  const [first, second] = queuedInputsForSession(state, 's1');

  state = cancelQueuedInput(state, first.queued.id);
  assert.equal(isQueuedInputPaused(state, 's1'), true, '还剩一条,暂停继续生效');
  state = cancelQueuedInput(state, second.queued.id);
  assert.equal(queuedInputsForSession(state, 's1').length, 0);
  assert.equal(isQueuedInputPaused(state, 's1'), false, '最后一条删掉后暂停标记清除');
});

// 触发场景:ChatView 的 drain effect 在 busy→false 那一帧拿到 transcript 的
// lastTurnOutcome 与队列状态,判断这次收尾要不要转入暂停。
// 期望行为:只有「中断收尾 + 有待发送消息 + 尚未暂停」三者同时成立才返回 true;
// 正常完成(completed)/ 出错(error)/ 没有卡片 / 已经暂停都返回 false。
// 回归:修复前根本没有这条判断,中断收尾与正常收尾一样直接 drain。
run('shouldPauseQueuedInputAfterAbort:只有中断收尾且有待发送消息才暂停', () => {
  let state = createChatInputQueueState();
  assert.equal(shouldPauseQueuedInputAfterAbort({ state, sessionId: 's1', lastTurnOutcome: 'aborted' }), false, '没有卡片');

  state = enqueueQueuedInput(state, { sessionId: 's1', text: 'a', now: 1 });
  assert.equal(shouldPauseQueuedInputAfterAbort({ state, sessionId: 's1', lastTurnOutcome: 'aborted' }), true);
  assert.equal(shouldPauseQueuedInputAfterAbort({ state, sessionId: 's1', lastTurnOutcome: 'completed' }), false, '正常完成照常 drain');
  assert.equal(shouldPauseQueuedInputAfterAbort({ state, sessionId: 's1', lastTurnOutcome: 'error' }), false, '出错收尾沿用旧行为');
  assert.equal(shouldPauseQueuedInputAfterAbort({ state, sessionId: 's1', lastTurnOutcome: '' }), false);
  assert.equal(shouldPauseQueuedInputAfterAbort({ state, sessionId: '', lastTurnOutcome: 'aborted' }), false, '无会话');
  assert.equal(shouldPauseQueuedInputAfterAbort({ state, sessionId: 's2', lastTurnOutcome: 'aborted' }), false, '别的会话没有卡片');

  const paused = pauseQueuedInput(state, 's1');
  assert.equal(shouldPauseQueuedInputAfterAbort({ state: paused, sessionId: 's1', lastTurnOutcome: 'aborted' }), false, '已暂停不重复暂停');
});

// 触发场景:FAILED 卡片在暂停期间被点「重试」(ChatView 会顺带 resume);
// 这里只验证纯函数层:retry 不改暂停标记,由调用方决定是否 resume。
run('retryQueuedInput 不隐式改动暂停标记', () => {
  let state = createChatInputQueueState();
  state = enqueueQueuedInput(state, { sessionId: 's1', text: 'a', now: 1 });
  const id = queuedInputsForSession(state, 's1')[0].queued.id;
  state = markQueuedInputFailed(state, id, 'boom');
  state = pauseQueuedInput(state, 's1');
  const retried = retryQueuedInput(state, id);
  assert.equal(retried.items[0].queued.state, QUEUED_INPUT_STATE.QUEUED);
  assert.equal(isQueuedInputPaused(retried, 's1'), true);
});
