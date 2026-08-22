import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import {
  addManualModelToDraft,
  applyCatalogModelToDraft,
  applyCatalogProviderToDraft,
  buildModelMutationPayload,
  buildModelMutationPayloads,
  compatibleCredentialSources,
  emptyModelProfileDraft,
  hasAdvancedModelValues,
  markModelMetadataOverrides,
  modelFieldPolicy,
  modelNameSuggestion,
  modelProfileDraftFromSaved,
  normalizeModelCatalogSummary,
  normalizeProviderModelQuery,
  normalizeSavedModelList,
  redactModelDraftSecrets,
  replaceDraftModelsFromProbe,
  serializeModelReasoningMutation,
  toggleCatalogModelInDraft,
  toggleModelCapability,
  validateModelProfileDraft,
} from './modelSettings.js';

const sharedCatalogContract = JSON.parse(readFileSync(
  new URL('../../../tests/fixtures/model_catalog_contract.json', import.meta.url),
  'utf8',
));
const sharedMutationContract = JSON.parse(readFileSync(
  new URL('../../../tests/fixtures/model_mutation_contract.json', import.meta.url),
  'utf8',
));

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

const openRouterProvider = {
  id: 'openrouter',
  name: 'OpenRouter',
  runtime_provider: 'openai',
  base_url: 'https://openrouter.ai/api/v1',
  doc: 'https://openrouter.ai/docs',
  api_key_env: 'OPENROUTER_API_KEY',
  auth_mode: 'required',
  endpoint_editable: false,
  endpoint_modes: ['base_url'],
  model_input: 'catalog',
  models_dev_provider_id: 'openrouter',
  group: 'catalog',
};

const customProvider = {
  id: 'custom-openai',
  name: '自定义 OpenAI 兼容 API',
  runtime_provider: 'openai',
  base_url: 'https://api.openai.com/v1',
  doc: '',
  api_key_env: '',
  auth_mode: 'required',
  endpoint_editable: true,
  endpoint_modes: ['base_url', 'full_url'],
  model_input: 'manual',
  models_dev_provider_id: null,
  group: 'custom',
};

const copilotProvider = {
  id: 'copilot',
  name: 'GitHub Copilot',
  runtime_provider: 'copilot',
  base_url: '',
  doc: 'https://github.com/features/copilot',
  api_key_env: '',
  auth_mode: 'managed',
  endpoint_editable: false,
  endpoint_modes: [],
  model_input: 'catalog',
  models_dev_provider_id: null,
  group: 'native',
};

const grokProvider = {
  id: 'grok',
  name: 'Grok Coding Plan',
  runtime_provider: 'grok',
  base_url: '',
  doc: 'https://docs.x.ai',
  api_key_env: '',
  auth_mode: 'managed',
  endpoint_editable: false,
  endpoint_modes: [],
  model_input: 'catalog',
  models_dev_provider_id: 'xai',
  group: 'native',
};

const anthropicProvider = {
  id: 'anthropic',
  name: 'Anthropic',
  runtime_provider: 'anthropic',
  base_url: 'https://api.anthropic.com/v1',
  doc: 'https://docs.anthropic.com',
  api_key_env: 'ANTHROPIC_API_KEY',
  auth_mode: 'required',
  endpoint_editable: false,
  endpoint_modes: ['base_url'],
  model_input: 'catalog',
  models_dev_provider_id: 'anthropic',
  group: 'native',
};

const localProvider = {
  id: 'ollama',
  name: 'Ollama',
  runtime_provider: 'openai',
  base_url: 'http://127.0.0.1:11434/v1',
  doc: 'https://ollama.com',
  api_key_env: '',
  auth_mode: 'none',
  endpoint_editable: false,
  endpoint_modes: ['base_url'],
  model_input: 'catalog',
  models_dev_provider_id: 'ollama',
  group: 'local',
};

function catalogFixture() {
  return {
    catalog: {
      source: 'models.dev',
      version: 7,
      updated_at: '2026-08-09T12:00:00Z',
      freshness: 'bundled',
    },
    providers: [openRouterProvider, customProvider, copilotProvider, grokProvider, localProvider].map((provider) => ({
      ...provider,
      endpoint_modes: [...provider.endpoint_modes],
    })),
  };
}

run('目录摘要严格保留 Provider 元数据且不再暴露热门预置', () => {
  const normalized = normalizeModelCatalogSummary(catalogFixture());
  assert.equal(normalized.catalog.version, 7);
  assert.equal(normalized.providers[0].doc, 'https://openrouter.ai/docs');
  assert.equal(Object.hasOwn(normalized, 'recommended_models'), false);
});

run('Web 严格 normalizer 消费与 C++ 共享的 canonical catalog fixture', () => {
  const summary = normalizeModelCatalogSummary(sharedCatalogContract.summary);
  assert.equal(summary.catalog.version, 7);
  assert.equal(Object.hasOwn(summary, 'recommended_models'), false);
  assert.deepEqual(summary.providers.find((item) => item.id === 'copilot').endpoint_modes, []);
  const acemodel = summary.providers.find((item) => item.id === 'acemodel');
  assert.equal(acemodel.group, 'custom');
  assert.equal(acemodel.base_url, 'https://ge.bigjuan.xyz/aceapi/v1');
  const custom = summary.providers.find((item) => item.id === 'custom-openai');
  assert.equal(custom.auth_mode, 'required');
  assert.deepEqual(custom.endpoint_modes, ['base_url', 'full_url']);

  const query = normalizeProviderModelQuery(sharedCatalogContract.query, 'openrouter');
  assert.equal(query.models[0].id, 'exact-model');
  assert.deepEqual(query.models[0].pricing, { input: null, output: null });
  assert.deepEqual(query.models[0].input_modalities, []);
  assert.deepEqual(query.models[0].output_modalities, []);
});

