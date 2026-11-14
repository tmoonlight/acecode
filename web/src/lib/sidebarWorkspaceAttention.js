import { normalizePinnedIds } from './pinnedSessions.js';
import { sessionAttentionState } from './sessionStatus.js';

// 工作区行「新建任务」位上的汇总状态(进行中 / 未读)。
//
// 侧栏只给可见的工作区加载会话行,折叠的工作区冷启动时一行都没有;只凭会话行
// 汇总,重启后折叠工作区里的未读就丢了(展开过一次之后会话行留在内存里才又出现)。
// daemon 订阅工作区状态时推的 session_status_snapshot 覆盖该工作区全部未归档主任务,
// 这里只记「哪些会话属于哪个工作区」,状态本身仍以 statusBySession 为准。
//
// 成员表:Map<workspace_hash, Set<session_id>>。内容没变时返回同一个 Map,
// 调用方可以直接跳过 setState。

const STATUS_MESSAGE_TYPES = new Set([
  'session_status',
  'mark_session_read_ack',
  'mark_session_unread_ack',
]);

function sameMembers(a, b) {
  if (a.size !== b.size) return false;
  for (const id of a) if (!b.has(id)) return false;
  return true;
}

function isChildStatus(message, payload) {
  return !!(payload.parent_session_id || message.parent_session_id);
}

export function applyWorkspaceStatusMembership(membership, message = {}) {
  const current = membership instanceof Map ? membership : new Map();
  const payload = message.payload || {};

  if (message.type === 'session_status_snapshot') {
    const hash = String(payload.workspace_hash || message.workspace_hash || '');
    if (!hash) return current;
    const ids = new Set();
    for (const item of Array.isArray(payload.sessions) ? payload.sessions : []) {
      const id = item?.session_id || item?.id || '';
      if (id && !item.parent_session_id) ids.add(id);
    }
    const previous = current.get(hash);
    if (previous && sameMembers(previous, ids)) return current;
    return new Map(current).set(hash, ids);
  }

  if (!STATUS_MESSAGE_TYPES.has(message.type)) return current;
  const id = payload.session_id || message.session_id || '';
  if (!id) return current;

  if (isChildStatus(message, payload)) {
    // 子代理会话不是侧栏里的任务,即便和父会话同属一个工作区也不计入。
    return removeWorkspaceStatusMembers(current, [id]);
  }

  // 快照之后新建的任务只会以单条状态出现;只往已经收到过快照的工作区里补,
  // 没快照的工作区等快照到了一并算。
  const hash = String(payload.workspace_hash || message.workspace_hash || '');
  const members = hash ? current.get(hash) : null;
  if (!members || members.has(id)) return current;
  return new Map(current).set(hash, new Set(members).add(id));
}

export function removeWorkspaceStatusMembers(membership, ids = []) {
  const current = membership instanceof Map ? membership : new Map();
  const removed = new Set(Array.from(ids || []).filter(Boolean));
  if (removed.size === 0) return current;
  let next = current;
  for (const [hash, members] of current) {
    if (!Array.from(removed).some((id) => members.has(id))) continue;
    if (next === current) next = new Map(current);
    const kept = new Set(members);
    for (const id of removed) kept.delete(id);
    next.set(hash, kept);
  }
  return next;
}

// rows:该工作区已加载、已合并状态的会话行(不含置顶);memberIds:快照成员;
// pinnedIds:该工作区的置顶会话 —— 它们显示在「置顶」分组里,不算进工作区。
export function workspaceAttentionSummary({
  rows = [],
  memberIds = null,
  statusMap = null,
  pinnedIds = [],
} = {}) {
  let inProgress = false;
  let unread = false;
  const note = (state) => {
    if (state === 'in_progress') inProgress = true;
    else if (state === 'unread') unread = true;
  };
  const seen = new Set();
  for (const row of Array.isArray(rows) ? rows : []) {
    if (row?.id) seen.add(row.id);
    note(sessionAttentionState(row || {}));
  }
  if (memberIds && statusMap) {
    const pinned = new Set(normalizePinnedIds(pinnedIds || []));
    for (const id of memberIds) {
      if (seen.has(id) || pinned.has(id)) continue;
      const status = statusMap.get(id);
      if (status) note(sessionAttentionState(status));
    }
  }
  return { inProgress, unread };
}

// 进行中只在工作区折叠时显示(展开后每行自己有转圈),且优先于未读绿点。
export function workspaceAttentionIndicator({ inProgress = false, unread = false } = {}, { expanded = false } = {}) {
  if (!expanded && inProgress) return 'in_progress';
  return unread ? 'unread' : '';
}
