import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const srcRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

function source(relativePath) {
  return fs.readFileSync(path.join(srcRoot, relativePath), 'utf8').replace(/\r\n?/g, '\n');
}

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

// 场景:后台任务(后台命令 + 子代理)的列表和子会话内容。
// 期望:不再是对话区旁边可拖宽的独立分栏,而是详情栏里的「后台任务」页签;ChatView 只在
// 该页签激活时渲染 BackgroundTasksView 并交给 PreviewDetailsPanel,正在看的任务记在页签状态里。
// 回归:曾经是 ChatView 里单独一列(App 持久化宽度、ChatView 画拖动分隔条),用户要求并进详情栏。
run('background tasks live in a detail panel tab instead of a split pane', () => {
  const app = source('App.jsx');
  const chat = source('components/ChatView.jsx');
  const details = source('components/PreviewDetailsPanel.jsx');
  const view = source('components/SubagentPanel.jsx');

  assert.doesNotMatch(app, /onSubagentPanelResize|setSubagentPanelWidth|subagentPanelWidth=/);
  assert.doesNotMatch(chat, /data-subagent-splitter|startSubagentPanelResize|subagentPanelOpen|<SubagentPanel/);
  assert.doesNotMatch(chat, /subagentOpen:/);
  assert.match(chat, /activePreview\?\.type === PREVIEW_TAB_TYPES\.BACKGROUND_TASKS \? \(\s*<BackgroundTasksView/);
  assert.match(chat, /backgroundTasksContent=\{backgroundTasksContent\}/);
  assert.match(chat, /openBackgroundTasksTab\(prev, \{/);
  assert.match(chat, /updateBackgroundTasksTab\(prev, \{ sessionId: sid, taskId \}\)/);
  assert.match(details, /if \(active\.type === PREVIEW_TAB_TYPES\.BACKGROUND_TASKS\) \{\s*return backgroundTasksContent;/);
  assert.match(details, /PREVIEW_TAB_TYPES\.BACKGROUND_TASKS\) return '后台任务'/);
  assert.match(view, /export function BackgroundTasksView\(\{ tasks = \[\], taskId = '', onSelectTask,/);
  assert.doesNotMatch(view, /export function SubagentPanel|style=\{\{ width \}\}/);
});

// 场景:用户从三处打开后台任务:会话三点菜单、对话里的智能体分组卡片、摘要面板(运行中任务行 /「N 已完成」)。
// 期望:都走 openBackgroundTasksPreview:三点菜单保留页签里正在看的任务,分组卡片与任务行定位到该任务,
// 「N 已完成」回到任务列表。
run('every background task entry opens the detail panel tab', () => {
  const chat = source('components/ChatView.jsx');
  const summary = source('components/SummaryPanel.jsx');
  assert.match(chat, /id: 'background_tasks', label: '后台任务'[\s\S]*?onSelect: \(\) => openBackgroundTasksPreview\(\)/);
  assert.match(chat, /onOpenSubagent=\{openBackgroundTasksPreview\}/);
  assert.match(chat, /const openSummaryTask = useStableCallback\(\(taskId\) => \{\s*closeSummaryPopup\(\);\s*openBackgroundTasksPreview\(taskId\);/);
  assert.match(chat, /const openSummaryFinishedTasks = useStableCallback\(\(\) => \{\s*closeSummaryPopup\(\);\s*openBackgroundTasksPreview\(''\);/);
  assert.match(chat, /onOpenFinishedTasks=\{openSummaryFinishedTasks\}/);
  // 摘要面板的「后台任务」分组常驻(不随运行中任务清空而隐藏),末尾是「N 已完成」。
  assert.doesNotMatch(summary, /running\.length > 0 && \(/);
  assert.match(summary, /<Row icon="check" onClick=\{onOpenFinishedTasks\}>\s*\{finishedCount\} 已完成/);
});

// 场景:模型派子代理(spawn_subagent / 网状 agent_spawn),或子代理在后台跑着。
// 期望:后台任务面板不自动弹出,只在用户点三点菜单「后台任务」或点智能体分组卡片
// (含摘要面板里的子代理行)时打开;打开面板的调用点只有这两处。
// 回归:曾经父会话一收到 spawn_subagent 的 tool_start 就自动打开面板,模型更常派
// 子代理之后面板反复弹出、挤占对话区,用户要求去掉。
run('spawning a subagent does not open the background task panel by itself', () => {
  const taskHook = source('lib/useSubagentTasks.js');
  const chat = source('components/ChatView.jsx');

  assert.match(taskHook, /export function useSubagentTasks\(parentSessionId\) \{/);
  assert.doesNotMatch(taskHook, /tool_start|onSpawnStart/);
  assert.match(chat, /useSubagentTasks\(sid\);/);
  assert.doesNotMatch(chat, /onSpawnStart|openSubagentPanelForSpawn/);
  // 打开后台任务页签只有一个入口函数,调用点全是用户操作(见上一条测试)。
  const opens = chat.match(/openBackgroundTasksTab\(/g) || [];
  assert.equal(opens.length, 1);
  const callers = chat.match(/openBackgroundTasksPreview\(/g) || [];
  assert.equal(callers.length, 3);
});

// 场景:Desktop 的 daemon 同时服务多个工作区,子会话不在内存里(Desktop 重启后、
// 网状 agent 被换出)时打开后台任务里的子会话。
// 期望:子会话历史请求带上父会话所在工作区,daemon 才能去那个工作区读盘。
// 回归:曾经只带 sessionId,多工作区 daemon 只查自己的 cwd,面板显示
// 「加载失败:HTTP 404 SESSION_NOT_FOUND」,子会话内容看不到。
run('subagent transcript reads the parent workspace so unloaded children still open', () => {
  const panel = source('components/SubagentPanel.jsx');
  const chat = source('components/ChatView.jsx');
  const hook = source('lib/useSubagentTasks.js');

  assert.match(panel, /function SubagentTranscriptView\(\{ task, workspaceHash = '', messageAutoCollapse \}\)/);
  assert.match(panel, /sessionId: task\.id,\s*workspaceHash,/);
  assert.match(panel, /workspaceHash=\{workspaceHash\}/);
  assert.match(chat, /workspaceHash=\{subagentWorkspaceHash\}/);
  assert.match(hook, /useSubagentTasks\(parentSessionId\)/);
});

// 场景:后台任务面板里已结束的子任务(星型后台任务、网状 agent)。
// 期望:面板没有任何单独归档 / 清除 / 删除入口,子任务始终跟随主会话留在面板里,
// 只在主会话归档后被永久删除时由 daemon 一起删除(用户决策:事后分析要完整上下文)。
// 回归:曾经有「清除」(直接永久删除),后来改成「归档」(从面板收起),
// 两种都会让子会话离开主会话,用户要求整个入口去掉。
run('settled subagent tasks stay in the panel with no archive or delete entry', () => {
  const panel = source('components/SubagentPanel.jsx');
  const hook = source('lib/useSubagentTasks.js');
  const chat = source('components/ChatView.jsx');

  assert.doesNotMatch(hook, /clearSettled|archiveSession|archiveWorkspaceSession|purgeSession|purgeTask/);
  // 旧按钮文案是 JSX 字符串 '清除' / '归档'(注释里出现这两个词不算)。
  assert.doesNotMatch(panel, /onClearSettled|clearSettled|['>]归档|['>]清除/);
  assert.doesNotMatch(chat, /onClearSettled|clearSettled/);
});

console.log('subagentPanelSplitArchitecture.test.js: all tests passed');
