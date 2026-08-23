import assert from 'node:assert/strict';
import {
  activePreviewTab,
  activatePreviewTab,
  closePreviewTab,
  closeVisiblePreviewTabs,
  closeVisiblePreviewTabsConfirmationMessage,
  defaultBrowserTabTitle,
  discardFileTabDraft,
  normalizeBrowserTabFavicon,
  openFileTab,
  openBrowserTab,
  openGitChangesTab,
  openSessionChangesTab,
  previewAbsolutePath,
  previewFileLocation,
  previewScopeKey,
  previewTabHasUnsavedDraft,
  previewTabsWithUnsavedDrafts,
  refreshPreviewTab,
  reorderPreviewTab,
  resolveSessionChangesTabContent,
  sessionWorkingCwd,
  updateBrowserTabFavicon,
  updateBrowserTabMetadata,
  updateBrowserTabTitle,
  updateGitChangesTab,
  updateFileTabDraft,
  updateSessionChangesTab,
  visibleUnsavedPreviewTabs,
  visiblePreviewTabs,
} from './previewTabs.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('openFileTab scopes files by workspace and reuses duplicate paths', () => {
  let state = {};
  state = openFileTab(state, { scopeKey: 'workspace-a', sessionId: 's1', cwd: 'C:/a', path: 'src\\main.cpp' });
  state = openFileTab(state, { scopeKey: 'workspace-a', sessionId: 's1', cwd: 'C:/a', path: 'src/main.cpp' });
  state = openFileTab(state, { scopeKey: 'workspace-b', sessionId: 's2', cwd: 'C:/b', path: 'src/main.cpp' });

  assert.equal(visiblePreviewTabs(state, { scopeKey: 'workspace-a', sessionId: 's1' }).length, 1);
  assert.equal(visiblePreviewTabs(state, { scopeKey: 'workspace-b', sessionId: 's2' }).length, 1);
  assert.equal(activePreviewTab(state, { scopeKey: 'workspace-a', sessionId: 's1' }).path, 'src/main.cpp');
});

run('file preview drafts remain tab-scoped and derive the unsaved state from text', () => {
  let state = openFileTab({}, {
    scopeKey: 'workspace-a', sessionId: 's1', cwd: 'C:/a', path: 'notes.md',
  });
  const tabKey = activePreviewTab(state, { scopeKey: 'workspace-a', sessionId: 's1' }).key;
  state = updateFileTabDraft(state, {
    scopeKey: 'workspace-a',
    tabKey,
    patch: {
      editing: true,
      baselineText: '# 原文\n',
      text: '# 草稿\n',
      readId: 'read-1',
    },
  });

  const active = activePreviewTab(state, { scopeKey: 'workspace-a', sessionId: 's1' });
  assert.equal(active.edit.text, '# 草稿\n');
  assert.equal(active.edit.dirty, true);
  assert.equal(previewTabHasUnsavedDraft(active), true);
  assert.deepEqual(previewTabsWithUnsavedDrafts([active]), [active]);
  assert.deepEqual(
    visibleUnsavedPreviewTabs(state, { scopeKey: 'workspace-a', sessionId: 's1' }),
    [active],
  );

  state = updateFileTabDraft(state, {
    scopeKey: 'workspace-a', tabKey, patch: { baselineText: '# 草稿\n' },
  });
  assert.equal(
    previewTabHasUnsavedDraft(activePreviewTab(state, { scopeKey: 'workspace-a', sessionId: 's1' })),
    false,
  );
});

run('discarding a file draft restores the latest baseline and keeps direct editing open', () => {
  let state = openFileTab({}, {
    scopeKey: 'workspace-a', sessionId: 's1', cwd: 'C:/a', path: 'a.txt',
  });
  const tabKey = activePreviewTab(state, { scopeKey: 'workspace-a', sessionId: 's1' }).key;
  state = updateFileTabDraft(state, {
    scopeKey: 'workspace-a',
    tabKey,
    patch: { editing: true, baselineText: 'saved', text: 'draft', error: 'conflict' },
  });
  state = discardFileTabDraft(state, { scopeKey: 'workspace-a', tabKey });
  const tab = activePreviewTab(state, { scopeKey: 'workspace-a', sessionId: 's1' });
  assert.equal(tab.edit.text, 'saved');
  assert.equal(tab.edit.editing, true);
  assert.equal(tab.edit.dirty, false);
  assert.equal(tab.edit.error, '');
});

run('automatic file refresh preserves dirty drafts and marks possible disk changes', () => {
  let state = openFileTab({}, {
    scopeKey: 'workspace-a', sessionId: 's1', cwd: 'C:/a', path: 'a.txt',
  });
  const tabKey = activePreviewTab(state, { scopeKey: 'workspace-a', sessionId: 's1' }).key;
  state = updateFileTabDraft(state, {
    scopeKey: 'workspace-a',
    tabKey,
    patch: { editing: true, baselineText: 'saved', text: 'draft' },
  });
  state = refreshPreviewTab(state, { scopeKey: 'workspace-a', sessionId: 's1', tabKey });
  const tab = activePreviewTab(state, { scopeKey: 'workspace-a', sessionId: 's1' });
  assert.equal(tab.edit.text, 'draft');
  assert.equal(tab.edit.externalChanged, true);
  assert.equal(tab.reloadRevision || 0, 0);
});

