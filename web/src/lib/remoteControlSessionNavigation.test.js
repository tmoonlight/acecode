import assert from 'node:assert/strict';
import { normalizeRemoteControlSessionSelected } from './remoteControlSessionNavigation.js';

function test(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

test('normalizes the selected-session payload into a session jump target', () => {
  const result = normalizeRemoteControlSessionSelected({
    type: 'remote_control_session_selected',
    payload: {
      session_id: ' session-7 ',
      workspace_hash: 'workspace-1',
      cwd: 'C:/work/project',
      title: 'Remote task',
      updated_at: '2026-08-05T12:00:00Z',
      remote_control_bound: false,
      token: 'must-not-leak',
    },
  });

  assert.deepEqual(result, {
    sessionId: 'session-7',
    workspaceHash: 'workspace-1',
    noWorkspace: false,
    active: true,
    cwd: 'C:/work/project',
    title: 'Remote task',
    updated_at: '2026-08-05T12:00:00Z',
    session: {
      id: 'session-7',
      session_id: 'session-7',
      workspace_hash: 'workspace-1',
      no_workspace: false,
      remote_control_bound: true,
      cwd: 'C:/work/project',
      title: 'Remote task',
      updated_at: '2026-08-05T12:00:00Z',
    },
  });
  assert.equal(Object.hasOwn(result.session, 'token'), false);
});

test('accepts a top-level session id and normalizes no-workspace selection', () => {
  const result = normalizeRemoteControlSessionSelected({
    type: 'remote_control_session_selected',
    session_id: 'session-8',
    payload: {
      workspace_hash: 'ignored-when-no-workspace',
      no_workspace: true,
    },
  });

  assert.equal(result.sessionId, 'session-8');
  assert.equal(result.workspaceHash, '');
  assert.equal(result.noWorkspace, true);
  assert.equal(result.active, true);
  assert.deepEqual(result.session, {
    id: 'session-8',
    session_id: 'session-8',
    workspace_hash: '',
    no_workspace: true,
    remote_control_bound: true,
  });
});

test('rejects unrelated events and selected-session events without an id', () => {
  assert.equal(normalizeRemoteControlSessionSelected({ type: 'message', payload: { session_id: 's1' } }), null);
  assert.equal(normalizeRemoteControlSessionSelected({ type: 'remote_control_session_selected', payload: {} }), null);
  assert.equal(normalizeRemoteControlSessionSelected(null), null);
});
