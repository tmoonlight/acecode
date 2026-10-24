import { useEffect, useState, useSyncExternalStore } from 'react';
import { api } from '../lib/api.js';
import { lookupErrorMessage } from '../lib/errors.js';
import { summaryGenerationSettingsStore } from '../lib/summaryGenerationSettings.js';
import { Toggle } from './Modal.jsx';
import { VsIcon } from './Icon.jsx';
import { toast } from './Toast.jsx';
import { ModelSettingsSection } from './model-settings/ModelSettingsSection.jsx';

export function SummaryGenerationSettings({ onCheckUpdates, onModelProfileUpdated }) {
  const store = summaryGenerationSettingsStore(api);
  const { snapshot, draft, loading, saving, error } = useSyncExternalStore(store.subscribe, store.getSnapshot);
  const [expanded, setExpanded] = useState(false);
  const [addingModel, setAddingModel] = useState(false);
  const hasModel = !!draft?.model_name && !!snapshot?.models.some((model) => model.name === draft.model_name);
  const errorMessage = error ? lookupErrorMessage(error.code,
    error.action === 'load' ? '加载摘要生成配置失败' : '保存摘要生成配置失败') : '';

  useEffect(() => {
    void store.load();
    const flush = () => { void store.flush(); };
    window.addEventListener('blur', flush);
    window.addEventListener('pagehide', flush);
    return () => {
      window.removeEventListener('blur', flush);
      window.removeEventListener('pagehide', flush);
      void store.flush().then((ok) => {
        if (!ok) toast({ kind: 'err', text: '保存摘要生成配置失败' });
      });
    };
  }, [store]);

  const update = (field, value) => {
    store.update(field, value);
    void store.flush();
  };

  return (
    <div className="bg-surface border border-border mb-2" aria-busy={loading || saving} data-summary-generation="true">
      <div className="flex items-center gap-3 px-3.5 py-3 border border-border">
        <div data-settings-surface="true" className="w-10 h-10 rounded-md bg-surface-alt border border-border flex items-center justify-center shrink-0 text-fg">
          <VsIcon name="document" size={20} />
        </div>
        <div className="flex-1 min-w-0">
          <div className="flex items-center gap-1.5">
            <div className="text-[13px] font-normal">摘要生成</div>
            <span className="ace-help-tip" tabIndex={0} aria-label="摘要生成帮助" aria-describedby="summary-generation-help">
              <VsIcon name="help" size={14} />
              <span id="summary-generation-help" role="tooltip" className="ace-help-tip-bubble">
                协助生成会话或者标题，记忆的摘要，建议使用本地模型或者轻量级模型。
              </span>
            </span>
          </div>
        </div>
        <button type="button" className="px-1.5 py-0.5 text-[11px] hover:underline disabled:opacity-50"
          disabled={!snapshot || loading} aria-expanded={expanded} aria-controls="summary-generation-config"
          onClick={() => setExpanded((value) => !value)}>{expanded ? '收起' : '配置'}</button>
        <Toggle on={!!draft?.enabled} disabled={!snapshot || loading} ariaLabel="启用摘要生成"
          onChange={(enabled) => {
            if (enabled && !hasModel) setExpanded(true);
            update('enabled', enabled);
          }} />
      </div>

      {error && <div role="alert" className="px-3.5 pb-3 text-[12px] text-danger">
        {errorMessage}
        {error.code === 'SUMMARY_SETTINGS_UNSUPPORTED' && onCheckUpdates &&
          <button type="button" onClick={onCheckUpdates} className="ml-2 text-accent hover:underline">检查更新</button>}
        {error.code !== 'SUMMARY_MODEL_REQUIRED' &&
          <button type="button" onClick={() => error.action === 'load' ? store.load() : store.flush()}
            disabled={loading || saving} className="ml-2 hover:underline">重试</button>}
      </div>}
      {draft?.model_name && !hasModel && !error &&
        <div role="status" className="px-3.5 pb-3 text-[12px] text-warn">所选摘要模型不可用，请重新选择模型</div>}

      {expanded && draft && <div id="summary-generation-config" className="border-t border-border px-3.5 py-3">
        <label htmlFor="summary-generation-model" className="block mb-1 text-[12px] text-fg-mute">摘要模型</label>
        <div className="flex flex-wrap items-center gap-2">
          <select id="summary-generation-model" value={draft.model_name} disabled={loading}
            aria-invalid={!!draft.enabled && !hasModel}
            className="min-w-0 flex-1 h-8 px-2 rounded-md text-[12px] border border-border bg-surface-alt text-fg outline-none focus:border-accent disabled:opacity-50"
            onChange={(event) => update('model_name', event.target.value)}>
            <option value="" disabled>选择摘要模型</option>
            {draft.model_name && !hasModel && <option value={draft.model_name} disabled>{draft.model_name} · 模型不可用</option>}
            {snapshot.models.map((model) => <option key={model.name} value={model.name}>{model.name}</option>)}
          </select>
          <button type="button" onClick={() => setAddingModel(true)} disabled={loading || saving}
            className="shrink-0 px-3 py-1.5 rounded-md text-[12px] border border-border bg-surface-alt hover:bg-surface-hi disabled:opacity-50">添加模型</button>
        </div>
      </div>}

      {addingModel && <ModelSettingsSection addOnly onClose={() => { setAddingModel(false); void store.load(); }}
        onModelProfileUpdated={(model) => {
          store.selectAddedModel(model);
          void store.flush();
          onModelProfileUpdated?.(model);
        }} />}
    </div>
  );
}
