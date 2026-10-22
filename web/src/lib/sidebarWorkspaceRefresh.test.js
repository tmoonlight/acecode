import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import vm from 'node:vm';
import { setImmediate as nextTask } from 'node:timers/promises';
import { parseSync } from '@babel/core';
import * as workspaceSessions from './sidebarWorkspaceSessions.js';
import * as sidebarSessions from './sidebarSessions.js';
import * as sidebarTree from './sidebarSessionTree.js';
import * as pinnedSessions from './pinnedSessions.js';
import * as auxiliaryFetch from './sidebarAuxiliaryFetch.js';
import { applyStatusUpdate } from './sessionStatus.js';
import { createWorkspaceFolderOrderController } from './workspaceFolderOrder.js';
import { commitIfChanged, structurallyEqual } from './structuralState.js';

// Execute the production callbacks, including the await boundaries and state
// setters. Testing only the sequence helper cannot expose a check before a later
// await, or a background request invalidating a pending user full-list request.
const source = readFileSync(new URL('../components/Sidebar.jsx', import.meta.url), 'utf8');
const ast = parseSync(source, { configFile: false, babelrc: false, parserOpts: { plugins: ['jsx'] } });
const component = ast.program.body.map((node) => node.declaration || node)
  .find((node) => node.id?.name === 'Sidebar');
assert.ok(component, 'Missing production Sidebar');
const declarations = component.body.body.flatMap((statement) => statement.declarations || []);

function installCallback(context, name) {
  const declaration = declarations.find((node) => node.id.name === name);
  assert.ok(['useCallback', 'useStableCallback'].includes(declaration?.init.callee?.name),
    `Missing production callback ${name}`);
  const callback = declaration.init.arguments[0];
  vm.runInContext(`${name} = (${source.slice(callback.start, callback.end)})`, context);
}

function deferred() {
  let resolve;
  let reject;
  const promise = new Promise((yes, no) => { resolve = yes; reject = no; });
  return { promise, resolve, reject };
}

function sessions(count, prefix = 'session') {
  return Array.from({ length: count }, (_, i) => ({
    id: `${prefix}-${i}`, workspace_hash: 'w', title: `${prefix} ${i}`,
    updated_at: new Date(Date.UTC(2026, 8, 20, 0, count - i)).toISOString(),
  }));
}

const compactPage = () => ({ sessions: sessions(5), total: 20, has_more: true });

