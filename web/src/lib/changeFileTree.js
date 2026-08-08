import {
  normalizeTreePath,
  normalizeWorkspaceRelativePath,
} from './fileTreeChangeStatus.js';

export const CHANGE_LIST_VIEW_STORAGE_KEY = 'acecode.changeListView.v1';
export const CHANGE_LIST_VIEW_FLAT = 'flat';
export const CHANGE_LIST_VIEW_TREE = 'tree';
export const DEFAULT_CHANGE_LIST_VIEW = CHANGE_LIST_VIEW_FLAT;

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
