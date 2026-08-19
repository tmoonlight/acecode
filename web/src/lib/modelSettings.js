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
  parseRequestHeadersJson,
  splitModelIds,
} from './modelManager.js';

export const MODEL_CATALOG_QUERY_LIMIT = 50;
export const MODEL_ENDPOINT_MODES = ['base_url', 'full_url'];
export const MODEL_REASONING_EFFORTS = ['minimal', 'low', 'medium', 'high', 'xhigh', 'max'];

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

function catalogModelMetadataDraft(raw) {
  return modelMetadataDraftFromNormalized(normalizeCatalogModel(raw));
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
  return next;
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
  let selectedMetadata = null;

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
      selectedMetadata = catalogModelMetadataDraft(rawModel);
      metadataById[id] = selectedMetadata;
      activeId = id;
    }
  }

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
  if (!wasSelected && selectedMetadata && !String(draft?.name || '').trim()) {
    next.name = String(rawModel?.name || id).trim();
  }
  return next;
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

export function emptyModelProfileDraft() {
  return {
    name: '',
    provider: 'openai',
    catalog_provider_id: '',
    models_dev_provider_id: '',
    model: '',
    base_url: OPENAI_DEFAULT_BASE_URL,
    endpoint_mode: 'base_url',
    api_key: '',
    has_api_key: false,
    clear_api_key: false,
    credential_source_name: '',
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
    api_key: '',
    has_api_key: false,
    clear_api_key: !!draft?.has_api_key && provider.auth_mode === 'none',
    credential_source_name: '',
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
  const metadata = modelMetadataDraftFromNormalized(normalized);
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

export function modelNameSuggestion(baseName, existingNames = []) {
  const normalizedBase = String(baseName || 'model').trim() || 'model';
  const occupied = new Set(existingNames.map((name) => String(name || '').trim()));
  if (!occupied.has(normalizedBase)) return normalizedBase;
  let suffix = 2;
  while (occupied.has(`${normalizedBase}-${suffix}`)) suffix += 1;
  return `${normalizedBase}-${suffix}`;
}

function normalizeBaseUrlIdentity(value) {
  const raw = String(value || '').trim();
  if (!raw) return '';
  try {
    const parsed = new URL(raw);
    const path = parsed.pathname.replace(/\/+$/, '') || '/';
    return `${parsed.protocol.toLowerCase()}//${parsed.host.toLowerCase()}${path}${parsed.search}${parsed.hash}`;
  } catch {
    return raw.replace(/\/+$/, '');
  }
}

export function compatibleCredentialSources(models, draft) {
  const provider = String(draft?.provider || '');
  const providerId = String(draft?.models_dev_provider_id || '');
  const baseUrl = normalizeBaseUrlIdentity(draft?.base_url);
  return (Array.isArray(models) ? models : []).filter((model) => (
    !!model?.has_api_key
      && model.provider === provider
      && String(model.models_dev_provider_id || '') === providerId
      && normalizeBaseUrlIdentity(model.base_url) === baseUrl
  ));
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
      && !String(draft.credential_source_name || '').trim()
      && !(editing && draft.has_api_key)) {
    return { ok: false, code: 'INVALID_API_KEY' };
  }
  if (draft.clear_api_key && !policy.can_clear_api_key) {
    return { ok: false, code: 'INVALID_API_KEY' };
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
  if (policy.show_endpoint_mode) payload.endpoint_mode = draft.endpoint_mode || 'base_url';
  else if (options.editing && !policy.managed) payload.endpoint_mode = null;
  if (policy.show_api_key && String(draft.api_key || '').trim()) {
    payload.api_key = String(draft.api_key).trim();
  }
  if (policy.show_api_key && !options.editing && !payload.api_key
      && String(draft.credential_source_name || '').trim()) {
    payload.credential_source_name = String(draft.credential_source_name).trim();
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

export function buildModelMutationPayloads(draft, provider, options = {}) {
  const selected = splitModelIds(draft?.model);
  if (selected.length === 0) return { ok: false, code: 'MISSING_MODEL' };
  if (options.editing && selected.length !== 1) {
    return { ok: false, code: 'MULTI_MODEL_EDIT' };
  }
  const generated = buildModelDraftsFromSelection(draft);
  const useModelIdAsName = isCustomOpenAiCompatibilityProvider(provider)
    && !String(draft?.name || '').trim();
  const payloads = [];
  for (const item of generated) {
    const namedItem = useModelIdAsName ? { ...item, name: item.model } : item;
    const effective = draftForSelectedModelMutation(namedItem, item.model, options);
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
