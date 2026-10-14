import { sidebarSessionRoots, sidebarTreeTarget } from './sidebarSessionTree.js';

export const SIDEBAR_SESSION_COLLAPSE_LIMIT = 5;

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

function sessionTime(session) {
  const s = session || {};
  const updated = Date.parse(s.updated_at || s.updatedAt || '');
  if (Number.isFinite(updated)) return updated;
  const created = Date.parse(s.created_at || s.createdAt || '');
  if (Number.isFinite(created)) return created;
  return 0;
}

function numericCounter(value) {
  const n = Number(value);
  return Number.isFinite(n) && n >= 0 ? n : null;
}

function contentCounter(session, snakeName, camelName) {
  const s = session || {};
  return numericCounter(s[snakeName] ?? s[camelName]);
}

function sessionContentChanged(previous, next) {
  const prevMessages = contentCounter(previous, 'message_count', 'messageCount');
  const nextMessages = contentCounter(next, 'message_count', 'messageCount');
  if (prevMessages != null && nextMessages != null && prevMessages !== nextMessages) return true;

  const prevTurns = contentCounter(previous, 'turn_count', 'turnCount');
  const nextTurns = contentCounter(next, 'turn_count', 'turnCount');
  if (prevTurns != null && nextTurns != null && prevTurns !== nextTurns) return true;

  return false;
}

export function sidebarSessionHasWorktree(session = {}) {
  const worktree = session?.worktree;
  if (!worktree || typeof worktree !== 'object') return false;
  return [worktree.name, worktree.branch]
    .some((value) => String(value || '').trim().length > 0);
}

export function sidebarSessionMarker(session = {}) {
  const loopExecution = session?.loop_execution || session?.loopExecution;
  if (loopExecution && typeof loopExecution === 'object') {
    const hasLoopOrigin = [loopExecution.loop_id, loopExecution.loopId, loopExecution.run_id, loopExecution.runId]
      .some((value) => String(value || '').trim().length > 0);
    if (hasLoopOrigin) return 'loop';
  }
  return sidebarSessionHasWorktree(session) ? 'worktree' : '';
}

export function shouldStartRemoteControlSurge(previousBound, nextBound) {
  return !Boolean(previousBound) && Boolean(nextBound);
}

export function remoteControlSurgeTargetKey(session = {}) {
  return sidebarRevealTargetKey(sidebarRevealTarget(session));
}

export function applyRemoteControlSessionSelection(sessions = [], selectedSession = null) {
  const selectedId = sessionId(selectedSession);
  if (!selectedId) return Array.isArray(sessions) ? sessions : [];

  const selected = {
    ...(selectedSession || {}),
    id: selectedId,
    remote_control_bound: true,
    remoteControlBound: true,
  };
  const selectedKey = sessionKey(selected);
  let found = false;
  const cleared = (Array.isArray(sessions) ? sessions : []).map((session) => {
    if (sessionKey(session) === selectedKey) {
      found = true;
      return { ...session, ...selected };
    }
    if (session?.remote_control_bound || session?.remoteControlBound) {
      return {
        ...session,
        remote_control_bound: false,
        remoteControlBound: false,
      };
    }
    return session;
  });

  return found ? cleared : upsertSidebarSession(cleared, selected);
}

export function clearRemoteControlSessionBindings(sessions = []) {
  const source = Array.isArray(sessions) ? sessions : [];
  let changed = false;
  const cleared = source.map((session) => {
    if (!session?.remote_control_bound && !session?.remoteControlBound) return session;
    changed = true;
    return {
      ...session,
      remote_control_bound: false,
      remoteControlBound: false,
    };
  });
  return changed ? cleared : source;
}

export function nextRemoteControlSurgeRequest(currentRequest, targetKey, sequence) {
  const normalizedTargetKey = String(targetKey || '').trim();
  if (!normalizedTargetKey) return currentRequest || null;
  if (currentRequest?.targetKey === normalizedTargetKey) return currentRequest;
  return {
    targetKey: normalizedTargetKey,
    sequence: Math.max(1, Number(sequence) || 1),
  };
}