function fixture({
  initialSessions = sessions(5), loaded = true, noWorkspace, pinned, cachedPinnedIds = [],
  workspaceList, otherWorkspaces = [],
} = {}) {
  const workspace = { hash: 'w', cwd: '/fixture', active: true };
  const state = {
    sessions: initialSessions,
    loading: new Set(), loaded: new Set(loaded ? ['w'] : []), fullyLoaded: new Set(),
    totals: new Map(), statuses: new Map(), workspaces: [workspace, ...otherWorkspaces],
    expandedSessionLists: new Map(),
    retainedSyncs: [],
  };
  const requests = [];
  const context = vm.createContext({
    Map, Set, Array, Promise, setTimeout,
    ...workspaceSessions, ...sidebarSessions, ...sidebarTree, ...pinnedSessions, ...auxiliaryFetch,
    applyStatusUpdate, commitIfChanged,
    workspaces: state.workspaces, activeWorkspaceHash: 'w',
    revealTarget: { noWorkspace: false, workspaceHash: 'w' },
    api: {
      listWorkspaces: () => workspaceList?.promise || Promise.resolve([workspace, ...otherWorkspaces]),
      listSidebarSessions: (hash, query) => {
        if (!hash) return noWorkspace?.promise || Promise.resolve([]);
        const request = { hash, query, ...deferred() };
        requests.push(request);
        return request.promise;
      },
      getPinnedSessions: () => pinned?.promise || Promise.resolve({ session_ids: cachedPinnedIds }),
      getNoWorkspacePinnedSessions: async () => ({ session_ids: [] }),
      getPinnedSessionOrder: async () => ({ items: [] }),
    },
    hasDesktopBridge: () => false,
    desktopTaskbarBadge: { retainWorkspaces() {}, replaceScope() {} },
    desktopTaskbarBadgeAvailable: () => false,
    connection: { subscribeWorkspaceStatus() {} },
    refreshOpencodeImportPreview: async () => {},
    syncRetainedSessionIds(ids) { state.retainedSyncs.push(ids); }, cancelSessionSelection() {}, onOpenHome() {}, onBeforeNavigate: null,
    acceptSidebarTreePage() {},
    sidebarTreeOptions: (scope, limit) => ({ limit }),
    setPinnedMap: (value) => { context.pinnedByWorkspaceRef.current = value; },
    setPinnedOrder: (value) => { context.pinnedOrderItemsRef.current = value; },
    updateExpanded: (updater) => { context.expandedRef.current = updater(context.expandedRef.current); },
    setActiveWorkspaceHash: (value) => { context.activeWorkspaceHash = value; },
  });
  const refs = {
    sessionsRef: state.sessions,
    expandedSessionListsRef: state.expandedSessionLists,
    sessionLoadedWorkspacesRef: state.loaded,
    sessionFullyLoadedWorkspacesRef: state.fullyLoaded,
    workspaceSessionLoadSeqRef: new Map(),
    pendingWorkspaceLoadsRef: new Map(),
    refreshingRef: false, pendingRefreshHashRef: '',
    treeSavingRef: false, treeRevisionRef: 0, batchOperationRef: false,
    archiveOperationsRef: new Set(),
    refreshSelectionRef: { activeWorkspaceHash: 'w', revealTarget: { noWorkspace: false, workspaceHash: 'w' } },
    expandedRef: new Set(['w']),
    workspaceCollapseAllRef: false, userCollapsedWorkspacesRef: new Set(),
    sessionListDisclosureCompactRef: new Set(),
    opencodePreviewProbedRef: new Set(),
    pinnedByWorkspaceRef: new Map([['w', cachedPinnedIds]]), pinnedOrderItemsRef: [],
  };
  for (const [name, current] of Object.entries(refs)) context[name] = { current };
  // structural = 生产代码里用 useStructuralState 声明的状态:内容不变时不写入。
  // state.writes 记录真正写入 React 状态的次数。
  state.writes = {};
  for (const [setter, key, ref, structural] of [
    ['setSessions', 'sessions', 'sessionsRef', true],
    ['setSessionLoadingWorkspaces', 'loading', undefined, true],
    ['setSessionLoadedWorkspaces', 'loaded', 'sessionLoadedWorkspacesRef'],
    ['setSessionFullyLoadedWorkspaces', 'fullyLoaded', 'sessionFullyLoadedWorkspacesRef'],
    ['setSessionListTotals', 'totals', undefined, true],
    ['setStatusBySession', 'statuses', undefined, true],
    ['setWorkspaces', 'workspaces', undefined, true],
    ['setExpandedSessionLists', 'expandedSessionLists', 'expandedSessionListsRef'],
  ]) {
    context[setter] = (update) => {
      const next = typeof update === 'function' ? update(state[key]) : update;
      if (structural && structurallyEqual(state[key], next)) return;
      state.writes[key] = (state.writes[key] || 0) + 1;
      state[key] = next;
      if (ref) context[ref].current = state[key];
      if (key === 'workspaces') context.workspaces = state[key];
      if (key === 'fullyLoaded') context.sessionFullyLoadedWorkspaces = state[key];
      if (key === 'loaded') context.sessionLoadedWorkspaces = state[key];
    };
  }
  context.sessionFullyLoadedWorkspaces = state.fullyLoaded;
  context.sessionLoadedWorkspaces = state.loaded;
  context.NO_WORKSPACE_SESSION_LIST_KEY = pinnedSessions.NO_WORKSPACE_PIN_SCOPE;
  context.workspaceOrderControllerRef = { current: createWorkspaceFolderOrderController({
    getWorkspaces: () => state.workspaces,
    setWorkspaces: context.setWorkspaces,
    save: async (hashes) => ({ hashes }),
  }) };
  for (const name of ['isNoWorkspaceSession', 'normalizeNoWorkspaceSession', 'normalizeWorkspaceSession']) {
    const node = ast.program.body.find((item) => item.type === 'FunctionDeclaration' && item.id.name === name);
    assert.ok(node, `Missing production helper ${name}`);
    vm.runInContext(source.slice(node.start, node.end), context);
  }
  for (const name of [
    'setSessionWorkspaceLoading', 'setSessionWorkspacesLoaded', 'markWorkspaceSessionsFullyLoaded',
    'applyWorkspaceSessionList', 'loadWorkspaceSessions', 'toggleSessionListExpanded', 'refresh', 'onActivate',
    'invalidateArchiveLists', 'refreshArchivedScope',
  ]) installCallback(context, name);
  context.treeApplyRef = { current: (scope, page) => context.applyWorkspaceSessionList({ hash: scope }, page) };
  return { context, state, requests, workspace };
}

