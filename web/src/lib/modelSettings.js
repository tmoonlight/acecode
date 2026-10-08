// Model settings catalog, adaptive form, and mutation helpers.
//
// The daemon contract is deliberately snake_case. These helpers fail fast for
// missing required catalog data instead of fabricating Provider behavior in
// React. Authenticated model-management responses may include saved API keys
// so the edit dialog can round-trip the original value.

import {
  ANTHROPIC_DEFAULT_BASE_URL,
  OPENAI_DEFAULT_BASE_URL,
  buildModelDraftsFromSelection,
  formatRequestHeadersJson,
  normalizeModelCapabilities,
  normalizeModelProbeResult,
  parseRequestHeadersJson,
  splitModelIds,
} from './modelManager.js';
import { autoModelAliasForSelection, uniqueModelAlias } from './modelAlias.js';
import { providerDisplayName } from './providerCatalogGroups.js';

export const MODEL_CATALOG_QUERY_LIMIT = 50;
export const MODEL_ENDPOINT_MODES = ['base_url', 'full_url'];
export const MODEL_API_PROTOCOLS = ['chat_completions', 'responses'];
import { MODEL_REASONING_EFFORTS } from './modelReasoning.js';
export { MODEL_REASONING_EFFORTS } from './modelReasoning.js';

const RUNTIME_PROVIDERS = new Set(['openai', 'anthropic', 'copilot', 'grok']);
const AUTH_MODES = new Set(['required', 'optional', 'none', 'managed']);
const MODEL_INPUT_MODES = new Set(['catalog', 'manual']);
const PROVIDER_GROUPS = new Set(['first_party', 'native', 'local', 'catalog', 'custom']);
const MODEL_METADATA_FIELDS = [
  'context_window',
  'max_output_tokens',
  'capabilities',
  'reasoning',
];

function isObject(value) {
  return !!value && typeof value === 'object' && !Array.isArray(value);
}

function contractError(message) {
  const error = new TypeError(message);
  error.code = 'MODEL_CATALOG_CONTRACT';
  return error;
}

function requireObject(value, label) {
  if (!isObject(value)) throw contractError(`${label} must be an object`);
  return value;
}

function requireArray(value, label) {
  if (!Array.isArray(value)) throw contractError(`${label} must be an array`);
  return value;
}

function requireString(value, label) {
  const normalized = String(value ?? '').trim();
  if (!normalized) throw contractError(`${label} must be a non-empty string`);
  return normalized;
}

function requireText(value, label) {
  if (typeof value !== 'string') throw contractError(`${label} must be a string`);
  return value.trim();
}

function requireNullableText(value, label) {
  if (value === null) return '';
  return requireText(value, label);
}

function optionalString(value, label = 'value') {
  if (value === undefined || value === null) return '';
  if (typeof value !== 'string') throw contractError(`${label} must be a string`);
  return value.trim();
}

function requireNonNegativeInteger(value, label) {
  if (typeof value !== 'number' || !Number.isSafeInteger(value) || value < 0) {
    throw contractError(`${label} must be a non-negative integer`);
  }
  return value;
}

function optionalNonNegativeNumber(value, label) {
  if (value === undefined || value === null) return null;
  if (typeof value !== 'number' || !Number.isFinite(value) || value < 0) {
    throw contractError(`${label} must be a non-negative number`);
  }
  return value;
}

function optionalStringArray(value, label) {
  if (value === undefined) return [];
  if (!Array.isArray(value)) throw contractError(`${label} must be an array`);
  return value.map((item, index) => requireString(item, `${label}[${index}]`));
}

function optionalPositiveInteger(value, label = 'value', { allowNumericString = false } = {}) {
  if (value === undefined || value === null || value === '') return null;
  if (typeof value !== 'number'
      && !(allowNumericString && typeof value === 'string' && value.trim())) {
    throw contractError(`${label} must be a positive integer`);
  }
  const parsed = Number(value);
  if (!Number.isInteger(parsed) || parsed <= 0 || parsed > 2147483647) {
    throw contractError(`${label} must be a positive integer`);
  }
  return parsed;
}

function optionalCatalogLimit(value, label) {
  // models.dev uses numeric zero for limits that do not apply (for example,
  // image/video generators). Catalog metadata treats that sentinel as
  // unknown; persisted model profiles remain strictly positive.
  if (value === 0) return null;
  return optionalPositiveInteger(value, label);
}

function requireBoolean(value, label) {
  if (typeof value !== 'boolean') throw contractError(`${label} must be a boolean`);
  return value;
}

function optionalBoolean(value, label, fallback = false) {
  if (value === undefined) return fallback;
  if (typeof value !== 'boolean') throw contractError(`${label} must be a boolean`);
  return value;
}

function normalizeResponseCapabilities(value, label) {
  if (value === undefined) return [];
  if (!Array.isArray(value)) throw contractError(`${label} must be an array`);
  const normalized = normalizeModelCapabilities(value);
  if (normalized.length !== value.length) {
    throw contractError(`${label} contains an invalid or duplicate capability`);
  }
  return normalized;
}

function optionalReasoningString(value, label) {
  if (value === undefined || value === null) return '';
  if (typeof value !== 'string') throw contractError(`${label} must be a string`);
  return value.trim();
}

