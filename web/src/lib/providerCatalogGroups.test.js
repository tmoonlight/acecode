import assert from 'node:assert/strict';
import {
  groupCatalogProviders,
  providerDisplayName,
  PROVIDER_GROUP_ORDER,
} from './providerCatalogGroups.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

const providers = [
  { id: 'openrouter', name: 'OpenRouter', group: 'catalog' },
  { id: 'anthropic', name: 'Anthropic', group: 'native' },
  { id: 'acemodel', name: 'ACEModel', group: 'custom' },
  { id: 'custom-openai', name: 'Custom OpenAI-compatible API', group: 'custom' },
  { id: 'xai', name: 'xAI API', group: 'catalog' },
  { id: 'grok', name: 'Grok Coding Plan', group: 'native' },
  { id: 'openai', name: 'OpenAI', group: 'native' },
  { id: 'kimi-for-coding', name: 'Kimi For Coding', group: 'catalog' },
  { id: 'deepseek', name: 'DeepSeek', group: 'catalog' },
  { id: 'zhipuai', name: 'Zhipu AI', group: 'catalog' },
  { id: 'copilot', name: 'GitHub Copilot', group: 'native' },
  { id: 'ollama', name: 'Ollama', group: 'local' },
];

run('自定义模型置顶且自营模型紧随其后', () => {
  const groups = groupCatalogProviders(providers);
  assert.deepEqual(PROVIDER_GROUP_ORDER, [
    'custom',
    'first_party',
    'popular',
    'native',
    'local',
    'catalog',
  ]);
  assert.deepEqual(groups.map((group) => group.group), PROVIDER_GROUP_ORDER);
  assert.deepEqual(groups[0].items.map((provider) => provider.id), ['custom-openai']);
  assert.deepEqual(groups[1].items.map((provider) => provider.id), ['acemodel']);
  assert.deepEqual(groups[2].items.map((provider) => provider.id), [
    'deepseek',
    'zhipuai',
    'kimi-for-coding',
    'openai',
    'anthropic',
    'grok',
  ]);
  assert.deepEqual(groups[3].items.map((provider) => provider.id), ['copilot']);
});

run('热门模型使用用户可识别品牌名并保留原名称搜索', () => {
  assert.equal(providerDisplayName(providers.find((provider) => provider.id === 'zhipuai')), 'GLM');
  assert.equal(providerDisplayName(providers.find((provider) => provider.id === 'kimi-for-coding')), 'Kimi');
  assert.equal(providerDisplayName(providers.find((provider) => provider.id === 'grok')), 'Grok');
  assert.deepEqual(
    groupCatalogProviders(providers, 'glm')[0].items.map((provider) => provider.id),
    ['zhipuai'],
  );
  assert.deepEqual(
    groupCatalogProviders(providers, 'xai')[0].items.map((provider) => provider.id),
    ['xai'],
  );
});
