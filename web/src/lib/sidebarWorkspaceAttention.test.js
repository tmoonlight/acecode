// 工作区行「新建任务」位上的汇总状态(进行中转圈 / 未读绿点)测试。

import assert from 'node:assert/strict';
import fs from 'node:fs';
import { applyStatusSnapshot, applyStatusUpdate } from './sessionStatus.js';
import {
  applyWorkspaceStatusMembership,
  removeWorkspaceStatusMembers,
  workspaceAttentionIndicator,
  workspaceAttentionSummary,
} from './sidebarWorkspaceAttention.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

const source = (path) => fs.readFileSync(new URL(`../${path}`, import.meta.url), 'utf8');

function snapshot(workspaceHash, sessions) {
  return { type: 'session_status_snapshot', workspace_hash: workspaceHash, payload: { workspace_hash: workspaceHash, sessions } };
}

// 触发场景:关掉 ACECode 再打开,工作区 A 是折叠的,侧栏不给它加载会话行(items 为空),
// 但 daemon 订阅状态时推来的快照里 A 有一个未读任务。
// 期望行为:汇总出 unread,工作区行照样显示绿点。
// 回归:旧实现只看已加载的会话行,冷启动时折叠工作区的绿点不显示,展开过一次后才出现。
run('冷启动时折叠工作区没有会话行,靠状态快照汇总出未读', () => {
  const message = snapshot('A', [
    { session_id: 's1', workspace_hash: 'A', state: 'read' },
    { session_id: 's2', workspace_hash: 'A', state: 'unread' },
  ]);
  const members = applyWorkspaceStatusMembership(new Map(), message);
  const statusMap = applyStatusSnapshot(new Map(), message.payload);
  const summary = workspaceAttentionSummary({ rows: [], memberIds: members.get('A'), statusMap });
  assert.deepEqual(summary, { inProgress: false, unread: true });
  assert.equal(workspaceAttentionIndicator(summary, { expanded: false }), 'unread');
});

// 触发场景:同一份状态 map 里还有别的工作区的未读任务。
// 期望行为:只汇总本工作区快照里的成员,不串到别的工作区。
run('只汇总本工作区的快照成员', () => {
  let members = applyWorkspaceStatusMembership(new Map(), snapshot('A', [{ session_id: 'a1', state: 'read' }]));
  members = applyWorkspaceStatusMembership(members, snapshot('B', [{ session_id: 'b1', state: 'unread' }]));
  const statusMap = applyStatusSnapshot(new Map(), { sessions: [
    { session_id: 'a1', state: 'read' },
    { session_id: 'b1', state: 'unread' },
  ] });
  assert.deepEqual(workspaceAttentionSummary({ memberIds: members.get('A'), statusMap }), { inProgress: false, unread: false });
  assert.deepEqual(workspaceAttentionSummary({ memberIds: members.get('B'), statusMap }), { inProgress: false, unread: true });
});

// 触发场景:折叠的工作区里一个任务在跑、另一个未读。
// 期望行为:折叠时显示转圈(进行中优先于绿点);展开后每行自己有转圈,工作区只留绿点。
run('进行中只在折叠时显示且优先于未读', () => {
  const summary = { inProgress: true, unread: true };
  assert.equal(workspaceAttentionIndicator(summary, { expanded: false }), 'in_progress');
  assert.equal(workspaceAttentionIndicator(summary, { expanded: true }), 'unread');
  assert.equal(workspaceAttentionIndicator({ inProgress: true, unread: false }, { expanded: true }), '');
  assert.equal(workspaceAttentionIndicator({ inProgress: false, unread: false }, { expanded: false }), '');
});

// 触发场景:已加载的会话行里有一行正在运行(行状态已合并 statusBySession)。
// 期望行为:没有快照(例如快照还没到)也能从会话行汇总出进行中。
run('已加载的会话行照常参与汇总', () => {
  const summary = workspaceAttentionSummary({ rows: [
    { id: 'r1', attention_state: 'read' },
    { id: 'r2', busy: true },
  ] });
  assert.deepEqual(summary, { inProgress: true, unread: false });
});