run('目录摘要不掩盖缺少 Provider 数组或非法 Copilot 合同', () => {
  assert.throws(() => normalizeModelCatalogSummary({ catalog: {}, providers: [] }), {
    code: 'MODEL_CATALOG_CONTRACT',
  });
  const badCopilot = catalogFixture();
  badCopilot.providers[2] = { ...badCopilot.providers[2], auth_mode: 'required' };
  assert.throws(() => normalizeModelCatalogSummary(badCopilot), /Copilot must use managed auth/);
});

run('目录字段存在但类型或值非法时立即拒绝而不是静默回退', () => {
  const stringVersion = catalogFixture();
  stringVersion.catalog.version = '7';
  assert.throws(() => normalizeModelCatalogSummary(stringVersion), /non-negative integer/);

  const badEndpointModes = catalogFixture();
  badEndpointModes.providers[0].endpoint_modes = ['base_url', 'guess'];
  assert.throws(() => normalizeModelCatalogSummary(badEndpointModes), /endpoint_modes.*unsupported/);

  const badGroup = catalogFixture();
  badGroup.providers[0].group = 'mystery';
  assert.throws(() => normalizeModelCatalogSummary(badGroup), /group is unsupported/);

  assert.throws(() => normalizeSavedModelList([{
    name: 'bad-endpoint',
    provider: 'openai',
    model: 'gpt',
    endpoint_mode: 'automatic',
  }]), /endpoint_mode is unsupported/);
});

run('目录模型推理 effort 接受 max 并拒绝未知值', () => {
  const model = {
    id: 'reasoning-model',
    reasoning: {
      supported: true,
      mandatory: false,
      default_enabled: true,
      supported_efforts: ['high', 'max'],
      default_effort: 'max',
      supports_max_tokens: false,
    },
  };
  const normalized = normalizeProviderModelQuery({
    provider_id: 'openrouter',
    models: [model],
    limit: 1,
  });
  assert.deepEqual(normalized.models[0].reasoning.supported_efforts, ['high', 'max']);
  assert.equal(normalized.models[0].reasoning.default_effort, 'max');

  const unknown = structuredClone(model);
  unknown.reasoning.supported_efforts = ['ultra-secret'];
  assert.throws(() => normalizeProviderModelQuery({
    provider_id: 'openrouter',
    models: [unknown],
    limit: 1,
  }), /supported_efforts.*unsupported/);
});

run('Provider 模型查询校验 provider 身份并保留有限结果', () => {
  const result = normalizeProviderModelQuery({
    provider_id: 'openrouter',
    limit: 25,
    models: [{
      id: 'openai/gpt-5.6-luna',
      name: 'GPT-5.6 Luna',
      context_window: 1050000,
      max_output_tokens: 128000,
      capabilities: ['vision', 'tool_use'],
      reasoning: {
        supported: true,
        mandatory: false,
        default_enabled: true,
        supported_efforts: ['low', 'medium', 'high', 'xhigh', 'max'],
        supports_max_tokens: false,
      },
      deprecated: false,
      input_modalities: ['text', 'image'],
      output_modalities: ['text'],
      knowledge_cutoff: '2025-08',
      pricing: { input: 1.75, output: 14 },
    }],
  }, 'openrouter');
  assert.equal(result.limit, 25);
  assert.equal(result.models[0].id, 'openai/gpt-5.6-luna');
  assert.deepEqual(result.models[0].pricing, { input: 1.75, output: 14 });
  assert.deepEqual(result.models[0].input_modalities, ['text', 'image']);
  assert.deepEqual(result.models[0].output_modalities, ['text']);
  assert.deepEqual(
    result.models[0].reasoning.supported_efforts,
    ['low', 'medium', 'high', 'xhigh', 'max'],
  );
  assert.throws(() => normalizeProviderModelQuery({
    provider_id: 'other', models: [], limit: 25,
  }, 'openrouter'), /does not match/);

  assert.throws(() => normalizeProviderModelQuery({
    provider_id: 'openrouter',
    models: [{ model_id: 'legacy-alias' }],
    limit: 25,
  }, 'openrouter'), /models\[0\]\.id/);

  assert.throws(() => normalizeProviderModelQuery({
    provider_id: 'openrouter',
    models: [{ id: 'bad-price', pricing: { input: '-1', output: null } }],
    limit: 25,
  }, 'openrouter'), /pricing\.input must be a non-negative number/);

  assert.throws(() => normalizeProviderModelQuery({
    provider_id: 'openrouter',
    models: [{ id: 'bad-modalities', input_modalities: null }],
    limit: 25,
  }, 'openrouter'), /input_modalities must be an array/);
});

run('切换到 Copilot 清空秘密与端点并隐藏 API Key/Base URL', () => {
  const previous = {
    ...emptyModelProfileDraft(),
    provider: 'openai',
    base_url: 'https://secret.example/v1',
    api_key: 'never-keep-this',
    request_headers_json: '{"Authorization":"secret"}',
    model: 'old-model',
  };
  const next = applyCatalogProviderToDraft(previous, copilotProvider);
  assert.equal(next.provider, 'copilot');
  assert.equal(next.base_url, '');
  assert.equal(next.api_key, '');
  assert.equal(next.request_headers_json, '');
  assert.equal(next.model, '');
  assert.deepEqual(modelFieldPolicy(copilotProvider), {
    managed: true,
    show_api_key: false,
    api_key_required: false,
    can_clear_api_key: false,
    show_base_url: false,
    edit_base_url: false,
    show_endpoint_mode: false,
    show_request_headers: false,
    show_max_output: false,
    show_reasoning: false,
    can_probe: true,
    model_input: 'catalog',
  });
});

