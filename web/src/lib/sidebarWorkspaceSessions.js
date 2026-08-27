import { normalizePinnedIds } from './pinnedSessions.js';
import {
  reconcileSidebarSessions,
  SIDEBAR_SESSION_COLLAPSE_LIMIT,
} from './sidebarSessions.js';

function sessionId(session) {
  return String(session?.id || session?.session_id || session?.sessionId || '').trim();
}

function sessionWorkspace(session) {
  return String(session?.workspace_hash || session?.workspaceHash || '').trim();
}

function isNoWorkspaceSession(session) {
  return !!(session?.noWorkspace || session?.no_workspace);
}

function sessionKey(session) {
  const id = sessionId(session);
  return id ? `${sessionWorkspace(session)}\u0000${id}` : '';
}

function pinnedIdsForWorkspace(pinnedByWorkspace, workspaceHash) {
  const hash = String(workspaceHash || '').trim();
  if (!hash) return new Set();
  if (pinnedByWorkspace instanceof Map) {
    return new Set(normalizePinnedIds(pinnedByWorkspace.get(hash) || []));
  }
  if (pinnedByWorkspace && typeof pinnedByWorkspace === 'object') {
    return new Set(normalizePinnedIds(pinnedByWorkspace[hash] || []));
  }
  return new Set();
}

export function normalizeWorkspaceSessionListResponse(data) {
  if (Array.isArray(data)) {
    return { sessions: data, total: data.length };
  }
  const sessions = Array.isArray(data?.sessions) ? data.sessions : [];
  const rawTotal = Number(data?.total);
  const total = Number.isFinite(rawTotal) && rawTotal >= sessions.length
    ? Math.floor(rawTotal)
    : sessions.length;
  return { sessions, total };
}

export function sidebarWorkspaceSessionListQuery({ full = false } = {}) {
  if (full) return {};
  return { limit: SIDEBAR_SESSION_COLLAPSE_LIMIT };
}

export function retainUnrefreshedSidebarSessions(
  previousSessions = [],
  incomingSessions = [],
  options = {},
) {
  const previous = Array.isArray(previousSessions) ? previousSessions : [];
  const incoming = Array.isArray(incomingSessions) ? incomingSessions : [];
  const refreshed = new Set(
    Array.from(options.refreshedWorkspaceHashes || []).map((hash) => String(hash || '').trim()).filter(Boolean),
  );
  const refreshNoWorkspace = options.refreshNoWorkspace === true;
  const incomingKeys = new Set();
  for (const session of incoming) {
    const key = sessionKey(session);
    if (key) incomingKeys.add(key);
  }

  const retained = [];
  for (const session of previous) {
    const key = sessionKey(session);
    if (!key || incomingKeys.has(key)) continue;
    if (isNoWorkspaceSession(session)) {
      if (!refreshNoWorkspace) retained.push(session);
      continue;
    }
    const hash = sessionWorkspace(session);
    if (!hash || !refreshed.has(hash)) {
      retained.push(session);
      continue;
    }
    if (pinnedIdsForWorkspace(options.pinnedByWorkspace, hash).has(sessionId(session))) {
      retained.push(session);
    }
  }

  return reconcileSidebarSessions(previous, [...incoming, ...retained]);
}

export function workspaceHasCachedSidebarSessions(sessions = [], workspaceHash = '') {
  const hash = String(workspaceHash || '').trim();
  if (!hash) return false;
  return (Array.isArray(sessions) ? sessions : []).some((session) => (
    !isNoWorkspaceSession(session) && sessionWorkspace(session) === hash
  ));
}
