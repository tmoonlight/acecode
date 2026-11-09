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

// 卡片上的结果标签。
export function commandTaskStatusLabel(task) {
  if (!task) return '';
  if (task.status === SUBAGENT_TASK_STATUS.RUNNING) return '运行中';
  if (task.state === 'stopped') return '已停止';
  if (task.state === 'failed') return task.exitCode == null ? '失败' : `失败 · 退出码 ${task.exitCode}`;
  return '已完成';
}
