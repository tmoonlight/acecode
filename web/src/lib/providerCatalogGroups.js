const POPULAR_PROVIDER_SPECS = Object.freeze([
  Object.freeze({ id: 'deepseek', label: 'DeepSeek' }),
  Object.freeze({ id: 'zhipuai', label: 'GLM' }),
  Object.freeze({ id: 'kimi-for-coding', label: 'Kimi' }),
  Object.freeze({ id: 'openai', label: 'OpenAI' }),
  Object.freeze({ id: 'anthropic', label: 'Anthropic' }),
  Object.freeze({ id: 'grok', label: 'Grok' }),
]);

const POPULAR_PROVIDER_BY_ID = new Map(
  POPULAR_PROVIDER_SPECS.map((item, index) => [item.id, { ...item, index }]),
);

export const PROVIDER_GROUP_LABELS = Object.freeze({
  custom: '自定义模型',
  popular: '热门模型',
  native: '原生与受管',
  local: '本地服务',
  catalog: '模型目录',
});

export const PROVIDER_GROUP_ORDER = Object.freeze([
  'custom',
  'popular',
  'native',
  'local',
  'catalog',
]);

export function providerDisplayName(provider) {
  return POPULAR_PROVIDER_BY_ID.get(provider?.id)?.label || provider?.name || provider?.id || '';
}

function providerSearchText(provider) {
  return [
    providerDisplayName(provider),
    provider?.id,
    provider?.name,
    provider?.runtime_provider,
    provider?.models_dev_provider_id,
  ].filter(Boolean).join(' ').toLowerCase();
}

export function groupCatalogProviders(providers, query = '') {
  const needle = query.trim().toLowerCase();
  const groups = new Map();

  providers
    .filter((provider) => !needle || providerSearchText(provider).includes(needle))
    .forEach((provider) => {
      const popular = POPULAR_PROVIDER_BY_ID.get(provider.id);
      const group = popular ? 'popular' : (provider.group || 'catalog');
      if (!groups.has(group)) groups.set(group, []);
      groups.get(group).push(provider);
    });

  const popularItems = groups.get('popular');
  if (popularItems) {
    popularItems.sort((left, right) => (
      POPULAR_PROVIDER_BY_ID.get(left.id).index - POPULAR_PROVIDER_BY_ID.get(right.id).index
    ));
  }

  return PROVIDER_GROUP_ORDER
    .filter((group) => groups.has(group))
    .map((group) => ({ group, items: groups.get(group) }));
}
