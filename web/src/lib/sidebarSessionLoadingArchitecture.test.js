import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { parseSync, traverse } from '@babel/core';

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
  assert.ok(
    selection.indexOf('onSelect?.({') < selection.indexOf('await sessionLoadPoolRef.current.request('),
    'main content must switch before the slow runtime resume settles',
  );
  assert.match(selection, /onSelect\?\.\(\{[\s\S]*active: session\.active === true/);
  assert.match(selection, /resumePending: session\.active !== true/);
  assert.match(selection, /preserveSidebarSessionLoading: true/);
  assert.match(source('lib/sessionTranscript.js'), /resumePending === true/);
});

run('最新点击立即提交导航且旧恢复结果不能覆盖它', () => {
  const sidebar = source('components/Sidebar.jsx');
  const selection = between(sidebar, 'const selectSession', 'const onRename');
  const latestGuards = selection.match(
    /sessionSelectionIntentRef\.current\?\.sequence !== sequence/g,
  ) || [];

  assert.ok(latestGuards.length >= 2, 'error and completion paths must reject stale resumes');
  assert.equal(
    (selection.match(/onSelect\?\.\(\{/g) || []).length,
    3,
    'the session is projected immediately, cleared on failure, and promoted on success',
  );
  const firstCommit = selection.indexOf('onSelect?.({');
  const resumeRequest = selection.indexOf('await sessionLoadPoolRef.current.request(');
  const liveCommit = selection.lastIndexOf('onSelect?.({');
  assert.ok(firstCommit < resumeRequest && resumeRequest < liveCommit,
    'the latest click must project before resume and promote after it');
});

run('乐观导航保留恢复意图，外部导航、归档和卸载会清理它', () => {
  const sidebar = source('components/Sidebar.jsx');
  const app = source('App.jsx');

  assert.match(sidebar, /revealedIntent && activeRef\?\.resumePending !== true/);
  assert.match(sidebar, /!revealedIntent && activeNavigationIdentity !== intent\.baselineNavigationIdentity/);
  assert.match(app, /if \(!options\.preserveSidebarSessionLoading\) resetSidebarSessionLoading\(\)/);
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
  assert.match(app, /phase=\{activeRef\?\.resumePending \? '' : \(sidebarSessionLoadState\?\.phase \|\| ''\)\}/);
  assert.match(chat, /phase=\{sessionContentLoadingPhase\(\{/);
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
  assert.match(
    loading,
    /title\s*\? t\('sessionNavigation\.sidebarLoading', \{ title \}\)\s*:\s*t\('sessionNavigation\.sidebarLoadingFallback'\)/,
  );
  assert.match(app, /<SessionNavigationMask\s+open=\{sessionNavigationPending\}/);
});

run('会话只有一个内容遮罩，直接挂在会话主列而非输入区', () => {
  const chat = source('components/ChatView.jsx');
  const ast = parseSync(chat, {
    configFile: false,
    babelrc: false,
    sourceType: 'module',
    parserOpts: { plugins: ['jsx'] },
  });
  const overlays = [];
  traverse(ast, {
    JSXElement(nodePath) {
      if (nodePath.node.openingElement.name.name === 'SessionContentLoading') overlays.push(nodePath);
    },
  });
  assert.equal(overlays.length, 1, '记录读取和运行环境恢复必须共用同一个遮罩');
  const parent = overlays[0].parentPath.node;
  assert.equal(parent.type, 'JSXElement');
  const anchor = parent.openingElement.attributes.find(
    (attribute) => attribute.name?.name === 'data-session-content-loading-anchor',
  );
  assert.equal(anchor?.value?.value, 'true', '遮罩必须以会话主列为直接父节点，不能落入输入区');
  assert.match(chat.slice(parent.start, parent.openingElement.end), /ace-session-panel[^']*relative/);
  assert.match(chat, /readOnly: readOnlyExternalSession/);
  assert.match(chat, /if \(sessionRuntimeUnavailable\) return fail\(\);/);
  assert.match(chat, /if \(!targetSid \|\| busy \|\| drainRef\.current \|\| sessionRuntimeUnavailable\) return;/);
});

run('会话加载反馈保留中英文文案', () => {
  const zh = source('i18n/catalogs/zh-CN.js');
  const en = source('i18n/catalogs/en-US.js');

  for (const catalog of [zh, en]) {
    assert.match(catalog, /sidebarQueued:/);
    assert.match(catalog, /sidebarLoading:/);
    assert.match(catalog, /sidebarLoadingFallback:/);
    assert.match(catalog, /transcriptLoading:/);
    assert.match(catalog, /transcriptError:/);
  }
});
