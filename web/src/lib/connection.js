// WebSocket 连接 + 指数退避重连 + sessionStorage 持久化 last_seq。
// 服务端→客户端事件 dispatch 给 listener。客户端→服务端命令 send_*() 直接写 socket。
// 心跳与订阅游标核对见 liveConnectionHealth.js:连接卡死时主动重连,服务端事件
// 分发器换实例(seq 重新从 1 数)时清零游标并广播 session_seq_reset。

import { getToken } from './auth.js';
import {
  HEARTBEAT_TICK_MS,
  HEARTBEAT_VISIBLE_IDLE_MS,
  evaluateSubscriptionCursor,
  heartbeatAction,
  webSocketStateName,
} from './liveConnectionHealth.js';

const MAX_BACKOFF_MS = 30_000;
const NORMAL_CLOSE = 1000;

function readSessionStorage(key) {
  try {
    return typeof sessionStorage === 'undefined' ? null : sessionStorage.getItem(key);
  } catch {
    return null;
  }
}

function writeSessionStorage(key, value) {
  try {
    if (typeof sessionStorage !== 'undefined') sessionStorage.setItem(key, value);
  } catch {}
}

const seqStorageKey = (sessionId) => `ace-seq-${sessionId}`;
const epochStorageKey = (sessionId) => `ace-seq-epoch-${sessionId}`;

function storedCursor(sessionId) {
  return {
    lastSeq: parseInt(readSessionStorage(seqStorageKey(sessionId)) || '0', 10) || 0,
    epoch: readSessionStorage(epochStorageKey(sessionId)) || '',
  };
}

function closeWs(ws, reason = 'client closing') {
  if (!ws) return;
  try { ws.close(NORMAL_CLOSE, reason); } catch {}
}

export function createSessionSubscriptionManager({ subscribe, unsubscribe } = {}) {
  const refs = new Map();
  return {
    retain(sessionId) {
      if (!sessionId) return 0;
      const current = refs.get(sessionId) || 0;
      const next = current + 1;
      refs.set(sessionId, next);
      if (current === 0) subscribe?.(sessionId);
      return next;
    },
    release(sessionId) {
      if (!sessionId) return 0;
      const current = refs.get(sessionId) || 0;
      if (current <= 1) {
        refs.delete(sessionId);
        if (current === 1) unsubscribe?.(sessionId);
        return 0;
      }
      const next = current - 1;
      refs.set(sessionId, next);
      return next;
    },
    count(sessionId) {
      return refs.get(sessionId) || 0;
    },
    snapshot() {
      return new Map(refs);
    },
    clear() {
      refs.clear();
    },
  };
}

export class AceConnection extends EventTarget {
  constructor() {
    super();
    this.ws = null;
    this.sessionId = '';
    this.sessions = new Map();
    this.statusWorkspaces = new Set();
    this.attempts = 0;
    this.closing = false;
    this._host = '';   // 留空 → 用 location.host(单 workspace 默认)
    this._port = '';
    this._token = '';
    // 心跳:最近一次收到任何消息的时间、尚未得到答复的 ping 的发出时间。
    this.lastMessageAt = 0;
    this.pingSentAt = 0;
    this.heartbeatTimer = null;
    this.heartbeatTimeouts = 0;
    this.lastForcedReconnect = null;
    this.diagnosticsReporter = null;
    if (typeof document !== 'undefined' && typeof document.addEventListener === 'function') {
      // 页面重新可见(解锁屏幕、从托盘唤起)时立刻验一次连接,不等下一个心跳周期。
      document.addEventListener('visibilitychange', () => {
        if (document.visibilityState === 'visible') this.checkLiveness();
      });
    }
    this.sessionRefs = createSessionSubscriptionManager({
      subscribe: (sessionId) => this.subscribe(sessionId),
      unsubscribe: (sessionId) => this.unsubscribe(sessionId),
    });
  }