run('openBrowserTab keeps distinct page-backed Browser tabs and deduplicates state events', () => {
  let state = openFileTab({}, {
    scopeKey: 'workspace-a', sessionId: 's1', cwd: 'C:/a', path: 'a.txt',
  });
  state = openBrowserTab(state, {
    scopeKey: 'workspace-a', sessionId: 's1', pageId: 'page-1',
  });
  state = openBrowserTab(state, {
    scopeKey: 'workspace-a', sessionId: 's1', pageId: 'page-2',
  });
  state = openBrowserTab(state, {
    scopeKey: 'workspace-a', sessionId: 's1', pageId: 'page-2',
  });

  const tabs = visiblePreviewTabs(state, { scopeKey: 'workspace-a', sessionId: 's1' });
  assert.deepEqual(tabs.map((tab) => tab.type), ['file', 'browser', 'browser']);
  assert.deepEqual(tabs.filter((tab) => tab.type === 'browser').map((tab) => tab.pageId), [
    'page-1', 'page-2',
  ]);
  assert.equal(activePreviewTab(state, { scopeKey: 'workspace-a', sessionId: 's1' }).pageId, 'page-2');
  assert.deepEqual(
    tabs.filter((tab) => tab.type === 'browser').map((tab) => tab.title),
    [defaultBrowserTabTitle(), defaultBrowserTabTitle()],
  );
  assert.equal(state.browserTabsBySession.s1.length, 2);
});

run('Browser tab titles sync by page id without activating or reordering background tabs', () => {
  let state = openBrowserTab({}, {
    scopeKey: 'workspace-a', sessionId: 's1', pageId: 'page-1', title: 'First page',
  });
  state = openBrowserTab(state, {
    scopeKey: 'workspace-a', sessionId: 's1', pageId: 'page-2', title: 'Second page',
  });
  const activeBefore = activePreviewTab(state, {
    scopeKey: 'workspace-a', sessionId: 's1',
  }).key;
  const orderBefore = visiblePreviewTabs(state, {
    scopeKey: 'workspace-a', sessionId: 's1',
  }).map((tab) => tab.key);

  state = updateBrowserTabTitle(state, {
    sessionId: 's1', pageId: 'page-1', title: 'Updated first page',
  });

  const tabs = visiblePreviewTabs(state, { scopeKey: 'workspace-a', sessionId: 's1' });
  assert.equal(tabs.find((tab) => tab.pageId === 'page-1').title, 'Updated first page');
  assert.equal(tabs.find((tab) => tab.pageId === 'page-2').title, 'Second page');
  assert.equal(activePreviewTab(state, { scopeKey: 'workspace-a', sessionId: 's1' }).key, activeBefore);
  assert.deepEqual(tabs.map((tab) => tab.key), orderBefore);
});

run('Browser tab title fallback is 新标签页 and activation without a title preserves the current title', () => {
  let state = openBrowserTab({}, {
    scopeKey: 'workspace-a', sessionId: 's1', pageId: 'page-1', title: 'Example Domain',
  });
  state = openBrowserTab(state, {
    scopeKey: 'workspace-a', sessionId: 's1', pageId: 'page-1',
  });
  assert.equal(
    activePreviewTab(state, { scopeKey: 'workspace-a', sessionId: 's1' }).title,
    'Example Domain',
  );

  state = updateBrowserTabTitle(state, {
    sessionId: 's1', pageId: 'page-1', title: '  ',
  });
  assert.equal(
    activePreviewTab(state, { scopeKey: 'workspace-a', sessionId: 's1' }).title,
    defaultBrowserTabTitle(),
  );
});

run('Browser favicons sync by page id, reject unsafe schemes, and preserve tab order', () => {
  const firstIcon = 'data:image/svg+xml;base64,PHN2Zy8+';
  const secondIcon = 'https://example.org/favicon.ico';
  let state = openBrowserTab({}, {
    scopeKey: 'workspace-a', sessionId: 's1', pageId: 'page-1', favicon: firstIcon,
  });
  state = openBrowserTab(state, {
    scopeKey: 'workspace-a', sessionId: 's1', pageId: 'page-2', favicon: secondIcon,
  });
  const activeBefore = activePreviewTab(state, {
    scopeKey: 'workspace-a', sessionId: 's1',
  }).key;
  const orderBefore = visiblePreviewTabs(state, {
    scopeKey: 'workspace-a', sessionId: 's1',
  }).map((tab) => tab.key);

  state = updateBrowserTabMetadata(state, {
    sessionId: 's1', pageId: 'page-1', title: 'Icon page', favicon: 'https://example.com/icon.png',
  });
  let tabs = visiblePreviewTabs(state, { scopeKey: 'workspace-a', sessionId: 's1' });
  assert.equal(tabs.find((tab) => tab.pageId === 'page-1').favicon, 'https://example.com/icon.png');
  assert.equal(tabs.find((tab) => tab.pageId === 'page-1').title, 'Icon page');
  assert.equal(tabs.find((tab) => tab.pageId === 'page-2').favicon, secondIcon);
  assert.equal(activePreviewTab(state, { scopeKey: 'workspace-a', sessionId: 's1' }).key, activeBefore);
  assert.deepEqual(tabs.map((tab) => tab.key), orderBefore);

  state = updateBrowserTabFavicon(state, {
    sessionId: 's1', pageId: 'page-1', favicon: 'javascript:alert(1)',
  });
  tabs = visiblePreviewTabs(state, { scopeKey: 'workspace-a', sessionId: 's1' });
  assert.equal(tabs.find((tab) => tab.pageId === 'page-1').favicon, '');
  assert.equal(normalizeBrowserTabFavicon('file:///favicon.ico'), '');
});