const tests = [];
function test(name, run) { tests.push({ name, run }); }

test('an old compact result cannot overwrite a full load while waiting for no-workspace sessions', async () => {
  const noWorkspace = deferred();
  const { context, state, requests } = fixture({ noWorkspace });
  const refresh = context.refresh();
  await nextTask();
  assert.equal(requests.length, 1);
  assert.equal(requests[0].query.limit, 5);
  requests[0].resolve({ ...compactPage(), total: 99 });
  await nextTask();
  const full = context.loadWorkspaceSessions('w', { visibleCount: 20 });
  requests[1].resolve(sessions(20));
  await full;
  assert.equal(state.sessions.length, 20, 'user full load commits before the old refresh resumes');
  noWorkspace.resolve([]);
  await refresh;
  assert.equal(state.sessions.length, 20);
  assert.equal(state.totals.get('w'), 20);
  assert.equal(state.fullyLoaded.has('w'), true);
});

test('a periodic refresh reuses a pending user full load instead of superseding it with five rows', async () => {
  const { context, state, requests } = fixture();
  const full = context.loadWorkspaceSessions('w', { visibleCount: 20 });
  const refresh = context.refresh();
  await nextTask();
  // Resolve a compact request too on the unfixed PR, exposing the resulting
  // five-row list without hanging the regression test.
  requests.slice(1).forEach((request) => request.resolve(compactPage()));
  requests[0].resolve(sessions(20));
  await Promise.all([full, refresh]);
  assert.equal(state.sessions.length, 20);
  assert.equal(state.fullyLoaded.has('w'), true);
  assert.equal(requests.length, 1, 'background refresh must share the full request');
});

test('reactivating an already loaded empty workspace exits loading after the current request settles', async () => {
  const { context, state, requests, workspace } = fixture({ initialSessions: [] });
  const productionRefresh = context.refresh;
  let refresh;
  context.refresh = (...args) => { refresh = productionRefresh(...args); return refresh; };
  await context.onActivate(workspace);
  await nextTask();
  assert.equal(state.loading.has('w'), true);
  assert.equal(requests.length, 1);
  requests[0].resolve([]);
  await refresh;
  assert.equal(state.loading.has('w'), false);
  assert.equal(state.loaded.has('w'), true);
  assert.equal(state.sessions.length, 0);
});

test('an old refresh cannot clear the loading indicator of a newer pending full request', async () => {
  const { context, state, requests } = fixture({ initialSessions: [], loaded: false });
  const refresh = context.refresh();
  await nextTask();
  const full = context.loadWorkspaceSessions('w', { visibleCount: 20 });
  requests[0].resolve(compactPage());
  await refresh;
  assert.equal(state.loading.has('w'), true, 'new full request still owns loading');
  requests[1].resolve(sessions(20));
  await full;
  assert.equal(state.loading.has('w'), false);
  assert.equal(state.sessions.length, 20);
});

test('a failed current request also clears activation loading for an already loaded empty workspace', async () => {
  const { context, state, requests, workspace } = fixture({ initialSessions: [] });
  const productionRefresh = context.refresh;
  let refresh;
  context.refresh = (...args) => { refresh = productionRefresh(...args); return refresh; };
  await context.onActivate(workspace);
  await nextTask();
  assert.equal(requests.length, 1);
  requests[0].reject(new Error('unavailable'));
  await refresh;
  assert.equal(state.loading.has('w'), false);
  assert.equal(state.loaded.has('w'), true);
  assert.equal(state.sessions.length, 0);
});