  reconfigure({ port, token }) {
    const nextHost = port !== undefined ? (port ? `127.0.0.1:${port}` : '') : this._host;
    const nextToken = token != null ? token : this._token;
    const changed = nextHost !== this._host || nextToken !== this._token;
    this._host = nextHost;
    if (port !== undefined) this._port = port ? String(port) : '';
    this._token = nextToken;
    if (changed && (this.sessions.size || this.statusWorkspaces.size)) this._reopen();
  }

  bind(sessionId) {
    this.subscribe(sessionId);
  }

  retainSession(sessionId, options = {}) {
    const retained = this.sessionRefs.count(sessionId);
    if (options.replayFromStart) {
      const previous = this.sessions.get(sessionId);
      this.sessions.set(sessionId, {
        lastSeq: Math.max(0, Number(options.since) || 0),
        epoch: previous?.epoch || readSessionStorage(epochStorageKey(sessionId)) || '',
        replayFromStart: true,
      });
      if (retained > 0) this._sendSubscribe(sessionId);
    }
    return this.sessionRefs.retain(sessionId);
  }

  releaseSession(sessionId) {
    return this.sessionRefs.release(sessionId);
  }

  subscribe(sessionId) {
    if (!sessionId) return;
    this.sessionId = sessionId;
    if (!this.sessions.has(sessionId)) {
      this.sessions.set(sessionId, storedCursor(sessionId));
    }
    if (!this.ws || this.ws.readyState === WebSocket.CLOSED) this._open();
    else if (this.ws.readyState === WebSocket.OPEN) this._sendSubscribe(sessionId);
  }

  unsubscribe(sessionId) {
    if (!sessionId || !this.sessions.has(sessionId)) return;
    this._send({ type: 'unsubscribe', payload: { session_id: sessionId } });
    this.sessions.delete(sessionId);
    if (this.sessionId === sessionId) this.sessionId = this.sessions.keys().next().value || '';
    if (this.sessions.size === 0 && this.statusWorkspaces.size === 0) this._closeSocket('client no sessions');
  }

  subscribeWorkspaceStatus(workspaceHash) {
    if (!workspaceHash) return;
    const alreadySubscribed = this.statusWorkspaces.has(workspaceHash);
    this.statusWorkspaces.add(workspaceHash);
    if (!this.ws || this.ws.readyState === WebSocket.CLOSED) this._open();
    else if (!alreadySubscribed && this.ws.readyState === WebSocket.OPEN) this._sendStatusSubscribe(workspaceHash);
  }

  unsubscribeWorkspaceStatus(workspaceHash) {
    if (!workspaceHash || !this.statusWorkspaces.has(workspaceHash)) return;
    this._send({ type: 'status_unsubscribe', payload: { workspace_hash: workspaceHash } });
    this.statusWorkspaces.delete(workspaceHash);
    if (this.sessions.size === 0 && this.statusWorkspaces.size === 0) this._closeSocket('client no status scopes');
  }

  unbind() {
    this._stopHeartbeat();
    this.sessions.clear();
    this.statusWorkspaces.clear();
    this.sessionRefs.clear();
    if (this.ws) {
      this._closeSocket('client unbind');
    }
    this.sessionId = '';
    this.attempts = 0;
    this.closing = false;
  }

  _activeHost()  { return this._host  || location.host; }
  _activeToken() { return this._token || getToken() || ''; }

  _closeSocket(reason) {
    if (!this.ws) return;
    this._stopHeartbeat();
    this.closing = true;
    try { this.ws.onopen = this.ws.onmessage = this.ws.onclose = this.ws.onerror = null; } catch {}
    closeWs(this.ws, reason);
    this.ws = null;
    this.closing = false;
  }

  _reopen() {
    this._closeSocket('client reconfigure');
    this._open();
  }

  _sendSubscribe(sessionId) {
    const state = this.sessions.get(sessionId);
    if (!state) return;
    this._send({
      type: 'subscribe',
      payload: { session_id: sessionId, since: state.lastSeq || 0,
        ...(state.replayFromStart ? { replay_from_start: true } : {}) },
    });
  }