run('Copilot saved profile 通过受管路径生成且 payload 不含端点或密钥', () => {
  const draft = {
    ...applyCatalogProviderToDraft(emptyModelProfileDraft(), copilotProvider),
    name: 'copilot-fast',
    model: 'gpt-5',
    base_url: 'https://must-not-leak.example',
    api_key: 'must-not-leak',
  };
  const result = buildModelMutationPayload(draft, copilotProvider);
  assert.equal(result.ok, true);
  assert.deepEqual(result.payload, {
    name: 'copilot-fast',
    provider: 'copilot',
    model: 'gpt-5',
  });
  const edited = buildModelMutationPayload(draft, copilotProvider, { editing: true });
  assert.equal(edited.ok, true);
  assert.equal(Object.hasOwn(edited.payload, 'max_output_tokens'), false);
});

run('原生 Anthropic 保留推理配置和受支持的请求头', () => {
  const policy = modelFieldPolicy(anthropicProvider);
  assert.equal(policy.show_api_key, true);
  assert.equal(policy.show_reasoning, true);
  assert.equal(policy.show_request_headers, true);
  assert.equal(policy.show_max_output, true);
  assert.equal(policy.can_probe, false);
});

run('原生 Anthropic 编辑可保留并显式清空请求头', () => {
  const draft = modelProfileDraftFromSaved({
    name: 'claude-native',
    provider: 'anthropic',
    model: 'claude-opus',
    base_url: 'https://api.anthropic.com/v1',
    has_api_key: true,
    request_headers: { 'anthropic-beta': 'context-1m-2025-08-07' },
  });
  const preserved = buildModelMutationPayload(draft, anthropicProvider, { editing: true });
  assert.equal(preserved.ok, true);
  assert.deepEqual(preserved.payload.request_headers, {
    'anthropic-beta': 'context-1m-2025-08-07',
  });
  const cleared = buildModelMutationPayload({
    ...draft,
    request_headers_json: '',
  }, anthropicProvider, { editing: true });
  assert.equal(cleared.ok, true);
  assert.deepEqual(cleared.payload.request_headers, {});
});

run('无需认证的本地 Provider 可见且保存 payload 不要求或携带 API Key', () => {
  const normalized = normalizeModelCatalogSummary(catalogFixture());
  const provider = normalized.providers.find((item) => item.id === 'ollama');
  assert.equal(provider.group, 'local');
  assert.equal(modelFieldPolicy(provider).api_key_required, false);
  assert.equal(modelFieldPolicy(provider).show_api_key, false);
  const result = buildModelMutationPayload({
    ...applyCatalogProviderToDraft(emptyModelProfileDraft(), provider),
    name: 'local-qwen',
    model: 'qwen3-coder',
  }, provider);
  assert.equal(result.ok, true);
  assert.equal(Object.hasOwn(result.payload, 'api_key'), false);
  assert.equal(result.payload.base_url, 'http://127.0.0.1:11434/v1');
});

run('目录模型写入能力/推理默认值并保留现有预设名', () => {
  const draft = applyCatalogModelToDraft({
    ...applyCatalogProviderToDraft(emptyModelProfileDraft(), openRouterProvider),
    name: 'my-profile',
  }, {
    id: 'deepseek/deepseek-v4-flash-0731',
    name: 'DeepSeek V4 Flash',
    context_window: 1048576,
    max_output_tokens: 384000,
    capabilities: ['tool_use', 'reasoning'],
    reasoning: {
      supported: true,
      default_enabled: true,
      supported_efforts: ['high'],
      default_effort: 'high',
    },
  });
  assert.equal(draft.name, 'my-profile');
  assert.equal(draft.context_window, '1048576');
  assert.equal(draft.capabilities_source, 'catalog');
  assert.equal(draft.reasoning.supported, true);
});

run('编辑草稿回填响应里的 API Key 并保留 has_api_key 状态', () => {
  const draft = modelProfileDraftFromSaved({
    name: 'safe',
    provider: 'openai',
    model: 'gpt-safe',
    base_url: 'https://api.example/v1',
    api_key: 'sk-original',
    has_api_key: true,
  });
  assert.equal(draft.api_key, 'sk-original');
  assert.equal(draft.has_api_key, true);
  assert.throws(() => normalizeSavedModelList([{
    name: 'invalid', provider: 'openai', model: 'gpt', api_key: 42,
  }]), /api_key must be a string/);
});

run('目录 Token 上限的零值哨兵归一化为未知', () => {
  const result = normalizeProviderModelQuery({
    provider_id: 'grok',
    limit: 1,
    models: [{
      id: 'grok-imagine-image',
      context_window: 0,
      max_output_tokens: 0,
    }],
  }, 'grok');
  assert.equal(result.models[0].context_window, null);
  assert.equal(result.models[0].max_output_tokens, null);

  assert.throws(() => normalizeProviderModelQuery({
    provider_id: 'grok',
    limit: 1,
    models: [{ id: 'bad-limit', max_output_tokens: -1 }],
  }, 'grok'), /max_output_tokens must be a positive integer/);
});

