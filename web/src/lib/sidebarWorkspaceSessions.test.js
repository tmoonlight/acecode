import assert from 'node:assert/strict';
import { filterPinnedSessions } from './pinnedSessions.js';
import {
  SIDEBAR_SESSION_COLLAPSE_LIMIT,
  sidebarSessionProjection,
} from './sidebarSessions.js';
import {
  normalizeWorkspaceSessionListResponse,
  retainUnrefreshedSidebarSessions,
  settleSidebarWorkspacePage,
  sidebarWorkspacePageIsCurrent,
  sidebarWorkspaceSessionListQuery,
  workspaceHasCachedSidebarSessions,
  workspaceNeedsInitialSidebarLoad,
} from './sidebarWorkspaceSessions.js';

const pendingTests = [];

function test(name, fn) {
  const run = Promise.resolve().then(fn).then(() => {
    console.log(`[pass] ${name}`);
  }, (error) => {
    console.error(`[fail] ${name}`);
    throw error;
  });
  pendingTests.push(run);
}

// 触发场景:后端对无 limit 的请求仍回裸数组(老客户端兼容路径),对带 limit
// 的请求回信封。期望行为:两种形态都归一成同一个结构,裸数组视为已读完。
test('normalizeWorkspaceSessionListResponse accepts arrays and envelopes', () => {
  assert.deepEqual(normalizeWorkspaceSessionListResponse([{ id: 'a' }, { id: 'b' }]), {
    sessions: [{ id: 'a' }, { id: 'b' }],
    total: 2,
    totalExact: true,
    hasMore: false,
  });
  assert.deepEqual(normalizeWorkspaceSessionListResponse({
    sessions: [{ id: 'a' }],
    total: 9,
  }), {
    sessions: [{ id: 'a' }],
    total: 9,
    totalExact: true,
    hasMore: true,
  });
  assert.deepEqual(normalizeWorkspaceSessionListResponse({ sessions: [{ id: 'a' }] }), {
    sessions: [{ id: 'a' }],
    total: 1,
    totalExact: true,
    hasMore: false,
  });
  assert.deepEqual(normalizeWorkspaceSessionListResponse(null), {
    sessions: [],
    total: 0,
    totalExact: true,
    hasMore: false,
  });
});

// 触发场景:分页在读满一页后停止扫描目录,此时 total 是个偏大的上界。
// 期望行为:hasMore 直接采信服务端的 has_more,不再拿 sessions.length 与
// total 相比 —— 否则上界会把「还有更多」判成「已全部加载」的反面,展开时
// 该发的全量请求被跳过。
test('normalizeWorkspaceSessionListResponse trusts has_more over an inexact total', () => {
  const truncated = normalizeWorkspaceSessionListResponse({
    sessions: [{ id: 'a' }, { id: 'b' }],
    total: 1428,
    total_exact: false,
    has_more: true,
  });
  assert.equal(truncated.totalExact, false);
  assert.equal(truncated.hasMore, true);

  // 上界恰好等于已返回条数,但服务端明说还有更多:仍以 has_more 为准。
  const boundaryTruncated = normalizeWorkspaceSessionListResponse({
    sessions: [{ id: 'a' }],
    total: 1,
    total_exact: false,
    has_more: true,
  });
  assert.equal(boundaryTruncated.hasMore, true);

  // 反向:服务端读完了整个目录,即使 total 大于返回条数也不算「还有更多」。
  const exhausted = normalizeWorkspaceSessionListResponse({
    sessions: [{ id: 'a' }],
    total: 5,
    total_exact: true,
    has_more: false,
  });
  assert.equal(exhausted.hasMore, false);
});

test('sidebar workspace compact query asks for five rows', () => {
  assert.deepEqual(sidebarWorkspaceSessionListQuery(), {
    limit: SIDEBAR_SESSION_COLLAPSE_LIMIT,
  });
  assert.deepEqual(sidebarWorkspaceSessionListQuery({ visibleCount: 10 }), { limit: 10 });
});

test('workspaceNeedsInitialSidebarLoad only reports true before any workspace data exists', () => {
  assert.equal(workspaceNeedsInitialSidebarLoad(), true);
  assert.equal(workspaceNeedsInitialSidebarLoad({ hasCachedSessions: true }), false);
  assert.equal(workspaceNeedsInitialSidebarLoad({ hasLoaded: true }), false);
});

test('settleSidebarWorkspacePage observes success and failure immediately', async () => {
  assert.deepEqual(await settleSidebarWorkspacePage(Promise.resolve({ sessions: [] })), {
    ok: true,
    page: { sessions: [] },
  });
  const failure = new Error('failed');
  assert.deepEqual(await settleSidebarWorkspacePage(Promise.reject(failure)), {
    ok: false,
    error: failure,
  });
});

test('sidebarWorkspacePageIsCurrent rejects stale refresh generations', () => {
  assert.equal(sidebarWorkspacePageIsCurrent(3, 3), true);
  assert.equal(sidebarWorkspacePageIsCurrent(4, 3), false);
});

test('retainUnrefreshedSidebarSessions keeps collapsed workspaces and pinned extras', () => {
  const previous = [
    { id: 'keep-collapsed', workspace_hash: 'w2' },
    { id: 'visible-old', workspace_hash: 'w1', message_count: 1, turn_count: 1 },
    { id: 'pinned-old', workspace_hash: 'w1' },
    { id: 'drop-old', workspace_hash: 'w1' },
    { id: 'task', no_workspace: true },
  ];
  const incoming = [
    { id: 'visible-new', workspace_hash: 'w1' },
    { id: 'visible-old', workspace_hash: 'w1', message_count: 2, turn_count: 1 },
  ];
  const result = retainUnrefreshedSidebarSessions(previous, incoming, {
    refreshedWorkspaceHashes: ['w1'],
    pinnedByWorkspace: new Map([['w1', ['pinned-old']]]),
    refreshNoWorkspace: false,
  });
  assert.deepEqual(result.map((session) => session.id), [
    'visible-new',
    'visible-old',
    'keep-collapsed',
    'pinned-old',
    'task',
  ]);
});

