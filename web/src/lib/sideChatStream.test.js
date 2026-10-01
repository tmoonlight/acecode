import assert from 'node:assert/strict';
import { createApi } from './api.js';
import { createSideChatStream, sideChatWebSocketUrl, SIDE_CHAT_CONNECT_TIMEOUT_MS, SIDE_CHAT_STOP_TIMEOUT_MS } from './sideChatStream.js';

function run(name, fn) {
  try { fn(); console.log(`[pass] side chat stream: ${name}`); }
  catch (error) { console.error(`[fail] side chat stream: ${name}`); throw error; }
}

function setup() {
  const sockets = [];
  const timers = new Map();
  const events = [];
  let timerId = 0;
  class Socket {
    constructor(url) { this.url = url; this.readyState = 0; this.sent = []; this.closed = 0; sockets.push(this); }
    send(message) { this.sent.push(JSON.parse(message)); }
    close() { this.closed++; this.readyState = 3; }
    open() { this.readyState = 1; this.onopen?.(); }
    message(type, payload) { this.onmessage?.({ data: JSON.stringify({ type, payload }) }); }
  }
  const dependencies = {
    WebSocket: Socket,
    setTimeout(fn, ms) { const id = ++timerId; timers.set(id, { fn, ms }); return id; },
    clearTimeout(id) { timers.delete(id); },
  };
  const options = {
    sessionId: 'session / one', origin: 'https://daemon.example:8443', token: 'token+secret',
    requestId: 'request-one', question: 'question', history: [],
    onDelta: (delta) => events.push(['delta', delta]),
    onReset: () => events.push(['reset']),
    onTool: (tool) => events.push(['tool', tool]),
    onDone: (result) => events.push(['done', result]),
    onError: (error) => events.push(['error', error]),
  };
  const handle = createSideChatStream(options, dependencies);
  return {
    handle, socket: sockets[0], timers, events, options, dependencies,
    fireTimer(ms) {
      const entry = [...timers].find(([, value]) => value.ms === ms);
      assert.ok(entry, `timer ${ms} exists`);
      timers.delete(entry[0]);
      entry[1].fn();
    },
  };
}

run('URL uses daemon scope, encoded identity and token with secure protocol', () => {
  assert.equal(sideChatWebSocketUrl('session / one', 'https://daemon.example:8443', 'token+secret'),
    'wss://daemon.example:8443/ws/sessions/session%20%2F%20one?token=token%2Bsecret');
  assert.equal(sideChatWebSocketUrl('local', '', '', { origin: 'http://localhost:3000' }),
    'ws://localhost:3000/ws/sessions/local');
});

run('private connection starts one side request without hello or main subscriptions', () => {
  const { socket, timers } = setup();
  assert.equal(socket.sent.length, 0);
  socket.open();
  assert.deepEqual(socket.sent, [{
    type: 'side_chat_start',
    payload: { session_id: 'session / one', request_id: 'request-one', question: 'question', history: [] },
  }]);
  assert.equal(timers.size, 0);
});

run('deltas, retry reset and authoritative completion are delivered privately', () => {
  const { socket, events, timers } = setup();
  socket.open();
  socket.message('side_chat_delta', { request_id: 'other', delta: 'foreign text' });
  socket.message('side_chat_delta', { request_id: 'request-one', delta: 'discard' });
  socket.message('side_chat_reset', { request_id: 'request-one' });
  socket.message('side_chat_delta', { request_id: 'request-one', delta: 'new' });
  const lateMessage = socket.onmessage;
  socket.message('side_chat_done', { request_id: 'request-one', answer: 'new answer', cancelled: false });
  lateMessage({ data: JSON.stringify({ type: 'side_chat_delta', payload: { request_id: 'request-one', delta: 'late' } }) });
  assert.deepEqual(events, [['delta', 'discard'], ['reset'], ['delta', 'new'], ['done', { answer: 'new answer', cancelled: false }]]);
  assert.equal(socket.closed, 1);
  assert.equal(socket.onmessage, null);
  assert.equal(timers.size, 0);
});

