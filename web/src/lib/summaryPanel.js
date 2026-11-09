// 摘要面板(对话区与右侧详情之间)的纯数据整形:工作区行、变更合计、
// 后台任务行。组件 SummaryPanel.jsx 只渲染这里的结果。
import {
  SUBAGENT_TASK_STATUS,
  formatElapsed,
  taskDisplayTitle,
  taskElapsedSeconds,
} from './subagentTasks.js';
import { isDefaultWorkspaceIcon, resolveWorkspaceIcon } from './workspaceIcons.js';
import { workspaceFolderKey } from './workspaceProfile.js';

const NUMBER_FORMAT = new Intl.NumberFormat('en-US');

function finiteCount(value) {
  const number = Number(value);
  return Number.isFinite(number) && number > 0 ? Math.round(number) : 0;
}

// 侧栏里能右键「编辑」的工作区才有齿轮,判定与 Sidebar 的 onEdit 一致:
// 有 hash 且不是本机兜底项(__local__)。
export function isEditableWorkspaceHash(hash) {
  const value = String(hash || '');
  return !!value && value !== '__local__';
}

// 目录行:会话实际工作的目录在前(worktree 会话就是 worktree 路径),再接工作区
// 「编辑项目」里加的附加文件夹;同一路径(Windows 不分大小写、忽略结尾斜杠)只列一次。
function summaryFolders(cwd, extraFolders) {
  const folders = [];
  const seen = new Set();
  for (const folder of [cwd, ...(Array.isArray(extraFolders) ? extraFolders : [])]) {
    const path = typeof folder === 'string' ? folder.trim() : '';
    const key = path && workspaceFolderKey(path);
    if (!key || seen.has(key)) continue;
    seen.add(key);
    folders.push(path);
  }
  return folders;
}

// directory:侧栏已加载的工作区条目(workspaceDirectory),可能为空(列表还没到 /
// 无工作区会话)。名称优先用它(侧栏改名后会话引用里的名字可能是旧的)。
export function summaryWorkspaceModel({
  name = '',
  cwd = '',
  hash = '',
  noWorkspace = false,
  gitInfo = null,
  directory = null,
} = {}) {
  const branch = gitInfo?.is_repo ? String(gitInfo.branch || '').trim() : '';
  const entry = noWorkspace ? null : directory;
  const icon = entry && !isDefaultWorkspaceIcon(entry.icon) ? resolveWorkspaceIcon(entry.icon) : null;
  return {
    name: noWorkspace ? '' : String(entry?.name || name || '').trim(),
    folders: summaryFolders(cwd, entry?.extra_folders),
    icon,
    editable: !noWorkspace && isEditableWorkspaceHash(hash),
    hash: String(hash || ''),
    branch,
  };
}

// 目录行显示成「目录名 · 上级路径」(与 Claude 桌面版一致),完整路径放悬停提示。
// 正反斜杠都认,结尾的分隔符忽略;盘符根(C:/)与没有分隔符的路径整体当名字。
export function folderLabel(path) {
  const raw = String(path || '').trim();
  let end = raw.length;
  while (end > 1 && (raw[end - 1] === '/' || raw[end - 1] === String.fromCharCode(92))) end -= 1;
  const trimmed = raw.slice(0, end);
  const cut = Math.max(trimmed.lastIndexOf('/'), trimmed.lastIndexOf(String.fromCharCode(92)));
  if (cut < 0 || /^[A-Za-z]:$/.test(trimmed)) return { name: raw, parent: '' };
  const name = trimmed.slice(cut + 1);
  let parent = trimmed.slice(0, cut);
  // C:/x 的上级是盘符根,保留分隔符写成 C:/ 而不是光秃秃的 C:。
  if (/^[A-Za-z]:$/.test(parent)) parent = trimmed.slice(0, cut + 1);
  return name ? { name, parent } : { name: raw, parent: '' };
}

// git 仓库取工作树相对基线的合计(与右侧「变更」页同一份数据);非仓库退到
// 本会话自己的改动汇总。没有任何改动时返回 null,整段不显示。
export function summaryChangeTotals({ isRepo = false, gitList = null, sessionSummary = null } = {}) {
  if (isRepo) {
    if (!gitList) return null;
    const additions = finiteCount(gitList.total_additions);
    const deletions = finiteCount(gitList.total_deletions);
    const files = finiteCount(gitList.total_count ?? gitList.files?.length);
    if (files === 0 && additions === 0 && deletions === 0) return null;
    return { additions, deletions };
  }
  if (!sessionSummary?.hasChanges) return null;
  return {
    additions: finiteCount(sessionSummary.totalAdditions),
    deletions: finiteCount(sessionSummary.totalDeletions),
  };
}

export function formatChangeCount(value) {
  return NUMBER_FORMAT.format(finiteCount(value));
}

// 只列运行中的任务,先开始的在上面;已结束的去后台任务面板里看。
export function summaryRunningTasks(tasks, nowMs = Date.now()) {
  return (Array.isArray(tasks) ? tasks : [])
    .filter((task) => task?.status === SUBAGENT_TASK_STATUS.RUNNING)
    .slice()
    .sort((a, b) => (a.createdAtMs || 0) - (b.createdAtMs || 0))
    .map((task) => ({
      id: task.id,
      kind: task.kind === 'command' ? 'command' : 'agent',
      title: task.kind === 'command' ? (task.command || task.title || task.id) : taskDisplayTitle(task),
      elapsed: formatElapsed(taskElapsedSeconds(task, nowMs)),
      stopping: task.abortPending === true || task.stopPending === true,
    }));
}
