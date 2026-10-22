import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { sessionBatchItems, sessionBatchOutcome, sessionBatchScopes } from './sidebarSessionBatch.js';

function test(name, fn) {
  try { fn(); console.log('ok -', name); }
  catch (error) { console.error('not ok -', name); throw error; }
}

const workspaceEntry = { key: 'ws1\u0000a', pinned: false, session: { id: 'a', workspace_hash: 'ws1' } };
const taskEntry = { key: '__no_workspace__\u0000t', pinned: true, session: { id: 't', no_workspace: true, workspace_hash: '' } };
const otherEntry = { key: 'ws2\u0000b', pinned: false, session: { id: 'b', workspaceHash: 'ws2' } };

// 场景:多选三个会话(工作区会话、无工作区任务、另一个工作区)后批量操作。
// 期望:请求条目保持侧栏可见顺序;无工作区任务的 workspace_hash 置空并带 no_workspace;
// 只有 read 带客户端已看到的游标,游标为 0 / 非法时不发。
test('批量请求条目保持可见顺序并区分无工作区任务', () => {
  assert.deepEqual(sessionBatchItems('pin', [workspaceEntry, taskEntry, otherEntry]), [
    { session_id: 'a', workspace_hash: 'ws1', no_workspace: false },
    { session_id: 't', workspace_hash: '', no_workspace: true },
    { session_id: 'b', workspace_hash: 'ws2', no_workspace: false },
  ]);
  const cursors = { a: 12, t: 0 };
  const items = sessionBatchItems('read', [workspaceEntry, taskEntry], { cursorFor: session => cursors[session.id] });
  assert.equal(items[0].cursor, 12);
  assert.equal('cursor' in items[1], false);
  assert.equal('cursor' in sessionBatchItems('unread', [workspaceEntry], { cursorFor: () => 5 })[0], false);
});

// 场景:服务端返回部分成功(b 不存在),并带回置顶作用域与全局顺序。
// 期望:按「作用域 + id」把结果对齐到 entry,失败项的 key 用来保留选中;置顶数据规范化后返回。
test('批量结果按作用域与 id 对齐到选中项', () => {
  const outcome = sessionBatchOutcome({
    results: [
      { session_id: 'a', workspace_hash: 'ws1', no_workspace: false, ok: true },
      { session_id: 't', workspace_hash: '', no_workspace: true, ok: true, status: { session_id: 't', attention_state: 'read' } },
      { session_id: 'b', workspace_hash: 'ws2', no_workspace: false, ok: false, error: 'session cannot be pinned' },
    ],
    pinned_scopes: [{ pin_scope: 'ws1', session_ids: ['a', 'a', 'x'] }],
    pinned_order: { items: [{ workspace_hash: 'ws1', session_id: 'a' }] },
  }, [workspaceEntry, taskEntry, otherEntry]);
  assert.deepEqual(outcome.succeeded.map(({ entry }) => entry.session.id), ['a', 't']);
  assert.deepEqual(outcome.failedKeys, ['ws2\u0000b']);
  assert.equal(outcome.failed[0].error, 'session cannot be pinned');
  assert.deepEqual([...outcome.pinnedScopes.entries()], [['ws1', ['a', 'x']]]);
  assert.deepEqual(outcome.pinnedOrder, [{ workspace_hash: 'ws1', session_id: 'a' }]);
  assert.deepEqual(outcome.statuses, [{ session_id: 't', attention_state: 'read' }]);
});

// 场景:服务端漏回了某一项(或旧 daemon 返回了不认识的格式)。
// 期望:缺失的项按失败处理,界面不会把没处理的会话当成功;没有 pinned_order 时为 null(不改顺序)。
test('服务端漏回的条目算失败', () => {
  const outcome = sessionBatchOutcome({ results: [{ session_id: 'a', workspace_hash: 'ws1', ok: true }] }, [workspaceEntry, otherEntry]);
  assert.deepEqual(outcome.failedKeys, ['ws2\u0000b']);
  assert.equal(outcome.pinnedOrder, null);
  assert.equal(sessionBatchOutcome(null, [workspaceEntry]).failed.length, 1);
});

test('批量涉及的作用域去重', () => {
  assert.deepEqual(sessionBatchScopes([workspaceEntry, taskEntry, { session: { id: 'c', workspace_hash: 'ws1' } }]), ['ws1', '__no_workspace__']);
});

// 回归:批量操作曾逐个调用单项接口(置顶每项 2 个请求,归档每项还要刷新一次列表)。
// 守住 Sidebar 的批量入口只发一次 batchSessions,不再循环调用单项函数。
test('Sidebar 批量操作只走一次批量接口', () => {
  const sidebar = readFileSync(fileURLToPath(new URL('../components/Sidebar.jsx', import.meta.url)), 'utf8');
  const start = sidebar.indexOf('const runSessionBatch = useStableCallback(');
  assert.ok(start > 0);
  const body = sidebar.slice(start, sidebar.indexOf('const handleSelectionContextMenu', start));
  assert.match(body, /api\.batchSessions\(action, sessionBatchItems\(action, entries/);
  assert.doesNotMatch(body, /togglePinnedSession\(|archiveSession\(|markSessionUnread\(|markSessionRead\(/);
  assert.doesNotMatch(body, /for \(const entry of/);
});