function normalizeReasoning(value, label = 'reasoning', { draft = false } = {}) {
  if (value === undefined || value === null) {
    return {
      supported: false,
      mandatory: false,
      default_enabled: false,
      enabled: null,
      supported_efforts: [],
      default_effort: '',
      effort: '',
      supports_max_tokens: false,
      max_tokens: null,
    };
  }
  requireObject(value, label);
  let efforts = [];
  if (value.supported_efforts !== undefined) {
    if (!Array.isArray(value.supported_efforts)) {
      throw contractError(`${label}.supported_efforts must be an array`);
    }
    efforts = [...new Set(value.supported_efforts.map((effort, index) => {
      const normalized = requireString(effort, `${label}.supported_efforts[${index}]`);
      if (!MODEL_REASONING_EFFORTS.includes(normalized)) {
        throw contractError(`${label}.supported_efforts[${index}] is unsupported`);
      }
      return normalized;
    }))];
  }
  const supported = optionalBoolean(value.supported, `${label}.supported`, false);
  const declaredMandatory = optionalBoolean(value.mandatory, `${label}.mandatory`, false);
  const declaredDefaultEnabled = optionalBoolean(
    value.default_enabled,
    `${label}.default_enabled`,
    false,
  );
  const declaredSupportsMaxTokens = optionalBoolean(
    value.supports_max_tokens,
    `${label}.supports_max_tokens`,
    false,
  );
  const mandatory = declaredMandatory;
  const defaultEnabled = mandatory || declaredDefaultEnabled;
  const enabled = value.enabled === undefined || value.enabled === null
    ? null
    : optionalBoolean(value.enabled, `${label}.enabled`);
  const defaultEffort = optionalReasoningString(value.default_effort, `${label}.default_effort`);
  const effort = optionalReasoningString(value.effort, `${label}.effort`);
  for (const [field, current] of [['default_effort', defaultEffort], ['effort', effort]]) {
    if (current && (!MODEL_REASONING_EFFORTS.includes(current) || !efforts.includes(current))) {
      throw contractError(`${label}.${field} is unsupported`);
    }
  }
  const maxTokens = optionalPositiveInteger(
    value.max_tokens,
    `${label}.max_tokens`,
    { allowNumericString: draft },
  );
  if (!draft && !supported && (
    declaredMandatory
      || declaredDefaultEnabled
      || declaredSupportsMaxTokens
      || enabled !== null
      || efforts.length > 0
      || defaultEffort
      || effort
      || maxTokens
  )) {
    throw contractError(`${label} options require supported=true`);
  }
  if (!draft && mandatory && value.default_enabled === false) {
    throw contractError(`${label}.mandatory requires default_enabled=true`);
  }
  if (!draft && mandatory && enabled === false) {
    throw contractError(`${label}.mandatory cannot be disabled`);
  }
  if (!draft && maxTokens && !declaredSupportsMaxTokens) {
    throw contractError(`${label}.max_tokens is unsupported`);
  }
  return {
    supported,
    mandatory,
    default_enabled: defaultEnabled,
    enabled,
    supported_efforts: efforts,
    default_effort: defaultEffort,
    effort,
    supports_max_tokens: declaredSupportsMaxTokens,
    max_tokens: maxTokens,
  };
}

function normalizeCatalogProvider(raw, index) {
  const value = requireObject(raw, `providers[${index}]`);
  const id = requireString(value.id, `providers[${index}].id`);
  const runtimeProvider = requireString(
    value.runtime_provider,
    `providers[${index}].runtime_provider`,
  );
  if (!RUNTIME_PROVIDERS.has(runtimeProvider)) {
    throw contractError(`providers[${index}].runtime_provider is unsupported`);
  }
  const authMode = requireString(value.auth_mode, `providers[${index}].auth_mode`);
  if (!AUTH_MODES.has(authMode)) {
    throw contractError(`providers[${index}].auth_mode is unsupported`);
  }
  const endpointEditable = requireBoolean(
    value.endpoint_editable,
    `providers[${index}].endpoint_editable`,
  );
  const modelInput = requireString(value.model_input, `providers[${index}].model_input`);
  if (!MODEL_INPUT_MODES.has(modelInput)) {
    throw contractError(`providers[${index}].model_input is unsupported`);
  }
  if (!Array.isArray(value.endpoint_modes)) {
    throw contractError(`providers[${index}].endpoint_modes must be an array`);
  }
  const endpointModes = [...new Set(value.endpoint_modes.map((mode, modeIndex) => {
    const normalized = requireString(mode, `providers[${index}].endpoint_modes[${modeIndex}]`);
    if (!MODEL_ENDPOINT_MODES.includes(normalized)) {
      throw contractError(`providers[${index}].endpoint_modes[${modeIndex}] is unsupported`);
    }
    return normalized;
  }))];
  const managedRuntime = runtimeProvider === 'copilot' || runtimeProvider === 'grok';
  const managedLabel = runtimeProvider === 'copilot' ? 'Copilot' : 'Grok';
  if (managedRuntime && (authMode !== 'managed' || endpointEditable)) {
    throw contractError(`${managedLabel} must use managed auth and a managed endpoint`);
  }
  if (managedRuntime && endpointModes.includes('full_url')) {
    throw contractError(`${managedLabel} must use a managed endpoint mode`);
  }
  if (!managedRuntime && endpointModes.length === 0) {
    throw contractError(`providers[${index}].endpoint_modes must not be empty`);
  }
  const group = requireString(value.group, `providers[${index}].group`);
  if (!PROVIDER_GROUPS.has(group)) {
    throw contractError(`providers[${index}].group is unsupported`);
  }
  return {
    id,
    name: requireString(value.name, `providers[${index}].name`),
    runtime_provider: runtimeProvider,
    base_url: requireText(value.base_url, `providers[${index}].base_url`),
    doc: requireText(value.doc, `providers[${index}].doc`),
    api_key_env: requireText(value.api_key_env, `providers[${index}].api_key_env`),
    auth_mode: authMode,
    endpoint_editable: endpointEditable,
    endpoint_modes: endpointModes,
    model_input: modelInput,
    models_dev_provider_id: requireNullableText(
      value.models_dev_provider_id,
      `providers[${index}].models_dev_provider_id`,
    ),
    group,
  };
}

