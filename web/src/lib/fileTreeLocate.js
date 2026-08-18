import { normalizeTreePath, normalizeWorkspaceRelativePath } from './fileTreeChangeStatus.js';

export function pathAncestors(path) {
  const parts = normalizeTreePath(path).split('/').filter(Boolean);
  const result = [];
  for (let i = 1; i < parts.length; i += 1) {
    result.push(parts.slice(0, i).join('/'));
  }
  return result;
}

// 把聊天里的文件/目录链接收成文件树可消费的定位计划:
// selectedPath 是 cwd 相对路径;expandedDirs 含祖先,目录再带上自身以便展开子项。
export function fileTreeLocatePlan(path, cwd = '', { includeSelf = false } = {}) {
  const selectedPath = normalizeWorkspaceRelativePath(path, cwd);
  if (!selectedPath) return null;
  const expandedDirs = pathAncestors(selectedPath);
  if (includeSelf) expandedDirs.push(selectedPath);
  return { selectedPath, expandedDirs };
}
