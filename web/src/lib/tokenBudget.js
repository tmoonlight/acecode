function toNonNegativeInt(value) {
  const n = Number(value);
  if (!Number.isFinite(n)) return 0;
  return Math.max(0, Math.trunc(n));
}

function readTokenValue(source, camelKey, snakeKey) {
  if (!source || typeof source !== 'object') return 0;
  return toNonNegativeInt(source[camelKey] ?? source[snakeKey]);
}

function hasApiUsageData(usage) {
  if (!usage || typeof usage !== 'object') return false;
  return usage.hasData === true || usage.has_data === true;
}

export const CONTEXT_USAGE_CATEGORIES = Object.freeze([
  Object.freeze({ key: 'systemPrompt', snakeKey: 'system_prompt', tone: 'system-prompt', label: '系统提示' }),
  Object.freeze({ key: 'projectRules', snakeKey: 'project_rules', tone: 'project-rules', label: '项目与自定义规则' }),
  Object.freeze({ key: 'skills', snakeKey: 'skills', tone: 'skills', label: 'Skills' }),
  Object.freeze({ key: 'builtinTools', snakeKey: 'builtin_tools', tone: 'builtin-tools', label: '内置工具' }),
  Object.freeze({ key: 'mcpTools', snakeKey: 'mcp_tools', tone: 'mcp-tools', label: 'MCP 工具' }),
  Object.freeze({ key: 'conversation', snakeKey: 'conversation', tone: 'conversation', label: '对话' }),
  Object.freeze({ key: 'dynamicContext', snakeKey: 'dynamic_context', tone: 'dynamic-context', label: '动态上下文' }),
]);

// Share of total input tokens served from the provider's prompt cache.
// `promptTokens` is the total input on every provider (the Anthropic
// provider normalizes its split counters server-side), so cache reads are a
// subset of it. Returns null when the provider reported no usage at all;
// a reported 0% is meaningful and must stay visible.
export function computeCacheHitPercent(usage) {
  if (!hasApiUsageData(usage)) return null;
  const promptTokens = readTokenValue(usage, 'promptTokens', 'prompt_tokens');
  if (promptTokens <= 0) return null;
  const cacheRead = readTokenValue(usage, 'cacheReadTokens', 'cache_read_tokens');
  return Math.min(100, Math.max(0, Math.round((cacheRead / promptTokens) * 100)));
}

export function formatCompactTokenCount(value) {
  const tokens = toNonNegativeInt(value);
  if (tokens < 1000) return String(tokens);
  const divisor = tokens >= 1000000 ? 1000000 : 1000;
  const suffix = tokens >= 1000000 ? 'M' : 'K';
  const scaled = tokens / divisor;
  const digits = scaled >= 100 ? 0 : 1;
  return `${scaled.toFixed(digits).replace(/\.0$/, '')}${suffix}`;
}

function normalizeContextBreakdown(usage, usedTokens, limitTokens) {
  const source = usage?.contextBreakdown ?? usage?.context_breakdown;
  const hasData = source?.hasData === true || source?.has_data === true;
  if (!hasData || !source || typeof source !== 'object' || usedTokens <= 0) {
    return { known: false, categories: [] };
  }

  const rawValues = CONTEXT_USAGE_CATEGORIES.map((category) => (
    readTokenValue(source, category.key, category.snakeKey)
  ));
  const rawTotal = rawValues.reduce((sum, value) => sum + value, 0);
  const normalizedValues = new Array(rawValues.length).fill(0);

  if (rawTotal <= 0) {
    const conversationIndex = CONTEXT_USAGE_CATEGORIES.findIndex(({ key }) => key === 'conversation');
    normalizedValues[conversationIndex] = usedTokens;
  } else {
    const remainders = rawValues.map((value, index) => {
      const numerator = value * usedTokens;
      normalizedValues[index] = Math.floor(numerator / rawTotal);
      return { index, value: numerator % rawTotal };
    });
    remainders.sort((a, b) => (b.value - a.value) || (a.index - b.index));
    let residual = usedTokens - normalizedValues.reduce((sum, value) => sum + value, 0);
    for (let i = 0; i < residual; i += 1) {
      normalizedValues[remainders[i % remainders.length].index] += 1;
    }
  }

  return {
    known: true,
    categories: CONTEXT_USAGE_CATEGORIES.map((category, index) => {
      const tokens = normalizedValues[index];
      return {
        ...category,
        tokens,
        compactTokens: formatCompactTokenCount(tokens),
        usedShare: usedTokens > 0 ? tokens / usedTokens : 0,
        windowShare: limitTokens > 0 ? tokens / limitTokens : 0,
      };
    }),
  };
}

