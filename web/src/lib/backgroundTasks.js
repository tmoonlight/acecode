// 后台命令(bash run_in_background、前台超时 / 插话 / 手动转入后台)的前端状态。
//
// 数据来源:GET /api/sessions/:id/background-tasks 快照 + 会话 WS 的
// background_task 事件(开始 / 结束 / 状态变化)。后台子代理仍由 subagentTasks.js
// 管(它们本身就是会话),这里只收 kind=shell 的条目,产出与子代理任务同形的对象
// (status / createdAtMs / updatedAtMs),摘要面板、后台任务面板和活动行可以直接合并。

import { SUBAGENT_TASK_STATUS } from './subagentTasks.js';
import { canonicalTranscriptTool } from './toolPresentation.js';

export const COMMAND_TASK_KIND = 'command';

function finiteMs(value) {
  const number = Number(value);
  return Number.isFinite(number) && number > 0 ? number : 0;
}

export function normalizeCommandTask(raw) {
  if (!raw || typeof raw !== 'object' || raw.kind !== 'shell') return null;
  const id = String(raw.id || '');
  if (!id) return null;
  const state = String(raw.state || 'running');
  const command = String(raw.command || raw.title || '');
  return {
    id,
    kind: COMMAND_TASK_KIND,
    title: command,
    command,
    cwd: String(raw.cwd || ''),
    state,
    status: state === 'running'
      ? SUBAGENT_TASK_STATUS.RUNNING
      : (state === 'stopped' ? SUBAGENT_TASK_STATUS.ABORTED : SUBAGENT_TASK_STATUS.COMPLETED),
    origin: String(raw.origin || ''),
    exitCode: Number.isInteger(raw.exit_code) ? raw.exit_code : null,
    outputFile: String(raw.output_file || ''),
    outputBytes: Math.max(0, Number(raw.output_bytes) || 0),
    monitor: raw.monitor === true,
    stopReason: String(raw.stop_reason || ''),
    error: String(raw.error || ''),
    createdAtMs: finiteMs(raw.started_at_ms),
    updatedAtMs: finiteMs(raw.ended_at_ms) || finiteMs(raw.last_output_at_ms),
  };
}

function newestFirst(a, b) {
  return (b.createdAtMs || 0) - (a.createdAtMs || 0);
}

// REST 快照 → 任务数组(新任务在前)。本地已发出的停止请求保留到服务端确认。
export function mergeCommandTaskList(prevTasks, rawList) {
  const previous = new Map((prevTasks || []).map((task) => [task.id, task]));
  const next = [];
  for (const raw of Array.isArray(rawList) ? rawList : []) {
    const task = normalizeCommandTask(raw);
    if (!task) continue;
    const old = previous.get(task.id);
    next.push(old?.stopPending && task.status === SUBAGENT_TASK_STATUS.RUNNING
      ? { ...task, stopPending: true } : task);
  }
  return next.sort(newestFirst);
}

// background_task 事件 → 插入或替换对应任务;不是命令任务时原样返回。
export function applyBackgroundTaskEvent(tasks, rawTask) {
  const task = normalizeCommandTask(rawTask);
  if (!task) return tasks;
  const list = Array.isArray(tasks) ? tasks : [];
  const index = list.findIndex((item) => item.id === task.id);
  if (index < 0) return [task, ...list].sort(newestFirst);
  const next = list.slice();
  next[index] = list[index].stopPending && task.status === SUBAGENT_TASK_STATUS.RUNNING
    ? { ...task, stopPending: true } : task;
  return next;
}

export function markCommandTaskStopping(tasks, id) {
  const list = Array.isArray(tasks) ? tasks : [];
  const index = list.findIndex((task) => task.id === id);
  if (index < 0 || list[index].status !== SUBAGENT_TASK_STATUS.RUNNING) return list;
  const next = list.slice();
  next[index] = { ...list[index], stopPending: true };
  return next;
}

export function isCommandTask(task) {
  return task?.kind === COMMAND_TASK_KIND;
}

// 能转到后台的前台工具:shell 命令,以及阻塞等待子代理的两个工具。
const BACKGROUNDABLE_TOOLS = new Set(['bash', 'spawn_subagent', 'wait_subagent']);
// 「转到后台」按钮在前台工具跑满这么久后才出现,短命令不打扰。
export const MOVE_TO_BACKGROUND_DELAY_MS = 2000;

