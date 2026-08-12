import { useCallback, useEffect, useMemo, useState } from 'react';
import { api } from '../../lib/api.js';
import { lookupErrorMessage } from '../../lib/errors.js';
import { clsx } from '../../lib/format.js';
import {
  applyCatalogProviderToDraft,
  emptyModelProfileDraft,
  modelProfileDraftFromSaved,
  normalizeModelCatalogSummary,
  normalizeSavedModelList,
  normalizeSavedModelProfile,
} from '../../lib/modelSettings.js';
import { copyTextToSystemClipboard } from '../../lib/systemClipboard.js';
import { openExternalUrl } from '../../lib/externalUrl.js';
import { Modal } from '../Modal.jsx';
import { RefreshIcon, VsIcon } from '../Icon.jsx';
import { toast } from '../Toast.jsx';
import { ModelConnectionCard } from './ModelConnectionCard.jsx';
import { ModelProfileDialog } from './ModelProfileDialog.jsx';
import { SavedModelList } from './SavedModelList.jsx';

function initialProvider(providers) {
  return providers.find((provider) => provider.id === 'custom-openai')
    || providers.find((provider) => provider.group === 'custom')
    || providers.find((provider) => provider.runtime_provider !== 'copilot')
    || providers[0]
    || null;
}

function providerForSavedModel(providers, model) {
  return providers.find((provider) => (
    !!model.models_dev_provider_id
      && (provider.id === model.models_dev_provider_id
        || provider.models_dev_provider_id === model.models_dev_provider_id)
  ))
    || providers.find((provider) => (
      provider.runtime_provider === model.provider
        && (model.provider !== 'openai' || provider.group === 'custom')
    ))
    || initialProvider(providers);
}

function catalogErrorCopy(error) {
  return error?.code === 'MODEL_CATALOG_CONTRACT'
    ? '本地模型目录格式不完整，请更新 ACECode 或刷新目录。'
    : lookupErrorMessage(error?.code, error?.message);
}

