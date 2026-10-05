import { normalizeTokenBudget } from './tokenBudget.js';
import { toolIconName, toolIconSvg } from './toolIcons.js';

export const OFFICE_LIMIT = 5;
export const OFFICE_WORKER_SEATS = 7;
const idOf = value => String(value?.id || value?.sessionId || value?.session_id || '');
export const officeSessionRef = value => ({
  sessionId: idOf(value),
  workspaceHash: String(value?.workspaceHash || value?.workspace_hash || ''),
});
export function stableOfficeSeed(value) {
  let hash = 2166136261;
  for (const c of String(value)) hash = Math.imul(hash ^ c.codePointAt(0), 16777619);
  return hash >>> 0;
}
export function recentOffices(sessions = []) {
  const unique = new Map();
  for (const session of sessions) {
    if (!idOf(session) || session.archived || session.parent_session_id || !session.last_user_message_at) continue;
    unique.set(idOf(session), session);
  }
  return [...unique.values()].sort((a,b) =>
    String(b.last_user_message_at).localeCompare(String(a.last_user_message_at)) || idOf(a).localeCompare(idOf(b)))
    .slice(0, OFFICE_LIMIT);
}

export function officeActor(session, rootId) {
  const id = idOf(session), root = id === rootId, a = session.activity || {};
  const busy = session.busy === true || session.status === 'running' || a.busy === true;
  const outcome = String(a.outcome || session.last_turn_outcome || '');
  const phase = String(a.phase || ''), tool = String(a.tool || '');
  let state = 'idle', label = '待命';
  if (busy) {
    if (phase === 'permission_waiting') { state = 'permission'; label = '等待授权'; }
    else if (phase === 'question_waiting') { state = 'question'; label = '等待回答'; }
    else if (phase === 'compacting' || phase === 'context_repair') { state = 'compact'; label = '整理上下文'; }
    else if (phase === 'model_retry') { state = 'retry'; label = '正在重试'; }
    else if (tool || phase === 'tool_running') { state = 'work'; label = tool || '工作中'; }
    else { state = 'think'; label = String(a.label || '思考中'); }
  } else if (outcome === 'completed') { state = root ? 'sleep' : 'done'; label = root ? 'zzz' : '已完成'; }
  else if (outcome === 'error' || outcome === 'errored') { state = 'error'; label = '执行失败'; }
  else if (outcome === 'aborted' || outcome === 'interrupted') { state = 'stopped'; label = '已停止'; }
  const context = normalizeTokenBudget({usage: session.token_usage, contextWindow: session.context_window});
  const icon = tool ? toolIconName(tool) : '';
  return {
    id, root, name: root ? 'Maestro' : String(session.title || session.agent_path?.split('/').pop() || 'Agent'),
    path: String(session.agent_path || (root ? '/root' : '')),
    state, label, tool, icon, iconSvg: tool ? toolIconSvg(tool) : '', busy,
    seed: stableOfficeSeed(id), contextKnown: context.known,
    contextRatio: context.usedRatio, contextTokens: context.usedTokens, contextLimit: context.limitTokens,
    compactId: String(a.compact_id || ''), seq: Number(a.seq) || 0,
    transfers: Array.isArray(a.transfers) ? a.transfers : [],
    // Completed workers leave immediately. Failed/stopped workers remain visible
    // for their current parent turn, so they cannot masquerade as success.
    present: root || busy || state === 'error' || state === 'stopped',
    lastActivityAt: String(session.updated_at || session.last_user_message_at || ''),
  };
}

export function projectDesktopOffice(snapshot = {}, { follow = true, connected = true } = {}) {
  const selected = snapshot.selected || null;
  const rootId = idOf(selected);
  const rootTime = String(selected?.last_user_message_at || '');
  const agents = (snapshot.agents || []).map(session => officeActor(session, rootId))
    .filter(actor => actor.present && (actor.root || actor.busy || !rootTime || actor.lastActivityAt >= rootTime));
  agents.sort((a,b) => Number(b.root) - Number(a.root) || a.id.localeCompare(b.id));
  return {
    version: 1, follow, connected, complete: snapshot.complete !== false,
    offices: recentOffices(snapshot.offices).map(session => ({
      ...officeSessionRef(session), title: String(session.title || session.summary || '未命名会话'),
      busy: session.busy === true || session.status === 'running',
      lastUserMessageAt: session.last_user_message_at,
      workspaceName: String(session.workspace_name || ''),
    })),
    selected: selected ? { ...officeSessionRef(selected), title: String(selected.title || selected.summary || '未命名会话') } : null,
    seed: stableOfficeSeed(rootId || 'empty-office'),
    agents,
    completed: (snapshot.agents || []).filter(session => session.id !== rootId && officeActor(session, rootId).state === 'done').map(session => session.id),
    overflow: Math.max(0, agents.length - OFFICE_WORKER_SEATS - 1),
  };
}
