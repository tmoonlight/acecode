// Shortcut detection and shared native-picker compatibility guards.

import assert from 'node:assert/strict';
import {
  isBlockedBrowserDefaultShortcut,
  isBrowserZoomShortcut,
  installBrowserDefaultGuards,
} from './browserDefaults.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('Ctrl+Plus zoom shortcut is blocked', () => {
  assert.equal(isBrowserZoomShortcut({ key: '+', code: 'Equal', ctrlKey: true }), true);
});

run('Ctrl+Minus zoom shortcut is blocked', () => {
  assert.equal(isBrowserZoomShortcut({ key: '-', code: 'Minus', ctrlKey: true }), true);
});

run('Ctrl+0 zoom reset shortcut is blocked', () => {
  assert.equal(isBrowserZoomShortcut({ key: '0', code: 'Digit0', ctrlKey: true }), true);
});

run('Meta+NumpadAdd zoom shortcut is blocked', () => {
  assert.equal(isBrowserZoomShortcut({ key: 'Add', code: 'NumpadAdd', metaKey: true }), true);
});

run('Ctrl+K remains available for app shortcuts', () => {
  assert.equal(isBrowserZoomShortcut({ key: 'k', code: 'KeyK', ctrlKey: true }), false);
});

run('Zoom keys without ctrl/meta are ignored', () => {
  assert.equal(isBrowserZoomShortcut({ key: '+', code: 'Equal' }), false);
});

run('Ctrl+F native find shortcut is blocked', () => {
  assert.equal(isBlockedBrowserDefaultShortcut({ key: 'f', ctrlKey: true }), true);
});

run('F3 native find shortcut is blocked', () => {
  assert.equal(isBlockedBrowserDefaultShortcut({ key: 'F3' }), true);
});

run('F5 page refresh shortcut is blocked', () => {
  assert.equal(isBlockedBrowserDefaultShortcut({ key: 'F5' }), true);
});

run('Ctrl+R page refresh shortcut remains available', () => {
  assert.equal(isBlockedBrowserDefaultShortcut({ key: 'r', ctrlKey: true }), false);
});

function pickerWindow({ shell = true, os = 'windows', supported = true } = {}) {
  const win = new EventTarget();
  const attributes = new Map();
  win.__ACECODE_DESKTOP_SHELL__ = shell;
  win.__ACECODE_OS__ = os;
  win.CSS = { supports: () => supported };
  win.document = { documentElement: {
    getAttribute: (name) => attributes.get(name) ?? null,
    setAttribute: (name, value) => attributes.set(name, value),
    removeAttribute: (name) => attributes.delete(name),
  } };
  win.getComputedStyle = (element) => ({ appearance: element.appearance });
  return win;
}

function pickerEscape(win, { open = true, appearance = 'base-select' } = {}) {
  const event = new Event('keydown', { cancelable: true });
  const select = { appearance, matches: () => open };
  Object.defineProperties(event, {
    key: { value: 'Escape' },
    target: { value: { closest: selector => selector === 'select' ? select : null } },
  });
  win.dispatchEvent(event);
  return event;
}

run('supported Windows Desktop opts in and restores its previous marker on cleanup', () => {
  const win = pickerWindow();
  const root = win.document.documentElement;
  root.setAttribute('data-ace-webview-selects', 'previous');
  const dispose = installBrowserDefaultGuards(win);
  assert.equal(root.getAttribute('data-ace-webview-selects'), 'true');
  dispose();
  assert.equal(root.getAttribute('data-ace-webview-selects'), 'previous');
});

run('open rendered picker consumes Escape propagation without cancelling browser dismissal', () => {
  const win = pickerWindow();
  const dispose = installBrowserDefaultGuards(win);
  let dialogDismissals = 0;
  win.addEventListener('keydown', () => { dialogDismissals += 1; });
  assert.equal(pickerEscape(win).defaultPrevented, false);
  assert.equal(dialogDismissals, 0);
  pickerEscape(win, { open: false });
  pickerEscape(win, { appearance: 'auto' });
  assert.equal(dialogDismissals, 2);
  dispose();
  assert.equal(win.document.documentElement.getAttribute('data-ace-webview-selects'), null);
  pickerEscape(win);
  assert.equal(dialogDismissals, 3);
});

run('browser, non-Windows shells, and unsupported engines retain native picker behavior', () => {
  for (const options of [{ shell: false }, { os: 'macos' }, { supported: false }]) {
    const win = pickerWindow(options);
    const dispose = installBrowserDefaultGuards(win);
    assert.equal(win.document.documentElement.getAttribute('data-ace-webview-selects'), null);
    let handled = false;
    win.addEventListener('keydown', () => { handled = true; });
    pickerEscape(win);
    assert.equal(handled, true);
    dispose();
  }
});

function ctrlWheel(win, { ctrlKey = true } = {}) {
  const event = new Event('wheel', { cancelable: true });
  Object.defineProperties(event, { ctrlKey: { value: ctrlKey }, deltaY: { value: -100 } });
  win.dispatchEvent(event);
  return event;
}

// 场景:页面上没有别的控件处理的 Ctrl+滚轮(桌面版页面缩放)。
// 期望:拦下浏览器默认缩放,并交给 onZoomWheel 换算页面缩放比例。
run('unconsumed Ctrl+wheel is blocked and handed to page zoom', () => {
  const win = pickerWindow();
  const zoomed = [];
  const dispose = installBrowserDefaultGuards(win, { onZoomWheel: event => zoomed.push(event) });
  const event = ctrlWheel(win);
  assert.equal(event.defaultPrevented, true);
  assert.deepEqual(zoomed, [event]);
  dispose();
  ctrlWheel(win);
  assert.equal(zoomed.length, 1);
});

// 场景:图片 / 文档预览先用 Ctrl+滚轮缩放了自己的内容(在自身上 preventDefault)。
// 期望:页面缩放不再跟着动。
// 回归:改动前守卫挂在 capture 阶段先 preventDefault,代码预览按 defaultPrevented 判断,自己的缩放永远不触发。
run('Ctrl+wheel consumed by a preview does not zoom the page', () => {
  const win = pickerWindow();
  const zoomed = [];
  win.addEventListener('wheel', event => event.preventDefault(), { capture: true });
  const dispose = installBrowserDefaultGuards(win, { onZoomWheel: event => zoomed.push(event) });
  ctrlWheel(win);
  assert.equal(zoomed.length, 0);
  dispose();
});

// 场景:不按 Ctrl 的普通滚动,以及浏览器直连时没有页面缩放处理。
// 期望:普通滚动完全不拦;没有 onZoomWheel 时 Ctrl+滚轮仍被拦下(与改动前一样不触发浏览器缩放)。
run('plain wheel scrolls and Ctrl+wheel stays blocked without page zoom', () => {
  const win = pickerWindow({ shell: false });
  const dispose = installBrowserDefaultGuards(win);
  assert.equal(ctrlWheel(win, { ctrlKey: false }).defaultPrevented, false);
  assert.equal(ctrlWheel(win).defaultPrevented, true);
  dispose();
});
