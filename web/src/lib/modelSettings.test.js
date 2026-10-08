import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import {
  addManualModelToDraft,
  applyCatalogModelToDraft,
  applyCatalogProviderToDraft,
  buildModelMutationPayload,
  buildModelMutationPayloads,
  emptyModelProfileDraft,
  hasAdvancedModelValues,
  isAutoModelAlias,
  isCustomReasoningDraft,
  markModelMetadataOverrides,
  modelAliasProviderName,
  modelFieldPolicy,
  modelNameSuggestion,
  modelRowsFromProbe,
  updateModelReasoningEfforts,
  modelProfileDraftFromSaved,
  normalizeModelCatalogSummary,
  normalizeProviderModelQuery,
  normalizeSavedModelList,
  providerForSavedModel,
  redactModelDraftSecrets,
  replaceDraftModelsFromProbe,
  serializeModelReasoningMutation,
  syncAutoModelAlias,
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

const aceModelProvider = {
  ...customProvider,
  id: 'acemodel',
  name: 'ACEModel',
  base_url: 'https://acemodel.example/v1',
  api_key_env: 'ACEMODEL_API_KEY',
  models_dev_provider_id: 'acemodel',
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

run('编辑预设按持久化 Provider 身份解析且不受 ACEModel 目录顺序影响', () => {
  const providers = [aceModelProvider, customProvider, openRouterProvider];

  assert.equal(providerForSavedModel(providers, {
    provider: 'openai',
    model: 'aurora',
    models_dev_provider_id: '',
  })?.id, 'custom-openai');
  assert.equal(providerForSavedModel(providers, {
    provider: 'openai',
    model: 'aurora',
    models_dev_provider_id: 'acemodel',
  })?.id, 'acemodel');
  assert.equal(providerForSavedModel(providers, {
    provider: 'openai',
    model: 'some-model',
    models_dev_provider_id: 'openrouter',
  })?.id, 'openrouter');
  assert.equal(providerForSavedModel(providers, {
    provider: 'openai',
    model: 'some-model',
    models_dev_provider_id: 'missing-provider',
  }), null);
});

run('Web 严格 normalizer 消费与 C++ 共享的 canonical catalog fixture', () => {
  const summary = normalizeModelCatalogSummary(sharedCatalogContract.summary);
  assert.equal(summary.catalog.version, 7);
  assert.equal(Object.hasOwn(summary, 'recommended_models'), false);
  assert.deepEqual(summary.providers.find((item) => item.id === 'copilot').endpoint_modes, []);
  const acemodel = summary.providers.find((item) => item.id === 'acemodel');
  assert.equal(acemodel.group, 'custom');
  assert.equal(acemodel.base_url,
    sharedCatalogContract.summary.providers.find((item) => item.id === 'acemodel').base_url);
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
    show_api_protocol: false,
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
    show_api_protocol: false,
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

// 场景:别名留空提交 → 无论自定义兼容 API 还是目录 Provider,保存名都精确等于模型 ID
// (含 `/`,不做 slug 清洗;旧实现目录 Provider 会变成 vendor-model-v1)。
run('空别名精确回退 Model ID 原文', () => {
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
  assert.equal(catalog.payloads[0].name, 'vendor/model-v1');
});

// 场景:编辑目录 Provider 的条目时把别名清空 → 仍报 INVALID_NAME,不会像新增那样
// 悄悄改名成模型 ID(那等于替用户把条目重命名了)。
run('编辑模式空别名报 INVALID_NAME 而不是回退模型 ID', () => {
  const result = buildModelMutationPayloads({
    ...applyCatalogProviderToDraft(emptyModelProfileDraft(), openRouterProvider),
    name: '',
    model: 'vendor/model-v1',
    api_key: 'sk-openrouter',
  }, openRouterProvider, { editing: true, existingNames: ['vendor/model-v1'] });
  assert.equal(result.ok, false);
  assert.equal(result.code, 'INVALID_NAME');
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

// 场景:多选批量新增 → 每条 = <别名前缀>-<模型 ID>,与已保存条目撞名的那条追加 (1);
// 「复用已有凭据」已删除,payload 不再带 credential_source_name,API Key 缺失就按
// Provider 的 auth_mode 校验(openrouter 必填 → INVALID_API_KEY)。
run('批量新增按前缀-模型 ID 命名并对已保存条目去重', () => {
  const draft = {
    ...applyCatalogProviderToDraft(emptyModelProfileDraft(), openRouterProvider),
    name: 'coding',
    model: 'alpha/model, beta/model',
    api_key: 'sk-batch',
  };
  const result = buildModelMutationPayloads(draft, openRouterProvider, {
    existingNames: ['coding-beta/model'],
  });
  assert.equal(result.ok, true);
  assert.deepEqual(result.payloads.map((item) => item.name), [
    'coding-alpha/model',
    'coding-beta/model(1)',
  ]);
  for (const payload of result.payloads) {
    assert.equal(Object.hasOwn(payload, 'credential_source_name'), false);
    assert.equal(payload.api_key, 'sk-batch');
  }

  const withoutKey = buildModelMutationPayloads({ ...draft, api_key: '' }, openRouterProvider);
  assert.equal(withoutKey.ok, false);
  assert.equal(withoutKey.code, 'INVALID_API_KEY');
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

// 场景:保存撞名后弹出的「另存为」建议名,与别名自动去重同一套 (N) 规则
// (旧实现是 luna-2 风格,与别名字段的 luna(1) 并存会让界面上出现两种编号)。
run('名称冲突建议使用 (N) 后缀且取最小可用序号', () => {
  assert.equal(modelNameSuggestion('luna', ['luna', 'luna(1)', 'luna(3)']), 'luna(2)');
  assert.equal(modelNameSuggestion('fresh', ['luna']), 'fresh');
  assert.equal(modelNameSuggestion('', ['model']), 'model(1)');
});

// 场景:目录 Provider 里勾选 / 取消模型,别名跟着自动变。
//   单选 → 模型 ID(撞已有名字追加 (1));多选 → 厂商名作前缀;全部取消 → 空。
// 回归:旧实现在勾第一个模型时把它的目录显示名写进 name,多选时其它条目就被拼成
// <首个模型显示名>-<其它模型 ID>。
run('syncAutoModelAlias 单选填模型 ID、多选退化为厂商名前缀、清空选择回到空', () => {
  const options = { providerName: 'OpenRouter', existingNames: ['vendor/model-a'] };
  let draft = applyCatalogProviderToDraft(emptyModelProfileDraft(), openRouterProvider);
  assert.equal(syncAutoModelAlias(draft, options), draft, '没有选择时无需改动,返回同一引用');

  draft = syncAutoModelAlias(toggleCatalogModelInDraft(
    draft, 'vendor/model-a', { id: 'vendor/model-a', name: 'Model A' }, { allowMultiple: true },
  ), options);
  assert.equal(draft.name, 'vendor/model-a(1)', '单选取模型 ID 而不是目录显示名,并对已有条目去重');
  assert.equal(draft._auto_alias, 'vendor/model-a(1)');

  draft = syncAutoModelAlias(toggleCatalogModelInDraft(
    draft, 'vendor/model-b', { id: 'vendor/model-b', name: 'Model B' }, { allowMultiple: true },
  ), options);
  assert.equal(draft.name, 'OpenRouter', '多选时别名字段变成厂商名前缀');

  draft = syncAutoModelAlias(toggleCatalogModelInDraft(
    draft, 'vendor/model-a', null, { allowMultiple: true },
  ), options);
  assert.equal(draft.name, 'vendor/model-b', '退回单选后重新取剩下那个模型的 ID');

  draft = syncAutoModelAlias(toggleCatalogModelInDraft(
    draft, 'vendor/model-b', null, { allowMultiple: true },
  ), options);
  assert.equal(draft.name, '', '全部取消后别名清空');
  assert.equal(isAutoModelAlias(draft), true);
});

// 场景:用户手改过别名之后再增删模型 → 手填值保持不动;把输入框清空又会恢复自动跟随。
run('syncAutoModelAlias 不覆盖用户手改的别名,清空后恢复自动', () => {
  const options = { providerName: 'OpenRouter', existingNames: [] };
  let draft = syncAutoModelAlias(addManualModelToDraft(
    applyCatalogProviderToDraft(emptyModelProfileDraft(), openRouterProvider),
    'vendor/model-a', { allowMultiple: true },
  ), options);
  assert.equal(draft.name, 'vendor/model-a');

  draft = { ...draft, name: 'my-favorite' };
  assert.equal(isAutoModelAlias(draft), false);
  const afterAdd = syncAutoModelAlias(
    addManualModelToDraft(draft, 'vendor/model-b', { allowMultiple: true }), options,
  );
  assert.equal(afterAdd.name, 'my-favorite', '多选也不能把手填别名改成厂商名前缀');

  const cleared = syncAutoModelAlias(
    addManualModelToDraft({ ...afterAdd, name: '' }, 'vendor/model-c', { allowMultiple: true }),
    options,
  );
  assert.equal(cleared.name, 'OpenRouter', '清空后下一次选择变化重新自动生成');
});

// 场景:编辑已有条目时,改模型不能碰别名(那是用户已经保存过的名字)。
run('syncAutoModelAlias 编辑模式不改别名', () => {
  const draft = {
    ...modelProfileDraftFromSaved({
      name: 'kept-name', provider: 'openai', model: 'old/model',
      models_dev_provider_id: 'openrouter', base_url: 'https://openrouter.ai/api/v1',
    }),
    model: 'new/model',
  };
  const result = syncAutoModelAlias(draft, {
    providerName: 'OpenRouter', existingNames: [], editing: true,
  });
  assert.equal(result, draft);
  assert.equal(result.name, 'kept-name');
});

// 场景:切换 Provider 会清空模型选择,此时自动别名也要跟着清空,不能把上一个
// Provider 模型的 ID 带到新 Provider 的表单里。
run('syncAutoModelAlias 切换 Provider 后自动别名随选择一起清空', () => {
  let draft = syncAutoModelAlias(addManualModelToDraft(
    applyCatalogProviderToDraft(emptyModelProfileDraft(), openRouterProvider),
    'vendor/model-a',
  ), { providerName: 'OpenRouter', existingNames: [] });
  assert.equal(draft.name, 'vendor/model-a');
  draft = syncAutoModelAlias(
    applyCatalogProviderToDraft(draft, anthropicProvider),
    { providerName: 'Anthropic', existingNames: [] },
  );
  assert.equal(draft.model, '');
  assert.equal(draft.name, '');
});

// 场景:多选前缀取厂商展示名;自定义 OpenAI 兼容 API 没有真正的厂商,前缀留空只用模型 ID。
run('modelAliasProviderName 目录 Provider 取展示名,自定义兼容 API 为空', () => {
  assert.equal(modelAliasProviderName(openRouterProvider), 'OpenRouter');
  // 真实的 ACEModel 目录条目是 model_input=catalog(见 model_catalog_handler.cpp),
  // 这里的 aceModelProvider fixture 从 customProvider 继承了 manual,显式改回去。
  assert.equal(modelAliasProviderName({ ...aceModelProvider, model_input: 'catalog' }), 'ACEModel');
  assert.equal(modelAliasProviderName(customProvider), '');
  assert.equal(modelAliasProviderName(null), '');
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

run('ACEModel discovery preserves exact per-model effort declarations and clears stale capability guesses', () => {
  const response = {
    models: ['with-effort', 'no-effort'],
    model_capabilities: { 'with-effort': ['vision'], 'no-effort': ['reasoning', 'tool_use'] },
    model_reasoning: {
      'with-effort': { supported: true, default_enabled: true, supported_efforts: ['low', 'high'], default_effort: 'high' },
      'no-effort': null,
    },
  };
  const rows = modelRowsFromProbe(response, aceModelProvider);
  assert.deepEqual(rows[0].capabilities, ['vision', 'reasoning']);
  assert.deepEqual(rows[1].capabilities, ['tool_use']);
  const draft = { ...applyCatalogProviderToDraft(emptyModelProfileDraft(), aceModelProvider), api_key: 'fake-key' };
  const selected = replaceDraftModelsFromProbe(draft, rows, response.models, { allowMultiple: true });
  const built = buildModelMutationPayloads(selected, aceModelProvider);
  assert.equal(built.ok, true);
  assert.deepEqual(built.payloads[0].reasoning.supported_efforts, ['low', 'high']);
  assert.equal(built.payloads[0].reasoning.default_effort, 'high');
  assert.equal(Object.hasOwn(built.payloads[1], 'reasoning'), false);

  const previous = markModelMetadataOverrides({ ...selected, model: 'with-effort' }, {
    reasoning: rows[0].reasoning, capabilities: rows[0].capabilities,
  });
  const legacyRows = modelRowsFromProbe({ models: ['with-effort'], model_capabilities: { 'with-effort': ['reasoning'] } }, aceModelProvider);
  const removed = replaceDraftModelsFromProbe(previous, legacyRows, ['with-effort']);
  assert.equal(removed.reasoning.supported, false);
  assert.deepEqual(removed.capabilities, ['vision']);
  const updated = buildModelMutationPayload({ ...removed, name: 'saved' }, aceModelProvider, { editing: true });
  assert.equal(updated.ok, true);
  assert.equal(updated.payload.reasoning, null);
});

run('custom OpenAI and Anthropic require manual reasoning opt-in and maintain editable effort lists', () => {
  for (const provider of [customProvider, anthropicProvider]) {
    const draft = addManualModelToDraft({ ...applyCatalogProviderToDraft(emptyModelProfileDraft(), provider), name: 'fake', api_key: 'fake-key' }, 'thinking-max');
    assert.equal(draft.reasoning.supported, false);
    const enabled = toggleModelCapability(draft, 'reasoning');
    assert.equal(enabled.reasoning.enabled, true);
    assert.deepEqual(enabled.reasoning.supported_efforts, ['low', 'medium', 'high']);
    const highest = updateModelReasoningEfforts(enabled, 'max', true);
    const chosen = markModelMetadataOverrides(highest, { reasoning: { ...highest.reasoning, default_effort: 'max', effort: 'max' } });
    const removedChoice = updateModelReasoningEfforts(chosen, 'max', false);
    assert.equal(removedChoice.reasoning.default_effort, '');
    assert.equal(removedChoice.reasoning.effort, '');
    assert.deepEqual(removedChoice.reasoning.supported_efforts, ['low', 'medium', 'high']);
    assert.equal(buildModelMutationPayload(removedChoice, provider).ok, true);
    const disabled = toggleModelCapability(chosen, 'reasoning');
    assert.equal(disabled.reasoning.supported, false);
    assert.equal(disabled.reasoning.effort, '');
    assert.equal(buildModelMutationPayload(disabled, provider, { editing: true }).payload.reasoning, null);

    if (provider.id === 'custom-openai') {
      const rows = modelRowsFromProbe({ models: ['thinking-max'], model_capabilities: { 'thinking-max': ['reasoning'] }, model_reasoning: { 'thinking-max': enabled.reasoning } }, provider);
      assert.equal(rows[0].reasoning, null);
      assert.deepEqual(rows[0].capabilities, []);
    }
  }
});

run('manually configured Anthropic reasoning remains editable on reopen', () => {
  const draft = addManualModelToDraft({ ...applyCatalogProviderToDraft(emptyModelProfileDraft(), anthropicProvider), name: 'fake', api_key: 'fake-key' }, 'fake-manual-model');
  const enabled = toggleModelCapability(draft, 'reasoning');
  const built = buildModelMutationPayload(enabled, anthropicProvider);
  assert.equal(built.ok, true);
  assert.equal(built.payload.models_dev_provider_id, 'anthropic');
  const reopened = modelProfileDraftFromSaved(built.payload);
  assert.equal(reopened.catalog_provider_id, 'anthropic');
  assert.equal(isCustomReasoningDraft(reopened), true);
  assert.deepEqual(reopened.reasoning.supported_efforts, ['low', 'medium', 'high']);
});

run('ACEModel reasoning refresh preserves same-model manual capabilities without copying them to other IDs', () => {
  const reasoning = { supported: true, default_enabled: true, supported_efforts: ['low', 'high'], default_effort: 'high' };
  const base = { ...applyCatalogProviderToDraft(emptyModelProfileDraft(), aceModelProvider), name: 'fake', api_key: 'fake-key', model: 'existing', capabilities: ['vision', 'tool_use', 'reasoning'], capabilities_source: 'manual', reasoning };
  const rows = modelRowsFromProbe({ models: ['existing', 'new-model'], model_reasoning: { existing: null, 'new-model': reasoning } }, aceModelProvider);
  const removed = replaceDraftModelsFromProbe(base, rows, ['existing']);
  assert.deepEqual(removed.capabilities, ['vision', 'tool_use']);
  assert.equal(removed.reasoning.supported, false);
  const freshReasoning = { ...reasoning, supported_efforts: ['medium'], default_effort: 'medium' };
  const refreshRows = modelRowsFromProbe({ models: ['existing', 'new-model'], model_reasoning: { existing: freshReasoning, 'new-model': reasoning } }, aceModelProvider);
  const refreshed = replaceDraftModelsFromProbe(removed, refreshRows, ['existing']);
  assert.deepEqual(refreshed.capabilities, ['vision', 'tool_use', 'reasoning']);
  assert.deepEqual(refreshed.reasoning.supported_efforts, ['medium']);
  const switched = replaceDraftModelsFromProbe(base, refreshRows, ['new-model']);
  assert.deepEqual(switched.capabilities, ['reasoning']);
  const batch = replaceDraftModelsFromProbe(base, rows, ['existing', 'new-model'], { allowMultiple: true });
  const built = buildModelMutationPayloads(batch, aceModelProvider);
  assert.equal(built.ok, true);
  assert.deepEqual(built.payloads[0].capabilities, ['vision', 'tool_use']);
  assert.deepEqual(built.payloads[1].capabilities, ['reasoning']);
  const manualEdit = markModelMetadataOverrides(batch, { capabilities: ['vision'] });
  const rebuilt = buildModelMutationPayloads(manualEdit, aceModelProvider);
  assert.equal(rebuilt.ok, true);
  assert.deepEqual(rebuilt.payloads[0].capabilities, ['vision']);
  assert.deepEqual(rebuilt.payloads[1].capabilities, ['reasoning']);
  const reprobed = replaceDraftModelsFromProbe(manualEdit, refreshRows, ['existing', 'new-model'], { allowMultiple: true });
  const reprobedPayloads = buildModelMutationPayloads(reprobed, aceModelProvider);
  assert.equal(reprobedPayloads.ok, true);
  assert.deepEqual(reprobedPayloads.payloads[0].capabilities, ['vision', 'reasoning']);
  assert.deepEqual(reprobedPayloads.payloads[1].capabilities, ['reasoning']);
});

run('Responses 协议贯通保存、编辑及显式切回 Chat Completions', () => {
  const saved = {
    name: 'responses-model', provider: 'openai', model: 'model-id',
    base_url: 'https://api.example/v1', api_key: 'test-key', api_protocol: 'responses',
  };
  const restored = modelProfileDraftFromSaved(saved);
  assert.equal(restored.api_protocol, 'responses');
  assert.equal(modelFieldPolicy(customProvider).show_api_protocol, true);
  const result = buildModelMutationPayload(restored, customProvider, { editing: true });
  assert.equal(result.ok, true);
  assert.equal(result.payload.api_protocol, 'responses');
  const chat = buildModelMutationPayload({ ...restored, api_protocol: 'chat_completions' }, customProvider, { editing: true });
  assert.equal(chat.payload.api_protocol, 'chat_completions');
  const legacy = modelProfileDraftFromSaved({ ...saved, api_protocol: undefined });
  assert.equal(legacy.api_protocol, '');
  assert.equal(Object.hasOwn(buildModelMutationPayload(legacy, customProvider).payload, 'api_protocol'), false);
  assert.equal(validateModelProfileDraft({ ...restored, api_protocol: 'auto' }, customProvider).code, 'INVALID_API_PROTOCOL');
  assert.throws(() => normalizeSavedModelList([{ ...saved, api_protocol: 'auto' }]), /api_protocol is unsupported/);
});

run('协议不泄露到非 OpenAI Provider，切换 Provider 时重置选择', () => {
  for (const provider of [anthropicProvider, copilotProvider, grokProvider]) {
    const switched = applyCatalogProviderToDraft({ ...emptyModelProfileDraft(), api_protocol: 'responses' }, provider);
    assert.equal(switched.api_protocol, '');
    assert.equal(modelFieldPolicy(provider).show_api_protocol, false);
    const draft = { ...switched, name: 'model', model: 'id', api_key: 'test-key' };
    const result = buildModelMutationPayload(draft, provider);
    assert.equal(result.ok, true);
    assert.equal(Object.hasOwn(result.payload, 'api_protocol'), false);
    assert.equal(validateModelProfileDraft({ ...draft, api_protocol: 'responses' }, provider).code, 'INVALID_API_PROTOCOL');
    assert.throws(() => normalizeSavedModelList([{
      name: 'model', provider: provider.runtime_provider, model: 'id', api_protocol: 'responses',
    }]), /api_protocol is unsupported/);
  }
});

run('同 OpenAI runtime 切换服务商显式恢复 Chat Completions', () => {
  const previous = modelProfileDraftFromSaved({
    name: 'responses-model', provider: 'openai', model: 'model-id',
    base_url: 'https://api.example/v1', api_key: 'test-key', api_protocol: 'responses',
  });
  const next = applyCatalogProviderToDraft(previous, openRouterProvider);
  assert.equal(next.api_protocol, 'chat_completions');
  const result = buildModelMutationPayload({ ...next, model: 'new-model', api_key: 'new-key' }, openRouterProvider, { editing: true });
  assert.equal(result.ok, true);
  assert.equal(result.payload.api_protocol, 'chat_completions');
});
