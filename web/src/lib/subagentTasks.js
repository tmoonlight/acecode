// 后台任务(spawn_subagent 子会话)面板的纯状态逻辑。
//
// 数据来源分两层:
//   1. REST GET /api/sessions?parent=<id> 的会话快照(标题/tokens/时间戳)
//      → normalizeSubagentTask 归一化成任务对象。
//   2. 子会话的实时 WS 事件(busy_changed / usage / session_updated)→
//      applySubagentSessionEvent 增量更新任务行(运行状态、tokens、标题)。
//      工具事件不改任务:列表不显示工具统计,不为它重画整个对话。
//
// DOM 端(SubagentPanel.jsx)只消费这里产出的结构;所有状态转移在此测试。

import { relativeTime } from './format.js';

export const SUBAGENT_TASK_STATUS = Object.freeze({
  RUNNING: 'running',
  COMPLETED: 'completed',
  ABORTED: 'aborted',
});

function readTokens(session) {
  const usage = session?.session_token_usage || session?.sessionTokenUsage || null;
  const total = Number(usage?.total_tokens ?? usage?.totalTokens);
  return Number.isFinite(total) && total > 0 ? total : 0;
}

function parseIsoMs(value) {
  if (!value) return 0;
  const ms = Date.parse(String(value));
  return Number.isFinite(ms) ? ms : 0;
}

// REST 会话快照 → 任务对象。busy(active 会话实时)或 status==='running'
// 视为运行中;其余是已结束(区分不了"完成/中止",统一 completed,中止由
// 前端 abort 操作时本地标记)。
export function normalizeSubagentTask(session) {
  if (!session || typeof session !== 'object') return null;
  const id = String(session.id || session.session_id || '');
  if (!id) return null;
  const busy = session.busy === true || session.status === 'running';
  return {
    id,
    parentId: String(session.parent_session_id || ''),
    title: String(session.title || '').trim(),
    summary: String(session.summary || '').trim(),
    status: busy ? SUBAGENT_TASK_STATUS.RUNNING : SUBAGENT_TASK_STATUS.COMPLETED,
    createdAtMs: parseIsoMs(session.created_at),
    updatedAtMs: parseIsoMs(session.updated_at),
    tokens: readTokens(session),
    turnCount: Math.max(0, Number(session.turn_count) || 0),
    // 蜂群模式（网状）的子 agent 都扁平挂在根会话下,真实层级看路径(/root/a/b)。
    agentPath: String(session.agent_path || ''),
    model: String(session.model || ''),
    expert: session.expert || null,
  };
}

// REST 列表 → 任务数组(按创建时间倒序,新任务在前)。保留旧任务的本地 aborted 标记,
// 避免 refetch 把它洗回 completed。
// 子会话刚建好、还没写元数据时列表里 created_at 是空的(wait=false 派出的子代理
// 往往就在这一刻被列出,之后不再刷新):运行中的任务先用第一次看到它的时间计时,
// 拿到真实 created_at 后再换成它,耗时不会停在 00s。
export function mergeSubagentTaskList(prevTasks, sessions, nowMs = Date.now()) {
  const prevById = new Map((prevTasks || []).map((t) => [t.id, t]));
  const next = [];
  for (const session of Array.isArray(sessions) ? sessions : []) {
    const task = normalizeSubagentTask(session);
    if (!task) continue;
    const prev = prevById.get(task.id);
    if (!task.createdAtMs) {
      task.createdAtMs = prev?.createdAtMs
        || (task.status === SUBAGENT_TASK_STATUS.RUNNING ? nowMs : 0);
    }
    if (prev) {
      task.abortPending = !!prev.abortPending && task.status === SUBAGENT_TASK_STATUS.RUNNING;
      if (prev.abortPending && task.status !== SUBAGENT_TASK_STATUS.RUNNING) {
        task.status = SUBAGENT_TASK_STATUS.ABORTED;
      }
      if (prev.status === SUBAGENT_TASK_STATUS.ABORTED &&
          task.status !== SUBAGENT_TASK_STATUS.RUNNING) {
        task.status = SUBAGENT_TASK_STATUS.ABORTED;
      }
    }
    next.push(task);
  }
  next.sort((a, b) => (b.createdAtMs - a.createdAtMs) || (a.id < b.id ? 1 : -1));
  return next;
}

