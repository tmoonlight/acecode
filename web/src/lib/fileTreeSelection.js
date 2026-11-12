// 右侧文件树的多选:Shift 选一段、Ctrl / Cmd 逐个加减,规则与左侧会话栏、系统资源管理器一致。
// 行的先后以「当前画出来的行」为准:展开的目录才把子项算进去,折叠后看不见的行不留在选区里,
// 批量操作因此永远只作用在用户看得见的行上。
import { normalizeTreePath } from './fileTreeChangeStatus.js';
import { pruneSidebarSelection, selectSidebarRange } from './sidebarSessionSelection.js';

export const FILE_TREE_BATCH_ACTIONS = Object.freeze({
  ADD_CONTEXT: 'add_context',
  PREVIEW: 'preview',
  COPY_RELATIVE_PATHS: 'copy_relative_paths',
  COPY_ABSOLUTE_PATHS: 'copy_absolute_paths',
  EXPAND: 'expand',
  COLLAPSE: 'collapse',
});

export function emptyFileTreeSelection() {
  return { anchor: '', keys: new Set() };
}

function entryName(path) {
  return String(path || '').split('/').filter(Boolean).pop() || String(path || '');
}

// treeCache:目录路径 → /api/files 条目;expandedDirs:已展开的目录路径。
// 返回与渲染顺序一致的行:[{ path, name, directory }]。
export function visibleFileTreeEntries(treeCache, expandedDirs) {
  const rows = [];
  const walked = new Set();
  const walk = (parentPath) => {
    if (walked.has(parentPath)) return;
    walked.add(parentPath);
    const entries = treeCache?.get?.(parentPath);
    if (!Array.isArray(entries)) return;
    for (const entry of entries) {
      if (!entry?.path) continue;
      const directory = entry.kind === 'dir';
      rows.push({ path: entry.path, name: entry.name || entryName(entry.path), directory });
      if (directory && expandedDirs?.has?.(entry.path)) walk(entry.path);
    }
  };
  walk('');
  return rows;
}

// activePath:正在预览的文件所在的行(行上有高亮)。还没有选区就按住 Shift / Ctrl 点别的行时,
// 这一行看起来已经选中,所以算进选区 —— 否则用户看到两行高亮,操作却只对一行生效。
export function selectFileTreeRow(state, visiblePaths, path, modifiers = {}, activePath = '') {
  const modified = !!(modifiers.shiftKey || modifiers.ctrlKey || modifiers.metaKey);
  const adoptActive = modified && state.keys.size === 0 && activePath && activePath !== path
    && visiblePaths.includes(activePath);
  const base = adoptActive
    ? {
        anchor: visiblePaths.includes(state.anchor) ? state.anchor : activePath,
        keys: new Set([activePath]),
      }
    : state;
  return selectSidebarRange(base, visiblePaths, path, modifiers);
}

export function selectAllFileTreeRows(state, visiblePaths) {
  if (!visiblePaths.length) return state;
  const anchor = visiblePaths.includes(state.anchor) ? state.anchor : visiblePaths[0];
  return { anchor, keys: new Set(visiblePaths) };
}

export function pruneFileTreeSelection(state, visiblePaths) {
  return pruneSidebarSelection(state, visiblePaths);
}

// 预览中的文件换了(点页签、定位)时,没有多选就让选区跟过去,避免旧行还留着选中底色。
export function followActiveFileTreeRow(state, visiblePaths, activePath) {
  if (!activePath || state.keys.size > 1 || !visiblePaths.includes(activePath)) return state;
  if (state.anchor === activePath && state.keys.size === 1 && state.keys.has(activePath)) return state;
  return { anchor: activePath, keys: new Set([activePath]) };
}

export function selectedFileTreeEntries(visibleEntries, keys) {
  return (Array.isArray(visibleEntries) ? visibleEntries : []).filter(entry => keys?.has?.(entry.path));
}

// 选中的目录已经带上了里面的全部内容:引用 / 拖动时只留最外层,不重复给子项。
export function outermostFileTreeEntries(entries) {
  const list = Array.isArray(entries) ? entries : [];
  const directories = list.filter(entry => entry.directory).map(entry => normalizeTreePath(entry.path));
  return list.filter((entry) => {
    const path = normalizeTreePath(entry.path);
    return !directories.some(directory => directory !== path && path.startsWith(`${directory}/`));
  });
}

// 多选右键菜单的内容:[{ action, count, separatorBefore }]。数量是这一项实际会处理的条数。
export function fileTreeBatchMenu(entries, expandedDirs) {
  const list = Array.isArray(entries) ? entries : [];
  if (list.length < 2) return [];
  const files = list.filter(entry => !entry.directory);
  const directories = list.filter(entry => entry.directory);
  const expanded = directories.filter(entry => expandedDirs?.has?.(entry.path));
  const items = [
    { action: FILE_TREE_BATCH_ACTIONS.ADD_CONTEXT, count: outermostFileTreeEntries(list).length },
  ];
  if (files.length) items.push({ action: FILE_TREE_BATCH_ACTIONS.PREVIEW, count: files.length });
  items.push(
    { action: FILE_TREE_BATCH_ACTIONS.COPY_RELATIVE_PATHS, count: list.length, separatorBefore: true },
    { action: FILE_TREE_BATCH_ACTIONS.COPY_ABSOLUTE_PATHS, count: list.length },
  );
  if (directories.length > expanded.length) {
    items.push({
      action: FILE_TREE_BATCH_ACTIONS.EXPAND,
      count: directories.length - expanded.length,
      separatorBefore: true,
    });
  }
  if (expanded.length) {
    items.push({
      action: FILE_TREE_BATCH_ACTIONS.COLLAPSE,
      count: expanded.length,
      separatorBefore: directories.length === expanded.length,
    });
  }
  return items;
}

// 批量展开 / 折叠:只动选中的目录本身,与单个目录的右键菜单行为一致。
export function toggleFileTreeDirectories(expandedDirs, directoryPaths, expand) {
  const paths = Array.isArray(directoryPaths) ? directoryPaths.filter(Boolean) : [];
  const current = expandedDirs instanceof Set ? expandedDirs : new Set();
  if (!paths.some(path => current.has(path) !== !!expand)) return expandedDirs;
  const next = new Set(current);
  for (const path of paths) {
    if (expand) next.add(path);
    else next.delete(path);
  }
  return next;
}
