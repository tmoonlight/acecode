import assert from 'node:assert/strict';
import {
  normalizeRemoteControlSessionSelection,
  remoteControlSelectionMatchesSession,
  remoteControlSurgeNonceForSession,
} from './remoteControlSessionNavigation.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('remote-control selection event normalizes into an already-active jump target', () => {
  const target = normalizeRemoteControlSessionSelection({
    type: 'remote_control_session_selected',
    payload: {
      session_id: 'session-2',
      workspace_hash: 'workspace-b',
      cwd: 'C:/work/b',
      title: 'Second task',
      remote_control_bound: true,
      selection_nonce: 17,
    },
  });
  assert.deepEqual(target, {
    id: 'session-2',
    sessionId: 'session-2',
    workspaceHash: 'workspace-b',
    workspace_hash: 'workspace-b',
    cwd: 'C:/work/b',
    title: 'Second task',
    noWorkspace: false,
    no_workspace: false,
    active: true,
    remote_control_bound: true,
    selectionNonce: '17',
  });
});

run('remote-control selection rejects unrelated or replay-unsafe events', () => {
  assert.equal(normalizeRemoteControlSessionSelection({ type: 'done', payload: {} }), null);
  assert.equal(normalizeRemoteControlSessionSelection({
    type: 'remote_control_session_selected',
    payload: { session_id: 'session-2' },
  }), null);
});

run('remote-control surge matches exact workspace and no-workspace targets', () => {
  const workspaceSelection = {
    sessionId: 'session-2',
    workspaceHash: 'workspace-b',
    selectionNonce: '18',
  };
  assert.equal(remoteControlSelectionMatchesSession(
    workspaceSelection,
    { id: 'session-2', workspace_hash: 'workspace-b' },
  ), true);
  assert.equal(remoteControlSelectionMatchesSession(
    workspaceSelection,
    { id: 'session-2', workspace_hash: 'workspace-a' },
  ), false);
  assert.equal(remoteControlSurgeNonceForSession(
    workspaceSelection,
    { id: 'session-2', workspace_hash: 'workspace-b' },
  ), '18');

  const noWorkspaceSelection = {
    sessionId: 'session-3',
    noWorkspace: true,
    selectionNonce: '19',
  };
  assert.equal(remoteControlSelectionMatchesSession(
    noWorkspaceSelection,
    { id: 'session-3', no_workspace: true },
  ), true);
  assert.equal(remoteControlSelectionMatchesSession(
    noWorkspaceSelection,
    { id: 'session-3', workspace_hash: 'workspace-a' },
  ), false);
});
