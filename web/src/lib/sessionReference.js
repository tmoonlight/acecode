import { mergeSessionContentMatches, rankSessions } from './searchSessions.js';
import { sessionDisplayTitle, withNewSessionDisplayTitles } from './sessionTitle.js';

export const SESSION_REFERENCE_PREFIX = '@session:';
export const RECENT_SESSION_REFERENCE_LIMIT = 10;
export const SESSION_REFERENCE_VERSION = 1;
export const SESSION_REFERENCE_CACHE_TTL_MS = 60_000;

const sessionDataCache = new WeakMap();

function stringField(value) {
  return typeof value === 'string' ? value.trim() : '';
}

function sessionId(session = {}) {
  return stringField(session.id || session.session_id || session.sessionId);
}

function workspaceHash(session = {}) {
  return stringField(session.workspace_hash || session.workspaceHash);
}

function noWorkspace(session = {}) {
  return !!(session.no_workspace || session.noWorkspace);
}

function parentSessionId(session = {}) {
  return stringField(
    session.parent_session_id
    || session.parentSessionId
    || session.parent_id
    || session.parentId,
  );
}

function isArchived(session = {}) {
  return !!(session.archived || session.is_archived || session.isArchived)
    || String(session.status || '').toLowerCase() === 'archived';
}

function safeDecode(value) {
  try {
    return decodeURIComponent(value);
  } catch {
    return '';
  }
}

export async function loadSessionReferenceData(api, {
  now = Date.now(),
  ttlMs = SESSION_REFERENCE_CACHE_TTL_MS,
} = {}) {
  if (!api || typeof api !== 'object' || typeof api.listAllWorkspaceSessions !== 'function') {
    return { sessions: [], workspaces: [], errors: [] };
  }
  const cached = sessionDataCache.get(api);
  if (cached?.data && now - cached.ts < ttlMs) return cached.data;
  if (cached?.promise) return cached.promise;
  const promise = Promise.resolve(api.listAllWorkspaceSessions())
    .then((result) => {
      const data = result && typeof result === 'object'
        ? result
        : { sessions: [], workspaces: [], errors: [] };
      sessionDataCache.set(api, { ts: Date.now(), data, promise: null });
      return data;
    })
    .catch((error) => {
      sessionDataCache.delete(api);
      throw error;
    });
  sessionDataCache.set(api, { ts: cached?.ts || 0, data: cached?.data || null, promise });
  return promise;
}

export function invalidateSessionReferenceData(api) {
  if (api && typeof api === 'object') sessionDataCache.delete(api);
}

function normalizedReference(value = {}) {
  const id = stringField(value.id || value.session_id || value.sessionId || value.i);
  if (!id) return null;
  const title = stringField(value.title || value.t) || id;
  const isNoWorkspace = !!(value.no_workspace || value.noWorkspace || value.n);
  return {
    session_id: id,
    workspace_hash: isNoWorkspace
      ? ''
      : stringField(value.workspace_hash || value.workspaceHash || value.h),
    no_workspace: isNoWorkspace,
    title,
    workspace_name: stringField(value.workspace_name || value.workspaceName || value.w),
  };
}

export function normalizeSessionReferenceCandidate(session = {}, noWorkspaceLabel = '任务') {
  const reference = normalizedReference({
    ...session,
    title: sessionDisplayTitle(session, ''),
    workspace_name: noWorkspace(session)
      ? noWorkspaceLabel
      : stringField(session.workspaceName || session.workspace_name || session.cwd),
  });
  if (!reference) return null;
  return {
    ...session,
    kind: 'session',
    id: reference.session_id,
    title: reference.title,
    workspace_hash: reference.workspace_hash,
    no_workspace: reference.no_workspace,
    workspaceName: reference.workspace_name,
  };
}

export function eligibleSessionReferenceCandidates(
  sessions = [],
  currentSessionId = '',
  noWorkspaceLabel = '任务',
) {
  const current = stringField(currentSessionId);
  const seen = new Set();
  return withNewSessionDisplayTitles(Array.isArray(sessions) ? sessions : [])
    .filter((session) => sessionId(session))
    .filter((session) => sessionId(session) !== current)
    .filter((session) => !isArchived(session) && !parentSessionId(session))
    .map((session) => normalizeSessionReferenceCandidate(session, noWorkspaceLabel))
    .filter((session) => {
      if (!session) return false;
      const key = `${session.no_workspace ? 'task' : session.workspace_hash}::${session.id}`;
      if (seen.has(key)) return false;
      seen.add(key);
      return true;
    });
}