// Parent subscriptions receive additive child session_status events. Only an
// explicit parent match may trigger discovery: treating every unknown busy
// workspace session as a child causes unrelated conversations to refetch and
// can attach permission/question attention to the wrong parent.
export function shouldRefreshSubagentTasksFromStatus(
  parentSessionId,
  knownTaskIds,
  msg,
) {
  if (!parentSessionId || msg?.type !== 'session_status') return false;
  const payload = msg?.payload || {};
  const sessionId = String(payload.session_id || msg?.session_id || '').trim();
  const statusParentId = String(
    payload.parent_session_id || msg?.parent_session_id || '',
  ).trim();
  if (!sessionId || statusParentId !== parentSessionId) return false;
  return !(knownTaskIds instanceof Set && knownTaskIds.has(sessionId));
}

// 子会话自己的 WS 事件 → 任务增量。返回新数组;无关事件返回原引用
// (调用方可用引用相等跳过 setState)。
export function applySubagentSessionEvent(tasks, msg) {
  const sid = msg?.session_id || msg?.payload?.session_id || '';
  if (!sid) return tasks;
  const index = (tasks || []).findIndex((t) => t.id === sid);
  if (index < 0) return tasks;
  const task = tasks[index];
  const type = msg?.type || '';
  const p = msg?.payload || {};
  let patch = null;

  if (type === 'busy_changed' || type === 'session_status' || type === 'done') {
    const busy = p.busy === true;
    if (busy && task.status !== SUBAGENT_TASK_STATUS.RUNNING) {
      patch = { status: SUBAGENT_TASK_STATUS.RUNNING };
    } else if (!busy && task.status === SUBAGENT_TASK_STATUS.RUNNING) {
      patch = {
        status: p.outcome === 'aborted' || (task.abortPending && !p.outcome)
          ? SUBAGENT_TASK_STATUS.ABORTED : SUBAGENT_TASK_STATUS.COMPLETED,
        abortPending: false, updatedAtMs: Date.now(),
      };
    }
  } else if (type === 'usage') {
    const total = Number(p.total_tokens ?? p.totalTokens);
    if (Number.isFinite(total) && total > task.tokens) {
      patch = { tokens: total };
    }
  } else if (type === 'session_updated') {
    const title = String(p.title || '').trim();
    if (title && title !== task.title) patch = { title };
  }

  if (!patch) return tasks;
  const next = tasks.slice();
  next[index] = { ...task, ...patch };
  return next;
}

// Keep the subscription and running group until authoritative completion.
// This legacy export name denotes the user's request, not a completed stop.
export function markSubagentTaskAborted(tasks, id) {
  const index = (tasks || []).findIndex((t) => t.id === id);
  if (index < 0) return tasks;
  const next = tasks.slice();
  if (next[index].status !== SUBAGENT_TASK_STATUS.RUNNING) return tasks;
  next[index] = { ...next[index], abortPending: true };
  return next;
}

export function removeSubagentTask(tasks, id) {
  const next = (tasks || []).filter((t) => t.id !== id);
  return next.length === (tasks || []).length ? tasks : next;
}

export function subagentTaskGroups(tasks) {
  const running = [];
  const settled = [];
  for (const task of tasks || []) {
    (task.status === SUBAGENT_TASK_STATUS.RUNNING ? running : settled).push(task);
  }
  return { running, settled };
}

export function runningSubagentCount(tasks) {
  return (tasks || []).reduce(
    (n, t) => n + (t.status === SUBAGENT_TASK_STATUS.RUNNING ? 1 : 0), 0);
}

// '34.0k' 风格的 token 数(图中卡片样式);< 1000 原样。
export function formatTaskTokens(total) {
  const n = Math.max(0, Number(total) || 0);
  if (n < 1000) return String(n);
  const k = n / 1000;
  return `${k >= 100 ? Math.round(k) : k.toFixed(1)}k`;
}

// 卡片耗时:运行中 = now - createdAt;已结束 = updatedAt - createdAt。
export function taskElapsedSeconds(task, nowMs = Date.now()) {
  if (!task?.createdAtMs) return 0;
  const end = task.status === SUBAGENT_TASK_STATUS.RUNNING
    ? nowMs
    : (task.updatedAtMs || nowMs);
  return Math.max(0, Math.round((end - task.createdAtMs) / 1000));
}