test('a failed shared full request retains cached rows and permits a later retry', async () => {
  const { context, state, requests } = fixture();
  const full = context.loadWorkspaceSessions('w', { visibleCount: 20 });
  const refresh = context.refresh();
  await nextTask();
  requests.slice(1).forEach((request) => request.reject(new Error('unavailable')));
  requests[0].reject(new Error('unavailable'));
  await Promise.all([full, refresh]);
  assert.equal(state.sessions.length, 5);
  assert.equal(state.fullyLoaded.has('w'), false);
  assert.equal(state.loading.has('w'), false);
  const beforeRetry = requests.length;
  const retry = context.loadWorkspaceSessions('w', { visibleCount: 20 });
  assert.equal(requests.length, beforeRetry + 1);
  requests.at(-1).resolve(sessions(20));
  await retry;
  assert.equal(state.sessions.length, 20);
  assert.equal(state.fullyLoaded.has('w'), true);
});

test('settling an earlier full request does not remove a later full request from refresh sharing', async () => {
  const { context, state, requests } = fixture();
  const first = context.loadWorkspaceSessions('w', { visibleCount: 20 });
  const second = context.loadWorkspaceSessions('w', { visibleCount: 20 });
  requests[0].resolve(sessions(10));
  await first;
  assert.equal(state.sessions.length, 5, 'superseded full result is discarded');
  const refresh = context.refresh();
  await nextTask();
  requests.slice(2).forEach((request) => request.resolve(compactPage()));
  requests[1].resolve(sessions(20));
  await Promise.all([second, refresh]);
  assert.equal(state.sessions.length, 20);
  assert.equal(requests.length, 2, 'refresh shares the second full request');
});

test('visible session requests still start before slow pinned metadata resolves', async () => {
  const pinned = deferred();
  const { context, state, requests } = fixture({ pinned, initialSessions: [], loaded: false });
  const refresh = context.refresh();
  await nextTask();
  assert.equal(requests.length, 1, 'retain the early request benefit of PR #64');
  requests[0].resolve(compactPage());
  await nextTask();
  assert.equal(state.sessions.length, 5, 'render rows while pinned request is still pending');
  pinned.resolve({ session_ids: [] });
  await refresh;
  assert.equal(state.sessions.length, 5);
  assert.equal(state.totals.get('w'), 20);
});

test('early compact requests include cached pinned slots so five ordinary rows remain', async () => {
  const { context, state, requests } = fixture({ cachedPinnedIds: ['session-0', 'session-1'] });
  const refresh = context.refresh();
  await nextTask();
  assert.equal(requests[0].query.limit, 7);
  requests[0].resolve({ sessions: sessions(7), total: 20, has_more: true });
  await refresh;
  assert.equal(requests.length, 1);
  assert.equal(pinnedSessions.filterPinnedSessions(state.sessions, context.pinnedByWorkspaceRef.current).length, 5);
});

test('new pinned slots trigger a supplemental compact request after the early request', async () => {
  const pinned = deferred();
  const { context, state, requests } = fixture({ pinned });
  const refresh = context.refresh();
  await nextTask();
  assert.equal(requests[0].query.limit, 5);
  requests[0].resolve(compactPage());
  pinned.resolve({ session_ids: ['session-0', 'session-1'] });
  await nextTask();
  assert.equal(requests.length, 2);
  assert.equal(requests[1].query.limit, 7);
  requests[1].resolve({ sessions: sessions(7), total: 20, has_more: true });
  await refresh;
  assert.equal(pinnedSessions.filterPinnedSessions(state.sessions, context.pinnedByWorkspaceRef.current).length, 5);
});

test('a newer full result prevents stale compact pinned supplementation', async () => {
  const pinned = deferred();
  const { context, state, requests } = fixture({ pinned });
  const refresh = context.refresh();
  await nextTask();
  requests[0].resolve(compactPage());
  const full = context.loadWorkspaceSessions('w', { visibleCount: 20 });
  requests[1].resolve(sessions(20));
  await full;
  pinned.resolve({ session_ids: ['session-0', 'session-1'] });
  await refresh;
  assert.equal(requests.length, 2, 'do not start a third request for a superseded compact page');
  assert.equal(state.sessions.length, 20);
  assert.equal(state.fullyLoaded.has('w'), true);
});

test('a pending full request is shared even when pinned metadata grows', async () => {
  const pinned = deferred();
  const { context, state, requests } = fixture({ pinned });
  const full = context.loadWorkspaceSessions('w', { visibleCount: 20 });
  const refresh = context.refresh();
  await nextTask();
  pinned.resolve({ session_ids: ['session-0', 'session-1'] });
  await nextTask();
  assert.equal(requests.length, 1);
  requests[0].resolve(sessions(20));
  await Promise.all([full, refresh]);
  assert.equal(state.sessions.length, 20);
  assert.equal(requests.length, 1);
});