run('Grok Coding Plan 使用受管路径且 payload 不含端点、密钥或运行时覆盖', () => {
  const draft = {
    ...applyCatalogProviderToDraft(emptyModelProfileDraft(), grokProvider),
    name: 'grok-coding',
    model: 'grok-4.5',
    base_url: 'https://must-not-leak.example/v1',
    api_key: 'must-not-leak',
    request_headers_json: '{"Authorization":"must-not-leak"}',
    max_output_tokens: '2048',
  };
  const result = buildModelMutationPayload(draft, grokProvider);
  assert.equal(result.ok, true);
  assert.deepEqual(result.payload, {
    name: 'grok-coding',
    provider: 'grok',
    model: 'grok-4.5',
    models_dev_provider_id: 'xai',
  });
  assert.deepEqual(modelFieldPolicy(grokProvider), {
    managed: true,
    show_api_key: false,
    api_key_required: false,
    can_clear_api_key: false,
    show_base_url: false,
    edit_base_url: false,
    show_endpoint_mode: false,
    show_request_headers: false,
    show_max_output: false,
    show_reasoning: false,
    can_probe: true,
    model_input: 'catalog',
  });
  const restored = modelProfileDraftFromSaved({
    name: 'grok-coding',
    provider: 'grok',
    model: 'grok-4.5',
    models_dev_provider_id: 'xai',
  });
  assert.equal(restored.catalog_provider_id, 'grok');
  assert.equal(restored.provider, 'grok');
});

run('模型错误文案会脱敏当前 API Key 和请求头值', () => {
  const draft = {
    api_key: 'sk-private-value',
    request_headers_json: '{"Authorization":"Bearer private-header","X-Team":"acecode"}',
  };
  assert.equal(
    redactModelDraftSecrets(
      'request failed for sk-private-value with Bearer private-header in acecode',
      draft,
    ),
    'request failed for •••• with •••• in ••••',
  );
  assert.equal(
    redactModelDraftSecrets(
      'request failed with Bearer private-header in acecode',
      {
        ...draft,
        request_headers_json: '\u3000{\n  "Authorization": "Bearer private-header",\n'
          + '  "X-Team": "acecode",\n}\u3000',
      },
    ),
    'request failed with •••• in ••••',
  );
});

run('旧响应缺少密钥时仍可省略 api_key 并保留高级 payload', () => {
  const draft = {
    ...modelProfileDraftFromSaved({
      name: 'safe',
      provider: 'openai',
      model: 'gpt-safe',
      base_url: 'https://api.example/v1',
      has_api_key: true,
      endpoint_mode: 'full_url',
      max_output_tokens: 32768,
      capabilities: ['tool_use'],
      capabilities_source: 'manual',
    }),
    catalog_provider_id: 'custom-openai',
  };
  const result = buildModelMutationPayload(draft, customProvider, { editing: true });
  assert.equal(result.ok, true);
  assert.equal(Object.hasOwn(result.payload, 'api_key'), false);
  assert.equal(result.payload.endpoint_mode, 'full_url');
  assert.equal(result.payload.max_output_tokens, 32768);
  assert.deepEqual(result.payload.capabilities, ['tool_use']);
});

run('编辑时将回填的原 API Key 连同模型字段提交', () => {
  const draft = modelProfileDraftFromSaved({
    name: 'safe',
    provider: 'openai',
    model: 'gpt-safe',
    base_url: 'https://api.example/v1',
    api_key: 'sk-original',
    has_api_key: true,
  });
  const result = buildModelMutationPayload(draft, customProvider, { editing: true });
  assert.equal(result.ok, true);
  assert.equal(result.payload.api_key, 'sk-original');
});

run('reasoning mutation 只发送必填元数据和有真实值的可选覆盖', () => {
  assert.deepEqual(serializeModelReasoningMutation({
    supported: true,
    mandatory: false,
    default_enabled: true,
    supported_efforts: ['low', 'high', 'max'],
    default_effort: 'high',
    supports_max_tokens: true,
    enabled: null,
    effort: '',
    max_tokens: null,
  }), {
    supported: true,
    mandatory: false,
    default_enabled: true,
    supported_efforts: ['low', 'high', 'max'],
    supports_max_tokens: true,
    default_effort: 'high',
  });

  const withOverrides = serializeModelReasoningMutation({
    supported: true,
    mandatory: false,
    default_enabled: true,
    supported_efforts: ['low', 'high'],
    supports_max_tokens: true,
    enabled: false,
    effort: 'low',
    max_tokens: 4096,
  });
  assert.equal(withOverrides.enabled, false);
  assert.equal(withOverrides.effort, 'low');
  assert.equal(withOverrides.max_tokens, 4096);
});

run('Web mutation builder 产出与 C++ 可共享的紧凑编辑 fixture', () => {
  const draft = modelProfileDraftFromSaved({
    name: 'fixture-openrouter',
    provider: 'openai',
    model: 'vendor/reasoning-model',
    models_dev_provider_id: 'openrouter',
    base_url: 'https://openrouter.ai/api/v1',
    has_api_key: true,
    capabilities: ['tool_use', 'reasoning'],
    capabilities_source: 'catalog',
    reasoning: {
      supported: true,
      mandatory: false,
      default_enabled: true,
      supported_efforts: ['low', 'high'],
      default_effort: 'high',
      supports_max_tokens: true,
      enabled: null,
      effort: '',
      max_tokens: null,
    },
  });
  const built = buildModelMutationPayload(draft, openRouterProvider, { editing: true });
  assert.equal(built.ok, true);
  assert.deepEqual(built.payload, sharedMutationContract.edit_payload);
});