export function normalizeCatalogModel(raw, index = 0) {
  const value = requireObject(raw, `models[${index}]`);
  const pricing = value.pricing === undefined
    ? null
    : requireObject(value.pricing, `models[${index}].pricing`);
  return {
    id: requireString(value.id, `models[${index}].id`),
    name: optionalString(value.name, `models[${index}].name`)
      || requireString(value.id, `models[${index}].id`),
    context_window: optionalCatalogLimit(value.context_window, `models[${index}].context_window`),
    max_output_tokens: optionalCatalogLimit(
      value.max_output_tokens,
      `models[${index}].max_output_tokens`,
    ),
    capabilities: normalizeResponseCapabilities(value.capabilities, `models[${index}].capabilities`),
    reasoning: normalizeReasoning(value.reasoning, `models[${index}].reasoning`),
    deprecated: optionalBoolean(value.deprecated, `models[${index}].deprecated`, false),
    unavailable: optionalBoolean(value.unavailable, `models[${index}].unavailable`, false),
    pricing: pricing ? {
      input: optionalNonNegativeNumber(pricing.input, `models[${index}].pricing.input`),
      output: optionalNonNegativeNumber(pricing.output, `models[${index}].pricing.output`),
    } : null,
    input_modalities: optionalStringArray(
      value.input_modalities,
      `models[${index}].input_modalities`,
    ),
    output_modalities: optionalStringArray(
      value.output_modalities,
      `models[${index}].output_modalities`,
    ),
    knowledge_cutoff: optionalString(
      value.knowledge_cutoff,
      `models[${index}].knowledge_cutoff`,
    ),
  };
}

export function normalizeModelCatalogSummary(payload) {
  const value = requireObject(payload, 'catalog response');
  const catalog = requireObject(value.catalog, 'catalog response.catalog');
  const providers = requireArray(value.providers, 'catalog response.providers')
    .map(normalizeCatalogProvider);
  if (new Set(providers.map((provider) => provider.id)).size !== providers.length) {
    throw contractError('catalog response contains duplicate provider ids');
  }
  return {
    catalog: {
      source: requireString(catalog.source, 'catalog response.catalog.source'),
      version: requireNonNegativeInteger(
        catalog.version,
        'catalog response.catalog.version',
      ),
      updated_at: requireText(
        catalog.updated_at,
        'catalog response.catalog.updated_at',
      ),
      freshness: requireString(catalog.freshness, 'catalog response.catalog.freshness'),
    },
    providers,
  };
}

export function normalizeProviderModelQuery(payload, expectedProviderId = '') {
  const value = requireObject(payload, 'provider model response');
  const providerId = requireString(value.provider_id, 'provider model response.provider_id');
  if (expectedProviderId && providerId !== expectedProviderId) {
    throw contractError('provider model response does not match requested provider');
  }
  return {
    provider_id: providerId,
    models: requireArray(value.models, 'provider model response.models').map(normalizeCatalogModel),
    limit: optionalPositiveInteger(value.limit, 'provider model response.limit')
      || MODEL_CATALOG_QUERY_LIMIT,
  };
}

export function normalizeSavedModelProfile(raw, index = 0) {
  const value = requireObject(raw, `saved_models[${index}]`);
  const provider = requireString(value.provider, `saved_models[${index}].provider`);
  if (!RUNTIME_PROVIDERS.has(provider)) {
    throw contractError(`saved_models[${index}].provider is unsupported`);
  }
  const apiKey = optionalString(value.api_key, `saved_models[${index}].api_key`);
  const apiProtocol = optionalString(value.api_protocol, `saved_models[${index}].api_protocol`);
  if (apiProtocol && (provider !== 'openai' || !MODEL_API_PROTOCOLS.includes(apiProtocol))) {
    throw contractError(`saved_models[${index}].api_protocol is unsupported`);
  }
  return {
    ...value,
    name: requireString(value.name, `saved_models[${index}].name`),
    provider,
    model: requireString(value.model, `saved_models[${index}].model`),
    base_url: optionalString(value.base_url, `saved_models[${index}].base_url`),
    models_dev_provider_id: optionalString(
      value.models_dev_provider_id,
      `saved_models[${index}].models_dev_provider_id`,
    ),
    context_window: optionalPositiveInteger(
      value.context_window,
      `saved_models[${index}].context_window`,
    ),
    max_output_tokens: optionalPositiveInteger(
      value.max_output_tokens,
      `saved_models[${index}].max_output_tokens`,
    ),
    capabilities: normalizeResponseCapabilities(
      value.capabilities,
      `saved_models[${index}].capabilities`,
    ),
    capabilities_source: optionalString(
      value.capabilities_source,
      `saved_models[${index}].capabilities_source`,
    ),
    endpoint_mode: value.endpoint_mode === undefined || value.endpoint_mode === null
      ? 'base_url'
      : MODEL_ENDPOINT_MODES.includes(value.endpoint_mode)
        ? value.endpoint_mode
        : (() => { throw contractError(`saved_models[${index}].endpoint_mode is unsupported`); })(),
    api_protocol: apiProtocol,
    reasoning: normalizeReasoning(value.reasoning, `saved_models[${index}].reasoning`),
    request_headers: value.request_headers === undefined || value.request_headers === null
      ? undefined
      : isObject(value.request_headers)
        ? value.request_headers
        : (() => { throw contractError(`saved_models[${index}].request_headers must be an object`); })(),
    api_key: apiKey,
    has_api_key: optionalBoolean(
      value.has_api_key,
      `saved_models[${index}].has_api_key`,
      !!apiKey,
    ),
  };
}

export function normalizeSavedModelList(payload) {
  return requireArray(payload, 'saved models response').map(normalizeSavedModelProfile);
}