test('first full history load retains manual order and expansion advances in five-row batches', async () => {
  const initialSessions = sessions(5).reverse();
  const { context, state, requests } = fixture({ initialSessions });
  context.toggleSessionListExpanded('w');
  assert.equal(state.expandedSessionLists.get('w'), 10);
  assert.equal(requests.length, 1);
  requests[0].resolve(sessions(20));
  await nextTask();
  assert.deepEqual(state.sessions.slice(0, 5).map((item) => item.id), initialSessions.map((item) => item.id));
  context.toggleSessionListExpanded('w');
  assert.equal(state.expandedSessionLists.get('w'), 15);
  assert.equal(requests.length, 1, 'already full history is reused by later expansion batches');
  context.toggleSessionListExpanded('w', 'collapse');
  assert.equal(state.expandedSessionLists.has('w'), false);
});

test('refresh uses the real workspace order controller to preserve a concurrent folder reorder', async () => {
  const workspaceList = deferred();
  const other = { hash: 'other', cwd: '/other', active: false };
  const { context, state, requests, workspace } = fixture({ workspaceList, otherWorkspaces: [other] });
  const refresh = context.refresh();
  assert.equal(await context.workspaceOrderControllerRef.current.reorder([other, workspace]), true);
  workspaceList.resolve([workspace, other]);
  await nextTask();
  assert.deepEqual(state.workspaces.map((item) => item.hash), ['other', 'w']);
  assert.equal(requests.length, 1);
  requests[0].resolve(compactPage());
  await refresh;
  assert.deepEqual(state.workspaces.map((item) => item.hash), ['other', 'w']);
});


test('expansion and subsequent refresh request only the displayed batches', async () => {
  const { context, state, requests } = fixture();
  context.toggleSessionListExpanded('w');
  assert.equal(requests[0].query.limit, 10);
  requests[0].resolve({ sessions: sessions(10), total: 30, has_more: true });
  await nextTask();
  context.toggleSessionListExpanded('w');
  assert.equal(requests[1].query.limit, 15);
  requests[1].resolve({ sessions: sessions(15), total: 30, has_more: true });
  await nextTask();
  const refresh = context.refresh();
  await nextTask();
  assert.equal(requests[2].query.limit, 15);
  requests[2].resolve({ sessions: sessions(15), total: 30, has_more: true });
  await refresh;
  assert.equal(state.sessions.length, 15);
  assert.ok(requests.every(request => Number.isFinite(request.query.limit)));
});

// 场景:Desktop 挂机,侧栏每 5 秒刷新一次,服务端数据完全没变。
// 期望:第二轮刷新不写入任何侧栏状态(连 setState 都不调用),各状态仍是第一轮的
// 同一个引用,React 因此不渲染、也不在 hook 队列里留下 update。
// 回归 bug:每轮都写回新的 Map / 数组触发渲染,侧栏的 useCallback 把每次渲染的作用域
// 串成链留在内存里(实测空闲首页每次约 70 KB),渲染进程一两天涨到上限后显示
// 「内存不足」错误页,只能手动刷新。只在 updater 里返回旧值也不行:React 18 会把这类
// update 挂在 hook 队列里直到组件下次渲染,组件空闲时队列照样无限增长。
test('an idle periodic refresh with unchanged data writes no sidebar state', async () => {
  const { context, state, requests } = fixture();
  const first = context.refresh();
  await nextTask();
  requests[0].resolve(compactPage());
  await first;
  const keys = ['sessions', 'statuses', 'totals', 'loading', 'loaded', 'fullyLoaded', 'workspaces', 'expandedSessionLists'];
  const before = Object.fromEntries(keys.map((key) => [key, state[key]]));
  const writesBefore = { ...state.writes };
  const second = context.refresh();
  await nextTask();
  assert.equal(requests.length, 2);
  requests[1].resolve(compactPage());
  await second;
  for (const key of keys) assert.equal(state[key], before[key], `${key} must keep its reference`);
  assert.deepEqual(state.writes, writesBefore, 'an idle refresh must not call any state setter');
});

