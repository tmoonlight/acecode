import { normalizeDesktopPetKind } from './desktopPetKinds.js';

export const OFFICE_PREFERENCES_EVENT = 'ace-desktop-office-preferences';

export function parseOfficePreferences(value) {
  const result = typeof value === 'string' ? JSON.parse(value) : value;
  if (!result || typeof result.available !== 'boolean' || typeof result.enabled !== 'boolean') {
    throw new Error('Invalid office preferences');
  }
  return { ok: result.ok, available: result.available, enabled: result.enabled,
    petKind: normalizeDesktopPetKind(result.petKind), error: result.error };
}

// One owner for the menu, settings and native close notifications.
export function createDesktopOfficePreferences(win) {
  let state = { ready: false, available: false, enabled: false, petKind: 'office',
    busy: false, error: '' };
  let revision = 0;
  let disposed = false;
  const listeners = new Set();
  const update = patch => {
    if (disposed) return;
    state = { ...state, ...patch };
    for (const listener of listeners) listener();
  };
  const accept = result => update({ ...result, ready: true,
    error: result.ok === false ? '无法更新桌面宠物，请重试。' : '' });
  const onChange = event => {
    try { const value = parseOfficePreferences(event.detail); revision++; accept(value); } catch {}
  };
  async function request(method, ...args) {
    const before = revision;
    try {
      const result = parseOfficePreferences(await win[method](...args));
      // A later native close must win over an older bridge response.
      if (revision === before) accept(result);
      return result;
    } catch {
      if (revision === before) update({ ready: true, error: '无法更新桌面宠物，请重试。' });
      return { ok: false };
    }
  }
  return {
    getSnapshot: () => state,
    subscribe(listener) { listeners.add(listener); return () => listeners.delete(listener); },
    start() {
      disposed = false;
      if (typeof win?.aceDesktop_getOfficePreferences !== 'function') {
        update({ ready: true });
        return;
      }
      win.addEventListener(OFFICE_PREFERENCES_EVENT, onChange);
      void request('aceDesktop_getOfficePreferences');
    },
    dispose() {
      disposed = true;
      revision++;
      win?.removeEventListener(OFFICE_PREFERENCES_EVENT, onChange);
    },
    async setEnabled(enabled) {
      if (!state.available || state.busy) return { ok: false };
      update({ busy: true, error: '' });
      const result = await request('aceDesktop_setOfficeEnabled', !!enabled);
      update({ busy: false });
      return result;
    },
    async setPetKind(kind) {
      if (!state.available || state.busy || normalizeDesktopPetKind(kind) !== kind) return { ok: false };
      update({ busy: true, error: '' });
      const result = await request('aceDesktop_setPetKind', kind);
      update({ busy: false });
      return result;
    },
  };
}
