// 把右侧文件树里的行拖出去:落在输入框上 = 在光标处插入 @路径 引用(与右键「添加到会话」相同),
// 落在控制台终端上 = 贴入加好引号的绝对路径(与从系统拖文件进终端相同)。
// 文件树的拖动是自绘的指针拖动(不是 HTML5 拖放,桌面壳会把后者当成系统文件拖放拦走),
// 目标收不到 drop 事件,所以由文件树在松手时判断落点、派发 FILE_TREE_DROP_EVENT,落点所在的一方认领。
import { quoteShellPath } from './consoleDropPaths.js';
import { joinWorkspacePath } from './desktopContextMenu.js';
import { normalizeTreePath } from './fileTreeChangeStatus.js';
import { insertPathReferenceAtCaret, normalizeReferencePath } from './pathReference.js';

export const FILE_TREE_DROP_EVENT = 'acecode:file-tree-drop';
// 可接收的输入框根元素带这个属性(值为 "true");禁用时不带。
export const FILE_TREE_DROP_ATTR = 'data-file-tree-drop';
// 拖动经过时加在落点元素上的高亮 class。
export const FILE_TREE_DROP_ACTIVE_CLASS = 'is-file-tree-drop';
// 指针移动超过这个距离才算拖动,否则仍是一次点击。
export const FILE_TREE_DRAG_START_PX = 5;
// 拖动预览最多列出的名字个数,其余只体现在数量角标上。
export const FILE_TREE_DRAG_PREVIEW_LIMIT = 3;

const COMPOSER_SELECTOR = `[${FILE_TREE_DROP_ATTR}="true"]`;
const CONSOLE_SELECTOR = '.ace-console-term[data-tab-id]';

function sameWorkspace(a, b) {
  const left = normalizeTreePath(a).toLowerCase();
  return !!left && left === normalizeTreePath(b).toLowerCase();
}

// entries:[{ path, name, directory }](工作区相对路径)→ 拖放携带的条目。
export function fileTreeDropItems(entries, cwd = '') {
  return (Array.isArray(entries) ? entries : [])
    .filter(entry => entry?.path)
    .map(entry => ({
      path: entry.path,
      absolutePath: joinWorkspacePath(cwd, entry.path),
      name: entry.name || '',
      directory: !!entry.directory,
    }));
}

export function fileTreeDropTargetFrom(element) {
  if (!element?.closest) return null;
  // 菜单、弹层盖在上面时不算落在它下面的输入框 / 终端上。
  if (element.closest('[data-ace-native-overlay]')) return null;
  const composer = element.closest(COMPOSER_SELECTOR);
  if (composer) return { kind: 'composer', element: composer };
  const terminal = element.closest(CONSOLE_SELECTOR);
  if (terminal && terminal.style?.display !== 'none') return { kind: 'console', element: terminal };
  return null;
}

export function fileTreeDropTargetAt(doc, clientX, clientY) {
  if (!doc?.elementFromPoint || !Number.isFinite(clientX) || !Number.isFinite(clientY)) return null;
  return fileTreeDropTargetFrom(doc.elementFromPoint(clientX, clientY));
}

// 右键「添加到会话」没有落点:送给文件树所在会话的主输入框。
export function mainComposerFileTreeDropTarget(doc = globalThis.document) {
  const element = doc?.querySelector?.(`[data-main-composer="true"]${COMPOSER_SELECTOR}`);
  return element ? { kind: 'composer', element } : null;
}

// 返回 true 表示有一方认领了这次放下。
export function dispatchFileTreeDrop(target, items, { cwd = '', clientX, clientY } = {}, win = globalThis.window) {
  if (!target?.element || !items?.length || !win?.dispatchEvent) return false;
  const detail = { kind: target.kind, target: target.element, items, cwd, clientX, clientY, handled: false };
  win.dispatchEvent(new win.CustomEvent(FILE_TREE_DROP_EVENT, { detail }));
  return detail.handled;
}

// 输入框与文件树同一个工作区时引用相对路径(与「添加到会话」一致),否则给绝对路径。
export function insertFileTreeReferences(text, caret, items, { cwd = '', composerCwd = '' } = {}) {
  const relative = !cwd || !composerCwd || sameWorkspace(cwd, composerCwd);
  let next = {
    text: String(text || ''),
    cursor: Number.isFinite(caret) ? caret : String(text || '').length,
  };
  let count = 0;
  for (const item of Array.isArray(items) ? items : []) {
    const path = normalizeReferencePath((relative ? item?.path : item?.absolutePath) || item?.path || '');
    if (!path) continue;
    next = insertPathReferenceAtCaret(next.text, next.cursor, path, { directory: !!item.directory });
    count += 1;
  }
  return { ...next, count };
}

// 一段可直接贴进终端的输入:各路径按目标系统的 shell 规则加引号,空格分隔,末尾留一个空格。
export function fileTreeTerminalText(items, os) {
  const parts = (Array.isArray(items) ? items : [])
    .map(item => String(item?.absolutePath || ''))
    .filter(Boolean)
    .map(path => quoteShellPath(os === 'windows' ? path.replace(/\//g, '\\') : path, os));
  return parts.length ? `${parts.join(' ')} ` : '';
}

// 拖动预览跟在指针右下方;主要落点(输入框、控制台)都贴着窗口底边,
// 放不下时翻到指针上方 / 收回窗口内,不让预览被窗口边缘切掉。
export function fileTreeDragGhostPosition(point, { width = 0, height = 0, viewportWidth, viewportHeight } = {}) {
  const x = Number(point?.x) || 0;
  const y = Number(point?.y) || 0;
  const margin = 4;
  let left = x + 12;
  let top = y + 14;
  if (Number.isFinite(viewportWidth) && viewportWidth > 0) {
    left = Math.max(margin, Math.min(left, viewportWidth - width - margin));
  }
  if (Number.isFinite(viewportHeight) && viewportHeight > 0 && top + height > viewportHeight - margin) {
    top = Math.max(margin, y - height - 10);
  }
  return { left: Math.round(left), top: Math.round(top) };
}

// 拖动预览:最多列出前几个名字,总数由角标显示。
export function fileTreeDragPreview(entries) {
  const list = Array.isArray(entries) ? entries : [];
  return {
    count: list.length,
    rows: list.slice(0, FILE_TREE_DRAG_PREVIEW_LIMIT).map(entry => ({
      path: entry.path,
      name: entry.name || entry.path,
      directory: !!entry.directory,
    })),
  };
}
