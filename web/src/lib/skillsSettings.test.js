// skillsSettings.js 的单元测试。
//
// 设置页「技能」tab 的过滤/分组/计数全部走这些纯函数;组件层只做渲染。
// 覆盖:
//  - normalizeSkillList 对脏数据(非数组 / 缺 name / 未知 source)的容错
//  - filterSkills 同时匹配 name 与 description、大小写不敏感、空查询直通
//  - groupSkillsBySource 项目/全局二分,幽灵条目(source="")落全局组
//  - skillsEnabledSummary 的 N/M 文案

import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import {
  DEFAULT_SKILL_PAGE_SIZE,
  SKILL_PAGE_SIZES,
  enabledRatioLabel,
  filterSkills,
  groupSkillsBySource,
  isSkillPageSize,
  normalizeSkillList,
  normalizeWorkspaceList,
  paginateSkills,
  skillsEnabledSummary,
  workspaceAutoExpand,
} from './skillsSettings.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

function between(text, start, end) {
  const from = text.indexOf(start);
  const to = text.indexOf(end, from + start.length);
  assert.notEqual(from, -1, `missing start marker: ${start}`);
  assert.notEqual(to, -1, `missing end marker: ${end}`);
  return text.slice(from, to);
}

run('normalizeSkillList 非数组输入返回空数组', () => {
  assert.deepEqual(normalizeSkillList(null), []);
  assert.deepEqual(normalizeSkillList(undefined), []);
  assert.deepEqual(normalizeSkillList({}), []);
});

run('normalizeSkillList 丢掉无 name 条目;未知 source 归 global', () => {
  const out = normalizeSkillList([
    { name: 'a', description: 'x', source: 'project', enabled: true },
    { name: '', description: 'dropped' },
    { description: 'no name at all' },
    // 幽灵禁用条目:后端给 source=""
    { name: 'ghost', description: '', source: '', enabled: false },
    { name: 'weird', source: 'somewhere-else', enabled: 1 },
  ]);
  assert.deepEqual(out, [
    { name: 'a', description: 'x', source: 'project', enabled: true },
    { name: 'ghost', description: '', source: 'global', enabled: false },
    { name: 'weird', description: '', source: 'global', enabled: true },
  ]);
});

const SKILLS = normalizeSkillList([
  { name: 'code-review', description: '代码审查和建议', source: 'project', enabled: true },
  { name: 'git-workflow', description: 'Git 操作自动化', source: 'global', enabled: true },
  { name: 'test-writer', description: '自动生成测试用例', source: 'global', enabled: false },
]);

run('filterSkills 空查询/全空白查询直通原列表', () => {
  assert.equal(filterSkills(SKILLS, ''), SKILLS);
  assert.equal(filterSkills(SKILLS, '   '), SKILLS);
  assert.equal(filterSkills(SKILLS, null), SKILLS);
});

run('filterSkills 按名称匹配(大小写不敏感)', () => {
  const out = filterSkills(SKILLS, 'GIT');
  assert.deepEqual(out.map((s) => s.name), ['git-workflow']);
});

run('filterSkills 按 description 匹配(用户要求:搜索同时搜描述)', () => {
  const out = filterSkills(SKILLS, '测试用例');
  assert.deepEqual(out.map((s) => s.name), ['test-writer']);
});

run('filterSkills 无命中返回空数组', () => {
  assert.deepEqual(filterSkills(SKILLS, 'nonexistent-keyword'), []);
});

run('skill pagination defaults to 50 and covers 20/50/100 without gaps', () => {
  assert.equal(DEFAULT_SKILL_PAGE_SIZE, 50);
  assert.deepEqual(SKILL_PAGE_SIZES, [20, 50, 100]);
  const skills = Array.from({ length: 237 }, (_, i) => ({ name: `skill-${i}`, description: '', enabled: i % 2 === 0 }));
  assert.equal(paginateSkills(skills).items.length, 50);
  for (const size of SKILL_PAGE_SIZES) {
    assert.equal(isSkillPageSize(size), true);
    const all = [];
    const pageCount = Math.ceil(skills.length / size);
    for (let page = 1; page <= pageCount; page += 1) {
      const result = paginateSkills(skills, page, size);
      assert.equal(result.page, page);
      assert.equal(result.pageCount, pageCount);
      assert.equal(result.total, skills.length);
      assert.ok(result.items.length <= size);
      all.push(...result.items);
    }
    assert.deepEqual(all, skills);
  }
  for (const invalid of [0, 1, 25, 200, '50', null, undefined, NaN]) {
    assert.equal(isSkillPageSize(invalid), false);
    assert.equal(paginateSkills(skills, 1, invalid).pageSize, 50);
  }
});

run('skill pagination clamps after deletion and handles empty catalogs', () => {
  const skills = Array.from({ length: 51 }, (_, i) => ({ name: `skill-${i}` }));
  const last = paginateSkills(skills, 9);
  assert.equal(last.page, 2);
  assert.deepEqual(last.items, [skills[50]]);
  const shortened = paginateSkills(skills.slice(0, 30), last.page);
  assert.equal(shortened.page, 1);
  assert.equal(shortened.items.length, 30);
  for (const empty of [[], null, undefined]) {
    assert.deepEqual(paginateSkills(empty, 9), { items: [], page: 1, pageCount: 1, pageSize: 50, total: 0 });
  }
  for (const invalid of [-1, 0, NaN, Infinity]) assert.equal(paginateSkills(skills, invalid).page, 1);
});

