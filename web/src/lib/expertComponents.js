export const EXPERT_PRIMARY_TABS = Object.freeze([
  Object.freeze({ id: 'agent', label: '专家' }),
  Object.freeze({ id: 'team', label: '专家团' }),
]);

// Kept for callers that still import the old name while the catalog now uses
// type tabs plus a separate, non-exclusive Tag rail.
export const EXPERT_FILTERS = EXPERT_PRIMARY_TABS;
export const EXPERT_SORTS = Object.freeze([
  Object.freeze({ id: 'recent', label: '最近使用' }),
  Object.freeze({ id: 'created', label: '最近创建' }),
]);
export const CAPABILITY_KINDS = Object.freeze(['skills', 'mcp_servers', 'tools']);

export function normalizeStringList(value) {
  if (!Array.isArray(value)) return [];
  const seen = new Set();
  const result = [];
  for (const item of value) {
    if (typeof item !== 'string') continue;
    const normalized = item.trim();
    if (!normalized || seen.has(normalized)) continue;
    seen.add(normalized);
    result.push(normalized);
  }
  return result;
}

export function parseLineList(value) {
  return normalizeStringList(String(value || '').split(/\r?\n/));
}

function optionalScope(capabilities, key) {
  if (!capabilities || typeof capabilities !== 'object'
      || !Object.prototype.hasOwnProperty.call(capabilities, key)) {
    return undefined;
  }
  return normalizeStringList(capabilities[key]);
}

export function normalizeExpertCapabilities(value) {
  const normalized = {};
  for (const key of CAPABILITY_KINDS) {
    const scope = optionalScope(value, key);
    if (scope !== undefined) normalized[key] = scope;
  }
  return normalized;
}

export function safeExpertAvatarUrl(value) {
  const url = String(value || '').trim();
  if (!url) return '';
  if (/^(?:https?:|blob:|data:image\/)/i.test(url)) return url;
  if (url.startsWith('/') && !url.startsWith('//')) return url;
  return '';
}

function normalizeAgent(value) {
  if (!value || typeof value !== 'object') return null;
  return {
    ...value,
    id: String(value.id || ''),
    display_name: String(value.display_name || ''),
    profession: String(value.profession || ''),
    instructions: String(value.instructions || ''),
    avatar_url: safeExpertAvatarUrl(value.avatar_url),
  };
}

export function normalizeExperts(value) {
  const list = Array.isArray(value) ? value : (Array.isArray(value?.experts) ? value.experts : []);
  return list
    .filter((item) => item && typeof item === 'object' && typeof item.id === 'string')
    .map((item) => ({
      ...item,
      id: String(item.id || ''),
      type: item.type === 'team' ? 'team' : 'agent',
      display_name: String(item.display_name || item.id || ''),
      author: String(item.author || item.call_name || ''),
      profession: String(item.profession || ''),
      description: String(item.description || ''),
      source: item.source === 'workspace' ? 'workspace' : 'global',
      managed_global: item.managed_global === true,
      avatar_url: safeExpertAvatarUrl(item.avatar_url),
      tags: normalizeStringList(item.tags),
      expertise: normalizeStringList(item.expertise),
      quick_prompts: normalizeStringList(item.quick_prompts),
      created_at: String(item.created_at || ''),
      updated_at: String(item.updated_at || ''),
      capabilities: normalizeExpertCapabilities(item.capabilities),
      agents: Array.isArray(item.agents) ? item.agents.map(normalizeAgent).filter(Boolean) : [],
      references_existing_experts: item.references_existing_experts === true,
      lead_expert_id: String(item.lead_expert_id || item.lead_agent_id || ''),
      member_expert_ids: normalizeStringList(item.member_expert_ids),
    }));
}

export function collectExpertTags(experts, type = 'agent') {
  const tags = new Set();
  for (const expert of normalizeExperts(experts)) {
    if (type !== 'all' && expert.type !== type) continue;
    expert.tags.forEach((tag) => tags.add(tag));
  }
  return [...tags];
}

