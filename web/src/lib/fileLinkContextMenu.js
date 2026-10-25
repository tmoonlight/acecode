// 对话内容里的本地文件链接(markdown.js 渲染的 a[data-file-path]、用户气泡里的
// @路径芯片)的右键菜单目标。链接本身只带模型写下的原始路径,相对路径要靠所在
// 区域声明的会话工作目录才能还原成绝对路径,所以只认带 data-file-link-root 的
// 区域(主对话转录区、侧边对话);区域外的链接(例如文件预览里的 markdown)沿用
// 通用菜单,不冒充能复制绝对路径。

export const FILE_LINK_TARGET_SELECTOR = '[data-file-path]';
export const FILE_LINK_ROOT_SELECTOR = '[data-file-link-root]';

const WINDOWS_DRIVE = /^[A-Za-z]:[\\/]/;

export function isAbsoluteLocalPath(path) {
  const value = String(path || '');
  return WINDOWS_DRIVE.test(value) || value.startsWith('\\\\') || value.startsWith('/');
}

function isWindowsStylePath(path) {
  const value = String(path || '');
  return WINDOWS_DRIVE.test(value) || value.startsWith('\\\\');
}

// 拆成「根」+「其余段」:N:/、//server/share/、/。
function splitRoot(unified) {
  if (/^[A-Za-z]:\//.test(unified)) return { root: unified.slice(0, 3), rest: unified.slice(3) };
  if (unified.startsWith('//')) {
    const parts = unified.slice(2).split('/');
    const host = parts.shift() || '';
    const share = parts.shift() || '';
    return { root: share ? `//${host}/${share}/` : `//${host}/`, rest: parts.join('/') };
  }
  return { root: '/', rest: unified.replace(/^\/+/, '') };
}

// 相对路径按工作目录拼接,并消掉 . / .. 段。输出用该平台的分隔符(Windows 路径用
// 反斜杠,与资源管理器地址栏、终端里粘贴的形态一致)。还原不了(相对路径但没有
// 工作目录)时返回空串,菜单据此把「绝对路径」「在资源管理器中显示」置灰。
export function resolveFileLinkAbsolutePath(cwd = '', path = '') {
  const raw = String(path || '').trim();
  if (!raw) return '';
  const base = String(cwd || '').trim();
  let combined = raw;
  if (!isAbsoluteLocalPath(raw)) {
    if (!isAbsoluteLocalPath(base)) return '';
    combined = `${base.replace(/[\\/]+$/, '')}/${raw}`;
  }
  const windows = isWindowsStylePath(combined);
  const { root, rest } = splitRoot(combined.replace(/\\/g, '/'));
  const segments = [];
  for (const segment of rest.split('/')) {
    if (!segment || segment === '.') continue;
    if (segment === '..') segments.pop();
    else segments.push(segment);
  }
  const resolved = root + segments.join('/');
  return windows ? resolved.replace(/\//g, '\\') : resolved;
}

// 落在工作目录内时给出相对路径(添加到会话时插入更短的 @路径),否则空串。
// Windows 路径大小写不敏感。
export function fileLinkWorkspaceRelativePath(cwd = '', absolutePath = '') {
  const base = resolveFileLinkAbsolutePath('', cwd);
  if (!base || !absolutePath) return '';
  const windows = isWindowsStylePath(base);
  const unifiedBase = base.replace(/\\/g, '/').replace(/\/+$/, '');
  const unifiedPath = String(absolutePath).replace(/\\/g, '/');
  const comparableBase = windows ? unifiedBase.toLowerCase() : unifiedBase;
  const comparablePath = windows ? unifiedPath.toLowerCase() : unifiedPath;
  if (!comparablePath.startsWith(`${comparableBase}/`)) return '';
  return unifiedPath.slice(unifiedBase.length + 1);
}

export function fileLinkName(path = '') {
  const parts = String(path || '').replace(/\\/g, '/').split('/').filter(Boolean);
  return parts.length ? parts[parts.length - 1] : '';
}

function attr(element, name) {
  return typeof element?.getAttribute === 'function' ? element.getAttribute(name) || '' : '';
}

export function fileLinkTargetFromElement(target) {
  if (!target || typeof target.closest !== 'function') return null;
  const link = target.closest(FILE_LINK_TARGET_SELECTOR);
  if (!link || typeof link.closest !== 'function') return null;
  const root = link.closest(FILE_LINK_ROOT_SELECTOR);
  if (!root) return null;
  const path = attr(link, 'data-file-path');
  if (!path) return null;
  const cwd = attr(root, 'data-file-link-cwd');
  const absolutePath = resolveFileLinkAbsolutePath(cwd, path);
  const lineValue = Number(attr(link, 'data-file-line'));
  return {
    type: 'file-link',
    path,
    kind: attr(link, 'data-file-kind') === 'directory' ? 'directory' : 'file',
    line: Number.isInteger(lineValue) && lineValue > 0 ? lineValue : null,
    absolutePath,
    // 工作目录外的路径没有相对形态,添加到会话时退回绝对路径。
    relativePath: absolutePath
      ? fileLinkWorkspaceRelativePath(cwd, absolutePath)
      : (isAbsoluteLocalPath(path) ? '' : path),
    name: fileLinkName(absolutePath || path),
    canAddContext: attr(root, 'data-file-link-attach') === 'true',
  };
}