export function completeRemoteControlSurgeRequest(currentRequest, sequence) {
  return currentRequest?.sequence === sequence ? null : (currentRequest || null);
}

export function shouldRunRemoteControlForcedSurge(
  remoteControlBound,
  scheduledSequence,
  currentSequence,
) {
  const scheduled = Number(scheduledSequence) || 0;
  const current = Number(currentSequence) || 0;
  return Boolean(remoteControlBound) && scheduled > 0 && scheduled === current;
}

export function sidebarWorkspaceListKeys(workspaces = []) {
  const keys = [];
  const seen = new Set();
  const list = Array.isArray(workspaces) ? workspaces : [];
  for (const workspace of list) {
    const hash = typeof workspace === 'string'
      ? workspace.trim()
      : String(
        workspace?.hash
        || workspace?.workspace_hash
        || workspace?.workspaceHash
        || '',
      ).trim();
    if (!hash || seen.has(hash)) continue;
    seen.add(hash);
    keys.push(hash);
  }
  return keys;
}

export function expandedSessionListsAfterWorkspaceCollapseAll(
  currentExpanded = new Map(),
  workspaces = [],
) {
  const next = currentExpanded instanceof Map
    ? new Map(currentExpanded)
    : new Map();
  for (const hash of sidebarWorkspaceListKeys(workspaces)) {
    next.delete(hash);
  }
  return next;
}

export function expandedSessionListsAfterWorkspaceDisclosure(
  currentExpanded = new Map(),
  workspaceHash = '',
) {
  const hash = String(workspaceHash || '').trim();
  const current = currentExpanded instanceof Map ? currentExpanded : new Map();
  if (!hash || !current.has(hash)) return current;
  const next = new Map(current);
  next.delete(hash);
  return next;
}

export function allowSidebarWorkspaceAutoExpand(
  workspaceHash = '',
  {
    noWorkspace = false,
    workspaceCollapseAll = false,
    userCollapsedWorkspaces = new Set(),
  } = {},
) {
  if (noWorkspace) return false;
  const hash = String(workspaceHash || '').trim();
  if (!hash) return false;
  if (workspaceCollapseAll) return false;
  return !(userCollapsedWorkspaces instanceof Set && userCollapsedWorkspaces.has(hash));
}

export function allowSidebarSessionListRevealExpansion({
  listKey = '',
  noWorkspace = false,
  workspaceCollapseAll = false,
  disclosureCompactKeys = new Set(),
} = {}) {
  const key = String(listKey || '').trim();
  if (!key) return false;
  if (disclosureCompactKeys instanceof Set && disclosureCompactKeys.has(key)) return false;
  if (!noWorkspace && workspaceCollapseAll) return false;
  return true;
}

function sidebarSessionLimit(value, fallback = SIDEBAR_SESSION_COLLAPSE_LIMIT) {
  return Number.isFinite(value) && value >= 1 ? Math.floor(value) : fallback;
}

export function sidebarSessionProjection(
  sessions = [],
  visibleLimit = SIDEBAR_SESSION_COLLAPSE_LIMIT,
  limit = SIDEBAR_SESSION_COLLAPSE_LIMIT,
  total = null,
) {
  const list = sidebarSessionRoots(Array.isArray(sessions) ? sessions : []);
  const max = sidebarSessionLimit(limit);
  const visibleCount = Math.max(max, sidebarSessionLimit(visibleLimit, max));
  const knownTotal = Number.isFinite(total) && total > list.length ? Math.floor(total) : list.length;
  const collapsible = knownTotal > max;
  const visibleSessions = list.slice(0, visibleCount);
  const hiddenCount = Math.max(0, knownTotal - visibleSessions.length);
  return {
    visibleSessions,
    collapsible,
    action: collapsible ? (hiddenCount > 0 ? 'expand' : 'collapse') : '',
    hiddenCount,
  };
}

export function sidebarRevealTarget(activeRef = {}) {
  const id = sessionId(activeRef);
  if (!id) return { sessionId: '', workspaceHash: '', noWorkspace: false };
  const noWorkspace = isNoWorkspaceSession(activeRef);
  return {
    sessionId: id,
    workspaceHash: noWorkspace ? '' : sessionWorkspace(activeRef),
    noWorkspace,
  };
}

