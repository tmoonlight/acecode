import { useEffect, useSyncExternalStore } from 'react';
import { api } from '../lib/api.js';
import { upgradeSettingsStore } from '../lib/upgradeSettings.js';

export function UpgradeSettings({ disabled = false }) {
  const store = upgradeSettingsStore(api);
  const state = useSyncExternalStore(store.subscribe, store.getSnapshot);
  useEffect(() => {
    void store.load();
    const flush = () => { void store.save(); };
    window.addEventListener('blur', flush);
    return () => { window.removeEventListener('blur', flush); flush(); };
  }, [store]);
  const unavailable = disabled || !state.draft || state.loading;
  return (
    <section className="ace-settings-group">
      <h3>升级服务</h3>
      <div className="ace-settings-row">
        <label className="ace-settings-label" htmlFor="upgrade-service-url">升级服务 URL</label>
        <div className="ace-settings-value">
          <input id="upgrade-service-url" value={state.draft?.base_url || ''} disabled={unavailable}
            onChange={(event) => store.update('base_url', event.target.value)}
            onBlur={() => { void store.save(); }}
            onKeyDown={(event) => { if (event.key === 'Enter') event.currentTarget.blur(); }} />
        </div>
      </div>
      <div className="ace-settings-row">
        <label className="ace-settings-label" htmlFor="upgrade-auto-update">自动更新</label>
        <button id="upgrade-auto-update" type="button" role="switch" aria-checked={state.draft?.auto_update === true}
          aria-label="自动更新" disabled={unavailable}
          onClick={() => { store.update('auto_update', !state.draft.auto_update); void store.save(); }}
          className={`relative w-9 h-5 shrink-0 rounded-full transition-colors focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-accent disabled:opacity-50 ${state.draft?.auto_update ? 'bg-accent' : 'bg-border'}`}>
          <span className={`absolute top-0.5 left-0.5 w-4 h-4 rounded-full bg-white transition-transform ${state.draft?.auto_update ? 'translate-x-4' : ''}`} />
        </button>
      </div>
      {state.error && <div role="alert" className="flex items-center gap-2 text-[12px] text-danger mt-2">
        <span>{state.error}</span>
        <button type="button" className="ace-settings-button" onClick={() => { void (state.draft ? store.save() : store.load()); }}>重试</button>
      </div>}
    </section>
  );
}
