import { sideChatRetentionMilliseconds } from './sideChatRetention.js';
import { composerContentAttachments, composerContentFromText, normalizeComposerContent } from './composerContent.js';
import { normalizeComposerPayload } from './composerSubmission.js';
import { mergeComposerAttachmentResources } from './composerDraft.js';
import { createSideChatDraft } from './sideChatDraft.js';
import { withPasteBlocksFrom } from './pastedText.js';

export const SIDE_CHAT_QUESTION_MAX_BYTES = 16000;
export const SIDE_CHAT_HISTORY_MAX_BYTES = 256 * 1024;
export const SIDE_CHAT_HISTORY_MAX_MESSAGES = 200;

const utf8 = new TextEncoder();
let requestSequence = 0;

function initialSnapshot() {
  return { open: false, turns: [], draft: '', busy: false, stopping: false, queued: [], queuePaused: false,
    composerContent: null, attachments: [], inputHistory: [], modelName: '', reasoningEffort: null };
}

function historyFromTurns(turns) {
  return turns.flatMap((turn) => (
    (turn.status === 'success' || turn.status === 'stopped') && turn.answer.trim()
      ? [{ role: 'user', content: turn.question, ...(turn.input ? { input: turn.input } : {}) }, { role: 'assistant', content: turn.answer }]
      : []
  ));
}

// Read-only tools the daemon lets a side answer call, with their row verbs.
const SIDE_CHAT_TOOL_VERBS = {
  file_read: '读取',
  grep: '搜索',
  glob: '查找文件',
  lsp: '查询代码',
  send_to_main_session: '发到主会话',
};

// 「发到主会话」卡片的状态:pending 可编辑,sending 等结果,其余是最终结果。
export const SIDE_CHAT_SEND_FINAL_STATUSES = new Set(['steered', 'started', 'queued', 'cancelled', 'failed']);

// 用户在侧边回答上点「发到主会话」时的预填内容(对齐 Claude 侧边对话的 Branch 预填)。
// 主会话看不到侧边对话,所以把问与答都带上。
export function sideChatHandoffText(turn) {
  const question = String(turn?.question || '').trim();
  const answer = String(turn?.answer || '').trim();
  return ['接续侧边对话中的问题。', `问:${question}`, `答:“${answer}”`].join(PARAGRAPH_BREAK);
}

const LINE_BREAK = String.fromCharCode(10);
const PARAGRAPH_BREAK = LINE_BREAK + LINE_BREAK;

// 主会话里选中的文字以引用块追加到侧边输入框末尾,光标留在下一段。
export function sideChatQuoteDraft(draft, text) {
  const source = String(text || '').trim();
  if (!source) return String(draft || '');
  const quote = source.split(LINE_BREAK)
    .map((line) => `> ${line.replace(/\r$/, '')}`.trimEnd())
    .join(LINE_BREAK);
  const base = String(draft || '').trimEnd();
  return `${base ? base + PARAGRAPH_BREAK : ''}${quote}${PARAGRAPH_BREAK}`;
}

export function sideChatToolVerb(name) {
  return SIDE_CHAT_TOOL_VERBS[name] || '调用工具';
}

// Splits an answer at the positions where tool calls happened, so tool rows
// render between the text written before and after them. Whitespace-only
// slices (the blank line between steps) are dropped.
export function sideChatTurnParts(turn) {
  const answer = String(turn?.answer || '');
  const tools = Array.isArray(turn?.tools) ? turn.tools : [];
  const parts = [];
  let cursor = 0;
  const pushText = (end) => {
    const text = answer.slice(cursor, end);
    if (text.trim()) parts.push({ kind: 'text', key: `text-${cursor}`, text });
    cursor = end;
  };
  for (const tool of tools) {
    const offset = Math.min(Math.max(Number(tool.offset) || 0, cursor), answer.length);
    if (offset > cursor) pushText(offset);
    const last = parts[parts.length - 1];
    if (last?.kind === 'tools') last.tools.push(tool);
    else parts.push({ kind: 'tools', key: `tools-${tool.id}`, tools: [tool] });
  }
  if (cursor < answer.length) pushText(answer.length);
  return parts;
}

