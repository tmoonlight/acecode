// 实时连接健康度的纯逻辑:WS 心跳判定与订阅游标核对。connection.js 负责计时器与
// socket,这里只给出决定,便于 Node 单测覆盖各个时间边界。
//
// 背景:页面一直显示「正在推理」、切走再切回才看到回合已完成 —— daemon 早已收尾,
// 页面却没处理 busy_changed(false) / done。两条与连接相关的成因在这里兜底:
//   1. 连接已经不通但没触发 onclose:前端从不发心跳,永远不会重连;
//   2. 服务端事件分发器换了实例(daemon 重启 / 会话运行实体重建),seq 从 1 重新数,
//      前端手里的旧 seq 更大,新事件全被当成过期帧丢掉。

// 心跳检查节拍。后台标签页里计时器会被浏览器节流到分钟级,判定只依赖时间差,
// 晚到的检查不会误判。
export const HEARTBEAT_TICK_MS = 5000;
// 连接上这么久没有任何消息就发一次 ping。
export const HEARTBEAT_IDLE_MS = 20000;
// ping 发出后这么久仍没有任何消息(pong 或别的帧)就判定连接已死,主动重连。
export const HEARTBEAT_TIMEOUT_MS = 10000;
// 页面重新可见时,只要连接静默超过这个时长就立即补一次 ping,尽快发现死连接。
export const HEARTBEAT_VISIBLE_IDLE_MS = 5000;

// 返回 'none' | 'ping' | 'wait' | 'reconnect'。
// - 有未答复的 ping(发出之后没有收到任何消息):超时则 'reconnect',否则 'wait';
// - 没有未答复的 ping:静默达到 idleMs 发 'ping'。
// 任何消息都算连接存活的证据,不要求必须是 pong。
export function heartbeatAction({
  now,
  lastMessageAt = 0,
  pingSentAt = 0,
  idleMs = HEARTBEAT_IDLE_MS,
  timeoutMs = HEARTBEAT_TIMEOUT_MS,
} = {}) {
  const current = Number(now) || 0;
  const lastMessage = Number(lastMessageAt) || 0;
  const pingAt = Number(pingSentAt) || 0;
  if (pingAt > 0 && lastMessage < pingAt) {
    return current - pingAt >= timeoutMs ? 'reconnect' : 'wait';
  }
  return current - lastMessage >= idleMs ? 'ping' : 'none';
}

function cursorEpoch(value) {
  return typeof value === 'string' ? value.trim() : '';
}

function cursorSeq(value) {
  if (value === null || value === undefined || value === '') return null;
  const parsed = Number(value);
  return Number.isFinite(parsed) && parsed >= 0 ? parsed : null;
}

// 订阅回执(subscribe_ack / hello_ack)带回服务端当前 seq 与分发器实例号时,核对
// 本地保存的游标。返回 { reset, reason, epoch }:
// - 实例号与本地记录不同 → reset(epoch_changed):旧 seq 属于已经不存在的实例;
// - 本地 seq 比服务端当前 seq 还大 → reset(seq_regressed):同上,只是本地还没记过实例号
//   (例如 sessionStorage 里留着上一次 daemon 运行时的 seq);
// - 其它情况不重置,epoch 返回应当保存的实例号(服务端没给就沿用本地的)。
export function evaluateSubscriptionCursor({
  storedSeq = 0,
  storedEpoch = '',
  serverSeq = null,
  serverEpoch = '',
} = {}) {
  const local = Number(storedSeq) || 0;
  const localEpoch = cursorEpoch(storedEpoch);
  const remoteEpoch = cursorEpoch(serverEpoch);
  const remoteSeq = cursorSeq(serverSeq);
  const epoch = remoteEpoch || localEpoch;
  if (remoteEpoch && localEpoch && remoteEpoch !== localEpoch) {
    return { reset: true, reason: 'epoch_changed', epoch };
  }
  if (remoteSeq !== null && local > remoteSeq) {
    return { reset: true, reason: 'seq_regressed', epoch };
  }
  return { reset: false, reason: '', epoch };
}

export function webSocketStateName(ws) {
  if (!ws) return 'none';
  switch (ws.readyState) {
    case 0: return 'connecting';
    case 1: return 'open';
    case 2: return 'closing';
    case 3: return 'closed';
    default: return 'unknown';
  }
}
