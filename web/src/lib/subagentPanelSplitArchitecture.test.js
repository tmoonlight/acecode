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

run('live spawn_subagent tool_start opens the panel through the task hook', () => {
  const taskState = source('lib/subagentTasks.js');
  const taskHook = source('lib/useSubagentTasks.js');
  const chat = source('components/ChatView.jsx');

  assert.match(taskState, /export function isSubagentSpawnStartEvent\(parentSessionId, msg\)/);
  assert.match(taskState, /msg\?\.type !== 'tool_start'/);
  // 星型 spawn_subagent 与网状 agent_spawn(add-mesh-swarm-mode)都是面板打开信号。
  assert.match(
    taskState,
    /eventSessionId === parentId &&\s*\(payload\.tool === 'spawn_subagent' \|\| payload\.tool === 'agent_spawn'\)/,
  );
  assert.match(taskHook, /useSubagentTasks\(parentSessionId, \{ onSpawnStart, workspaceHash = '' \} = \{\}\)/);
  assert.match(taskHook, /isSubagentSpawnStartEvent\(parentSessionId, msg\)/);
  assert.match(taskHook, /onSpawnStartRef\.current\?\.\(msg\)/);
  assert.match(chat, /openSubagentPanelForSpawn[\s\S]*setSubagentPanelOpen\(true\)/);
  assert.match(chat, /useSubagentTasks\(sid, \{\s*onSpawnStart: openSubagentPanelForSpawn,\s*workspaceHash: subagentWorkspaceHash,\s*\}\)/);
  assert.match(chat, /onClick=\{\(\) => setSubagentPanelOpen\(\(v\) => !v\)\}/);
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
  assert.match(chat, /useSubagentTasks\(sid, \{\s*onSpawnStart: openSubagentPanelForSpawn,\s*workspaceHash: subagentWorkspaceHash,\s*\}\)/);
  assert.match(chat, /workspaceHash=\{subagentWorkspaceHash\}/);
  assert.match(hook, /useSubagentTasks\(parentSessionId, \{ onSpawnStart, workspaceHash = '' \} = \{\}\)/);
});

// 场景:用户点后台任务面板「已完成」组的按钮。
// 期望:只归档(从面板收起),不再永久删除;子会话与普通会话一样长期保存,
// 主会话永久删除时由 daemon 级联删除。按钮文案随之改为「归档」。
// 回归:旧按钮「清除」直接 purge,子会话记录一点就没了。
run('settled subagent tasks are archived, never purged from the panel', () => {
  const panel = source('components/SubagentPanel.jsx');
  const hook = source('lib/useSubagentTasks.js');

  assert.match(hook, /api\.archiveWorkspaceSession\(workspaceHash, id\)/);
  assert.match(hook, /: api\.archiveSession\(id\)\)/);
  const clearStart = hook.indexOf('const clearSettled = useCallback');
  assert.ok(clearStart >= 0);
  assert.doesNotMatch(hook, /purgeSession|purgeTask/);
  assert.match(panel, /\{clearing \? '归档中…' : '归档'\}/);
  assert.doesNotMatch(panel, /永久删除全部已结束任务/);
});

console.log('subagentPanelSplitArchitecture.test.js: all tests passed');