export function sidebarRevealTargetKey(target = {}) {
  const normalized = sidebarRevealTarget(target);
  if (!normalized.sessionId) return '';
  return [
    normalized.noWorkspace ? 'no-workspace' : 'workspace',
    normalized.workspaceHash,
    normalized.sessionId,
  ].join('\u0000');
}

export function sessionMatchesRevealTarget(session = {}, target = {}) {
  const targetId = String(target?.sessionId || target?.id || target?.session_id || '').trim();
  if (!targetId || sessionId(session) !== targetId) return false;
  const targetNoWorkspace = !!(target?.noWorkspace || target?.no_workspace);
  if (targetNoWorkspace) return isNoWorkspaceSession(session) || !sessionWorkspace(session);
  const targetWorkspace = String(target?.workspaceHash || target?.workspace_hash || '').trim();
  if (!targetWorkspace) return !isNoWorkspaceSession(session);
  return sessionWorkspace(session) === targetWorkspace;
}

export function sessionListNeedsRevealExpansion(
  sessions = [],
  target = {},
  visibleLimit = SIDEBAR_SESSION_COLLAPSE_LIMIT,
  limit = SIDEBAR_SESSION_COLLAPSE_LIMIT,
) {
  if (!target?.sessionId) return false;
  const root = sidebarTreeTarget(sessions, target?.sessionId);
  const rootTarget = root ? { ...target, sessionId: root.id } : target;
  const projection = sidebarSessionProjection(sessions, visibleLimit, limit);
  if (!projection.collapsible) return false;
  const list = Array.isArray(sessions) ? sessions : [];
  const hasTarget = list.some((session) => sessionMatchesRevealTarget(session, target));
  if (!hasTarget) return false;
  return !projection.visibleSessions.some((session) => sessionMatchesRevealTarget(session, rootTarget));
}

export function sidebarSessionRevealLimit(sessions = [], target = {}, limit = SIDEBAR_SESSION_COLLAPSE_LIMIT) {
  const batchSize = sidebarSessionLimit(limit);
  const list = sidebarSessionRoots(Array.isArray(sessions) ? sessions : []);
  const root = sidebarTreeTarget(sessions, target?.sessionId);
  const rootTarget = root ? { ...target, sessionId: root.id } : target;
  const targetIndex = list.findIndex((session) => sessionMatchesRevealTarget(session, rootTarget));
  return Math.max(batchSize, Math.ceil((targetIndex + 1) / batchSize) * batchSize);
}

export function sortSidebarSessionsNewestFirst(sessions = []) {
  const list = Array.isArray(sessions) ? sessions : [];
  return list
    .map((session, index) => ({ session, index, time: sessionTime(session) }))
    .sort((a, b) => {
      if (a.time !== b.time) return b.time - a.time;
      return a.index - b.index;
    })
    .map((item) => item.session);
}

export function reorderSidebarWorkspaceSession(
  sessions = [],
  workspaceHash = '',
  sourceId = '',
  targetId = '',
  placement = 'before',
) {
  const list = Array.isArray(sessions) ? sessions : [];
  const workspace = String(workspaceHash || '').trim();
  const source = String(sourceId || '').trim();
  const target = String(targetId || '').trim();
  if (!workspace || !source || !target || source === target) return list;

  const workspaceIndexes = [];
  const workspaceSessions = [];
  list.forEach((session, index) => {
    if (!isNoWorkspaceSession(session) && sessionWorkspace(session) === workspace) {
      workspaceIndexes.push(index);
      workspaceSessions.push(session);
    }
  });

  const sourceIndex = workspaceSessions.findIndex((session) => sessionId(session) === source);
  const targetIndex = workspaceSessions.findIndex((session) => sessionId(session) === target);
  if (sourceIndex < 0 || targetIndex < 0) return list;

  const reordered = [...workspaceSessions];
  const [sourceSession] = reordered.splice(sourceIndex, 1);
  const targetAfterRemoval = reordered.findIndex((session) => sessionId(session) === target);
  if (targetAfterRemoval < 0) return list;
  reordered.splice(targetAfterRemoval + (placement === 'after' ? 1 : 0), 0, sourceSession);

  const next = [...list];
  workspaceIndexes.forEach((index, workspaceIndex) => {
    next[index] = reordered[workspaceIndex];
  });
  return next;
}