export function providerForSavedModel(providers, model) {
  const choices = Array.isArray(providers) ? providers : [];
  const normalizeIdentity = (value) => (
    typeof value === 'string' ? value.trim().toLowerCase() : ''
  );
  const runtimeProvider = normalizeIdentity(model?.provider);
  const providerHint = normalizeIdentity(model?.models_dev_provider_id);
  if (!runtimeProvider) return null;

  if (providerHint) {
    return choices.find((provider) => (
      normalizeIdentity(provider?.runtime_provider) === runtimeProvider
        && (normalizeIdentity(provider?.id) === providerHint
          || normalizeIdentity(provider?.models_dev_provider_id) === providerHint)
    )) || null;
  }

  if (runtimeProvider === 'openai') {
    return choices.find((provider) => (
      normalizeIdentity(provider?.runtime_provider) === 'openai'
        && normalizeIdentity(provider?.id) === 'custom-openai'
    )) || null;
  }

  return choices.find((provider) => (
    normalizeIdentity(provider?.runtime_provider) === runtimeProvider
      && normalizeIdentity(provider?.id) === runtimeProvider
  ))
    || choices.find((provider) => (
      normalizeIdentity(provider?.runtime_provider) === runtimeProvider
        && !normalizeIdentity(provider?.models_dev_provider_id)
    ))
    || choices.find((provider) => (
      normalizeIdentity(provider?.runtime_provider) === runtimeProvider
    ))
    || null;
}

function emptyModelMetadataDraft() {
  return {
    context_window: '',
    max_output_tokens: '',
    capabilities: [],
    capabilities_source: '',
    reasoning: normalizeReasoning(null),
  };
}

function modelMetadataDraftFromNormalized(model) {
  return {
    context_window: model.context_window ? String(model.context_window) : '',
    max_output_tokens: model.max_output_tokens ? String(model.max_output_tokens) : '',
    capabilities: [...model.capabilities],
    capabilities_source: 'catalog',
    reasoning: {
      ...model.reasoning,
      supported_efforts: [...model.reasoning.supported_efforts],
    },
  };
}

function catalogModelMetadataDraft(raw, draft) {
  const metadata = modelMetadataDraftFromNormalized(normalizeCatalogModel(raw));
  if (isCustomReasoningDraft(draft)) {
    metadata.capabilities = metadata.capabilities.filter((item) => item !== 'reasoning');
    metadata.reasoning = normalizeReasoning(null);
  }
  return metadata;
}

function draftMetadataOverrides(draft) {
  return isObject(draft?._model_metadata_overrides)
    ? draft._model_metadata_overrides
    : {};
}

function catalogMetadataMap(draft) {
  return isObject(draft?._catalog_model_metadata)
    ? draft._catalog_model_metadata
    : {};
}

function isAceModelDraft(draft) {
  return draft?.models_dev_provider_id === 'acemodel' || draft?.catalog_provider_id === 'acemodel';
}

function activeDraftModelId(draft) {
  const selected = splitModelIds(draft?.model);
  const active = String(draft?._active_catalog_model_id || '');
  return selected.includes(active) ? active : (selected.length === 1 ? selected[0] : '');
}

function applyVisibleModelMetadata(draft, metadata, overrides) {
  const next = { ...draft };
  for (const field of MODEL_METADATA_FIELDS) {
    if (!overrides[field]) next[field] = metadata[field];
  }
  if (!overrides.capabilities) next.capabilities_source = metadata.capabilities_source;
  return next;
}

export function markModelMetadataOverrides(draft, patch) {
  const overrides = { ...draftMetadataOverrides(draft) };
  for (const field of MODEL_METADATA_FIELDS) {
    if (Object.prototype.hasOwnProperty.call(patch, field)) overrides[field] = true;
  }
  const next = {
    ...draft,
    ...patch,
    _model_metadata_overrides: overrides,
  };
  if (overrides.capabilities) next.capabilities_source = 'manual';
  // ACEModel capabilities are per model. Editing the visible selection must
  // never spread its vision/tool/reasoning settings to other selected IDs.
  const activeId = activeDraftModelId(draft);
  if (isAceModelDraft(draft) && activeId
      && (Object.hasOwn(patch, 'capabilities') || Object.hasOwn(patch, 'reasoning'))) {
    const metadata = {
      ...emptyModelMetadataDraft(),
      ...Object.fromEntries(MODEL_METADATA_FIELDS.map((field) => [field, draft[field]])),
      capabilities_source: draft.capabilities_source,
      ...catalogMetadataMap(draft)[activeId],
    };
    for (const field of ['capabilities', 'reasoning']) {
      if (!Object.hasOwn(patch, field)) continue;
      metadata[field] = patch[field];
      delete overrides[field];
    }
    if (Object.hasOwn(patch, 'capabilities')) metadata.capabilities_source = 'manual';
    next._catalog_model_metadata = { ...catalogMetadataMap(draft), [activeId]: metadata };
    next._active_catalog_model_id = activeId;
  }
  return next;
}

export function isCustomReasoningDraft(draft) {
  return draft?.catalog_provider_id === 'custom-openai'
    || (draft?.provider === 'anthropic' && draft?.capabilities_source === 'manual')
    || (!draft?.catalog_provider_id && !draft?.models_dev_provider_id
      && ['openai', 'anthropic'].includes(draft?.provider));
}

export function modelRowsFromProbe(response, provider) {
  const normalized = normalizeModelProbeResult(response);
  const aceModel = provider?.id === 'acemodel' || provider?.models_dev_provider_id === 'acemodel';
  const custom = provider?.id === 'custom-openai';
  return normalized.models.map((id) => {
    const reasoning = custom ? null : normalized.reasoningByModel[id];
    const capabilities = normalized.capabilitiesByModel[id] || [];
    return {
      id, name: id,
      context_window: normalized.contextWindows[id] || null,
      max_output_tokens: null,
      capabilities: aceModel || custom
        ? [...capabilities.filter((item) => item !== 'reasoning'), ...(reasoning?.supported_efforts.length ? ['reasoning'] : [])]
        : capabilities,
      reasoning: aceModel && !reasoning?.supported_efforts.length ? null : reasoning,
    };
  });
}