export function rankSessionReferenceCandidates({
  sessions = [],
  contentMatches = [],
  query = '',
  currentSessionId = '',
  noWorkspaceLabel = '任务',
  now = Date.now(),
} = {}) {
  const eligible = eligibleSessionReferenceCandidates(
    sessions,
    currentSessionId,
    noWorkspaceLabel,
  );
  const eligibleKeys = new Set(eligible.map((session) => (
    `${session.no_workspace ? 'task' : session.workspace_hash}::${session.id}`
  )));
  const normalizedMatches = eligibleSessionReferenceCandidates(
    contentMatches,
    currentSessionId,
    noWorkspaceLabel,
  ).filter((session) => eligibleKeys.has(
    `${session.no_workspace ? 'task' : session.workspace_hash}::${session.id}`,
  ));
  const merged = mergeSessionContentMatches(eligible, normalizedMatches);
  const ranked = rankSessions(merged, query, now);
  return String(query || '').trim()
    ? ranked
    : ranked.slice(0, RECENT_SESSION_REFERENCE_LIMIT);
}

export function formatSessionReferenceToken(session = {}, { trailingSpace = true } = {}) {
  const reference = normalizedReference(session);
  if (!reference) return '';
  const payload = {
    v: SESSION_REFERENCE_VERSION,
    i: reference.session_id,
    h: reference.workspace_hash,
    n: reference.no_workspace ? 1 : 0,
    t: reference.title,
    w: reference.workspace_name,
  };
  const token = `${SESSION_REFERENCE_PREFIX}${encodeURIComponent(JSON.stringify(payload))}`;
  return trailingSpace ? `${token} ` : token;
}

export function parseSessionReferenceToken(token = '') {
  const input = String(token || '');
  if (!input.startsWith(SESSION_REFERENCE_PREFIX)) return null;
  const encoded = input.slice(SESSION_REFERENCE_PREFIX.length);
  const decoded = safeDecode(encoded);
  if (!decoded) return null;
  try {
    const payload = JSON.parse(decoded);
    if (Number(payload?.v) !== SESSION_REFERENCE_VERSION) return null;
    return normalizedReference(payload);
  } catch {
    return null;
  }
}

export function sessionReferenceTokenAt(text = '', at = 0) {
  const input = String(text || '');
  const begin = Number.isFinite(at) ? Math.max(0, Math.min(input.length, at)) : 0;
  if (!input.startsWith(SESSION_REFERENCE_PREFIX, begin)) return null;
  let end = begin + SESSION_REFERENCE_PREFIX.length;
  while (end < input.length && !/\s/.test(input[end])) end += 1;
  const token = input.slice(begin, end);
  const reference = parseSessionReferenceToken(token);
  return reference ? { begin, end, token, reference } : null;
}

export function replaceQueryWithSessionReference(text, queryToken, session) {
  const input = String(text || '');
  const begin = Math.max(0, Math.min(input.length, Number(queryToken?.begin) || 0));
  const end = Math.max(begin, Math.min(input.length, Number(queryToken?.end) || begin));
  const replacement = formatSessionReferenceToken(session);
  if (!replacement) return { text: input, cursor: begin };
  return {
    text: input.slice(0, begin) + replacement + input.slice(end),
    cursor: begin + replacement.length,
  };
}

export function extractSessionReferences(text = '') {
  const input = String(text || '');
  const references = [];
  const seen = new Set();
  let displayText = '';
  let cursor = 0;
  while (cursor < input.length) {
    const at = input.indexOf(SESSION_REFERENCE_PREFIX, cursor);
    if (at < 0) break;
    const parsed = sessionReferenceTokenAt(input, at);
    if (!parsed) {
      displayText += input.slice(cursor, at + 1);
      cursor = at + 1;
      continue;
    }
    displayText += input.slice(cursor, at);
    displayText += `@${parsed.reference.title}`;
    const key = `${parsed.reference.no_workspace ? 'task' : parsed.reference.workspace_hash}`
      + `::${parsed.reference.session_id}`;
    if (!seen.has(key)) {
      seen.add(key);
      references.push(parsed.reference);
    }
    cursor = parsed.end;
  }
  displayText += input.slice(cursor);
  return { displayText, references };
}