// 触发场景:快照里的未读任务在本工作区被置顶,它显示在「置顶」分组里。
// 期望行为:不再算进工作区汇总,和会话行(已过滤掉置顶)同一口径。
run('置顶会话不计入工作区汇总', () => {
  const members = applyWorkspaceStatusMembership(new Map(), snapshot('A', [{ session_id: 'p1', state: 'unread' }]));
  const statusMap = applyStatusSnapshot(new Map(), { sessions: [{ session_id: 'p1', state: 'unread' }] });
  assert.equal(workspaceAttentionSummary({ memberIds: members.get('A'), statusMap, pinnedIds: ['p1'] }).unread, false);
});

// 触发场景:快照之后在折叠的工作区里新建了任务(例如定时任务、消息通道发来的消息),
// 它只以单条 session_status 出现。
// 期望行为:补进该工作区的成员;已在成员里的再来一条状态不产生新的 Map(不触发重渲染)。
run('快照之后新建的任务从单条状态补进成员', () => {
  const members = applyWorkspaceStatusMembership(new Map(), snapshot('A', [{ session_id: 's1' }]));
  const added = applyWorkspaceStatusMembership(members, {
    type: 'session_status', session_id: 'new', workspace_hash: 'A',
    payload: { session_id: 'new', workspace_hash: 'A', state: 'in_progress', busy: true },
  });
  assert.deepEqual([...added.get('A')].sort(), ['new', 's1']);
  const again = applyWorkspaceStatusMembership(added, {
    type: 'session_status', payload: { session_id: 'new', workspace_hash: 'A', state: 'read' },
  });
  assert.equal(again, added);
});

// 触发场景:还没收到某个工作区的快照时,先来了一条它的单条状态。
// 期望行为:不凭单条状态建立成员表,等快照到了一并算(快照才知道完整成员)。
run('没收到快照的工作区不凭单条状态建立成员', () => {
  const members = new Map();
  const next = applyWorkspaceStatusMembership(members, {
    type: 'session_status', payload: { session_id: 'x', workspace_hash: 'A', state: 'unread' },
  });
  assert.equal(next, members);
});

// 触发场景:父会话派出的子代理在跑,它的状态带 parent_session_id,workspace_hash 与父会话相同。
// 期望行为:子代理不是侧栏任务,不计入工作区;若之前被误记成员也要移除。
run('子代理会话不计入工作区成员', () => {
  const members = applyWorkspaceStatusMembership(new Map(), snapshot('A', [
    { session_id: 'parent' },
    { session_id: 'child', parent_session_id: 'parent' },
  ]));
  assert.deepEqual([...members.get('A')], ['parent']);
  const withStale = new Map(members).set('A', new Set(['parent', 'child2']));
  const cleaned = applyWorkspaceStatusMembership(withStale, {
    type: 'session_status', parent_session_id: 'parent',
    payload: { session_id: 'child2', workspace_hash: 'A', busy: true, parent_session_id: 'parent' },
  });
  assert.deepEqual([...cleaned.get('A')], ['parent']);
  const ignored = applyWorkspaceStatusMembership(members, {
    type: 'session_status', payload: { session_id: 'child3', workspace_hash: 'A', parent_session_id: 'parent' },
  });
  assert.equal(ignored, members);
});