  _sendStatusSubscribe(workspaceHash) {
    this._send({
      type: 'status_subscribe',
      payload: { workspace_hash: workspaceHash },
    });
  }

  _open() {
    if (this.sessions.size === 0 && this.statusWorkspaces.size === 0) return;
    this._stopHeartbeat();
    if (this.ws) {
      try { this.ws.onopen = this.ws.onmessage = this.ws.onclose = this.ws.onerror = null; } catch {}
      closeWs(this.ws, 'client rebind');
    }
    const proto   = location.protocol === 'https:' ? 'wss:' : 'ws:';
    const token   = this._activeToken();
    const tokenQs = token ? `?token=${encodeURIComponent(token)}` : '';
    const host    = this._activeHost();
    const url     = `${proto}//${host}/ws/sessions/_multiplex${tokenQs}`;
    const ws = new WebSocket(url);
    this.ws = ws;
    ws.onopen = () => {
      if (this.ws !== ws) return;
      this.attempts = 0;
      this.lastMessageAt = Date.now();
      this.pingSentAt = 0;
      this._startHeartbeat(ws);
      this.dispatchEvent(new Event('open'));
      for (const sessionId of this.sessions.keys()) this._sendSubscribe(sessionId);
      for (const workspaceHash of this.statusWorkspaces.keys()) this._sendStatusSubscribe(workspaceHash);
    };
    ws.onmessage = (e) => {
      if (this.ws !== ws) return;
      this.lastMessageAt = Date.now();
      let msg;
      try { msg = JSON.parse(e.data); } catch { return; }
      // pong 只证明连接活着,不属于任何会话;别让它被下面的兜底归到当前会话、
      // 再被 transcript 当成一帧事件提交(每次心跳都会触发一次重渲染)。
      if (msg?.type === 'pong') return;
      const sid = msg.session_id || msg.payload?.session_id || this.sessionId || '';
      if (sid) msg.session_id = sid;
      if (sid && (msg.type === 'subscribe_ack' || msg.type === 'hello_ack')) {
        const state = this.sessions.get(sid);
        if (state) {
          state.replayFromStart = false;
          this._applyAckCursor(sid, state, msg.payload || {});
        }
      }
      if (sid && typeof msg.seq === 'number') {
        const state = this.sessions.get(sid) || { lastSeq: 0 };
        // 乱序探针:正常情况下同一连接内 seq 严格递增(服务端先按序补发历史、
        // 再切实时)。一旦收到 seq < 已见最大值,说明实时帧抢在历史回放之前
        // 送达 —— 这正是 EventDispatcher::subscribe 有序补发修复要消灭的现象,
        // 也是 "消息显示不全" 的根因信号。留作回归告警(低噪声,只在异常时打)。
        if (state.lastSeq && msg.seq < state.lastSeq) {
          // eslint-disable-next-line no-console
          console.warn(
            `[ace-ws] out-of-order event sid=${sid} seq=${msg.seq} < lastSeq=${state.lastSeq} type=${msg.type || msg.payload?.role || '?'}`,
          );
        }
        if (msg.seq > state.lastSeq) {
          state.lastSeq = msg.seq;
          this.sessions.set(sid, state);
          writeSessionStorage(seqStorageKey(sid), String(state.lastSeq));
        }
      }
      this.dispatchEvent(new CustomEvent('message', { detail: msg }));
    };
    const reconnect = () => {
      if (this.closing || (this.sessions.size === 0 && this.statusWorkspaces.size === 0)) return;
      if (this.ws !== ws) return;
      this._stopHeartbeat();
      this.ws = null;
      const delay = Math.min(MAX_BACKOFF_MS, 1000 * Math.pow(2, this.attempts++));
      this.dispatchEvent(new CustomEvent('disconnect', { detail: { delay } }));
      setTimeout(() => this._open(), delay);
    };
    ws.onclose = reconnect;
    ws.onerror = () => closeWs(ws, 'client error');
  }

