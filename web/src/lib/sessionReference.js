import { mergeSessionContentMatches, rankSessions } from './searchSessions.js';
import { sessionDisplayTitle, withNewSessionDisplayTitles } from './sessionTitle.js';

export const SESSION_REFERENCE_PREFIX = '@session:';
export const RECENT_SESSION_REFERENCE_LIMIT = 10;
export const SESSION_REFERENCE_VERSION = 1;
export const SESSION_REFERENCE_CACHE_TTL_MS = 60_000;
// 空查询只要最近会话:服务端对空查询本来就最多给 50 条。
export const SESSION_REFERENCE_RECENT_FETCH_LIMIT = 50;
// 有查询词时取服务端按相关度排好的前 100 条(服务端上限),下拉框足够用。
export const SESSION_REFERENCE_QUERY_LIMIT = 100;
// 同一个查询词短时间内复用结果:光标在 @ 词里移动、退格又敲回来都不再请求。
export const SESSION_REFERENCE_QUERY_CACHE_TTL_MS = 15_000;
const SESSION_REFERENCE_QUERY_CACHE_MAX = 32;
// daemon 刚启动时全局会话目录还在后台预热,结果不完整;下拉框开着就按这个
// 间隔重查,直到目录完整。每次重查只是内存过滤,有上限防止无限轮询。
export const SESSION_REFERENCE_CATALOG_POLL_MS = 250;
export const SESSION_REFERENCE_CATALOG_MAX_POLLS = 120;

const sessionDataCache = new WeakMap();
const sessionQueryCache = new WeakMap();

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

function catalogComplete(data) {
  return !data?.progress || data.progress.complete !== false;
}

function hasGlobalSessionSearch(api) {
  return !!api && typeof api === 'object' && typeof api.listGlobalSessions === 'function';
}

export function canLoadSessionReferences(api) {
  return hasGlobalSessionSearch(api)
    || (!!api && typeof api === 'object' && typeof api.listAllWorkspaceSessions === 'function');
}

function waitForCatalogPoll(ms, signal) {
  return new Promise((resolve) => {
    if (signal?.aborted) {
      resolve();
      return;
    }
    const finish = () => {
      clearTimeout(timer);
      signal?.removeEventListener?.('abort', finish);
      resolve();
    };
    const timer = setTimeout(finish, ms);
    signal?.addEventListener?.('abort', finish, { once: true });
  });
}

// 空查询:最近会话。只需要会话目录,不再顺带请求 /api/workspaces(@ 菜单用不到项目列表)。
export async function loadSessionReferenceData(api, {
  now = Date.now(),
  ttlMs = SESSION_REFERENCE_CACHE_TTL_MS,
} = {}) {
  if (!canLoadSessionReferences(api)) {
    return { sessions: [], workspaces: [], errors: [] };
  }
  const cached = sessionDataCache.get(api);
  if (cached?.data && now - cached.ts < ttlMs) return cached.data;
  if (cached?.promise) return cached.promise;
  const request = hasGlobalSessionSearch(api)
    ? () => api.listGlobalSessions({ limit: SESSION_REFERENCE_RECENT_FETCH_LIMIT })
    : () => api.listAllWorkspaceSessions();
  const promise = Promise.resolve(request())
    .then((result) => {
      const data = result && typeof result === 'object'
        ? result
        : { sessions: [], workspaces: [], errors: [] };
      // 目录还在预热时的结果不完整,只保留给本次使用,下一次按键重新取。
      sessionDataCache.set(api, {
        ts: catalogComplete(data) ? Date.now() : 0,
        data,
        promise: null,
      });
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
  if (api && typeof api === 'object') {
    sessionDataCache.delete(api);
    sessionQueryCache.delete(api);
  }
}

function rememberQueryResult(api, query, data) {
  let cache = sessionQueryCache.get(api);
  if (!cache) {
    cache = new Map();
    sessionQueryCache.set(api, cache);
  }
  cache.delete(query);
  cache.set(query, { ts: Date.now(), data });
  while (cache.size > SESSION_REFERENCE_QUERY_CACHE_MAX) {
    cache.delete(cache.keys().next().value);
  }
}

// 有查询词:交给服务端在全量会话目录里过滤(已预热的内存目录,毫秒级)。
// 不能拿空查询的「最近 50 条」在本地过滤 —— 那样更早的会话永远搜不到
// (实测 2.8 万个会话里,标题完全一致的会话排在第 9726 位,@ 菜单显示「没有匹配」)。
// 正文全文搜索要逐项目读 meta、开 SQLite,全量一遍要几十秒,不在 @ 菜单里按键触发;
// 最近一条用户消息已在 summary 里,由这里的元数据检索覆盖,全文搜索留给 Ctrl+K。
export async function searchSessionReferenceData(api, query, {
  signal,
  now = Date.now(),
  ttlMs = SESSION_REFERENCE_QUERY_CACHE_TTL_MS,
  pollMs = SESSION_REFERENCE_CATALOG_POLL_MS,
  maxPolls = SESSION_REFERENCE_CATALOG_MAX_POLLS,
  onPartial,
} = {}) {
  const normalized = String(query || '').trim();
  // 空查询、或没有服务端检索的旧接口:退回最近会话列表本地过滤。
  if (!normalized || !hasGlobalSessionSearch(api)) {
    return loadSessionReferenceData(api, { now });
  }
  const cached = sessionQueryCache.get(api)?.get(normalized);
  if (cached && now - cached.ts < ttlMs) return cached.data;

  let data = { sessions: [], progress: null };
  for (let poll = 0; poll < Math.max(1, maxPolls); poll += 1) {
    if (poll > 0) {
      onPartial?.(data);
      await waitForCatalogPoll(pollMs, signal);
      if (signal?.aborted) return data;
    }
    const payload = await api.listGlobalSessions({
      query: normalized,
      limit: SESSION_REFERENCE_QUERY_LIMIT,
      signal,
    });
    data = {
      sessions: Array.isArray(payload?.sessions) ? payload.sessions : [],
      progress: payload?.progress || null,
    };
    if (catalogComplete(data)) {
      rememberQueryResult(api, normalized, data);
      return data;
    }
  }
  return data;
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