// 侧边对话的只读工具调用:进度原样转给界面;reset 只丢弃当前这一步的正文,
// 工具调用之前的正文保留(停止时作为已生成内容交回)。
run('tool progress is forwarded and reset keeps text written before the tool call', () => {
  const { handle, socket, events, fireTimer } = setup();
  socket.open();
  socket.message('side_chat_delta', { request_id: 'request-one', delta: 'before' });
  socket.message('side_chat_tool', {
    request_id: 'request-one', call_id: 'c1', name: 'file_read', target: 'a.txt', status: 'running',
  });
  const draft = `${String.fromCharCode(10).repeat(2)}draft`;
  socket.message('side_chat_delta', { request_id: 'request-one', delta: draft });
  socket.message('side_chat_reset', { request_id: 'request-one' });
  handle.stop();
  fireTimer(SIDE_CHAT_STOP_TIMEOUT_MS);
  assert.deepEqual(events, [
    ['delta', 'before'],
    ['tool', { callId: 'c1', name: 'file_read', target: 'a.txt', status: 'running' }],
    ['delta', draft],
    ['reset'],
    ['done', { answer: 'before', cancelled: true }],
  ]);
});

run('malformed tool progress is a protocol error', () => {
  const { socket, events } = setup();
  socket.open();
  socket.message('side_chat_tool', { request_id: 'request-one', name: 'file_read' });
  assert.equal(events.length, 1);
  assert.equal(events[0][0], 'error');
  assert.equal(events[0][1].code, 'SIDE_CHAT_PROTOCOL_ERROR');
});

run('stop waits for acknowledgement and sends cancellation once', () => {
  const { handle, socket, events, timers } = setup();
  socket.open();
  socket.message('side_chat_delta', { request_id: 'request-one', delta: 'partial' });
  handle.stop();
  handle.stop();
  assert.equal(socket.sent.length, 2);
  assert.deepEqual(socket.sent[1], { type: 'side_chat_stop', payload: { request_id: 'request-one' } });
  assert.equal(events.length, 1);
  assert.equal(socket.closed, 0);
  socket.message('side_chat_done', { request_id: 'request-one', answer: 'partial', cancelled: true });
  assert.deepEqual(events.at(-1), ['done', { answer: 'partial', cancelled: true }]);
  assert.equal(timers.size, 0);
  assert.equal(socket.closed, 1);
});

run('stop timeout closes the connection and retains streamed text', () => {
  const { handle, socket, events, fireTimer, timers } = setup();
  socket.open();
  socket.message('side_chat_delta', { request_id: 'request-one', delta: 'partial' });
  handle.stop();
  fireTimer(SIDE_CHAT_STOP_TIMEOUT_MS);
  assert.deepEqual(events.at(-1), ['done', { answer: 'partial', cancelled: true }]);
  assert.equal(socket.closed, 1);
  assert.equal(timers.size, 0);
});

run('stop before connection opens cancels without starting any model request', () => {
  const { handle, socket, events, timers } = setup();
  handle.stop();
  socket.open();
  assert.deepEqual(socket.sent, []);
  assert.deepEqual(events, [['done', { answer: '', cancelled: true }]]);
  assert.equal(timers.size, 0);
});

run('connect timeout reports failure and releases all resources', () => {
  const { socket, events, fireTimer, timers } = setup();
  fireTimer(SIDE_CHAT_CONNECT_TIMEOUT_MS);
  assert.equal(events[0][0], 'error');
  assert.equal(events[0][1].code, 'SIDE_CHAT_CONNECT_TIMEOUT');
  assert.equal(socket.closed, 1);
  assert.equal(timers.size, 0);
});