run('closing one Browser tab preserves sibling Browser and file tabs', () => {
  let state = openFileTab({}, {
    scopeKey: 'workspace-a', sessionId: 's1', cwd: 'C:/a', path: 'a.txt',
  });
  state = openBrowserTab(state, {
    scopeKey: 'workspace-a', sessionId: 's1', pageId: 'page-1',
  });
  state = openBrowserTab(state, {
    scopeKey: 'workspace-a', sessionId: 's1', pageId: 'page-2',
  });
  const browser = activePreviewTab(state, { scopeKey: 'workspace-a', sessionId: 's1' });
  state = closePreviewTab(state, {
    scopeKey: 'workspace-a', sessionId: 's1', tabKey: browser.key,
  });

  assert.deepEqual(state.browserTabsBySession.s1.map((tab) => tab.pageId), ['page-1']);
  assert.deepEqual(
    visiblePreviewTabs(state, { scopeKey: 'workspace-a', sessionId: 's1' }).map((tab) => tab.type),
    ['file', 'browser'],
  );
});

run('refreshPreviewTab increments only the requested file tab reload revision', () => {
  let state = {};
  state = openFileTab(state, { scopeKey: 'w', sessionId: 's1', cwd: 'C:/repo', path: 'a.js' });
  state = openFileTab(state, { scopeKey: 'w', sessionId: 's1', cwd: 'C:/repo', path: 'b.js' });
  const before = visiblePreviewTabs(state, { scopeKey: 'w', sessionId: 's1' });
  const aKey = before.find((tab) => tab.path === 'a.js').key;

  state = refreshPreviewTab(state, { scopeKey: 'w', sessionId: 's1', tabKey: aKey });
  const after = visiblePreviewTabs(state, { scopeKey: 'w', sessionId: 's1' });
  assert.equal(after.find((tab) => tab.path === 'a.js').reloadRevision, 1);
  assert.equal(after.find((tab) => tab.path === 'b.js').reloadRevision, undefined);
});

run('refreshPreviewTab increments change-tab revision and preserves it when selecting a file', () => {
  let state = openSessionChangesTab({}, {
    scopeKey: 'w', sessionId: 's1', expandedFile: 'a.js', fileCount: 1,
  });
  let tab = activePreviewTab(state, { scopeKey: 'w', sessionId: 's1' });
  state = refreshPreviewTab(state, { scopeKey: 'w', sessionId: 's1', tabKey: tab.key });
  state = openSessionChangesTab(state, {
    scopeKey: 'w', sessionId: 's1', expandedFile: 'b.js', fileCount: 2,
  });
  tab = activePreviewTab(state, { scopeKey: 'w', sessionId: 's1' });
  assert.equal(tab.reloadRevision, 1);
  assert.equal(tab.expandedFile, 'b.js');
});

run('refreshPreviewTab leaves state identity unchanged for unknown tabs', () => {
  const state = { fileTabsByScope: {} };
  assert.equal(refreshPreviewTab(state, { scopeKey: 'w', sessionId: 's1', tabKey: 'file:w:missing' }), state);
  assert.equal(refreshPreviewTab(state, { scopeKey: 'w', sessionId: 's1', tabKey: 'unknown' }), state);
});

// 场景:聊天正文 foo.cpp:42 链接带行号打开文件预览。
// 期望:新 tab 记录 line + lineRevision=1;同 tab 再次带行号打开时 line 更新且
// revision 递增(重复点击同一链接也要重新触发滚动);不带行号的打开(文件树
// 点击)不清已有定位、revision 不动。
run('openFileTab records focus line and bumps revision on repeat', () => {
  let state = {};
  state = openFileTab(state, { scopeKey: 'w', sessionId: 's1', cwd: 'C:/a', path: 'src/a.cpp', line: 42 });
  let tab = activePreviewTab(state, { scopeKey: 'w', sessionId: 's1' });
  assert.equal(tab.line, 42);
  assert.equal(tab.lineRevision, 1);

  // 同一链接再点一次:line 相同,revision 必须递增才能重新滚动
  state = openFileTab(state, { scopeKey: 'w', sessionId: 's1', cwd: 'C:/a', path: 'src/a.cpp', line: 42 });
  tab = activePreviewTab(state, { scopeKey: 'w', sessionId: 's1' });
  assert.equal(tab.lineRevision, 2);

  // 换行号:line 更新
  state = openFileTab(state, { scopeKey: 'w', sessionId: 's1', cwd: 'C:/a', path: 'src/a.cpp', line: 7 });
  tab = activePreviewTab(state, { scopeKey: 'w', sessionId: 's1' });
  assert.equal(tab.line, 7);
  assert.equal(tab.lineRevision, 3);

  // 不带行号重开(文件树点击):保留定位,revision 不动
  state = openFileTab(state, { scopeKey: 'w', sessionId: 's1', cwd: 'C:/a', path: 'src/a.cpp' });
  tab = activePreviewTab(state, { scopeKey: 'w', sessionId: 's1' });
  assert.equal(tab.line, 7);
  assert.equal(tab.lineRevision, 3);
});