run('编辑清空高级值显式发送 null 或空对象，新增空值继续省略', () => {
  const saved = modelProfileDraftFromSaved({
    name: 'clearable',
    provider: 'openai',
    model: 'gpt-clearable',
    base_url: 'https://gateway.example/v1',
    has_api_key: true,
    context_window: 128000,
    max_output_tokens: 32768,
    request_headers: { 'X-Team': 'acecode' },
    reasoning: {
      supported: true,
      mandatory: false,
      default_enabled: true,
      supported_efforts: ['low', 'high'],
      supports_max_tokens: true,
      enabled: true,
      effort: 'high',
      max_tokens: 4096,
    },
  });
  const cleared = {
    ...saved,
    catalog_provider_id: 'custom-openai',
    context_window: '',
    max_output_tokens: '',
    request_headers_json: '',
    reasoning: {
      ...saved.reasoning,
      enabled: null,
      effort: '',
      max_tokens: null,
    },
  };
  const edited = buildModelMutationPayload(cleared, customProvider, { editing: true });
  assert.equal(edited.ok, true);
  assert.equal(edited.payload.context_window, null);
  assert.equal(edited.payload.max_output_tokens, null);
  assert.deepEqual(edited.payload.request_headers, {});
  assert.equal(Object.hasOwn(edited.payload.reasoning, 'enabled'), false);
  assert.equal(Object.hasOwn(edited.payload.reasoning, 'effort'), false);
  assert.equal(Object.hasOwn(edited.payload.reasoning, 'max_tokens'), false);

  const added = buildModelMutationPayload({
    ...cleared,
    name: 'new-empty',
    has_api_key: false,
    api_key: 'sk-new',
    reasoning: { supported: false },
  }, customProvider);
  assert.equal(added.ok, true);
  assert.equal(Object.hasOwn(added.payload, 'context_window'), false);
  assert.equal(Object.hasOwn(added.payload, 'max_output_tokens'), false);
  assert.equal(Object.hasOwn(added.payload, 'request_headers'), false);
  assert.equal(Object.hasOwn(added.payload, 'reasoning'), false);
});

run('同 runtime Provider 切换会显式清除 models.dev 身份和隐藏端点模式', () => {
  const catalogSaved = modelProfileDraftFromSaved({
    name: 'catalog-old',
    provider: 'openai',
    model: 'vendor/old',
    base_url: 'https://openrouter.ai/api/v1',
    models_dev_provider_id: 'openrouter',
    endpoint_mode: 'base_url',
    has_api_key: true,
  });
  const toCustom = {
    ...applyCatalogProviderToDraft(catalogSaved, customProvider),
    name: 'custom-new',
    model: 'vendor/custom',
    base_url: 'https://gateway.example/custom/chat',
    endpoint_mode: 'full_url',
    api_key: 'sk-custom-new',
  };
  const customPayload = buildModelMutationPayload(toCustom, customProvider, { editing: true });
  assert.equal(customPayload.ok, true);
  assert.equal(customPayload.payload.models_dev_provider_id, null);
  assert.equal(customPayload.payload.endpoint_mode, 'full_url');

  const customSaved = modelProfileDraftFromSaved({
    name: 'custom-old',
    provider: 'openai',
    model: 'vendor/custom-old',
    base_url: 'https://gateway.example/custom/chat',
    endpoint_mode: 'full_url',
    has_api_key: true,
  });
  const toCatalog = {
    ...applyCatalogProviderToDraft(customSaved, openRouterProvider),
    name: 'catalog-new',
    model: 'vendor/catalog-new',
    api_key: 'sk-catalog-new',
  };
  const catalogPayload = buildModelMutationPayload(toCatalog, openRouterProvider, { editing: true });
  assert.equal(catalogPayload.ok, true);
  assert.equal(catalogPayload.payload.models_dev_provider_id, 'openrouter');
  assert.equal(catalogPayload.payload.endpoint_mode, null);
});

run('有密钥的 catalog OpenAI 切到 no-auth 本地 Provider 会明确清除旧密钥', () => {
  const saved = modelProfileDraftFromSaved({
    name: 'remote-keyed',
    provider: 'openai',
    model: 'remote/model',
    base_url: 'https://openrouter.ai/api/v1',
    models_dev_provider_id: 'openrouter',
    has_api_key: true,
  });
  const localDraft = {
    ...applyCatalogProviderToDraft(saved, localProvider),
    name: 'local-no-key',
    model: 'qwen3-coder',
  };
  assert.equal(localDraft.clear_api_key, true);
  const result = buildModelMutationPayload(localDraft, localProvider, { editing: true });
  assert.equal(result.ok, true);
  assert.equal(result.payload.clear_api_key, true);
  assert.equal(Object.hasOwn(result.payload, 'api_key'), false);
});

run('凭据复用只接受 Provider、规范 Base URL 与 models.dev 身份均匹配的条目', () => {
  const models = [
    {
      name: 'openrouter-key', provider: 'openai', model: 'a', has_api_key: true,
      base_url: 'https://openrouter.ai/api/v1/', models_dev_provider_id: 'openrouter',
    },
    {
      name: 'wrong-provider', provider: 'anthropic', model: 'b', has_api_key: true,
      base_url: 'https://openrouter.ai/api/v1', models_dev_provider_id: 'openrouter',
    },
    {
      name: 'no-key', provider: 'openai', model: 'c', has_api_key: false,
      base_url: 'https://openrouter.ai/api/v1', models_dev_provider_id: 'openrouter',
    },
  ];
  const draft = {
    provider: 'openai',
    base_url: 'HTTPS://OPENROUTER.AI/api/v1',
    models_dev_provider_id: 'openrouter',
  };
  assert.deepEqual(compatibleCredentialSources(models, draft).map((item) => item.name), [
    'openrouter-key',
  ]);
});

