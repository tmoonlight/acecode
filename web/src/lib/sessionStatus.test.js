// Sidebar session attention 状态合并测试。

import assert from 'node:assert/strict';
import {
  applyStatusSnapshot,
  applyStatusUpdate,
  mergeSessionStatus,
  optimisticReadStatus,
  optimisticUnreadStatus,
  sessionAttentionState,
  workspaceHasUnread,
} from './sessionStatus.js';

run('手动未读在快照中恢复，并在显式已读后清除', () => {
  const snapshot = applyStatusSnapshot(new Map(), { sessions: [
    { session_id: 'restored', state: 'unread', manual_unread: true, cursor: 5, read_cursor: 4 },
    { session_id: 'legacy', state: 'unread', cursor: 3 },
  ] });
  assert.equal(mergeSessionStatus({ id: 'restored' }, snapshot).manual_unread, true);
  assert.equal(snapshot.get('legacy').manual_unread, false);
  assert.equal(optimisticUnreadStatus({ id: 'new' }).manual_unread, true);
  assert.equal(optimisticReadStatus(snapshot.get('restored')).manual_unread, false);
  assert.equal(optimisticReadStatus({ id: 'busy', busy: true, manual_unread: true }).manual_unread, false);
});

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('缺失状态字段默认视为已读', () => {
  assert.equal(sessionAttentionState({ id: 's1' }), 'read');
});

run('running/busy 优先显示进行中', () => {
  assert.equal(sessionAttentionState({ id: 's1', attention_state: 'unread', busy: true }), 'in_progress');
  assert.equal(sessionAttentionState({ id: 's1', status: 'running' }), 'in_progress');
});

run('WebSocket snapshot 合并到状态 map', () => {
  const map = applyStatusSnapshot(new Map(), {
    workspace_hash: 'w1',
    sessions: [
      { session_id: 's1', workspace_hash: 'w1', state: 'unread', cursor: 4 },
      { session_id: 's2', workspace_hash: 'w1', state: 'read', cursor: 1 },
    ],
  });
  assert.equal(map.get('s1').attention_state, 'unread');
  assert.equal(map.get('s1').status_cursor, 4);
  assert.equal(map.get('s2').attention_state, 'read');
});

run('增量状态覆盖 HTTP session 字段', () => {
  const map = applyStatusUpdate(new Map(), {
    session_id: 's1',
    workspace_hash: 'w1',
    state: 'unread',
    cursor: 9,
    timestamp_ms: 100,
  });
  const merged = mergeSessionStatus({ id: 's1', status: 'idle', attention_state: 'read' }, map);
  assert.equal(merged.attention_state, 'unread');
  assert.equal(merged.status_cursor, 9);
});

run('WS idle 状态覆盖 HTTP running 旧值', () => {
  const map = applyStatusUpdate(new Map(), {
    session_id: 's1',
    state: 'read',
    busy: false,
    timestamp_ms: 100,
  });
  const merged = mergeSessionStatus({ id: 's1', status: 'running', busy: true }, map);
  assert.equal(merged.attention_state, 'read');
  assert.equal(merged.status, 'idle');
});

run('旧增量不会覆盖较新的状态', () => {
  let map = applyStatusUpdate(new Map(), { session_id: 's1', state: 'unread', timestamp_ms: 200 });
  map = applyStatusUpdate(map, { session_id: 's1', state: 'read', timestamp_ms: 100 });
  assert.equal(map.get('s1').attention_state, 'unread');
});

run('项目未读只由 child unread 聚合', () => {
  assert.equal(workspaceHasUnread([{ id: 's1', attention_state: 'read' }, { id: 's2', attention_state: 'in_progress' }]), false);
  assert.equal(workspaceHasUnread([{ id: 's1', attention_state: 'unread' }]), true);
});

// 场景:会话右键「标记为未读」,本地先乐观更新再等 daemon 回执。期望:与 daemon
// mark_session_attention_unread 同一口径 —— 已读游标退到最新输出之前,状态变 unread;
// 从没有输出(游标 0)的会话也能标成未读;乐观状态带当前时间戳,不会被更旧的广播盖掉。
run('标记为未读的乐观状态与 daemon 游标口径一致', () => {
  const read = mergeSessionStatus({ id: 's1', attention_state: 'read', update_cursor: 50, read_cursor: 50, status_cursor: 50 }, new Map());
  const unread = optimisticUnreadStatus(read);
  assert.equal(unread.attention_state, 'unread');
  assert.equal(unread.update_cursor, 50);
  assert.equal(unread.read_cursor, 49);
  assert.ok(unread.timestamp_ms > 0);

  const fresh = optimisticUnreadStatus({ id: 's2' });
  assert.equal(fresh.attention_state, 'unread');
  assert.equal(fresh.update_cursor, 1);
  assert.equal(fresh.read_cursor, 0);

  // 再标记已读又回到 read。
  const map = applyStatusUpdate(new Map(), unread);
  const back = optimisticReadStatus(mergeSessionStatus({ id: 's1' }, map));
  assert.equal(back.attention_state, 'read');
});

// 场景:运行中的会话被标记为未读。期望:显示仍是运行中(in_progress 优先),
// 不会把运行中的转圈图标换成未读圆点。
run('运行中的会话标记为未读仍显示运行中', () => {
  const unread = optimisticUnreadStatus({ id: 's1', busy: true, update_cursor: 9, read_cursor: 9 });
  assert.equal(unread.attention_state, 'in_progress');
  assert.equal(unread.read_cursor, 8);
});
