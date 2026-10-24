import { apiConnectionScope } from './api.js';
import { UPGRADE_CONFIG_CHANGED_EVENT } from './themeImports.js';

const stores = new WeakMap();
const normalize = (value) => ({ base_url: value?.base_url || '', auto_update: value?.auto_update === true });

export function upgradeSettingsStore(client) {
  const scope = apiConnectionScope(client);
  if (stores.has(scope)) return stores.get(scope);
  let state = { snapshot: null, draft: null, loading: false, saving: false, error: '' };
  let reading;
  let writing;
  let requested = false;
  const listeners = new Set();
  const publish = (patch) => { state = { ...state, ...patch }; listeners.forEach((fn) => fn()); };
  const dirty = () => state.draft && Object.keys(state.draft).some((key) => state.draft[key] !== state.snapshot[key]);
  const requireConnection = () => {
    if (apiConnectionScope(client) !== scope) throw new Error('更新设置连接已变更');
  };
  const store = {
    getSnapshot: () => state,
    subscribe(fn) { listeners.add(fn); return () => listeners.delete(fn); },
    update(key, value) {
      if (state.draft) publish({ draft: { ...state.draft, [key]: value }, error: '' });
    },
    load() {
      if (reading) return reading;
      const pendingWrite = writing;
      reading = Promise.resolve().then(async () => {
        if (pendingWrite) await pendingWrite;
        if (dirty()) return;
        publish({ loading: true, error: '' });
        try {
          requireConnection();
          const snapshot = normalize(await client.getUpgradeConfig());
          requireConnection();
          publish({ snapshot, draft: snapshot });
        } catch (error) { publish({ error: error?.message || '读取更新设置失败' }); }
      }).finally(() => { reading = null; publish({ loading: false }); });
      return reading;
    },
    save() {
      requested = true;
      if (writing) return writing;
      const pendingRead = reading;
      writing = Promise.resolve().then(async () => {
        if (pendingRead) await pendingRead;
        while (requested) {
          requested = false;
          if (!dirty()) continue;
          const submitted = state.draft;
          const patch = Object.fromEntries(Object.entries(submitted)
            .filter(([key, value]) => value !== state.snapshot[key]));
          if ('base_url' in patch) {
            patch.base_url = patch.base_url.trim();
            if (!/^https?:\/\//i.test(patch.base_url)) {
              publish({ error: '升级服务 URL 必须使用 http 或 https' });
              return false;
            }
          }
          publish({ saving: true, error: '' });
          try {
            requireConnection();
            const snapshot = normalize(await client.setUpgradeConfig(patch));
            requireConnection();
            const draft = { ...snapshot };
            for (const key of Object.keys(draft)) {
              if (state.draft[key] !== submitted[key]) draft[key] = state.draft[key];
            }
            publish({ snapshot, draft });
            if (typeof window !== 'undefined') window.dispatchEvent(new Event(UPGRADE_CONFIG_CHANGED_EVENT));
          } catch (error) {
            publish({ error: error?.message || '保存更新设置失败' });
            return false;
          }
        }
        return true;
      }).finally(() => { writing = null; publish({ saving: false }); });
      return writing;
    },
  };
  stores.set(scope, store);
  return store;
}