run('凭据复用规范 scheme/host/尾斜杠但保留大小写敏感 URL path', () => {
  const models = [{
    name: 'upper-path',
    provider: 'openai',
    model: 'a',
    has_api_key: true,
    base_url: 'HTTPS://Gateway.Example:443/TeamAPI/',
    models_dev_provider_id: 'private',
  }];
  const baseDraft = {
    provider: 'openai',
    models_dev_provider_id: 'private',
  };
  assert.deepEqual(compatibleCredentialSources(models, {
    ...baseDraft,
    base_url: 'https://gateway.example/TeamAPI',
  }).map((item) => item.name), ['upper-path']);
  assert.deepEqual(compatibleCredentialSources(models, {
    ...baseDraft,
    base_url: 'https://gateway.example/teamapi',
  }), []);
});

run('凭据复用保留大小写敏感的 URL query 与 fragment 身份', () => {
  const models = [{
    name: 'scoped-key',
    provider: 'openai',
    model: 'a',
    has_api_key: true,
    base_url: 'HTTPS://Gateway.Example:443/api/?Tenant=TeamA#RouteOne',
    models_dev_provider_id: 'private',
  }];
  const baseDraft = {
    provider: 'openai',
    models_dev_provider_id: 'private',
  };
  assert.deepEqual(compatibleCredentialSources(models, {
    ...baseDraft,
    base_url: 'https://gateway.example/api?Tenant=TeamA#RouteOne',
  }).map((item) => item.name), ['scoped-key']);
  assert.deepEqual(compatibleCredentialSources(models, {
    ...baseDraft,
    base_url: 'https://gateway.example/api?tenant=teama#routeone',
  }), []);
  assert.deepEqual(compatibleCredentialSources(models, {
    ...baseDraft,
    base_url: 'https://gateway.example/api?Tenant=TeamA#RouteTwo',
  }), []);
});

run('自定义 OpenAI 兼容 API 的空预设名称精确回退 Model ID', () => {
  const draft = {
    ...applyCatalogProviderToDraft(emptyModelProfileDraft(), customProvider),
    name: '   ',
    model: 'vendor/model-v1',
    api_key: 'sk-custom',
  };
  const direct = buildModelMutationPayload(draft, customProvider);
  assert.equal(direct.ok, true);
  assert.equal(direct.payload.name, 'vendor/model-v1');

  const built = buildModelMutationPayloads(draft, customProvider);
  assert.equal(built.ok, true);
  assert.equal(built.payloads.length, 1);
  assert.equal(built.payloads[0].name, 'vendor/model-v1');

  const catalog = buildModelMutationPayloads({
    ...applyCatalogProviderToDraft(emptyModelProfileDraft(), openRouterProvider),
    name: '',
    model: 'vendor/model-v1',
    api_key: 'sk-openrouter',
  }, openRouterProvider);
  assert.equal(catalog.ok, true);
  assert.equal(catalog.payloads[0].name, 'vendor-model-v1');
});

run('探测模型确认事务性替换选择并保留自定义连接草稿', () => {
  const draft = {
    ...applyCatalogProviderToDraft(emptyModelProfileDraft(), customProvider),
    name: '',
    model: 'manual/old-model',
    base_url: 'https://gateway.example/v1',
    api_key: 'sk-probe',
    request_headers_json: '{"X-Team":"acecode"}',
  };
  const models = [
    {
      id: 'vendor/model-b',
      name: 'Model B',
      context_window: 200000,
      max_output_tokens: null,
      capabilities: [],
      reasoning: null,
    },
    {
      id: 'vendor/model-a',
      name: 'Model A',
      context_window: 100000,
      max_output_tokens: null,
      capabilities: [],
      reasoning: null,
    },
  ];

  const unchanged = replaceDraftModelsFromProbe(draft, models, [], { allowMultiple: true });
  assert.equal(unchanged, draft);

  const selected = replaceDraftModelsFromProbe(
    draft,
    models,
    ['vendor/model-a', 'vendor/model-b'],
    { allowMultiple: true },
  );
  assert.equal(selected.model, 'vendor/model-b, vendor/model-a');
  assert.equal(selected.name, '');
  assert.equal(selected.base_url, draft.base_url);
  assert.equal(selected.api_key, draft.api_key);
  assert.equal(selected.request_headers_json, draft.request_headers_json);
  assert.equal(selected.context_window, '200000');
  assert.equal(selected._catalog_model_metadata['vendor/model-a'].context_window, '100000');
  assert.equal(selected._catalog_model_metadata['vendor/model-b'].context_window, '200000');

  const built = buildModelMutationPayloads(selected, customProvider);
  assert.equal(built.ok, true);
  assert.deepEqual(built.payloads.map((payload) => payload.name), [
    'vendor/model-b',
    'vendor/model-a',
  ]);
  assert.deepEqual(built.payloads.map((payload) => payload.context_window), [
    200000,
    100000,
  ]);

  const single = replaceDraftModelsFromProbe(
    { ...draft, name: 'stable-name' },
    models,
    ['vendor/model-a', 'vendor/model-b'],
  );
  assert.equal(single.model, 'vendor/model-b');
  assert.equal(single.name, 'stable-name');
});