export function formatElapsed(seconds) {
  const s = Math.max(0, Math.round(Number(seconds) || 0));
  if (s < 60) return `${String(s).padStart(2, '0')}s`;
  const m = Math.floor(s / 60);
  if (m < 60) return `${m}m${String(s % 60).padStart(2, '0')}s`;
  const h = Math.floor(m / 60);
  return `${h}h${String(m % 60).padStart(2, '0')}m`;
}

// 列表行标题后面的 token 数:'34.0k tokens';还没有用量时不显示。
export function taskTokensLabel(task) {
  return task?.tokens > 0 ? `${formatTaskTokens(task.tokens)} tokens` : '';
}

// 列表行靠右的时间:运行中是已经跑了多久,结束后是多久之前结束的。
export function taskTimeLabel(task, nowMs = Date.now()) {
  if (task?.status === SUBAGENT_TASK_STATUS.RUNNING) {
    return formatElapsed(taskElapsedSeconds(task, nowMs));
  }
  return relativeTime(task?.updatedAtMs || task?.createdAtMs, undefined, nowMs);
}

// 运行中的任务多出来的那一行状态;已结束的任务没有这一行。
export function taskRunningStatusLabel(task) {
  if (task?.status !== SUBAGENT_TASK_STATUS.RUNNING) return '';
  return task.abortPending ? '正在停止…' : '运行中';
}

// 一行投影结果里包着的子代理,按出现顺序:子代理分组就是它自己的那几个;工具汇总行
// (「调用了 3 个智能体」「已运行 1 条命令，调用了 2 个智能体」)取它折起来的各项里的。
// 不含子代理时返回同一个空数组。投影出来的行对象不会原地改,按对象缓存。
const NO_AGENTS = Object.freeze([]);
const agentsByItem = new WeakMap();
export function subagentAgentsOfItem(item) {
  if (!item || typeof item !== 'object') return NO_AGENTS;
  const cached = agentsByItem.get(item);
  if (cached) return cached;
  let agents = NO_AGENTS;
  if (item.kind === 'subagent_group') {
    if (Array.isArray(item.agents) && item.agents.length > 0) agents = item.agents;
  } else if (Array.isArray(item.collapsedItems)) {
    const found = item.collapsedItems.flatMap(subagentAgentsOfItem);
    if (found.length > 0) agents = found;
  }
  agentsByItem.set(item, agents);
  return agents;
}

// 头像右上角的状态图标画哪一种(图标本身见 avatarStatus.js):运行中(正在停止也算)、
// 已完成、已中止。没查到任务(旧会话、子会话已删)时不画,不靠工具项的 done 标记猜。
export function subagentAvatarStatus(task) {
  if (task?.status === SUBAGENT_TASK_STATUS.RUNNING) return 'running';
  if (task?.status === SUBAGENT_TASK_STATUS.ABORTED) return 'stopped';
  if (task?.status === SUBAGENT_TASK_STATUS.COMPLETED) return 'done';
  return '';
}

// 对话里「调用了 N 个智能体」一行前面的头像:拿到子会话 id 的子代理每个一张,最多 limit 张
// (个数已经写在文案里,不再补「+N」)。
export const SUBAGENT_GROUP_AVATAR_LIMIT = 6;
export function subagentGroupAvatars(agents, tasksById, limit = SUBAGENT_GROUP_AVATAR_LIMIT) {
  const avatars = [];
  for (const agent of Array.isArray(agents) ? agents : []) {
    const sessionId = String(agent?.sessionId || '');
    if (!sessionId) continue;
    if (avatars.length >= limit) break;
    const task = tasksById?.get?.(sessionId) || null;
    avatars.push({ sessionId, expert: task?.expert || null, status: subagentAvatarStatus(task) });
  }
  return avatars;
}

// 卡片标题:auto-title 优先,退到首条 user 输入摘要,再退会话 id。
// auto-title 生成失败会留下 "[Error] ..." 形式的占位(后端
// is_generated_error_title 语义)——对用户无意义,直接退到摘要。
export function taskDisplayTitle(task) {
  const title = String(task?.title || '');
  if (title && !title.startsWith('[Error]')) return title;
  return task?.summary || task?.id || '';
}
