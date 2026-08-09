import {
  normalizeTreePath,
  normalizeWorkspaceRelativePath,
} from './fileTreeChangeStatus.js';

export const CHANGE_LIST_VIEW_BY_CWD_STORAGE_KEY = 'acecode.changeListViewByCwd.v1';
export const CHANGE_LIST_VIEW_FLAT = 'flat';
export const CHANGE_LIST_VIEW_TREE = 'tree';
export const DEFAULT_CHANGE_LIST_VIEW = CHANGE_LIST_VIEW_TREE;
export const DEFAULT_CHANGE_LIST_VIEW_BY_CWD = Object.freeze({});

const CHANGE_LIST_VIEWS = new Set([
  CHANGE_LIST_VIEW_FLAT,
  CHANGE_LIST_VIEW_TREE,
]);

export function validateChangeListView(value) {
  return CHANGE_LIST_VIEWS.has(value);
}

export function effectiveChangeListView(value) {
  return validateChangeListView(value) ? value : DEFAULT_CHANGE_LIST_VIEW;
}

export function normalizeChangeListViewCwd(cwd = '') {
  const raw = typeof cwd === 'string' ? cwd.trim() : '';
  if (!raw) return '';

  const slashPath = raw.replace(/\\/g, '/');
  const rootPrefix = slashPath.startsWith('//')
    ? '//'
    : (slashPath.startsWith('/') ? '/' : '');
  const body = slashPath
    .slice(rootPrefix.length)
    .replace(/\/{2,}/g, '/')
    .replace(/\/+$/, '');
  let normalized = `${rootPrefix}${body}` || rootPrefix;

  if (/^[A-Za-z]:(?:\/|$)/.test(normalized) || normalized.startsWith('//')) {
    normalized = normalized.toLowerCase();
  }
  return normalized;
}

export function validateChangeListViewByCwd(value) {
  return Boolean(value)
    && typeof value === 'object'
    && !Array.isArray(value)
    && Object.values(value).every(validateChangeListView);
}

export function changeListViewForCwd(value, cwd = '') {
  if (!validateChangeListViewByCwd(value)) return DEFAULT_CHANGE_LIST_VIEW;
  return effectiveChangeListView(value[normalizeChangeListViewCwd(cwd)]);
}

export function updateChangeListViewForCwd(value, cwd, viewMode) {
  const current = validateChangeListViewByCwd(value)
    ? value
    : DEFAULT_CHANGE_LIST_VIEW_BY_CWD;
  if (!validateChangeListView(viewMode)) return current;

  const cwdKey = normalizeChangeListViewCwd(cwd);
  if (current[cwdKey] === viewMode) return current;
  return { ...current, [cwdKey]: viewMode };
}

export function normalizeChangeTreePath(path, cwd = '') {
  const workspaceRelative = normalizeWorkspaceRelativePath(path, cwd);
  return normalizeTreePath(workspaceRelative || path);
}

function compareNames(left, right) {
  const a = String(left || '');
  const b = String(right || '');
  const foldedA = a.toLocaleLowerCase();
  const foldedB = b.toLocaleLowerCase();
  if (foldedA < foldedB) return -1;
  if (foldedA > foldedB) return 1;
  if (a < b) return -1;
  if (a > b) return 1;
  return 0;
}

function sortTreeNodes(nodes) {
  nodes.sort((left, right) => {
    if (left.type !== right.type) return left.type === 'directory' ? -1 : 1;
    return compareNames(left.name, right.name) || left.order - right.order;
  });
  for (const node of nodes) {
    if (node.type === 'directory') sortTreeNodes(node.children);
  }
}

/**
 * Project compact Changes rows into a directory tree while retaining each
 * original row and path for diff lookup, selection, and native context actions.
 */
export function buildChangeFileTree(rows, cwd = '') {
  const list = Array.isArray(rows) ? rows : [];
  const root = { type: 'root', path: '', name: '', children: [] };
  const directories = new Map([['', root]]);

  list.forEach((row, order) => {
    const originalPath = typeof row?.path === 'string' ? row.path : '';
    const treePath = normalizeChangeTreePath(originalPath, cwd);
    const parts = treePath.split('/').filter(Boolean);
    const fallbackName = originalPath.trim() || `未命名文件 ${order + 1}`;
    const fileName = parts.pop() || fallbackName;
    let parent = root;
    let parentPath = '';

    for (const part of parts) {
      const directoryPath = parentPath ? `${parentPath}/${part}` : part;
      let directory = directories.get(directoryPath);
      if (!directory) {
        directory = {
          type: 'directory',
          key: `directory:${directoryPath}`,
          name: part,
          path: directoryPath,
          children: [],
          order,
        };
        directories.set(directoryPath, directory);
        parent.children.push(directory);
      }
      parent = directory;
      parentPath = directoryPath;
    }

    parent.children.push({
      type: 'file',
      key: `file:${treePath || 'unnamed'}:${order}`,
      name: fileName,
      path: originalPath,
      treePath,
      row,
      order,
    });
  });

  sortTreeNodes(root.children);
  return root.children;
}

export function changeTreeAncestorPaths(path, cwd = '') {
  const parts = normalizeChangeTreePath(path, cwd).split('/').filter(Boolean);
  const ancestors = [];
  for (let index = 1; index < parts.length; index += 1) {
    ancestors.push(parts.slice(0, index).join('/'));
  }
  return ancestors;
}

export function flattenVisibleChangeTree(nodes, collapsedPaths = new Set(), depth = 0) {
  const list = Array.isArray(nodes) ? nodes : [];
  const collapsed = collapsedPaths instanceof Set ? collapsedPaths : new Set();
  const visible = [];
  for (const node of list) {
    visible.push({ node, depth });
    if (node.type === 'directory' && !collapsed.has(node.path)) {
      visible.push(...flattenVisibleChangeTree(node.children, collapsed, depth + 1));
    }
  }
  return visible;
}
