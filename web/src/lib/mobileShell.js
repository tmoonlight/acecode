// 手机上的几条外壳规则(安卓 App 与手机浏览器共用):
//   - 是不是在 ACECode 安卓 App 里(App 在 WebView 的 UA 末尾加了 ACECodeAndroid/<版本>);
//   - 窄屏分界:与安卓「紧凑宽度」一致的 600,手机竖屏都在这条线以下;
//   - 长按是手指发起、落在输入框或选中文字上时,让系统自己的复制 / 粘贴工具条出来;
//   - 返回键先关最上面那一层浮层(各浮层本来就响应 Esc,这里只判断「有没有」再补发一次 Esc)。
// 只放不依赖 React 的判定,测试里用鸭子类型的假 document 即可;React 侧见 useNarrowLayout.js。

export const ANDROID_APP_UA_MARK = 'ACECodeAndroid/';
export const NARROW_LAYOUT_MAX_WIDTH = 599;
export const NARROW_LAYOUT_QUERY = `(max-width: ${NARROW_LAYOUT_MAX_WIDTH}px)`;

export function isAndroidApp(userAgent = globalThis.navigator?.userAgent || '') {
  return String(userAgent).includes(ANDROID_APP_UA_MARK);
}

export function shouldUseNativeContextMenu({
  pointerType = '',
  firesTouchEvents = false,
  editable = false,
  hasSelection = false,
} = {}) {
  const touch = pointerType === 'touch' || pointerType === 'pen' || firesTouchEvents === true;
  return touch && (editable || hasSelection);
}

// 返回键能关掉的浮层:对话框、设置窗口、侧栏浮层、菜单。
export const BACK_DISMISSIBLE_SELECTOR = [
  '[data-ace-modal-dialog="true"]',
  '[aria-modal="true"]',
  '[data-settings-window="true"]',
  '[data-sidebar-popup]:not([data-sidebar-hidden="true"])',
  '[role="menu"]',
  '[data-shortcut-menu="true"]',
  '.ace-composer-popup',
  '[data-composer-capability-menu="true"]',
].join(',');

function isRendered(element) {
  return typeof element?.getClientRects !== 'function' || element.getClientRects().length > 0;
}

export function hasBackDismissibleLayer(doc) {
  if (!doc?.querySelectorAll) return false;
  return [...doc.querySelectorAll(BACK_DISMISSIBLE_SELECTOR)].some(isRendered);
}

// 补发的 Esc 打在 body 上:document 级的浮层监听都收得到,
// 而输入框自己的 Esc(清空补全、退出语音等)不会被误触发。
export function handleBack(doc = globalThis.document) {
  if (!hasBackDismissibleLayer(doc)) return false;
  const KeyboardEventCtor = doc.defaultView?.KeyboardEvent || globalThis.KeyboardEvent;
  doc.body.dispatchEvent(new KeyboardEventCtor('keydown', {
    key: 'Escape',
    code: 'Escape',
    bubbles: true,
    cancelable: true,
  }));
  return true;
}

export function installBackBridge(win = globalThis.window) {
  if (!win) return;
  win.__aceHandleBack = () => handleBack(win.document);
}
