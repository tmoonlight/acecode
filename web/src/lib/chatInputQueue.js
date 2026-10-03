import { normalizeComposerContent, composerContentAttachments } from './composerContent.js';

export const QUEUED_INPUT_STATE = Object.freeze({
  QUEUED: 'queued',
  SENDING: 'sending',
  FAILED: 'failed',
  GUIDING: 'guiding',
  COMPLETED: 'completed',
  CANCELLED: 'cancelled',
});

// 队列暂停原因。目前只有一种:用户(或其它客户端)中断了正在进行的回合。
// 中断意味着「停下来」,此时把排队消息自动发出去等于替用户做了决定,
// 所以队列改为暂停,等用户点「继续」或再次主动发送。
export const QUEUE_PAUSE_REASON = Object.freeze({
  INTERRUPTED: 'interrupted',
});

function normalizeSessionId(sessionId) {
  return String(sessionId || '');
}

function normalizeText(text) {
  return String(text ?? '');
}

function normalizePayload({ text, payload } = {}) {
  if (payload && typeof payload === 'object' && !Array.isArray(payload)) {
    const normalized = {
      text: normalizeText(payload.text),
      attachments: Array.isArray(payload.attachments) ? payload.attachments : [],
      contexts: Array.isArray(payload.contexts) ? payload.contexts : [],
    };
    // 会话级蜂群模式随这条消息写给服务端:'star' | 'mesh' | 'off';旧排队项的 true = 星型。
    if (payload.swarm_mode === true || ['star', 'mesh', 'off'].includes(payload.swarm_mode)) {
      normalized.swarm_mode = payload.swarm_mode;
    }
    const content = normalizeComposerContent(payload.composer_content);
    if (content) normalized.composer_content = content;
    if (Array.isArray(payload.session_references)) normalized.session_references = payload.session_references;
    return normalized;
  }
  return {
    text: normalizeText(text),
    attachments: [],
    contexts: [],
  };
}

function cloneItems(state) {
  return Array.isArray(state?.items) ? state.items : [];
}

// paused: { [sessionId]: { reason, pausedAt } },按会话记录暂停态。
function clonePaused(state) {
  const paused = state?.paused;
  return paused && typeof paused === 'object' && !Array.isArray(paused) ? paused : {};
}

function nextSequence(state) {
  const next = Number(state?.nextLocalId || 1);
  return Number.isFinite(next) && next > 0 ? next : 1;
}

function buildQueuedId(sessionId, sequence) {
  const sid = normalizeSessionId(sessionId).replace(/[^a-zA-Z0-9_-]+/g, '-');
  return `queued-${sid || 'session'}-${sequence}`;
}

export function createChatInputQueueState(overrides = {}) {
  return {
    nextLocalId: nextSequence(overrides),
    items: cloneItems(overrides),
    paused: clonePaused(overrides),
  };
}

export function enqueueQueuedInput(state, { sessionId, text, payload, now = Date.now() } = {}) {
  const sid = normalizeSessionId(sessionId);
  const queuedPayload = normalizePayload({ text, payload });
  const content = queuedPayload.text;
  const hasExtras = queuedPayload.attachments.length > 0 || queuedPayload.contexts.length > 0;
  if (!sid || (content.trim().length === 0 && !hasExtras)) return state || createChatInputQueueState();

  const current = createChatInputQueueState(state);
  const sequence = nextSequence(current);
  const id = buildQueuedId(sid, sequence);
  const item = {
    kind: 'msg',
    id,
    messageId: '',
    role: 'user',
    content,
    ...(queuedPayload.composer_content ? { composerContent: queuedPayload.composer_content } : {}),
    ts: now,
    queued: {
      id,
      sessionId: sid,
      state: QUEUED_INPUT_STATE.QUEUED,
      createdAt: now,
      updatedAt: now,
      error: '',
      payload: queuedPayload,
    },
  };

  return {
    ...current,
    nextLocalId: sequence + 1,
    items: [...current.items, item],
  };
}