export function filterExperts(experts, {
  query = '',
  type = 'agent',
  tag = 'all',
} = {}) {
  const needle = String(query || '').trim().toLocaleLowerCase();
  return normalizeExperts(experts).filter((expert) => {
    if (type !== 'all' && expert.type !== type) return false;
    if (tag && tag !== 'all' && !expert.tags.includes(tag)) return false;
    if (!needle) return true;
    return [
      expert.display_name,
      expert.author,
      expert.profession,
      expert.description,
      ...expert.tags,
      ...expert.expertise,
      ...expert.quick_prompts,
    ].some((entry) => String(entry || '').toLocaleLowerCase().includes(needle));
  });
}

function parsedTime(value) {
  const timestamp = Date.parse(String(value || ''));
  return Number.isFinite(timestamp) ? timestamp : 0;
}

export function sortExperts(experts, {
  sort = 'recent',
  recentIds = [],
} = {}) {
  const list = [...normalizeExperts(experts)];
  const recentOrder = new Map(
    normalizeStringList(recentIds).map((id, index) => [id, index]),
  );
  return list.sort((left, right) => {
    if (sort === 'recent') {
      const leftRecent = recentOrder.has(left.id) ? recentOrder.get(left.id) : Number.MAX_SAFE_INTEGER;
      const rightRecent = recentOrder.has(right.id) ? recentOrder.get(right.id) : Number.MAX_SAFE_INTEGER;
      if (leftRecent !== rightRecent) return leftRecent - rightRecent;
      const updatedDelta = parsedTime(right.updated_at) - parsedTime(left.updated_at);
      if (updatedDelta) return updatedDelta;
    } else {
      const createdDelta = parsedTime(right.created_at) - parsedTime(left.created_at);
      if (createdDelta) return createdDelta;
    }
    return left.display_name.localeCompare(right.display_name, 'zh-CN');
  });
}

export function expertiseSummary(expert, fallback = '') {
  const values = normalizeStringList(expert?.expertise);
  if (values.length > 0) return values.slice(0, 3).join(' / ');
  return String(fallback || expert?.profession || '').trim();
}

export function createExpertInternalId(type = 'agent') {
  const prefix = type === 'team' ? 'team' : 'expert';
  const randomPart = globalThis.crypto?.randomUUID
    ? globalThis.crypto.randomUUID().replaceAll('-', '').slice(0, 12)
    : Math.random().toString(36).slice(2, 14);
  return `${prefix}-${Date.now().toString(36)}-${randomPart}`.slice(0, 64);
}

export function emptyExpertForm(type = 'agent', initialExpertId = '') {
  const normalizedType = type === 'team' ? 'team' : 'agent';
  const selectedExpertIds = initialExpertId ? [String(initialExpertId)] : [];
  return {
    id: createExpertInternalId(normalizedType),
    displayName: '',
    author: '',
    profession: '',
    description: '',
    type: normalizedType,
    tags: [],
    expertiseText: '',
    instructions: '',
    quickPromptsText: '',
    capabilities: {},
    selectedExpertIds,
    leadExpertId: selectedExpertIds[0] || '',
  };
}

export function expertFormFromDetail(expert) {
  const normalized = normalizeExperts([expert])[0] || {};
  const agents = Array.isArray(expert?.agents) ? expert.agents : [];
  const lead = agents.find((agent) => agent?.id === expert?.lead_agent_id)
    || agents.find((agent) => agent?.id === normalized.lead_expert_id)
    || agents[0]
    || {};
  const selectedExpertIds = normalized.type === 'team'
    ? normalizeStringList([normalized.lead_expert_id, ...normalized.member_expert_ids])
    : [];
  return {
    id: normalized.id || '',
    displayName: normalized.display_name || '',
    author: normalized.author || '',
    profession: normalized.profession || '',
    description: normalized.description || '',
    type: normalized.type || 'agent',
    tags: normalized.tags || [],
    expertiseText: (normalized.expertise || []).join('\n'),
    instructions: normalized.type === 'agent' ? String(lead.instructions || '') : '',
    quickPromptsText: (normalized.quick_prompts || []).join('\n'),
    capabilities: normalizeExpertCapabilities(normalized.capabilities),
    selectedExpertIds,
    leadExpertId: normalized.lead_expert_id || selectedExpertIds[0] || '',
  };
}