// 边界:非法行号(0/负数/NaN/非数字)一律当作未带行号,tab 不携带 line 字段。
run('openFileTab ignores invalid line values', () => {
  for (const bad of [0, -3, NaN, 'x']) {
    const state = openFileTab({}, { scopeKey: 'w', sessionId: 's1', cwd: 'C:/a', path: 'src/a.cpp', line: bad });
    const tab = activePreviewTab(state, { scopeKey: 'w', sessionId: 's1' });
    assert.equal(tab.line, undefined, `line=${bad} 应被忽略`);
    assert.equal(tab.lineRevision, undefined, `lineRevision(line=${bad}) 应被忽略`);
  }
});

run('previewFileLocation splits absolute Windows paths when cwd is unavailable', () => {
  assert.deepEqual(
    previewFileLocation({ cwd: '', path: 'C:\\Users\\shao\\ttt\\hello.txt' }),
    { cwd: 'C:/Users/shao/ttt', path: 'hello.txt' },
  );
  assert.deepEqual(
    previewFileLocation({ cwd: 'C:/repo', path: 'src/main.cpp' }),
    { cwd: 'C:/repo', path: 'src/main.cpp' },
  );
  assert.deepEqual(
    previewFileLocation({ cwd: '/home/shao/repo/', path: 'src/main.cpp' }),
    { cwd: '/home/shao/repo', path: 'src/main.cpp' },
  );
});

run('sessionWorkingCwd prefers an active worktree and keeps workspace fallback behavior', () => {
  assert.equal(
    sessionWorkingCwd({
      worktree: { path: 'N:\\repo\\.acecode\\worktrees\\ses-123\\' },
      cwd: 'N:/repo',
      fallbackCwd: 'N:/daemon',
    }),
    'N:/repo/.acecode/worktrees/ses-123',
  );
  assert.equal(
    sessionWorkingCwd({ worktree: { name: 'ses-legacy' }, cwd: 'N:/repo/' }),
    'N:/repo',
  );
  assert.equal(
    sessionWorkingCwd({ worktree: { path: '\\\\?\\N:\\repo\\worktree\\' } }),
    'N:/repo/worktree',
  );
  assert.equal(
    sessionWorkingCwd({ worktree: { path: '\\\\server\\share\\repo\\worktree\\' } }),
    '//server/share/repo/worktree',
  );
  assert.equal(sessionWorkingCwd({ fallbackCwd: '/srv/repo/' }), '/srv/repo');
});

run('previewFileLocation resolves absolute links under the worktree without duplicating the root', () => {
  const worktree = 'N:/repo/.acecode/worktrees/ses-123';
  assert.deepEqual(
    previewFileLocation({
      cwd: worktree,
      path: 'n:\\repo\\.acecode\\worktrees\\ses-123\\docs\\design.md',
    }),
    { cwd: worktree, path: 'docs/design.md' },
  );
  assert.deepEqual(
    previewFileLocation({ cwd: '/srv/repo/wt', path: '/srv/repo/wt/docs/design.md' }),
    { cwd: '/srv/repo/wt', path: 'docs/design.md' },
  );
  assert.deepEqual(
    previewFileLocation({
      cwd: '//SERVER/share/repo/wt',
      path: '\\\\server\\share\\repo\\wt\\docs\\design.md',
    }),
    { cwd: '//SERVER/share/repo/wt', path: 'docs/design.md' },
  );
});

run('previewAbsolutePath produces the same canonical target for relative and absolute links', () => {
  const worktree = 'N:/repo/.acecode/worktrees/ses-123';
  const absolute = `${worktree}/docs/design.md`;
  assert.equal(previewAbsolutePath({ cwd: worktree, path: 'docs/design.md' }), absolute);
  assert.equal(previewAbsolutePath({ cwd: worktree, path: absolute }), absolute);
  assert.equal(
    previewAbsolutePath({ cwd: worktree, path: 'N:\\repo\\.acecode\\worktrees\\ses-123\\docs\\design.md' }),
    absolute,
  );
});

run('previewScopeKey isolates worktree file tabs from the shared workspace scope', () => {
  const workspaceScope = previewScopeKey({ cwd: 'N:/repo', workspaceHash: 'workspace-1' });
  const worktreeScope = previewScopeKey({
    cwd: 'N:/repo/.acecode/worktrees/ses-123',
    workspaceHash: 'workspace-1',
    worktreePath: 'N:\\repo\\.acecode\\worktrees\\ses-123',
  });
  assert.equal(workspaceScope, 'workspace-1');
  assert.equal(worktreeScope, 'worktree:N:/repo/.acecode/worktrees/ses-123');

  let state = openFileTab({}, {
    scopeKey: workspaceScope,
    sessionId: 'main-session',
    cwd: 'N:/repo',
    path: 'docs/design.md',
  });
  state = openFileTab(state, {
    scopeKey: worktreeScope,
    sessionId: 'worktree-session',
    cwd: 'N:/repo/.acecode/worktrees/ses-123',
    path: 'docs/design.md',
  });
  assert.equal(visiblePreviewTabs(state, {
    scopeKey: workspaceScope,
    sessionId: 'main-session',
  }).length, 1);
  assert.equal(visiblePreviewTabs(state, {
    scopeKey: worktreeScope,
    sessionId: 'worktree-session',
  }).length, 1);
});

