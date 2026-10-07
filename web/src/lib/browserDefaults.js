// WebView/Chromium defaults that feel like browser chrome rather than app UI.
// Keep these guards at the document edge so individual controls can stay simple.

import { isFindShortcut } from './globalFind.js';
import { isDesktopShell } from './desktopShellMode.js';
import { APP_SHORTCUT_BROWSER_GUARD, matchAppShortcut } from './appShortcuts.js';

const ZOOM_SHORTCUT_KEYS = new Set(['+', '=', '-', '_', '0']);
const ZOOM_SHORTCUT_CODES = new Set([
  'Equal',
  'Minus',
  'Digit0',
  'NumpadAdd',
  'NumpadSubtract',
  'Numpad0',
]);

function hasZoomModifier(event) {
  return !!(event && (event.ctrlKey || event.metaKey));
}

export function isBrowserZoomShortcut(event) {
  if (!hasZoomModifier(event)) return false;
  const key = typeof event.key === 'string' ? event.key : '';
  const code = typeof event.code === 'string' ? event.code : '';
  return ZOOM_SHORTCUT_KEYS.has(key) || ZOOM_SHORTCUT_CODES.has(code);
}

export function isBlockedBrowserDefaultShortcut(event) {
  return event?.key === 'F5' || isBrowserZoomShortcut(event) || isFindShortcut(event);
}

export function installBrowserDefaultGuards(target = globalThis.window) {
  if (!target || typeof target.addEventListener !== 'function') {
    return () => {};
  }

  const root = target.document?.documentElement;
  const renderedSelects = !!(root && isDesktopShell(target)
    && target.__ACECODE_OS__ === 'windows'
    && target.CSS?.supports?.('appearance', 'base-select')
    && target.CSS?.supports?.('selector(::picker(select))'));
  const previousSelectFlag = root?.getAttribute('data-ace-webview-selects');
  if (renderedSelects) root.setAttribute('data-ace-webview-selects', 'true');

  const prevent = (event) => event.preventDefault();
  const onWheel = (event) => {
    if (hasZoomModifier(event)) event.preventDefault();
  };
  const onKeyDown = (event) => {
    if (renderedSelects && event.key === 'Escape') {
      const select = event.target?.closest?.('select');
      if (select?.matches(':open') && target.getComputedStyle(select).appearance === 'base-select') {
        // Let the browser dismiss its picker without closing the parent modal.
        event.stopImmediatePropagation();
        return;
      }
    }
    // These two zoom-shaped chords belong to ACECode history navigation.
    const shortcut = matchAppShortcut(event);
    if (shortcut === 'forward' || shortcut === 'back') {
      event.preventDefault();
      event[APP_SHORTCUT_BROWSER_GUARD] = true;
      return;
    }
    if (isBlockedBrowserDefaultShortcut(event)) event.preventDefault();
  };

  const activeCapture = { capture: true, passive: false };
  target.addEventListener('wheel', onWheel, activeCapture);
  target.addEventListener('keydown', onKeyDown, activeCapture);
  target.addEventListener('gesturestart', prevent, activeCapture);
  target.addEventListener('gesturechange', prevent, activeCapture);
  target.addEventListener('gestureend', prevent, activeCapture);

  return () => {
    if (renderedSelects) {
      if (previousSelectFlag == null) root.removeAttribute('data-ace-webview-selects');
      else root.setAttribute('data-ace-webview-selects', previousSelectFlag);
    }
    target.removeEventListener('wheel', onWheel, activeCapture);
    target.removeEventListener('keydown', onKeyDown, activeCapture);
    target.removeEventListener('gesturestart', prevent, activeCapture);
    target.removeEventListener('gesturechange', prevent, activeCapture);
    target.removeEventListener('gestureend', prevent, activeCapture);
  };
}
