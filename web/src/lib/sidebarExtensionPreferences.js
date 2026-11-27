import {
  normalizeSidebarExtensionPrefs,
  validateSidebarExtensionPrefs,
} from './sidebarNavigation.js';
import { readWithFallback, writeSafely } from './usePreference.js';

export const SIDEBAR_EXTENSION_PREFS_STORAGE_KEY = 'acecode.sidebarExtensions.v1';

export function createSidebarExtensionPreferences({ storage, desktopApi }) {
  const key = SIDEBAR_EXTENSION_PREFS_STORAGE_KEY;
  const cached = readWithFallback(key, null, validateSidebarExtensionPrefs, storage);
  let prefs = normalizeSidebarExtensionPrefs(cached);
  let restoration = null;
  let pending = null;
  let saving = null;
  const listeners = new Set();

  function publish(next) {
    prefs = next;
    writeSafely(key, prefs, storage);
    for (const listener of listeners) listener();
  }

  async function restoreDesktop() {
    let saved = await desktopApi.getSidebarExtensions();
    if (saved === null && cached) {
      saved = await desktopApi.setSidebarExtensions(prefs, { ifAbsent: true });
    }
    if (saved === null) return;
    if (!validateSidebarExtensionPrefs(saved)) throw new Error('Invalid sidebar extension preferences');
    publish(normalizeSidebarExtensionPrefs(saved));
  }

  function restore() {
    if (!desktopApi) return Promise.resolve();
    if (!restoration) {
      restoration = restoreDesktop().catch((error) => {
        restoration = null;
        throw error;
      });
    }
    return restoration;
  }

  async function savePending() {
    while (pending) {
      const target = pending;
      const saved = await desktopApi.setSidebarExtensions(target);
      if (!validateSidebarExtensionPrefs(saved)) throw new Error('Invalid sidebar extension preferences');
      if (pending === target) pending = null;
    }
  }

  function flush() {
    if (!saving && pending) {
      saving = savePending().finally(() => { saving = null; });
    }
    return saving || Promise.resolve();
  }

  return {
    getSnapshot: () => prefs,
    subscribe(listener) {
      listeners.add(listener);
      return () => listeners.delete(listener);
    },
    restore,
    change(next) {
      publish(next);
      if (desktopApi) {
        pending = next;
        // “完成”会等待队列并重试尚未成功的快照。
        void flush().catch(() => {});
      }
    },
    flush,
  };
}
