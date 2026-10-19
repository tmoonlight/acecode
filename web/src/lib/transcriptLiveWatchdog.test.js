import assert from 'node:assert/strict';
import {
  STALL_PROBE_AFTER_MS,
  VISIBLE_PROBE_AFTER_MS,
  buildTranscriptStallReport,
  decideTranscriptStallAction,
  shouldProbeTranscript,
} from './transcriptLiveWatchdog.js';

const base = { live: true, running: true, probing: false, lastProbeAt: 0 };

// 场景:回合在跑,但只静默了 10 秒(模型思考、工具执行都可能这样)。
// 期望:周期检查不探针,避免对正常的安静期频繁请求。
assert.equal(shouldProbeTranscript({ ...base, now: 100_000, lastEventAt: 90_000 }), false);

// 场景:运行中连续 20 秒没收到本会话任何事件。期望:探一次服务端。
assert.equal(shouldProbeTranscript({ ...base, now: 100_000, lastEventAt: 100_000 - STALL_PROBE_AFTER_MS }), true);

// 场景:用户解锁屏幕 / 切回窗口,页面重新可见,静默已超过 4 秒。
// 期望:用更低的门槛立刻核对 —— 用户正看着「正在推理」,要尽快纠正。
assert.equal(shouldProbeTranscript({
  ...base, trigger: 'visible', now: 100_000, lastEventAt: 100_000 - VISIBLE_PROBE_AFTER_MS,
}), true);

// 场景:页面不在跑、不是实时会话,或上一次探针还没回来。期望:都不探。
assert.equal(shouldProbeTranscript({ ...base, running: false, now: 100_000, lastEventAt: 0 }), false);
assert.equal(shouldProbeTranscript({ ...base, live: false, now: 100_000, lastEventAt: 0 }), false);
assert.equal(shouldProbeTranscript({ ...base, probing: true, now: 100_000, lastEventAt: 0 }), false);

// 场景:20 秒前刚探过,服务端说还在忙(长时间无输出的工具)。期望:周期检查等满间隔再探。
assert.equal(shouldProbeTranscript({ ...base, now: 100_000, lastEventAt: 0, lastProbeAt: 90_000 }), false);
console.log('[pass] stall probe waits for real silence and fires sooner when the page becomes visible');

const client = { clientRunning: true, clientLastSeq: 120, clientEpoch: 'e1' };

// 场景:复现用户反馈 —— 页面停在「正在推理」,服务端回合早已结束(busy=false)。
// 期望:resync,按服务端重建(等同自动「来回切一下」)。
const idle = decideTranscriptStallAction({
  ...client, server: { active: true, busy: false, event_seq: 180, event_epoch: 'e1' },
});
assert.equal(idle.action, 'resync');
assert.equal(idle.reason, 'server_idle');

// 场景:会话已从 daemon 内存卸载,探针只回 active=false。期望:同样按空闲对账。
assert.equal(decideTranscriptStallAction({
  ...client, server: { active: false, busy: false },
}).reason, 'server_unloaded');

// 场景:服务端仍在忙,但 seq 已经走到页面没见过的位置 —— 事件流断了(连接假活 /
// 订阅丢了)。期望:resync 补齐。
assert.deepEqual(
  decideTranscriptStallAction({
    ...client, server: { active: true, busy: true, event_seq: 130, event_epoch: 'e1' },
  }).action,
  'resync',
);

// 场景:服务端仍在忙且 seq 与页面一致 —— 长时间无输出的工具(sleep、慢 MCP)。
// 期望:不动界面,不误判为卡住。
assert.equal(decideTranscriptStallAction({
  ...client, server: { active: true, busy: true, event_seq: 120, event_epoch: 'e1' },
}).reason, 'quiet_turn');

// 场景:daemon 重启 / 会话运行实体重建,分发器实例号变了,或服务端 seq 比页面还小。
// 期望:reset —— 先清零本地游标再对账,否则新实例的事件会被当成过期帧丢掉。
assert.equal(decideTranscriptStallAction({
  ...client, server: { active: true, busy: true, event_seq: 500, event_epoch: 'e2' },
}).reason, 'epoch_changed');
assert.equal(decideTranscriptStallAction({
  ...client, clientEpoch: '', server: { active: true, busy: true, event_seq: 3, event_epoch: 'e2' },
}).action, 'reset');

// 场景:同一个服务端状态已经对过一次账,页面仍判定在跑(对账没能纠正)。
// 期望:不再每 20 秒重复对账,避免空转;服务端状态变化(seq 前进)后才会再对。
assert.equal(decideTranscriptStallAction({
  ...client, lastResyncKey: idle.key, server: { active: true, busy: false, event_seq: 180, event_epoch: 'e1' },
}).reason, 'already_resynced');
assert.equal(decideTranscriptStallAction({
  ...client, lastResyncKey: idle.key, server: { active: true, busy: false, event_seq: 181, event_epoch: 'e1' },
}).action, 'resync');

// 场景:探针失败或页面其实已空闲。期望:不动。
assert.equal(decideTranscriptStallAction({ ...client, server: null }).reason, 'probe_failed');
assert.equal(decideTranscriptStallAction({ ...client, clientRunning: false, server: {} }).reason, 'client_idle');
console.log('[pass] stall decisions resync only when the server disagrees with the page');

// 场景:组装诊断上报。期望:字段名与 daemon 白名单一致;带空格、换行、超长的字符串在
// 前端就丢掉(否则整条上报会被 daemon 400 拒收),undefined 字段不出现在请求体里。
const report = buildTranscriptStallReport({
  sessionId: '20261019-101010-abcd',
  trigger: 'tick',
  decision: idle,
  msSinceEvent: 21_000.6,
  client: { running: true, lastSeq: 120, epoch: 'e1', activeTurnId: 'turn 1 with spaces' },
  server: { active: true, busy: false, event_seq: 180, event_epoch: 'e1' },
  liveness: { wsState: 'open', msSinceMessage: 500, heartbeatTimeouts: 0, msSinceForcedReconnect: null },
  visibility: 'visible',
});
assert.deepEqual(report, {
  kind: 'stall_probe',
  session_id: '20261019-101010-abcd',
  reason: 'server_idle',
  action: 'resync',
  trigger: 'tick',
  ms_since_event: 21000,
  client_running: true,
  client_last_seq: 120,
  client_epoch: 'e1',
  server_active: true,
  server_busy: false,
  server_event_seq: 180,
  server_epoch: 'e1',
  ws_state: 'open',
  ws_ms_since_message: 500,
  heartbeat_timeouts: 0,
  visibility: 'visible',
});
console.log('[pass] stall report keeps only allowlisted, log-safe fields');
