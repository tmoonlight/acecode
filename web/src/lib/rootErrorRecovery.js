// 根级错误兜底的判定逻辑(纯函数,components/RootErrorBoundary.jsx 调用)。
//
// 背景:页面活过一次前端重建或 daemon 升级后,按需加载的代码块(文件名带内容哈希)
// 已经不在了,动态 import 失败;没有错误边界接住时 React 会卸掉整棵界面,窗口只剩
// 底色。这类错误重新加载页面就能拿到新的文件名,所以自动重载一次;其它渲染错误
// 重载不一定有用,只给按钮让用户决定。

// 各内核对「动态 import 失败」的报错文本,加上 Vite 预加载样式失败的文本。
const CHUNK_LOAD_PATTERNS = [
  /Failed to fetch dynamically imported module/i,
  /error loading dynamically imported module/i,
  /Importing a module script failed/i,
  /Unable to preload CSS/i,
];

export const ROOT_ERROR_AUTO_RELOAD_KEY = 'acecode.rootErrorAutoReloadAt';

// 自动重载后这么久之内再出同类错误就不再重载:重载后代码块仍然缺失(例如 daemon
// 指向的静态目录本身不完整)时,靠它停在兜底界面而不是无限刷新。
export const ROOT_ERROR_AUTO_RELOAD_COOLDOWN_MS = 60_000;

export function rootErrorMessage(error) {
  if (error == null) return '';
  if (typeof error === 'string') return error;
  try {
    return typeof error.message === 'string' ? error.message : String(error);
  } catch {
    return '';
  }
}

export function isChunkLoadError(error) {
  const message = rootErrorMessage(error);
  return CHUNK_LOAD_PATTERNS.some((pattern) => pattern.test(message));
}

// 返回 'reload'(立刻重新加载页面)或 'fallback'(显示兜底界面)。
export function planRootErrorRecovery({ error, now, lastAutoReloadAt } = {}) {
  if (!isChunkLoadError(error)) return 'fallback';
  if (!Number.isFinite(now)) return 'fallback';
  if (Number.isFinite(lastAutoReloadAt)) {
    const elapsed = now - lastAutoReloadAt;
    // 时钟回拨时 elapsed 为负,同样按「刚重载过」处理。
    if (elapsed < ROOT_ERROR_AUTO_RELOAD_COOLDOWN_MS) return 'fallback';
  }
  return 'reload';
}

// storage 取不到或抛错(隐私模式、被禁用)时一律当作没有记录。
export function readRootErrorAutoReloadAt(storage) {
  try {
    const raw = storage?.getItem?.(ROOT_ERROR_AUTO_RELOAD_KEY);
    if (raw == null || raw === '') return null;
    const value = Number(raw);
    return Number.isFinite(value) ? value : null;
  } catch {
    return null;
  }
}

// 写不进去时返回 false:调用方据此放弃自动重载,否则没有记录就可能无限刷新。
export function writeRootErrorAutoReloadAt(storage, now) {
  try {
    if (!storage || typeof storage.setItem !== 'function') return false;
    storage.setItem(ROOT_ERROR_AUTO_RELOAD_KEY, String(now));
    return true;
  } catch {
    return false;
  }
}
