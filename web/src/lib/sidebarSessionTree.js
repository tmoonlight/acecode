import { applyStatusUpdate, mergeSessionsWithStatus, sessionAttentionState } from './sessionStatus.js';

export function sidebarSessionStatusRows(sessions = []) {
  const rows = new Map();
  for (const session of sessions) {
    if (!session.sidebar_group) rows.set(session.id, session);
  }
  for (const session of sessions) {
    if (session.sidebar_group) {
      for (const member of session.sidebar_members || []) rows.set(member.id, { ...rows.get(member.id), ...member });
    }
  }
  return Array.from(rows.values());
}

export function applySidebarSessionStatuses(statusMap, sessions) {
  return sidebarSessionStatusRows(sessions).reduce((map, session) => applyStatusUpdate(map, {
    ...session, session_id: session.id,
    state: session.attention_state || session.read_state || session.state,
    cursor: session.status_cursor ?? session.cursor,
  }), statusMap);
}

export function mergeSidebarSessionStatuses(sessions, statusMap) {
  return mergeSessionsWithStatus(sessions, statusMap).map(session => {
    if (!session.sidebar_group) return session;
    const unread = (session.sidebar_members || []).some(member => (
      sessionAttentionState(statusMap?.get(member.id) || member) === 'unread'
    ));
    return { ...session, busy: false, status: 'idle', attention_state: unread ? 'unread' : 'read', read_state: unread ? 'unread' : 'read' };
  });
}

export function sidebarNodeKey(session) {
  return session?.sidebar_key || `session:${session?.id || ''}`;
}

export function sidebarSessionRoots(sessions = []) {
  const roots = [];
  const children = new Map();
  for (const session of sessions) {
    const parent = session.sidebar_parent_key || '';
    if (parent) {
      if (!children.has(parent)) children.set(parent, []);
      children.get(parent).push(session);
    } else roots.push(session);
  }
  const sort = (rows) => rows.sort((a, b) => (
    Number.isFinite(a.sidebar_rank) && Number.isFinite(b.sidebar_rank)
      ? a.sidebar_rank - b.sidebar_rank : 0
  ));
  return sort(roots).map((session) => children.has(sidebarNodeKey(session)) || session.sidebar_child_count > 0
    ? { ...session, sidebar_children: sort(children.get(sidebarNodeKey(session)) || []) }
    : session);
}

export function sidebarTreeTarget(sessions, sessionId) {
  const session = sessions.find(row => row.id === sessionId);
  if (!session?.sidebar_parent_key) return session;
  return sessions.find(row => sidebarNodeKey(row) === session.sidebar_parent_key) || session;
}

export function validSidebarTreeDrop(source, target, placement, targetParent = null) {
  if (!source || !target || source.key === target.key) return false;
  if (!['before', 'after', 'inside'].includes(placement)) return false;
  const parent = placement === 'inside' ? target.key : target.parent;
  if (placement === 'inside' && target.parent) return false;
  if (parent === source.key) return false;
  if (parent && (source.group || source.childCount > 0)) return false;
  if (parent) {
    const receiver = placement === 'inside' ? target : targetParent;
    if (receiver?.key !== parent || !receiver.scheduled || receiver.parent) return false;
  }
  return true;
}

export function sidebarTreeDropTarget(rows, source, clientX, clientY) {
  if (!rows.length || !source) return null;
  let target = rows.find(row => clientY >= row.top && clientY <= row.bottom);
  let placement;
  if (target) {
    if (target.parent && clientX < target.left) {
      target = rows.find(row => row.key === target.parent) || target;
      placement = 'after';
    } else {
      const ratio = (clientY - target.top) / Math.max(1, target.bottom - target.top);
      placement = ratio < 0.25 ? 'before' : ratio > 0.75 ? 'after' : 'inside';
    }
  } else {
    target = rows.find(row => !row.parent && clientY < row.top);
    placement = target ? 'before' : 'after';
    if (!target) target = [...rows].reverse().find(row => !row.parent);
  }
  const targetParent = target?.parent ? rows.find(row => row.key === target.parent) : null;
  return validSidebarTreeDrop(source, target, placement, targetParent) ? { ...target, placement } : null;
}