run('skill pagination searches the full catalog and keeps full enabled totals', () => {
  const skills = Array.from({ length: 120 }, (_, i) => ({ name: `skill-${i}`, description: i === 119 ? 'needle' : '', enabled: true }));
  assert.equal(paginateSkills(skills).items.some((skill) => skill.description === 'needle'), false);
  const result = paginateSkills(filterSkills(skills, 'needle'));
  assert.deepEqual(result.items, [skills[119]]);
  assert.equal(result.total, 1);
  assert.equal(skillsEnabledSummary(skills).total, 120);
  assert.equal(skillsEnabledSummary(skills).enabled, 120);
});

run('groupSkillsBySource 项目/全局二分并保序', () => {
  const { project, global } = groupSkillsBySource(SKILLS);
  assert.deepEqual(project.map((s) => s.name), ['code-review']);
  assert.deepEqual(global.map((s) => s.name), ['git-workflow', 'test-writer']);
});

run('skillsEnabledSummary 统计全量启用数(图稿右上角 N/M 已启用)', () => {
  const summary = skillsEnabledSummary(SKILLS);
  assert.equal(summary.enabled, 2);
  assert.equal(summary.total, 3);
  assert.equal(summary.label, '2 / 3 已启用');
});

run('skillsEnabledSummary 空列表', () => {
  assert.equal(skillsEnabledSummary([]).label, '0 / 0 已启用');
});

run('enabledRatioLabel 工作区折叠行紧凑计数(启用/总数)', () => {
  assert.equal(enabledRatioLabel(SKILLS.filter((s) => s.source === 'global')), '1/2');
  assert.equal(enabledRatioLabel([]), '0/0');
  // 未加载(非数组)按空处理,不抛异常
  assert.equal(enabledRatioLabel(null), '0/0');
});

run('normalizeWorkspaceList 丢掉缺 hash/cwd 的条目,name 空时回退 cwd', () => {
  const out = normalizeWorkspaceList([
    { hash: 'h1', cwd: 'N:/repo', name: 'repo' },
    { hash: 'h2', cwd: 'N:/other', name: '' },
    { hash: '', cwd: 'N:/ghost' },
    { cwd: 'N:/nohash' },
    null,
  ]);
  assert.deepEqual(out, [
    { hash: 'h1', cwd: 'N:/repo', name: 'repo' },
    { hash: 'h2', cwd: 'N:/other', name: 'N:/other' },
  ]);
  assert.deepEqual(normalizeWorkspaceList(null), []);
});

run('workspaceAutoExpand 搜索命中已加载的工作区才自动展开', () => {
  const wsSkills = normalizeSkillList([
    { name: 'rail-analyzer', description: '铁路数据分析', source: 'project', enabled: true },
  ]);
  assert.equal(workspaceAutoExpand(wsSkills, '铁路'), true);
  assert.equal(workspaceAutoExpand(wsSkills, 'no-hit'), false);
  // 未加载(null)不展开 — 防止搜索时所有工作区无脑弹开
  assert.equal(workspaceAutoExpand(null, '铁路'), false);
});

run('技能设置以响应式卡片网格复用全局和工作区条目', () => {
  const srcRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
  const settings = fs.readFileSync(path.join(srcRoot, 'components', 'SettingsPage.jsx'), 'utf8');
  const cards = between(settings, 'function SkillCard(', '// 单个工作区的折叠组');
  const workspace = between(settings, 'function WorkspaceSkillGroup(', 'function SectionSkills(');
  const section = between(settings, 'function SectionSkills(', 'function SectionMCP(');

  assert.match(cards, /data-skill-card="true"/);
  assert.match(cards, /data-skill-card-grid="true"/);
  assert.match(cards, /lg:grid-cols-2 xl:grid-cols-3 2xl:grid-cols-4/);
  assert.match(cards, /border-accent\/40 bg-accent-bg/);
  assert.doesNotMatch(cards, /shadow-\[inset_[^\]]+\]/);
  assert.doesNotMatch(cards, /border-l-(?:accent|\[[^\]]+\])/);
  assert.match(cards, /line-clamp-4/);
  assert.match(cards, /ariaLabel=\{`切换技能 \$\{skill\.name\}`\}/);
  assert.match(cards, /disabled=\{busyName === skill\.name\}/);
  assert.match(cards, /busyName=\{busyName\}/);
  assert.doesNotMatch(cards, /disabled=\{busy\}/);
  assert.match(workspace, /<SkillCardGrid skills=\{shown\}/);
  assert.match(workspace, /busyName=\{busyName\}/);
  assert.match(section, /<SkillCardGrid skills=\{filteredGlobal\}/);
  assert.match(section, /busyName=\{savingName\}/);
  assert.doesNotMatch(section, /busy=\{!!savingName\}/);
  assert.doesNotMatch(settings, /function SkillRow\(/);
});
