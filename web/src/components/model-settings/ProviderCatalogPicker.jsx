import { useEffect, useMemo, useRef, useState } from 'react';
import { clsx } from '../../lib/format.js';
import {
  addManualModelToDraft,
  formatModelTokenLimit,
  normalizeProviderModelQuery,
  redactModelDraftSecrets,
  replaceDraftModelsFromProbe,
  toggleCatalogModelInDraft,
} from '../../lib/modelSettings.js';
import { lookupErrorMessage } from '../../lib/errors.js';
import {
  groupCatalogProviders,
  providerDisplayName,
  PROVIDER_GROUP_LABELS,
} from '../../lib/providerCatalogGroups.js';
import {
  filterProviderModels,
  normalizeModelProbeResult,
  parseRequestHeadersJson,
  splitModelIds,
} from '../../lib/modelManager.js';
import { openExternalUrl } from '../../lib/externalUrl.js';
import { RefreshIcon, VsIcon } from '../Icon.jsx';
import { ModelConnectionCard } from './ModelConnectionCard.jsx';
import { ModelProbeDialog } from './ModelProbeDialog.jsx';
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

function modelRowsFromProbe(response) {
  const normalized = normalizeModelProbeResult(response);
  return normalized.models.map((id) => ({
    id,
    name: id,
    context_window: normalized.contextWindows[id] || null,
    max_output_tokens: null,
    capabilities: [],
    reasoning: null,
  }));
}

function modelProbeRequest(provider, draft) {
  const headers = parseRequestHeadersJson(draft.request_headers_json, draft.provider);
  if (!headers.ok) return { ok: false, code: headers.code };
  return {
    ok: true,
    value: {
      catalog_provider_id: provider?.id || '',
      provider: draft.provider,
      base_url: draft.provider === 'openai' ? draft.base_url : '',
      api_key: draft.provider === 'openai' ? draft.api_key : '',
      request_headers: headers.headers,
    },
  };
}

