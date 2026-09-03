// skillsSettings.js 的单元测试。
//
// 设置页「技能」tab 的过滤/分组/计数全部走这些纯函数;组件层只做渲染。
// 覆盖:
//  - normalizeSkillList 对脏数据(非数组 / 缺 name / 未知 source)的容错
//  - filterSkills 同时匹配 name 与 description、大小写不敏感、空查询直通
//  - groupSkillsBySource 项目/全局二分,幽灵条目(source="")落全局组
//  - skillsEnabledSummary 的 N/M 文案
//  - status=error/warning 条目的判定、计数与计入分母的方式(坏 skill 必须
//    出现在列表里并被标出来,而不是消失或把整页拖成 500)

import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import {
  enabledRatioLabel,
  filterSkills,
  groupSkillsBySource,
  normalizeSkillList,
  normalizeWorkspaceList,
  skillHasWarning,
  skillIssueCounts,
  skillIssueMessage,
  skillLoadFailed,
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
    { name: 'a', description: 'x', source: 'project', enabled: true, status: 'ok', error: '', errorCode: '', path: '' },
    { name: 'ghost', description: '', source: 'global', enabled: false, status: 'ok', error: '', errorCode: '', path: '' },
    { name: 'weird', description: '', source: 'global', enabled: true, status: 'ok', error: '', errorCode: '', path: '' },
  ]);
});

run('normalizeSkillList 保留 status/error/path;老 daemon 缺 status 时按 ok', () => {
  const out = normalizeSkillList([
    { name: 'legacy', description: 'no status field', source: 'global', enabled: true },
    {
      name: 'broken',
      description: '',
      source: 'global',
      enabled: false,
      status: 'error',
      error: 'SKILL.md 为空或无法读取',
      error_code: 'unreadable',
      path: '/skills/broken/SKILL.md',
    },
    { name: 'odd', source: 'global', status: 'not-a-real-status' },
  ]);
  assert.equal(out[0].status, 'ok');
  assert.equal(out[1].status, 'error');
  assert.equal(out[1].errorCode, 'unreadable');
  assert.equal(out[1].path, '/skills/broken/SKILL.md');
  // 未知 status 一律按 ok,避免后端加新状态时前端整块变红
  assert.equal(out[2].status, 'ok');
});

run('skillLoadFailed / skillHasWarning 按 status 判定', () => {
  const [ok, warned, failed] = normalizeSkillList([
    { name: 'ok', status: 'ok' },
    { name: 'warned', status: 'warning', error: 'frontmatter 未闭合' },
    { name: 'failed', status: 'error', error: '无法读取' },
  ]);
  assert.equal(skillLoadFailed(ok), false);
  assert.equal(skillLoadFailed(failed), true);
  assert.equal(skillHasWarning(warned), true);
  assert.equal(skillHasWarning(failed), false);
  // 未加载 / 脏输入不抛
  assert.equal(skillLoadFailed(null), false);
  assert.equal(skillHasWarning(undefined), false);
});

run('skillIssueMessage 按 error_code 出本地化文案,未知 code 回落后端原文', () => {
  const [known, unknown, plain, ok] = normalizeSkillList([
    { name: 'a', status: 'error', error_code: 'unreadable', error: 'SKILL.md is empty or could not be read' },
    // 新 daemon + 旧前端:code 不认识时用后端原文,总比什么都不说好
    { name: 'b', status: 'error', error_code: 'invented_later', error: 'something new went wrong' },
    // 连 error 都没有时兜一句通用文案
    { name: 'c', status: 'error' },
    { name: 'd', status: 'ok', error: 'ignored' },
  ]);
  assert.equal(skillIssueMessage(known), 'SKILL.md 为空或无法读取');
  assert.equal(skillIssueMessage(unknown), 'something new went wrong');
  assert.equal(skillIssueMessage(plain), '无法解析该技能的 SKILL.md');
  assert.equal(skillIssueMessage(ok), '');
  assert.equal(skillIssueMessage(null), '');
});