  // 订阅回执带回服务端当前 seq 与分发器实例号时核对本地游标。服务端换了实例
  // (daemon 重启 / 会话运行实体重建)就清零游标并广播 session_seq_reset:各 transcript
  // 据此把自己的 lastSeq 清零并与服务端重新对账,否则新实例的事件 seq 都比旧值小,
  // 会被当成过期帧全部丢弃。服务端的实时投递与 since 无关,这里不需要重新订阅。
  _applyAckCursor(sessionId, state, payload) {
    if (!Object.prototype.hasOwnProperty.call(payload, 'event_seq')
      && !Object.prototype.hasOwnProperty.call(payload, 'event_epoch')) return;
    const previousSeq = state.lastSeq || 0;
    const previousEpoch = state.epoch || '';
    const verdict = evaluateSubscriptionCursor({
      storedSeq: previousSeq,
      storedEpoch: previousEpoch,
      serverSeq: payload.event_seq,
      serverEpoch: payload.event_epoch,
    });
    state.epoch = verdict.epoch;
    if (verdict.epoch) writeSessionStorage(epochStorageKey(sessionId), verdict.epoch);
    if (!verdict.reset) return;
    state.lastSeq = 0;
    writeSessionStorage(seqStorageKey(sessionId), '0');
    this.dispatchEvent(new CustomEvent('session_seq_reset', {
      detail: {
        sessionId,
        reason: verdict.reason,
        previousSeq,
        previousEpoch,
        serverSeq: Number(payload.event_seq) || 0,
        serverEpoch: verdict.epoch,
      },
    }));
  }

  _startHeartbeat(ws) {
    this._stopHeartbeat();
    const timer = setInterval(() => this._heartbeatTick(ws), HEARTBEAT_TICK_MS);
    timer?.unref?.();
    this.heartbeatTimer = timer;
  }

  _stopHeartbeat() {
    if (this.heartbeatTimer !== null) {
      clearInterval(this.heartbeatTimer);
      this.heartbeatTimer = null;
    }
    this.pingSentAt = 0;
  }

  _heartbeatTick(ws, options = {}) {
    if (this.ws !== ws || !ws || ws.readyState !== WebSocket.OPEN) return;
    const now = Date.now();
    const action = heartbeatAction({
      now,
      lastMessageAt: this.lastMessageAt,
      pingSentAt: this.pingSentAt,
      ...(options.idleMs !== undefined ? { idleMs: options.idleMs } : {}),
    });
    if (action === 'ping') {
      if (this._send({ type: 'ping' })) this.pingSentAt = now;
      else this._forceReconnect('heartbeat_send_failed');
    } else if (action === 'reconnect') {
      this._forceReconnect('heartbeat_timeout');
    }
  }

  // 页面重新可见时调用:连接静默超过几秒就立刻补一次 ping,下一次节拍即可判死。
  checkLiveness() {
    if (!this.ws) return;
    this._heartbeatTick(this.ws, { idleMs: HEARTBEAT_VISIBLE_IDLE_MS });
  }

  // 连接已死但浏览器没触发 onclose:摘掉旧 socket 的回调后直接新建连接。不能只调
  // ws.close() 等 onclose —— 对端无响应时关闭握手要很久才超时。
  _forceReconnect(reason) {
    const ws = this.ws;
    if (!ws) return;
    const now = Date.now();
    const msSinceMessage = this.lastMessageAt ? now - this.lastMessageAt : null;
    this._stopHeartbeat();
    try { ws.onopen = ws.onmessage = ws.onclose = ws.onerror = null; } catch {}
    closeWs(ws, reason);
    this.ws = null;
    this.heartbeatTimeouts += 1;
    this.lastForcedReconnect = { reason, at: now, msSinceMessage };
    // eslint-disable-next-line no-console
    console.warn(`[ace-ws] forced reconnect reason=${reason} quiet_ms=${msSinceMessage ?? '?'}`);
    this.dispatchEvent(new CustomEvent('disconnect', { detail: { delay: 0, reason } }));
    this._reportDiagnostics({
      kind: 'heartbeat_timeout',
      reason,
      ...(msSinceMessage !== null ? { ws_ms_since_message: msSinceMessage } : {}),
      heartbeat_timeouts: this.heartbeatTimeouts,
      ...(typeof document !== 'undefined' && document.visibilityState
        ? { visibility: document.visibilityState }
        : {}),
    });
    setTimeout(() => {
      if (!this.ws) this._open();
    }, 0);
  }