function unknownBudget(limitTokens, reason = 'no_usage') {
  const label = reason === 'no_limit'
    ? '上下文窗口未知，暂无法计算 token 余量'
    : '尚未收到 token 用量数据';
  return {
    known: false,
    reason,
    hasData: false,
    usedTokens: 0,
    completionTokens: 0,
    totalTokens: 0,
    limitTokens,
    remainingTokens: null,
    usedRatio: 0,
    rawUsedRatio: 0,
    percent: 0,
    severity: 'unknown',
    cacheReadTokens: 0,
    cacheWriteTokens: 0,
    cacheHitPercent: null,
    cacheLabel: '',
    cacheTitle: '',
    compactUsedTokens: '0',
    compactLimitTokens: formatCompactTokenCount(limitTokens),
    breakdownKnown: false,
    categories: [],
    title: label,
    ariaLabel: label,
  };
}

export function normalizeTokenBudget({ usage = null, contextWindow = 0 } = {}) {
  const limitTokens = toNonNegativeInt(contextWindow);
  if (!hasApiUsageData(usage)) {
    return unknownBudget(limitTokens, 'no_usage');
  }
  if (limitTokens <= 0) {
    return unknownBudget(limitTokens, 'no_limit');
  }

  const usedTokens = readTokenValue(usage, 'promptTokens', 'prompt_tokens');
  const completionTokens = readTokenValue(usage, 'completionTokens', 'completion_tokens');
  const totalTokens = readTokenValue(usage, 'totalTokens', 'total_tokens');
  const rawUsedRatio = usedTokens / limitTokens;
  const usedRatio = Math.min(1, Math.max(0, rawUsedRatio));
  const percent = Math.round(usedRatio * 100);
  const remainingTokens = Math.max(0, limitTokens - usedTokens);
  const severity = rawUsedRatio >= 0.9
    ? 'danger'
    : rawUsedRatio >= 0.7
      ? 'warning'
      : 'safe';
  const cacheReadTokens = readTokenValue(usage, 'cacheReadTokens', 'cache_read_tokens');
  const cacheWriteTokens = readTokenValue(usage, 'cacheWriteTokens', 'cache_write_tokens');
  const cacheHitPercent = computeCacheHitPercent(usage);
  const cacheTitle = cacheHitPercent === null
    ? ''
    : `提示词缓存命中：${cacheReadTokens} / ${usedTokens} 输入 token（${cacheHitPercent}%），缓存写入 ${cacheWriteTokens}`;
  const title = `上下文 token：${usedTokens} / ${limitTokens}（${percent}% 已用），剩余 ${remainingTokens}`;
  const breakdown = normalizeContextBreakdown(usage, usedTokens, limitTokens);

  return {
    known: true,
    reason: '',
    hasData: true,
    usedTokens,
    completionTokens,
    totalTokens,
    limitTokens,
    remainingTokens,
    usedRatio,
    rawUsedRatio,
    percent,
    severity,
    cacheReadTokens,
    cacheWriteTokens,
    cacheHitPercent,
    cacheLabel: cacheHitPercent === null ? '' : `cache ${cacheHitPercent}%`,
    cacheTitle,
    compactUsedTokens: formatCompactTokenCount(usedTokens),
    compactLimitTokens: formatCompactTokenCount(limitTokens),
    breakdownKnown: breakdown.known,
    categories: breakdown.categories,
    title,
    ariaLabel: title,
  };
}
