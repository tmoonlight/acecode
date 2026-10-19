// 实时 transcript 卡住检测的纯逻辑。useSessionTranscript 负责计时器、探针请求与
// 对账提交,这里只回答三个问题:现在该不该探一次、探回来之后要不要对账、上报什么。
//
// 背景:页面一直显示「正在推理」,daemon 早已收尾,切走再切回才看到完成 —— 页面
// 没处理这一回合的 busy_changed(false) / done。既有的「完成回合自愈」只在看到
// busy→false 之后才触发,恰好覆盖不到「根本没看到收尾」。这里不追究丢帧原因(连接
// 假活、游标作废、帧被前端丢弃都可能),只要页面认为在跑、却很久没收到事件,就向
// 服务端核对一次,对不上就按服务端重建,等同于自动「来回切一下」。

// 周期检查的节拍;是否真的发探针由 shouldProbeTranscript 按时间差决定。
export const STALL_CHECK_TICK_MS = 5000;
// 运行中连续这么久没收到本会话的任何事件,才去问服务端。长时间无输出的工具
// (sleep、慢 MCP)期间服务端仍是 busy 且 seq 不动,探针只确认、不动界面。
export const STALL_PROBE_AFTER_MS = 20000;
// 两次计时器触发的探针之间的最短间隔。
export const STALL_PROBE_INTERVAL_MS = 20000;
// 页面重新可见 / 获得焦点时的门槛更低:用户正看着,尽快纠正。
export const VISIBLE_PROBE_AFTER_MS = 4000;
export const VISIBLE_PROBE_INTERVAL_MS = 3000;

function finiteSeq(value) {
  if (value === null || value === undefined || value === '') return null;
  const parsed = Number(value);
  return Number.isFinite(parsed) && parsed >= 0 ? parsed : null;
}

function text(value) {
  return typeof value === 'string' ? value.trim() : '';
}

export function shouldProbeTranscript({
  trigger = 'tick',
  now = 0,
  live = false,
  running = false,
  probing = false,
  lastEventAt = 0,
  lastProbeAt = 0,
} = {}) {
  if (!live || !running || probing) return false;
  const quietMs = now - (Number(lastEventAt) || 0);
  const sinceProbeMs = now - (Number(lastProbeAt) || 0);
  if (trigger === 'visible') {
    return quietMs >= VISIBLE_PROBE_AFTER_MS && sinceProbeMs >= VISIBLE_PROBE_INTERVAL_MS;
  }
  return quietMs >= STALL_PROBE_AFTER_MS && sinceProbeMs >= STALL_PROBE_INTERVAL_MS;
}

const none = (reason) => ({ action: 'none', reason, key: '' });

// 探针结果 → 动作。action:
//   'none'   不动(本来就没在跑 / 服务端仍在忙且没有新事件 / 同一状态已经对过账);
//   'resync' 按服务端重拉历史尾页并从当前游标重新订阅;
//   'reset'  先把本地游标清零再 resync —— 服务端分发器换了实例,本地 seq 已作废。
// key 标识「已经为这个服务端状态对过账」,同一 key 不重复对账,避免对账无效时每
// 20 秒循环一次。
export function decideTranscriptStallAction({
  clientRunning = false,
  clientLastSeq = 0,
  clientEpoch = '',
  server = null,
  lastResyncKey = '',
} = {}) {
  if (!clientRunning) return none('client_idle');
  if (!server || typeof server !== 'object') return none('probe_failed');
  const serverSeq = finiteSeq(server.event_seq);
  const serverEpoch = text(server.event_epoch);
  const localEpoch = text(clientEpoch);
  const localSeq = Number(clientLastSeq) || 0;
  const decide = (action, reason) => {
    const key = `${reason}:${serverEpoch}:${serverSeq ?? ''}`;
    if (key === lastResyncKey) return none('already_resynced');
    return { action, reason, key };
  };
  if (serverEpoch && localEpoch && serverEpoch !== localEpoch) return decide('reset', 'epoch_changed');
  if (serverSeq !== null && localSeq > serverSeq) return decide('reset', 'seq_regressed');
  if (server.busy !== true) {
    return decide('resync', server.active === false ? 'server_unloaded' : 'server_idle');
  }
  if (serverSeq !== null && serverSeq > localSeq) return decide('resync', 'missed_events');
  return none('quiet_turn');
}

const TOKEN = /^[A-Za-z0-9\-_:.]+$/;

function token(value, maxLength) {
  const raw = typeof value === 'string' ? value.trim() : '';
  if (!raw || raw.length > maxLength || !TOKEN.test(raw)) return undefined;
  return raw;
}

function count(value) {
  // null / 空串表示「拿不到」,不能被 Number() 变成 0 混进诊断(0 是有意义的取值)。
  if (value === null || value === undefined || value === '') return undefined;
  const parsed = Number(value);
  return Number.isFinite(parsed) && parsed >= 0 ? Math.trunc(parsed) : undefined;
}

// 组装 POST /api/diagnostics/transcript-stall 的请求体。daemon 端按同一份白名单
// 校验,字符串不合规直接 400,所以这里先按同样的规则丢掉不合规的值。
export function buildTranscriptStallReport({
  kind = 'stall_probe',
  sessionId = '',
  trigger = '',
  decision = null,
  client = {},
  server = null,
  liveness = null,
  visibility = '',
  msSinceEvent = null,
} = {}) {
  const report = {
    kind,
    session_id: token(sessionId, 128),
    reason: token(decision?.reason, 48),
    action: token(decision?.action, 32),
    trigger: token(trigger, 32),
    ms_since_event: count(msSinceEvent),
    client_running: typeof client.running === 'boolean' ? client.running : undefined,
    client_last_seq: count(client.lastSeq),
    client_epoch: token(client.epoch, 64),
    client_active_turn_id: token(client.activeTurnId, 128),
    server_active: typeof server?.active === 'boolean' ? server.active : undefined,
    server_busy: typeof server?.busy === 'boolean' ? server.busy : undefined,
    server_event_seq: count(server?.event_seq),
    server_epoch: token(server?.event_epoch, 64),
    server_active_turn_id: token(server?.active_turn_id, 128),
    ws_state: token(liveness?.wsState, 16),
    ws_ms_since_message: count(liveness?.msSinceMessage),
    heartbeat_timeouts: count(liveness?.heartbeatTimeouts),
    ms_since_forced_reconnect: count(liveness?.msSinceForcedReconnect),
    visibility: token(visibility, 16),
  };
  for (const key of Object.keys(report)) {
    if (report[key] === undefined) delete report[key];
  }
  return report;
}
