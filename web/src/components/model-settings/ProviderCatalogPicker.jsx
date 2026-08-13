import { useEffect, useMemo, useState } from 'react';
import { clsx } from '../../lib/format.js';
import {
  addManualModelToDraft,
  formatModelTokenLimit,
  normalizeProviderModelQuery,
  redactModelDraftSecrets,
  toggleCatalogModelInDraft,
} from '../../lib/modelSettings.js';
import { lookupErrorMessage } from '../../lib/errors.js';
import {
  groupCatalogProviders,
  providerDisplayName,
  PROVIDER_GROUP_LABELS,
} from '../../lib/providerCatalogGroups.js';
import {
  normalizeModelProbeResult,
  parseRequestHeadersJson,
  splitModelIds,
} from '../../lib/modelManager.js';
import { openExternalUrl } from '../../lib/externalUrl.js';
import { RefreshIcon, VsIcon } from '../Icon.jsx';
import { ModelConnectionCard } from './ModelConnectionCard.jsx';
import { ProviderIcon } from './ProviderIcon.jsx';

function mergeCatalogModels(primary, exact) {
  const seen = new Set();
  return [...primary, ...exact].filter((model) => {
    if (!model?.id || seen.has(model.id)) return false;
    seen.add(model.id);
    return true;
  });
}

function modelMetadataSummary(model) {
  const parts = [];
  const context = formatModelTokenLimit(model?.context_window);
  const output = formatModelTokenLimit(model?.max_output_tokens);
  if (context) parts.push(`上下文 ${context}`);
  if (output) parts.push(`最大输出 ${output}`);
  if (model?.capabilities?.length) parts.push(`能力 ${model.capabilities.join(' · ')}`);
  return parts.join(' · ');
}

