import { useEffect, useMemo, useRef, useState } from 'react';
import { api } from './api.js';
import { createThemeDownloadController } from './themePackages.js';
import { UPGRADE_CONFIG_CHANGED_EVENT } from './themeImports.js';

export function useThemeDownloads({ enabled, prepare, apply, remove, forget }) {
  const callbacks = useRef({ prepare, apply, remove, forget });
  callbacks.current = { prepare, apply, remove, forget };
  const [state, setState] = useState({ entry: null, localEntries: [], deletingId: '', loading: false, error: '', job: { state: 'idle' } });
  const controller = useMemo(() => createThemeDownloadController({
    api,
    prepare: (id, options) => callbacks.current.prepare(id, options),
    apply: (id, options) => callbacks.current.apply(id, options),
    remove: (id) => callbacks.current.remove(id),
    forget: (id) => callbacks.current.forget(id),
    onChange: setState,
  }), []);
  useEffect(() => {
    controller.activate();
    return () => controller.dispose();
  }, [controller]);
  useEffect(() => {
    if (!enabled) return undefined;
    const refresh = () => { void controller.refresh(); };
    const changed = () => { controller.invalidateCatalog(); refresh(); };
    refresh();
    window.addEventListener('focus', refresh);
    window.addEventListener(UPGRADE_CONFIG_CHANGED_EVENT, changed);
    return () => {
      window.removeEventListener('focus', refresh);
      window.removeEventListener(UPGRADE_CONFIG_CHANGED_EVENT, changed);
    };
  }, [controller, enabled]);
  return { ...state, controller };
}
