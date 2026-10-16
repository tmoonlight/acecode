import { normalizeTokenBudget } from './tokenBudget.js';
import { toolActivityVerb, toolIconName, toolIconSvg } from './toolIcons.js';
import { sessionAvatarIdentity, stableSeed } from './officeCharacters.js';

export const OFFICE_LIMIT = 5;
export const OFFICE_WORKER_SEATS = 7;
const idOf = value => String(value?.id || value?.sessionId || value?.session_id || '');
export const officeSessionRef = value => ({
  sessionId: idOf(value),
  workspaceHash: String(value?.workspaceHash || value?.workspace_hash || ''),
});
export function stableOfficeSeed(value) {
  return stableSeed(value);
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

// Bubble text is measured in half-width units: CJK and full-width glyphs count twice.
const charUnits = ch => (/[ᄀ-ᅟ⺀-꓏가-힣豈-﫿︰-﹏＀-｠￠-￦]/.test(ch)
  || ch.codePointAt(0) > 0xffff ? 2 : 1);

// Keep the end of a line (latest words of a reply, file name of a path).
export function fitTail(text, units = 28) {
  const chars = [...String(text || '').replace(/\s+/g, ' ').trim()];
  let width = 0, start = chars.length;
  while (start > 0 && width + charUnits(chars[start - 1]) <= units) width += charUnits(chars[--start]);
  return start > 0 ? '…' + chars.slice(start).join('') : chars.join('');
}

// Last visible words of streamed Markdown, without the formatting punctuation.
export function speechTail(text, units = 28) {
  const plain = String(text || '')
    .replace(/```[\w-]*/g, ' ')
    .replace(/[`*#]+/g, '')
    .replace(/^\s*>\s?/gm, '')
    .replace(/\[([^\]]*)\]\([^)]*\)/g, '$1');
  return fitTail(plain, units);
}

const GENERIC_LABEL = /^正在(?:等待模型响应|推理|准备工具调用|准备调用\s|调用工具\s)/;
const WAIT_TOOLS = new Set(['wait_subagent', 'agent_wait']);

export function officeActor(session, rootId) {
  const id = idOf(session), root = id === rootId, a = session.activity || {};
  const busy = session.busy === true || session.status === 'running' || a.busy === true;
  const outcome = String(a.outcome || session.last_turn_outcome || '');
  const phase = String(a.phase || ''), tool = String(a.tool || '');
  const raw = String(a.label || '');
  // Concrete progress titles (work mode) are richer than generic engine labels.
  const concrete = raw && !GENERIC_LABEL.test(raw) ? raw.replace(/^正在/, '') : '';
  let state = 'idle', label = '待命', detail = '';
  if (busy) {
    if (phase === 'permission_waiting') {
      state = 'permission'; label = '等待授权'; detail = tool ? toolActivityVerb(tool) : '';
    } else if (phase === 'question_waiting') { state = 'question'; label = '等待回答'; }
    else if (phase === 'compacting' || phase === 'context_repair') { state = 'compact'; label = '整理上下文'; }
    else if (phase === 'model_retry') { state = 'retry'; label = '正在重试'; }
    else if (phase === 'responding') { state = 'type'; label = '撰写回复'; detail = speechTail(a.text); }
    else if (phase === 'tool_planning') {
      state = 'type'; label = tool ? '准备' + toolActivityVerb(tool) : '准备调用工具';
    } else if (tool || phase === 'tool_running') {
      state = WAIT_TOOLS.has(tool.toLowerCase()) ? 'wait' : 'work';
      label = concrete || (tool ? toolActivityVerb(tool) : '调用工具');
      detail = fitTail(a.detail, 30);
    } else if (phase === 'reasoning') { state = 'think'; label = concrete || '思考中'; detail = speechTail(a.text, 24); }
    else { state = 'think'; label = concrete || '思考中'; }
  } else if (outcome === 'completed') { state = root ? 'sleep' : 'done'; label = root ? 'zzz' : '已完成'; }
  else if (outcome === 'error' || outcome === 'errored') { state = 'error'; label = '执行失败'; }
  else if (outcome === 'aborted' || outcome === 'interrupted') { state = 'stopped'; label = '已停止'; }
  const context = normalizeTokenBudget({usage: session.token_usage, contextWindow: session.context_window});
  const shownTool = state === 'work' || state === 'wait' || state === 'permission' || phase === 'tool_planning' ? tool : '';
  const path = String(session.agent_path || (root ? '/root' : ''));
  const avatarIdentity = sessionAvatarIdentity(session);
  return {
    id, root, name: root ? 'Maestro' : String(session.title || session.agent_path?.split('/').pop() || session.summary || 'Agent'),
    path, parentPath: path.includes('/', 1) ? path.slice(0, path.lastIndexOf('/')) : '',
    state, label, detail, tool: shownTool, icon: shownTool ? toolIconName(shownTool) : '',
    iconSvg: shownTool ? toolIconSvg(shownTool) : '', busy,
    avatarIdentity, seed: stableOfficeSeed(avatarIdentity || id), contextKnown: context.known,
    contextRatio: context.usedRatio, contextTokens: context.usedTokens, contextLimit: context.limitTokens,
    compactId: String(a.compact_id || ''), seq: Number(a.seq) || 0,
    transfers: Array.isArray(a.transfers) ? a.transfers : [],
    // Completed workers leave immediately. Failed/stopped workers remain visible
    // for their current parent turn, so they cannot masquerade as success.
    present: root || busy || state === 'error' || state === 'stopped',
    lastActivityAt: String(session.updated_at || session.last_user_message_at || ''),
  };
}

