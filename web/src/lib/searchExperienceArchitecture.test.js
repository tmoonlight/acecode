import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const srcRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

function source(relativePath) {
  return fs.readFileSync(path.join(srcRoot, relativePath), 'utf8');
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

run('conversation menu owns find and retains the transcript search root', () => {
  const chatView = source('components/ChatView.jsx');
  assert.match(chatView, /onSelect: onFindInConversation/);
  assert.match(chatView, /id: 'find_conversation', label: '查找', icon: 'search'/);
  assert.match(chatView, /data-conversation-find-root="true"/);
});

run('global find is controlled and searches only the conversation root', () => {
  const overlay = source('components/GlobalFindOverlay.jsx');
  assert.match(overlay, /enabled = false/);
  assert.match(overlay, /openRequest = 0/);
  assert.match(overlay, /collectFindMatches\(resolveFindRoot\(\), query\)/);
  assert.doesNotMatch(overlay, /collectFindMatches\(document\.body/);
  assert.match(overlay, /placeholder="搜索当前对话内容"/);
});

run('global find closes only when a pointer starts outside the overlay', () => {
  const overlay = source('components/GlobalFindOverlay.jsx');
  assert.match(
    overlay,
    /const onPointerDown = \(event\) => \{\s*if \(overlayRef\.current\?\.contains\(event\.target\)\) return;\s*close\(\);\s*\}/,
  );
  assert.match(overlay, /document\.addEventListener\('pointerdown', onPointerDown, true\)/);
  assert.match(overlay, /document\.removeEventListener\('pointerdown', onPointerDown, true\)/);
});

run('search palette renders task, project and settings groups in one result sequence', () => {
  const palette = source('components/SearchPalette.jsx');
  assert.match(palette, /buildSearchResultSequence\(taskItems, projectItems, settingItems\)/);
  assert.match(palette, /<span>任务<\/span>/);
  assert.match(palette, /<span>项目<\/span>/);
  assert.match(palette, /<span>设置<\/span>/);
  assert.match(palette, /placeholder="搜索任务、项目或设置"/);
  assert.match(palette, /onSelectWorkspace\?\.\(item\.value\)/);
  assert.match(palette, /onSelectSetting\?\.\(item\.value, query\.trim\(\)\)/);
  assert.match(palette, /window\.addEventListener\(SESSION_LIST_CHANGED_EVENT/);
});

run('settings hits in the palette reuse the settings window index and never call the daemon', () => {
  // 设置项是本地固定清单:面板只能复用 settingsSearchEntries / rankSettingsForPalette,
  // 不允许另起一份设置索引或为它发 REST 请求;设置组下标必须接在任务 + 项目之后。
  const palette = source('components/SearchPalette.jsx');
  assert.match(palette, /settingsSearchEntries\(loadDeveloperModeUnlocked\(\)\)/);
  assert.match(palette, /rankSettingsForPalette\(settingsEntries, query\)/);
  assert.match(palette, /const index = taskItems\.length \+ projectItems\.length \+ settingIndex;/);
  assert.doesNotMatch(palette, /api\.\w*[sS]ettings?\w*\(/);
});

run('the search palette shortcut has a single definition shared by every surface', () => {
  // Ctrl+K 的判定统一在 lib/appShortcuts.js:App 的全局监听、TopBar 提示、
  // 快捷菜单 kbd、控制台(xterm)放行都引用它,不能各自再写一遍 e.key === 'k'。
  const app = source('App.jsx');
  const topBar = source('components/TopBar.jsx');
  const consoleDock = source('components/ConsoleDock.jsx');
  const quickActions = source('lib/topBarQuickActions.js');
  const quickMenu = source('components/SidebarQuickMenu.jsx');
  assert.match(app, /useAppShortcuts\(/);
  assert.match(source('lib/searchPaletteShortcut.js'), /matchAppShortcut\(event\) === 'search'/);
  assert.doesNotMatch(app, /toLowerCase\(\) === 'k'/);
  assert.match(topBar, /withSearchPaletteShortcutHint\('搜索任务'\)/);
  assert.match(consoleDock, /matchAppShortcut\(ev\)/);
  assert.match(quickActions, /searchPaletteShortcutLabel\(win\)/);
  assert.match(quickMenu, /topBarQuickActionShortcutLabel\(action\)/);
});

run('a settings hit opens the settings window on the same query and result', () => {
  // App 把面板的原始查询 + 结果 id 作为种子传给 SettingsPage;SettingsPage 用同一份索引重跑,
  // 按 id 对齐选中项,而且首轮防抖不得把种下的选中项清回 0。
  const app = source('App.jsx');
  const settings = source('components/SettingsPage.jsx');
  assert.match(app, /onSelectSetting=\{handleSelectSetting\}/);
  assert.match(app, /initialSearch=\{settingsSearchSeed\}/);
  assert.match(app, /setSettingsSearchSeed\(null\);/);
  assert.match(settings, /settingsSearchResultIndex\(searchSettings\(searchEntries, query\), initialSearch\)/);
  assert.match(settings, /if \(appliedSearchTermRef\.current !== searchQuery\) \{\s*appliedSearchTermRef\.current = searchQuery;\s*setSearchIndex\(0\);/);
  assert.doesNotMatch(settings, /setSearchTerm\(searchQuery\); setSearchIndex\(0\);/);
});

run('every search palette exit converges on local abort and server cancellation', () => {
  const palette = source('components/SearchPalette.jsx');
  assert.match(palette, /search\.controller\?\.abort\(\)/);
  assert.match(palette, /api\.cancelSessionSearch\(search\.requestId\)/);
  assert.match(palette, /return \(\) => cancelSearch\(search\)/);
  assert.match(palette, /event\.key === 'Escape'[\s\S]*closePalette\(\)/);
  assert.match(palette, /onClick=\{closePalette\}/);
  assert.match(palette, /event\.target === event\.currentTarget\) closePalette\(\)/);
  assert.match(palette, /\[open, query, searchRevision, cancelSearch\]/);
  assert.doesNotMatch(palette, /loadState === 'loading'/);
});

run('search progress is a one-pixel accent edge with no status-row copy', () => {
  const palette = source('components/SearchPalette.jsx');
  assert.match(palette, /const progressPercent = searchProgressPercent\(/);
  assert.match(palette, /aria-busy=\{progressPercent !== null\}/);
  assert.match(
    palette,
    /\{progressPercent !== null && \(\s*<div\s+aria-hidden="true"\s+className="absolute inset-x-0 bottom-0 h-px pointer-events-none"/,
  );
  assert.match(palette, /className="h-full bg-accent transition-\[width\] duration-150 ease-out motion-reduce:transition-none"/);
  assert.doesNotMatch(palette, /progressText|正在建立任务索引|正在增量搜索正文|可随时关闭/);
});

run('polling refresh preserves manual result scrolling', () => {
  const palette = source('components/SearchPalette.jsx');
  assert.match(
    palette,
    /useEffect\(\(\) => \{\s*setSelectedIndex\(0\);\s*if \(listRef\.current\) listRef\.current\.scrollTop = 0;\s*\}, \[query\]\);/,
  );
  assert.match(
    palette,
    /setSelectedIndex\(\(previous\) => Math\.min\(previous, Math\.max\(0, items\.length - 1\)\)\)/,
  );
  assert.match(
    palette,
    /const revealKeyboardSelection = useCallback\(\(index\) => \{[\s\S]*?scrollIntoView\(\{ block: 'nearest' \}\);[\s\S]*?\}, \[\]\);/,
  );
  assert.equal((palette.match(/scrollIntoView/g) || []).length, 1);
  assert.doesNotMatch(palette, /\[query, data\.sessions, contentSearch\]/);
  assert.doesNotMatch(palette, /\[selectedIndex, items\.length\]/);
});

run('global session search is independent from visible workspace discovery', () => {
  const api = source('lib/api.js');
  const app = source('App.jsx');
  const catalogCall = api.slice(
    api.indexOf('listAllWorkspaceSessions:'),
    api.indexOf('listAllArchivedSessions:'),
  );

  assert.match(catalogCall, /mergeGlobalSessionsAndWorkspaces/);
  assert.match(catalogCall, /sessionCatalogSearchPath/);
  assert.match(api, /`\/api\/session-search\/sessions\?\$\{queryString\}`/);
  assert.match(catalogCall, /\/api\/workspaces/);
  assert.doesNotMatch(catalogCall, /\/api\/workspaces\/\$\{[^}]+\}\/sessions/);
  assert.match(
    app,
    /!noWorkspace[\s\S]*targetHash[\s\S]*sessionJumpWorkspaceVisible\(target\)[\s\S]*aceDesktop_activateWorkspace/,
  );
});

run('project selection is handed from App to Sidebar activation', () => {
  const app = source('App.jsx');
  const sidebar = source('components/Sidebar.jsx');
  assert.match(app, /workspaceActivationRequest=\{workspaceActivationRequest\}/);
  assert.match(app, /onSelectWorkspace=\{handleSelectWorkspace\}/);
  assert.match(sidebar, /handledWorkspaceActivationRequestRef/);
  assert.match(sidebar, /Promise\.resolve\(onActivate\(workspace\)\)/);
  assert.match(sidebar, /SIDEBAR_SECTION_IDS\.WORKSPACES/);
});
