export const RECENT_EXPERT_LIMIT = 5;
export const RECENT_EXPERTS_STORAGE_KEY = 'acecode.recentExperts.v1';
export const DEFAULT_RECENT_EXPERT_IDS = Object.freeze([]);

export function normalizeRecentExpertIds(value, limit = RECENT_EXPERT_LIMIT) {
  if (!Array.isArray(value)) return [];
  const boundedLimit = Math.max(0, Number.isFinite(limit) ? Math.floor(limit) : RECENT_EXPERT_LIMIT);
  if (boundedLimit === 0) return [];
  const seen = new Set();
  const normalized = [];
  for (const item of value) {
    if (typeof item !== 'string') continue;
    const id = item.trim();
    if (!id || seen.has(id)) continue;
    seen.add(id);
    normalized.push(id);
    if (normalized.length >= boundedLimit) break;
  }
  return normalized;
}

export function validateRecentExpertIds(value) {
  if (!Array.isArray(value) || value.length > RECENT_EXPERT_LIMIT) return false;
  const normalized = normalizeRecentExpertIds(value);
  return normalized.length === value.length
    && normalized.every((id, index) => id === value[index]);
}

export function recordRecentExpert(value, expertId) {
  const id = typeof expertId === 'string' ? expertId.trim() : '';
  if (!id) return normalizeRecentExpertIds(value);
  return normalizeRecentExpertIds([id, ...(Array.isArray(value) ? value : [])]);
}

export function resolveRecentExperts(experts, recentIds) {
  const byId = new Map(
    (Array.isArray(experts) ? experts : [])
      .filter((expert) => expert && typeof expert.id === 'string' && expert.id)
      .map((expert) => [expert.id, expert]),
  );
  return normalizeRecentExpertIds(recentIds)
    .map((id) => byId.get(id))
    .filter(Boolean);
}