run('openSessionChangesTab scopes change tab by session id', () => {
  let state = {};
  state = openSessionChangesTab(state, {
    scopeKey: 'workspace-a',
    sessionId: 's1',
    expandedFile: 'src/a.cpp',
    fileCount: 2,
  });
  state = openSessionChangesTab(state, {
    scopeKey: 'workspace-a',
    sessionId: 's2',
    expandedFile: 'src/b.cpp',
    fileCount: 3,
  });

  assert.equal(visiblePreviewTabs(state, { scopeKey: 'workspace-a', sessionId: 's1' }).length, 1);
  assert.equal(activePreviewTab(state, { scopeKey: 'workspace-a', sessionId: 's1' }).expandedFile, 'src/a.cpp');
  assert.equal(activePreviewTab(state, { scopeKey: 'workspace-a', sessionId: 's2' }).expandedFile, 'src/b.cpp');
});

run('openSessionChangesTab reuses the same session tab and updates clicked file', () => {
  let state = {};
  state = openSessionChangesTab(state, {
    scopeKey: 'workspace-a',
    sessionId: 's1',
    expandedFile: 'src/a.cpp',
    fileCount: 2,
  });
  state = openSessionChangesTab(state, {
    scopeKey: 'workspace-a',
    sessionId: 's1',
    expandedFile: 'src/b.cpp',
    fileCount: 3,
  });

  const tabs = visiblePreviewTabs(state, { scopeKey: 'workspace-a', sessionId: 's1' });
  assert.equal(tabs.length, 1);
  assert.equal(tabs[0].expandedFile, 'src/b.cpp');
  assert.equal(tabs[0].fileCount, 3);
});

run('openSessionChangesTab increments expanded file revision for repeated clicks', () => {
  let state = {};
  state = openSessionChangesTab(state, {
    scopeKey: 'workspace-a',
    sessionId: 's1',
    expandedFile: 'src/a.cpp',
    fileCount: 2,
  });
  const firstRevision = activePreviewTab(state, { scopeKey: 'workspace-a', sessionId: 's1' }).expandedFileRevision;

  state = openSessionChangesTab(state, {
    scopeKey: 'workspace-a',
    sessionId: 's1',
    expandedFile: 'src/a.cpp',
    fileCount: 2,
  });

  const active = activePreviewTab(state, { scopeKey: 'workspace-a', sessionId: 's1' });
  assert.equal(active.expandedFile, 'src/a.cpp');
  assert.equal(active.expandedFileRevision, firstRevision + 1);
});

run('轮次变更页签在详情内切文件时保持 user message UUID，全会话入口会清除它', () => {
  let state = {};
  state = openSessionChangesTab(state, {
    scopeKey: 'workspace-a',
    sessionId: 's1',
    expandedFile: 'src/a.cpp',
    fileCount: 2,
    turnUserMessageId: 'user-turn-1',
  });
  state = openSessionChangesTab(state, {
    scopeKey: 'workspace-a',
    sessionId: 's1',
    expandedFile: 'src/b.cpp',
    fileCount: 2,
    turnUserMessageId: 'user-turn-1',
  });

  let active = activePreviewTab(state, { scopeKey: 'workspace-a', sessionId: 's1' });
  assert.equal(active.expandedFile, 'src/b.cpp');
  assert.equal(active.turnUserMessageId, 'user-turn-1');

  state = openSessionChangesTab(state, {
    scopeKey: 'workspace-a',
    sessionId: 's1',
    expandedFile: 'src/global.cpp',
    fileCount: 5,
  });
  active = activePreviewTab(state, { scopeKey: 'workspace-a', sessionId: 's1' });
  assert.equal(active.expandedFile, 'src/global.cpp');
  assert.equal(active.turnUserMessageId, '');
  assert.equal(active.fileCount, 5);
});

run('resolveSessionChangesTabContent 只返回页签 UUID 对应轮次的 groups 与 summary', () => {
  const globalGroups = [{ file: 'global.cpp' }];
  const globalSummary = { fileCount: 9, totalAdditions: 9, totalDeletions: 0, hasChanges: true };
  const turn = {
    userMessageId: 'user-turn-2',
    groups: [{ file: 'turn.cpp' }],
    summary: { fileCount: 1, totalAdditions: 2, totalDeletions: 1, hasChanges: true },
  };

  assert.deepEqual(resolveSessionChangesTabContent(
    { turnUserMessageId: 'user-turn-2' },
    { changeGroups: globalGroups, changeSummary: globalSummary, turnChangeSets: [turn] },
  ), {
    groups: turn.groups,
    summary: turn.summary,
    turnUserMessageId: 'user-turn-2',
  });
  assert.deepEqual(resolveSessionChangesTabContent(
    { turnUserMessageId: '' },
    { changeGroups: globalGroups, changeSummary: globalSummary, turnChangeSets: [turn] },
  ), {
    groups: globalGroups,
    summary: globalSummary,
    turnUserMessageId: '',
  });
  assert.deepEqual(resolveSessionChangesTabContent(
    { turnUserMessageId: 'missing-turn' },
    { changeGroups: globalGroups, changeSummary: globalSummary, turnChangeSets: [turn] },
  ), {
    groups: [],
    summary: { fileCount: 0, totalAdditions: 0, totalDeletions: 0, hasChanges: false },
    turnUserMessageId: 'missing-turn',
  });
});

run('file tab can become active when session change tab also exists', () => {
  let state = {};
  state = openSessionChangesTab(state, {
    scopeKey: 'workspace-a',
    sessionId: 's1',
    expandedFile: 'src/a.cpp',
    fileCount: 1,
  });
  state = openFileTab(state, {
    scopeKey: 'workspace-a',
    sessionId: 's1',
    cwd: 'C:/a',
    path: 'README.md',
  });

  const active = activePreviewTab(state, { scopeKey: 'workspace-a', sessionId: 's1' });
  assert.equal(active.type, 'file');
  assert.equal(active.path, 'README.md');
});

