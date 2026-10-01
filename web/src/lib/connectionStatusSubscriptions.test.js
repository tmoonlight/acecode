import assert from 'node:assert/strict';
import { AceConnection } from './connection.js';

const priorSocket = globalThis.WebSocket;
const priorLocation = globalThis.location;
const frames = [];
const sockets = [];
class Socket {
  static OPEN = 1;
  static CLOSED = 3;
  readyState = 0;
  constructor() { sockets.push(this); }
  send(text) { frames.push(JSON.parse(text)); }
  close() { this.readyState = Socket.CLOSED; }
}
try {
  globalThis.WebSocket = Socket;
  globalThis.location = { protocol: 'http:', host: '127.0.0.1:12345' };
  const connection = new AceConnection();
  connection._token = 'synthetic';
  for (let round = 0; round < 3; round++) {
    for (let i = 0; i < 10; i++) connection.subscribeWorkspaceStatus('workspace-' + i);
    if (round === 0) { sockets[0].readyState = Socket.OPEN; sockets[0].onopen(); }
  }
  assert.equal(frames.filter(frame => frame.type === 'status_subscribe').length, 10);
  connection.reconfigure({ token: 'second-synthetic' });
  sockets[1].readyState = Socket.OPEN;
  sockets[1].onopen();
  assert.equal(frames.filter(frame => frame.type === 'status_subscribe').length, 20);
  connection.subscribeWorkspaceStatus('workspace-1');
  assert.equal(frames.filter(frame => frame.type === 'status_subscribe').length, 20);
  connection.unsubscribeWorkspaceStatus('workspace-1');
  connection.subscribeWorkspaceStatus('workspace-1');
  assert.equal(frames.filter(frame => frame.type === 'status_subscribe').length, 21);
  connection.retainSession('recovered', { since: 0, replayFromStart: true });
  const replay = frames.find(frame => frame.type === 'subscribe');
  assert.equal(replay.payload.since, 0);
  assert.equal(replay.payload.replay_from_start, true);
  // A second observer can request catch-up even when the sidebar already owns a subscription.
  connection.retainSession('recovered', { since: 0, replayFromStart: true });
  assert.equal(frames.filter(frame => frame.type === 'subscribe').length, 2);
  connection.unbind();
} finally {
  globalThis.WebSocket = priorSocket;
  globalThis.location = priorLocation;
}
console.log('[pass] status subscriptions deduplicate refreshes and replay once after reconnect');