run('unexpected disconnect is an error, while disconnect during stop confirms cancellation', () => {
  const first = setup();
  first.socket.open();
  first.socket.onclose();
  assert.equal(first.events[0][1].code, 'SIDE_CHAT_CONNECTION_ERROR');
  const second = setup();
  second.socket.open();
  second.socket.message('side_chat_delta', { request_id: 'request-one', delta: 'partial' });
  second.handle.stop();
  second.socket.onclose();
  assert.deepEqual(second.events.at(-1), ['done', { answer: 'partial', cancelled: true }]);
  assert.equal(second.timers.size, 0);
});

run('old backend generic errors fail promptly without a request id', () => {
  for (const reason of ['unknown type', 'send hello first']) {
    const { socket, events } = setup();
    socket.open();
    socket.message('error', { reason, got: 'side_chat_start' });
    assert.equal(events[0][1].code, 'SIDE_CHAT_UNSUPPORTED');
    assert.equal(socket.closed, 1);
  }
});

run('provider errors preserve the server message and ignore foreign request failures', () => {
  const { socket, events } = setup();
  socket.open();
  socket.message('side_chat_error', { request_id: 'other', message: 'foreign failure' });
  assert.equal(events.length, 0);
  socket.message('side_chat_error', { request_id: 'request-one', code: 'MODEL_ERROR', message: 'provider failed' });
  assert.deepEqual(events, [['error', { code: 'MODEL_ERROR', message: 'provider failed' }]]);
});

run('invalid streaming payload fails rather than injecting non-text data', () => {
  const { socket, events } = setup();
  socket.open();
  socket.message('side_chat_delta', { request_id: 'request-one', delta: { content: 'bad' } });
  assert.equal(events[0][1].code, 'SIDE_CHAT_PROTOCOL_ERROR');
});

run('native agent providers explain why tool-free side chat is unavailable', () => {
  const { socket, events } = setup();
  socket.open();
  socket.message('side_chat_error', {
    request_id: 'request-one', code: 'SIDE_CHAT_PROVIDER_UNSUPPORTED', message: 'tool-free requests unsupported',
  });
  assert.deepEqual(events, [['error', {
    code: 'SIDE_CHAT_PROVIDER_UNSUPPORTED', message: '当前模型不支持只读旁路聊天，请切换模型后重试。',
  }]]);
  assert.equal(socket.closed, 1);
});

run('dispose suppresses queued callbacks and clears both socket and timer ownership', () => {
  const { handle, socket, events, timers } = setup();
  const lateOpen = socket.onopen;
  const lateClose = socket.onclose;
  handle.dispose();
  handle.dispose();
  lateOpen();
  lateClose();
  assert.deepEqual(events, []);
  assert.deepEqual(socket.sent, []);
  assert.equal(socket.closed, 1);
  assert.equal(timers.size, 0);
});

run('socket constructor failure is reported synchronously with a usable disposal handle', () => {
  const errors = [];
  const handle = createSideChatStream({ sessionId: 'session', origin: 'http://localhost', requestId: 'id', onError: (error) => errors.push(error) }, {
    WebSocket: class { constructor() { throw new Error('no network'); } },
  });
  assert.equal(errors[0].code, 'SIDE_CHAT_CONNECTION_ERROR');
  handle.stop();
  handle.dispose();
});

run('createApi forwards its daemon connection scope to private side chat sockets', () => {
  const previousSocket = globalThis.WebSocket;
  const urls = [];
  try {
    globalThis.WebSocket = class { constructor(url) { urls.push(url); } close() {} };
    const api = createApi({ origin: 'https://separate-daemon.example:49001', token: 'scoped-token' });
    const handle = api.streamSideChat('private session', { requestId: 'request', question: 'question' });
    handle.dispose();
    assert.deepEqual(urls, ['wss://separate-daemon.example:49001/ws/sessions/private%20session?token=scoped-token']);
  } finally { globalThis.WebSocket = previousSocket; }
});
