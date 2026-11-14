import { api } from './api.js';

export function createInviteClipboardStore(bridge) {
  let state = { loaded: false, enabled: true, saving: false, error: '' };
  let saved = true;
  let initialized;
  let worker;
  const listeners = new Set();
  const publish = (patch) => { state = { ...state, ...patch }; listeners.forEach((listener) => listener()); };
  const parse = (raw) => typeof raw === 'string' ? JSON.parse(raw) : raw;
  const flush = () => {
    if (worker) return worker;
    if (state.enabled === saved) return Promise.resolve();
    worker = (async () => {
      publish({ saving: true, error: '' });
      try {
        while (state.enabled !== saved) {
          const value = state.enabled;
          const result = parse(await bridge.set(value));
          if (!result?.ok) throw new Error('save_failed');
          saved = value;
        }
      } catch { publish({ error: '保存失败，请重试' }); }
      finally { publish({ saving: false }); worker = null; }
    })();
    return worker;
  };
  return {
    getSnapshot: () => state,
    subscribe: (listener) => { listeners.add(listener); return () => listeners.delete(listener); },
    load: () => initialized ||= Promise.resolve().then(async () => {
      try { const result = parse(await bridge.get()); if (!result?.ok) throw new Error(); saved = result.enabled !== false; publish({ loaded: true, enabled: saved }); }
      catch { publish({ error: '无法读取设置' }); initialized = null; }
    }),
    set: (enabled) => { publish({ enabled: !!enabled }); return flush(); },
    retry: flush,
  };
}

export const inviteClipboardStore = createInviteClipboardStore({
  get: () => api.getInviteClipboardPreference(),
  set: async (enabled) => {
    const saved = await api.setInviteClipboardPreference(enabled);
    await window.aceDesktop_applyInviteClipboardPreference(enabled);
    return saved;
  },
});
