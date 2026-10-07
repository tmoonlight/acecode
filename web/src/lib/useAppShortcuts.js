import { useEffect, useRef } from 'react';
import { APP_SHORTCUT_BROWSER_GUARD, appShortcutContextAllows, matchAppShortcut } from './appShortcuts.js';

// Actions stay with their React owner; one event is consumed at most once.
export function useAppShortcuts(handlers) {
  const latest = useRef(handlers);
  latest.current = handlers;
  useEffect(() => {
    let composing = false;
    let settleTimer;
    const start = () => { clearTimeout(settleTimer); composing = true; };
    const end = () => { settleTimer = setTimeout(() => { composing = false; }, 0); };
    const onKey = (event) => {
      if ((event.defaultPrevented && !event[APP_SHORTCUT_BROWSER_GUARD]) || composing) return;
      const id = matchAppShortcut(event);
      const handler = latest.current[id];
      if (!handler || !appShortcutContextAllows(id, event.target)) return;
      if (event.repeat) { event.preventDefault(); return; }
      if (handler(event) === false) return;
      event.preventDefault();
      event.stopImmediatePropagation();
    };
    window.addEventListener('compositionstart', start, true);
    window.addEventListener('compositionend', end, true);
    window.addEventListener('keydown', onKey);
    return () => {
      clearTimeout(settleTimer);
      window.removeEventListener('compositionstart', start, true);
      window.removeEventListener('compositionend', end, true);
      window.removeEventListener('keydown', onKey);
    };
  }, []);
}
