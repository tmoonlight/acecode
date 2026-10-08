import { useEffect, useMemo, useRef, useState } from 'react';
import { clsx } from '../../lib/format.js';
import { lookupErrorMessage } from '../../lib/errors.js';
import {
  applyCatalogProviderToDraft,
  buildModelMutationPayloads,
  hasAdvancedModelValues,
  isCustomOpenAiCompatibilityProvider,
  isCustomReasoningDraft,
  MODEL_REASONING_EFFORTS,
  updateModelReasoningEfforts,
  markModelMetadataOverrides,
  modelAliasProviderName,
  modelFieldPolicy,
  modelNameSuggestion,
  redactModelDraftSecrets,
  syncAutoModelAlias,
  toggleModelCapability,
} from '../../lib/modelSettings.js';
import { MODEL_CAPABILITY_OPTIONS, splitModelIds } from '../../lib/modelManager.js';
import { expandModelAliases } from '../../lib/modelAlias.js';
import { modelTestFailureMessage, testModelDraft } from '../../lib/modelConnectionTest.js';
import { Modal, Toggle } from '../Modal.jsx';
import { VsIcon } from '../Icon.jsx';
import { toast } from '../Toast.jsx';
import { ProviderCatalogPicker } from './ProviderCatalogPicker.jsx';

function inputClass(extra = '') {
  return clsx(
    'h-9 w-full rounded-md border border-border bg-surface px-3 text-[12px] text-fg outline-none transition',
    'placeholder:text-fg-mute focus:border-accent focus:ring-1 focus:ring-accent-soft disabled:cursor-not-allowed disabled:opacity-60',
    extra,
  );
}

function fieldLabel(id, label, optional = false) {
  return (
    <label htmlFor={id} className="mb-1.5 block text-[11px] font-normal text-fg-2">
      {label}
      {optional && <span className="ml-1 font-normal text-fg-mute">可选</span>}
    </label>
  );
}

function ModelApiProtocolField({ draft, onPatchDraft }) {
  return (
    <div className="md:col-span-2">
      {fieldLabel('model-api-protocol', 'API 协议')}
      <select
        id="model-api-protocol"
        value={draft.api_protocol || 'chat_completions'}
        onChange={(event) => onPatchDraft({ api_protocol: event.target.value })}
        aria-describedby="model-api-protocol-help"
        className={inputClass()}
      >
        <option value="chat_completions">Chat Completions</option>
        <option value="responses">Responses</option>
      </select>
      <p id="model-api-protocol-help" className="mt-1.5 text-[11px] leading-5 text-fg-mute">
        按服务商支持的接口选择；需要 Responses 的模型请在此切换。
      </p>
    </div>
  );
}

function ModelApiKeyInput({ draft, apiKeyVisible, onPatchDraft, onToggleApiKey,
  testStatus, onTest, submitting }) {
  const testing = testStatus === 'testing';
  const succeeded = testStatus === 'success';
  const testLabel = testing ? '检测中…' : succeeded ? '检测成功，点击重新检测' : '检测';
  return (
    <div className="relative">
      <input
        id="model-api-key"
        type={apiKeyVisible ? 'text' : 'password'}
        value={draft.api_key}
        onChange={(event) => onPatchDraft({
          api_key: event.target.value,
          clear_api_key: false,
        })}
        placeholder="输入 API Key"
        className={inputClass('pr-24')}
        autoComplete="off"
        spellCheck={false}
      />
      <button
        type="button"
        onClick={onToggleApiKey}
        aria-label={apiKeyVisible ? '隐藏 API Key' : '显示 API Key'}
        aria-pressed={apiKeyVisible}
        aria-controls="model-api-key"
        title={apiKeyVisible ? '隐藏 API Key' : '显示 API Key'}
        className={clsx(
          'absolute inset-y-px right-14 flex w-9 items-center justify-center transition',
          'text-fg-mute hover:bg-surface-hi hover:text-fg focus:outline-none focus:ring-1 focus:ring-inset focus:ring-accent',
          apiKeyVisible && 'text-accent',
        )}
      >
        <VsIcon name="eye" size={14} />
      </button>
      <button
        type="button"
        onClick={onTest}
        disabled={testing || submitting}
        aria-label={testLabel}
        aria-busy={testing}
        title={testLabel}
        data-model-connection-test={testStatus}
        className={clsx(
          'absolute inset-y-0 right-0 flex w-14 items-center justify-center rounded-r-md text-[12px] transition',
          'focus:outline-none focus:ring-1 focus:ring-inset focus:ring-accent disabled:cursor-not-allowed disabled:opacity-60',
          succeeded
            ? 'border border-ok text-ok hover:bg-ok-bg'
            : 'border-l border-border text-fg-2 hover:bg-surface-hi',
        )}
      >
        {testing ? <span className="ace-spinner" />
          : succeeded ? <VsIcon name="check" size={17} /> : '检测'}
      </button>
      <span className="sr-only" role="status">{testStatus === 'idle' ? '' : testLabel}</span>
    </div>
  );
}