run('closePreviewTab activates nearest remaining tab and hides when empty', () => {
  let state = {};
  state = openFileTab(state, { scopeKey: 'workspace-a', sessionId: 's1', cwd: 'C:/a', path: 'a.txt' });
  state = openFileTab(state, { scopeKey: 'workspace-a', sessionId: 's1', cwd: 'C:/a', path: 'b.txt' });
  const first = visiblePreviewTabs(state, { scopeKey: 'workspace-a', sessionId: 's1' })[0];
  const second = visiblePreviewTabs(state, { scopeKey: 'workspace-a', sessionId: 's1' })[1];

  state = closePreviewTab(state, { scopeKey: 'workspace-a', sessionId: 's1', tabKey: second.key });
  assert.equal(activePreviewTab(state, { scopeKey: 'workspace-a', sessionId: 's1' }).key, first.key);

  state = closePreviewTab(state, { scopeKey: 'workspace-a', sessionId: 's1', tabKey: first.key });
  assert.equal(visiblePreviewTabs(state, { scopeKey: 'workspace-a', sessionId: 's1' }).length, 0);
  assert.equal(activePreviewTab(state, { scopeKey: 'workspace-a', sessionId: 's1' }), null);
});

run('closeVisiblePreviewTabs clears current visible scope and session', () => {
  let state = {};
  state = openFileTab(state, { scopeKey: 'workspace-a', sessionId: 's1', cwd: 'C:/a', path: 'a.txt' });
  state = openSessionChangesTab(state, { scopeKey: 'workspace-a', sessionId: 's1', expandedFile: 'a.txt', fileCount: 1 });
  state = openFileTab(state, { scopeKey: 'workspace-b', sessionId: 's2', cwd: 'C:/b', path: 'b.txt' });

  state = closeVisiblePreviewTabs(state, { scopeKey: 'workspace-a', sessionId: 's1' });
  assert.equal(visiblePreviewTabs(state, { scopeKey: 'workspace-a', sessionId: 's1' }).length, 0);
  assert.equal(visiblePreviewTabs(state, { scopeKey: 'workspace-b', sessionId: 's2' }).length, 1);
});

run('closeVisiblePreviewTabsConfirmationMessage warns about all visible tabs', () => {
  assert.equal(
    closeVisiblePreviewTabsConfirmationMessage(3),
    '关闭预览面板会关闭当前预览区域的全部 3 个标签页。是否继续？',
  );
  assert.equal(closeVisiblePreviewTabsConfirmationMessage(0), '');
  assert.equal(closeVisiblePreviewTabsConfirmationMessage(-1), '');
});

run('updateSessionChangesTab updates global count without replacing expanded file or scoped turn count', () => {
  let state = {};
  state = openSessionChangesTab(state, {
    scopeKey: 'workspace-a',
    sessionId: 's1',
    expandedFile: 'src/a.cpp',
    fileCount: 1,
  });
  state = updateSessionChangesTab(state, { sessionId: 's1', fileCount: 4 });
  const tab = activePreviewTab(state, { scopeKey: 'workspace-a', sessionId: 's1' });
  assert.equal(tab.fileCount, 4);
  assert.equal(tab.expandedFile, 'src/a.cpp');

  state = openSessionChangesTab(state, {
    scopeKey: 'workspace-a',
    sessionId: 's1',
    expandedFile: 'src/turn.cpp',
    fileCount: 2,
    turnUserMessageId: 'user-turn-3',
  });
  state = updateSessionChangesTab(state, { sessionId: 's1', fileCount: 10 });
  const scopedTab = activePreviewTab(state, { scopeKey: 'workspace-a', sessionId: 's1' });
  assert.equal(scopedTab.fileCount, 2);
  assert.equal(scopedTab.turnUserMessageId, 'user-turn-3');
});

// ── git 级「变更」页签(git-changes 类型)──────────────────────────
// 场景:git 仓库会话点变更文件,应在中间详情栏开一个 git 变更页签,
// 携带 cwd/base/expandedFile,行为对齐 openSessionChangesTab。

run('openGitChangesTab 创建携带 cwd/base/expandedFile 的 git 变更页签', () => {
  let state = {};
  state = openGitChangesTab(state, {
    scopeKey: 'workspace-a',
    sessionId: 's1',
    cwd: 'C:/repo',
    base: 'origin/main',
    expandedFile: 'src/a.cpp',
    fileCount: 5,
  });
  const tab = activePreviewTab(state, { scopeKey: 'workspace-a', sessionId: 's1' });
  assert.equal(tab.type, 'git-changes');
  assert.equal(tab.key, 'git-changes:s1');
  assert.equal(tab.cwd, 'C:/repo');
  assert.equal(tab.base, 'origin/main');
  assert.equal(tab.expandedFile, 'src/a.cpp');
  assert.equal(tab.fileCount, 5);
  assert.equal(tab.expandedFileRevision, 1);
});

run('openGitChangesTab 重复点击自增 revision 触发详情栏滚动/展开', () => {
  let state = {};
  state = openGitChangesTab(state, {
    scopeKey: 'workspace-a', sessionId: 's1', cwd: 'C:/repo', base: 'HEAD', expandedFile: 'a.cpp',
  });
  const first = activePreviewTab(state, { scopeKey: 'workspace-a', sessionId: 's1' }).expandedFileRevision;
  state = openGitChangesTab(state, {
    scopeKey: 'workspace-a', sessionId: 's1', cwd: 'C:/repo', base: 'HEAD', expandedFile: 'a.cpp',
  });
  const tab = activePreviewTab(state, { scopeKey: 'workspace-a', sessionId: 's1' });
  assert.equal(tab.expandedFileRevision, first + 1);
});