function validationError(question, history, rich = false) {
  if (utf8.encode(question).length > (rich ? SIDE_CHAT_HISTORY_MAX_BYTES : SIDE_CHAT_QUESTION_MAX_BYTES)) {
    return { code: 'SIDE_CHAT_QUESTION_TOO_LONG', message: '问题过长，请缩短后重试。' };
  }
  if (history.length > SIDE_CHAT_HISTORY_MAX_MESSAGES
      || history.reduce((bytes, message) => bytes + utf8.encode(message.content).length, 0) > SIDE_CHAT_HISTORY_MAX_BYTES) {
    return { code: 'SIDE_CHAT_HISTORY_TOO_LONG', message: '旁路聊天记录过长，无法继续发送。' };
  }
  return null;
}

function createTurn(question, error) {
  return {
    id: `side-${Date.now().toString(36)}-${++requestSequence}`,
    question,
    answer: '',
    tools: [],
    sends: [],
    stepStart: 0,
    status: error ? 'error' : 'loading',
    error: error?.message || '',
    errorCode: error?.code || '',
  };
}

// State ownership stays outside React so submitting, stopping and session
// changes cannot race stale render closures or late transport callbacks.
export function createSideChatController({ startStream, sendToMain, uploadAttachment, now = Date.now } = {}) {
  if (typeof startStream !== 'function') throw new TypeError('startStream is required');
  const listeners = new Set();
  let snapshot = initialSnapshot();
  let active = null;
  let lastFinishedAt = null;
  let userSendSequence = 0;
  let queueScheduled = false;
  const draft = createSideChatDraft({ read: () => snapshot, publish,
    uploadAttachment: file => uploadAttachment(file) });

  function scheduleQueue() {
    if (queueScheduled) return;
    queueScheduled = true;
    queueMicrotask(() => {
      queueScheduled = false;
      if (!snapshot.open || active || snapshot.busy || snapshot.queuePaused) return;
      const next = snapshot.queued[0];
      if (next && !controller.submit(next.text, { ...next, queuedId: next.id })) publish({ queuePaused: true });
    });
  }

  function pauseQueue() {
    if (snapshot.queued.length) publish({ queuePaused: true });
  }

  function findSend(sendId) {
    for (const turn of snapshot.turns) {
      const send = (turn.sends || []).find((item) => item.id === sendId);
      if (send) return { turn, send };
    }
    return null;
  }

  function updateSend(sendId, patch) {
    publish({
      turns: snapshot.turns.map((turn) => ((turn.sends || []).some((item) => item.id === sendId)
        ? { ...turn, sends: turn.sends.map((item) => (item.id === sendId ? { ...item, ...patch } : item)) }
        : turn)),
    });
  }

  // 请求结束时,模型发起、还没作答的卡片随请求作废(后端已不再等待)。
  function settledSends(request) {
    const current = snapshot.turns.find((turn) => turn.id === request.id);
    return (current?.sends || []).map((item) => (
      item.origin === 'model' && (item.status === 'pending' || item.status === 'sending')
        ? { ...item, status: 'cancelled' }
        : item
    ));
  }

  function publish(patch) {
    snapshot = { ...snapshot, ...patch };
    for (const listener of listeners) listener();
  }

  function updateTurn(request, patch, statePatch = {}) {
    publish({
      ...statePatch,
      turns: snapshot.turns.map((turn) => turn.id === request.id ? { ...turn, ...patch } : turn),
    });
  }

  function settledTools(request) {
    const current = snapshot.turns.find((turn) => turn.id === request.id);
    return (current?.tools || []).map((tool) => (
      tool.status === 'running' ? { ...tool, status: 'cancelled' } : tool
    ));
  }

  function finish(request, patch) {
    if (active !== request) return;
    const pause = patch.status === 'error'
      || ((snapshot.stopping || patch.status === 'stopped') && !request.resumeQueueAfterStop);
    active = null;
    lastFinishedAt = now();
    request.handle?.dispose();
    updateTurn(request, { ...patch, tools: settledTools(request), sends: settledSends(request) },
      { busy: false, stopping: false, queuePaused: snapshot.queuePaused || (pause && snapshot.queued.length > 0) });
    scheduleQueue();
  }

  function cancelImmediately() {
    if (!active) return;
    const request = active;
    // Invalidate first: even a synchronous stop acknowledgement is stale.
    active = null;
    lastFinishedAt = now();
    try { request.handle?.stop(); }
    finally { request.handle?.dispose(); }
    updateTurn(request, { status: 'stopped', tools: settledTools(request), sends: settledSends(request) },
      { busy: false, stopping: false });
  }

  const controller = {
    subscribe(listener) {
      listeners.add(listener);
      return () => listeners.delete(listener);
    },
    getSnapshot() { return snapshot; },
    open() { publish({ open: true }); },
    setDraft(text, content) {
      const next = content === undefined ? (snapshot.composerContent ? withPasteBlocksFrom(
        composerContentFromText(text, composerContentAttachments(snapshot.composerContent, snapshot.attachments)), snapshot.composerContent,
      ) : null)
        : normalizeComposerContent(content);
      publish({ draft: String(text ?? ''), composerContent: next,
        attachments: next ? mergeComposerAttachmentResources(snapshot.attachments, composerContentAttachments(next, snapshot.attachments)) : snapshot.attachments });
      return true;
    },
    addFiles: draft.addFiles,
    removeAttachment: draft.remove,
    retryUpload: draft.retry,
    paste: draft.paste,
    setModel(modelName, reasoningEffort = null) { publish({ modelName, reasoningEffort }); },
    submitDraft() {
      const attachments = snapshot.composerContent ? composerContentAttachments(snapshot.composerContent, snapshot.attachments) : snapshot.attachments;
      if (attachments.some(item => item.uploading || item.upload_error || !item.id)) return false;
      const rich = !!snapshot.composerContent || attachments.length > 0;
      const input = rich ? normalizeComposerPayload(snapshot.draft, attachments, [], null, snapshot.composerContent) : undefined;
      const text = (input?.text ?? snapshot.draft).trim();
      const options = { ...(input ? { input, attachments } : {}),
        modelName: snapshot.modelName, reasoningEffort: snapshot.reasoningEffort };
      if (!text && !input?.attachments?.length) {
        if (!snapshot.busy && snapshot.queuePaused) return controller.resumeQueue();
        return false;
      }
      if (snapshot.busy) {
        const error = validationError(text, [], rich);
        if (error) {
          publish({ turns: [...snapshot.turns, createTurn(text, error)] });
          return false;
        }
        // An explicit new submission resumes the queue, including when it must
        // wait for an in-flight stop acknowledgement (same as the main composer).
        if (snapshot.stopping && active) active.resumeQueueAfterStop = true;
        publish({
          queued: [...snapshot.queued, { id: `queued-side-${++requestSequence}`, text, ...options }],
          draft: '',
          composerContent: null, attachments: [],
          queuePaused: false,
        });
        draft.submitted();
        scheduleQueue();
        return true;
      }
      return controller.submit(text, { ...options, resumeQueue: true });
    },
    cancelQueued(id) {
      const queued = snapshot.queued.filter((item) => item.id !== id);
      publish({ queued, queuePaused: queued.length > 0 && snapshot.queuePaused });
    },
    resumeQueue() {
      if (!snapshot.queued.length) return false;
      publish({ queuePaused: false });
      scheduleQueue();
      return true;
    },
    submit(question = snapshot.draft, { queuedId, resumeQueue = false, input, attachments = [],
      modelName = snapshot.modelName, reasoningEffort = snapshot.reasoningEffort } = {}) {
      if (active || snapshot.busy) return false;
      const text = String(question ?? '').trim();
      if (!text && !input?.attachments?.length) return false;
      const history = historyFromTurns(snapshot.turns);
      const error = validationError(text, history, !!input);
      const turn = createTurn(text, error);
      if (input) Object.assign(turn, { input, attachments });
      const { id } = turn;
      if (error) {
        publish({ open: true, turns: [...snapshot.turns, turn] });
        return false;
      }
      const request = { id, handle: null };
      active = request;
      publish({
        open: true,
        turns: [...snapshot.turns, turn],
        draft: queuedId ? snapshot.draft : '',
        composerContent: queuedId ? snapshot.composerContent : null,
        attachments: queuedId ? snapshot.attachments : [],
        inputHistory: [...snapshot.inputHistory, { text, ...(input?.composer_content ? { composer_content: input.composer_content } : {}), attachments }],
        queued: queuedId ? snapshot.queued.filter((item) => item.id !== queuedId) : snapshot.queued,
        queuePaused: resumeQueue ? false : snapshot.queuePaused,
        busy: true,
        stopping: false,
      });
      if (!queuedId) draft.submitted();
      // A subscriber can close/reset during publication.
      if (active !== request) return true;
      try {
        const handle = startStream({
          requestId: id,
          question: text,
          history,
          ...(input ? { input } : {}),
          ...(modelName ? { modelName, reasoningEffort } : {}),
          onDelta(delta) {
            if (active !== request || typeof delta !== 'string') return;
            const current = snapshot.turns.find((item) => item.id === id);
            updateTurn(request, { answer: current.answer + delta, status: 'streaming' });
          },
          onReset() {
            if (active !== request) return;
            // Discard only the current model step; text before its tool calls stays.
            const current = snapshot.turns.find((item) => item.id === id);
            const answer = current.answer.slice(0, current.stepStart);
            updateTurn(request, { answer, status: answer || current.tools.length ? 'streaming' : 'loading' });
          },
          onTool(event) {
            if (active !== request || !event?.callId) return;
            const current = snapshot.turns.find((item) => item.id === id);
            const existing = current.tools.some((tool) => tool.id === event.callId);
            const tools = existing
              ? current.tools.map((tool) => (tool.id === event.callId ? { ...tool, status: event.status } : tool))
              : [...current.tools, {
                id: event.callId,
                name: event.name,
                target: event.target,
                status: event.status,
                offset: current.answer.length,
              }];
            updateTurn(request, { tools, stepStart: current.answer.length, status: 'streaming' });
          },
          onSendRequest(event) {
            if (active !== request || !event?.sendId) return;
            const current = snapshot.turns.find((item) => item.id === id);
            updateTurn(request, {
              sends: [...(current.sends || []), {
                id: event.sendId,
                callId: event.callId || '',
                origin: 'model',
                text: String(event.message || ''),
                status: 'pending',
                error: '',
              }],
            });
          },
          onSendResult(event) {
            if (active !== request || !event?.sendId) return;
            const status = SIDE_CHAT_SEND_FINAL_STATUSES.has(event.status) ? event.status : 'failed';
            updateSend(event.sendId, {
              status,
              error: status === 'failed' ? String(event.message || '发送失败') : '',
            });
          },
          onDone(result = {}) {
            if (active !== request) return;
            const current = snapshot.turns.find((item) => item.id === id);
            const finalAnswer = typeof result.answer === 'string' ? result.answer : current.answer;
            const answer = result.cancelled && !finalAnswer ? current.answer : finalAnswer;
            if (!result.cancelled && !answer.trim()) {
              finish(request, { status: 'error', error: '旁路聊天未返回内容，请重试。', errorCode: 'SIDE_CHAT_EMPTY_RESPONSE' });
              return;
            }
            const patch = { status: result.cancelled ? 'stopped' : 'success' };
            patch.answer = answer;
            finish(request, patch);
          },
          onError(error) {
            finish(request, {
              status: 'error',
              error: typeof error === 'string' ? error : String(error?.message || '旁路聊天请求失败，请重试。'),
              errorCode: String(error?.code || 'SIDE_CHAT_ERROR'),
            });
          },
        });
        request.handle = handle;
        // A constructor can report a failure before returning its handle.
        if (active !== request) handle?.dispose();
        else if (snapshot.stopping) handle?.stop();
      } catch (error) {
        finish(request, { status: 'error', error: String(error?.message || '旁路聊天请求失败，请重试。'), errorCode: 'SIDE_CHAT_ERROR' });
      }
      return true;
    },
    // 引用和手动输入使用同一草稿,回答进行中也可继续编辑。
    insertQuote(text) {
      const next = sideChatQuoteDraft(snapshot.draft, text);
      controller.setDraft(next);
      publish({ open: true });
      return true;
    },
    // 用户在某条侧边回答上点「发到主会话」:挂一张预填问答的卡片,确认后才发送。
    startSendToMain(turnId) {
      const turn = snapshot.turns.find((item) => item.id === turnId);
      if (!turn || typeof sendToMain !== 'function' || !String(turn.answer || '').trim()) return false;
      if ((turn.sends || []).some((item) => item.origin === 'user'
          && (item.status === 'pending' || item.status === 'sending'))) return false;
      const send = {
        id: `user-send-${++userSendSequence}`,
        callId: '',
        origin: 'user',
        text: sideChatHandoffText(turn),
        status: 'pending',
        error: '',
      };
      publish({
        turns: snapshot.turns.map((item) => (item.id === turnId
          ? { ...item, sends: [...(item.sends || []), send] }
          : item)),
      });
      return true;
    },
    editSend(sendId, text) {
      const found = findSend(sendId);
      if (!found || found.send.status !== 'pending') return false;
      updateSend(sendId, { text: String(text ?? '') });
      return true;
    },
    confirmSend(sendId) {
      const found = findSend(sendId);
      if (!found || found.send.status !== 'pending') return false;
      const text = found.send.text.trim();
      if (!text) return false;
      if (found.send.origin === 'model') {
        const request = active;
        if (!request || request.id !== found.turn.id || !request.handle?.respondSend?.(sendId, true, text)) {
          updateSend(sendId, { status: 'failed', error: '侧边对话已结束，未发送。' });
          return false;
        }
        updateSend(sendId, { status: 'sending' });
        return true;
      }
      updateSend(sendId, { status: 'sending' });
      Promise.resolve()
        .then(() => sendToMain(text))
        .then((result) => {
          const status = SIDE_CHAT_SEND_FINAL_STATUSES.has(result?.delivery) ? result.delivery : 'started';
          if (findSend(sendId)?.send.status === 'sending') updateSend(sendId, { status });
        }, (error) => {
          if (findSend(sendId)?.send.status === 'sending') {
            updateSend(sendId, { status: 'failed', error: String(error?.message || '发送失败') });
          }
        });
      return true;
    },
    cancelSend(sendId) {
      const found = findSend(sendId);
      if (!found || found.send.status !== 'pending') return false;
      if (found.send.origin === 'model') {
        const request = active;
        if (request && request.id === found.turn.id) request.handle?.respondSend?.(sendId, false);
        updateSend(sendId, { status: 'cancelled' });
        return true;
      }
      // 用户自己开的卡片取消即移除。
      publish({
        turns: snapshot.turns.map((turn) => (turn.id === found.turn.id
          ? { ...turn, sends: turn.sends.filter((item) => item.id !== sendId) }
          : turn)),
      });
      return true;
    },
    stop() {
      if (!active || snapshot.stopping) return false;
      const request = active;
      publish({ stopping: true, queuePaused: snapshot.queued.length > 0 });
      if (active === request) {
        try { request.handle?.stop(); }
        catch { finish(request, { status: 'stopped' }); }
      }
      return true;
    },
    close() {
      pauseQueue();
      cancelImmediately();
      publish({ open: false });
    },
    suspend() {
      pauseQueue();
      cancelImmediately();
    },
    resume(retentionMinutes) {
      if (!active && lastFinishedAt !== null
          && now() - lastFinishedAt >= sideChatRetentionMilliseconds(retentionMinutes)) {
        controller.reset();
        return true;
      }
      return false;
    },
    clear() {
      cancelImmediately();
      draft.destroy();
      lastFinishedAt = null;
      publish({ ...initialSnapshot(), open: snapshot.open });
    },
    reset() {
      cancelImmediately();
      draft.destroy();
      lastFinishedAt = null;
      publish(initialSnapshot());
    },
    dispose() {
      cancelImmediately();
      draft.destroy();
      lastFinishedAt = null;
      listeners.clear();
      snapshot = initialSnapshot();
    },
  };
  return controller;
}