// 别名 = saved_models 的 name。单选时自动填模型 ID,多选时退化为「前缀」,
// 每个模型保存为 <前缀>-<模型 ID>;规则见 lib/modelAlias.js,自动值由
// syncAutoModelAlias 维护,用户手改过就不再覆盖。
function ModelAliasField({ draft, editing, existingNames, onPatchDraft }) {
  const selected = splitModelIds(draft.model);
  const multiple = !editing && selected.length > 1;
  let help = null;
  if (multiple) {
    const names = expandModelAliases(draft.name, selected, { existingNames });
    const shown = names.slice(0, 2).join('、');
    help = names.length > 2 ? `${shown} 等 ${names.length} 个` : shown;
  }
  return (
    <div>
      {fieldLabel('model-profile-name', multiple ? '别名前缀' : '别名', !editing)}
      <input
        id="model-profile-name"
        type="text"
        value={draft.name}
        onChange={(event) => onPatchDraft({ name: event.target.value })}
        placeholder={editing ? '' : multiple ? '留空则直接使用模型 ID' : '选择模型后自动填入模型 ID'}
        aria-describedby={help ? 'model-profile-name-help' : undefined}
        className={inputClass()}
        autoComplete="off"
        spellCheck={false}
      />
      {help && (
        <p id="model-profile-name-help" className="mt-1.5 truncate text-[10px] text-fg-mute" title={help}>
          {'将分别保存为：'}
          <span className="ml-0.5 text-fg-2">{help}</span>
        </p>
      )}
    </div>
  );
}

function CustomCompatibilityApiFields({
  draft,
  policy,
  editing,
  existingNames,
  apiKeyVisible,
  onPatchDraft,
  onToggleApiKey,
  testStatus,
  onTest,
  submitting,
}) {
  return (
    <div className="grid grid-cols-1 gap-3 md:grid-cols-2">
      <div className="md:col-span-2">
        <ModelAliasField
          draft={draft}
          editing={editing}
          existingNames={existingNames}
          onPatchDraft={onPatchDraft}
        />
      </div>
      {policy?.show_api_protocol && (
        <ModelApiProtocolField draft={draft} onPatchDraft={onPatchDraft} />
      )}
      {policy?.show_base_url && (
        <div className={policy.show_api_key ? '' : 'md:col-span-2'}>
          {fieldLabel('model-base-url', draft.endpoint_mode === 'full_url' ? '完整端点 URL' : 'Base URL')}
          <input
            id="model-base-url"
            type="url"
            value={draft.base_url}
            onChange={(event) => onPatchDraft({ base_url: event.target.value })}
            readOnly={!policy.edit_base_url}
            className={inputClass(!policy.edit_base_url && 'bg-surface-alt text-fg-mute')}
            spellCheck={false}
          />
        </div>
      )}
      {policy?.show_api_key && (
        <div>
          {fieldLabel('model-api-key', 'API Key')}
          <ModelApiKeyInput
            draft={draft}
            apiKeyVisible={apiKeyVisible}
            onPatchDraft={onPatchDraft}
            onToggleApiKey={onToggleApiKey}
            testStatus={testStatus}
            onTest={onTest}
            submitting={submitting}
          />
        </div>
      )}
    </div>
  );
}

function providerForDraft(providers, draft) {
  return providers.find((provider) => provider.id === draft.catalog_provider_id)
    || providers.find((provider) => (
      provider.runtime_provider === draft.provider
        && !!draft.models_dev_provider_id
        && provider.models_dev_provider_id === draft.models_dev_provider_id
    ))
    || providers.find((provider) => (
      provider.runtime_provider === draft.provider
        && (draft.provider !== 'openai' || provider.group === 'custom')
    ))
    || providers[0]
    || null;
}

