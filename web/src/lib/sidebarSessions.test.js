import assert from 'node:assert/strict';
import {
  SIDEBAR_SESSION_COLLAPSE_LIMIT,
  applyRemoteControlSessionSelection,
  expandedSessionListsAfterWorkspaceCollapseAll,
  projectRemoteControlBinding,
  reconcileSidebarSessions,
  remoteControlSurgeTargetKey,
  sessionListNeedsRevealExpansion,
  sessionMatchesRevealTarget,
  shouldStartRemoteControlSurge,
  sidebarSessionHasWorktree,
  sidebarSessionMarker,
  sidebarRevealTarget,
  sidebarRevealTargetKey,
  sidebarSessionProjection,
  sortSidebarSessionsNewestFirst,
  upsertSidebarSession,
} from './sidebarSessions.js';

function test(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

test('sidebarSessionHasWorktree requires a worktree name or branch', () => {
  assert.equal(sidebarSessionHasWorktree({}), false);
  assert.equal(sidebarSessionHasWorktree({ worktree: {} }), false);
  assert.equal(sidebarSessionHasWorktree({ worktree: { name: '   ', branch: '' } }), false);
  assert.equal(sidebarSessionHasWorktree({ worktree: { name: 'ses-abc' } }), true);
  assert.equal(sidebarSessionHasWorktree({ worktree: { branch: 'worktree-ses-abc' } }), true);
});

test('sidebarSessionMarker gives LOOP alarm priority over worktree', () => {
  assert.equal(sidebarSessionMarker({}), '');
  assert.equal(sidebarSessionMarker({
    worktree: { name: 'ses-abc' },
  }), 'worktree');
  assert.equal(sidebarSessionMarker({
    loop_execution: { loop_id: 'loop-1', run_id: 'run-1' },
  }), 'loop');
  assert.equal(sidebarSessionMarker({
    loop_execution: { loop_id: 'loop-1', run_id: 'run-1' },
    worktree: { name: 'ses-abc', branch: 'worktree-ses-abc' },
  }), 'loop');
  assert.equal(sidebarSessionMarker({
    loop_execution: {},
    worktree: { name: 'ses-abc' },
  }), 'worktree');
});

test('remote-control surge starts only on a false-to-true binding transition', () => {
  assert.equal(shouldStartRemoteControlSurge(false, true), true);
  assert.equal(shouldStartRemoteControlSurge(undefined, true), true);
  assert.equal(shouldStartRemoteControlSurge(true, true), false);
  assert.equal(shouldStartRemoteControlSurge(true, false), false);
  assert.equal(shouldStartRemoteControlSurge(false, false), false);
});

test('remote-control selection clears old bindings and keeps the selected row bound', () => {
  const result = applyRemoteControlSessionSelection([
    { id: 'old', workspace_hash: 'w1', remote_control_bound: true },
    { id: 'other', workspace_hash: 'w1' },
  ], {
    id: 'target',
    workspace_hash: 'w2',
    title: 'Target',
  });

  assert.deepEqual(result.map((session) => ({
    id: session.id,
    workspace: session.workspace_hash,
    bound: session.remote_control_bound,
  })), [
    { id: 'old', workspace: 'w1', bound: false },
    { id: 'other', workspace: 'w1', bound: undefined },
    { id: 'target', workspace: 'w2', bound: true },
  ]);
});

test('remote-control surge target key distinguishes workspace and no-workspace rows', () => {
  assert.equal(
    remoteControlSurgeTargetKey({ id: 's1', workspace_hash: 'w1' }),
    'workspace\u0000w1\u0000s1',
  );
  assert.equal(
    remoteControlSurgeTargetKey({ id: 's1', no_workspace: true }),
    'no-workspace\u0000\u0000s1',
  );
});

test('remote-control binding projection survives a later session-list refresh', () => {
  const result = projectRemoteControlBinding([
    { id: 'old', workspace_hash: 'w1', remote_control_bound: true },
    { id: 'target', workspace_hash: 'w2' },
  ], {
    id: 'target',
    workspace_hash: 'w2',
  });
  assert.deepEqual(result.map((session) => [session.id, session.remote_control_bound]), [
    ['old', false],
    ['target', true],
  ]);
});

test('five or fewer sidebar sessions are not collapsible', () => {
  const sessions = Array.from({ length: SIDEBAR_SESSION_COLLAPSE_LIMIT }, (_, i) => ({ id: String(i) }));
  const result = sidebarSessionProjection(sessions, false);
  assert.equal(result.collapsible, false);
  assert.equal(result.action, '');
  assert.deepEqual(result.visibleSessions.map((s) => s.id), ['0', '1', '2', '3', '4']);
});

test('more than five sidebar sessions collapse to first five', () => {
  const sessions = Array.from({ length: 7 }, (_, i) => ({ id: String(i) }));
  const result = sidebarSessionProjection(sessions, false);
  assert.equal(result.collapsible, true);
  assert.equal(result.action, 'expand');
  assert.equal(result.hiddenCount, 2);
  assert.deepEqual(result.visibleSessions.map((s) => s.id), ['0', '1', '2', '3', '4']);
});

test('expanded sidebar sessions show all rows and collapse action', () => {
  const sessions = Array.from({ length: 7 }, (_, i) => ({ id: String(i) }));
  const result = sidebarSessionProjection(sessions, true);
  assert.equal(result.collapsible, true);
  assert.equal(result.action, 'collapse');
  assert.equal(result.hiddenCount, 0);
  assert.deepEqual(result.visibleSessions.map((s) => s.id), ['0', '1', '2', '3', '4', '5', '6']);
});

test('collapse all workspaces resets registered session lists to the default compact state', () => {
  const expanded = expandedSessionListsAfterWorkspaceCollapseAll(
    new Set(['__no_workspace__', 'w1', 'w2', 'w3', 'stale-workspace']),
    [
      { hash: 'w1' },
      { workspace_hash: 'w2' },
      { workspaceHash: 'w3' },
      { hash: '' },
    ],
  );
  assert.deepEqual(
    Array.from(expanded),
    ['__no_workspace__', 'stale-workspace'],
  );
  const sessions = Array.from({ length: 7 }, (_, index) => ({ id: String(index) }));
  const projection = sidebarSessionProjection(sessions, expanded.has('w1'));
  assert.equal(projection.action, 'expand');
  assert.deepEqual(projection.visibleSessions.map((session) => session.id), ['0', '1', '2', '3', '4']);
  assert.equal(expanded.has('__no_workspace__'), true);
});

test('sidebarRevealTarget keeps workspace session identity', () => {
  assert.deepEqual(sidebarRevealTarget({
    sessionId: 's1',
    workspaceHash: 'w1',
  }), {
    sessionId: 's1',
    workspaceHash: 'w1',
    noWorkspace: false,
  });
});

test('sidebarRevealTarget marks no-workspace sessions without workspace hash', () => {
  assert.deepEqual(sidebarRevealTarget({
    session_id: 's1',
    workspace_hash: 'w1',
    no_workspace: true,
  }), {
    sessionId: 's1',
    workspaceHash: '',
    noWorkspace: true,
  });
});

test('sidebarRevealTargetKey distinguishes workspace and no-workspace targets', () => {
  assert.equal(
    sidebarRevealTargetKey({ sessionId: 's1', workspaceHash: 'w1' }),
    'workspace\u0000w1\u0000s1',
  );
  assert.equal(
    sidebarRevealTargetKey({
      session_id: 's1',
      workspace_hash: 'stale-workspace',
      no_workspace: true,
    }),
    'no-workspace\u0000\u0000s1',
  );
  assert.equal(sidebarRevealTargetKey({ workspaceHash: 'w1' }), '');
});

test('sidebarRevealTargetKey stays stable across refresh-only metadata updates', () => {
  const previous = sidebarRevealTargetKey({
    sessionId: 's1',
    workspaceHash: 'w1',
    title: 'Earlier title',
    updated_at: '2026-07-30T01:00:00Z',
  });
  const refreshed = sidebarRevealTargetKey({
    session_id: 's1',
    workspace_hash: 'w1',
    title: 'Updated title',
    updated_at: '2026-07-30T02:00:00Z',
    attention_state: 'working',
  });
  assert.equal(refreshed, previous);
  assert.notEqual(
    sidebarRevealTargetKey({ sessionId: 's1', workspaceHash: 'w2' }),
    previous,
  );
  assert.notEqual(
    sidebarRevealTargetKey({ sessionId: 's2', workspaceHash: 'w1' }),
    previous,
  );
});

test('sessionListNeedsRevealExpansion expands when target row is hidden', () => {
  const sessions = Array.from({ length: 7 }, (_, i) => ({
    id: String(i),
    workspace_hash: 'w1',
  }));
  assert.equal(sessionListNeedsRevealExpansion(sessions, {
    sessionId: '6',
    workspaceHash: 'w1',
  }, false), true);
  assert.equal(sessionListNeedsRevealExpansion(sessions, {
    sessionId: '3',
    workspaceHash: 'w1',
  }, false), false);
});

test('sessionMatchesRevealTarget separates workspace and no-workspace rows', () => {
  assert.equal(sessionMatchesRevealTarget({
    id: 's1',
    workspace_hash: 'w1',
  }, {
    sessionId: 's1',
    workspaceHash: 'w1',
  }), true);
  assert.equal(sessionMatchesRevealTarget({
    id: 's1',
    workspace_hash: 'w1',
  }, {
    sessionId: 's1',
    noWorkspace: true,
  }), false);
});

test('sortSidebarSessionsNewestFirst orders by updated then created time', () => {
  const result = sortSidebarSessionsNewestFirst([
    { id: 'old', updated_at: '2026-05-17T01:00:00Z' },
    { id: 'new', updated_at: '2026-05-17T03:00:00Z' },
    { id: 'middle', created_at: '2026-05-17T02:00:00Z' },
  ]);
  assert.deepEqual(result.map((s) => s.id), ['new', 'middle', 'old']);
});

test('upsertSidebarSession inserts new session newest-first', () => {
  const result = upsertSidebarSession([
    { id: 'old', workspace_hash: 'w1', updated_at: '2026-05-17T01:00:00Z' },
  ], {
    id: 'fork',
    workspace_hash: 'w1',
    updated_at: '2026-05-17T04:00:00Z',
  });
  assert.deepEqual(result.map((s) => s.id), ['fork', 'old']);
});

test('upsertSidebarSession replaces existing session without duplicates', () => {
  const result = upsertSidebarSession([
    { id: 'other', updated_at: '2026-05-17T02:00:00Z' },
    { id: 'same', title: 'old', updated_at: '2026-05-17T01:00:00Z' },
  ], {
    id: 'same',
    title: 'new',
    updated_at: '2026-05-17T03:00:00Z',
  });
  assert.deepEqual(result.map((s) => s.id), ['other', 'same']);
  assert.equal(result[1].title, 'new');
});

test('upsertSidebarSession promotes existing session only when content counters change', () => {
  const result = upsertSidebarSession([
    { id: 'other', updated_at: '2026-05-17T02:00:00Z' },
    { id: 'same', title: 'old', updated_at: '2026-05-17T01:00:00Z', message_count: 2 },
  ], {
    id: 'same',
    title: 'new',
    updated_at: '2026-05-17T03:00:00Z',
    message_count: 4,
  });
  assert.deepEqual(result.map((s) => s.id), ['same', 'other']);
  assert.equal(result[0].title, 'new');
});

test('reconcileSidebarSessions preserves row order when only updated_at changes', () => {
  const previous = [
    { id: 'a', workspace_hash: 'w1', updated_at: '2026-05-17T01:00:00Z', message_count: 2, turn_count: 1 },
    { id: 'b', workspace_hash: 'w1', updated_at: '2026-05-17T02:00:00Z', message_count: 4, turn_count: 2 },
    { id: 'c', workspace_hash: 'w1', updated_at: '2026-05-17T03:00:00Z', message_count: 6, turn_count: 3 },
  ];
  const incoming = [
    { id: 'c', workspace_hash: 'w1', updated_at: '2026-05-17T09:00:00Z', message_count: 6, turn_count: 3 },
    { id: 'b', workspace_hash: 'w1', updated_at: '2026-05-17T02:00:00Z', message_count: 4, turn_count: 2 },
    { id: 'a', workspace_hash: 'w1', updated_at: '2026-05-17T01:00:00Z', message_count: 2, turn_count: 1 },
  ];
  const result = reconcileSidebarSessions(previous, incoming);
  assert.deepEqual(result.map((s) => s.id), ['a', 'b', 'c']);
  assert.equal(result[2].updated_at, '2026-05-17T09:00:00Z');
});

test('reconcileSidebarSessions promotes content changes and new sessions', () => {
  const previous = [
    { id: 'a', workspace_hash: 'w1', updated_at: '2026-05-17T01:00:00Z', message_count: 2, turn_count: 1 },
    { id: 'b', workspace_hash: 'w1', updated_at: '2026-05-17T02:00:00Z', message_count: 4, turn_count: 2 },
    { id: 'c', workspace_hash: 'w1', updated_at: '2026-05-17T03:00:00Z', message_count: 6, turn_count: 3 },
  ];
  const incoming = [
    { id: 'a', workspace_hash: 'w1', updated_at: '2026-05-17T01:00:00Z', message_count: 2, turn_count: 1 },
    { id: 'b', workspace_hash: 'w1', updated_at: '2026-05-17T10:00:00Z', message_count: 8, turn_count: 3 },
    { id: 'c', workspace_hash: 'w1', updated_at: '2026-05-17T03:00:00Z', message_count: 6, turn_count: 3 },
    { id: 'new', workspace_hash: 'w1', updated_at: '2026-05-17T11:00:00Z', message_count: 0, turn_count: 0 },
  ];
  const result = reconcileSidebarSessions(previous, incoming);
  assert.deepEqual(result.map((s) => s.id), ['new', 'b', 'a', 'c']);
});