export function validateExpertFormFields(form, selectableExperts) {
  const errors = {};
  if (!/^[a-z0-9](?:[a-z0-9-]{0,62}[a-z0-9])?$/.test(String(form?.id || ''))) {
    errors.id = '无法保存，请重新打开后再试';
  }
  if (!String(form?.displayName || '').trim()) errors.displayName = '请填写专家名称';
  if (!String(form?.author || '').trim()) errors.author = '请填写姓名或称呼';
  if (form?.type === 'team') {
    const selected = normalizeStringList(form.selectedExpertIds);
    if (selected.length < 2) errors.members = '专家团至少需要两位专家';
    if (!errors.members && Array.isArray(selectableExperts)) {
      const unavailable = selectedTeamMemberRows(form, selectableExperts)
        .filter((member) => member.unavailable);
      if (unavailable.length > 0) {
        errors.members = `请移除不可用成员：${unavailable.map((member) => member.display_name).join('、')}`;
      }
    }
    if (!form.leadExpertId || !selected.includes(form.leadExpertId)) {
      errors.leadExpertId = '请选择一位主理人';
    } else if (Array.isArray(selectableExperts)) {
      const lead = selectedTeamMemberRows(form, selectableExperts)
        .find((member) => member.id === form.leadExpertId);
      if (!lead || lead.unavailable) errors.leadExpertId = '主理人必须是当前可用的单专家';
    }
  } else if (!String(form?.instructions || '').trim()) {
    errors.instructions = '请填写这个专家的工作方式';
  }
  return errors;
}

export function validateExpertForm(form) {
  return Object.values(validateExpertFormFields(form))[0] || '';
}

function capabilitiesPayload(form) {
  const value = normalizeExpertCapabilities(form?.capabilities);
  return Object.keys(value).length > 0 ? value : undefined;
}

export function expertPayloadFromForm(form) {
  const common = {
    id: String(form.id || '').trim(),
    type: form.type === 'team' ? 'team' : 'agent',
    display_name: String(form.displayName || '').trim(),
    author: String(form.author || '').trim(),
    profession: String(form.profession || '').trim(),
    description: String(form.description || '').trim(),
    tags: normalizeStringList(form.tags),
    expertise: parseLineList(form.expertiseText),
    quick_prompts: parseLineList(form.quickPromptsText),
  };
  if (common.type === 'team') {
    const selected = normalizeStringList(form.selectedExpertIds);
    return {
      ...common,
      lead_expert_id: String(form.leadExpertId || ''),
      member_expert_ids: selected.filter((id) => id !== form.leadExpertId),
    };
  }
  const capabilities = capabilitiesPayload(form);
  return {
    ...common,
    ...(capabilities ? { capabilities } : {}),
    lead: {
      id: 'lead',
      display_name: common.display_name,
      profession: common.profession,
      instructions: String(form.instructions || '').trim(),
    },
  };
}

export function singleExpertsForTeam(experts, editingTeamId = '') {
  return normalizeExperts(experts).filter(
    (expert) => expert.type === 'agent' && expert.id !== editingTeamId,
  );
}

export function selectedTeamMemberRows(form, experts) {
  const normalized = normalizeExperts(experts);
  const byId = new Map(normalized.map((expert) => [expert.id, expert]));
  const validIds = new Set(singleExpertsForTeam(normalized, form?.id).map((expert) => expert.id));
  return normalizeStringList(form?.selectedExpertIds).map((id) => {
    const expert = byId.get(id);
    if (!expert) {
      return {
        id,
        type: 'agent',
        display_name: `${id}（不可用）`,
        profession: '',
        expertise: [],
        avatar_url: '',
        unavailable: true,
        unavailable_reason: 'missing',
      };
    }
    if (!validIds.has(id)) {
      return {
        ...expert,
        unavailable: true,
        unavailable_reason: expert.type === 'team' ? 'nested_team' : 'out_of_scope',
      };
    }
    return {
      ...expert,
      unavailable: false,
      unavailable_reason: '',
    };
  });
}

