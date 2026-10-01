export const SIDE_CHAT_QUESTION_MAX_BYTES = 16000;
export const SIDE_CHAT_HISTORY_MAX_BYTES = 256 * 1024;
export const SIDE_CHAT_HISTORY_MAX_MESSAGES = 200;

const utf8 = new TextEncoder();
let requestSequence = 0;

function initialSnapshot() {
  return { open: false, turns: [], draft: '', busy: false, stopping: false };
}

function historyFromTurns(turns) {
  return turns.flatMap((turn) => (
    (turn.status === 'success' || turn.status === 'stopped') && turn.answer.trim()
      ? [{ role: 'user', content: turn.question }, { role: 'assistant', content: turn.answer }]
      : []
  ));
}

// Read-only tools the daemon lets a side answer call, with their row verbs.
const SIDE_CHAT_TOOL_VERBS = {
  file_read: '读取',
  grep: '搜索',
  glob: '查找文件',
  lsp: '查询代码',
};

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

function validationError(question, history) {
  if (utf8.encode(question).length > SIDE_CHAT_QUESTION_MAX_BYTES) {
    return { code: 'SIDE_CHAT_QUESTION_TOO_LONG', message: '问题过长，请缩短后重试。' };
  }
  if (history.length > SIDE_CHAT_HISTORY_MAX_MESSAGES
      || history.reduce((bytes, message) => bytes + utf8.encode(message.content).length, 0) > SIDE_CHAT_HISTORY_MAX_BYTES) {
    return { code: 'SIDE_CHAT_HISTORY_TOO_LONG', message: '旁路聊天记录过长，无法继续发送。' };
  }
  return null;
}

// State ownership stays outside React so submitting, stopping and session
// changes cannot race stale render closures or late transport callbacks.
export function createSideChatController({ startStream } = {}) {
  if (typeof startStream !== 'function') throw new TypeError('startStream is required');
  const listeners = new Set();
  let snapshot = initialSnapshot();
  let active = null;

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
    active = null;
    request.handle?.dispose();
    updateTurn(request, { ...patch, tools: settledTools(request) }, { busy: false, stopping: false });
  }

  function cancelImmediately() {
    if (!active) return;
    const request = active;
    // Invalidate first: even a synchronous stop acknowledgement is stale.
    active = null;
    try { request.handle?.stop(); }
    finally { request.handle?.dispose(); }
    updateTurn(request, { status: 'stopped', tools: settledTools(request) }, { busy: false, stopping: false });
  }

  const controller = {
    subscribe(listener) {
      listeners.add(listener);
      return () => listeners.delete(listener);
    },
    getSnapshot() { return snapshot; },
    open() { publish({ open: true }); },
    setDraft(draft) {
      if (snapshot.busy) return false;
      publish({ draft: String(draft ?? '') });
      return true;
    },
    submit(question = snapshot.draft) {
      if (active || snapshot.busy) return false;
      const text = String(question ?? '').trim();
      if (!text) return false;
      const history = historyFromTurns(snapshot.turns);
      const error = validationError(text, history);
      const id = `side-${Date.now().toString(36)}-${++requestSequence}`;
      const turn = {
        id,
        question: text,
        answer: '',
        tools: [],
        stepStart: 0,
        status: error ? 'error' : 'loading',
        error: error?.message || '',
        errorCode: error?.code || '',
      };
      if (error) {
        publish({ open: true, turns: [...snapshot.turns, turn] });
        return false;
      }
      const request = { id, handle: null };
      active = request;
      publish({ open: true, turns: [...snapshot.turns, turn], draft: '', busy: true, stopping: false });
      // A subscriber can close/reset during publication.
      if (active !== request) return true;
      try {
        const handle = startStream({
          requestId: id,
          question: text,
          history,
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
    stop() {
      if (!active || snapshot.stopping) return false;
      const request = active;
      publish({ stopping: true });
      if (active === request) {
        try { request.handle?.stop(); }
        catch { finish(request, { status: 'stopped' }); }
      }
      return true;
    },
    close() {
      cancelImmediately();
      publish({ open: false });
    },
    clear() {
      cancelImmediately();
      publish({ ...initialSnapshot(), open: snapshot.open });
    },
    reset() {
      cancelImmediately();
      publish(initialSnapshot());
    },
    dispose() {
      cancelImmediately();
      listeners.clear();
      snapshot = initialSnapshot();
    },
  };
  return controller;
}
