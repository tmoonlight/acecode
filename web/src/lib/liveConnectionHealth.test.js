import assert from 'node:assert/strict';
import {
  HEARTBEAT_IDLE_MS,
  HEARTBEAT_TIMEOUT_MS,
  evaluateSubscriptionCursor,
  heartbeatAction,
  webSocketStateName,
} from './liveConnectionHealth.js';

// 场景:连接刚收到过消息。期望:不发 ping —— 有流量就证明连接活着,不额外打扰服务端。
assert.equal(heartbeatAction({ now: 10_000, lastMessageAt: 9_000, pingSentAt: 0 }), 'none');

// 场景:连接静默达到 20 秒且没有未答复的 ping。期望:发一次 ping 探活。
assert.equal(heartbeatAction({ now: 30_000, lastMessageAt: 30_000 - HEARTBEAT_IDLE_MS, pingSentAt: 0 }), 'ping');

// 场景:ping 已发出、还没收到任何消息,但未到 10 秒超时。期望:继续等,不重复发 ping。
assert.equal(heartbeatAction({ now: 35_000, lastMessageAt: 20_000, pingSentAt: 30_000 }), 'wait');

// 场景:ping 发出 10 秒仍没有任何消息 —— 连接已死但浏览器没触发 onclose,这正是
// 「界面一直正在推理、切走再切回才好」的成因之一。期望:判定重连。
assert.equal(
  heartbeatAction({ now: 30_000 + HEARTBEAT_TIMEOUT_MS, lastMessageAt: 20_000, pingSentAt: 30_000 }),
  'reconnect',
);

// 场景:ping 之后收到了别的帧(不一定是 pong)。期望:视为已答复,回到正常节奏;
// 后台标签页里计时器被节流到分钟级时,晚到的检查也不会误判。
assert.equal(heartbeatAction({ now: 90_000, lastMessageAt: 30_001, pingSentAt: 30_000 }), 'ping');
assert.equal(heartbeatAction({ now: 31_000, lastMessageAt: 30_001, pingSentAt: 30_000 }), 'none');
console.log('[pass] heartbeat pings an idle socket and reconnects only after an unanswered ping times out');

// 场景:第一次拿到带游标的回执,本地没有记录过实例号,本地 seq 不超过服务端。
// 期望:不重置,记下服务端实例号。
assert.deepEqual(
  evaluateSubscriptionCursor({ storedSeq: 5, storedEpoch: '', serverSeq: 9, serverEpoch: 'e1' }),
  { reset: false, reason: '', epoch: 'e1' },
);

// 场景:daemon 重启,新分发器实例号不同(即使新实例的 seq 已经超过旧值)。
// 期望:重置 —— 旧 seq 属于不存在的实例,继续用它会把新实例的事件当成过期帧丢掉。
assert.deepEqual(
  evaluateSubscriptionCursor({ storedSeq: 5, storedEpoch: 'e1', serverSeq: 900, serverEpoch: 'e2' }),
  { reset: true, reason: 'epoch_changed', epoch: 'e2' },
);

// 场景:sessionStorage 里留着上一次 daemon 运行的 seq(当时还没有实例号),新实例 seq 更小。
// 期望:按 seq 回退判定重置。
assert.deepEqual(
  evaluateSubscriptionCursor({ storedSeq: 4000, storedEpoch: '', serverSeq: 12, serverEpoch: 'e3' }),
  { reset: true, reason: 'seq_regressed', epoch: 'e3' },
);

// 场景:旧版 daemon 的回执不带游标字段。期望:什么都不改,沿用本地实例号。
assert.deepEqual(
  evaluateSubscriptionCursor({ storedSeq: 40, storedEpoch: 'e1' }),
  { reset: false, reason: '', epoch: 'e1' },
);
console.log('[pass] subscription ack resets the cursor when the dispatcher instance changed');

assert.equal(webSocketStateName(null), 'none');
assert.equal(webSocketStateName({ readyState: 1 }), 'open');
assert.equal(webSocketStateName({ readyState: 3 }), 'closed');
console.log('[pass] websocket state names are stable diagnostics tokens');