// 场景:详情栏内点另一个文件时只带 expandedFile、不带 base(undefined),
// 必须保留页签原比较基线,否则详情栏会拿空 base 去拉 diff 而崩。
run('openGitChangesTab base 缺省时保留页签原有 base', () => {
  let state = {};
  state = openGitChangesTab(state, {
    scopeKey: 'workspace-a', sessionId: 's1', cwd: 'C:/repo', base: 'origin/main', expandedFile: 'a.cpp',
  });
  state = openGitChangesTab(state, {
    scopeKey: 'workspace-a', sessionId: 's1', expandedFile: 'b.cpp', // 不传 base/cwd
  });
  const tab = activePreviewTab(state, { scopeKey: 'workspace-a', sessionId: 's1' });
  assert.equal(tab.base, 'origin/main');
  assert.equal(tab.cwd, 'C:/repo');
  assert.equal(tab.expandedFile, 'b.cpp');
});

// 场景:SidePanel 切基线,已打开的 git 页签同步 base,但不改 expandedFile/revision。
run('updateGitChangesTab 只换 base 不动 expandedFile 与 revision', () => {
  let state = {};
  state = openGitChangesTab(state, {
    scopeKey: 'workspace-a', sessionId: 's1', cwd: 'C:/repo', base: 'HEAD', expandedFile: 'a.cpp',
  });
  const rev = activePreviewTab(state, { scopeKey: 'workspace-a', sessionId: 's1' }).expandedFileRevision;
  state = updateGitChangesTab(state, { sessionId: 's1', base: 'origin/dev' });
  const tab = activePreviewTab(state, { scopeKey: 'workspace-a', sessionId: 's1' });
  assert.equal(tab.base, 'origin/dev');
  assert.equal(tab.expandedFile, 'a.cpp');
  assert.equal(tab.expandedFileRevision, rev);
});

// 回归:session-changes 与 git-changes 共用 changeTabsBySession 槽位,
// 每回合触发的 updateSessionChangesTab(会话 hunks 计数)绝不能误改 git 页签,
// 否则 git 页签 fileCount 会被会话计数覆盖、类型语义被污染。
run('updateSessionChangesTab 不污染同槽位的 git 页签', () => {
  let state = {};
  state = openGitChangesTab(state, {
    scopeKey: 'workspace-a', sessionId: 's1', cwd: 'C:/repo', base: 'HEAD', expandedFile: 'a.cpp', fileCount: 7,
  });
  state = updateSessionChangesTab(state, { sessionId: 's1', fileCount: 99 });
  const tab = activePreviewTab(state, { scopeKey: 'workspace-a', sessionId: 's1' });
  assert.equal(tab.type, 'git-changes');
  assert.equal(tab.fileCount, 7);
});

run('closePreviewTab 关闭 git 变更页签后详情栏清空', () => {
  let state = {};
  state = openGitChangesTab(state, {
    scopeKey: 'workspace-a', sessionId: 's1', cwd: 'C:/repo', base: 'HEAD', expandedFile: 'a.cpp',
  });
  const key = activePreviewTab(state, { scopeKey: 'workspace-a', sessionId: 's1' }).key;
  state = closePreviewTab(state, { scopeKey: 'workspace-a', sessionId: 's1', tabKey: key });
  assert.equal(visiblePreviewTabs(state, { scopeKey: 'workspace-a', sessionId: 's1' }).length, 0);
  assert.equal(activePreviewTab(state, { scopeKey: 'workspace-a', sessionId: 's1' }), null);
});

run('activatePreviewTab 能在 file 页签之外重新激活 git 变更页签', () => {
  let state = {};
  state = openGitChangesTab(state, {
    scopeKey: 'workspace-a', sessionId: 's1', cwd: 'C:/repo', base: 'HEAD', expandedFile: 'a.cpp',
  });
  const gitKey = activePreviewTab(state, { scopeKey: 'workspace-a', sessionId: 's1' }).key;
  state = openFileTab(state, { scopeKey: 'workspace-a', sessionId: 's1', cwd: 'C:/repo', path: 'README.md' });
  assert.equal(activePreviewTab(state, { scopeKey: 'workspace-a', sessionId: 's1' }).type, 'file');
  state = activatePreviewTab(state, { scopeKey: 'workspace-a', sessionId: 's1', tabKey: gitKey });
  assert.equal(activePreviewTab(state, { scopeKey: 'workspace-a', sessionId: 's1' }).type, 'git-changes');
});

run('reorderPreviewTab reorders visible file tabs', () => {
  let state = {};
  state = openFileTab(state, { scopeKey: 'workspace-a', sessionId: 's1', cwd: 'C:/a', path: 'a.txt' });
  state = openFileTab(state, { scopeKey: 'workspace-a', sessionId: 's1', cwd: 'C:/a', path: 'b.txt' });
  state = openFileTab(state, { scopeKey: 'workspace-a', sessionId: 's1', cwd: 'C:/a', path: 'c.txt' });
  const tabs = visiblePreviewTabs(state, { scopeKey: 'workspace-a', sessionId: 's1' });

  state = reorderPreviewTab(state, {
    scopeKey: 'workspace-a',
    sessionId: 's1',
    sourceKey: tabs[2].key,
    targetKey: tabs[0].key,
    placement: 'before',
  });

  assert.deepEqual(
    visiblePreviewTabs(state, { scopeKey: 'workspace-a', sessionId: 's1' }).map((tab) => tab.path),
    ['c.txt', 'a.txt', 'b.txt'],
  );
});

