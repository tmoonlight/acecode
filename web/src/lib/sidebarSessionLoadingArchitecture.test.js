import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const srcRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

function source(relativePath) {
  return fs.readFileSync(path.join(srcRoot, relativePath), 'utf8').replace(/\r\n?/g, '\n');
}

function between(text, start, end) {
  const startIndex = text.indexOf(start);
  const endIndex = text.indexOf(end, startIndex);
  assert.notEqual(startIndex, -1, `missing start marker: ${start}`);
  assert.notEqual(endIndex, -1, `missing end marker: ${end}`);
  return text.slice(startIndex, endIndex);
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

run('侧栏先投影最新选择，再通过有界池恢复会话', () => {
  const sidebar = source('components/Sidebar.jsx');
  const selection = between(sidebar, 'const resumeSidebarSession', 'const onRename');

  assert.match(sidebar, /createSidebarSessionLoadPool\(\{/);
  assert.match(
    sidebar,
    /sidebarRevealTarget\(sessionSelectionIntent\?\.target \|\| activeRef\)/,
  );
  assert.match(sidebar, /activeTarget=\{selectedRevealTarget\}/);
  assert.match(selection, /replaceSessionSelectionIntent\(intent\)/);
  assert.match(selection, /sessionLoadPoolRef\.current\.request\(/);
  assert.match(selection, /sessionLoadPoolRef\.current\.cancelPending\(\)/);
});

run('只有仍为最新点击的恢复结果可以提交导航', () => {
  const sidebar = source('components/Sidebar.jsx');
  const selection = between(sidebar, 'const selectSession', 'const onRename');
  const latestGuards = selection.match(
    /sessionSelectionIntentRef\.current\?\.sequence !== sequence/g,
  ) || [];

  assert.ok(latestGuards.length >= 3, 'error, result and commit paths must all reject stale clicks');
  assert.ok(
    selection.indexOf('sessionSelectionIntentRef.current?.sequence !== sequence')
      < selection.indexOf('onSelect?.({'),
    'latest-only guard must run before navigation commit',
  );
  assert.match(selection, /if \(result\.status === 'superseded'\) return/);
});

run('外部导航、归档和卸载会清理侧栏 pending 意图', () => {
  const sidebar = source('components/Sidebar.jsx');
  const app = source('App.jsx');

  assert.match(sidebar, /sessionLoadResetSequence = 0/);
  assert.match(sidebar, /cancelSessionSelection\(\);\s*\}, \[cancelSessionSelection, sessionLoadResetSequence\]\)/);
  assert.match(sidebar, /sessionSelectionIntentRef\.current\?\.loadKey === loadKey[\s\S]*cancelSessionSelection\(\)/);
  assert.match(sidebar, /sessionLoadPoolRef\.current\?\.dispose\(\)/);
  assert.match(app, /resetSidebarSessionLoading\(\);\s*const navigationId = beginSessionNavigation\(\)/);
  assert.match(app, /sessionLoadResetSequence=\{sidebarSessionLoadResetSequence\}/);
});

run('右侧局部遮罩不复用全屏导航遮罩且不会挡住侧栏', () => {
  const app = source('App.jsx');
  const chat = source('components/ChatView.jsx');
  const loading = source('components/SessionContentLoading.jsx');

  assert.match(app, /onSessionLoadStateChange=\{setSidebarSessionLoadState\}/);
  assert.match(app, /<SessionContentLoading\s+phase=\{sidebarSessionLoadState\?\.phase \|\| ''\}/);
  assert.match(app, /anchorSelector="\[data-session-content-loading-anchor='true'\]"/);
  assert.equal(
    (chat.match(/data-session-content-loading-anchor="true"/g) || []).length,
    2,
    'home and active conversation columns must both expose the visual center anchor',
  );
  assert.match(loading, /absolute inset-0 z-\[80\]/);
  assert.doesNotMatch(loading, /fixed inset-0/);
  assert.match(loading, /DEFAULT_REVEAL_DELAY_MS = 160/);
  assert.match(loading, /data-ace-native-overlay="blocking"/);
  assert.match(loading, /data-session-content-loading=\{phase\}/);
  assert.match(loading, /sessionContentLoadingAnchorFrame\(/);
  assert.match(loading, /new window\.ResizeObserver\(update\)/);
  assert.match(loading, /role="status"/);
  assert.match(app, /<SessionNavigationMask\s+open=\{sessionNavigationPending\}/);
});

run('ChatView 在 transcript 加载和失败时给出局部反馈', () => {
  const chat = source('components/ChatView.jsx');
  const zh = source('i18n/catalogs/zh-CN.js');
  const en = source('i18n/catalogs/en-US.js');

  assert.match(
    chat,
    /transcriptLoadState === 'loading'[\s\S]*\? 'transcript'[\s\S]*transcriptLoadState === 'error' \? 'error'/,
  );
  for (const catalog of [zh, en]) {
    assert.match(catalog, /sidebarQueued:/);
    assert.match(catalog, /sidebarLoading:/);
    assert.match(catalog, /transcriptLoading:/);
    assert.match(catalog, /transcriptError:/);
  }
});
