// 侧栏已加载的工作区列表(/api/workspaces)的只读共享视图。
//
// 这个接口要扫很多目录,摘要面板等其它地方不另行请求,只读侧栏发布的这一份:
// Sidebar 每次拿到新列表就 publish,读方按 workspace hash 取自己那一项
// (名称、自定义图标、附加文件夹)。同一批对象引用重复发布不通知。
let entries = new Map();
const listeners = new Set();

export function publishWorkspaceDirectory(list) {
  const next = new Map();
  for (const workspace of Array.isArray(list) ? list : []) {
    const hash = String(workspace?.hash || '');
    if (hash) next.set(hash, workspace);
  }
  const unchanged = next.size === entries.size
    && [...next].every(([hash, workspace]) => entries.get(hash) === workspace);
  if (unchanged) return false;
  entries = next;
  for (const listener of [...listeners]) listener();
  return true;
}

export function workspaceDirectoryEntry(hash) {
  return entries.get(String(hash || '')) || null;
}

export function subscribeWorkspaceDirectory(listener) {
  listeners.add(listener);
  return () => listeners.delete(listener);
}
