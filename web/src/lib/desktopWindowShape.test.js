// 主题异形窗口:主题包声明的解析、向桌面壳推送的时机与去重、生效标记,以及
// 只在切角主题下才改缩放角与外框的样式约束。
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { themeWindowShape } from './themePackages.js';
import { WINDOW_SHAPE_ATTRIBUTE, createWindowShapePusher, windowShapeRequest } from './desktopWindowShape.js';

async function run(name, fn) { await fn(); console.log(`[pass] ${name}`); }
const read = (relative) => readFileSync(new URL(relative, import.meta.url), 'utf8');
const flush = () => new Promise((resolve) => setTimeout(resolve, 0));

// 假桌面环境:记录桥接调用,按 reply 决定返回值;documentElement 记录属性。
function fakeDesktop({ accent = ' #FF4D57', reply = { ok: true }, bridge = true } = {}) {
  const calls = [];
  const attributes = new Map();
  const doc = {
    documentElement: {
      setAttribute: (name, value) => attributes.set(name, value),
      removeAttribute: (name) => attributes.delete(name),
    },
  };
  const win = {
    getComputedStyle: () => ({ getPropertyValue: (name) => (name === '--ace-accent' ? accent : '') }),
  };
  if (bridge) win.aceDesktop_setWindowShape = (request) => { calls.push(request); return Promise.resolve(reply); };
  return { win, doc, calls, attributes };
}

// 场景:主题包顶层 "window" 字段。期望:只认 {"shape":"chamfer"};缺省、数组、
// 字符串、未知形状一律视为普通窗口(旧主题、写错的主题都不会被切角)。
await run('packages declare a chamfered window with a known shape only', () => {
  assert.equal(themeWindowShape({ window: { shape: 'chamfer' } }), 'chamfer');
  for (const windowShape of [undefined, null, 'chamfer', ['chamfer'], {}, { shape: 'round' }]) {
    assert.equal(themeWindowShape({ window: windowShape }), null, JSON.stringify(windowShape));
  }
  assert.equal(themeWindowShape(null), null);
});

// 场景:请求体。期望:切角带上规范化后的辉光色;颜色读不出时只切角不带辉光;
// 其它值一律是 none。
await run('requests carry the normalized glow color', () => {
  assert.deepEqual(windowShapeRequest('chamfer', ' #FF4D57'), { shape: 'chamfer', glow: '#ff4d57' });
  assert.deepEqual(windowShapeRequest('chamfer', 'rgb(1,2,3)'), { shape: 'chamfer' });
  assert.deepEqual(windowShapeRequest(null, '#ffffff'), { shape: 'none' });
});

// 场景:普通主题的页面。期望:只在页面第一次推送时发一次 none(桌面壳对本来就是
// 矩形的窗口不做任何事),之后重复渲染不再过桥;<html> 上没有标记。
await run('ordinary themes send a single none per page', async () => {
  const push = createWindowShapePusher();
  const desktop = fakeDesktop();
  assert.equal(push(null, desktop.win, desktop.doc), true);
  assert.equal(push(null, desktop.win, desktop.doc), false);
  await flush();
  assert.deepEqual(desktop.calls, [{ shape: 'none' }]);
  assert.equal(desktop.attributes.has(WINDOW_SHAPE_ATTRIBUTE), false);
});

// 场景:切角主题下整页切换工作区或重载页面,新页面是普通主题。
// 回归:推送器曾初始视为 none,新页面一次桥接都不发,桌面壳留着上一页的切角和补画阴影,
// 普通主题的窗口仍是切角的。期望:新页面的第一次推送把 none 送到桌面壳。
await run('a reloaded page clears a chamfer left by the previous page', async () => {
  const desktop = fakeDesktop();
  createWindowShapePusher()('chamfer', desktop.win, desktop.doc);
  const reloaded = createWindowShapePusher();
  assert.equal(reloaded(null, desktop.win, desktop.doc), true);
  assert.deepEqual(desktop.calls.at(-1), { shape: 'none' });
});

