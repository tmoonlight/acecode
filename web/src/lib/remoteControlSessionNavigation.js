function text(value) {
  return String(value == null ? '' : value).trim();
}

function sourceObject(value) {
  return value && typeof value === 'object' && !Array.isArray(value) ? value : {};
}

function firstText(...values) {
  for (const value of values) {
    const normalized = text(value);
    if (normalized) return normalized;
  }
  return '';
}

// The server event is an optional navigation hint.  Keep its untrusted shape
// narrow: only session-routing/display fields become part of UI state.
export function normalizeRemoteControlSessionSelected(message = {}) {
  const envelope = sourceObject(message);
  if (envelope.type !== 'remote_control_session_selected') return null;

  const payload = sourceObject(envelope.payload);
  const sessionId = firstText(payload.session_id, payload.sessionId, envelope.session_id, envelope.sessionId);
  if (!sessionId) return null;

  const noWorkspace = payload.no_workspace === true || payload.noWorkspace === true
    || envelope.no_workspace === true || envelope.noWorkspace === true;
  const workspaceHash = noWorkspace
    ? ''
    : firstText(payload.workspace_hash, payload.workspaceHash, envelope.workspace_hash, envelope.workspaceHash);
  const cwd = noWorkspace ? '' : firstText(payload.cwd, envelope.cwd);
  const title = firstText(payload.title, envelope.title);
  const updatedAt = firstText(payload.updated_at, payload.updatedAt, envelope.updated_at, envelope.updatedAt);

  const session = {
    id: sessionId,
    session_id: sessionId,
    workspace_hash: workspaceHash,
    no_workspace: noWorkspace,
    remote_control_bound: true,
  };
  if (cwd) session.cwd = cwd;
  if (title) session.title = title;
  if (updatedAt) session.updated_at = updatedAt;

  return {
    sessionId,
    workspaceHash,
    noWorkspace,
    // The daemon has already resumed and bound this session before emitting
    // the event.  resumeAndOpenSession therefore only commits the UI ref.
    active: true,
    remote_control_bound: true,
    cwd,
    title,
    updated_at: updatedAt,
    session,
  };
}