function updateQueuedInput(state, id, updater) {
  const current = createChatInputQueueState(state);
  let changed = false;
  const items = current.items.map((item) => {
    if (item?.queued?.id !== id) return item;
    const nextItem = updater(item);
    if (nextItem !== item) changed = true;
    return nextItem;
  });
  if (changed) return { ...current, items };
  return state && typeof state === 'object' ? state : current;
}

function setQueuedInputState(state, id, nextState, extraQueued = {}) {
  const now = Date.now();
  return updateQueuedInput(state, id, (item) => ({
    ...item,
    queued: {
      ...item.queued,
      state: nextState,
      updatedAt: now,
      ...extraQueued,
    },
  }));
}

export function cancelQueuedInput(state, id) {
  const before = createChatInputQueueState(state);
  const cancelled = before.items.find((item) => item?.queued?.id === id);
  const next = setQueuedInputState(state, id, QUEUED_INPUT_STATE.CANCELLED);
  // 暂停态只对「还有待发送消息」有意义:最后一条被删掉后顺手清掉暂停标记,
  // 否则之后(比如别的客户端启动回合、用户又排了新消息)会被一个看不见的
  // 暂停态卡住,横幅却因为没有卡片而不显示。
  const sid = normalizeSessionId(cancelled?.queued?.sessionId);
  if (sid && next !== state && queuedInputsForSession(next, sid).length === 0) {
    return resumeQueuedInput(next, sid);
  }
  return next;
}

export function beginQueuedGuidance(
  state,
  id,
  { turnId = '', now = Date.now() } = {},
) {
  return updateQueuedInput(state, id, (item) => {
    const currentState = item?.queued?.state;
    if (currentState !== QUEUED_INPUT_STATE.QUEUED &&
        currentState !== QUEUED_INPUT_STATE.FAILED) return item;
    return {
      ...item,
      queued: {
        ...item.queued,
        state: QUEUED_INPUT_STATE.GUIDING,
        guidancePreviousState: currentState,
        steerTurnId: normalizeText(turnId),
        acceptedAt: 0,
        error: '',
        updatedAt: now,
      },
    };
  });
}

export function markQueuedGuidanceAccepted(
  state,
  id,
  { turnId = '', now = Date.now() } = {},
) {
  return updateQueuedInput(state, id, (item) => {
    if (item?.queued?.state !== QUEUED_INPUT_STATE.GUIDING) return item;
    return {
      ...item,
      queued: {
        ...item.queued,
        steerTurnId: normalizeText(turnId) || item.queued.steerTurnId || '',
        acceptedAt: now,
        updatedAt: now,
        error: '',
      },
    };
  });
}

export function finishQueuedGuidance(state, id, { succeeded = false } = {}) {
  if (succeeded) return markQueuedGuidanceAccepted(state, id);
  return updateQueuedInput(state, id, (item) => {
    if (item?.queued?.state !== QUEUED_INPUT_STATE.GUIDING) return item;
    const previous = item.queued.guidancePreviousState === QUEUED_INPUT_STATE.FAILED
      ? QUEUED_INPUT_STATE.FAILED
      : QUEUED_INPUT_STATE.QUEUED;
    const queued = { ...item.queued, state: previous, updatedAt: Date.now() };
    delete queued.guidancePreviousState;
    delete queued.steerTurnId;
    delete queued.acceptedAt;
    return { ...item, queued };
  });
}

export function markQueuedInputSending(state, id, { now = Date.now() } = {}) {
  return setQueuedInputState(state, id, QUEUED_INPUT_STATE.SENDING, {
    sentAt: now,
    updatedAt: now,
    error: '',
  });
}

export function markQueuedInputFailed(state, id, error = '') {
  return setQueuedInputState(state, id, QUEUED_INPUT_STATE.FAILED, {
    error: String(error || '发送失败'),
  });
}

export function markQueuedInputCompleted(state, id) {
  return setQueuedInputState(state, id, QUEUED_INPUT_STATE.COMPLETED, { error: '' });
}