export function ProviderCatalogPicker({
  apiClient,
  providers,
  provider,
  draft,
  allowMultiple,
  directModelDetails,
  managedAuthenticated,
  managedConnection,
  onProviderChange,
  onDraftChange,
}) {
  const [providerQuery, setProviderQuery] = useState('');
  const [modelQuery, setModelQuery] = useState('');
  const [catalogModels, setCatalogModels] = useState([]);
  const [modelResultSource, setModelResultSource] = useState('catalog');
  const [catalogStatus, setCatalogStatus] = useState('idle');
  const [catalogError, setCatalogError] = useState('');
  const [probeStatus, setProbeStatus] = useState('idle');
  const [probeError, setProbeError] = useState('');
  const [probeResultOrigin, setProbeResultOrigin] = useState('none');
  const [probeDialogOpen, setProbeDialogOpen] = useState(false);
  const [manualModel, setManualModel] = useState('');
  const modelResultSourceRef = useRef('catalog');
  const catalogRequestRevisionRef = useRef(0);
  const probeRequestRevisionRef = useRef(0);
  const probeCacheRequestRevisionRef = useRef(0);
  const providerIdRef = useRef(provider?.id || '');
  providerIdRef.current = provider?.id || '';
  const selectedModels = splitModelIds(draft.model);
  const directModelIdInput = provider?.model_input === 'manual';
  const managedProvider = provider?.runtime_provider === 'copilot'
    || provider?.runtime_provider === 'grok';
  const supportsProbe = provider?.runtime_provider === 'openai' || managedProvider;
  const canReadProbeCache = supportsProbe
    && (managedProvider || !!draft.base_url);
  const canProbe = managedProvider
    ? !!managedAuthenticated
    : provider?.runtime_provider === 'openai' && !!draft.base_url;

  const groupedProviders = useMemo(
    () => groupCatalogProviders(providers, providerQuery),
    [providerQuery, providers],
  );

  useEffect(() => {
    modelResultSourceRef.current = 'catalog';
    catalogRequestRevisionRef.current += 1;
    probeRequestRevisionRef.current += 1;
    probeCacheRequestRevisionRef.current += 1;
    setModelResultSource('catalog');
    setModelQuery('');
    setCatalogModels([]);
    setCatalogStatus('idle');
    setCatalogError('');
    setProbeStatus('idle');
    setProbeError('');
    setProbeDialogOpen(false);
  }, [provider?.id]);

  useEffect(() => {
    if (!provider || provider.model_input !== 'catalog'
        || modelResultSource === 'probe') return undefined;
    let cancelled = false;
    const requestRevision = ++catalogRequestRevisionRef.current;
    const requestProviderId = provider.id;
    const timer = window.setTimeout(async () => {
      if (cancelled
          || providerIdRef.current !== requestProviderId
          || modelResultSourceRef.current !== 'catalog'
          || requestRevision !== catalogRequestRevisionRef.current) return;
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
        if (cancelled
            || providerIdRef.current !== requestProviderId
            || modelResultSourceRef.current !== 'catalog'
            || requestRevision !== catalogRequestRevisionRef.current) return;
        setCatalogModels(mergeCatalogModels(normalized.models, exactModels));
        setCatalogStatus('ready');
      } catch (error) {
        if (cancelled
            || providerIdRef.current !== requestProviderId
            || modelResultSourceRef.current !== 'catalog'
            || requestRevision !== catalogRequestRevisionRef.current) return;
        setCatalogModels([]);
        setCatalogError(error?.message || '目录查询失败');
        setCatalogStatus('error');
      }
    }, 180);
    return () => {
      cancelled = true;
      window.clearTimeout(timer);
    };
  }, [apiClient, draft.model, modelQuery, modelResultSource, provider]);

  useEffect(() => {
    const requestRevision = ++probeCacheRequestRevisionRef.current;
    const requestProviderId = provider?.id || '';

    // A connection change invalidates the in-memory result immediately. The
    // cache read below is local-only; a miss deliberately falls back to the
    // static catalog or manual Model ID without probing upstream.
    modelResultSourceRef.current = 'catalog';
    catalogRequestRevisionRef.current += 1;
    probeRequestRevisionRef.current += 1;
    setModelResultSource('catalog');
    setCatalogModels([]);
    setCatalogStatus('idle');
    setCatalogError('');
    setProbeStatus('idle');
    setProbeError('');
    setProbeResultOrigin('none');
    setProbeDialogOpen(false);

    if (!canReadProbeCache) return undefined;
    const parsedRequest = modelProbeRequest(provider, draft);
    if (!parsedRequest.ok) return undefined;

    let cancelled = false;
    const timer = window.setTimeout(async () => {
      try {
        const response = await apiClient.getModelProbeCache(parsedRequest.value);
        if (cancelled
            || providerIdRef.current !== requestProviderId
            || requestRevision !== probeCacheRequestRevisionRef.current
            || response?.cached !== true) return;
        const models = modelRowsFromProbe(response);
        modelResultSourceRef.current = 'probe';
        catalogRequestRevisionRef.current += 1;
        setCatalogModels(models);
        setModelResultSource('probe');
        setCatalogStatus('ready');
        setCatalogError('');
        setProbeStatus('ready');
        setProbeError('');
        setProbeResultOrigin('cache');
      } catch {
        // Cache lookup is a background convenience. Older daemons and local
        // state read failures must not block the existing catalog/manual flow.
      }
    }, 120);
    return () => {
      cancelled = true;
      window.clearTimeout(timer);
    };
  }, [
    apiClient,
    canReadProbeCache,
    draft.api_key,
    draft.base_url,
    draft.provider,
    draft.request_headers_json,
    provider?.id,
    provider?.runtime_provider,
  ]);

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

  const probeModels = async () => {
    if (!canProbe || probeStatus === 'loading') return;
    const requestRevision = ++probeRequestRevisionRef.current;
    probeCacheRequestRevisionRef.current += 1;
    const requestProviderId = provider?.id || '';
    const hadUsableProbeResult = modelResultSourceRef.current === 'probe'
      && probeStatus === 'ready';
    setProbeStatus('loading');
    setProbeError('');
    try {
      const parsedRequest = modelProbeRequest(provider, draft);
      if (!parsedRequest.ok) {
        throw Object.assign(
          new Error('自定义请求头格式不正确'),
          { code: parsedRequest.code },
        );
      }
      const response = await apiClient.probeModels(parsedRequest.value);
      if (providerIdRef.current !== requestProviderId
          || requestRevision !== probeRequestRevisionRef.current) return;
      const models = modelRowsFromProbe(response);
      modelResultSourceRef.current = 'probe';
      catalogRequestRevisionRef.current += 1;
      setCatalogModels(models);
      setModelResultSource('probe');
      setCatalogStatus('ready');
      setCatalogError('');
      setProbeStatus('ready');
      setProbeError(response?.cache_persisted === false
        ? '模型已探测，但本地缓存写入失败；下次仍需重新探测。'
        : '');
      setProbeResultOrigin('network');
    } catch (error) {
      if (providerIdRef.current !== requestProviderId
          || requestRevision !== probeRequestRevisionRef.current) return;
      setProbeError(redactModelDraftSecrets(
        lookupErrorMessage(error?.code, error?.message || 'Provider 探测失败'),
        draft,
      ));
      setProbeStatus(hadUsableProbeResult ? 'ready' : 'error');
    }
  };

  const openProbeDialog = () => {
    if (!canProbe || probeStatus === 'loading') return;
    setProbeDialogOpen(true);
    if (modelResultSourceRef.current !== 'probe' || probeStatus !== 'ready') {
      void probeModels();
    }
  };

  const confirmProbedModels = (modelIds) => {
    const nextDraft = replaceDraftModelsFromProbe(
      draft,
      catalogModels,
      modelIds,
      { allowMultiple },
    );
    if (nextDraft === draft) return;
    onDraftChange(nextDraft);
    setProbeDialogOpen(false);
  };

  const displayedModels = modelResultSource === 'probe'
    ? filterProviderModels(catalogModels, modelQuery)
    : catalogModels;

  return (
    <>
      <div className="grid min-h-[320px] grid-cols-1 overflow-hidden rounded-md border border-border bg-surface md:h-[420px] md:grid-cols-[220px_minmax(0,1fr)]">
      <div className="border-b border-border bg-surface-alt p-2.5 md:flex md:min-h-0 md:flex-col md:border-b-0 md:border-r">
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
          className="mt-2 max-h-[210px] space-y-2 overflow-y-auto pr-0.5 md:min-h-0 md:max-h-none md:flex-1"
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

      <div className="min-w-0 p-3 md:min-h-0 md:overflow-y-auto">
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
                {!directModelIdInput && (provider.runtime_provider === 'openai' || managedProvider) && (
                  <button
                    type="button"
                    onClick={probeModels}
                    disabled={!canProbe || probeStatus === 'loading'}
                    className="inline-flex h-7 items-center gap-1 rounded-md border border-border px-2 text-[10px] text-fg-2 transition hover:bg-surface-hi focus:outline-none focus:ring-1 focus:ring-accent disabled:opacity-40"
                     title={managedProvider && !managedAuthenticated
                       ? `请先连接 ${provider.name}`
                       : modelResultSource === 'probe'
                         ? '忽略本地缓存并重新访问当前 Provider'
                         : '从当前 Provider 探测真实模型列表'}
                   >
                     <RefreshIcon size={11} className={clsx(probeStatus === 'loading' && 'animate-spin')} />
                     {modelResultSource === 'probe' ? '重新探测' : '探测模型'}
                  </button>
                )}
              </div>
            </div>

            {managedProvider && managedConnection && (
              <div className="mt-3">
                <ModelConnectionCard {...managedConnection} />
              </div>
            )}

            {directModelIdInput ? (
              <div className="mt-4 space-y-4">
                <div>
                  <div className="mb-1.5 flex items-center justify-between gap-3">
                    <label
                      htmlFor="custom-openai-model-id"
                      className="block text-[11px] font-medium text-fg-2"
                    >
                      Model ID
                    </label>
                    {provider.runtime_provider === 'openai' && (
                      <button
                        type="button"
                        onClick={openProbeDialog}
                        disabled={!canProbe || probeStatus === 'loading'}
                        className="inline-flex h-7 items-center gap-1 rounded-md border border-border bg-surface px-2.5 text-[10px] font-medium text-fg-2 transition hover:bg-surface-hi focus:outline-none focus:ring-1 focus:ring-accent disabled:cursor-not-allowed disabled:opacity-40"
                       title={!draft.base_url
                         ? '请先填写 Base URL'
                         : modelResultSource === 'probe' && probeStatus === 'ready'
                           ? '打开本地保存的探测结果'
                           : '从当前 Provider 探测真实模型列表'}
                      >
                        <RefreshIcon
                          size={11}
                          className={clsx(probeStatus === 'loading' && 'animate-spin')}
                        />
                       {modelResultSource === 'probe' && probeStatus === 'ready'
                         ? '查看探测结果'
                         : '探测模型'}
                      </button>
                    )}
                  </div>
                  <input
                    id="custom-openai-model-id"
                    type="text"
                    value={draft.model}
                    onChange={(event) => onDraftChange({ ...draft, model: event.target.value })}
                    placeholder="例如：gpt-4.1"
                    autoComplete="off"
                    spellCheck={false}
                    aria-describedby="custom-openai-model-id-help"
                    className="h-9 w-full rounded-md border border-border bg-surface px-3 text-[12px] text-fg outline-none transition placeholder:text-fg-mute focus:border-accent focus:ring-1 focus:ring-accent-soft"
                  />
                  <p id="custom-openai-model-id-help" className="mt-1.5 text-[10px] text-fg-mute">
                    {allowMultiple
                      ? '可直接输入一个或多个模型 ID（使用逗号分隔），也可探测后多选。'
                      : '直接输入模型 ID，或从当前 OpenAI 兼容接口探测后选择。'}
                  </p>
                </div>
                {directModelDetails}
              </div>
            ) : (
              <>
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
                      placeholder="搜索目录模型"
                      className="h-8 w-full rounded-md border border-border bg-surface pl-8 pr-2 text-[11px] text-fg outline-none placeholder:text-fg-mute focus:border-accent focus:ring-1 focus:ring-accent-soft"
                    />
                  </label>
                </div>

                <div
                  className="mt-2 max-h-[210px] overflow-y-auto rounded-md border border-border bg-surface"
                  role={allowMultiple ? 'group' : 'radiogroup'}
                  aria-label="模型列表"
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
                      <label
                        key={model.id}
                        className={clsx(
                          'flex w-full cursor-pointer items-center gap-2.5 px-3 py-2 text-left transition focus-within:ring-1 focus-within:ring-inset focus-within:ring-accent',
                          index > 0 && 'border-t border-border',
                          selected ? 'bg-accent-bg' : 'hover:bg-surface-hi',
                        )}
                      >
                        <input
                          type={allowMultiple ? 'checkbox' : 'radio'}
                          name={allowMultiple ? undefined : `model-catalog-selection-${provider.id}`}
                          checked={selected}
                          onChange={() => updateSelectedModels(model.id, model)}
                          className="h-[17px] w-[17px] shrink-0 accent-accent"
                        />
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
                      </label>
                    );
                  })}
                  {catalogStatus !== 'loading' && probeStatus !== 'loading'
                    && displayedModels.length === 0 && !catalogError && !probeError && (
                    <div className="px-3 py-5 text-center text-[11px] text-fg-mute">
                      目录中没有匹配模型，仍可手动输入模型 ID。
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
            )}
          </>
        ) : (
          <div className="flex h-full min-h-[260px] items-center justify-center text-[11px] text-fg-mute">
            请选择一个 Provider
          </div>
        )}
      </div>
      </div>

      {probeDialogOpen && (
        <ModelProbeDialog
          models={catalogModels}
          status={probeStatus}
          error={probeError}
          initialModelIds={draft.model}
          allowMultiple={allowMultiple}
          fromCache={probeResultOrigin === 'cache'}
          onRefresh={() => { void probeModels(); }}
          onConfirm={confirmProbedModels}
          onClose={() => setProbeDialogOpen(false)}
        />
      )}
    </>
  );
}