export function reconcileSidebarSessions(previousSessions = [], incomingSessions = []) {
  const previous = Array.isArray(previousSessions) ? previousSessions : [];
  const incoming = Array.isArray(incomingSessions) ? incomingSessions : [];
  if (previous.length === 0) return sortSidebarSessionsNewestFirst(incoming);

  const previousByKey = new Map();
  for (const session of previous) {
    const key = sessionKey(session);
    if (key && !previousByKey.has(key)) previousByKey.set(key, session);
  }

  const incomingByKey = new Map();
  for (const session of incoming) {
    const key = sessionKey(session);
    if (key) incomingByKey.set(key, session);
  }

  // 每个工作区当前显示的最新活动时间。新出现的会话只有比它更新(新建、或刚有
  // 动静)才顶到最前;更旧的是「补位」—— 折叠态只取最新 5 条,归档 / 删除一条后
  // 下一条旧会话被拉进来。补位按时间插回原位,不能顶到最前:否则 3 天前的会话
  // 会在归档别的会话时突然窜到顶部(LIUXIN557 反馈)。
  const newestPreviousByWorkspace = new Map();
  for (const session of previous) {
    const workspace = sessionWorkspace(session);
    const time = sessionTime(session);
    if (!newestPreviousByWorkspace.has(workspace) || time > newestPreviousByWorkspace.get(workspace)) {
      newestPreviousByWorkspace.set(workspace, time);
    }
  }
  // 只有双方都有时间戳、且严格更旧才算补位;没有时间戳的保持原来的置顶行为。
  const isBackfill = (session) => {
    const newest = newestPreviousByWorkspace.get(sessionWorkspace(session)) || 0;
    const time = sessionTime(session);
    return newest > 0 && time > 0 && time < newest;
  };

  const promoted = new Set();
  const top = [];
  const backfill = [];
  for (const session of sortSidebarSessionsNewestFirst(incoming)) {
    const key = sessionKey(session);
    if (!key || promoted.has(key)) continue;
    const previousSession = previousByKey.get(key);
    if (!previousSession && isBackfill(session)) {
      promoted.add(key);
      backfill.push(session);
    } else if (!previousSession || sessionContentChanged(previousSession, session)) {
      promoted.add(key);
      top.push(session);
    }
  }

  const stable = [];
  const emitted = new Set(promoted);
  for (const session of previous) {
    const key = sessionKey(session);
    if (!key || emitted.has(key)) continue;
    const next = incomingByKey.get(key);
    if (!next) continue;
    emitted.add(key);
    stable.push(next);
  }

  for (const session of backfill) {
    const time = sessionTime(session);
    const index = stable.findIndex((existing) => sessionTime(existing) < time);
    if (index < 0) stable.push(session);
    else stable.splice(index, 0, session);
  }

  return [...top, ...stable];
}

export function upsertSidebarSession(sessions = [], nextSession = null, options = {}) {
  const id = sessionId(nextSession);
  if (!id) return Array.isArray(sessions) ? sessions : [];

  const list = Array.isArray(sessions) ? sessions : [];
  const promoteToTop = options?.promoteToTop === true;
  const nextKey = sessionKey(nextSession);
  const existingIndex = list.findIndex((session) => sessionKey(session) === nextKey);
  if (existingIndex < 0) {
    const normalized = { ...nextSession, id };
    return promoteToTop
      ? [normalized, ...list]
      : sortSidebarSessionsNewestFirst([...list, normalized]);
  }

  const existing = list[existingIndex];
  const merged = { ...existing, ...nextSession, id };
  const remaining = list.filter((session, index) => (
    promoteToTop ? sessionKey(session) !== nextKey : index !== existingIndex
  ));
  if (promoteToTop || sessionContentChanged(existing, merged)) return [merged, ...remaining];
  return [
    ...list.slice(0, existingIndex),
    merged,
    ...list.slice(existingIndex + 1),
  ];
}
