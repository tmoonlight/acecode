import { useEffect, useSyncExternalStore } from 'react';
import { api } from '../lib/api.js';
import { lookupErrorMessage } from '../lib/errors.js';
import { autoReviewModelOptions, autoReviewSettingsStore } from '../lib/autoReviewSettings.js';
import { VsIcon } from './Icon.jsx';

// 设置 > 工具 里「自动模式（试验）」的卡片:只有一个下拉,选裁决模型。
// 第一项「跟随当前模型」= 用会话当前的模型裁决。选了就存。
export function AutoReviewSettings({ onCheckUpdates }) {
  const store = autoReviewSettingsStore(api);
  const { snapshot, modelName, loading, saving, error } = useSyncExternalStore(store.subscribe, store.getSnapshot);
  const options = autoReviewModelOptions(snapshot, modelName);
  const errorMessage = error ? lookupErrorMessage(error.code,
    error.action === 'load' ? '加载自动模式配置失败' : '保存自动模式配置失败') : '';

  useEffect(() => { void store.load(); }, [store]);

  return (
    <div className="bg-surface border border-border mb-2" aria-busy={loading || saving} data-auto-review-settings="true">
      <div className="flex items-center gap-3 px-3.5 py-3 border border-border">
        <div data-settings-surface="true" className="w-10 h-10 rounded-md bg-surface-alt border border-border flex items-center justify-center shrink-0 text-fg">
          <VsIcon name="ShieldWarning" size={20} />
        </div>
        <label htmlFor="auto-review-model" className="flex-1 min-w-0 text-[13px] font-normal">自动模式（试验）</label>
        <select id="auto-review-model" value={modelName} disabled={!snapshot || loading}
          className="w-56 max-w-[50%] h-8 px-2 rounded-md text-[12px] border border-border bg-surface-alt text-fg outline-none focus:border-accent disabled:opacity-50"
          onChange={(event) => { void store.choose(event.target.value); }}>
          {options.map((option) => (
            <option key={option.value} value={option.value} disabled={option.disabled}>{option.label}</option>
          ))}
        </select>
      </div>

      {error && <div role="alert" className="px-3.5 py-2 text-[12px] text-danger">
        {errorMessage}
        {error.code === 'AUTO_REVIEW_SETTINGS_UNSUPPORTED' && onCheckUpdates &&
          <button type="button" onClick={onCheckUpdates} className="ml-2 text-accent hover:underline">检查更新</button>}
        {error.action === 'load' &&
          <button type="button" onClick={() => store.load()} disabled={loading} className="ml-2 hover:underline">重试</button>}
      </div>}
    </div>
  );
}
