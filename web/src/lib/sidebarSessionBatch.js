// 侧栏多选批量操作(POST /api/session-batch)的请求组装与结果解析。纯函数,
// Sidebar 只负责把结果写回各自的状态。曾经批量操作是对每个会话逐个调用单项接口,
// 选 20 个会话置顶就是 40 个请求;现在一次请求,服务端合并落盘。
import { NO_WORKSPACE_PIN_SCOPE, normalizePinnedIds, normalizePinnedOrderItems, sessionPinScope } from './pinnedSessions.js';

export const SESSION_BATCH_ACTIONS = Object.freeze(['pin', 'unpin', 'archive', 'unarchive', 'read', 'unread']);

function sessionIdOf(session = {}) {
  return String(session?.id || session?.session_id || session?.sessionId || '');
}

function isNoWorkspace(session = {}) {
  return !!(session?.no_workspace || session?.noWorkspace);
}

// entries 来自 useSidebarSessionSelection().entries():[{ key, pinned, session }],
// 顺序即侧栏可见顺序。置顶时服务端按这个顺序插到最前,第一个排在最上面。
export function sessionBatchItems(action, entries = [], { cursorFor } = {}) {
  return entries.map(({ session }) => {
    const noWorkspace = isNoWorkspace(session);
    const item = {
      session_id: sessionIdOf(session),
      workspace_hash: noWorkspace ? '' : String(session?.workspace_hash || session?.workspaceHash || ''),
      no_workspace: noWorkspace,
    };
    if (action === 'read' && typeof cursorFor === 'function') {
      const cursor = Number(cursorFor(session));
      if (Number.isFinite(cursor) && cursor > 0) item.cursor = Math.floor(cursor);
    }
    return item;
  }).filter(item => item.session_id);
}

function resultKey(result = {}) {
  const scope = result.no_workspace ? NO_WORKSPACE_PIN_SCOPE : String(result.workspace_hash || '');
  return `${scope}\u0000${String(result.session_id || '')}`;
}

function entryResultKey(entry = {}) {
  const session = entry.session || {};
  const scope = isNoWorkspace(session)
    ? NO_WORKSPACE_PIN_SCOPE
    : String(session.workspace_hash || session.workspaceHash || '');
  return `${scope}\u0000${sessionIdOf(session)}`;
}

// 把响应按 entry 对齐。没有出现在 results 里的 entry 一律算失败,
// 这样服务端漏回某项时界面不会误以为成功。
export function sessionBatchOutcome(response = {}, entries = []) {
  const byKey = new Map();
  for (const result of Array.isArray(response?.results) ? response.results : []) {
    byKey.set(resultKey(result), result);
  }
  const succeeded = [];
  const failed = [];
  for (const entry of entries) {
    const result = byKey.get(entryResultKey(entry));
    if (result?.ok) succeeded.push({ entry, result });
    else failed.push({ entry, error: String(result?.error || '') });
  }
  const pinnedScopes = new Map();
  for (const scope of Array.isArray(response?.pinned_scopes) ? response.pinned_scopes : []) {
    const name = String(scope?.pin_scope || '');
    if (name) pinnedScopes.set(name, normalizePinnedIds(scope?.session_ids || []));
  }
  const pinnedOrder = Array.isArray(response?.pinned_order?.items)
    ? normalizePinnedOrderItems(response.pinned_order.items)
    : null;
  return {
    succeeded,
    failed,
    failedKeys: failed.map(({ entry }) => entry.key).filter(Boolean),
    pinnedScopes,
    pinnedOrder,
    statuses: succeeded.map(({ result }) => result.status).filter(status => status && typeof status === 'object'),
  };
}

// 涉及到的置顶作用域(工作区 hash 或 "__no_workspace__"),用于批量后刷新列表。
export function sessionBatchScopes(entries = []) {
  return Array.from(new Set(entries.map(({ session }) => sessionPinScope(session)).filter(Boolean)));
}
