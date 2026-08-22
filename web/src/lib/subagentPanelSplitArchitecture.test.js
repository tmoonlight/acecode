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
  assert.match(taskState, /eventSessionId === parentId && payload\.tool === 'spawn_subagent'/);
  assert.match(taskHook, /useSubagentTasks\(parentSessionId, \{ onSpawnStart \} = \{\}\)/);
  assert.match(taskHook, /isSubagentSpawnStartEvent\(parentSessionId, msg\)/);
  assert.match(taskHook, /onSpawnStartRef\.current\?\.\(msg\)/);
  assert.match(chat, /openSubagentPanelForSpawn[\s\S]*setSubagentPanelOpen\(true\)/);
  assert.match(chat, /useSubagentTasks\(sid, \{\s*onSpawnStart: openSubagentPanelForSpawn,\s*\}\)/);
  assert.match(chat, /onClick=\{\(\) => setSubagentPanelOpen\(\(v\) => !v\)\}/);
  assert.match(chat, /onClose=\{\(\) => setSubagentPanelOpen\(false\)\}/);
});

console.log('subagentPanelSplitArchitecture.test.js: all tests passed');