run('批量新增生成稳定名称并只发送凭据来源名称', () => {
  const draft = {
    ...applyCatalogProviderToDraft(emptyModelProfileDraft(), openRouterProvider),
    name: 'coding',
    model: 'alpha/model, beta/model',
    credential_source_name: 'openrouter-key',
  };
  const result = buildModelMutationPayloads(draft, openRouterProvider);
  assert.equal(result.ok, true);
  assert.deepEqual(result.payloads.map((item) => item.name), [
    'coding-alpha-model',
    'coding-beta-model',
  ]);
  assert.equal(result.payloads[0].credential_source_name, 'openrouter-key');
  assert.equal(Object.hasOwn(result.payloads[0], 'api_key'), false);
});

run('目录批量选择按模型 ID 生成各自元数据，取消后不残留最后模型元数据', () => {
  const firstModel = {
    id: 'vendor/model-a',
    name: 'Model A',
    context_window: 100000,
    max_output_tokens: 8000,
    capabilities: ['tool_use'],
    reasoning: {
      supported: false,
      mandatory: false,
      default_enabled: false,
      supported_efforts: [],
      supports_max_tokens: false,
    },
  };
  const secondModel = {
    id: 'vendor/model-b',
    name: 'Model B',
    context_window: 200000,
    max_output_tokens: 16000,
    capabilities: ['vision', 'reasoning'],
    reasoning: {
      supported: true,
      mandatory: false,
      default_enabled: true,
      supported_efforts: ['low', 'high'],
      default_effort: 'high',
      supports_max_tokens: false,
    },
  };
  const base = {
    ...applyCatalogProviderToDraft(emptyModelProfileDraft(), openRouterProvider),
    name: 'batch',
    api_key: 'sk-batch',
  };
  const withFirst = toggleCatalogModelInDraft(
    base,
    firstModel.id,
    firstModel,
    { allowMultiple: true },
  );
  const selected = toggleCatalogModelInDraft(
    withFirst,
    secondModel.id,
    secondModel,
    { allowMultiple: true },
  );
  const built = buildModelMutationPayloads(selected, openRouterProvider);
  assert.equal(built.ok, true);
  const firstPayload = built.payloads.find((payload) => payload.model === firstModel.id);
  const secondPayload = built.payloads.find((payload) => payload.model === secondModel.id);
  assert.equal(firstPayload.context_window, 100000);
  assert.equal(firstPayload.max_output_tokens, 8000);
  assert.deepEqual(firstPayload.capabilities, ['tool_use']);
  assert.equal(Object.hasOwn(firstPayload, 'reasoning'), false);
  assert.equal(secondPayload.context_window, 200000);
  assert.equal(secondPayload.max_output_tokens, 16000);
  assert.deepEqual(secondPayload.capabilities, ['vision', 'reasoning']);
  assert.deepEqual(secondPayload.reasoning, {
    supported: true,
    mandatory: false,
    default_enabled: true,
    supported_efforts: ['low', 'high'],
    supports_max_tokens: false,
    default_effort: 'high',
  });

  const mixed = addManualModelToDraft(selected, 'manual/model-c', { allowMultiple: true });
  const mixedBuilt = buildModelMutationPayloads(mixed, openRouterProvider);
  const manualPayload = mixedBuilt.payloads.find((payload) => payload.model === 'manual/model-c');
  assert.equal(Object.hasOwn(manualPayload, 'context_window'), false);
  assert.equal(Object.hasOwn(manualPayload, 'max_output_tokens'), false);
  assert.deepEqual(manualPayload.capabilities, []);
  assert.equal(manualPayload.capabilities_source, 'manual');
  assert.equal(Object.hasOwn(manualPayload, 'reasoning'), false);

  const withoutSecond = toggleCatalogModelInDraft(
    mixed,
    secondModel.id,
    null,
    { allowMultiple: true },
  );
  assert.equal(withoutSecond.context_window, '100000');
  assert.deepEqual(withoutSecond.capabilities, ['tool_use']);
  const manualOnly = toggleCatalogModelInDraft(
    withoutSecond,
    firstModel.id,
    null,
    { allowMultiple: true },
  );
  assert.equal(manualOnly.context_window, '');
  assert.equal(manualOnly.max_output_tokens, '');
  assert.deepEqual(manualOnly.capabilities, []);
  assert.equal(manualOnly.reasoning.supported, false);
  assert.deepEqual(manualOnly._catalog_model_metadata, {});
});

run('批量模式的显式高级覆盖作为公共值应用到目录与手动模型', () => {
  const model = {
    id: 'vendor/catalog-model',
    name: 'Catalog Model',
    context_window: 100000,
    max_output_tokens: 8000,
    capabilities: ['vision'],
    reasoning: { supported: false },
  };
  let draft = {
    ...applyCatalogProviderToDraft(emptyModelProfileDraft(), openRouterProvider),
    name: 'shared',
    api_key: 'sk-shared',
  };
  draft = toggleCatalogModelInDraft(draft, model.id, model, { allowMultiple: true });
  draft = addManualModelToDraft(draft, 'manual/model', { allowMultiple: true });
  draft = markModelMetadataOverrides(draft, {
    context_window: '77777',
    capabilities: ['tool_use'],
  });
  const built = buildModelMutationPayloads(draft, openRouterProvider);
  assert.equal(built.ok, true);
  for (const payload of built.payloads) {
    assert.equal(payload.context_window, 77777);
    assert.deepEqual(payload.capabilities, ['tool_use']);
    assert.equal(payload.capabilities_source, 'manual');
  }
});

