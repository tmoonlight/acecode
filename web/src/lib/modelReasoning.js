// Shared capability validation for discovery, saved profiles and session state.
export const MODEL_REASONING_EFFORTS = ['minimal', 'low', 'medium', 'high', 'xhigh', 'max'];
export const MODEL_REASONING_LABELS = {
  minimal: '最低', low: '低', medium: '中', high: '高', xhigh: '极高', max: '最大',
};

export function normalizeModelReasoning(value) {
  if (!value || typeof value !== 'object' || Array.isArray(value) || value.supported !== true) return null;
  const efforts = value.supported_efforts;
  if (!Array.isArray(efforts) || efforts.some((effort) => !MODEL_REASONING_EFFORTS.includes(effort))
      || new Set(efforts).size !== efforts.length) return null;
  for (const field of ['mandatory', 'default_enabled', 'supports_max_tokens']) {
    if (value[field] !== undefined && typeof value[field] !== 'boolean') return null;
  }
  if (value.enabled != null && typeof value.enabled !== 'boolean') return null;
  const defaultEffort = value.default_effort ?? '';
  const effort = value.effort ?? '';
  if ((defaultEffort && !efforts.includes(defaultEffort)) || (effort && !efforts.includes(effort))) return null;
  if (typeof defaultEffort !== 'string' || typeof effort !== 'string') return null;
  if (value.mandatory && (value.enabled === false || value.default_enabled === false)) return null;
  if (value.max_tokens != null && (!value.supports_max_tokens
      || !Number.isInteger(value.max_tokens) || value.max_tokens <= 0)) return null;
  return {
    supported: true,
    mandatory: !!value.mandatory,
    default_enabled: !!value.mandatory || !!value.default_enabled,
    enabled: value.enabled ?? null,
    supported_efforts: [...efforts],
    default_effort: defaultEffort,
    effort,
    supports_max_tokens: !!value.supports_max_tokens,
    max_tokens: value.max_tokens ?? null,
  };
}

export function composerReasoningOptions(model, override = undefined) {
  if (!model || model.deleted || !['openai', 'anthropic'].includes(model.provider)) return null;
  const reasoning = normalizeModelReasoning(model.reasoning);
  if (!reasoning || !reasoning.supported_efforts.length
      || !(reasoning.mandatory || (reasoning.enabled ?? reasoning.default_enabled))) return null;
  const requested = override === undefined ? (model.reasoning_effort ?? model.reasoningEffort) : override;
  const selectedEffort = reasoning.supported_efforts.includes(requested) ? requested : null;
  const effectiveEffort = selectedEffort || reasoning.effort || reasoning.default_effort;
  return {
    label: MODEL_REASONING_LABELS[effectiveEffort] || '默认',
    selectedEffort,
    items: [
      { effort: null, label: '默认' },
      ...reasoning.supported_efforts.map((effort) => ({ effort, label: MODEL_REASONING_LABELS[effort] })),
    ],
  };
}

// Step through supported numeric depths, starting at the effective default.
// null means no available move; the default menu entry is not a depth.
export function nextReasoningEffort(model, direction, override = undefined) {
  const options = composerReasoningOptions(model, override);
  if (!options || ![-1, 1].includes(direction)) return null;
  const levels = MODEL_REASONING_EFFORTS.filter((effort) => options.items.some((item) => item.effort === effort));
  const reasoning = normalizeModelReasoning(model.reasoning);
  const current = options.selectedEffort || reasoning.effort || reasoning.default_effort;
  const index = levels.indexOf(current);
  if (index < 0) return direction > 0 ? levels[0] ?? null : levels[levels.length - 1] ?? null;
  return levels[index + direction] ?? null;
}
