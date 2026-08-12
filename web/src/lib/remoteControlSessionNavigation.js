export const REMOTE_CONTROL_SESSION_SELECTED_TYPE = 'remote_control_session_selected';

function text(value) {
  return String(value == null ? '' : value).trim();
}

function sessionId(value = {}) {
  return text(value.sessionId || value.session_id || value.id);
}

function noWorkspace(value = {}) {
  return Boolean(value.noWorkspace || value.no_workspace);
}

function workspaceHash(value = {}) {
  return noWorkspace(value)
    ? ''
    : text(value.workspaceHash || value.workspace_hash || value.hash);
}

// Normalize the daemon's optional navigation hint into the same target shape
// consumed by resumeAndOpenSession and Sidebar. A nonce is mandatory so a
// repeated switch to the same row can replay the one-shot surge.
export function normalizeRemoteControlSessionSelection(message = {}) {
  if (message?.type !== REMOTE_CONTROL_SESSION_SELECTED_TYPE) return null;
  const payload = message?.payload;
  if (!payload || typeof payload !== 'object') return null;
  const id = sessionId(payload);
  const selectionNonce = text(payload.selection_nonce ?? payload.selectionNonce);
  if (!id || !selectionNonce) return null;
  const isNoWorkspace = noWorkspace(payload);
  const hash = isNoWorkspace ? '' : workspaceHash(payload);
  return {
    id,
    sessionId: id,
    workspaceHash: hash,
    workspace_hash: hash,
    cwd: text(payload.cwd),
    title: text(payload.title),
    noWorkspace: isNoWorkspace,
    no_workspace: isNoWorkspace,
    active: true,
    remote_control_bound: payload.remote_control_bound !== false,
    selectionNonce,
  };
}

export function remoteControlSelectionMatchesSession(selection, session) {
  if (!selection || !session) return false;
  if (sessionId(selection) !== sessionId(session)) return false;
  const selectionNoWorkspace = noWorkspace(selection);
  const sessionNoWorkspace = noWorkspace(session);
  if (selectionNoWorkspace || sessionNoWorkspace) {
    return selectionNoWorkspace === sessionNoWorkspace;
  }
  const selectedWorkspace = workspaceHash(selection);
  const rowWorkspace = workspaceHash(session);
  return !selectedWorkspace || !rowWorkspace || selectedWorkspace === rowWorkspace;
}

export function remoteControlSurgeNonceForSession(selection, session) {
  if (!remoteControlSelectionMatchesSession(selection, session)) return '';
  return text(selection.selectionNonce || selection.selection_nonce);
}