export function updateModelReasoningEfforts(draft, effort, enabled) {
  if (!MODEL_REASONING_EFFORTS.includes(effort) || !draft?.reasoning?.supported) return draft;
  const chosen = new Set(draft.reasoning.supported_efforts || []);
  if (enabled) chosen.add(effort);
  else chosen.delete(effort);
  return markModelMetadataOverrides(draft, {
    reasoning: {
      ...draft.reasoning,
      supported_efforts: MODEL_REASONING_EFFORTS.filter((item) => chosen.has(item)),
      default_effort: chosen.has(draft.reasoning.default_effort) ? draft.reasoning.default_effort : '',
      effort: chosen.has(draft.reasoning.effort) ? draft.reasoning.effort : '',
    },
  });
}

export function toggleModelCapability(draft, capability) {
  const normalizedCapability = String(capability || '').trim();
  if (!normalizedCapability) return draft;
  const capabilities = new Set(normalizeModelCapabilities(draft?.capabilities));
  const enabling = !capabilities.has(normalizedCapability);
  if (enabling) capabilities.add(normalizedCapability);
  else capabilities.delete(normalizedCapability);
  const patch = { capabilities: [...capabilities] };
  if (normalizedCapability === 'reasoning') {
    patch.reasoning = {
      ...normalizeReasoning(null),
      supported: enabling,
      ...(enabling && isCustomReasoningDraft(draft) ? {
        default_enabled: true,
        enabled: true,
        supported_efforts: ['low', 'medium', 'high'],
      } : {}),
    };
  }
  return markModelMetadataOverrides(draft, patch);
}

export function toggleCatalogModelInDraft(
  draft,
  modelId,
  rawModel = null,
  { allowMultiple = false } = {},
) {
  const id = String(modelId || '').trim();
  if (!id) return draft;
  let selected = splitModelIds(draft?.model);
  const wasSelected = selected.includes(id);
  let metadataById = { ...catalogMetadataMap(draft) };
  let activeId = String(draft?._active_catalog_model_id || '');

  if (wasSelected) {
    selected = selected.filter((value) => value !== id);
    delete metadataById[id];
    if (activeId === id) activeId = '';
  } else {
    if (!allowMultiple) {
      selected = [];
      metadataById = {};
      activeId = '';
    }
    selected.push(id);
    if (rawModel) {
      metadataById[id] = catalogModelMetadataDraft(rawModel, draft);
      activeId = id;
    }
  }

  if (!activeId || !selected.includes(activeId) || !metadataById[activeId]) {
    activeId = [...selected].reverse().find((value) => metadataById[value]) || '';
  }
  const overrides = draftMetadataOverrides(draft);
  const visibleMetadata = activeId ? metadataById[activeId] : emptyModelMetadataDraft();
  // 别名不在这里写:曾经「name 为空就填第一个模型的目录显示名」,多选时再被
  // buildModelDraftsFromSelection 拼成 <第一个模型名>-<其他模型 ID>。现在别名
  // 统一由 syncAutoModelAlias 按整份选择维护。
  return applyVisibleModelMetadata({
    ...draft,
    model: selected.join(', '),
    _catalog_model_metadata: metadataById,
    _active_catalog_model_id: activeId,
  }, visibleMetadata, overrides);
}

export function addManualModelToDraft(draft, modelId, { allowMultiple = false } = {}) {
  const id = String(modelId || '').trim();
  if (!id) return draft;
  let selected = splitModelIds(draft?.model);
  if (selected.includes(id)) return draft;
  let metadataById = { ...catalogMetadataMap(draft) };
  let activeId = String(draft?._active_catalog_model_id || '');
  if (!allowMultiple) {
    selected = [];
    metadataById = {};
    activeId = '';
  }
  selected.push(id);
  if (!activeId || !selected.includes(activeId) || !metadataById[activeId]) {
    activeId = [...selected].reverse().find((value) => metadataById[value]) || '';
  }
  const overrides = draftMetadataOverrides(draft);
  const visibleMetadata = activeId ? metadataById[activeId] : emptyModelMetadataDraft();
  const next = applyVisibleModelMetadata({
    ...draft,
    model: selected.join(', '),
    _catalog_model_metadata: metadataById,
    _active_catalog_model_id: activeId,
  }, visibleMetadata, overrides);
  if (!activeId && !overrides.capabilities) next.capabilities_source = 'manual';
  return next;
}

export function replaceDraftModelsFromProbe(
  draft,
  rawModels,
  selectedModelIds,
  { allowMultiple = false } = {},
) {
  const selectedIds = new Set(splitModelIds(selectedModelIds));
  if (selectedIds.size === 0 || !Array.isArray(rawModels)) return draft;

  const seen = new Set();
  const selectedModels = [];
  for (const rawModel of rawModels) {
    const id = String(rawModel?.id || '').trim();
    if (!id || seen.has(id) || !selectedIds.has(id)) continue;
    seen.add(id);
    selectedModels.push({ raw: rawModel, id });
    if (!allowMultiple) break;
  }
  if (selectedModels.length === 0) return draft;

  const aceModel = isAceModelDraft(draft);
  const previousIds = new Set(splitModelIds(draft.model));
  const previousActiveId = activeDraftModelId(draft);
  const previousMetadata = catalogMetadataMap(draft);
  const metadataById = {};
  for (const model of selectedModels) {
    const metadata = catalogModelMetadataDraft(model.raw, draft);
    const previous = model.id === previousActiveId && draft.capabilities_source === 'manual'
      ? draft : previousMetadata[model.id];
    if (aceModel && previousIds.has(model.id) && previous?.capabilities_source === 'manual') {
      metadata.capabilities = [
        ...normalizeModelCapabilities(previous.capabilities).filter((item) => item !== 'reasoning'),
        ...(metadata.reasoning.supported && metadata.reasoning.supported_efforts.length ? ['reasoning'] : []),
      ];
      metadata.capabilities_source = 'manual';
    }
    metadataById[model.id] = metadata;
  }
  const activeId = selectedModels[0].id;
  const overrides = { ...draftMetadataOverrides(draft) };
  // Fresh reasoning declarations win; manual non-reasoning tags stay attached
  // to their existing model IDs in metadataById, never to a new/batch selection.
  if (aceModel) {
    delete overrides.reasoning;
    delete overrides.capabilities;
  }
  return applyVisibleModelMetadata({
    ...draft,
    _model_metadata_overrides: overrides,
    model: selectedModels.map((model) => model.id).join(', '),
    _catalog_model_metadata: metadataById,
    _active_catalog_model_id: activeId,
  }, metadataById[activeId], overrides);
}

