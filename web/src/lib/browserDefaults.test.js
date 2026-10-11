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
