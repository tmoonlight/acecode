import assert from 'node:assert/strict';
import { AceConnection } from './connection.js';

// 端到端覆盖 connection.js 的心跳、订阅游标核对与强制重订阅(假 WebSocket,不连网)。
// 对应反馈:页面一直显示「正在推理」,daemon 早已收尾,切走再切回才看到完成。

const priorSocket = globalThis.WebSocket;
const priorLocation = globalThis.location;
const sockets = [];
class Socket {
  static OPEN = 1;
  static CLOSED = 3;
  readyState = 0;
  frames = [];
  constructor(url) { this.url = url; sockets.push(this); }
  send(text) { this.frames.push(JSON.parse(text)); }
  close() { this.readyState = Socket.CLOSED; }
}
const open = (socket) => { socket.readyState = Socket.OPEN; socket.onopen(); };
const deliver = (socket, msg) => socket.onmessage({ data: JSON.stringify(msg) });
const nextTask = () => new Promise((resolve) => setTimeout(resolve, 0));

try {
  globalThis.WebSocket = Socket;
  globalThis.location = { protocol: 'http:', host: '127.0.0.1:12345' };
  const connection = new AceConnection();
  connection._token = 'synthetic';
  const messages = [];
  const resets = [];
  const disconnects = [];
  const reports = [];
  connection.addEventListener('message', (event) => messages.push(event.detail));
  connection.addEventListener('session_seq_reset', (event) => resets.push(event.detail));
  connection.addEventListener('disconnect', (event) => disconnects.push(event.detail));
  connection.setDiagnosticsReporter((payload) => { reports.push(payload); return Promise.resolve(); });

  connection.retainSession('s1');
  const first = sockets[0];
  open(first);
  assert.equal(first.frames.filter((frame) => frame.type === 'subscribe').length, 1);

  // 场景:心跳的 pong 没有 session_id。期望:不被兜底归到当前会话再派发给 transcript
  // (否则每 20 秒一次无意义的提交和重渲染),但算作连接存活的证据。
  connection.lastMessageAt = 0;
  deliver(first, { type: 'pong' });
  assert.equal(messages.length, 0);
  assert.ok(connection.lastMessageAt > 0);
  console.log('[pass] pong proves liveness without being dispatched as a session event');

  // 场景:首次回执带服务端游标,随后收到实时事件;再一次回执(重连)显示分发器实例号
  // 变了(daemon 重启)。期望:第一次只记下实例号;第二次清零游标并广播 session_seq_reset,
  // 带上旧 seq / 旧实例号供诊断。
  deliver(first, { type: 'subscribe_ack', session_id: 's1', payload: { session_id: 's1', event_seq: 10, event_epoch: 'e1' } });
  assert.deepEqual(connection.sessionCursor('s1'), { lastSeq: 0, epoch: 'e1' });
  deliver(first, { type: 'token', session_id: 's1', seq: 50, payload: { text: 'x' } });
  assert.equal(connection.sessionCursor('s1').lastSeq, 50);
  deliver(first, { type: 'subscribe_ack', session_id: 's1', payload: { session_id: 's1', event_seq: 3, event_epoch: 'e2' } });
  assert.equal(resets.length, 1);
  assert.equal(resets[0].reason, 'epoch_changed');
  assert.equal(resets[0].previousSeq, 50);
  assert.equal(resets[0].previousEpoch, 'e1');
  assert.deepEqual(connection.sessionCursor('s1'), { lastSeq: 0, epoch: 'e2' });
  console.log('[pass] a new dispatcher instance resets the stored cursor and notifies transcripts');

  // 场景:连接静默 25 秒。期望:发一次 ping。
  connection.lastMessageAt = Date.now() - 25_000;
  connection._heartbeatTick(first);
  assert.equal(first.frames.filter((frame) => frame.type === 'ping').length, 1);
  assert.ok(connection.pingSentAt > 0);

  // 场景:ping 发出 11 秒仍没有任何消息 —— 连接假活。期望:摘掉旧 socket 立即新建连接
  // (不等 onclose)、派发 disconnect、经注入的 reporter 上报 heartbeat_timeout;新连接
  // 打开后按当前游标重新订阅。
  connection.pingSentAt = Date.now() - 11_000;
  connection.lastMessageAt = connection.pingSentAt - 1;
  connection._heartbeatTick(first);
  assert.equal(first.readyState, Socket.CLOSED);
  assert.equal(first.onmessage, null, 'stale socket callbacks are detached');
  assert.equal(disconnects.at(-1).reason, 'heartbeat_timeout');
  assert.equal(reports.length, 1);
  assert.equal(reports[0].kind, 'heartbeat_timeout');
  assert.equal(connection.heartbeatTimeouts, 1);
  await nextTask();
  assert.equal(sockets.length, 2, 'a fresh socket replaces the dead one');
  const second = sockets[1];
  open(second);
  const resubscribed = second.frames.find((frame) => frame.type === 'subscribe');
  assert.equal(resubscribed.payload.session_id, 's1');
  console.log('[pass] an unanswered ping forces a reconnect and resubscription');

  // 场景:卡住检测对账完成后强制重订阅。期望:不改引用计数,发 replay_from_start 订阅,
  // since 用对账后的游标,并记下刚读到的实例号(免得下一个回执又判一次实例变化)。
  assert.equal(connection.resubscribe('s1', { since: 7, epoch: 'e9' }), true);
  const replay = second.frames.filter((frame) => frame.type === 'subscribe').at(-1);
  assert.equal(replay.payload.since, 7);
  assert.equal(replay.payload.replay_from_start, true);
  assert.equal(connection.sessionCursor('s1').epoch, 'e9');
  assert.equal(connection.sessionRefs.count('s1'), 1);
  assert.equal(connection.resubscribe('unknown-session', { since: 1 }), false);
  console.log('[pass] resubscribe replays from the reconciled cursor without touching ref counts');

  connection.unbind();
  assert.equal(connection.heartbeatTimer, null, 'unbind stops the heartbeat timer');
} finally {
  globalThis.WebSocket = priorSocket;
  globalThis.location = priorLocation;
}