run('skillIssueCounts 数出加载失败与告警条数', () => {
  const list = normalizeSkillList([
    { name: 'a', status: 'ok' },
    { name: 'b', status: 'error' },
    { name: 'c', status: 'warning' },
    { name: 'd', status: 'error' },
  ]);
  assert.deepEqual(skillIssueCounts(list), { failed: 2, warned: 1 });
  assert.deepEqual(skillIssueCounts(null), { failed: 0, warned: 0 });
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

run('filterSkills 也匹配失败原因(坏 skill 的 description 是空的)', () => {
  const list = normalizeSkillList([
    { name: 'broken', description: '', status: 'error', error_code: 'unterminated_frontmatter' },
    { name: 'fine', description: '正常技能', status: 'ok' },
  ]);
  // 匹配的是渲染出来的本地化文案,不是后端那份英文原文
  assert.deepEqual(filterSkills(list, '没有闭合').map((s) => s.name), ['broken']);
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
  assert.equal(summary.failed, 0);
  assert.equal(summary.label, '2 / 3 已启用');
});

run('skillsEnabledSummary 加载失败的不进分母,单独计数', () => {
  // 分母算上永远启不起来的条目,用户会以为自己关掉了几个技能。
  const summary = skillsEnabledSummary(normalizeSkillList([
    { name: 'a', enabled: true },
    { name: 'b', enabled: false },
    { name: 'broken', enabled: false, status: 'error', error: '无法读取' },
  ]));
  assert.equal(summary.enabled, 1);
  assert.equal(summary.total, 2);
  assert.equal(summary.failed, 1);
  assert.equal(summary.label, '1 / 2 已启用 · 1 个加载失败');
});

run('skillsEnabledSummary 空列表', () => {
  assert.equal(skillsEnabledSummary([]).label, '0 / 0 已启用');
  assert.equal(skillsEnabledSummary(null).label, '0 / 0 已启用');
});

run('enabledRatioLabel 工作区折叠行紧凑计数(启用/总数)', () => {
  assert.equal(enabledRatioLabel(SKILLS.filter((s) => s.source === 'global')), '1/2');
  assert.equal(enabledRatioLabel([]), '0/0');
  // 未加载(非数组)按空处理,不抛异常
  assert.equal(enabledRatioLabel(null), '0/0');
});

run('enabledRatioLabel 有加载失败时追加 ⚠N', () => {
  const list = normalizeSkillList([
    { name: 'a', enabled: true },
    { name: 'broken', status: 'error' },
  ]);
  assert.equal(enabledRatioLabel(list), '1/1 ⚠1');
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
  // 加载失败的技能没有可注册的技能名,开关必须一并禁用
  assert.match(cards, /disabled=\{busyName === skill\.name \|\| failed\}/);
  // 坏 skill 必须在卡片上被标出来,并给出原因和文件路径
  assert.match(cards, /data-skill-status=/);
  assert.match(cards, /加载失败/);
  assert.match(cards, /配置异常/);
  assert.match(cards, /border-danger\/40 bg-danger\/10/);
  assert.match(cards, /const issue = skillIssueMessage\(skill\);/);
  assert.match(cards, /\{issue\}/);
  assert.match(cards, /title=\{skill\.path\}/);
  assert.match(cards, /busyName=\{busyName\}/);
  assert.doesNotMatch(cards, /disabled=\{busy\}/);
  assert.match(workspace, /<SkillCardGrid skills=\{shown\}/);
  assert.match(workspace, /busyName=\{busyName\}/);
  assert.match(section, /<SkillCardGrid skills=\{filteredGlobal\}/);
  assert.match(section, /busyName=\{savingName\}/);
  assert.doesNotMatch(section, /busy=\{!!savingName\}/);
  assert.doesNotMatch(settings, /function SkillRow\(/);
});