export function ModelProfileDialog({
  apiClient,
  mode,
  seed,
  providers,
  savedModels,
  originalName = '',
  managedConnections,
  onSubmit,
  onResolveConflict,
  onClose,
}) {
  const editing = mode === 'edit';
  const [draft, setDraft] = useState(seed);
  const [apiKeyVisible, setApiKeyVisible] = useState(false);
  const [advancedOpen, setAdvancedOpen] = useState(() => editing && hasAdvancedModelValues(seed));
  const [submitting, setSubmitting] = useState(false);
  const [testResult, setTestResult] = useState(null);
  const testRunRef = useRef(null);
  const latestDraftRef = useRef(draft);
  latestDraftRef.current = draft;
  const testStatus = testResult?.draft === draft ? testResult.status : 'idle';
  const [formError, setFormError] = useState('');
  const [conflict, setConflict] = useState(null);
  const [conflictMode, setConflictMode] = useState('');
  const [saveAsName, setSaveAsName] = useState('');
  const saveAsRef = useRef(null);
  const advancedSectionRef = useRef(null);
  const provider = useMemo(() => providerForDraft(providers, draft), [draft, providers]);
  const policy = provider ? modelFieldPolicy(provider) : null;
  const customCompatibilityApi = isCustomOpenAiCompatibilityProvider(provider);
  const managedConnection = managedConnections?.[provider?.runtime_provider] || null;
  const managedAuthenticated = !!managedConnection?.auth?.authenticated;
  const existingNames = useMemo(
    () => (Array.isArray(savedModels) ? savedModels : []).map((model) => model.name),
    [savedModels],
  );

  useEffect(() => () => {
    testRunRef.current?.abort();
    testRunRef.current = null;
  }, [draft, apiClient]);

  const testConnection = async () => {
    if (submitting || testRunRef.current) return;
    const controller = new AbortController();
    testRunRef.current = controller;
    const isCurrent = () => testRunRef.current === controller
      && latestDraftRef.current === draft && !controller.signal.aborted;
    setTestResult({ draft, status: 'testing' });
    try {
      await testModelDraft(apiClient, draft, provider, {
        editing, originalName, signal: controller.signal,
      });
      if (isCurrent()) setTestResult({ draft, status: 'success' });
    } catch (error) {
      if (isCurrent()) {
        setTestResult({ draft, status: 'idle' });
        toast({ kind: 'err', text: modelTestFailureMessage(error, draft) });
      }
    } finally {
      if (testRunRef.current === controller) testRunRef.current = null;
    }
  };

  // 所有会改变「选了哪些模型 / 哪个 Provider」的草稿更新都经这里,让自动别名
  // 跟着刷新;只改其它字段的 patchDraft 不走它,避免用户正在手打的别名被覆盖。
  const withAutoAlias = (next) => syncAutoModelAlias(next, {
    providerName: modelAliasProviderName(providerForDraft(providers, next)),
    existingNames,
    editing,
  });
  const changeSelectionDraft = (next) => setDraft(withAutoAlias(next));

  const patchDraft = (patch) => setDraft((current) => ({ ...current, ...patch }));
  const patchMetadataDraft = (patch) => setDraft((current) => (
    markModelMetadataOverrides(current, patch)
  ));
  const selectProvider = (nextProvider) => {
    setDraft((current) => withAutoAlias(applyCatalogProviderToDraft(current, nextProvider)));
    setApiKeyVisible(false);
    setFormError('');
    setConflict(null);
  };

  const submit = async () => {
    if (!provider || submitting) return;
    const built = buildModelMutationPayloads(draft, provider, { editing, existingNames });
    if (!built.ok) {
      setFormError(lookupErrorMessage(built.code));
      return;
    }
    setSubmitting(true);
    setFormError('');
    try {
      const result = await onSubmit({
        mode,
        originalName,
        payloads: built.payloads,
      });
      if (result?.conflict) {
        const suggestedName = modelNameSuggestion(
          result.conflict.payload.name,
          savedModels.map((model) => model.name),
        );
        setConflict({ ...result.conflict, suggested_name: suggestedName });
        setConflictMode('');
        setSaveAsName(suggestedName);
        return;
      }
      onClose?.();
    } catch (error) {
      setFormError(redactModelDraftSecrets(
        lookupErrorMessage(error?.code, error?.message),
        draft,
      ));
    } finally {
      setSubmitting(false);
    }
  };

  const resolveConflict = async (action, name = '') => {
    if (!conflict || submitting) return;
    setSubmitting(true);
    setFormError('');
    try {
      const result = await onResolveConflict({ action, conflict, name });
      if (result?.conflict) {
        const suggestedName = modelNameSuggestion(
          result.conflict.payload.name,
          savedModels.map((model) => model.name),
        );
        setConflict({ ...result.conflict, suggested_name: suggestedName });
        setConflictMode('');
        setSaveAsName(suggestedName);
        return;
      }
      onClose?.();
    } catch (error) {
      setFormError(redactModelDraftSecrets(
        lookupErrorMessage(error?.code, error?.message),
        draft,
      ));
    } finally {
      setSubmitting(false);
    }
  };

  const startSaveAs = () => {
    setConflictMode('save-as');
    window.requestAnimationFrame(() => saveAsRef.current?.focus());
  };

  const toggleAdvancedSettings = () => {
    if (advancedOpen) {
      setAdvancedOpen(false);
      return;
    }
    setAdvancedOpen(true);
    window.requestAnimationFrame(() => {
      const section = advancedSectionRef.current;
      if (!section) return;
      const reduceMotion = window.matchMedia?.('(prefers-reduced-motion: reduce)')?.matches;
      section.scrollIntoView({
        behavior: reduceMotion ? 'auto' : 'smooth',
        block: 'start',
        inline: 'nearest',
      });
    });
  };

  const selectedCapabilities = new Set(draft.capabilities || []);
  const toggleCapability = (capability) => {
    setDraft((current) => toggleModelCapability(current, capability));
  };

  const reasoning = draft.reasoning || {};
  const updateReasoning = (patch) => patchMetadataDraft({
    reasoning: { ...reasoning, ...patch },
  });
  const title = editing ? `编辑模型 · ${originalName}` : '新增模型';

  return (
    <Modal
      onClose={() => { if (!submitting) onClose?.(); }}
      width="min(920px, calc(100vw - 32px))"
      dismissOnBackdrop={false}
      dismissOnEscape={!submitting}
      layerClassName="z-[310]"
      labelledBy="model-profile-dialog-title"
    >
      <div
        className="flex max-h-[min(820px,calc(100vh-32px))] flex-col"
        aria-busy={submitting}
      >
        <header className="flex shrink-0 items-start gap-3 border-b border-border px-5 py-4">
          <div className="min-w-0 flex-1">
            <h2 id="model-profile-dialog-title" className="text-[15px] font-semibold text-fg">
              {title}
            </h2>
            <p className="mt-1 text-[11px] leading-5 text-fg-mute">
              选择 Provider 和模型后，只显示当前连接真正支持的配置。
            </p>
          </div>
          <button
            type="button"
            onClick={() => onClose?.()}
            disabled={submitting}
            className="flex h-8 w-8 shrink-0 items-center justify-center rounded-md text-fg-mute transition hover:bg-surface-hi hover:text-fg focus:outline-none focus:ring-1 focus:ring-accent disabled:opacity-40"
            aria-label="关闭模型弹窗"
          >
            <VsIcon name="close" size={14} />
          </button>
        </header>

        <div className="min-h-0 flex-1 space-y-4 overflow-y-auto px-5 py-4">
          <ProviderCatalogPicker
            apiClient={apiClient}
            providers={providers}
            provider={provider}
            draft={draft}
            allowMultiple={!editing}
            directModelDetails={customCompatibilityApi ? (
              <CustomCompatibilityApiFields
                draft={draft}
                policy={policy}
                editing={editing}
                existingNames={existingNames}
                apiKeyVisible={apiKeyVisible}
                onPatchDraft={patchDraft}
                onToggleApiKey={() => setApiKeyVisible((visible) => !visible)}
                testStatus={testStatus}
                onTest={testConnection}
                submitting={submitting}
              />
            ) : null}
            managedAuthenticated={managedAuthenticated}
            managedConnection={managedConnection}
            onProviderChange={selectProvider}
            onDraftChange={changeSelectionDraft}
          />

          {!customCompatibilityApi && (
            <ModelAliasField
              draft={draft}
              editing={editing}
              existingNames={existingNames}
              onPatchDraft={patchDraft}
            />
          )}

          {!policy?.managed && !customCompatibilityApi && (
            <div className="grid grid-cols-1 gap-3 md:grid-cols-2">
              {policy?.show_api_protocol && (
                <ModelApiProtocolField draft={draft} onPatchDraft={patchDraft} />
              )}
              {policy?.show_base_url && (
                <div className={policy.show_api_key ? '' : 'md:col-span-2'}>
                  {fieldLabel('model-base-url', draft.endpoint_mode === 'full_url' ? '完整端点 URL' : 'Base URL')}
                  <input
                    id="model-base-url"
                    type="url"
                    value={draft.base_url}
                    onChange={(event) => patchDraft({ base_url: event.target.value })}
                    readOnly={!policy.edit_base_url}
                    className={inputClass(!policy.edit_base_url && 'bg-surface-alt text-fg-mute')}
                    spellCheck={false}
                  />
                </div>
              )}
              {policy?.show_api_key && (
                <div>
                  {fieldLabel('model-api-key', 'API Key')}
                  <ModelApiKeyInput
                    draft={draft}
                    apiKeyVisible={apiKeyVisible}
                    onPatchDraft={patchDraft}
                    onToggleApiKey={() => setApiKeyVisible((visible) => !visible)}
                    testStatus={testStatus}
                    onTest={testConnection}
                    submitting={submitting}
                  />
                </div>
              )}
            </div>
          )}

          {editing && policy?.can_clear_api_key && draft.has_api_key && (
            <label className="flex items-center gap-2 rounded-md border border-border bg-surface px-3.5 py-2.5 text-[11px] text-fg-2">
              <input
                type="checkbox"
                checked={!!draft.clear_api_key}
                onChange={(event) => {
                  setApiKeyVisible(false);
                  patchDraft({
                    clear_api_key: event.target.checked,
                    api_key: '',
                  });
                }}
                className="accent-accent"
              />
              清除已保存密钥
            </label>
          )}

          <div
            ref={advancedSectionRef}
            className="scroll-mt-4 rounded-md border border-border bg-surface"
          >
            <button
              type="button"
              onClick={toggleAdvancedSettings}
              aria-expanded={advancedOpen}
              aria-controls="model-profile-advanced-settings"
              className="flex w-full items-center gap-2 px-3.5 py-2.5 text-left text-[11px] font-normal text-fg-2 transition hover:bg-surface-hi focus:outline-none focus:ring-1 focus:ring-inset focus:ring-accent"
            >
              <VsIcon name={advancedOpen ? 'expandDown' : 'expandRight'} size={12} />
              高级设置
            </button>
            {advancedOpen && (
              <div
                id="model-profile-advanced-settings"
                className="space-y-4 border-t border-border px-3.5 py-3"
              >
                {policy?.show_endpoint_mode && (
                  <fieldset>
                    <legend className="mb-1.5 text-[11px] font-normal text-fg-2">端点模式</legend>
                    <div className="flex flex-wrap gap-2">
                      {provider.endpoint_modes.map((modeValue) => (
                        <button
                          key={modeValue}
                          type="button"
                          onClick={() => patchDraft({ endpoint_mode: modeValue })}
                          aria-pressed={draft.endpoint_mode === modeValue}
                          className={clsx(
                            'rounded-md border px-3 py-1.5 text-[11px] transition focus:outline-none focus:ring-1 focus:ring-accent',
                            draft.endpoint_mode === modeValue
                              ? 'border-accent-soft bg-accent-bg text-accent'
                              : 'border-border bg-surface text-fg-mute hover:bg-surface-hi hover:text-fg',
                          )}
                        >
                          {modeValue === 'full_url' ? '完整端点' : 'Base URL'}
                        </button>
                      ))}
                    </div>
                  </fieldset>
                )}

                <div className="grid grid-cols-1 gap-3 md:grid-cols-2">
                  <div>
                    {fieldLabel('model-context-window', '上下文窗口 Token', true)}
                    <input
                      id="model-context-window"
                      type="number"
                      min="1"
                      step="1"
                      value={draft.context_window}
                      onChange={(event) => patchMetadataDraft({
                        context_window: event.target.value,
                      })}
                      className={inputClass()}
                      placeholder="例如 128000"
                    />
                  </div>
                  {policy?.show_max_output && (
                    <div>
                      {fieldLabel('model-max-output', '最大输出 Token', true)}
                      <input
                        id="model-max-output"
                        type="number"
                        min="1"
                        step="1"
                        value={draft.max_output_tokens}
                        onChange={(event) => patchMetadataDraft({
                          max_output_tokens: event.target.value,
                        })}
                        className={inputClass()}
                        placeholder="由目录提供或手动覆盖"
                      />
                    </div>
                  )}
                </div>

                <fieldset>
                  <legend className="mb-1.5 text-[11px] font-normal text-fg-2">模型能力</legend>
                  <div className="flex flex-wrap gap-1.5">
                    {MODEL_CAPABILITY_OPTIONS.map((option) => {
                      const active = selectedCapabilities.has(option.id);
                      return (
                        <button
                          key={option.id}
                          type="button"
                          onClick={() => toggleCapability(option.id)}
                          aria-pressed={active}
                          className={clsx(
                            'rounded-md border px-2.5 py-1.5 text-[10px] transition focus:outline-none focus:ring-1 focus:ring-accent',
                            active
                              ? 'border-accent-soft bg-accent-bg text-accent'
                              : 'border-border bg-surface text-fg-mute hover:bg-surface-hi hover:text-fg',
                          )}
                        >
                          {option.label}
                        </button>
                      );
                    })}
                  </div>
                </fieldset>

                {policy?.show_reasoning && reasoning.supported && (
                  <fieldset className="space-y-3 rounded-md border border-border bg-surface-alt px-3 py-2.5">
                    <legend className="px-1 text-[11px] font-normal text-fg-2">推理设置</legend>
                    <div className="flex items-center justify-between gap-3">
                      <div>
                        <div className="text-[11px] text-fg">启用推理</div>
                        <div className="mt-0.5 text-[10px] text-fg-mute">
                          {reasoning.mandatory ? '此模型要求始终启用推理' : '仅在 Provider 明确支持时发送'}
                        </div>
                      </div>
                      <Toggle
                        on={reasoning.mandatory || (reasoning.enabled ?? reasoning.default_enabled)}
                        onChange={(enabled) => updateReasoning({ enabled })}
                        disabled={reasoning.mandatory}
                        ariaLabel="启用模型推理"
                      />
                    </div>
                    {isCustomReasoningDraft(draft) && (
                      <fieldset>
                        <legend className="mb-1.5 text-[11px] font-normal text-fg-2">可选思考深度</legend>
                        <div className="flex flex-wrap gap-x-3 gap-y-2">
                          {MODEL_REASONING_EFFORTS.map((effort) => (
                            <label key={effort} className="flex items-center gap-1.5 text-[11px] text-fg">
                              <input
                                type="checkbox"
                                checked={reasoning.supported_efforts?.includes(effort) || false}
                                onChange={(event) => setDraft((current) => updateModelReasoningEfforts(current, effort, event.target.checked))}
                                className="accent-accent"
                              />
                              {effort}
                            </label>
                          ))}
                        </div>
                      </fieldset>
                    )}
                    {reasoning.supported_efforts?.length > 0 && (
                      <div>
                        {fieldLabel('model-reasoning-effort', '推理强度', true)}
                        <select
                          id="model-reasoning-effort"
                          value={reasoning.effort || ''}
                          onChange={(event) => updateReasoning({ effort: event.target.value })}
                          className={inputClass('cursor-pointer')}
                        >
                          <option value="">{`使用默认值 ${reasoning.default_effort || ''}`}</option>
                          {reasoning.supported_efforts.map((effort) => (
                            <option key={effort} value={effort}>{effort}</option>
                          ))}
                        </select>
                      </div>
                    )}
                    {reasoning.supports_max_tokens && (
                      <div>
                        {fieldLabel('model-reasoning-budget', '推理 Token 预算', true)}
                        <input
                          id="model-reasoning-budget"
                          type="number"
                          min="1"
                          step="1"
                          value={reasoning.max_tokens || ''}
                          onChange={(event) => updateReasoning({ max_tokens: event.target.value })}
                          className={inputClass()}
                        />
                      </div>
                    )}
                  </fieldset>
                )}

                {policy?.show_request_headers && (
                  <div>
                    {fieldLabel('model-request-headers', '自定义请求头 JSON', true)}
                    <textarea
                      id="model-request-headers"
                      value={draft.request_headers_json}
                      onChange={(event) => patchDraft({ request_headers_json: event.target.value })}
                      className="min-h-[96px] w-full resize-y rounded-md border border-border bg-surface px-3 py-2 font-mono text-[11px] leading-5 text-fg outline-none transition placeholder:text-fg-mute focus:border-accent focus:ring-1 focus:ring-accent-soft"
                      placeholder={'{"X-Team":"acecode","Authorization":"Bearer {env:ACE_TOKEN}"}'}
                      spellCheck={false}
                    />
                  </div>
                )}
              </div>
            )}
          </div>

          {conflict && (
            <div role="alert" className="rounded-md border border-warn bg-warn-bg px-3.5 py-3 text-[11px] text-warn">
              <div className="font-normal">{`已存在名为“${conflict.payload.name}”的预设`}</div>
              <div className="mt-1 leading-5">请选择覆盖已有预设、另存为新名称，或取消本次冲突处理。</div>
              {conflictMode === 'save-as' ? (
                <div className="mt-2 flex flex-wrap items-center gap-2">
                  <label htmlFor="model-conflict-name" className="sr-only">另存为预设名称</label>
                  <input
                    ref={saveAsRef}
                    id="model-conflict-name"
                    type="text"
                    value={saveAsName}
                    onChange={(event) => setSaveAsName(event.target.value)}
                    className="h-8 min-w-[220px] flex-1 rounded-md border border-warn bg-surface px-2.5 text-[11px] text-fg outline-none focus:ring-1 focus:ring-warn"
                  />
                  <button
                    type="button"
                    onClick={() => resolveConflict('save-as', saveAsName)}
                    disabled={!saveAsName.trim() || submitting}
                    className="h-8 rounded-md bg-accent px-3 text-[11px] font-normal text-white disabled:opacity-50"
                  >
                    使用此名称保存
                  </button>
                  <button
                    type="button"
                    onClick={() => setConflictMode('')}
                    disabled={submitting}
                    className="h-8 rounded-md border border-border bg-surface px-3 text-[11px] text-fg-2 disabled:opacity-50"
                  >
                    返回
                  </button>
                </div>
              ) : (
                <div className="mt-2 flex flex-wrap gap-2">
                  <button
                    type="button"
                    onClick={() => resolveConflict('overwrite')}
                    disabled={submitting}
                    className="h-8 rounded-md border border-warn bg-surface px-3 text-[11px] font-normal text-warn transition hover:bg-surface-hi disabled:opacity-50"
                  >
                    覆盖已有预设
                  </button>
                  <button
                    type="button"
                    onClick={startSaveAs}
                    disabled={submitting}
                    className="h-8 rounded-md bg-accent px-3 text-[11px] font-normal text-white transition hover:opacity-90 disabled:opacity-50"
                  >
                    另存为
                  </button>
                  <button
                    type="button"
                    onClick={() => setConflict(null)}
                    disabled={submitting}
                    className="h-8 rounded-md border border-border bg-surface px-3 text-[11px] text-fg-2 transition hover:bg-surface-hi disabled:opacity-50"
                  >
                    取消
                  </button>
                </div>
              )}
            </div>
          )}

          {formError && (
            <div role="alert" className="rounded-md border border-danger bg-danger-bg px-3.5 py-2.5 text-[11px] text-danger">
              {formError}
            </div>
          )}
        </div>

        <footer className="flex shrink-0 flex-wrap items-center justify-end gap-2 border-t border-border bg-surface-alt px-5 py-3">
          <button
            type="button"
            onClick={() => onClose?.()}
            disabled={submitting}
            className="h-8 rounded-md border border-border bg-surface px-3.5 text-[11px] font-normal text-fg-2 transition hover:bg-surface-hi focus:outline-none focus:ring-1 focus:ring-accent disabled:opacity-50"
          >
            取消
          </button>
          <button
            type="button"
            data-ace-dialog-primary="true"
            onClick={submit}
            disabled={submitting || !provider || !draft.model}
            className="inline-flex h-8 items-center gap-1.5 rounded-md bg-accent px-4 text-[11px] font-normal text-white transition hover:opacity-90 focus:outline-none focus:ring-2 focus:ring-accent-soft disabled:cursor-not-allowed disabled:opacity-50"
          >
            {submitting && <span className="ace-spinner" />}
            {editing ? '保存修改' : '保存模型'}
          </button>
        </footer>
      </div>
    </Modal>
  );
}
