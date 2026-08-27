import assert from 'node:assert/strict';
import {
  SIDEBAR_SESSION_COLLAPSE_LIMIT,
  sidebarSessionProjection,
} from './sidebarSessions.js';
import {
  normalizeWorkspaceSessionListResponse,
  retainUnrefreshedSidebarSessions,
  sidebarWorkspaceSessionListQuery,
  workspaceHasCachedSidebarSessions,
} from './sidebarWorkspaceSessions.js';

function test(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

test('normalizeWorkspaceSessionListResponse accepts arrays and envelopes', () => {
  assert.deepEqual(normalizeWorkspaceSessionListResponse([{ id: 'a' }, { id: 'b' }]), {
    sessions: [{ id: 'a' }, { id: 'b' }],
    total: 2,
  });
  assert.deepEqual(normalizeWorkspaceSessionListResponse({
    sessions: [{ id: 'a' }],
    total: 9,
  }), {
    sessions: [{ id: 'a' }],
    total: 9,
  });
  assert.deepEqual(normalizeWorkspaceSessionListResponse({ sessions: [{ id: 'a' }] }), {
    sessions: [{ id: 'a' }],
    total: 1,
  });
  assert.deepEqual(normalizeWorkspaceSessionListResponse(null), {
    sessions: [],
    total: 0,
  });
});

test('sidebar workspace compact query asks for five rows', () => {
  assert.deepEqual(sidebarWorkspaceSessionListQuery({ full: false }), {
    limit: SIDEBAR_SESSION_COLLAPSE_LIMIT,
  });
  assert.deepEqual(sidebarWorkspaceSessionListQuery({ full: true }), {});
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
  const result = sidebarSessionProjection(sessions, false, SIDEBAR_SESSION_COLLAPSE_LIMIT, 12);
  assert.equal(result.collapsible, true);
  assert.equal(result.action, 'expand');
  assert.equal(result.hiddenCount, 7);
  assert.deepEqual(result.visibleSessions.map((session) => session.id), ['0', '1', '2', '3', '4']);
});
