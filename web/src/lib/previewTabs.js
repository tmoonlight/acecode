import { normalizeTreePath } from './fileTreeChangeStatus.js';

export const PREVIEW_TAB_TYPES = Object.freeze({
  FILE: 'file',
  SESSION_CHANGES: 'session-changes',
  GIT_CHANGES: 'git-changes',
  BROWSER: 'browser',
  SIMULATOR: 'ios-simulator',
});

export function defaultBrowserTabTitle() {
  return '新标签页';
}

export function normalizeBrowserTabTitle(title) {
  const value = String(title ?? '');
  return value.trim() ? value : defaultBrowserTabTitle();
}

export function defaultSimulatorTabTitle() {
  return 'iOS 模拟器';
}

export function normalizeBrowserTabFavicon(favicon) {
  const value = String(favicon ?? '').trim();
  if (!value || value.length > 256 * 1024) return '';
  return /^(?:https?:\/\/|data:image\/)/i.test(value) ? value : '';
}

function normalizePreviewCwd(cwd = '') {
  let normalized = String(cwd || '').replace(/\\/g, '/');
  if (/^\/\/\?\/UNC\//i.test(normalized)) {
    normalized = normalized.replace(/^\/\/\?\/UNC\//i, '//');
  } else {
    normalized = normalized.replace(/^\/\/\?\//, '');
  }
  const isUnc = normalized.startsWith('//');
  normalized = normalized.replace(/\/+/g, '/');
  if (isUnc) normalized = `/${normalized}`;
  if (normalized === '/' || /^[A-Za-z]:\/$/.test(normalized)) return normalized;
  return normalized.replace(/\/+$/g, '');
}

function isAbsolutePreviewPath(path = '') {
  const normalized = normalizePreviewCwd(path);
  return normalized.startsWith('/') || /^[A-Za-z]:\//.test(normalized);
}

function relativePathWithin(absolutePath, cwd) {
  const target = normalizePreviewCwd(absolutePath);
  const root = normalizePreviewCwd(cwd);
  if (!target || !root) return null;
  const caseInsensitive = (
    (/^[A-Za-z]:\//.test(target) && /^[A-Za-z]:\//.test(root))
    || (target.startsWith('//') && root.startsWith('//'))
  );
  const targetKey = caseInsensitive ? target.toLowerCase() : target;
  const rootKey = caseInsensitive ? root.toLowerCase() : root;
  if (targetKey === rootKey) return '';
  const prefix = rootKey.endsWith('/') ? rootKey : `${rootKey}/`;
  if (!targetKey.startsWith(prefix)) return null;
  return target.slice(prefix.length);
}

export function sessionWorkingCwd({ worktree = null, cwd = '', fallbackCwd = '' } = {}) {
  return normalizePreviewCwd(worktree?.path || cwd || fallbackCwd || '');
}

export function previewScopeKey({ cwd = '', workspaceHash = '', worktreePath = '' } = {}) {
  const normalizedWorktree = normalizePreviewCwd(worktreePath);
  if (normalizedWorktree) return `worktree:${normalizedWorktree}`;
  return workspaceHash || normalizePreviewCwd(cwd) || '';
}

// sessionId is also the local tab-owner key. ChatView passes its unique draft
// owner before creation; the scope fallback supports standalone preview callers.
export function previewTabContext({ scopeKey = '', sessionId = '' } = {}) {
  return { scopeKey, sessionId: sessionId || (scopeKey ? `@workspace:${scopeKey}` : '') };
}

export function previewFileLocation({ cwd = '', path = '' } = {}) {
  const normalizedCwd = normalizePreviewCwd(cwd);
  if (isAbsolutePreviewPath(path)) {
    const absolutePath = normalizePreviewCwd(path);
    const relativePath = relativePathWithin(absolutePath, normalizedCwd);
    if (relativePath != null) {
      return { cwd: normalizedCwd, path: normalizeTreePath(relativePath) };
    }

    const slash = absolutePath.lastIndexOf('/');
    if (slash >= 0 && slash < absolutePath.length - 1) {
      const parent = slash === 0
        ? '/'
        : (/^[A-Za-z]:\//.test(absolutePath) && slash === 2
          ? absolutePath.slice(0, 3)
          : absolutePath.slice(0, slash));
      return { cwd: parent, path: absolutePath.slice(slash + 1) };
    }
  }

  const normalizedPath = normalizeTreePath(path);
  if (!normalizedPath) return { cwd: normalizedCwd, path: '' };
  if (normalizedCwd) return { cwd: normalizedCwd, path: normalizedPath };
  return { cwd: normalizedCwd, path: normalizedPath };
}

export function previewAbsolutePath({ cwd = '', path = '' } = {}) {
  if (isAbsolutePreviewPath(path)) return normalizePreviewCwd(path);
  const normalizedCwd = normalizePreviewCwd(cwd);
  const normalizedPath = normalizeTreePath(path);
  if (!normalizedCwd) return normalizedPath;
  if (!normalizedPath) return normalizedCwd;
  return normalizedCwd.endsWith('/')
    ? `${normalizedCwd}${normalizedPath}`
    : `${normalizedCwd}/${normalizedPath}`;
}

export function visiblePreviewTabs(state, { scopeKey = '', sessionId = '' } = {}) {
  const source = state && typeof state === 'object' ? state : {};
  const fileTabs = source.fileTabsByView?.[viewKey(scopeKey, sessionId)] || [];
  const changeTab = sessionId ? source.changeTabsBySession?.[sessionId] : null;
  const storedBrowserTabs = sessionId ? source.browserTabsBySession?.[sessionId] : null;
  const browserTabs = Array.isArray(storedBrowserTabs)
    ? storedBrowserTabs
    : (storedBrowserTabs ? [storedBrowserTabs] : []);
  const simulatorTabs = sessionId ? simulatorTabsForSession(source, sessionId) : [];
  const naturalTabs = [
    ...fileTabs,
    ...(changeTab ? [changeTab] : []),
    ...browserTabs,
    ...simulatorTabs,
  ];
  return applyVisibleTabOrder(naturalTabs, source.tabOrderByView?.[viewKey(scopeKey, sessionId)]);
}

function viewKey(scopeKey, sessionId) {
  return JSON.stringify([scopeKey || '', sessionId || '']);
}

function applyVisibleTabOrder(tabs, order) {
  if (!Array.isArray(order) || order.length === 0 || tabs.length <= 1) return tabs.slice();
  const tabsByKey = new Map(tabs.map((tab) => [tab.key, tab]));
  const used = new Set();
  const ordered = [];
  order.forEach((key) => {
    if (used.has(key) || !tabsByKey.has(key)) return;
    used.add(key);
    ordered.push(tabsByKey.get(key));
  });
  tabs.forEach((tab) => {
    if (used.has(tab.key)) return;
    used.add(tab.key);
    ordered.push(tab);
  });
  return ordered;
}

function removeKeysFromOrders(orderMap, keysToRemove) {
  const keys = new Set(keysToRemove.filter(Boolean));
  if (!orderMap || keys.size === 0) return orderMap || {};
  const next = {};
  Object.entries(orderMap).forEach(([key, order]) => {
    if (!Array.isArray(order)) return;
    const filtered = order.filter((tabKey) => !keys.has(tabKey));
    if (filtered.length > 0) next[key] = filtered;
  });
  return next;
}

function activeKeyFor(state, scopeKey, sessionId) {
  const source = state && typeof state === 'object' ? state : {};
  const activeByView = source.activeTabByView?.[viewKey(scopeKey, sessionId)];
  if (activeByView) return activeByView;
  if (sessionId && source.activeTabBySession?.[sessionId]) return source.activeTabBySession[sessionId];
  return '';
}

export function activePreviewTab(state, context = {}) {
  const tabs = visiblePreviewTabs(state, context);
  const activeKey = activeKeyFor(state, context.scopeKey || '', context.sessionId || '');
  return tabs.find((tab) => tab.key === activeKey) || tabs[0] || null;
}

// 新建入口只移交给刚创建的会话，键也一起重建；已有会话不会隐式读工作区页签。
export function transferPreviewTabs(state, from, to) {
  const tabs = visiblePreviewTabs(state, from);
  const active = activePreviewTab(state, from)?.key;
  const keys = new Map();
  let next = state;
  for (const tab of tabs) {
    if (tab.type === PREVIEW_TAB_TYPES.FILE) {
      next = openFileTab(next, { ...to, cwd: tab.cwd, path: tab.path });
      const key = fileTabKey(to.scopeKey, to.sessionId, tab.path);
      keys.set(tab.key, key);
      const owner = viewKey(to.scopeKey, to.sessionId);
      next.fileTabsByView[owner] = next.fileTabsByView[owner].map((created) => created.key === key
        ? { ...tab, key, scopeKey: to.scopeKey, sessionId: to.sessionId } : created);
    } else if (tab.type === PREVIEW_TAB_TYPES.GIT_CHANGES) {
      next = openGitChangesTab(next, { ...tab, ...to });
      keys.set(tab.key, gitChangesTabKey(to.sessionId));
    }
  }
  next = closeVisiblePreviewTabs(next, from);
  const target = viewKey(to.scopeKey, to.sessionId);
  return {
    ...next,
    tabOrderByView: { ...next.tabOrderByView, [target]: tabs.map((tab) => keys.get(tab.key)).filter(Boolean) },
    activeTabByView: { ...next.activeTabByView, [target]: keys.get(active) || '' },
  };
}

export function previewTabHasUnsavedDraft(tab) {
  if (tab?.type !== PREVIEW_TAB_TYPES.FILE || !tab.edit) return false;
  const baseline = String(tab.edit.baselineText ?? '');
  const text = String(tab.edit.text ?? baseline);
  return text !== baseline;
}

export function previewTabsWithUnsavedDrafts(tabs) {
  return (Array.isArray(tabs) ? tabs : []).filter(previewTabHasUnsavedDraft);
}

export function visibleUnsavedPreviewTabs(state, context = {}) {
  return previewTabsWithUnsavedDrafts(visiblePreviewTabs(state, context));
}

function fileTabKey(scopeKey, sessionId, path) {
  return `file:${viewKey(scopeKey, sessionId)}:${normalizeTreePath(path)}`;
}

function sessionChangesTabKey(sessionId) {
  return `session-changes:${sessionId}`;
}

function gitChangesTabKey(sessionId) {
  return `git-changes:${sessionId}`;
}

function browserTabKey(pageId) {
  return `browser:${pageId}`;
}

function isBrowserTabKey(tabKey) {
  return typeof tabKey === 'string' && tabKey.startsWith('browser:');
}

// iOS 模拟器页签按 udid 区分;udid 为空的是手动打开、尚未选定设备的选择页。
function simulatorTabKey(udid) {
  return `ios-simulator:${udid || ''}`;
}

function isSimulatorTabKey(tabKey) {
  return typeof tabKey === 'string' && tabKey.startsWith('ios-simulator:');
}

// 会话级页签(变更 / 浏览器 / 模拟器)的激活态同时记在 activeTabBySession。
function isSessionScopedTabKey(tabKey) {
  return isChangeTabKey(tabKey) || isBrowserTabKey(tabKey) || isSimulatorTabKey(tabKey);
}

// session-changes 与 git-changes 是同一个「变更」概念的两种数据源(非 git
// 仓库走会话内 hunks 聚合,git 仓库走 numstat/diff),同一 session 二选一,
// 因此共用 `changeTabsBySession` 这一个槽位。close / activate 只需按 tab.key
// 的前缀区分,存储/查询逻辑不必翻倍。
function isChangeTabKey(tabKey) {
  return typeof tabKey === 'string'
    && (tabKey.startsWith('session-changes:') || tabKey.startsWith('git-changes:'));
}

function nextActiveAfterClose(tabs, closedKey, activeKey) {
  if (activeKey !== closedKey && tabs.some((tab) => tab.key === activeKey)) return activeKey;
  const index = tabs.findIndex((tab) => tab.key === closedKey);
  if (index < 0) return tabs[0]?.key || '';
  return tabs[index + 1]?.key || tabs[index - 1]?.key || '';
}

export function openFileTab(state, { scopeKey = '', sessionId = '', cwd = '', path = '', line = null } = {}) {
  const normalizedPath = normalizeTreePath(path);
  if (!scopeKey || !normalizedPath) return state || {};
  const source = state && typeof state === 'object' ? state : {};
  const tabs = source.fileTabsByView?.[viewKey(scopeKey, sessionId)] || [];
  const key = fileTabKey(scopeKey, sessionId, normalizedPath);
  // 聊天正文的 foo.cpp:42 链接带行号进来:tab 记录 line + lineRevision(仿
  // expandedFileRevision 模式)。revision 每次带行号打开都递增,让重复点击同一
  // 链接也能触发预览重新滚动。不带行号的打开(文件树点击)不清已有定位。
  const focusLine = Number.isFinite(line) && line > 0 ? Math.floor(line) : null;
  const existing = tabs.find((tab) => tab.key === key);
  let nextTabs;
  if (existing) {
    nextTabs = focusLine == null ? tabs : tabs.map((tab) => (tab.key === key
      ? { ...tab, line: focusLine, lineRevision: (tab.lineRevision || 0) + 1 }
      : tab));
  } else {
    nextTabs = [...tabs, {
      key,
      type: PREVIEW_TAB_TYPES.FILE,
      scopeKey,
      sessionId,
      cwd,
      path: normalizedPath,
      title: normalizedPath.split('/').pop() || normalizedPath,
      ...(focusLine != null ? { line: focusLine, lineRevision: 1 } : {}),
    }];
  }
  return {
    ...source,
    fileTabsByView: {
      ...(source.fileTabsByView || {}),
      [viewKey(scopeKey, sessionId)]: nextTabs,
    },
    activeTabByView: {
      ...(source.activeTabByView || {}),
      [viewKey(scopeKey, sessionId)]: key,
    },
  };
}

export function updateFileTabDraft(state, {
  scopeKey = '',
  sessionId = '',
  tabKey = '',
  patch = {},
} = {}) {
  if (!scopeKey || !tabKey) return state || {};
  const source = state && typeof state === 'object' ? state : {};
  const tabs = source.fileTabsByView?.[viewKey(scopeKey, sessionId)] || [];
  const index = tabs.findIndex((tab) => tab.key === tabKey && tab.type === PREVIEW_TAB_TYPES.FILE);
  if (index < 0) return source;

  const current = tabs[index].edit && typeof tabs[index].edit === 'object'
    ? tabs[index].edit
    : {};
  const nextEdit = {
    ...current,
    ...(patch && typeof patch === 'object' ? patch : {}),
  };
  const baselineText = String(nextEdit.baselineText ?? '');
  const text = String(nextEdit.text ?? baselineText);
  nextEdit.baselineText = baselineText;
  nextEdit.text = text;
  nextEdit.editing = nextEdit.editing === true;
  nextEdit.saving = nextEdit.saving === true;
  nextEdit.externalChanged = nextEdit.externalChanged === true;
  nextEdit.dirty = text !== baselineText;

  const nextTabs = tabs.slice();
  nextTabs[index] = { ...tabs[index], edit: nextEdit };
  return {
    ...source,
    fileTabsByView: {
      ...(source.fileTabsByView || {}),
      [viewKey(scopeKey, sessionId)]: nextTabs,
    },
  };
}

export function discardFileTabDraft(state, { scopeKey = '', sessionId = '', tabKey = '' } = {}) {
  if (!scopeKey || !tabKey) return state || {};
  const source = state && typeof state === 'object' ? state : {};
  const tabs = source.fileTabsByView?.[viewKey(scopeKey, sessionId)] || [];
  if (!tabs.some((tab) => tab.key === tabKey && tab.edit)) return source;
  return {
    ...source,
    fileTabsByView: {
      ...(source.fileTabsByView || {}),
      [viewKey(scopeKey, sessionId)]: tabs.map((tab) => {
        if (tab.key !== tabKey || !tab.edit) return tab;
        const baselineText = String(tab.edit.baselineText ?? '');
        return {
          ...tab,
          edit: {
            ...tab.edit,
            text: baselineText,
            editing: true,
            saving: false,
            dirty: false,
            externalChanged: false,
            error: '',
          },
        };
      }),
    },
  };
}

export function openSessionChangesTab(state, {
  scopeKey = '',
  sessionId = '',
  expandedFile = '',
  fileCount = 0,
  turnUserMessageId = '',
} = {}) {
  if (!sessionId) return state || {};
  const source = state && typeof state === 'object' ? state : {};
  const key = sessionChangesTabKey(sessionId);
  const previousTab = source.changeTabsBySession?.[sessionId];
  const tab = {
    key,
    type: PREVIEW_TAB_TYPES.SESSION_CHANGES,
    sessionId,
    turnUserMessageId: String(turnUserMessageId || ''),
    expandedFile: normalizeTreePath(expandedFile),
    expandedFileRevision: (Number(previousTab?.expandedFileRevision) || 0) + 1,
    reloadRevision: Number(previousTab?.reloadRevision) || 0,
    fileCount: Number.isFinite(Number(fileCount)) ? Number(fileCount) : 0,
  };
  return {
    ...source,
    changeTabsBySession: {
      ...(source.changeTabsBySession || {}),
      [sessionId]: tab,
    },
    activeTabBySession: {
      ...(source.activeTabBySession || {}),
      [sessionId]: key,
    },
    activeTabByView: {
      ...(source.activeTabByView || {}),
      [viewKey(scopeKey, sessionId)]: key,
    },
  };
}

const EMPTY_CHANGE_SUMMARY = Object.freeze({
  fileCount: 0,
  totalAdditions: 0,
  totalDeletions: 0,
  hasChanges: false,
});

export function resolveSessionChangesTabContent(activeTab, {
  changeGroups = [],
  changeSummary = null,
  turnChangeSets = [],
} = {}) {
  const turnUserMessageId = String(activeTab?.turnUserMessageId || '');
  if (!turnUserMessageId) {
    return {
      groups: Array.isArray(changeGroups) ? changeGroups : [],
      summary: changeSummary,
      turnUserMessageId: '',
    };
  }

  const turn = Array.isArray(turnChangeSets)
    ? turnChangeSets.find((set) => set?.userMessageId === turnUserMessageId)
    : null;
  if (!turn) {
    return {
      groups: [],
      summary: EMPTY_CHANGE_SUMMARY,
      turnUserMessageId,
    };
  }
  return {
    groups: Array.isArray(turn.groups) ? turn.groups : [],
    summary: turn.summary && typeof turn.summary === 'object'
      ? turn.summary
      : EMPTY_CHANGE_SUMMARY,
    turnUserMessageId,
  };
}

export function openBrowserTab(state, {
  scopeKey = '',
  sessionId = '',
  pageId = '',
  title,
  favicon,
} = {}) {
  if (!sessionId || !pageId) return state || {};
  const source = state && typeof state === 'object' ? state : {};
  const key = browserTabKey(pageId);
  const stored = source.browserTabsBySession?.[sessionId];
  const existingTabs = Array.isArray(stored) ? stored : (stored ? [stored] : []);
  const existing = existingTabs.find((tab) => tab.pageId === pageId || tab.key === key);
  const normalizedTitle = normalizeBrowserTabTitle(title);
  const normalizedFavicon = normalizeBrowserTabFavicon(favicon);
  const nextTabs = existing
    ? (title === undefined && favicon === undefined
      ? existingTabs
      : existingTabs.map((tab) => {
        if (tab !== existing) return tab;
        const nextTitle = title === undefined ? tab.title : normalizedTitle;
        const nextFavicon = favicon === undefined ? (tab.favicon || '') : normalizedFavicon;
        return tab.title === nextTitle && (tab.favicon || '') === nextFavicon
          ? tab
          : { ...tab, title: nextTitle, favicon: nextFavicon };
      }))
    : [...existingTabs, {
      key,
      type: PREVIEW_TAB_TYPES.BROWSER,
      sessionId,
      pageId,
      title: normalizedTitle,
      favicon: normalizedFavicon,
    }];
  return {
    ...source,
    browserTabsBySession: {
      ...(source.browserTabsBySession || {}),
      [sessionId]: nextTabs,
    },
    activeTabBySession: {
      ...(source.activeTabBySession || {}),
      [sessionId]: key,
    },
    activeTabByView: {
      ...(source.activeTabByView || {}),
      [viewKey(scopeKey, sessionId)]: key,
    },
  };
}

export function updateBrowserTabMetadata(state, options = {}) {
  const { sessionId = '', pageId = '', title, favicon } = options;
  if (!sessionId || !pageId) return state || {};
  const source = state && typeof state === 'object' ? state : {};
  const stored = source.browserTabsBySession?.[sessionId];
  const tabs = Array.isArray(stored) ? stored : (stored ? [stored] : []);
  const key = browserTabKey(pageId);
  const hasTitle = Object.prototype.hasOwnProperty.call(options, 'title');
  const hasFavicon = Object.prototype.hasOwnProperty.call(options, 'favicon');
  if (!hasTitle && !hasFavicon) return source;
  const normalizedTitle = hasTitle ? normalizeBrowserTabTitle(title) : '';
  const normalizedFavicon = hasFavicon ? normalizeBrowserTabFavicon(favicon) : '';
  let changed = false;
  const nextTabs = tabs.map((tab) => {
    if (tab.pageId !== pageId && tab.key !== key) return tab;
    const nextTitle = hasTitle ? normalizedTitle : tab.title;
    const nextFavicon = hasFavicon ? normalizedFavicon : (tab.favicon || '');
    if (tab.title === nextTitle && (tab.favicon || '') === nextFavicon) return tab;
    changed = true;
    return { ...tab, title: nextTitle, favicon: nextFavicon };
  });
  if (!changed) return source;
  return {
    ...source,
    browserTabsBySession: {
      ...(source.browserTabsBySession || {}),
      [sessionId]: nextTabs,
    },
  };
}

// 把登记表里某会话的页面集合同步成该会话的浏览器页签:补缺、去已关闭、同步
// 标题与图标。不改动激活页签与顺序(激活由调用方决定);已不存在的页签经
// closePreviewTab 关闭,激活回退语义与手工关闭一致。输入未变化时返回原状态。
export function syncBrowserTabsForSession(state, { scopeKey = '', sessionId = '', pages = [] } = {}) {
  if (!sessionId) return state || {};
  const source = state && typeof state === 'object' ? state : {};
  const wanted = new Map();
  for (const page of Array.isArray(pages) ? pages : []) {
    const pageId = String(page?.pageId || page?.page_id || '');
    if (pageId && !wanted.has(pageId)) wanted.set(pageId, page);
  }
  const stored = source.browserTabsBySession?.[sessionId];
  const existingTabs = Array.isArray(stored) ? stored : (stored ? [stored] : []);
  let next = source;
  for (const tab of existingTabs) {
    if (wanted.has(tab.pageId)) continue;
    next = closePreviewTab(next, { scopeKey, sessionId, tabKey: tab.key });
  }
  for (const [pageId, page] of wanted) {
    const current = next.browserTabsBySession?.[sessionId];
    const tabs = Array.isArray(current) ? current : (current ? [current] : []);
    const existing = tabs.find((tab) => tab.pageId === pageId);
    if (existing) {
      next = updateBrowserTabMetadata(next, {
        sessionId,
        pageId,
        ...(page.title !== undefined ? { title: page.title } : {}),
        ...(page.favicon !== undefined ? { favicon: page.favicon } : {}),
      });
      continue;
    }
    next = {
      ...next,
      browserTabsBySession: {
        ...(next.browserTabsBySession || {}),
        [sessionId]: [...tabs, {
          key: browserTabKey(pageId),
          type: PREVIEW_TAB_TYPES.BROWSER,
          sessionId,
          pageId,
          title: normalizeBrowserTabTitle(page.title),
          favicon: normalizeBrowserTabFavicon(page.favicon),
        }],
      },
    };
  }
  return next;
}

export function updateBrowserTabTitle(state, options = {}) {
  return updateBrowserTabMetadata(state, {
    ...options,
    title: options.title,
  });
}

export function updateBrowserTabFavicon(state, options = {}) {
  return updateBrowserTabMetadata(state, {
    ...options,
    favicon: options.favicon,
  });
}

export function simulatorTabsForSession(state, sessionId = '') {
  const stored = sessionId ? state?.simulatorTabsBySession?.[sessionId] : null;
  return Array.isArray(stored) ? stored : [];
}

function withSimulatorTabs(source, sessionId, tabs) {
  const next = { ...(source.simulatorTabsBySession || {}) };
  if (tabs.length > 0) next[sessionId] = tabs;
  else delete next[sessionId];
  return next;
}

function withSessionActiveTab(source, scopeKey, sessionId, key) {
  return {
    activeTabBySession: {
      ...(source.activeTabBySession || {}),
      [sessionId]: key,
    },
    activeTabByView: {
      ...(source.activeTabByView || {}),
      [viewKey(scopeKey, sessionId)]: key,
    },
  };
}

// 打开(或聚焦)某台模拟器的页签。focus=false 只在缺失时补开,不抢当前激活页签。
export function openSimulatorTab(state, {
  scopeKey = '',
  sessionId = '',
  udid = '',
  title,
  focus = true,
} = {}) {
  if (!sessionId) return state || {};
  const source = state && typeof state === 'object' ? state : {};
  const key = simulatorTabKey(udid);
  const tabs = simulatorTabsForSession(source, sessionId);
  const existing = tabs.find((tab) => tab.key === key);
  const nextTitle = String(title || '').trim();
  let nextTabs = tabs;
  if (!existing) {
    nextTabs = [...tabs, {
      key,
      type: PREVIEW_TAB_TYPES.SIMULATOR,
      sessionId,
      udid: String(udid || ''),
      title: nextTitle || defaultSimulatorTabTitle(),
    }];
  } else if (nextTitle && existing.title !== nextTitle) {
    nextTabs = tabs.map((tab) => (tab === existing ? { ...tab, title: nextTitle } : tab));
  }
  if (nextTabs === tabs && !focus) return source;
  return {
    ...source,
    simulatorTabsBySession: withSimulatorTabs(source, sessionId, nextTabs),
    ...(focus ? withSessionActiveTab(source, scopeKey, sessionId, key) : {}),
  };
}

// 把一个模拟器页签换成另一台设备(选择页选定设备、工具栏切换设备)。位置与激活态
// 跟着走;目标设备已有页签时关掉当前这个并聚焦已有的。
export function retargetSimulatorTab(state, {
  scopeKey = '',
  sessionId = '',
  tabKey = '',
  udid = '',
  title,
} = {}) {
  if (!sessionId || !tabKey || !udid) return state || {};
  const source = state && typeof state === 'object' ? state : {};
  const tabs = simulatorTabsForSession(source, sessionId);
  const index = tabs.findIndex((tab) => tab.key === tabKey);
  if (index < 0) return source;
  const nextKey = simulatorTabKey(udid);
  if (nextKey === tabKey) return openSimulatorTab(source, { scopeKey, sessionId, udid, title });
  if (tabs.some((tab) => tab.key === nextKey)) {
    const closed = closePreviewTab(source, { scopeKey, sessionId, tabKey });
    return openSimulatorTab(closed, { scopeKey, sessionId, udid, title });
  }
  const nextTabs = tabs.slice();
  nextTabs[index] = {
    key: nextKey,
    type: PREVIEW_TAB_TYPES.SIMULATOR,
    sessionId,
    udid,
    title: String(title || '').trim() || defaultSimulatorTabTitle(),
  };
  const renameOrder = (orderMap) => Object.fromEntries(Object.entries(orderMap || {})
    .map(([view, order]) => [view, Array.isArray(order)
      ? order.map((key) => (key === tabKey ? nextKey : key))
      : order]));
  const wasActive = activePreviewTab(source, { scopeKey, sessionId })?.key === tabKey;
  return {
    ...source,
    simulatorTabsBySession: withSimulatorTabs(source, sessionId, nextTabs),
    tabOrderByView: renameOrder(source.tabOrderByView),
    ...(wasActive ? withSessionActiveTab(source, scopeKey, sessionId, nextKey) : {}),
  };
}

export function closeSimulatorTab(state, { scopeKey = '', sessionId = '', udid = '' } = {}) {
  return closePreviewTab(state, { scopeKey, sessionId, tabKey: simulatorTabKey(udid) });
}

// git 级变更详情页签(仿 openSessionChangesTab):同一 session 单页签,
// 点不同文件只更新 expandedFile + 自增 revision(触发详情栏滚动/展开),
// cwd/base 决定详情栏拉哪一份 diff。
export function openGitChangesTab(state, {
  scopeKey = '',
  sessionId = '',
  cwd = '',
  base,
  expandedFile = '',
  fileCount,
} = {}) {
  if (!sessionId) return state || {};
  const source = state && typeof state === 'object' ? state : {};
  const key = gitChangesTabKey(sessionId);
  const previousTab = source.changeTabsBySession?.[sessionId];
  const previousIsGit = previousTab?.type === PREVIEW_TAB_TYPES.GIT_CHANGES;
  // base 缺省(在详情栏内点文件仅换展开文件时)保留页签原 base,别把比较基线清空。
  const nextBase = base !== undefined ? base : (previousIsGit ? previousTab?.base : '') || '';
  const nextFileCount = fileCount !== undefined
    ? (Number.isFinite(Number(fileCount)) ? Number(fileCount) : 0)
    : (previousIsGit ? (previousTab?.fileCount || 0) : 0);
  const tab = {
    key,
    type: PREVIEW_TAB_TYPES.GIT_CHANGES,
    sessionId,
    cwd: cwd || (previousIsGit ? previousTab?.cwd : '') || '',
    base: nextBase,
    expandedFile: normalizeTreePath(expandedFile),
    expandedFileRevision: (Number(previousIsGit ? previousTab?.expandedFileRevision : 0) || 0) + 1,
    reloadRevision: Number(previousIsGit ? previousTab?.reloadRevision : 0) || 0,
    fileCount: nextFileCount,
  };
  return {
    ...source,
    changeTabsBySession: {
      ...(source.changeTabsBySession || {}),
      [sessionId]: tab,
    },
    activeTabBySession: {
      ...(source.activeTabBySession || {}),
      [sessionId]: key,
    },
    activeTabByView: {
      ...(source.activeTabByView || {}),
      [viewKey(scopeKey, sessionId)]: key,
    },
  };
}

// 详情刷新与文件树刷新是两套独立生命周期。同一路径/同一变更页签需要在不
// 关闭 tab 的前提下重新读取，因此只递增目标 tab 自己的 reloadRevision。
export function refreshPreviewTab(state, {
  scopeKey = '',
  sessionId = '',
  tabKey = '',
} = {}) {
  if (!tabKey) return state || {};
  const source = state && typeof state === 'object' ? state : {};

  if (tabKey.startsWith('file:')) {
    const tabs = source.fileTabsByView?.[viewKey(scopeKey, sessionId)] || [];
    if (!tabs.some((tab) => tab.key === tabKey)) return source;
    return {
      ...source,
      fileTabsByView: {
        ...(source.fileTabsByView || {}),
        [viewKey(scopeKey, sessionId)]: tabs.map((tab) => (tab.key === tabKey
          ? (previewTabHasUnsavedDraft(tab)
            ? {
              ...tab,
              edit: {
                ...tab.edit,
                externalChanged: true,
                error: tab.edit?.error || '',
              },
            }
            : { ...tab, reloadRevision: (Number(tab.reloadRevision) || 0) + 1 })
          : tab)),
      },
    };
  }

  if (isChangeTabKey(tabKey) && sessionId) {
    const tab = source.changeTabsBySession?.[sessionId];
    if (!tab || tab.key !== tabKey) return source;
    return {
      ...source,
      changeTabsBySession: {
        ...(source.changeTabsBySession || {}),
        [sessionId]: {
          ...tab,
          reloadRevision: (Number(tab.reloadRevision) || 0) + 1,
        },
      },
    };
  }

  return source;
}

// 更新已打开的 git 变更页签的 base / fileCount,不动 expandedFile / revision
// (基线切换或文件数刷新时用)。页签不存在或不是 git 类型则原样返回。
export function updateGitChangesTab(state, {
  sessionId = '',
  base,
  fileCount,
} = {}) {
  if (!sessionId) return state || {};
  const source = state && typeof state === 'object' ? state : {};
  const existing = source.changeTabsBySession?.[sessionId];
  if (!existing || existing.type !== PREVIEW_TAB_TYPES.GIT_CHANGES) return source;
  const nextTab = { ...existing };
  if (base !== undefined) nextTab.base = base;
  if (fileCount !== undefined) {
    nextTab.fileCount = Number.isFinite(Number(fileCount)) ? Number(fileCount) : existing.fileCount;
  }
  return {
    ...source,
    changeTabsBySession: {
      ...(source.changeTabsBySession || {}),
      [sessionId]: nextTab,
    },
  };
}

export function updateSessionChangesTab(state, {
  sessionId = '',
  fileCount = 0,
} = {}) {
  if (!sessionId) return state || {};
  const source = state && typeof state === 'object' ? state : {};
  const existing = source.changeTabsBySession?.[sessionId];
  // 只更新会话级 hunks 页签;git 页签共用同一槽位,别被会话变更计数误改
  // (git 文件数由 GitChangesPanel 自己经 updateGitChangesTab 推送)。
  if (!existing || existing.type !== PREVIEW_TAB_TYPES.SESSION_CHANGES) return source;
  return {
    ...source,
    changeTabsBySession: {
      ...(source.changeTabsBySession || {}),
      [sessionId]: {
        ...existing,
        fileCount: existing.turnUserMessageId
          ? existing.fileCount
          : (Number.isFinite(Number(fileCount)) ? Number(fileCount) : 0),
      },
    },
  };
}

export function reorderPreviewTab(state, {
  scopeKey = '',
  sessionId = '',
  sourceKey = '',
  targetKey = '',
  placement = 'before',
} = {}) {
  const source = state && typeof state === 'object' ? state : {};
  if (!sourceKey || !targetKey || sourceKey === targetKey) return source;
  const tabs = visiblePreviewTabs(source, { scopeKey, sessionId });
  const keys = tabs.map((tab) => tab.key);
  if (!keys.includes(sourceKey) || !keys.includes(targetKey)) return source;

  const withoutSource = keys.filter((key) => key !== sourceKey);
  const targetIndex = withoutSource.indexOf(targetKey);
  if (targetIndex < 0) return source;
  const insertIndex = placement === 'after' ? targetIndex + 1 : targetIndex;
  const nextOrder = [
    ...withoutSource.slice(0, insertIndex),
    sourceKey,
    ...withoutSource.slice(insertIndex),
  ];

  return {
    ...source,
    tabOrderByView: {
      ...(source.tabOrderByView || {}),
      [viewKey(scopeKey, sessionId)]: nextOrder,
    },
  };
}

export function closePreviewTab(state, { scopeKey = '', sessionId = '', tabKey = '' } = {}) {
  if (!tabKey) return state || {};
  const source = state && typeof state === 'object' ? state : {};
  const visibleBeforeClose = visiblePreviewTabs(source, { scopeKey, sessionId });
  if (!visibleBeforeClose.some((tab) => tab.key === tabKey)) return source;
  const activeKey = activePreviewTab(source, { scopeKey, sessionId })?.key;
  if (tabKey.startsWith('file:')) {
    const tabs = source.fileTabsByView?.[viewKey(scopeKey, sessionId)] || [];
    const nextTabs = tabs.filter((tab) => tab.key !== tabKey);
    const nextFileTabs = { ...(source.fileTabsByView || {}) };
    if (nextTabs.length > 0) nextFileTabs[viewKey(scopeKey, sessionId)] = nextTabs;
    else delete nextFileTabs[viewKey(scopeKey, sessionId)];
    const nextVisibleActive = nextActiveAfterClose(visibleBeforeClose, tabKey, activeKey);
    return {
      ...source,
      fileTabsByView: nextFileTabs,
      activeTabByView: {
        ...(source.activeTabByView || {}),
        [viewKey(scopeKey, sessionId)]: nextVisibleActive,
      },
      tabOrderByView: removeKeysFromOrders(source.tabOrderByView, [tabKey]),
    };
  }
  if (isChangeTabKey(tabKey)) {
    const nextChangeTabs = { ...(source.changeTabsBySession || {}) };
    delete nextChangeTabs[sessionId];
    const nextActive = { ...(source.activeTabBySession || {}) };
    delete nextActive[sessionId];
    const nextActiveByView = { ...(source.activeTabByView || {}) };
    nextActiveByView[viewKey(scopeKey, sessionId)] = nextActiveAfterClose(
      visibleBeforeClose,
      tabKey,
      activeKey,
    );
    return {
      ...source,
      changeTabsBySession: nextChangeTabs,
      activeTabBySession: nextActive,
      activeTabByView: nextActiveByView,
      tabOrderByView: removeKeysFromOrders(source.tabOrderByView, [tabKey]),
    };
  }
  if (isBrowserTabKey(tabKey) && sessionId) {
    const nextBrowserTabs = { ...(source.browserTabsBySession || {}) };
    const stored = nextBrowserTabs[sessionId];
    const tabs = Array.isArray(stored) ? stored : (stored ? [stored] : []);
    const remaining = tabs.filter((tab) => tab.key !== tabKey);
    if (remaining.length > 0) nextBrowserTabs[sessionId] = remaining;
    else delete nextBrowserTabs[sessionId];
    const nextActive = { ...(source.activeTabBySession || {}) };
    const nextActiveByView = { ...(source.activeTabByView || {}) };
    const nextVisibleActive = nextActiveAfterClose(
      visibleBeforeClose,
      tabKey,
      activeKey,
    );
    nextActiveByView[viewKey(scopeKey, sessionId)] = nextVisibleActive;
    if (isSessionScopedTabKey(nextVisibleActive)) {
      nextActive[sessionId] = nextVisibleActive;
    } else {
      delete nextActive[sessionId];
    }
    return {
      ...source,
      browserTabsBySession: nextBrowserTabs,
      activeTabBySession: nextActive,
      activeTabByView: nextActiveByView,
      tabOrderByView: removeKeysFromOrders(source.tabOrderByView, [tabKey]),
    };
  }
  if (isSimulatorTabKey(tabKey) && sessionId) {
    const remaining = simulatorTabsForSession(source, sessionId).filter((tab) => tab.key !== tabKey);
    const nextActive = { ...(source.activeTabBySession || {}) };
    const nextVisibleActive = nextActiveAfterClose(visibleBeforeClose, tabKey, activeKey);
    if (isSessionScopedTabKey(nextVisibleActive)) nextActive[sessionId] = nextVisibleActive;
    else delete nextActive[sessionId];
    return {
      ...source,
      simulatorTabsBySession: withSimulatorTabs(source, sessionId, remaining),
      activeTabBySession: nextActive,
      activeTabByView: {
        ...(source.activeTabByView || {}),
        [viewKey(scopeKey, sessionId)]: nextVisibleActive,
      },
      tabOrderByView: removeKeysFromOrders(source.tabOrderByView, [tabKey]),
    };
  }
  return source;
}

export function closeVisiblePreviewTabs(state, { scopeKey = '', sessionId = '' } = {}) {
  const source = state && typeof state === 'object' ? state : {};
  const closedKeys = visiblePreviewTabs(source, { scopeKey, sessionId }).map((tab) => tab.key);
  const nextFileTabs = { ...(source.fileTabsByView || {}) };
  const nextActiveByView = { ...(source.activeTabByView || {}) };
  delete nextFileTabs[viewKey(scopeKey, sessionId)];
  delete nextActiveByView[viewKey(scopeKey, sessionId)];

  const nextChangeTabs = { ...(source.changeTabsBySession || {}) };
  const nextBrowserTabs = { ...(source.browserTabsBySession || {}) };
  const nextSimulatorTabs = { ...(source.simulatorTabsBySession || {}) };
  const nextActiveBySession = { ...(source.activeTabBySession || {}) };
  if (sessionId) {
    delete nextChangeTabs[sessionId];
    delete nextBrowserTabs[sessionId];
    delete nextSimulatorTabs[sessionId];
    delete nextActiveBySession[sessionId];
  }

  return {
    ...source,
    fileTabsByView: nextFileTabs,
    changeTabsBySession: nextChangeTabs,
    browserTabsBySession: nextBrowserTabs,
    simulatorTabsBySession: nextSimulatorTabs,
    activeTabBySession: nextActiveBySession,
    activeTabByView: nextActiveByView,
    tabOrderByView: removeKeysFromOrders(source.tabOrderByView, closedKeys),
  };
}

export function closeVisiblePreviewTabsConfirmationMessage(tabCount = 0) {
  const count = Number(tabCount);
  if (!Number.isFinite(count) || count <= 0) return '';
  return `关闭预览面板会关闭当前预览区域的全部 ${count} 个标签页。是否继续？`;
}

export function closeOtherPreviewTabs(state, { scopeKey = '', sessionId = '', tabKey = '' } = {}) {
  if (!tabKey) return state || {};
  const source = state && typeof state === 'object' ? state : {};
  const visible = visiblePreviewTabs(source, { scopeKey, sessionId });
  if (!visible.some((tab) => tab.key === tabKey)) return source;
  const closedKeys = visible.filter((tab) => tab.key !== tabKey).map((tab) => tab.key);
  if (closedKeys.length === 0) return source;
  let next = source;
  for (const key of closedKeys) {
    next = closePreviewTab(next, { scopeKey, sessionId, tabKey: key });
  }
  return {
    ...next,
    activeTabByView: {
      ...(next.activeTabByView || {}),
      [viewKey(scopeKey, sessionId)]: tabKey,
    },
  };
}

export function closePreviewTabsToRight(state, { scopeKey = '', sessionId = '', tabKey = '' } = {}) {
  if (!tabKey) return state || {};
  const source = state && typeof state === 'object' ? state : {};
  const visible = visiblePreviewTabs(source, { scopeKey, sessionId });
  const index = visible.findIndex((tab) => tab.key === tabKey);
  if (index < 0 || index >= visible.length - 1) return source;
  const closedKeys = visible.slice(index + 1).map((tab) => tab.key);
  let next = source;
  for (const key of closedKeys) {
    next = closePreviewTab(next, { scopeKey, sessionId, tabKey: key });
  }
  return next;
}

export function activatePreviewTab(state, { scopeKey = '', sessionId = '', tabKey = '' } = {}) {
  if (!tabKey) return state || {};
  const source = state && typeof state === 'object' ? state : {};
  if (!visiblePreviewTabs(source, { scopeKey, sessionId }).some((tab) => tab.key === tabKey)) return source;
  if (isSessionScopedTabKey(tabKey) && sessionId) {
    return {
      ...source,
      activeTabBySession: {
        ...(source.activeTabBySession || {}),
        [sessionId]: tabKey,
      },
      activeTabByView: {
        ...(source.activeTabByView || {}),
        [viewKey(scopeKey, sessionId)]: tabKey,
      },
    };
  }
  if (scopeKey) {
    return {
      ...source,
      activeTabByView: {
        ...(source.activeTabByView || {}),
        [viewKey(scopeKey, sessionId)]: tabKey,
      },
    };
  }
  return source;
}