export function queuedInputRequestPayload(item) {
  const clientMessageId = normalizeText(item?.queued?.id).trim();
  if (!clientMessageId) return null;
  const payload = normalizePayload({
    text: item?.content,
    payload: item?.queued?.payload,
  });
  return {
    ...payload,
    client_message_id: clientMessageId,
  };
}

export function acceptedQueuedInputEvent(item, { now = Date.now() } = {}) {
  const clientMessageId = normalizeText(item?.queued?.id).trim();
  if (!clientMessageId) return null;
  const payload = normalizePayload({
    text: item?.content,
    payload: item?.queued?.payload,
  });
  const content = payload.text || (payload.attachments.length > 0 ? '附件消息' : '上下文消息');
  return {
    type: 'queued_input_accepted',
    payload: {
      client_message_id: clientMessageId,
      content,
      ...(payload.composer_content ? { composer_content: payload.composer_content } : {}),
    },
    timestamp_ms: now,
  };
}

export function retryQueuedInput(state, id) {
  return setQueuedInputState(state, id, QUEUED_INPUT_STATE.QUEUED, { error: '' });
}

export function updateQueuedInputContent(state, id, text, { now = Date.now(), composerContent } = {}) {
  const nextText = normalizeText(text);
  return updateQueuedInput(state, id, (item) => {
    const currentState = item?.queued?.state;
    if (currentState !== QUEUED_INPUT_STATE.QUEUED &&
        currentState !== QUEUED_INPUT_STATE.FAILED) return item;
    const payload = normalizePayload({
      payload: {
        ...(item.queued.payload || {}),
        text: nextText,
        ...(composerContent !== undefined ? { composer_content: composerContent }
          : item.content !== nextText ? { composer_content: null } : {}),
      },
    });
    if (composerContent !== undefined && payload.composer_content) {
      payload.attachments = composerContentAttachments(payload.composer_content).filter((attachment) => attachment.id).map(({ id }) => ({ id }));
    }
    const hasExtras = payload.attachments.length > 0 || payload.contexts.length > 0;
    if (nextText.trim().length === 0 && !hasExtras) return item;
    if (composerContent === undefined && item.content === nextText && item.queued.payload?.text === nextText) return item;
    return {
      ...item,
      content: nextText,
      composerContent: payload.composer_content || null,
      queued: {
        ...item.queued,
        payload,
        updatedAt: now,
      },
    };
  });
}

export function queuedInputsForSession(state, sessionId, { includeDone = false } = {}) {
  const sid = normalizeSessionId(sessionId);
  const doneStates = new Set([
    QUEUED_INPUT_STATE.COMPLETED,
    QUEUED_INPUT_STATE.CANCELLED,
  ]);
  return cloneItems(state).filter((item) => {
    if (item?.queued?.sessionId !== sid) return false;
    if (includeDone) return true;
    return !doneStates.has(item.queued.state);
  });
}

export function hasSendingQueuedInput(state, sessionId) {
  return queuedInputsForSession(state, sessionId, { includeDone: true })
    .some((item) => item.queued?.state === QUEUED_INPUT_STATE.SENDING);
}

export function nextQueuedInput(state, sessionId) {
  const items = queuedInputsForSession(state, sessionId, { includeDone: true });
  if (items.some((item) => (
    item.queued?.state === QUEUED_INPUT_STATE.SENDING ||
    item.queued?.state === QUEUED_INPUT_STATE.GUIDING
  ))) return null;
  return items.find((item) => item.queued?.state === QUEUED_INPUT_STATE.QUEUED) || null;
}

