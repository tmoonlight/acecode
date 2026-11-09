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

run('App owns and persists the preferred subagent panel width', () => {
  const app = source('App.jsx');
  assert.match(app, /normalizeSubagentPanelWidth,/);
  assert.match(app, /const setSubagentPanelWidth = useCallback/);
  assert.match(app, /subagentPanel === prev\.subagentPanel \? prev : \{ \.\.\.prev, subagentPanel \}/);
  assert.match(app, /subagentPanelWidth=\{singleLayout\.subagentPanel \?\? DEFAULT_SINGLE_LAYOUT\.subagentPanel\}/);
  assert.match(app, /onSubagentPanelResize=\{setSubagentPanelWidth\}/);
});

run('ChatView renders an accessible draggable transcript splitter', () => {
  const chat = source('components/ChatView.jsx');
  assert.match(chat, /subagentPanelOpen && \(\s*<div\s+role="separator"/);
  assert.match(chat, /data-subagent-splitter="true"/);
  assert.match(chat, /aria-orientation="vertical"/);
  assert.match(chat, /aria-valuemin=\{subagentPanelRange\.min\}/);
  assert.match(chat, /aria-valuemax=\{subagentPanelAriaMax\}/);
  assert.match(chat, /aria-valuenow=\{renderedSubagentPanelWidth\}/);
  assert.match(chat, /onPointerDown=\{startSubagentPanelResize\}/);
  assert.match(chat, /onMouseDown=\{startSubagentPanelResize\}/);
  assert.match(chat, /onKeyDown=\{onSubagentPanelHandleKeyDown\}/);
});

run('split drag and keyboard directions resize the right-hand subagent pane', () => {
  const chat = source('components/ChatView.jsx');
  assert.match(chat, /startWidth \+ startX - moveEvent\.clientX/);
  assert.match(chat, /event\.key === 'ArrowLeft' \? step : -step/);
  assert.match(chat, /subagentPanelResizeCleanupRef\.current\?\.\(\)/);
  assert.match(chat, /document\.body\.classList\.add\('ace-resizing'\)/);
  assert.match(chat, /document\.body\.classList\.remove\('ace-resizing'\)/);
});

run('SubagentPanel consumes the constrained width without a fixed 380px class', () => {
  const panel = source('components/SubagentPanel.jsx');
  assert.match(panel, /width = DEFAULT_SUBAGENT_PANEL_WIDTH/);
  assert.match(panel, /style=\{\{ width \}\}/);
  assert.doesNotMatch(panel, /w-\[380px\]/);
  assert.doesNotMatch(panel, /max-w-\[85%\]/);
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
  const opens = chat.match(/setSubagentPanelOpen\(true\)/g) || [];
  assert.equal(opens.length, 2);
  assert.match(chat, /const openSubagentTranscript = useCallback\(\(sessionId\) => \{\s*if \(!sessionId\) return;\s*setSubagentPanelOpen\(true\);/);
  // 后台任务入口在会话三点菜单里,始终可点(没有任务时打开空列表),不再是有任务才出现的顶栏按钮。
  assert.match(chat, /id: 'background_tasks', label: '后台任务'[\s\S]*?onSelect: \(\) => setSubagentPanelOpen\(true\)/);
  assert.doesNotMatch(chat, /subagentTasks\.tasks\.length > 0 \|\| subagentPanelOpen/);
  assert.match(chat, /onClose=\{\(\) => setSubagentPanelOpen\(false\)\}/);
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