// 运行中活动汇总行里,最早开始、还没结束、可以转后台的工具的开始时间(毫秒);没有返回 0。
export function backgroundableToolStartedAt(items) {
  let earliest = 0;
  for (const item of Array.isArray(items) ? items : []) {
    const tool = item?.kind === 'tool' ? item.tool : null;
    if (!tool || tool.isDone) continue;
    if (!BACKGROUNDABLE_TOOLS.has(canonicalTranscriptTool(tool))) continue;
    const started = Number(tool.startedAtMs) || 0;
    if (started > 0 && (earliest === 0 || started < earliest)) earliest = started;
  }
  return earliest;
}

// ── 对话里的后台任务状态 ─────────────────────────────────────────────
//
// 状态挂在发起任务的那条工具行上,折在「已处理」里也一样:任何折叠条(已处理 /
// 工具分组 / 智能体分组)只要包着还没结束的后台任务就带炫光,任务结束一起停。
// 对话末尾不单独画「N 个后台任务正在运行」:回合已经结束,那行转圈让人以为还没跑完。

export function isBackgroundTaskRunning(task) {
  return task?.status === SUBAGENT_TASK_STATUS.RUNNING || task?.busy === true;
}

// 后台命令以失败或被停止收场(子代理任务没有 state,不算)。
export function isBackgroundTaskFailed(task) {
  if (!task || isBackgroundTaskRunning(task)) return false;
  return task.state === 'failed' || task.state === 'stopped';
}

// 这次工具调用启动(或转入后台)的任务 id;不是启动者返回 ''。
// stop_background_task 的结果也带 background_task_id,指的是别的调用启动的任务;
// 只有启动者同时带 background_origin。
export function startedBackgroundTaskId(tool) {
  const metadata = tool?.metadata;
  if (!metadata || typeof metadata !== 'object' || Array.isArray(metadata)) return '';
  if (!metadata.background_origin) return '';
  return String(metadata.background_task_id || '');
}

const NO_TASK_IDS = Object.freeze([]);
const taskIdsByItem = new WeakMap();

// 一个对话项(工具行 / 智能体分组 / 折叠条)包着的后台任务 id。对话项不原地修改,
// 内容变了就是新对象,所以按对象缓存。
export function backgroundTaskIdsOfItem(item) {
  if (!item || typeof item !== 'object') return NO_TASK_IDS;
  const cached = taskIdsByItem.get(item);
  if (cached) return cached;
  let ids = NO_TASK_IDS;
  if (item.kind === 'tool') {
    const id = startedBackgroundTaskId(item.tool);
    if (id) ids = [id];
  } else if (item.kind === 'subagent_group') {
    const found = (Array.isArray(item.agents) ? item.agents : [])
      .map((agent) => String(agent?.sessionId || ''))
      .filter(Boolean);
    if (found.length > 0) ids = found;
  } else if (Array.isArray(item.collapsedItems)) {
    const found = item.collapsedItems.flatMap(backgroundTaskIdsOfItem);
    if (found.length > 0) ids = found;
  }
  taskIdsByItem.set(item, ids);
  return ids;
}

export function itemHasRunningBackgroundTask(item, runningIds) {
  if (!runningIds || runningIds.size === 0) return false;
  return backgroundTaskIdsOfItem(item).some((id) => runningIds.has(id));
}

export const EMPTY_BACKGROUND_TASK_INDEX = Object.freeze({
  byId: new Map(),
  runningIds: new Set(),
});

// 后台任务列表(命令 + 子代理)→ 对话渲染用的索引。
export function indexBackgroundTasks(tasks) {
  const list = Array.isArray(tasks) ? tasks : [];
  if (list.length === 0) return EMPTY_BACKGROUND_TASK_INDEX;
  const byId = new Map();
  const runningIds = new Set();
  for (const task of list) {
    const id = String(task?.id || '');
    if (!id) continue;
    byId.set(id, task);
    if (isBackgroundTaskRunning(task)) runningIds.add(id);
  }
  return { byId, runningIds };
}

// 卡片上的结果标签。
export function commandTaskStatusLabel(task) {
  if (!task) return '';
  if (task.status === SUBAGENT_TASK_STATUS.RUNNING) return '运行中';
  if (task.state === 'stopped') return '已停止';
  if (task.state === 'failed') return task.exitCode == null ? '失败' : `失败 · 退出码 ${task.exitCode}`;
  return '已完成';
}
