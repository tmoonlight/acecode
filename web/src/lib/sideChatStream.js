// One private WebSocket per side request. It never subscribes to main-session
// events, and closing it only cancels that side request.
export const SIDE_CHAT_CONNECT_TIMEOUT_MS = 15000;
export const SIDE_CHAT_STOP_TIMEOUT_MS = 5000;

export function sideChatWebSocketUrl(sessionId, origin = '', token = '', location = globalThis.location) {
  const url = new URL(`/ws/sessions/${encodeURIComponent(sessionId)}`, origin || location?.origin);
  url.protocol = url.protocol === 'https:' ? 'wss:' : 'ws:';
  if (token) url.searchParams.set('token', token);
  return url.toString();
}

export function createSideChatStream({
  sessionId,
  origin = '',
  token = '',
  requestId,
  question,
  history = [],
  onDelta,
  onReset,
  onTool,
  onDone,
  onError,
}, dependencies = {}) {
  const Socket = dependencies.WebSocket || globalThis.WebSocket;
  const setTimer = dependencies.setTimeout || globalThis.setTimeout;
  const clearTimer = dependencies.clearTimeout || globalThis.clearTimeout;
  let socket = null;
  let connectTimer = null;
  let stopTimer = null;
  let finished = false;
  let stopping = false;
  let answer = '';
  // Length of the answer when the current model step started (after its
  // predecessor's tool calls); a reset truncates back to it.
  let stepStart = 0;

  function cleanup() {
    if (connectTimer !== null) clearTimer(connectTimer);
    if (stopTimer !== null) clearTimer(stopTimer);
    connectTimer = stopTimer = null;
    if (!socket) return;
    socket.onopen = socket.onmessage = socket.onclose = socket.onerror = null;
    try { socket.close(1000, 'side chat finished'); } catch { /* Already closed. */ }
  }

  function done(payload) {
    if (finished) return;
    finished = true;
    cleanup();
    onDone?.(payload);
  }

  function fail(code, message) {
    if (finished) return;
    finished = true;
    cleanup();
    onError?.({ code, message });
  }

  function send(type, payload) {
    socket.send(JSON.stringify({ type, payload }));
  }

  const handle = {
    stop() {
      if (finished || stopping) return;
      stopping = true;
      if (socket?.readyState !== 1) {
        done({ answer, cancelled: true });
        return;
      }
      // A daemon which never acknowledges cancellation cannot keep the input
      // locked forever. Disconnect is also a server-side cancellation signal.
      stopTimer = setTimer(() => done({ answer, cancelled: true }), SIDE_CHAT_STOP_TIMEOUT_MS);
      try { send('side_chat_stop', { request_id: requestId }); }
      catch { done({ answer, cancelled: true }); }
    },
    dispose() {
      if (finished) return;
      finished = true;
      cleanup();
    },
  };

  try {
    socket = new Socket(sideChatWebSocketUrl(sessionId, origin, token, dependencies.location));
    connectTimer = setTimer(
      () => fail('SIDE_CHAT_CONNECT_TIMEOUT', '旁路聊天连接超时，请重试。'),
      SIDE_CHAT_CONNECT_TIMEOUT_MS,
    );
    socket.onopen = () => {
      if (finished) return;
      clearTimer(connectTimer);
      connectTimer = null;
      try {
        send('side_chat_start', {
          session_id: sessionId,
          request_id: requestId,
          question,
          history,
        });
      } catch {
        fail('SIDE_CHAT_CONNECTION_ERROR', '旁路聊天连接失败，请重试。');
      }
    };
    socket.onmessage = (event) => {
      if (finished) return;
      let message;
      try { message = JSON.parse(event.data); }
      catch {
        fail('SIDE_CHAT_PROTOCOL_ERROR', '旁路聊天响应格式错误，请重试。');
        return;
      }
      const payload = message?.payload || {};
      if (message?.type === 'error') {
        // Older daemons reject this new message before request binding and may
        // therefore return no request_id. This socket has no other consumer.
        const reason = String(payload.message || payload.reason || '');
        const unsupported = /unknown type|send hello first/i.test(reason);
        fail(
          unsupported ? 'SIDE_CHAT_UNSUPPORTED' : (payload.code || 'SIDE_CHAT_PROTOCOL_ERROR'),
          unsupported ? '当前后端不支持流式旁路聊天，请更新 ACECode 后重试。' : (reason || '旁路聊天请求失败，请重试。'),
        );
        return;
      }
      if (payload.request_id !== requestId) return;
      switch (message.type) {
        case 'side_chat_delta':
          if (typeof payload.delta !== 'string') {
            fail('SIDE_CHAT_PROTOCOL_ERROR', '旁路聊天响应格式错误，请重试。');
            return;
          }
          answer += payload.delta;
          onDelta?.(payload.delta);
          break;
        case 'side_chat_reset':
          // Only the current model step is discarded; text before the step's
          // tool calls stays. The final side_chat_done answer is authoritative.
          answer = answer.slice(0, stepStart);
          onReset?.();
          break;
        case 'side_chat_tool':
          if (typeof payload.call_id !== 'string' || typeof payload.status !== 'string') {
            fail('SIDE_CHAT_PROTOCOL_ERROR', '旁路聊天响应格式错误，请重试。');
            return;
          }
          stepStart = answer.length;
          onTool?.({
            callId: payload.call_id,
            name: typeof payload.name === 'string' ? payload.name : '',
            target: typeof payload.target === 'string' ? payload.target : '',
            status: payload.status,
          });
          break;
        case 'side_chat_done':
          done({ answer: typeof payload.answer === 'string' ? payload.answer : answer, cancelled: payload.cancelled === true });
          break;
        case 'side_chat_error':
          fail(payload.code || 'SIDE_CHAT_ERROR', payload.code === 'SIDE_CHAT_PROVIDER_UNSUPPORTED'
            ? '当前模型不支持只读旁路聊天，请切换模型后重试。'
            : String(payload.message || '旁路聊天请求失败，请重试。'));
          break;
        default:
          if (String(message.type || '').startsWith('side_chat_')) {
            fail('SIDE_CHAT_PROTOCOL_ERROR', '旁路聊天响应格式错误，请重试。');
          }
      }
    };
    socket.onerror = socket.onclose = () => {
      if (stopping) done({ answer, cancelled: true });
      else fail('SIDE_CHAT_CONNECTION_ERROR', '旁路聊天连接已中断，请重试。');
    };
  } catch {
    fail('SIDE_CHAT_CONNECTION_ERROR', '旁路聊天连接失败，请重试。');
  }
  return handle;
}