// 场景:切到切角主题、重复渲染、切换风格(辉光色变)、再切回普通主题。
// 期望:切角只推一次,相同请求不重复过桥;风格变化带新辉光色重推;桌面壳确认后
// 才标 data-window-shape;切回普通主题推 none 并立即去掉标记。
await run('chamfer themes push once per change and mark the root after the shell confirms', async () => {
  const push = createWindowShapePusher();
  const desktop = fakeDesktop();
  assert.equal(push('chamfer', desktop.win, desktop.doc), true);
  assert.equal(push('chamfer', desktop.win, desktop.doc), false);
  assert.deepEqual(desktop.calls, [{ shape: 'chamfer', glow: '#ff4d57' }]);
  assert.equal(desktop.attributes.has(WINDOW_SHAPE_ATTRIBUTE), false);
  await flush();
  assert.equal(desktop.attributes.get(WINDOW_SHAPE_ATTRIBUTE), 'chamfer');

  const cyan = fakeDesktop({ accent: '#2bd3f5' });
  cyan.win.aceDesktop_setWindowShape = desktop.win.aceDesktop_setWindowShape;
  assert.equal(push('chamfer', cyan.win, desktop.doc), true);
  assert.deepEqual(desktop.calls.at(-1), { shape: 'chamfer', glow: '#2bd3f5' });

  assert.equal(push(null, desktop.win, desktop.doc), true);
  assert.deepEqual(desktop.calls.at(-1), { shape: 'none' });
  assert.equal(desktop.attributes.has(WINDOW_SHAPE_ATTRIBUTE), false);
  await flush();
  assert.equal(desktop.attributes.has(WINDOW_SHAPE_ATTRIBUTE), false);
});

// 场景:macOS / Linux 桌面壳(桥接返回 ok:false)与浏览器直连(没有桥接)。
// 期望:不标 data-window-shape,缩放角与外框保持原样。
await run('unsupported shells leave the root unmarked', async () => {
  const push = createWindowShapePusher();
  const mac = fakeDesktop({ reply: { ok: false } });
  push('chamfer', mac.win, mac.doc);
  await flush();
  assert.equal(mac.attributes.has(WINDOW_SHAPE_ATTRIBUTE), false);
  const browser = fakeDesktop({ bridge: false });
  assert.equal(createWindowShapePusher()('chamfer', browser.win, browser.doc), false);
});

// 场景:样式与接线。期望:缩放角与外框斜边的调整都挂在 data-window-shape 下(普通
// 主题与其它平台不受影响);ThemeProvider 只把当前主题声明的形状交给推送器。
await run('styles and provider only change shaped windows', () => {
  const globals = read('../styles/globals.css');
  const cornerRules = globals.match(/^[^{}\n]*data-frameless-resize-corner[^{}\n]*\{/gm) || [];
  assert.ok(cornerRules.length >= 5);
  for (const rule of cornerRules) assert.match(rule, /^:root\[data-window-shape="chamfer"\] /);
  for (const corner of ['top-left', 'top-right', 'bottom-left', 'bottom-right']) {
    assert.ok(globals.includes(`[data-frameless-resize-corner="${corner}"] {
  clip-path: polygon(`), corner);
  }
  assert.match(read('../styles/themeHud.css'), /:root\[data-theme-chrome="hud"\]\[data-window-shape="chamfer"\] \.ace-app-shell::after/);
  const handles = read('../components/FramelessResizeHandles.jsx');
  for (const corner of ['top-left', 'top-right', 'bottom-left', 'bottom-right']) {
    assert.match(handles, new RegExp(`data-frameless-resize-corner="${corner}"`));
  }
  assert.match(read('../theme.jsx'), /pushWindowShape\(installedTheme\?\.id === colorTheme \? themeWindowShape\(installedTheme\) : null\)/);
});