// 场景:有人把刷新路径上的状态改回普通 useState。
// 期望:直接失败 —— 上面那条用例里的测试替身按 useStructuralState 的语义写回,
// 只有生产声明也是 useStructuralState 时它才代表真实行为。
test('sidebar refresh state is declared with useStructuralState', () => {
  for (const name of ['workspaces', 'sessions', 'statusBySession', 'sessionLoadingWorkspaces', 'sessionListTotals', 'activeWorkspaceHash']) {
    const declaration = declarations.find((node) => (
      node.id.type === 'ArrayPattern' && node.id.elements[0]?.name === name));
    assert.equal(declaration?.init.callee?.name, 'useStructuralState', name);
  }
});

test('runtime promotion and internal workspace selection do not change refresh dependencies', () => {
  const refresh = declarations.find(node => node.id.name === 'refresh');
  const dependencies = source.slice(refresh.init.arguments[1].start, refresh.init.arguments[1].end);
  assert.doesNotMatch(dependencies, /activeWorkspaceHash|revealTarget/);
  assert.match(source, /\[refresh, revealTarget.workspaceHash, revealTarget.noWorkspace\]/);
});

test('sidebar default session list is explicitly scoped to no-workspace tasks', () => {
  assert.match(source, /api.listSidebarSessions\('', sidebarTreeOptions\(NO_WORKSPACE_PIN_SCOPE, 0\)\)/);
  assert.doesNotMatch(source, /api.listSessions\(\)/);
});

test('archive replaces a compact page atomically and rejects a pre-archive polling result', async () => {
  const { context, state, requests } = fixture();
  const staleRefresh = context.refresh();
  await nextTask();
  context.invalidateArchiveLists('w');
  const replacement = sessions(6).slice(1);
  const archiveRefresh = context.refreshArchivedScope('w');
  assert.equal(state.sessions[0].id, 'session-0', 'keep the complete page until its replacement arrives');
  requests[1].resolve({ sessions: replacement, total: 19, has_more: true });
  assert.equal(await archiveRefresh, true);
  assert.equal(state.sessions.length, 5);
  assert.ok(state.sessions.every(session => session.id !== 'session-0'));
  requests[0].resolve(compactPage());
  await staleRefresh;
  assert.equal(state.sessions.length, 5);
  assert.ok(state.sessions.every(session => session.id !== 'session-0'));
  assert.equal(state.totals.get('w'), 19);
  assert.equal(state.retainedSyncs.length, 0, 'discarded refresh must not release current status subscriptions');
});

test('concurrent archives keep the newest page when their responses arrive out of order', async () => {
  const { context, state, requests } = fixture();
  context.invalidateArchiveLists('w');
  const first = context.refreshArchivedScope('w');
  context.invalidateArchiveLists('w');
  const second = context.refreshArchivedScope('w');
  requests[1].resolve({ sessions: sessions(7).slice(2), total: 18, has_more: true });
  await second;
  requests[0].resolve({ sessions: sessions(6).slice(1), total: 19, has_more: true });
  await first;
  assert.equal(state.sessions.length, 5);
  assert.ok(state.sessions.every(session => !['session-0', 'session-1'].includes(session.id)));
  assert.equal(state.totals.get('w'), 18);
});

test('only the latest failed archive refresh requests a local removal fallback', async () => {
  const { context, requests } = fixture();
  const first = context.refreshArchivedScope('w');
  const second = context.refreshArchivedScope('w');
  requests[0].reject(new Error('old failure'));
  requests[1].reject(new Error('latest failure'));
  assert.equal(await first, true);
  assert.equal(await second, false);
});

test('archive invalidation also rejects pinned metadata collected before the mutation', async () => {
  const pinned = deferred();
  const { context, requests } = fixture({ pinned });
  const refresh = context.refresh();
  await nextTask();
  context.invalidateArchiveLists('w');
  context.pinnedByWorkspaceRef.current = new Map([['w', []]]);
  requests[0].resolve(compactPage());
  pinned.resolve({ session_ids: ['session-0'] });
  await refresh;
  assert.deepEqual(context.pinnedByWorkspaceRef.current.get('w'), []);
});

let failures = 0;
for (const { name, run } of tests) {
  try {
    await run();
    console.log(`[pass] ${name}`);
  } catch (error) {
    failures += 1;
    console.error(`[fail] ${name}\n${error.stack}`);
  }
}
assert.equal(failures, 0, `${failures} sidebar request timing regressions`);