  // 诊断上报出口由应用层注入(App 挂载时接到 /api/diagnostics/transcript-stall),
  // 连接层自己不发 HTTP。reporter 收到 (payload, { port, token })。
  setDiagnosticsReporter(reporter) {
    this.diagnosticsReporter = typeof reporter === 'function' ? reporter : null;
  }

  _reportDiagnostics(payload) {
    if (!this.diagnosticsReporter) return;
    try {
      const result = this.diagnosticsReporter(payload, { port: this._port, token: this._token });
      result?.catch?.(() => {});
    } catch {}
  }

  // 强制按游标重新订阅:服务端先退订旧订阅再回放 since 之后的事件(replay_from_start)。
  // 不改引用计数;连接不在时由 onopen 统一补发。epoch 是调用方刚从服务端读到的分发器
  // 实例号,先记下来,免得紧接着的订阅回执再判一次「实例变了」、重复对账。
  resubscribe(sessionId, { since, epoch } = {}) {
    const state = this.sessions.get(sessionId);
    if (!state) return false;
    if (since !== undefined) {
      state.lastSeq = Math.max(0, Number(since) || 0);
      writeSessionStorage(seqStorageKey(sessionId), String(state.lastSeq));
    }
    if (typeof epoch === 'string' && epoch) {
      state.epoch = epoch;
      writeSessionStorage(epochStorageKey(sessionId), epoch);
    }
    state.replayFromStart = true;
    if (this.ws && this.ws.readyState === WebSocket.OPEN) this._sendSubscribe(sessionId);
    else if (!this.ws || this.ws.readyState === WebSocket.CLOSED) this._open();
    return true;
  }

  sessionCursor(sessionId) {
    const state = this.sessions.get(sessionId);
    if (state) return { lastSeq: state.lastSeq || 0, epoch: state.epoch || '' };
    return storedCursor(sessionId);
  }

  liveness(now = Date.now()) {
    return {
      wsState: webSocketStateName(this.ws),
      msSinceMessage: this.lastMessageAt ? now - this.lastMessageAt : null,
      heartbeatTimeouts: this.heartbeatTimeouts,
      msSinceForcedReconnect: this.lastForcedReconnect ? now - this.lastForcedReconnect.at : null,
    };
  }

  sendUserInput(text, sessionId=this.sessionId) { this._send({ type: 'user_input', payload: { session_id: sessionId, text } }); }
  sendDecision(request_id, choice, sessionId=this.sessionId) { this._send({ type: 'decision', payload: { session_id: sessionId, request_id, choice } }); }
  sendQuestionAnswer(payload)      { this._send({ type: 'question_answer', payload }); }
  sendAbort(sessionId=this.sessionId) { return this._send({ type: 'abort', payload: { session_id: sessionId } }); }
  markSessionRead({ sessionId=this.sessionId, workspaceHash='', cursor=0 } = {}) {
    if (!sessionId) return false;
    return this._send({
      type: 'mark_session_read',
      payload: { session_id: sessionId, workspace_hash: workspaceHash || '', cursor: cursor || 0 },
    });
  }
  markSessionUnread({ sessionId=this.sessionId, workspaceHash='' } = {}) {
    if (!sessionId) return false;
    return this._send({
      type: 'mark_session_unread',
      payload: { session_id: sessionId, workspace_hash: workspaceHash || '' },
    });
  }
  ping()                           { this._send({ type: 'ping' }); }

  _send(msg) {
    if (!this.ws || this.ws.readyState !== WebSocket.OPEN) return false;
    try {
      this.ws.send(JSON.stringify(msg));
      return true;
    } catch {
      return false;
    }
  }
}

export const connection = new AceConnection();