export function emptyModelProfileDraft() {
  return {
    name: '',
    provider: 'openai',
    catalog_provider_id: '',
    models_dev_provider_id: '',
    model: '',
    base_url: OPENAI_DEFAULT_BASE_URL,
    endpoint_mode: 'base_url',
    api_protocol: '',
    api_key: '',
    has_api_key: false,
    clear_api_key: false,
    request_headers_json: '',
    context_window: '',
    max_output_tokens: '',
    capabilities: [],
    capabilities_source: '',
    reasoning: normalizeReasoning(null),
    _catalog_model_metadata: {},
    _model_metadata_overrides: {},
    _active_catalog_model_id: '',
    // 上一次自动生成的别名;name 仍等于它(或为空)说明用户没手改过,可继续跟随选择刷新。
    _auto_alias: '',
  };
}

export function modelProfileDraftFromSaved(raw) {
  const model = normalizeSavedModelProfile(raw);
  return {
    ...emptyModelProfileDraft(),
    name: model.name,
    provider: model.provider,
    catalog_provider_id: model.provider === 'anthropic' ? 'anthropic' :
      model.provider === 'copilot' ? 'copilot' :
        model.provider === 'grok' ? 'grok' :
          model.models_dev_provider_id || 'custom-openai',
    models_dev_provider_id: model.models_dev_provider_id,
    model: model.model,
    base_url: model.base_url || (
      model.provider === 'anthropic' ? ANTHROPIC_DEFAULT_BASE_URL :
        model.provider === 'openai' ? OPENAI_DEFAULT_BASE_URL : ''
    ),
    endpoint_mode: model.endpoint_mode,
    api_protocol: model.api_protocol,
    api_key: model.api_key,
    has_api_key: model.has_api_key || !!model.api_key,
    request_headers_json: formatRequestHeadersJson(model.request_headers),
    context_window: model.context_window ? String(model.context_window) : '',
    max_output_tokens: model.max_output_tokens ? String(model.max_output_tokens) : '',
    capabilities: model.capabilities,
    capabilities_source: model.capabilities_source,
    reasoning: model.reasoning,
  };
}

export function modelFieldPolicy(provider) {
  const value = requireObject(provider, 'provider');
  const managed = value.auth_mode === 'managed'
    || value.runtime_provider === 'copilot'
    || value.runtime_provider === 'grok';
  const supportsHttpOptions = !managed && (
    value.runtime_provider === 'openai' || value.runtime_provider === 'anthropic'
  );
  const endpointModes = Array.isArray(value.endpoint_modes) ? value.endpoint_modes : ['base_url'];
  return {
    managed,
    show_api_key: supportsHttpOptions && value.auth_mode !== 'none',
    api_key_required: value.auth_mode === 'required',
    can_clear_api_key: value.auth_mode === 'none' || value.auth_mode === 'optional',
    show_base_url: supportsHttpOptions && (!!value.base_url || !!value.endpoint_editable),
    edit_base_url: supportsHttpOptions && !!value.endpoint_editable,
    show_endpoint_mode: supportsHttpOptions && endpointModes.includes('full_url'),
    show_api_protocol: supportsHttpOptions && value.runtime_provider === 'openai',
    show_request_headers: supportsHttpOptions,
    show_max_output: !managed,
    show_reasoning: supportsHttpOptions,
    can_probe: value.runtime_provider === 'copilot'
      || value.runtime_provider === 'grok'
      || value.runtime_provider === 'openai',
    model_input: value.model_input,
  };
}

export function isCustomOpenAiCompatibilityProvider(provider) {
  return provider?.runtime_provider === 'openai'
    && provider?.model_input === 'manual';
}

export function applyCatalogProviderToDraft(draft, provider) {
  const policy = modelFieldPolicy(provider);
  return {
    ...emptyModelProfileDraft(),
    ...draft,
    provider: provider.runtime_provider,
    catalog_provider_id: provider.id,
    models_dev_provider_id: provider.models_dev_provider_id || '',
    model: '',
    base_url: policy.show_base_url ? provider.base_url : '',
    endpoint_mode: provider.endpoint_modes?.[0] || 'base_url',
    // An explicit provider switch must clear a previously selected Responses
    // protocol even when both providers share the openai runtime kind.
    api_protocol: provider.runtime_provider === 'openai' && draft?.api_protocol
      ? 'chat_completions' : '',
    api_key: '',
    has_api_key: false,
    clear_api_key: !!draft?.has_api_key && provider.auth_mode === 'none',
    request_headers_json: '',
    context_window: '',
    max_output_tokens: '',
    capabilities: [],
    capabilities_source: '',
    reasoning: normalizeReasoning(null),
    _catalog_model_metadata: {},
    _model_metadata_overrides: {},
    _active_catalog_model_id: '',
  };
}

export function applyCatalogModelToDraft(draft, model) {
  const normalized = normalizeCatalogModel(model);
  const metadata = catalogModelMetadataDraft(model, draft);
  return {
    ...draft,
    model: normalized.id,
    name: draft?.name || normalized.name || normalized.id,
    ...metadata,
    _catalog_model_metadata: { [normalized.id]: metadata },
    _model_metadata_overrides: {},
    _active_catalog_model_id: normalized.id,
  };
}

// 名称冲突时「另存为」的建议名,与别名自动去重同一套 (N) 规则。
export function modelNameSuggestion(baseName, existingNames = []) {
  return uniqueModelAlias(String(baseName || '').trim() || 'model', existingNames);
}