run('手动切换 reasoning 能力会同步清空或建立一致的推理元数据', () => {
  const withReasoning = {
    ...applyCatalogProviderToDraft(emptyModelProfileDraft(), openRouterProvider),
    name: 'reasoning-toggle',
    model: 'vendor/reasoning-model',
    has_api_key: true,
    capabilities: ['tool_use', 'reasoning'],
    capabilities_source: 'catalog',
    reasoning: {
      supported: true,
      mandatory: true,
      default_enabled: true,
      enabled: true,
      supported_efforts: ['high'],
      default_effort: 'high',
      effort: 'high',
      supports_max_tokens: true,
      max_tokens: 2048,
    },
  };
  const removed = toggleModelCapability(withReasoning, 'reasoning');
  assert.deepEqual(removed.capabilities, ['tool_use']);
  assert.equal(removed.capabilities_source, 'manual');
  assert.deepEqual(removed.reasoning, {
    supported: false,
    mandatory: false,
    default_enabled: false,
    enabled: null,
    supported_efforts: [],
    default_effort: '',
    effort: '',
    supports_max_tokens: false,
    max_tokens: null,
  });
  assert.equal(removed._model_metadata_overrides.capabilities, true);
  assert.equal(removed._model_metadata_overrides.reasoning, true);
  const removedPayload = buildModelMutationPayload(removed, openRouterProvider, { editing: true });
  assert.equal(removedPayload.ok, true);
  assert.deepEqual(removedPayload.payload.capabilities, ['tool_use']);
  assert.equal(removedPayload.payload.reasoning, null);

  const restored = toggleModelCapability(removed, 'reasoning');
  assert.deepEqual(restored.capabilities, ['tool_use', 'reasoning']);
  assert.deepEqual(restored.reasoning, {
    supported: true,
    mandatory: false,
    default_enabled: false,
    enabled: null,
    supported_efforts: [],
    default_effort: '',
    effort: '',
    supports_max_tokens: false,
    max_tokens: null,
  });
  const restoredPayload = buildModelMutationPayload(restored, openRouterProvider, { editing: true });
  assert.equal(restoredPayload.ok, true);
  assert.deepEqual(restoredPayload.payload.reasoning, {
    supported: true,
    mandatory: false,
    default_enabled: false,
    supported_efforts: [],
    supports_max_tokens: false,
  });
});

run('推理能力与推理 supported 不一致时在发送前拒绝', () => {
  const inconsistent = {
    ...applyCatalogProviderToDraft(emptyModelProfileDraft(), openRouterProvider),
    name: 'inconsistent',
    model: 'vendor/model',
    api_key: 'sk-test',
    capabilities: ['reasoning'],
    capabilities_source: 'manual',
    reasoning: { supported: false },
  };
  assert.equal(buildModelMutationPayload(inconsistent, openRouterProvider).code, 'INVALID_REASONING');
});

run('名称冲突建议使用稳定的最小可用后缀', () => {
  assert.equal(modelNameSuggestion('luna', ['luna', 'luna-2', 'luna-4']), 'luna-3');
  assert.equal(modelNameSuggestion('fresh', ['luna']), 'fresh');
});

run('高级校验拒绝完整端点越权、强制推理关闭和超出输出的推理预算', () => {
  const base = {
    ...applyCatalogProviderToDraft(emptyModelProfileDraft(), openRouterProvider),
    name: 'reasoning',
    model: 'reasoning/model',
    api_key: 'sk-new',
  };
  assert.equal(validateModelProfileDraft({ ...base, endpoint_mode: 'full_url' }, openRouterProvider).code,
    'INVALID_ENDPOINT_MODE');
  assert.equal(validateModelProfileDraft({
    ...base,
    reasoning: {
      supported: true,
      mandatory: true,
      default_enabled: true,
      enabled: false,
      supported_efforts: ['high'],
    },
  }, openRouterProvider).code, 'INVALID_REASONING');
  assert.equal(validateModelProfileDraft({
    ...base,
    max_output_tokens: '1000',
    reasoning: {
      supported: true,
      enabled: true,
      supported_efforts: ['high'],
      effort: 'high',
      supports_max_tokens: true,
      max_tokens: 1000,
    },
  }, openRouterProvider).code, 'INVALID_REASONING_BUDGET');
});

run('自定义 Provider 保留手动模型、完整端点和请求头', () => {
  const draft = {
    ...applyCatalogProviderToDraft(emptyModelProfileDraft(), customProvider),
    name: 'custom',
    model: 'org/manual-model',
    base_url: 'https://gateway.example/custom/chat',
    endpoint_mode: 'full_url',
    api_key: 'sk-custom',
    request_headers_json: '{"X-Team":"acecode"}',
    capabilities: ['tool_use'],
    capabilities_source: 'manual',
  };
  const result = buildModelMutationPayload(draft, customProvider);
  assert.equal(result.ok, true);
  assert.equal(result.payload.endpoint_mode, 'full_url');
  assert.deepEqual(result.payload.request_headers, { 'X-Team': 'acecode' });
  assert.equal(result.payload.model, 'org/manual-model');

  const missingKey = buildModelMutationPayload({ ...draft, api_key: '' }, customProvider);
  assert.equal(missingKey.ok, false);
  assert.equal(missingKey.code, 'INVALID_API_KEY');
});

run('有高级值的编辑草稿会自动展开高级设置', () => {
  assert.equal(hasAdvancedModelValues({ request_headers_json: '{"X":"1"}' }), true);
  assert.equal(hasAdvancedModelValues({ endpoint_mode: 'base_url' }), false);
});