// 触发场景:用户在侧栏归档了一个未读任务;daemon 不会为归档推状态。
// 期望行为:从成员表移除,工作区绿点随之消失;移除不存在的 id 返回同一个 Map。
run('归档后移除成员', () => {
  const members = applyWorkspaceStatusMembership(new Map(), snapshot('A', [{ session_id: 'u1' }, { session_id: 'r1' }]));
  const statusMap = applyStatusSnapshot(new Map(), { sessions: [
    { session_id: 'u1', state: 'unread' },
    { session_id: 'r1', state: 'read' },
  ] });
  const removed = removeWorkspaceStatusMembers(members, ['u1']);
  assert.deepEqual([...removed.get('A')], ['r1']);
  assert.equal(workspaceAttentionSummary({ memberIds: removed.get('A'), statusMap }).unread, false);
  assert.equal(removeWorkspaceStatusMembers(removed, ['missing']), removed);
});

// 触发场景:WebSocket 重连后 daemon 重发同样内容的快照。
// 期望行为:成员没变就返回同一个 Map(侧栏挂机时不能每次都生成新状态,见 structuralState.js)。
run('内容相同的快照不生成新 Map', () => {
  const message = snapshot('A', [{ session_id: 's1' }, { session_id: 's2' }]);
  const members = applyWorkspaceStatusMembership(new Map(), message);
  assert.equal(applyWorkspaceStatusMembership(members, message), members);
  assert.equal(applyWorkspaceStatusMembership(members, { type: 'session_updated', payload: {} }), members);
});

// 触发场景:会话行已加载(行上的状态已合并 statusBySession),成员表里也有同一个 id。
// 期望行为:同一 id 只按会话行算一次,不再到状态 map 里重复取。
run('同一会话以会话行状态为准', () => {
  const members = applyWorkspaceStatusMembership(new Map(), snapshot('A', [{ session_id: 's1' }]));
  const statusMap = applyStatusUpdate(new Map(), { session_id: 's1', state: 'unread', timestamp_ms: 1 });
  const summary = workspaceAttentionSummary({
    rows: [{ id: 's1', attention_state: 'read' }],
    memberIds: members.get('A'),
    statusMap,
  });
  assert.equal(summary.unread, false);
});

// 触发场景:工作区行的界面结构。
// 期望行为:汇总状态叠在新建任务按钮的同一位置(不另占一格),悬停 / 键盘聚焦时让位给按钮;
// 全部工作区都订阅状态,折叠的工作区才收得到快照。
run('汇总状态与新建任务按钮共用一个位置', () => {
  const sidebar = source('components/Sidebar.jsx');
  const css = source('styles/globals.css');
  const groupStart = sidebar.indexOf('function WorkspaceGroup({');
  const groupEnd = sidebar.indexOf('\nfunction NoWorkspaceSessionGroup(', groupStart);
  const group = sidebar.slice(groupStart, groupEnd);
  const slotStart = group.indexOf('<span className="relative flex shrink-0">');
  const indicator = group.indexOf('data-sidebar-workspace-attention={attentionIndicator}', slotStart);
  const newTask = group.indexOf('data-sidebar-workspace-new-task="true"', slotStart);
  assert.ok(slotStart > 0 && indicator > slotStart && newTask > indicator);
  assert.match(group.slice(indicator, newTask), /pointer-events-none absolute inset-0/);
  // 鼠标点行折叠后焦点留在行上;按钮若跟 :focus-within 显示,汇总状态会一直被盖住。
  assert.doesNotMatch(group, /group-focus-within:opacity-100/);
  assert.match(css, /\.ace-sidebar-workspace-row:is\(:hover, :focus-visible, :has\(:focus-visible\)\) \.ace-sidebar-workspace-attention \{\s*opacity: 0;/);
  assert.match(css, /\.ace-sidebar-workspace-row:is\(:focus-visible, :has\(:focus-visible\)\) \.ace-sidebar-workspace-action \{\s*opacity: 1;/);
  assert.match(sidebar, /withActive\.forEach\(\(w\) => connection\.subscribeWorkspaceStatus\(w\.hash\)\);/);
  assert.match(sidebar, /setWorkspaceStatusMembers\(\(prev\) => applyWorkspaceStatusMembership\(prev, msg\)\);/);
});