// 多选时作为别名前缀的厂商名。自定义 OpenAI 兼容 API 没有真正的厂商
// (目录名是 "Custom OpenAI-compatible API"),拼进别名只会碍眼,返回空串 → 只用模型 ID。
export function modelAliasProviderName(provider) {
  if (!provider || isCustomOpenAiCompatibilityProvider(provider)) return '';
  return providerDisplayName(provider);
}

// 别名当前是否仍是自动值:为空,或与上一次自动生成的值一致。
// 用户手改过(与 _auto_alias 不同且非空)之后就不再跟随选择刷新。
export function isAutoModelAlias(draft) {
  const name = String(draft?.name || '').trim();
  return !name || name === String(draft?._auto_alias || '');
}

// 选择(model / provider)变化后调用,按 modelAlias.js 的规则刷新自动别名。
// 只在别名仍是自动值且非编辑模式时改写;返回同一引用表示无需改动。
export function syncAutoModelAlias(
  draft,
  { providerName = '', existingNames = [], editing = false } = {},
) {
  if (!draft || editing || !isAutoModelAlias(draft)) return draft;
  const next = autoModelAliasForSelection(splitModelIds(draft.model), {
    providerName,
    existingNames,
  });
  if (String(draft.name || '') === next && String(draft._auto_alias || '') === next) {
    return draft;
  }
  return { ...draft, name: next, _auto_alias: next };
}

export function redactModelDraftSecrets(value, draft) {
  let output = String(value || '');
  const secrets = new Set();
  const apiKey = String(draft?.api_key || '');
  if (apiKey) secrets.add(apiKey);
  const parsedHeaders = parseRequestHeadersJson(draft?.request_headers_json, draft?.provider);
  if (parsedHeaders.ok && isObject(parsedHeaders.headers)) {
    Object.values(parsedHeaders.headers).forEach((headerValue) => {
      if (typeof headerValue === 'string' && headerValue) secrets.add(headerValue);
    });
  }
  for (const secret of secrets) output = output.split(secret).join('••••');
  return output;
}

function parsePositiveDraftInteger(value, code) {
  if (value === '' || value === undefined || value === null) return { ok: true, value: null };
  const parsed = Number(value);
  if (!Number.isInteger(parsed) || parsed <= 0 || parsed > 2147483647) {
    return { ok: false, code };
  }
  return { ok: true, value: parsed };
}

export function validateModelProfileDraft(draft, provider, { editing = false } = {}) {
  if (!draft || !provider) return { ok: false, code: 'BAD_REQUEST' };
  const policy = modelFieldPolicy(provider);
  if (draft.provider !== provider.runtime_provider) return { ok: false, code: 'UNKNOWN_PROVIDER' };
  if (splitModelIds(draft.model).length === 0) return { ok: false, code: 'MISSING_MODEL' };
  if (policy.show_base_url && !String(draft.base_url || '').trim()) {
    return { ok: false, code: 'MISSING_BASE_URL' };
  }
  if (policy.api_key_required && !String(draft.api_key || '').trim()
      && !(editing && draft.has_api_key)) {
    return { ok: false, code: 'INVALID_API_KEY' };
  }
  if (draft.clear_api_key && !policy.can_clear_api_key) {
    return { ok: false, code: 'INVALID_API_KEY' };
  }
  if (draft.api_protocol && (!policy.show_api_protocol
      || !MODEL_API_PROTOCOLS.includes(draft.api_protocol))) {
    return { ok: false, code: 'INVALID_API_PROTOCOL' };
  }
  if (draft.endpoint_mode === 'full_url' && !policy.show_endpoint_mode) {
    return { ok: false, code: 'INVALID_ENDPOINT_MODE' };
  }
  const contextWindow = parsePositiveDraftInteger(draft.context_window, 'INVALID_CONTEXT_WINDOW');
  if (!contextWindow.ok) return contextWindow;
  const maxOutput = parsePositiveDraftInteger(draft.max_output_tokens, 'INVALID_MAX_OUTPUT_TOKENS');
  if (!maxOutput.ok) return maxOutput;
  const headers = policy.show_request_headers
    ? parseRequestHeadersJson(draft.request_headers_json, draft.provider)
    : { ok: true, headers: undefined };
  if (!headers.ok) return headers;
  const reasoning = normalizeReasoning(draft.reasoning, 'reasoning', { draft: true });
  const capabilities = normalizeModelCapabilities(draft.capabilities);
  if (draft.capabilities_source
      && capabilities.includes('reasoning') !== reasoning.supported) {
    return { ok: false, code: 'INVALID_REASONING' };
  }
  if (reasoning.mandatory && reasoning.enabled === false) {
    return { ok: false, code: 'INVALID_REASONING' };
  }
  if (reasoning.max_tokens && !reasoning.supports_max_tokens) {
    return { ok: false, code: 'INVALID_REASONING' };
  }
  if (reasoning.effort && !reasoning.supported_efforts.includes(reasoning.effort)) {
    return { ok: false, code: 'INVALID_REASONING_EFFORT' };
  }
  if (!reasoning.supported && (
    reasoning.mandatory
      || reasoning.default_enabled
      || reasoning.supports_max_tokens
      || reasoning.enabled !== null
      || reasoning.effort
      || reasoning.max_tokens
  )) {
    return { ok: false, code: 'INVALID_REASONING' };
  }
  if (reasoning.max_tokens && maxOutput.value && reasoning.max_tokens >= maxOutput.value) {
    return { ok: false, code: 'INVALID_REASONING_BUDGET' };
  }
  return {
    ok: true,
    context_window: contextWindow.value,
    max_output_tokens: maxOutput.value,
    request_headers: headers.headers,
    reasoning,
  };
}