// Streaming tokens arrive over the WebSocket faster than snapshots. The overlay
// is newer than the snapshot only while its event sequence is ahead of it.
export function withLiveActivity(session, entry) {
  const a = session.activity || {};
  const seq = Number(a.seq) || 0;
  if (!entry || !(entry.seq > seq)) return session;
  const continues = a.phase === entry.phase && entry.fromSeq > seq;
  return {...session, busy: true, activity: {...a, busy: true, phase: entry.phase,
    text: (continues ? String(a.text || '') : '') + entry.text, tool: '', detail: '', seq: entry.seq}};
}

// Keep the start of a line (a finished reply reads best from its first words).
export function fitHead(text, units = 60) {
  const chars = [...String(text || '').replace(/\s+/g, ' ').trim()];
  let width = 0, end = 0;
  while (end < chars.length && width + charUnits(chars[end]) <= units) width += charUnits(chars[end++]);
  return end < chars.length ? chars.slice(0, end).join('') + '…' : chars.join('');
}
const plainMarkdown = text => String(text || '')
  .replace(/```[\w-]*/g, ' ').replace(/[`*#]+/g, '').replace(/^\s*>\s?/gm, '')
  .replace(/\[([^\]]*)\]\([^)]*\)/g, '$1');

export const ACTIVITY_LIMIT = 12;
// Codex Mini session cards: a main session appears while it runs or waits for
// the user, and after it finishes until its output is read (opening the session
// marks it read). status: running | waiting | failed | stopped | review.
export function officeActivity(session) {
  const id = idOf(session);
  if (!id || session.parent_session_id || session.archived) return null;
  const actor = officeActor(session, id);
  const a = session.activity || {};
  let status = '', body = '';
  if (actor.busy) {
    status = actor.state === 'permission' || actor.state === 'question' ? 'waiting' : 'running';
    if (actor.state === 'permission') body = actor.detail ? `需要你确认：${actor.detail}` : '需要你确认';
    else if (actor.state === 'question') body = '有问题等你回答';
    else if (a.phase === 'responding' && a.text) body = speechTail(a.text, 64);
    else body = [actor.label, actor.detail].filter(Boolean).join(' · ');
  } else if (session.attention_state === 'unread') {
    status = actor.state === 'error' ? 'failed' : actor.state === 'stopped' ? 'stopped' : 'review';
    const reply = fitHead(plainMarkdown(a.reply), 64);
    body = status === 'failed' ? '执行失败' : status === 'stopped' ? '已停止' : reply || '已完成';
  }
  if (!status) return null;
  const sortAt = String(session.last_user_message_at || session.updated_at || '');
  return { ...officeSessionRef(session), title: String(session.title || session.summary || '未命名会话'),
    status, state: actor.state, body, sortAt,
    // Dismissing hides one card until the session moves on to another state or turn.
    key: [id, status, sortAt, String(a.turn_id || '')].join('|') };
}
export function projectActivities(sessions = [], live = null) {
  const cards = new Map();
  for (const session of sessions || []) {
    const card = officeActivity(withLiveActivity(session, live?.get(idOf(session))));
    if (card && !cards.has(card.sessionId)) cards.set(card.sessionId, card);
  }
  return [...cards.values()]
    .sort((a, b) => b.sortAt.localeCompare(a.sortAt) || a.sessionId.localeCompare(b.sessionId))
    .slice(0, ACTIVITY_LIMIT);
}

export function projectDesktopOffice(snapshot = {}, { follow = true, connected = true, live = null } = {}) {
  const selected = snapshot.selected || null;
  const rootId = idOf(selected);
  const rootTime = String(selected?.last_user_message_at || '');
  const sessions = (snapshot.agents || []).map(session => withLiveActivity(session, live?.get(idOf(session))));
  const actors = sessions.map(session => officeActor(session, rootId));
  const agents = actors.filter(actor => actor.present && (actor.root || actor.busy || !rootTime || actor.lastActivityAt >= rootTime));
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
    activities: projectActivities(snapshot.activities, live),
    completed: actors.filter(actor => !actor.root && actor.state === 'done').map(actor => actor.id),
    overflow: Math.max(0, agents.length - OFFICE_WORKER_SEATS - 1),
  };
}
