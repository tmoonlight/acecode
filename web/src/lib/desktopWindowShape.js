// 主题异形窗口推送(主题包顶层 "window": {"shape": "chamfer"})。
//
// 只有当前主题声明了切角,才经 aceDesktop_setWindowShape 让 Windows 桌面壳把窗口
// 四角切掉、补画阴影;离开切角主题时推 {shape:'none'} 恢复矩形。普通主题每个页面
// 最多推一次 none:页面重载、整页切换工作区后,脚本不知道桌面壳是否还留着上一页
// 设的切角,而 none 对本来就是矩形的窗口是空操作(web_host 状态没变直接返回),
// 窗口保持系统原样。桌面壳确认生效后在 <html> 上标 data-window-shape,
// 缩放角(FramelessResizeHandles)与 HUD 外框的斜边据此挪到切角以内;macOS / Linux
// 桌面壳与浏览器直连都没有这个能力,属性不会出现。
//
// 辉光色取当前配色的 --ace-accent(随风格切换),与 desktopWindowBackground 一样
// 读 computed style,不在 JS 里维护主题色表。

import { normalizeThemeBackgroundColor } from './desktopWindowBackground.js';

export const WINDOW_SHAPE_ATTRIBUTE = 'data-window-shape';

export function windowShapeRequest(shape, glow) {
  if (shape !== 'chamfer') return { shape: 'none' };
  const color = normalizeThemeBackgroundColor(glow);
  return color ? { shape: 'chamfer', glow: color } : { shape: 'chamfer' };
}

// 每个页面一个推送器:记住上次推给桌面壳的请求,相同的不重复过桥(主题刷新、
// 重新渲染都会调用)。初始状态未知,第一次推送一定过桥。
export function createWindowShapePusher() {
  let lastKey = null;
  let generation = 0;
  return function pushWindowShape(
    shape,
    win = typeof window !== 'undefined' ? window : undefined,
    doc = typeof document !== 'undefined' ? document : undefined,
  ) {
    if (!win || !doc) return false;
    const bridge = win.aceDesktop_setWindowShape;
    if (typeof bridge !== 'function') return false;
    let glow = '';
    if (shape === 'chamfer') {
      try {
        glow = win.getComputedStyle(doc.documentElement).getPropertyValue('--ace-accent') || '';
      } catch {
        glow = '';
      }
    }
    const request = windowShapeRequest(shape, glow);
    const key = JSON.stringify(request);
    if (key === lastKey) return false;
    lastKey = key;
    const root = doc.documentElement;
    const current = ++generation;
    const settle = (ok) => {
      if (current !== generation) return;
      if (ok && request.shape === 'chamfer') root.setAttribute(WINDOW_SHAPE_ATTRIBUTE, 'chamfer');
      else root.removeAttribute(WINDOW_SHAPE_ATTRIBUTE);
    };
    if (request.shape !== 'chamfer') settle(false);
    try {
      Promise.resolve(bridge(request)).then((result) => settle(result?.ok === true), () => settle(false));
    } catch {
      settle(false);
    }
    return true;
  };
}

export const pushWindowShape = createWindowShapePusher();