export function selectedTeamExperts(form, experts) {
  const byId = new Map(singleExpertsForTeam(experts).map((expert) => [expert.id, expert]));
  return normalizeStringList(form?.selectedExpertIds).map((id) => byId.get(id)).filter(Boolean);
}

export function toggleTeamExpert(form, expertId) {
  const id = String(expertId || '');
  if (!id) return form;
  const selected = normalizeStringList(form?.selectedExpertIds);
  if (selected.includes(id)) {
    const next = selected.filter((item) => item !== id);
    return {
      ...form,
      selectedExpertIds: next,
      leadExpertId: form.leadExpertId === id ? (next[0] || '') : form.leadExpertId,
    };
  }
  return {
    ...form,
    selectedExpertIds: [...selected, id],
    leadExpertId: form.leadExpertId || id,
  };
}

function normalizeCapabilityOption(item, kind) {
  if (!item || typeof item !== 'object' || !String(item.id || '').trim()) return null;
  const id = String(item.id).trim();
  const status = String(item.status || (item.available === false ? 'unavailable' : 'available'));
  const available = item.available !== false;
  const configurable = item.configurable !== false;
  const globallyEnabled = Object.prototype.hasOwnProperty.call(item, 'globally_enabled')
    ? item.globally_enabled === true
    : available;
  const defaultEnabled = Object.prototype.hasOwnProperty.call(item, 'default_enabled')
    ? item.default_enabled === true
    : globallyEnabled;
  const expertSelectable = Object.prototype.hasOwnProperty.call(item, 'expert_selectable')
    ? item.expert_selectable === true
    : (available && configurable);
  return {
    ...item,
    id,
    kind,
    label: String(item.label || item.name || id),
    description: String(item.description || ''),
    source: String(item.source || ''),
    transport: String(item.transport || ''),
    status,
    available,
    globally_enabled: globallyEnabled,
    default_enabled: defaultEnabled,
    expert_selectable: expertSelectable,
    configurable,
    disabled_reason: String(item.disabled_reason || item.unavailable_reason || ''),
  };
}

export function normalizeCapabilityCatalog(value) {
  const source = value && typeof value === 'object' ? value : {};
  return Object.fromEntries(CAPABILITY_KINDS.map((kind) => [
    kind,
    (Array.isArray(source[kind]) ? source[kind] : [])
      .map((item) => normalizeCapabilityOption(item, kind))
      .filter(Boolean),
  ]));
}

export function capabilityOptionsWithSaved(options, selectedIds, kind) {
  const normalized = (Array.isArray(options) ? options : [])
    .map((item) => normalizeCapabilityOption(item, kind))
    .filter(Boolean);
  const known = new Set(normalized.map((item) => item.id));
  for (const id of normalizeStringList(selectedIds)) {
    if (known.has(id)) continue;
    normalized.push({
      id,
      kind,
      label: id,
      description: '',
      source: '',
      transport: '',
      status: 'missing',
      available: false,
      globally_enabled: false,
      default_enabled: false,
      expert_selectable: false,
      configurable: false,
      disabled_reason: '已保存，但当前运行环境不可用',
    });
  }
  return normalized;
}

export function setCapabilityScopeMode(capabilities, kind, mode, defaultIds = []) {
  const next = normalizeExpertCapabilities(capabilities);
  if (!CAPABILITY_KINDS.includes(kind)) return next;
  if (mode === 'inherit') delete next[kind];
  else if (!Object.prototype.hasOwnProperty.call(next, kind)) {
    next[kind] = normalizeStringList(defaultIds);
  }
  return next;
}

