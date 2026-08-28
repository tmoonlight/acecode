import { useEffect, useMemo, useState } from 'react';
import { clsx } from '../../lib/format.js';
import { formatModelTokenLimit } from '../../lib/modelSettings.js';
import { splitModelIds } from '../../lib/modelManager.js';
import { Modal } from '../Modal.jsx';
import { VsIcon } from '../Icon.jsx';

export function ModelProbeDialog({
  models,
  status,
  error,
  initialModelIds,
  allowMultiple,
  onConfirm,
  onClose,
}) {
  const [query, setQuery] = useState('');
  const [selectedModelIds, setSelectedModelIds] = useState([]);

  useEffect(() => {
    if (status !== 'ready') return;
    const availableIds = new Set(models.map((model) => model.id));
    const matchedIds = splitModelIds(initialModelIds)
      .filter((modelId) => availableIds.has(modelId));
    setSelectedModelIds(allowMultiple ? matchedIds : matchedIds.slice(0, 1));
  }, [allowMultiple, initialModelIds, models, status]);

  const displayedModels = useMemo(() => {
    const normalizedQuery = query.trim().toLowerCase();
    if (!normalizedQuery) return models;
    return models.filter((model) => (
      model.id.toLowerCase().includes(normalizedQuery)
        || String(model.name || '').toLowerCase().includes(normalizedQuery)
    ));
  }, [models, query]);

  const toggleModel = (modelId) => {
    setSelectedModelIds((current) => {
      if (!allowMultiple) return [modelId];
      return current.includes(modelId)
        ? current.filter((id) => id !== modelId)
        : [...current, modelId];
    });
  };

  const confirmSelection = () => {
    if (status !== 'ready' || selectedModelIds.length === 0) return;
    onConfirm?.(selectedModelIds);
  };

  const blockEscapeDismiss = (event) => {
    if (event.key !== 'Escape') return;
    event.preventDefault();
    event.stopPropagation();
    event.nativeEvent?.stopImmediatePropagation?.();
  };

  return (
    <Modal
      onClose={onClose}
      width="min(560px, calc(100vw - 32px))"
      dismissOnBackdrop={false}
      dismissOnEscape={false}
      layerClassName="z-[330]"
      labelledBy="model-probe-dialog-title"
    >
      <div
        className="flex max-h-[min(640px,calc(100vh-32px))] flex-col"
        aria-busy={status === 'loading'}
        onKeyDown={blockEscapeDismiss}
      >
        <header className="flex shrink-0 items-start gap-3 border-b border-border px-5 py-4">
          <div className="min-w-0 flex-1">
            <h2 id="model-probe-dialog-title" className="text-[15px] font-semibold text-fg">
              选择探测到的模型
            </h2>
            <p className="mt-1 text-[11px] leading-5 text-fg-mute">
              {allowMultiple
                ? '可选择多个模型，确认后一次性回填到新增模型表单。'
                : '请选择一个模型，确认后替换当前模型 ID。'}
            </p>
          </div>
          <button
            type="button"
            onClick={onClose}
            className="flex h-8 w-8 shrink-0 items-center justify-center rounded-md text-fg-mute transition hover:bg-surface-hi hover:text-fg focus:outline-none focus:ring-1 focus:ring-accent"
            aria-label="取消模型探测选择"
          >
            <VsIcon name="close" size={14} />
          </button>
        </header>

        <div className="min-h-0 flex-1 overflow-y-auto px-5 py-4">
          <label className="relative block">
            <span className="sr-only">搜索探测到的模型</span>
            <VsIcon
              name="search"
              size={13}
              className="pointer-events-none absolute left-3 top-1/2 -translate-y-1/2 text-fg-mute"
            />
            <input
              type="search"
              value={query}
              onChange={(event) => setQuery(event.target.value)}
              placeholder="搜索模型 ID"
              disabled={status !== 'ready' || models.length === 0}
              className="h-9 w-full rounded-md border border-border bg-surface pl-9 pr-3 text-[12px] text-fg outline-none transition placeholder:text-fg-mute focus:border-accent focus:ring-1 focus:ring-accent-soft disabled:opacity-50"
            />
          </label>

          <div
            className="mt-3 max-h-[360px] overflow-y-auto rounded-md border border-border bg-surface"
            role={allowMultiple ? 'group' : 'radiogroup'}
            aria-label="探测到的模型列表"
            aria-busy={status === 'loading'}
          >
            {status === 'loading' && (
              <div className="flex items-center justify-center gap-2 px-4 py-10 text-[11px] text-fg-mute">
                <span className="ace-spinner" />
                正在探测模型…
              </div>
            )}
            {status === 'error' && (
              <div role="alert" className="px-4 py-6 text-center text-[11px] leading-5 text-danger">
                {error || 'Provider 探测失败'}
              </div>
            )}
            {status === 'ready' && models.length === 0 && (
              <div className="px-4 py-10 text-center text-[11px] text-fg-mute">
                当前 Provider 没有返回可用模型。
              </div>
            )}
            {status === 'ready' && models.length > 0 && displayedModels.length === 0 && (
              <div className="px-4 py-10 text-center text-[11px] text-fg-mute">
                没有匹配的模型。
              </div>
            )}
            {status === 'ready' && displayedModels.map((model, index) => {
              const selected = selectedModelIds.includes(model.id);
              const contextWindow = formatModelTokenLimit(model.context_window);
              return (
                <label
                  key={model.id}
                  className={clsx(
                    'flex w-full cursor-pointer items-center gap-2.5 px-3.5 py-2.5 text-left transition focus-within:ring-1 focus-within:ring-inset focus-within:ring-accent',
                    index > 0 && 'border-t border-border',
                    selected ? 'bg-accent-bg' : 'hover:bg-surface-hi',
                  )}
                >
                  <input
                    type={allowMultiple ? 'checkbox' : 'radio'}
                    name={allowMultiple ? undefined : 'model-probe-selection'}
                    checked={selected}
                    onChange={() => toggleModel(model.id)}
                    className="h-[17px] w-[17px] shrink-0 accent-accent"
                  />
                  <span className="min-w-0 flex-1">
                    <span className="block truncate text-[11px] font-medium text-fg">
                      {model.name || model.id}
                    </span>
                    <span className="block truncate text-[10px] text-fg-mute">{model.id}</span>
                  </span>
                  {contextWindow && (
                    <span className="shrink-0 text-[10px] text-fg-mute">
                      上下文 {contextWindow}
                    </span>
                  )}
                </label>
              );
            })}
          </div>
        </div>

        <footer className="flex shrink-0 items-center justify-between gap-3 border-t border-border bg-surface-alt px-5 py-3">
          <span className="text-[11px] text-fg-mute" aria-live="polite">
            已选择 {selectedModelIds.length} 个模型
          </span>
          <button
            type="button"
            onClick={confirmSelection}
            disabled={status !== 'ready' || selectedModelIds.length === 0}
            className="h-8 rounded-md bg-accent px-4 text-[11px] font-semibold text-white transition hover:opacity-90 focus:outline-none focus:ring-2 focus:ring-accent-soft disabled:cursor-not-allowed disabled:opacity-50"
          >
            添加所选模型
          </button>
        </footer>
      </div>
    </Modal>
  );
}