export function serializeModelReasoningMutation(reasoning) {
  const normalized = normalizeReasoning(reasoning, 'reasoning', { draft: true });
  const payload = {
    supported: normalized.supported,
    mandatory: normalized.mandatory,
    default_enabled: normalized.default_enabled,
    supported_efforts: normalized.supported_efforts,
    supports_max_tokens: normalized.supports_max_tokens,
  };
  if (normalized.default_effort) payload.default_effort = normalized.default_effort;
  if (typeof normalized.enabled === 'boolean') payload.enabled = normalized.enabled;
  if (normalized.effort) payload.effort = normalized.effort;
  if (normalized.max_tokens) payload.max_tokens = normalized.max_tokens;
  return payload;
}

export function buildModelMutationPayload(draft, provider, options = {}) {
  const validation = validateModelProfileDraft(draft, provider, options);
  if (!validation.ok) return validation;
  const model = String(draft.model || '').trim();
  const requestedName = String(draft.name || '').trim();
  const payload = {
    name: requestedName || (isCustomOpenAiCompatibilityProvider(provider) ? model : ''),
    provider: provider.runtime_provider,
    model,
  };
  if (!payload.name || payload.name.startsWith('(')) {
    return { ok: false, code: payload.name ? 'RESERVED_NAME' : 'INVALID_NAME' };
  }
  const policy = modelFieldPolicy(provider);
  if (draft.models_dev_provider_id) {
    payload.models_dev_provider_id = String(draft.models_dev_provider_id);
  } else if (options.editing) {
    payload.models_dev_provider_id = null;
  }
  if (policy.show_base_url) payload.base_url = String(draft.base_url || '').trim();
  if (policy.show_api_protocol && draft.api_protocol) payload.api_protocol = draft.api_protocol;
  if (policy.show_endpoint_mode) payload.endpoint_mode = draft.endpoint_mode || 'base_url';
  else if (options.editing && !policy.managed) payload.endpoint_mode = null;
  if (policy.show_api_key && String(draft.api_key || '').trim()) {
    payload.api_key = String(draft.api_key).trim();
  }
  if (draft.clear_api_key && policy.can_clear_api_key) payload.clear_api_key = true;
  if (policy.show_request_headers) {
    if (validation.request_headers !== undefined) {
      payload.request_headers = validation.request_headers;
    } else if (options.editing) {
      payload.request_headers = {};
    }
  }
  if (validation.context_window) payload.context_window = validation.context_window;
  else if (options.editing) payload.context_window = null;
  if (policy.show_max_output && validation.max_output_tokens) {
    payload.max_output_tokens = validation.max_output_tokens;
  } else if (policy.show_max_output && options.editing) {
    payload.max_output_tokens = null;
  }
  const capabilities = normalizeModelCapabilities(draft.capabilities);
  if (draft.capabilities_source) {
    payload.capabilities = capabilities;
    payload.capabilities_source = String(draft.capabilities_source);
  } else if (capabilities.length > 0) {
    payload.capabilities = capabilities;
  }
  if (policy.show_reasoning && validation.reasoning.supported) {
    payload.reasoning = serializeModelReasoningMutation(validation.reasoning);
  } else if (policy.show_reasoning && options.editing) {
    payload.reasoning = null;
  }
  return { ok: true, payload };
}

function draftForSelectedModelMutation(draft, modelId, { editing = false } = {}) {
  if (editing) return { ...draft, model: modelId };
  const metadataById = catalogMetadataMap(draft);
  const catalogMetadata = metadataById[modelId];
  const hasCatalogMetadata = Object.keys(metadataById).length > 0;
  if (!catalogMetadata && !hasCatalogMetadata) return { ...draft, model: modelId };

  const overrides = draftMetadataOverrides(draft);
  const metadata = catalogMetadata || {
    ...emptyModelMetadataDraft(),
    capabilities_source: 'manual',
  };
  const effective = {
    ...draft,
    ...metadata,
    model: modelId,
  };
  for (const field of MODEL_METADATA_FIELDS) {
    if (overrides[field]) effective[field] = draft[field];
  }
  if (overrides.capabilities) effective.capabilities_source = 'manual';
  return effective;
}

// options.existingNames = 已保存条目的名字,供多选派生名与空别名回退时做 (N) 去重;
// 不传则只在本批内去重,撞已有名字交给后端 NAME_TAKEN 冲突流程。
export function buildModelMutationPayloads(draft, provider, options = {}) {
  const selected = splitModelIds(draft?.model);
  if (selected.length === 0) return { ok: false, code: 'MISSING_MODEL' };
  if (options.editing && selected.length !== 1) {
    return { ok: false, code: 'MULTI_MODEL_EDIT' };
  }
  const generated = buildModelDraftsFromSelection(draft, {
    existingNames: options.existingNames || [],
    editing: !!options.editing,
  });
  const payloads = [];
  for (const item of generated) {
    const effective = draftForSelectedModelMutation(item, item.model, options);
    const result = buildModelMutationPayload(effective, provider, options);
    if (!result.ok) return result;
    payloads.push(result.payload);
  }
  return { ok: true, payloads };
}

export function hasAdvancedModelValues(draft) {
  const reasoning = normalizeReasoning(draft?.reasoning, 'reasoning', { draft: true });
  return !!(
    draft?.endpoint_mode === 'full_url'
      || String(draft?.request_headers_json || '').trim()
      || String(draft?.context_window || '').trim()
      || String(draft?.max_output_tokens || '').trim()
      || draft?.capabilities_source
      || reasoning.supported
  );
}

export function formatModelTokenLimit(value) {
  let tokens = null;
  try {
    tokens = optionalPositiveInteger(value);
  } catch {
    return '';
  }
  if (!tokens) return '';
  if (tokens >= 1000000) return `${(tokens / 1000000).toFixed(tokens % 1000000 ? 2 : 0)}M`;
  if (tokens >= 1000) return `${(tokens / 1000).toFixed(tokens % 1000 ? 1 : 0)}K`;
  return String(tokens);
}