export function ModelSettingsSection({ onModelProfileUpdated }) {
  const [models, setModels] = useState([]);
  const [defaultName, setDefaultName] = useState('');
  const [modelsLoading, setModelsLoading] = useState(true);
  const [modelsError, setModelsError] = useState('');
  const [catalog, setCatalog] = useState(null);
  const [catalogLoading, setCatalogLoading] = useState(true);
  const [catalogError, setCatalogError] = useState('');
  const [catalogRefreshing, setCatalogRefreshing] = useState(false);
  const [savedQuery, setSavedQuery] = useState('');
  const [mutationBusy, setMutationBusy] = useState('');
  const [blockedDeletes, setBlockedDeletes] = useState(() => new Set());
  const [profileDialog, setProfileDialog] = useState(null);
  const [deleteTarget, setDeleteTarget] = useState(null);
  const [copilotAuth, setCopilotAuth] = useState({
    loading: true,
    authenticated: false,
    has_token: false,
  });
  const [copilotBusy, setCopilotBusy] = useState('');
  const [copilotFlow, setCopilotFlow] = useState(null);

  const providers = catalog?.providers || [];

  const loadSavedModels = useCallback(async ({ quiet = false } = {}) => {
    if (!quiet) setModelsLoading(true);
    setModelsError('');
    try {
      const [rawModels, rawDefault] = await Promise.all([
        api.listModels(),
        api.getDefaultModel().catch(() => ({ name: '' })),
      ]);
      const safeModels = normalizeSavedModelList(rawModels);
      setModels(safeModels);
      setDefaultName(rawDefault?.name || rawDefault?.default_model_name || '');
      setBlockedDeletes((current) => {
        const names = new Set(safeModels.map((model) => model.name));
        const next = new Set([...current].filter((name) => names.has(name)));
        return next.size === current.size ? current : next;
      });
      return safeModels;
    } catch (error) {
      const message = lookupErrorMessage(error?.code, error?.message);
      setModelsError(message);
      if (!quiet) toast({ kind: 'err', text: message });
      return null;
    } finally {
      if (!quiet) setModelsLoading(false);
    }
  }, []);

  const loadCatalog = useCallback(async ({ quiet = false } = {}) => {
    if (!quiet) setCatalogLoading(true);
    setCatalogError('');
    try {
      const rawCatalog = await api.getModelCatalog();
      const normalized = normalizeModelCatalogSummary(rawCatalog);
      setCatalog(normalized);
      return normalized;
    } catch (error) {
      setCatalogError(catalogErrorCopy(error));
      return null;
    } finally {
      if (!quiet) setCatalogLoading(false);
    }
  }, []);

  const loadCopilotAuth = useCallback(async () => {
    setCopilotAuth((current) => ({ ...current, loading: true }));
    try {
      const state = await api.getCopilotAuth();
      setCopilotAuth({
        loading: false,
        authenticated: !!state?.authenticated,
        has_token: !!state?.has_token,
      });
    } catch {
      setCopilotAuth({ loading: false, authenticated: false, has_token: false });
    }
  }, []);

  useEffect(() => {
    void loadSavedModels();
    void loadCatalog();
    void loadCopilotAuth();
  }, [loadCatalog, loadCopilotAuth, loadSavedModels]);

  const copyCopilotCode = useCallback(async (userCode, { silent = false } = {}) => {
    const result = await copyTextToSystemClipboard(userCode);
    if (!silent) {
      toast({
        kind: result.ok ? 'ok' : 'err',
        text: result.ok ? '验证码已复制' : `复制验证码失败：${result.error || ''}`,
      });
    }
    return result;
  }, []);

  const startCopilotLogin = useCallback(async () => {
    if (copilotBusy) return;
    setCopilotBusy('start');
    try {
      const flow = await api.startCopilotAuth();
      setCopilotFlow({
        ...flow,
        status: 'pending',
        interval: Math.max(1, Number(flow?.interval || 5)),
        message: '等待 GitHub 授权',
      });
      const copied = flow?.user_code
        ? await copyCopilotCode(flow.user_code, { silent: true })
        : null;
      if (flow?.verification_uri) {
        const opened = await openExternalUrl(flow.verification_uri);
        if (!opened.ok) toast({ kind: 'err', text: `无法打开系统浏览器：${opened.error || ''}` });
      }
      toast({
        kind: 'ok',
        text: copied?.ok ? 'Copilot 登录已开始，验证码已复制' : 'Copilot 登录已开始',
      });
    } catch (error) {
      toast({ kind: 'err', text: lookupErrorMessage(error?.code, error?.message) });
    } finally {
      setCopilotBusy('');
    }
  }, [copilotBusy, copyCopilotCode]);

  const pollCopilotFlow = useCallback(async () => {
    if (!copilotFlow?.device_code || copilotBusy) return;
    setCopilotBusy('poll');
    try {
      const state = await api.pollCopilotAuth(copilotFlow.device_code);
      if (state?.status === 'authenticated') {
        setCopilotFlow(null);
        setCopilotAuth({ loading: false, authenticated: true, has_token: true });
        toast({ kind: 'ok', text: 'Copilot 已登录' });
        return;
      }
      setCopilotFlow((current) => current ? {
        ...current,
        status: state?.status || 'pending',
        message: state?.message || '等待 GitHub 授权',
        interval: Math.max(
          1,
          Number(current.interval || 5) + Number(state?.interval_delta_seconds || 0),
        ),
      } : current);
    } catch (error) {
      setCopilotFlow((current) => current ? {
        ...current,
        status: 'failed',
        message: lookupErrorMessage(error?.code, error?.message),
      } : current);
    } finally {
      setCopilotBusy('');
    }
  }, [copilotBusy, copilotFlow]);

  useEffect(() => {
    if (!copilotFlow?.device_code || copilotBusy) return undefined;
    if (['authenticated', 'expired', 'failed'].includes(copilotFlow.status)) return undefined;
    const timeout = window.setTimeout(
      () => { void pollCopilotFlow(); },
      Math.max(1, Number(copilotFlow.interval || 5)) * 1000,
    );
    return () => window.clearTimeout(timeout);
  }, [copilotBusy, copilotFlow, pollCopilotFlow]);

  const logoutCopilot = useCallback(async () => {
    if (copilotBusy) return;
    setCopilotBusy('logout');
    try {
      await api.logoutCopilot();
      setCopilotFlow(null);
      setCopilotAuth({ loading: false, authenticated: false, has_token: false });
      toast({ kind: 'ok', text: 'Copilot 已退出' });
    } catch (error) {
      toast({ kind: 'err', text: lookupErrorMessage(error?.code, error?.message) });
    } finally {
      setCopilotBusy('');
    }
  }, [copilotBusy]);

  const refreshCatalog = useCallback(async () => {
    if (catalogRefreshing) return;
    setCatalogRefreshing(true);
    try {
      await api.refreshModelCatalog();
      const refreshed = await loadCatalog({ quiet: true });
      if (!refreshed) throw new Error('刷新后的本地目录无法读取');
      toast({ kind: 'ok', text: '模型目录已更新' });
    } catch (error) {
      toast({ kind: 'err', text: lookupErrorMessage(error?.code, error?.message) });
    } finally {
      setCatalogRefreshing(false);
    }
  }, [catalogRefreshing, loadCatalog]);

  const openAddDialog = useCallback(() => {
    const provider = initialProvider(providers);
    if (!provider) {
      toast({ kind: 'err', text: catalogError || '模型目录尚未就绪' });
      return;
    }
    setProfileDialog({
      mode: 'add',
      originalName: '',
      seed: applyCatalogProviderToDraft(emptyModelProfileDraft(), provider),
    });
  }, [catalogError, providers]);

  const openEditDialog = useCallback((model) => {
    const provider = providerForSavedModel(providers, model);
    if (!provider) {
      toast({ kind: 'err', text: '找不到该预设对应的 Provider 描述' });
      return;
    }
    const seed = modelProfileDraftFromSaved(model);
    seed.catalog_provider_id = provider.id;
    setProfileDialog({
      mode: 'edit',
      originalName: model.name,
      seed,
    });
  }, [providers]);

  const announceMutation = useCallback((result) => {
    if (result && typeof result === 'object' && result.name) {
      const safe = normalizeSavedModelProfile(result);
      onModelProfileUpdated?.(safe);
      return safe;
    }
    onModelProfileUpdated?.(result || {});
    return result;
  }, [onModelProfileUpdated]);

  const addPayloadQueue = useCallback(async (payloads) => {
    const queue = [...payloads];
    while (queue.length > 0) {
      const payload = queue.shift();
      try {
        const added = await api.addModel(payload);
        announceMutation(added);
      } catch (error) {
        if (error?.code === 'NAME_TAKEN') {
          return { conflict: { payload, remaining: queue, operation: 'add' } };
        }
        throw error;
      }
    }
    return { ok: true };
  }, [announceMutation]);

  const submitProfiles = useCallback(async ({ mode, originalName, payloads }) => {
    setMutationBusy('save');
    try {
      let result;
      if (mode === 'edit') {
        try {
          const updated = await api.updateModel(originalName, payloads[0]);
          announceMutation(updated);
          result = { ok: true };
        } catch (error) {
          if (error?.code !== 'NAME_TAKEN') throw error;
          result = {
            conflict: {
              payload: payloads[0],
              remaining: [],
              operation: 'edit',
              original_name: originalName,
            },
          };
        }
      } else {
        result = await addPayloadQueue(payloads);
      }
      await loadSavedModels({ quiet: true });
      if (!result.conflict) toast({ kind: 'ok', text: mode === 'edit' ? '模型已更新' : '模型已保存' });
      return result;
    } finally {
      setMutationBusy('');
    }
  }, [addPayloadQueue, announceMutation, loadSavedModels]);

  const resolveConflict = useCallback(async ({ action, conflict, name }) => {
    setMutationBusy('conflict');
    try {
      let resolved;
      if (action === 'overwrite') {
        const updated = await api.updateModel(conflict.payload.name, conflict.payload);
        announceMutation(updated);
        resolved = { ok: true };
      } else {
        const nextPayload = { ...conflict.payload, name: String(name || '').trim() };
        try {
          const added = await api.addModel(nextPayload);
          announceMutation(added);
          resolved = { ok: true };
        } catch (error) {
          if (error?.code !== 'NAME_TAKEN') throw error;
          resolved = {
            conflict: {
              ...conflict,
              payload: nextPayload,
            },
          };
        }
      }
      if (!resolved.conflict && conflict.remaining?.length) {
        resolved = await addPayloadQueue(conflict.remaining);
      }
      await loadSavedModels({ quiet: true });
      if (!resolved.conflict) toast({ kind: 'ok', text: '名称冲突已处理' });
      return resolved;
    } finally {
      setMutationBusy('');
    }
  }, [addPayloadQueue, announceMutation, loadSavedModels]);

  const setDefaultModel = useCallback(async (model) => {
    if (mutationBusy) return;
    setMutationBusy(`default:${model.name}`);
    try {
      await api.setDefaultModel(model.name);
      setDefaultName(model.name);
      onModelProfileUpdated?.({ type: 'default', name: model.name });
      toast({ kind: 'ok', text: `默认模型已设为 ${model.name}` });
    } catch (error) {
      toast({ kind: 'err', text: lookupErrorMessage(error?.code, error?.message) });
    } finally {
      setMutationBusy('');
    }
  }, [mutationBusy, onModelProfileUpdated]);

  const confirmDelete = useCallback(async () => {
    if (!deleteTarget || mutationBusy) return;
    setMutationBusy(`delete:${deleteTarget.name}`);
    setDeleteTarget((current) => current ? { ...current, error: '' } : current);
    try {
      await api.removeModel(deleteTarget.name);
      onModelProfileUpdated?.({ type: 'delete', name: deleteTarget.name });
      setDeleteTarget(null);
      await loadSavedModels({ quiet: true });
      toast({ kind: 'ok', text: `模型 ${deleteTarget.name} 已删除` });
    } catch (error) {
      if (error?.code === 'MODEL_IN_USE') {
        setBlockedDeletes((current) => new Set([...current, deleteTarget.name]));
      }
      setDeleteTarget((current) => current ? {
        ...current,
        error: lookupErrorMessage(error?.code, error?.message),
        blocked: error?.code === 'MODEL_IN_USE',
      } : current);
    } finally {
      setMutationBusy('');
    }
  }, [deleteTarget, loadSavedModels, mutationBusy, onModelProfileUpdated]);

  const catalogMeta = useMemo(() => {
    if (!catalog?.catalog) return '';
    return [catalog.catalog.freshness, catalog.catalog.updated_at].filter(Boolean).join(' · ');
  }, [catalog]);

  return (
    <div
      className="space-y-6 pb-8"
      aria-busy={modelsLoading || catalogLoading || catalogRefreshing || !!mutationBusy}
    >
      <header className="flex flex-wrap items-start justify-between gap-3">
        <div>
          <h2 className="text-[16px] font-semibold text-fg">模型</h2>
          <p className="mt-1 max-w-[680px] text-[12px] leading-5 text-fg-mute">
            快速找到、配置并维护聊天使用的模型连接。Provider 决定可用字段，预设保存后才会出现在会话模型选择器中。
          </p>
          {catalogMeta && (
            <div className="mt-1 text-[10px] text-fg-mute">{`目录状态：${catalogMeta}`}</div>
          )}
        </div>
        <button
          type="button"
          onClick={refreshCatalog}
          disabled={catalogRefreshing}
          className="inline-flex h-8 items-center gap-1.5 rounded-md border border-border bg-surface px-3 text-[11px] font-medium text-fg-2 transition hover:bg-surface-hi focus:outline-none focus:ring-1 focus:ring-accent disabled:opacity-50"
          title="显式更新本地 models.dev 目录；打开页面不会联网刷新"
        >
          <RefreshIcon size={12} className={clsx(catalogRefreshing && 'animate-spin')} />
          更新模型目录
        </button>
      </header>

      <ModelConnectionCard
        auth={copilotAuth}
        flow={copilotFlow}
        busy={!!copilotBusy}
        onConnect={startCopilotLogin}
        onLogout={logoutCopilot}
        onCopyCode={copyCopilotCode}
        onPoll={pollCopilotFlow}
      />

      {modelsError && (
        <div role="status" className="rounded-md border border-danger bg-danger-bg px-3.5 py-2.5 text-[11px] text-danger">
          {modelsError}
        </div>
      )}
      <SavedModelList
        models={models}
        defaultName={defaultName}
        query={savedQuery}
        onQueryChange={setSavedQuery}
        loading={modelsLoading}
        busy={mutationBusy}
        blockedDeletes={blockedDeletes}
        onRefresh={() => { void loadSavedModels(); }}
        onAdd={openAddDialog}
        onSetDefault={setDefaultModel}
        onEdit={openEditDialog}
        onDelete={(model) => setDeleteTarget({ ...model, error: '', blocked: false })}
      />

      {profileDialog && (
        <ModelProfileDialog
          key={`${profileDialog.mode}:${profileDialog.originalName}:${profileDialog.seed.model}`}
          apiClient={api}
          mode={profileDialog.mode}
          seed={profileDialog.seed}
          providers={providers}
          savedModels={models}
          originalName={profileDialog.originalName}
          copilotAuthenticated={copilotAuth.authenticated}
          onManageCopilot={startCopilotLogin}
          onSubmit={submitProfiles}
          onResolveConflict={resolveConflict}
          onClose={() => setProfileDialog(null)}
        />
      )}

      {deleteTarget && (
        <Modal
          onClose={() => { if (!mutationBusy) setDeleteTarget(null); }}
          width={440}
          dismissOnBackdrop={!mutationBusy}
          dismissOnEscape={!mutationBusy}
          labelledBy="delete-model-title"
        >
          <div className="p-5">
            <div className="flex items-start gap-3">
              <div className="flex h-8 w-8 shrink-0 items-center justify-center rounded-md border border-danger bg-danger-bg text-danger">
                <VsIcon name="warning" size={14} />
              </div>
              <div className="min-w-0 flex-1">
                <h2 id="delete-model-title" className="text-[14px] font-semibold text-fg">删除模型预设</h2>
                <p className="mt-1 text-[11px] leading-5 text-fg-2">
                  {`确定删除“${deleteTarget.name}”吗？此操作只删除预设，不会删除会话记录。`}
                </p>
                {deleteTarget.name === defaultName && (
                  <div className="mt-2 rounded-md border border-warn bg-warn-bg px-2.5 py-2 text-[11px] text-warn">
                    这是当前默认模型。删除后默认模型将被清空。
                  </div>
                )}
                {deleteTarget.error && (
                  <div role="alert" className="mt-2 rounded-md border border-danger bg-danger-bg px-2.5 py-2 text-[11px] text-danger">
                    {deleteTarget.error}
                  </div>
                )}
              </div>
            </div>
            <div className="mt-5 flex justify-end gap-2">
              <button
                type="button"
                onClick={() => setDeleteTarget(null)}
                disabled={!!mutationBusy}
                className="h-8 rounded-md border border-border px-3.5 text-[11px] text-fg-2 transition hover:bg-surface-hi focus:outline-none focus:ring-1 focus:ring-accent disabled:opacity-50"
              >
                取消
              </button>
              <button
                type="button"
                onClick={confirmDelete}
                disabled={!!mutationBusy || deleteTarget.blocked}
                className="inline-flex h-8 items-center gap-1.5 rounded-md bg-danger px-3.5 text-[11px] font-semibold text-white transition hover:opacity-90 focus:outline-none focus:ring-2 focus:ring-danger-bg disabled:cursor-not-allowed disabled:opacity-50"
              >
                {mutationBusy && <span className="ace-spinner" />}
                删除预设
              </button>
            </div>
          </div>
        </Modal>
      )}
    </div>
  );
}
