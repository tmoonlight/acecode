import assert from 'node:assert/strict';
import {
  capabilityOptionsWithSaved,
  collectExpertTags,
  createExpertInternalId,
  emptyExpertForm,
  expertFormFromDetail,
  expertPayloadFromForm,
  filterExperts,
  normalizeCapabilityCatalog,
  normalizeExpertCapabilities,
  normalizeExperts,
  parseLineList,
  selectedTeamExperts,
  setCapabilityScopeMode,
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
    author: '吴八哥',
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
    author: '言之',
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

test('Tag membership is non-exclusive and combines with plain-language search', () => {
  assert.deepEqual(collectExpertTags(catalog, 'agent'), ['OPC-一人公司', '开发', '设计']);
  assert.deepEqual(filterExperts(catalog, { type: 'agent', tag: '开发' }).map((item) => item.id), ['reviewer']);
  assert.deepEqual(filterExperts(catalog, { type: 'agent', tag: 'OPC-一人公司' }).map((item) => item.id), ['reviewer']);
  assert.deepEqual(filterExperts(catalog, { type: 'agent', query: '吴八哥' }).map((item) => item.id), ['reviewer']);
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

test('new expert forms generate valid hidden IDs', () => {
  assert.match(createExpertInternalId('agent'), /^expert-[a-z0-9-]+$/);
  assert.match(createExpertInternalId('team'), /^team-[a-z0-9-]+$/);
});

test('expert payload keeps author, Tags, expertise, prompts and optional scopes separate', () => {
  const form = emptyExpertForm('agent');
  Object.assign(form, {
    displayName: '代码审查专家',
    author: '吴八哥',
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
    author: '吴八哥',
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
  value = toggleCapabilitySelection(value, 'tools', 'file_read');
  assert.deepEqual(value, { tools: ['file_read'] });
  value = setCapabilityScopeMode(value, 'tools', 'inherit');
  assert.deepEqual(value, {});
});

test('saved missing capabilities stay visible and removable', () => {
  const normalized = normalizeCapabilityCatalog({
    tools: [{ id: 'file_read', available: true }],
    mcp_servers: [{ id: 'github', status: 'disconnected', available: false }],
  });
  assert.equal(normalized.tools[0].id, 'file_read');
  assert.equal(normalized.mcp_servers[0].status, 'disconnected');
  const options = capabilityOptionsWithSaved(normalized.tools, ['file_read', 'old_tool'], 'tools');
  assert.deepEqual(options.map((item) => item.id), ['file_read', 'old_tool']);
  assert.equal(options[1].status, 'missing');
  assert.equal(options[1].available, false);
});

test('expert detail form round-trips optional scopes', () => {
  const form = expertFormFromDetail({
    id: 'reviewer',
    display_name: '审查',
    author: '吴八哥',
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
    author: 'ACECode',
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
    author: 'ACECode',
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
  assert.equal(errors.author, '请填写姓名或称呼');
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