export function ProviderCatalogPicker({
  apiClient,
  providers,
  provider,
  draft,
  allowMultiple,
  managedAuthenticated,
  managedConnection,
  onProviderChange,
  onDraftChange,
}) {
  const [providerQuery, setProviderQuery] = useState('');
  const [modelQuery, setModelQuery] = useState('');
  const [catalogModels, setCatalogModels] = useState([]);
  const [catalogStatus, setCatalogStatus] = useState('idle');
  const [catalogError, setCatalogError] = useState('');
  const [probeStatus, setProbeStatus] = useState('idle');
  const [probeError, setProbeError] = useState('');
  const [manualModel, setManualModel] = useState('');
  const selectedModels = splitModelIds(draft.model);

  const groupedProviders = useMemo(
    () => groupCatalogProviders(providers, providerQuery),
    [providerQuery, providers],
  );

  useEffect(() => {
    setModelQuery('');
    setCatalogModels([]);
    setCatalogStatus('idle');
    setCatalogError('');
    setProbeStatus('idle');
    setProbeError('');
  }, [provider?.id]);

  useEffect(() => {
    if (!provider || provider.model_input !== 'catalog') return undefined;
    let cancelled = false;
    const timer = window.setTimeout(async () => {
      setCatalogStatus('loading');
      setCatalogError('');
      try {
        const currentId = splitModelIds(draft.model)[0] || '';
        const response = await apiClient.queryModelCatalog(provider.id, modelQuery, 50);
        const normalized = normalizeProviderModelQuery(response, provider.id);
        let exactModels = [];
        if (!modelQuery.trim() && currentId
            && !normalized.models.some((model) => model.id === currentId)) {
          const exactResponse = await apiClient.queryModelCatalog(provider.id, currentId, 1);
          exactModels = normalizeProviderModelQuery(exactResponse, provider.id).models;
        }
        if (cancelled) return;
        setCatalogModels(mergeCatalogModels(normalized.models, exactModels));
        setCatalogStatus('ready');
      } catch (error) {
        if (cancelled) return;
        setCatalogModels([]);
        setCatalogError(error?.message || '目录查询失败');
        setCatalogStatus('error');
      }
    }, 180);
    return () => {
      cancelled = true;
      window.clearTimeout(timer);
    };
  }, [apiClient, draft.model, modelQuery, provider]);

  const updateSelectedModels = (modelId, metadata = null) => {
    onDraftChange(toggleCatalogModelInDraft(
      draft,
      modelId,
      metadata,
      { allowMultiple },
    ));
  };

  const addManualModel = () => {
    const modelId = manualModel.trim();
    if (!modelId) return;
    onDraftChange(addManualModelToDraft(draft, modelId, { allowMultiple }));
    setManualModel('');
  };

  const managedProvider = provider?.runtime_provider === 'copilot'
    || provider?.runtime_provider === 'grok';
  const canProbe = managedProvider
    ? !!managedAuthenticated
    : provider?.runtime_provider === 'openai' && !!draft.base_url;

  const probeModels = async () => {
    if (!canProbe || probeStatus === 'loading') return;
    setProbeStatus('loading');
    setProbeError('');
    try {
      const headers = parseRequestHeadersJson(draft.request_headers_json, draft.provider);
      if (!headers.ok) throw Object.assign(new Error('自定义请求头格式不正确'), { code: headers.code });
      const response = await apiClient.probeModels({
        provider: draft.provider,
        base_url: draft.provider === 'openai' ? draft.base_url : '',
        api_key: draft.provider === 'openai' ? draft.api_key : '',
        request_headers: headers.headers,
      });
      const normalized = normalizeModelProbeResult(response);
      const models = normalized.models.map((id) => ({
        id,
        name: id,
        context_window: normalized.contextWindows[id] || null,
        max_output_tokens: null,
        capabilities: [],
        reasoning: null,
      }));
      setCatalogModels(models);
      setProbeStatus('ready');
    } catch (error) {
      setProbeError(redactModelDraftSecrets(
        lookupErrorMessage(error?.code, error?.message || 'Provider 探测失败'),
        draft,
      ));
      setProbeStatus('error');
    }
  };

  const displayedModels = provider?.model_input === 'catalog'
    ? catalogModels
    : catalogModels.filter((model) => (
      !modelQuery.trim() || model.id.toLowerCase().includes(modelQuery.trim().toLowerCase())
    ));

  return (
    <div className="grid min-h-[320px] grid-cols-1 overflow-hidden rounded-md border border-border bg-surface md:grid-cols-[220px_minmax(0,1fr)]">
      <div className="border-b border-border bg-surface-alt p-2.5 md:border-b-0 md:border-r">
        <label className="relative block">
          <span className="sr-only">搜索 Provider</span>
          <VsIcon name="search" size={13} className="pointer-events-none absolute left-2.5 top-1/2 -translate-y-1/2 text-fg-mute" />
          <input
            type="search"
            value={providerQuery}
            onChange={(event) => setProviderQuery(event.target.value)}
            placeholder="搜索 Provider"
            className="h-8 w-full rounded-md border border-border bg-surface pl-8 pr-2 text-[11px] text-fg outline-none placeholder:text-fg-mute focus:border-accent focus:ring-1 focus:ring-accent-soft"
          />
        </label>
        <div
          className="mt-2 max-h-[210px] space-y-2 overflow-y-auto pr-0.5 md:max-h-[360px]"
          role="listbox"
          aria-label="Provider 列表"
        >
          {groupedProviders.map((group) => (
            <div
              key={group.group}
              role="group"
              aria-labelledby={`model-provider-group-${group.group}`}
            >
              <div
                id={`model-provider-group-${group.group}`}
                className="px-2 py-1 text-[10px] font-semibold text-fg-mute"
              >
                {PROVIDER_GROUP_LABELS[group.group] || group.group}
              </div>
              <div className="space-y-0.5">
                {group.items.map((item) => {
                  const active = item.id === provider?.id;
                  return (
                    <button
                      key={item.id}
                      type="button"
                      role="option"
                      onClick={() => onProviderChange(item)}
                      aria-selected={active}
                      className={clsx(
                        'flex h-9 w-full items-center gap-2 rounded-md px-2 text-left transition focus:outline-none focus:ring-1 focus:ring-inset focus:ring-accent',
                        active
                          ? 'bg-accent-bg text-accent'
                          : 'text-fg-2 hover:bg-surface-hi hover:text-fg',
                      )}
                    >
                      <ProviderIcon provider={item} active={active} />
                      <span className="min-w-0 flex-1 truncate text-[11px] font-medium">
                        {providerDisplayName(item)}
                      </span>
                    </button>
                  );
                })}
              </div>
            </div>
          ))}
          {groupedProviders.length === 0 && (
            <div className="px-2 py-4 text-center text-[11px] text-fg-mute">没有匹配的 Provider</div>
          )}
        </div>
      </div>

      <div className="min-w-0 p-3">
        {provider ? (
          <>
            <div className="flex flex-wrap items-start justify-between gap-2">
              <div className="min-w-0">
                <div className="flex items-center gap-2">
                  <h4 className="truncate text-[12px] font-semibold text-fg">
                    {providerDisplayName(provider)}
                  </h4>
                  <span className="rounded border border-border bg-surface-alt px-1.5 py-0.5 text-[10px] text-fg-mute">
                    {provider.runtime_provider}
                  </span>
                </div>
                <p className="mt-0.5 text-[10px] text-fg-mute">
                  {provider.auth_mode === 'managed'
                    ? '使用 ACECode 管理的登录和端点'
                    : provider.auth_mode === 'none'
                      ? '无需 API Key'
                      : provider.api_key_env
                        ? `建议环境变量 ${provider.api_key_env}`
                        : '按 Provider 要求配置认证'}
                </p>
              </div>
              <div className="flex items-center gap-1.5">
                {provider.doc && (
                  <button
                    type="button"
                    onClick={() => { void openExternalUrl(provider.doc); }}
                    className="h-7 rounded-md px-2 text-[10px] text-fg-mute transition hover:bg-surface-hi hover:text-fg focus:outline-none focus:ring-1 focus:ring-accent"
                  >
                    Provider 文档
                  </button>
                )}
                {(provider.runtime_provider === 'openai' || managedProvider) && (
                  <button
                    type="button"
                    onClick={probeModels}
                    disabled={!canProbe || probeStatus === 'loading'}
                    className="inline-flex h-7 items-center gap-1 rounded-md border border-border px-2 text-[10px] text-fg-2 transition hover:bg-surface-hi focus:outline-none focus:ring-1 focus:ring-accent disabled:opacity-40"
                    title={managedProvider && !managedAuthenticated
                      ? `请先连接 ${provider.name}`
                      : '从当前 Provider 探测真实模型列表'}
                  >
                    <RefreshIcon size={11} className={clsx(probeStatus === 'loading' && 'animate-spin')} />
                    探测模型
                  </button>
                )}
              </div>
            </div>

            {managedProvider && managedConnection && (
              <div className="mt-3">
                <ModelConnectionCard {...managedConnection} />
              </div>
            )}

            <div className="mt-3 flex items-center gap-2 rounded-md border border-border bg-surface-alt px-2.5 py-2">
              <VsIcon name="edit" size={12} className="shrink-0 text-fg-mute" />
              <input
                type="text"
                aria-label="手动模型 ID"
                value={manualModel}
                onChange={(event) => setManualModel(event.target.value)}
                onKeyDown={(event) => {
                  if (event.key === 'Enter') {
                    event.preventDefault();
                    addManualModel();
                  }
                }}
                placeholder="输入模型 ID，按 Enter 添加"
                className="min-w-0 flex-1 bg-transparent text-[11px] text-fg outline-none placeholder:text-fg-mute"
              />
              <button
                type="button"
                onClick={addManualModel}
                disabled={!manualModel.trim()}
                className="h-6 rounded px-2 text-[10px] font-medium text-accent transition hover:bg-accent-bg focus:outline-none focus:ring-1 focus:ring-accent disabled:opacity-40"
              >
                添加
              </button>
            </div>

            <div className="mt-2 flex items-center gap-2">
              <label className="relative min-w-0 flex-1">
                <span className="sr-only">搜索模型</span>
                <VsIcon name="search" size={12} className="pointer-events-none absolute left-2.5 top-1/2 -translate-y-1/2 text-fg-mute" />
                <input
                  type="search"
                  value={modelQuery}
                  onChange={(event) => setModelQuery(event.target.value)}
                  placeholder={provider.model_input === 'catalog' ? '搜索目录模型' : '过滤探测结果'}
                  className="h-8 w-full rounded-md border border-border bg-surface pl-8 pr-2 text-[11px] text-fg outline-none placeholder:text-fg-mute focus:border-accent focus:ring-1 focus:ring-accent-soft"
                />
              </label>
              <span className="shrink-0 text-[10px] text-fg-mute">
                {allowMultiple ? '可多选' : '单选'}
              </span>
            </div>

            <div
              className="mt-2 max-h-[210px] overflow-y-auto rounded-md border border-border bg-surface"
              role="listbox"
              aria-label="模型列表"
              aria-multiselectable={allowMultiple || undefined}
              aria-busy={catalogStatus === 'loading' || probeStatus === 'loading'}
            >
              {(catalogStatus === 'loading' || probeStatus === 'loading') && (
                <div className="px-3 py-5 text-center text-[11px] text-fg-mute">正在读取模型…</div>
              )}
              {(catalogError || probeError) && (
                <div role="status" className="px-3 py-3 text-[11px] text-danger">
                  {catalogError || probeError}
                </div>
              )}
              {displayedModels.map((model, index) => {
                const selected = selectedModels.includes(model.id);
                const metadata = modelMetadataSummary(model);
                return (
                  <button
                    key={model.id}
                    type="button"
                    role="option"
                    onClick={() => updateSelectedModels(model.id, model)}
                    aria-selected={selected}
                    className={clsx(
                      'flex w-full items-center gap-2.5 px-3 py-2 text-left transition focus:outline-none focus:ring-1 focus:ring-inset focus:ring-accent',
                      index > 0 && 'border-t border-border',
                      selected ? 'bg-accent-bg' : 'hover:bg-surface-hi',
                    )}
                  >
                    <span className={clsx(
                      'flex h-[17px] w-[17px] shrink-0 items-center justify-center rounded border text-[10px]',
                      selected
                        ? 'border-accent bg-accent text-white'
                        : 'border-border bg-surface text-transparent',
                    )}>
                      ✓
                    </span>
                    <span className="min-w-0 flex-1">
                      <span className="block truncate text-[11px] font-medium text-fg">{model.name || model.id}</span>
                      <span className="block truncate text-[10px] text-fg-mute">{model.id}</span>
                      {metadata && (
                        <span className="mt-0.5 block truncate text-[10px] text-fg-mute">
                          {metadata}
                        </span>
                      )}
                    </span>
                    {(model.deprecated || model.unavailable) && (
                      <span className="shrink-0 text-[10px] text-warn">不可用警告</span>
                    )}
                  </button>
                );
              })}
              {catalogStatus !== 'loading' && probeStatus !== 'loading'
                && displayedModels.length === 0 && !catalogError && !probeError && (
                <div className="px-3 py-5 text-center text-[11px] text-fg-mute">
                  {provider.model_input === 'catalog'
                    ? '目录中没有匹配模型，仍可手动输入模型 ID。'
                    : '可以手动输入模型 ID，或使用“探测模型”。'}
                </div>
              )}
            </div>

            {selectedModels.length > 0 && (
              <div className="mt-2 flex flex-wrap gap-1.5" aria-label="已选择模型">
                {selectedModels.map((modelId) => (
                  <span key={modelId} className="inline-flex items-center gap-1 rounded-md border border-accent-soft bg-accent-bg py-1 pl-2 pr-1 text-[10px] text-accent">
                    <span className="max-w-[240px] truncate">{modelId}</span>
                    <button
                      type="button"
                      onClick={() => updateSelectedModels(modelId)}
                      className="flex h-4 w-4 items-center justify-center rounded hover:bg-surface-hi focus:outline-none focus:ring-1 focus:ring-accent"
                      aria-label={`移除模型 ${modelId}`}
                    >
                      ×
                    </button>
                  </span>
                ))}
              </div>
            )}
          </>
        ) : (
          <div className="flex h-full min-h-[260px] items-center justify-center text-[11px] text-fg-mute">
            请选择一个 Provider
          </div>
        )}
      </div>
    </div>
  );
}