run('reorderPreviewTab lets session-change tab participate in visible order', () => {
  let state = {};
  state = openFileTab(state, { scopeKey: 'workspace-a', sessionId: 's1', cwd: 'C:/a', path: 'a.txt' });
  state = openSessionChangesTab(state, {
    scopeKey: 'workspace-a',
    sessionId: 's1',
    expandedFile: 'a.txt',
    fileCount: 1,
  });
  const tabs = visiblePreviewTabs(state, { scopeKey: 'workspace-a', sessionId: 's1' });
  const fileTab = tabs.find((tab) => tab.type === 'file');
  const changeTab = tabs.find((tab) => tab.type === 'session-changes');

  state = reorderPreviewTab(state, {
    scopeKey: 'workspace-a',
    sessionId: 's1',
    sourceKey: changeTab.key,
    targetKey: fileTab.key,
    placement: 'before',
  });

  assert.deepEqual(
    visiblePreviewTabs(state, { scopeKey: 'workspace-a', sessionId: 's1' }).map((tab) => tab.type),
    ['session-changes', 'file'],
  );
  assert.equal(state.changeTabsBySession.s1.sessionId, 's1');
});

run('activatePreviewTab preserves reordered visible order', () => {
  let state = {};
  state = openFileTab(state, { scopeKey: 'workspace-a', sessionId: 's1', cwd: 'C:/a', path: 'a.txt' });
  state = openFileTab(state, { scopeKey: 'workspace-a', sessionId: 's1', cwd: 'C:/a', path: 'b.txt' });
  state = openFileTab(state, { scopeKey: 'workspace-a', sessionId: 's1', cwd: 'C:/a', path: 'c.txt' });
  const tabs = visiblePreviewTabs(state, { scopeKey: 'workspace-a', sessionId: 's1' });
  state = reorderPreviewTab(state, {
    scopeKey: 'workspace-a',
    sessionId: 's1',
    sourceKey: tabs[2].key,
    targetKey: tabs[0].key,
    placement: 'before',
  });
  const bKey = visiblePreviewTabs(state, { scopeKey: 'workspace-a', sessionId: 's1' })
    .find((tab) => tab.path === 'b.txt').key;
  state = activatePreviewTab(state, { scopeKey: 'workspace-a', sessionId: 's1', tabKey: bKey });

  assert.equal(activePreviewTab(state, { scopeKey: 'workspace-a', sessionId: 's1' }).path, 'b.txt');
  assert.deepEqual(
    visiblePreviewTabs(state, { scopeKey: 'workspace-a', sessionId: 's1' }).map((tab) => tab.path),
    ['c.txt', 'a.txt', 'b.txt'],
  );
});

run('closePreviewTab removes closed keys from stored visible order', () => {
  let state = {};
  state = openFileTab(state, { scopeKey: 'workspace-a', sessionId: 's1', cwd: 'C:/a', path: 'a.txt' });
  state = openFileTab(state, { scopeKey: 'workspace-a', sessionId: 's1', cwd: 'C:/a', path: 'b.txt' });
  state = openFileTab(state, { scopeKey: 'workspace-a', sessionId: 's1', cwd: 'C:/a', path: 'c.txt' });
  const tabs = visiblePreviewTabs(state, { scopeKey: 'workspace-a', sessionId: 's1' });
  state = reorderPreviewTab(state, {
    scopeKey: 'workspace-a',
    sessionId: 's1',
    sourceKey: tabs[2].key,
    targetKey: tabs[0].key,
    placement: 'before',
  });

  state = closePreviewTab(state, { scopeKey: 'workspace-a', sessionId: 's1', tabKey: tabs[2].key });

  assert.deepEqual(
    visiblePreviewTabs(state, { scopeKey: 'workspace-a', sessionId: 's1' }).map((tab) => tab.path),
    ['a.txt', 'b.txt'],
  );
  assert.equal(
    Object.values(state.tabOrderByView || {}).some((order) => order.includes(tabs[2].key)),
    false,
  );
});

run('closeVisiblePreviewTabs clears visible order keys', () => {
  let state = {};
  state = openFileTab(state, { scopeKey: 'workspace-a', sessionId: 's1', cwd: 'C:/a', path: 'a.txt' });
  state = openFileTab(state, { scopeKey: 'workspace-a', sessionId: 's1', cwd: 'C:/a', path: 'b.txt' });
  const tabs = visiblePreviewTabs(state, { scopeKey: 'workspace-a', sessionId: 's1' });
  state = reorderPreviewTab(state, {
    scopeKey: 'workspace-a',
    sessionId: 's1',
    sourceKey: tabs[1].key,
    targetKey: tabs[0].key,
    placement: 'before',
  });

  state = closeVisiblePreviewTabs(state, { scopeKey: 'workspace-a', sessionId: 's1' });

  assert.equal(visiblePreviewTabs(state, { scopeKey: 'workspace-a', sessionId: 's1' }).length, 0);
  assert.deepEqual(state.tabOrderByView, {});
});
