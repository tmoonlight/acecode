import { useEffect, useSyncExternalStore } from 'react';
import { api } from '../lib/api.js';
import { lookupErrorMessage } from '../lib/errors.js';
import {
  toolRewriteRows,
  toolRewritesStore,
  validateToolRewriteDraft,
} from '../lib/toolRewrites.js';
import { Toggle } from './Modal.jsx';
import { VsIcon } from './Icon.jsx';
import { toast } from './Toast.jsx';

const inputClass = 'w-full h-7 px-2 text-[12px] font-mono rounded-md border border-border bg-surface-alt text-fg outline-none focus:border-accent transition disabled:opacity-50';

// 标题右侧的「圈圈包问号」:hover / 键盘聚焦时弹出提示气泡(样式在 globals.css .ace-help-tip)。
function HelpTip({ children }) {
  return (
    <span className="ace-help-tip" tabIndex={0}>
      <VsIcon name="help" size={14} alt="帮助" />
      <span role="tooltip" className="ace-help-tip-bubble">{children}</span>
    </span>
  );
}

export function ToolRewriteSettings({ onCheckUpdates }) {
  const store = toolRewritesStore(api);
  const { snapshot, draft, loading, saving, error } = useSyncExternalStore(store.subscribe, store.getSnapshot);
  const rows = toolRewriteRows(snapshot, draft);
  const errors = validateToolRewriteDraft(draft, snapshot);
  const errorMessage = error ? lookupErrorMessage(error.code,
    error.action === 'load' ? '加载工具重写配置失败' : '保存工具重写配置失败') : '';

  useEffect(() => {
    void store.load();
    const flush = () => { void store.flush(); };
    window.addEventListener('blur', flush);
    window.addEventListener('pagehide', flush);
    return () => {
      window.removeEventListener('blur', flush);
      window.removeEventListener('pagehide', flush);
      void store.flush().then((ok) => {
        if (!ok) toast({ kind: 'err', text: '保存工具重写配置失败' });
      });
    };
  }, [store]);

  const saveOnBlur = () => { void store.flush(); };

  return (
    <>
      <div className="flex items-start justify-between gap-3 px-3.5 py-2.5 rounded-md bg-surface border border-border mb-2"
        aria-busy={loading || saving}>
        <div className="min-w-0">
          <div className="flex items-center gap-1.5">
            <div className="text-[14px] font-normal">工具重写</div>
            <HelpTip>某些特殊审计场景，您可能需要重写工具，请确保您知道您在做什么才勾选此选项。</HelpTip>
          </div>
        </div>
        <Toggle on={!!draft?.enabled} disabled={!snapshot || loading} ariaLabel="启用工具重写"
          onChange={(enabled) => { store.setEnabled(enabled); void store.flush(); }} />
      </div>

      {error && <div role="alert" className="mb-2 text-[12px] text-danger">
        {errorMessage}
        {error.code === 'TOOL_REWRITES_UNSUPPORTED' && onCheckUpdates &&
          <button type="button" onClick={onCheckUpdates} className="ml-2 text-accent hover:underline">检查更新</button>}
        <button type="button" onClick={() => error.action === 'load' ? store.load() : store.flush()}
          disabled={loading || saving} className="ml-2 hover:underline">重试</button>
      </div>}

      {snapshot?.warning && !error && <div role="status" className="mb-2 text-[12px] text-warn">
        配置文件无法解析，已按默认值显示：{snapshot.warning}
      </div>}

      {draft?.enabled && snapshot && (
        <div className="rounded-md bg-surface border border-border mb-2 overflow-hidden" data-testid="tool-rewrite-table">
          <div className="grid grid-cols-[minmax(0,1fr)_14rem] gap-3 px-3.5 py-2 text-[11px] font-normal text-fg-mute bg-surface-alt">
            <div>内置工具</div>
            <div>重写为</div>
          </div>
          {rows.map((row) => (
            <div key={row.name} className="grid grid-cols-[minmax(0,1fr)_14rem] items-center gap-3 px-3.5 py-2 border-t border-border">
              <div className="min-w-0">
                <div className="flex items-center gap-2 text-[13px] font-normal">
                  <span className="font-mono">{row.name}</span>
                  {!row.registered && <span className="text-[10px] px-1.5 py-0.5 rounded-full border border-border bg-surface-alt text-fg-mute">未注册</span>}
                </div>
                {row.description && <div className="text-[11px] text-fg-mute mt-0.5 truncate" title={row.description}>{row.description}</div>}
              </div>
              <div>
                <input className={inputClass} type="text" value={row.value} placeholder={row.name}
                  aria-label={row.name} aria-invalid={!!errors[row.name]} autoComplete="off" spellCheck={false}
                  disabled={loading}
                  onChange={(e) => store.setRewrite(row.name, e.target.value)} onBlur={saveOnBlur}
                  onKeyDown={(e) => { if (e.key === 'Enter') { e.preventDefault(); saveOnBlur(); } }} />
                {errors[row.name] && <div className="text-[11px] text-danger mt-0.5">{errors[row.name]}</div>}
              </div>
            </div>
          ))}
          <div className="flex items-center justify-between gap-3 px-3.5 py-2 border-t border-border text-[11px] text-fg-mute">
            <span className="min-w-0 truncate" title={snapshot.path}>文件位置：{snapshot.path}</span>
            <span className="flex items-center gap-2 shrink-0">
              <span>留空表示不重写</span>
              <button type="button" className="px-1.5 py-0.5 text-[11px] hover:underline disabled:opacity-50"
                disabled={loading || saving}
                onClick={() => { store.resetToDefaults(); void store.flush(); }}>恢复默认</button>
            </span>
          </div>
        </div>
      )}
    </>
  );
}