export function completeQueuedInputForMessage(
  state,
  { sessionId, content, ts, clientMessageId } = {},
) {
  const sid = normalizeSessionId(sessionId);
  const text = normalizeText(content);
  const correlationId = normalizeText(clientMessageId).trim();
  const current = createChatInputQueueState(state);
  const matched = current.items.find((item) => {
    if (item?.queued?.sessionId !== sid) return false;
    const queueState = item.queued.state;
    if (queueState !== QUEUED_INPUT_STATE.SENDING &&
        queueState !== QUEUED_INPUT_STATE.GUIDING) return false;
    if (queueState === QUEUED_INPUT_STATE.GUIDING) {
      return !!correlationId && item.queued.id === correlationId;
    }
    if (correlationId) return item.queued.id === correlationId;
    if (normalizeText(item.content) !== text) return false;
    const sentAt = Number(item.queued.sentAt || 0);
    const messageTs = Number(ts || 0);
    return !messageTs || !sentAt || messageTs >= sentAt - 2000;
  });
  return matched ? markQueuedInputCompleted(current, matched.queued.id) : current;
}

export function buildQueuedMessageItems(state, sessionId) {
  return queuedInputsForSession(state, sessionId).map((item) => ({ ...item }));
}

// ---- 队列暂停 ----------------------------------------------------------
// 用户中断回合后队列进入暂停:不自动 drain,卡片栈顶部显示「队列已暂停 / 继续」。
// 解除方式只有用户的明确动作:点「继续」、在空输入框上按发送、再次发送 /
// 排队一条新消息、重试某条失败消息。

export function queuedInputPause(state, sessionId) {
  const sid = normalizeSessionId(sessionId);
  if (!sid) return null;
  const entry = clonePaused(state)[sid];
  return entry && typeof entry === 'object' ? entry : null;
}

export function isQueuedInputPaused(state, sessionId) {
  return queuedInputPause(state, sessionId) !== null;
}

export function pauseQueuedInput(
  state,
  sessionId,
  { reason = QUEUE_PAUSE_REASON.INTERRUPTED, now = Date.now() } = {},
) {
  const sid = normalizeSessionId(sessionId);
  const current = createChatInputQueueState(state);
  // 没有待发送消息就没有可暂停的东西;已暂停则保持首次暂停的时间与原因。
  if (!sid || queuedInputsForSession(current, sid).length === 0) {
    return state && typeof state === 'object' ? state : current;
  }
  if (current.paused[sid]) return state && typeof state === 'object' ? state : current;
  return {
    ...current,
    paused: { ...current.paused, [sid]: { reason: normalizeText(reason) || QUEUE_PAUSE_REASON.INTERRUPTED, pausedAt: now } },
  };
}

export function resumeQueuedInput(state, sessionId) {
  const sid = normalizeSessionId(sessionId);
  const current = createChatInputQueueState(state);
  if (!sid || !current.paused[sid]) return state && typeof state === 'object' ? state : current;
  const paused = { ...current.paused };
  delete paused[sid];
  return { ...current, paused };
}

// 回合以「中断」收尾时队列是否应该转入暂停。
// 回归(bug 表现):用户排了一堆消息后点停止,busy 一翻 false 自动 drain 就把
// 下一条排队消息发了出去 —— 用户刚说「停」,界面却替他继续。
// lastTurnOutcome 来自 transcript reducer:本端点停止(turn_aborted)与远端中断
// (busy_changed / done 携带 outcome=aborted)都会置成 'aborted'。
export function shouldPauseQueuedInputAfterAbort({
  state,
  sessionId = '',
  lastTurnOutcome = '',
} = {}) {
  const sid = normalizeSessionId(sessionId);
  if (!sid || lastTurnOutcome !== 'aborted') return false;
  if (isQueuedInputPaused(state, sid)) return false;
  return queuedInputsForSession(state, sid).length > 0;
}

// 是否允许自动 drain 排队消息。
// 切会话时 useSessionTranscript 会先把 busy 置 false、loadState=loading,
// 若此时 drain 会把仍在等待的排队卡片立刻发出/消掉。必须等 transcript
// 真正 loaded 后再根据 busy 决定是否 drain。
// paused:用户中断回合后的暂停态,同样禁止自动 drain。
export function shouldDrainQueuedInput({
  sessionId = '',
  busy = false,
  loadState = 'loaded',
  paused = false,
} = {}) {
  if (!String(sessionId || '').trim()) return false;
  if (busy) return false;
  if (loadState && loadState !== 'loaded') return false;
  if (paused) return false;
  return true;
}