export function toggleCapabilitySelection(capabilities, kind, id) {
  const next = setCapabilityScopeMode(capabilities, kind, 'custom');
  const selected = normalizeStringList(next[kind]);
  next[kind] = selected.includes(id)
    ? selected.filter((item) => item !== id)
    : [...selected, id];
  return next;
}

export function normalizeTargetSessions(value) {
  const sessions = Array.isArray(value) ? value : (Array.isArray(value?.sessions) ? value.sessions : []);
  return sessions
    .filter((session) => session && typeof session === 'object' && String(session.id || session.session_id || ''))
    .map((session) => ({
      ...session,
      id: String(session.id || session.session_id || ''),
      title: String(session.title || session.name || '未命名对话'),
      workspace_hash: String(session.workspace_hash || session.workspaceHash || ''),
      updated_at: String(session.updated_at || session.created_at || ''),
    }))
    .sort((left, right) => parsedTime(right.updated_at) - parsedTime(left.updated_at));
}

export function normalizeExpertSwitchReceipt(value, fallbackExpertId = '') {
  const result = value && typeof value === 'object' ? value : {};
  const raw = result.receipt && typeof result.receipt === 'object' ? result.receipt : {};
  const sequence = Number(raw.sequence ?? result.control_sequence ?? 0);
  const applied = raw.applied === true
    || raw.state === 'applied'
    || result.applied === true;
  const state = applied ? 'applied' : 'queued';
  return {
    sequence: Number.isFinite(sequence) ? sequence : 0,
    expertId: String(raw.expert_id || result?.expert?.id || result.id || fallbackExpertId || ''),
    state,
    applied,
    pending: !applied,
    busy: !applied && result.busy === true,
    effectiveBoundary: String(
      raw.effective_boundary
      || result.effective_boundary
      || (applied ? 'applied' : 'queued_control'),
    ),
  };
}

export function shouldApplyExpertSwitchResponse(requestSequence, latestRequestSequence) {
  return Number(requestSequence) > 0
    && Number(requestSequence) === Number(latestRequestSequence);
}

export function resolveCanonicalExpertSwitchPoll({
  sessions,
  sessionId = '',
  targetExpertId = '',
  requestSequence = 0,
  latestRequestSequence = 0,
  latestTargetExpertId = '',
  attempt = 1,
  maxAttempts = 1,
  loadError = null,
} = {}) {
  const targetId = String(targetExpertId || '');
  const latestTargetId = String(latestTargetExpertId || '');
  if (!shouldApplyExpertSwitchResponse(requestSequence, latestRequestSequence)
      || !targetId
      || targetId !== latestTargetId) {
    return {
      status: 'stale',
      canonicalExpertId: '',
      session: null,
    };
  }

  const currentAttempt = Math.max(1, Number(attempt) || 1);
  const attemptLimit = Math.max(1, Number(maxAttempts) || 1);
  const exhausted = currentAttempt >= attemptLimit;
  if (loadError) {
    return {
      status: exhausted ? 'error' : 'retry',
      canonicalExpertId: '',
      session: null,
    };
  }

  const targetSessionId = String(sessionId || '');
  const session = normalizeTargetSessions(sessions)
    .find((item) => item.id === targetSessionId) || null;
  if (!session) {
    return {
      status: exhausted ? 'missing' : 'retry',
      canonicalExpertId: '',
      session: null,
    };
  }

  const canonicalExpertId = String(
    session.expert_id
    || session.expertId
    || session.expert?.id
    || '',
  );
  if (canonicalExpertId === targetId) {
    return {
      status: 'matched',
      canonicalExpertId,
      session,
    };
  }
  return {
    status: exhausted ? 'mismatch' : 'retry',
    canonicalExpertId,
    session,
  };
}

export function shouldRequestExpertSwitch({
  expertId = '',
  currentExpertId = '',
  pendingExpertId = '',
  requestInFlight = false,
  hasDraftText = false,
} = {}) {
  if (!String(expertId || '')) return false;
  if (hasDraftText || requestInFlight || String(pendingExpertId || '')) return true;
  return String(expertId) !== String(currentExpertId || '');
}
