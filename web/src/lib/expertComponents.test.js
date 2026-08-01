import assert from 'node:assert/strict';
import {
  capabilityOptionsWithSaved,
  collectExpertTags,
  createExpertInternalId,
  emptyExpertForm,
  expertDispatchDraftFromRef,
  expertFormFromDetail,
  expertPayloadFromForm,
  filterExperts,
  groupBuiltinToolOptions,
  normalizeCapabilityCatalog,
  normalizeExpertCapabilities,
  normalizeExpertSwitchReceipt,
  normalizeExperts,
  parseLineList,
  resolveCanonicalExpertSwitchPoll,
  safeExpertAvatarUrl,
  selectedTeamMemberRows,
  selectedTeamExperts,
  setCapabilitySelectionBatch,
  setCapabilityScopeMode,
  shouldApplyExpertSwitchResponse,
  shouldRequestExpertSwitch,
  singleExpertsForTeam,
  sortExperts,
  toggleCapabilitySelection,
  toggleTeamExpert,
  validateExpertForm,
  validateExpertFormFields,
} from './expertComponents.js';

function test(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

const catalog = normalizeExperts({ experts: [
  {
    id: 'reviewer',
    display_name: '代码审查',
    type: 'agent',
    source: 'global',
    description: '检查实现质量',
    tags: ['OPC-一人公司', '开发', '开发'],
    expertise: ['架构设计', '代码质量'],
    quick_prompts: ['审查当前改动'],
    created_at: '2026-07-24T08:00:00Z',
  },
  {
    id: 'designer',
    display_name: '界面设计',
    type: 'agent',
    tags: ['设计'],
    expertise: ['交互流程'],
    created_at: '2026-07-25T08:00:00Z',
  },
  { id: 'delivery-team', display_name: '交付团队', type: 'team', tags: ['开发'] },
] });

test('expert list normalization preserves distinct expertise and opening prompts', () => {
  assert.equal(catalog.length, 3);
  assert.deepEqual(catalog[0].tags, ['OPC-一人公司', '开发']);
  assert.deepEqual(catalog[0].expertise, ['架构设计', '代码质量']);
  assert.deepEqual(catalog[0].quick_prompts, ['审查当前改动']);
});

test('expert avatars accept controlled URLs and reject local filesystem paths', () => {
  assert.equal(safeExpertAvatarUrl('/api/experts/reviewer/avatar'), '/api/experts/reviewer/avatar');
  assert.equal(safeExpertAvatarUrl('https://assets.example/avatar.png'), 'https://assets.example/avatar.png');
  assert.equal(safeExpertAvatarUrl('N:\\private\\avatar.png'), '');
  assert.equal(safeExpertAvatarUrl('file:///N:/private/avatar.png'), '');
  const [normalized] = normalizeExperts([{
    id: 'local-avatar',
    avatar_url: 'C:\\Users\\name\\avatar.png',
    avatar_path: 'C:\\Users\\name\\avatar.png',
  }]);
  assert.equal(normalized.avatar_url, '');
});

test('Tag membership is non-exclusive and combines with plain-language search', () => {
  assert.deepEqual(collectExpertTags(catalog, 'agent'), ['OPC-一人公司', '开发', '设计']);
  assert.deepEqual(filterExperts(catalog, { type: 'agent', tag: '开发' }).map((item) => item.id), ['reviewer']);
  assert.deepEqual(filterExperts(catalog, { type: 'agent', tag: 'OPC-一人公司' }).map((item) => item.id), ['reviewer']);
  assert.deepEqual(filterExperts(catalog, { type: 'agent', query: '代码审查' }).map((item) => item.id), ['reviewer']);
  assert.deepEqual(filterExperts(catalog, { type: 'agent', query: '架构' }).map((item) => item.id), ['reviewer']);
  assert.deepEqual(filterExperts(catalog, { type: 'agent', query: '审查当前' }).map((item) => item.id), ['reviewer']);
  assert.deepEqual(filterExperts(catalog, { type: 'team' }).map((item) => item.id), ['delivery-team']);
  assert.deepEqual(filterExperts(catalog, { query: 'reviewer' }), []);
});

test('recent and created sorting have real deterministic meanings', () => {
  assert.deepEqual(sortExperts(catalog, { sort: 'recent', recentIds: ['reviewer'] }).map((item) => item.id), [
    'reviewer',
    'delivery-team',
    'designer',
  ]);
  assert.deepEqual(sortExperts(catalog.filter((item) => item.type === 'agent'), { sort: 'created' }).map((item) => item.id), [
    'designer',
    'reviewer',
  ]);
});

test('line list parser trims, removes blank lines, and de-duplicates in order', () => {
  assert.deepEqual(parseLineList(' 架构设计 \n\n代码质量\n架构设计\r\n'), ['架构设计', '代码质量']);
});

test('standalone expert opening prompt remains a one-shot new-task draft', () => {
  assert.deepEqual(
    expertDispatchDraftFromRef({ initialDraftText: '帮我确定最应该先验证的假设' }),
    {
      present: true,
      text: '帮我确定最应该先验证的假设',
    },
  );
  assert.deepEqual(
    expertDispatchDraftFromRef({ initialDraftText: null }),
    {
      present: true,
      text: '',
    },
  );
  assert.deepEqual(expertDispatchDraftFromRef({}), {
    present: false,
    text: '',
  });
});

test('new expert forms generate valid hidden IDs', () => {
  assert.match(createExpertInternalId('agent'), /^expert-[a-z0-9-]+$/);
  assert.match(createExpertInternalId('team'), /^team-[a-z0-9-]+$/);
});

test('expert payload keeps Tags, expertise, prompts and optional scopes separate', () => {
  const form = emptyExpertForm('agent');
  Object.assign(form, {
    displayName: '代码审查专家',
    profession: '高级开发工程师',
    description: '检查当前实现',
    tags: ['开发', 'OPC-一人公司'],
    expertiseText: '架构设计\n代码质量',
    quickPromptsText: '审查当前改动\n给出重构建议',
    instructions: '先检查再说明影响',
    capabilities: {
      skills: ['code-review'],
      mcp_servers: [],
    },
  });
  assert.equal(validateExpertForm(form), '');
  assert.deepEqual(expertPayloadFromForm(form), {
    id: form.id,
    type: 'agent',
    display_name: '代码审查专家',
    profession: '高级开发工程师',
    description: '检查当前实现',
    tags: ['开发', 'OPC-一人公司'],
    expertise: ['架构设计', '代码质量'],
    quick_prompts: ['审查当前改动', '给出重构建议'],
    capabilities: {
      skills: ['code-review'],
      mcp_servers: [],
    },
    lead: {
      id: 'lead',
      display_name: '代码审查专家',
      profession: '高级开发工程师',
      instructions: '先检查再说明影响',
    },
  });
});

test('legacy, explicit empty, and selected capability scopes remain distinct', () => {
  assert.deepEqual(normalizeExpertCapabilities(undefined), {});
  assert.deepEqual(normalizeExpertCapabilities({ tools: [] }), { tools: [] });
  assert.deepEqual(normalizeExpertCapabilities({ tools: ['file_read', 'file_read'] }), { tools: ['file_read'] });
  let value = setCapabilityScopeMode({}, 'tools', 'custom');
  assert.deepEqual(value, { tools: [] });
  assert.deepEqual(
    setCapabilityScopeMode({}, 'tools', 'custom', ['file_read', 'file_write']),
    { tools: ['file_read', 'file_write'] },
  );
  value = toggleCapabilitySelection(value, 'tools', 'file_read');
  assert.deepEqual(value, { tools: ['file_read'] });
  value = setCapabilityScopeMode(value, 'tools', 'inherit');
  assert.deepEqual(value, {});
});

test('built-in tools group by stable families, retain order, and keep unknown tools visible', () => {
  const groups = groupBuiltinToolOptions([
    { id: 'future_runtime_tool' },
    { id: 'browser_click' },
    { id: 'file_write' },
    { id: 'file_read' },
    { id: 'bash' },
    { id: 'show_image' },
    { id: 'AskUserQuestion' },
    { id: 'skill_view' },
    { id: 'memory_read' },
    { id: 'browser_click', description: 'duplicate must not render twice' },
  ]);
  assert.deepEqual(groups.map((group) => [group.id, group.label]), [
    ['files', '文件与代码'],
    ['command_workspace', '命令与工作区'],
    ['browser', '浏览器'],
    ['search_visual', '搜索与视觉'],
    ['planning', '计划与任务'],
    ['skills_collaboration', '技能与协作'],
    ['memory_context', '记忆与上下文'],
    ['other', '其他工具'],
  ]);
  assert.deepEqual(groups[0].options.map((option) => option.id), ['file_write', 'file_read']);
  assert.deepEqual(groups[2].options.map((option) => option.id), ['browser_click']);
  assert.deepEqual(groups.at(-1).options.map((option) => option.id), ['future_runtime_tool']);
});

test('batch capability selection preserves unrelated ids and explicit empty scope', () => {
  let value = setCapabilitySelectionBatch(
    { skills: ['review'], tools: ['file_read', 'old_tool', 'browser_click'] },
    'tools',
    ['file_read', 'file_write', 'file_write'],
    true,
  );
  assert.deepEqual(value, {
    skills: ['review'],
    tools: ['file_read', 'old_tool', 'browser_click', 'file_write'],
  });
  value = setCapabilitySelectionBatch(value, 'tools', ['file_read', 'old_tool'], false);
  assert.deepEqual(value, {
    skills: ['review'],
    tools: ['browser_click', 'file_write'],
  });
  assert.deepEqual(setCapabilitySelectionBatch({}, 'tools', ['file_read'], false), { tools: [] });
});

test('saved missing capabilities stay visible and removable', () => {
  const normalized = normalizeCapabilityCatalog({
    tools: [{ id: 'file_read', available: true }],
    mcp_servers: [{ id: 'github', status: 'disconnected', available: false }],
  });
  assert.equal(normalized.tools[0].id, 'file_read');
  assert.equal(normalized.tools[0].default_enabled, true);
  assert.equal(normalized.tools[0].expert_selectable, true);
  assert.equal(normalized.mcp_servers[0].status, 'disconnected');
  assert.equal(normalized.mcp_servers[0].default_enabled, false);
  assert.equal(normalized.mcp_servers[0].expert_selectable, false);
  const options = capabilityOptionsWithSaved(normalized.tools, ['file_read', 'old_tool'], 'tools');
  assert.deepEqual(options.map((item) => item.id), ['file_read', 'old_tool']);
  assert.equal(options[1].status, 'missing');
  assert.equal(options[1].available, false);
  assert.equal(options[1].expert_selectable, false);
});

test('globally disabled installed capabilities remain expert-selectable without becoming defaults', () => {
  const catalog = normalizeCapabilityCatalog({
    skills: [{
      id: 'review',
      available: false,
      globally_enabled: false,
      default_enabled: false,
      expert_selectable: true,
      disabled_reason: 'globally_disabled',
    }],
  });
  assert.equal(catalog.skills[0].available, false);
  assert.equal(catalog.skills[0].default_enabled, false);
  assert.equal(catalog.skills[0].expert_selectable, true);
});

test('expert detail form round-trips optional scopes', () => {
  const form = expertFormFromDetail({
    id: 'reviewer',
    display_name: '审查',
    type: 'agent',
    expertise: ['架构'],
    quick_prompts: ['审查'],
    capabilities: { tools: [] },
    agents: [{ id: 'lead', instructions: '严格检查' }],
  });
  assert.equal(form.expertiseText, '架构');
  assert.equal(form.quickPromptsText, '审查');
  assert.deepEqual(form.capabilities, { tools: [] });
});

test('expert team payload references selected existing experts and one lead', () => {
  const form = emptyExpertForm('team', 'reviewer');
  Object.assign(form, {
    displayName: '交付团队',
    profession: '研发交付',
    tags: ['项目质量'],
    expertiseText: '需求拆解\n体验验收',
    quickPromptsText: '推进这个需求',
    selectedExpertIds: ['reviewer', 'tester', 'reviewer'],
    leadExpertId: 'reviewer',
  });
  assert.equal(validateExpertForm(form), '');
  assert.deepEqual(expertPayloadFromForm(form), {
    id: form.id,
    type: 'team',
    display_name: '交付团队',
    profession: '研发交付',
    description: '',
    tags: ['项目质量'],
    expertise: ['需求拆解', '体验验收'],
    quick_prompts: ['推进这个需求'],
    lead_expert_id: 'reviewer',
    member_expert_ids: ['tester'],
  });
});

test('team validation associates missing values with fields', () => {
  const form = emptyExpertForm('team', 'reviewer');
  const errors = validateExpertFormFields(form);
  assert.equal(errors.displayName, '请填写专家名称');
  assert.match(errors.members, /至少需要两位/);
});

test('team selection preserves draft and reassigns lead when removed', () => {
  let form = { ...emptyExpertForm('team', 'reviewer'), displayName: '交付团队' };
  form = toggleTeamExpert(form, 'tester');
  assert.deepEqual(form.selectedExpertIds, ['reviewer', 'tester']);
  assert.equal(form.leadExpertId, 'reviewer');
  form = toggleTeamExpert(form, 'reviewer');
  assert.deepEqual(form.selectedExpertIds, ['tester']);
  assert.equal(form.leadExpertId, 'tester');
  assert.equal(form.displayName, '交付团队');
});

test('team picker only offers existing single experts and resolves selections', () => {
  assert.deepEqual(singleExpertsForTeam(catalog).map((item) => item.id), ['reviewer', 'designer']);
  assert.deepEqual(
    selectedTeamExperts(
      { selectedExpertIds: ['designer', 'delivery-team'] },
      catalog,
    ).map((item) => item.id),
    ['designer'],
  );
});

test('team validation rejects missing, out-of-scope, and nested-team references', () => {
  const form = {
    ...emptyExpertForm('team'),
    displayName: '交付团队',
    selectedExpertIds: ['reviewer', 'missing-expert'],
    leadExpertId: 'reviewer',
  };
  let errors = validateExpertFormFields(form, catalog);
  assert.match(errors.members, /不可用成员/);
  assert.match(errors.members, /missing-expert/);

  const nested = {
    ...form,
    selectedExpertIds: ['reviewer', 'delivery-team'],
  };
  errors = validateExpertFormFields(nested, catalog);
  assert.match(errors.members, /交付团队/);

  const selfReference = {
    ...form,
    id: 'reviewer',
    selectedExpertIds: ['reviewer', 'designer'],
    leadExpertId: 'designer',
  };
  errors = validateExpertFormFields(selfReference, catalog);
  assert.match(errors.members, /代码审查/);
});

test('saved unavailable team members remain visible and removable', () => {
  const rows = selectedTeamMemberRows({
    id: 'editing-team',
    selectedExpertIds: ['reviewer', 'missing-expert', 'delivery-team'],
  }, catalog);
  assert.deepEqual(rows.map((row) => row.id), ['reviewer', 'missing-expert', 'delivery-team']);
  assert.equal(rows[0].unavailable, false);
  assert.equal(rows[1].unavailable_reason, 'missing');
  assert.equal(rows[2].unavailable_reason, 'nested_team');
});

test('expert switch receipts and latest-request checks are authoritative', () => {
  assert.deepEqual(normalizeExpertSwitchReceipt({
    busy: true,
    receipt: {
      sequence: 9,
      expert_id: 'designer',
      state: 'queued',
      applied: false,
      effective_boundary: 'next_turn',
    },
  }), {
    sequence: 9,
    expertId: 'designer',
    state: 'queued',
    applied: false,
    pending: true,
    busy: true,
    effectiveBoundary: 'next_turn',
  });
  assert.equal(shouldApplyExpertSwitchResponse(4, 4), true);
  assert.equal(shouldApplyExpertSwitchResponse(3, 4), false);
  assert.equal(shouldRequestExpertSwitch({
    expertId: 'current-a',
    currentExpertId: 'current-a',
    pendingExpertId: 'pending-b',
  }), true);
  assert.equal(shouldRequestExpertSwitch({
    expertId: 'current-a',
    currentExpertId: 'current-a',
  }), false);
  assert.equal(shouldRequestExpertSwitch({
    expertId: 'current-a',
    currentExpertId: 'current-a',
    hasDraftText: true,
  }), true);
});

test('canonical expert switch polling requires the latest sequence and target', () => {
  const sessions = [{
    id: 'session-a',
    expert_id: 'designer',
    expert: { id: 'designer', display_name: '界面设计' },
  }];
  assert.equal(resolveCanonicalExpertSwitchPoll({
    sessions,
    sessionId: 'session-a',
    targetExpertId: 'designer',
    requestSequence: 4,
    latestRequestSequence: 5,
    latestTargetExpertId: 'designer',
  }).status, 'stale');
  assert.equal(resolveCanonicalExpertSwitchPoll({
    sessions,
    sessionId: 'session-a',
    targetExpertId: 'designer',
    requestSequence: 5,
    latestRequestSequence: 5,
    latestTargetExpertId: 'reviewer',
  }).status, 'stale');
  const matched = resolveCanonicalExpertSwitchPoll({
    sessions,
    sessionId: 'session-a',
    targetExpertId: 'designer',
    requestSequence: 5,
    latestRequestSequence: 5,
    latestTargetExpertId: 'designer',
  });
  assert.equal(matched.status, 'matched');
  assert.equal(matched.canonicalExpertId, 'designer');
  assert.equal(matched.session.expert.display_name, '界面设计');
});

test('canonical expert switch polling retries before bounded mismatch and missing timeouts', () => {
  const base = {
    sessions: [{ id: 'session-a', expert_id: 'reviewer' }],
    sessionId: 'session-a',
    targetExpertId: 'designer',
    requestSequence: 8,
    latestRequestSequence: 8,
    latestTargetExpertId: 'designer',
    maxAttempts: 3,
  };
  assert.equal(resolveCanonicalExpertSwitchPoll({
    ...base,
    attempt: 2,
  }).status, 'retry');
  const mismatch = resolveCanonicalExpertSwitchPoll({
    ...base,
    attempt: 3,
  });
  assert.equal(mismatch.status, 'mismatch');
  assert.equal(mismatch.canonicalExpertId, 'reviewer');
  assert.equal(resolveCanonicalExpertSwitchPoll({
    ...base,
    sessions: [],
    attempt: 3,
  }).status, 'missing');
  assert.equal(resolveCanonicalExpertSwitchPoll({
    ...base,
    loadError: new Error('offline'),
    attempt: 3,
  }).status, 'error');
});