test('retainUnrefreshedSidebarSessions can refresh the no-workspace list', () => {
  const previous = [
    { id: 'old-task', no_workspace: true },
    { id: 'w1-session', workspace_hash: 'w1' },
  ];
  const incoming = [{ id: 'new-task', no_workspace: true }];
  const result = retainUnrefreshedSidebarSessions(previous, incoming, {
    refreshedWorkspaceHashes: [],
    refreshNoWorkspace: true,
  });
  assert.deepEqual(result.map((session) => session.id), ['new-task', 'w1-session']);
});

test('first full history load appends older sessions after the visible first five', () => {
  const incoming = Array.from({ length: 18 }, (_, index) => ({
    id: String(index + 1),
    workspace_hash: 'w1',
    updated_at: `2026-09-${String(20 - index).padStart(2, '0')}T00:00:00Z`,
    message_count: 2,
  }));
  const previous = incoming.slice(0, 5);
  const result = retainUnrefreshedSidebarSessions(previous, incoming, {
    refreshedWorkspaceHashes: ['w1'],
    appendNewSessions: true,
  });
  assert.deepEqual(result.map((session) => session.id), incoming.map((session) => session.id));
  assert.deepEqual(sidebarSessionProjection(result, 10).visibleSessions.map((session) => session.id),
    ['1', '2', '3', '4', '5', '6', '7', '8', '9', '10']);
  assert.equal(previous.length, 5);
});

test('first full history load preserves manual ordering and unrelated workspace rows', () => {
  const incoming = Array.from({ length: 8 }, (_, index) => ({
    id: String(index + 1),
    workspace_hash: 'w1',
  }));
  const previous = [incoming[2], incoming[0], incoming[4], incoming[1], incoming[3], {
    id: 'other-workspace',
    workspace_hash: 'w2',
  }];
  const result = retainUnrefreshedSidebarSessions(previous, incoming, {
    refreshedWorkspaceHashes: ['w1'],
    appendNewSessions: true,
  });
  assert.deepEqual(result.map((session) => session.id),
    ['3', '1', '5', '2', '4', 'other-workspace', '6', '7', '8']);
});

test('history completion still promotes known sessions whose content counters change', () => {
  const previous = [
    { id: 'a', workspace_hash: 'w1', message_count: 1 },
    { id: 'b', workspace_hash: 'w1', message_count: 1 },
  ];
  const incoming = [
    previous[0],
    { ...previous[1], message_count: 2 },
    { id: 'older', workspace_hash: 'w1', message_count: 8 },
  ];
  const result = retainUnrefreshedSidebarSessions(previous, incoming, {
    refreshedWorkspaceHashes: ['w1'],
    appendNewSessions: true,
  });
  assert.deepEqual(result.map((session) => session.id), ['b', 'a', 'older']);
  assert.equal(result[0].message_count, 2);
});

test('ordinary refresh continues to promote previously unknown sessions', () => {
  const previous = [{ id: 'known', workspace_hash: 'w1' }];
  const incoming = [...previous, { id: 'new', workspace_hash: 'w1' }];
  for (const appendNewSessions of [undefined, false]) {
    const result = retainUnrefreshedSidebarSessions(previous, incoming, {
      refreshedWorkspaceHashes: ['w1'],
      appendNewSessions,
    });
    assert.deepEqual(result.map((session) => session.id), ['new', 'known']);
  }
});

test('compact workspace requests leave five ordinary rows after pinned sessions are filtered', () => {
  const sessions = Array.from({ length: 18 }, (_, index) => ({
    id: `s${index}`,
    workspace_hash: 'w1',
  }));
  const pinnedIds = ['s0', 's1', 's1', ''];
  const query = sidebarWorkspaceSessionListQuery({ pinnedIds });
  assert.deepEqual(query, { limit: 7 });
  const page = sessions.slice(0, query.limit);
  const ordinary = filterPinnedSessions(page, new Map([['w1', pinnedIds]]));
  assert.equal(sidebarSessionProjection(ordinary).visibleSessions.length, 5);
  assert.deepEqual(sidebarWorkspaceSessionListQuery({ visibleCount: 10, pinnedIds }), { limit: 12 });
});

test('workspaceHasCachedSidebarSessions only matches that workspace', () => {
  const sessions = [
    { id: 'a', workspace_hash: 'w1' },
    { id: 'task', no_workspace: true },
  ];
  assert.equal(workspaceHasCachedSidebarSessions(sessions, 'w1'), true);
  assert.equal(workspaceHasCachedSidebarSessions(sessions, 'w2'), false);
  assert.equal(workspaceHasCachedSidebarSessions(sessions, ''), false);
});

test('sidebarSessionProjection uses reported total when the compact page is shorter', () => {
  const sessions = Array.from({ length: 5 }, (_, index) => ({ id: String(index) }));
  const result = sidebarSessionProjection(sessions, SIDEBAR_SESSION_COLLAPSE_LIMIT, SIDEBAR_SESSION_COLLAPSE_LIMIT, 12);
  assert.equal(result.collapsible, true);
  assert.equal(result.action, 'expand');
  assert.equal(result.hiddenCount, 7);
  assert.deepEqual(result.visibleSessions.map((session) => session.id), ['0', '1', '2', '3', '4']);
});

await Promise.all(pendingTests);
