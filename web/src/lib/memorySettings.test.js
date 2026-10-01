// memorySettings.js 的单元测试 + 设置 > 个性化 > 记忆 的接线检查。
//
// 覆盖:
//  - 工作区 hash 规整与 REST 路径 / 重置请求体(全局条目不带工作区参数)
//  - /api/config/memory 载荷规整、单控件 PATCH、乐观合并、控件可用性
//  - 摘要模型下拉(「当前模型」+ 已保存模型 + 已失效的已选模型)
//  - /api/memory 总览:条目规整 / 去重 / 最新在前、作用域页签、摘要状态整理
//  - 编辑表单:草稿、校验、改动判定、PUT body
//  - 错误码与文案(旧版 daemon 的裸 404、结构化错误码、带说明的失败)
//  - API 客户端实际发出的请求(会替换全局 fetch,所以在 runTests.js 的串行段执行)
//  - 组件接线:个性化页挂载、确认框默认操作、设置搜索、/memory 命令、系统通知标题

import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { ApiError, createApi } from './api.js';
import {
  MEMORY_TYPE_OPTIONS,
  applyMemorySettingsPatch,
  memoryEntryDraft,
  memoryEntryDraftChanged,
  memoryEntryPath,
  memoryEntryUpdateBody,
  memoryErrorCode,
  memoryErrorMessage,
  memoryOverviewPath,
  memoryResetBody,
  memoryScopeTabs,
  memorySettingsControls,
  memorySettingsPatch,
  memoryStatusView,
  memorySummaryModelNames,
  memorySummaryModelOptions,
  memoryTimeMs,
  memoryTypeLabel,
  memoryWorkspaceHash,
  normalizeMemoryEntry,
  normalizeMemoryEntryDetail,
  normalizeMemoryOverview,
  normalizeMemorySettings,
  resolveMemoryScope,
  shouldRetryMemoryOverviewWithoutWorkspace,
  sortMemoryEntries,
  validateMemoryEntryDraft,
} from './memorySettings.js';
import { searchSettings, settingsSearchEntries } from './settingsSearch.js';
import { commandsWithFallback, parseExecutableBuiltinCommand } from './slashCommands.js';
import { translationCatalogs } from '../i18n/catalogs.js';

const srcRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const source = (relativePath) => fs.readFileSync(path.join(srcRoot, relativePath), 'utf8');

function between(text, start, end) {
  const from = text.indexOf(start);
  const to = text.indexOf(end, from + start.length);
  assert.notEqual(from, -1, `missing start marker: ${start}`);
  assert.notEqual(to, -1, `missing end marker: ${end}`);
  return text.slice(from, to);
}

async function run(name, fn) {
  try {
    await fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

// ─── 工作区与路径 ──────────────────────────────────────────────────────────

// 触发场景:设置窗口拿到的「当前工作区」可能是空串、前端的无工作区占位
//          (__no_workspace__)、单 daemon 网页模式下的 __local__,或带空白的真实 hash。
// 期望行为:空值与 __no_workspace__ 视为没有工作区;__local__ 原样保留 —— daemon 的
//          resolve_workspace 把它解析成自己的工作目录,是合法的当前工作区;
//          真实 hash 去空白后原样使用。
// 回归表现:曾把 __local__ 也当成占位丢掉,网页模式首页选中本地项目后,
//          「当前工作区」页签仍显示「当前没有打开工作区」。
await run('memoryWorkspaceHash 只把 __no_workspace__ 视为没有工作区', () => {
  assert.equal(memoryWorkspaceHash(''), '');
  assert.equal(memoryWorkspaceHash('__no_workspace__'), '');
  assert.equal(memoryWorkspaceHash('__local__'), '__local__');
  assert.equal(memoryWorkspaceHash(undefined), '');
  assert.equal(memoryWorkspaceHash(42), '');
  assert.equal(memoryWorkspaceHash('  a1b2c3  '), 'a1b2c3');
});

// 触发场景:读总览 / 读写删条目 / 重置。
// 期望行为:总览按需带 ?workspace=;全局条目与全局重置不带工作区 —— 当前工作区被移除后
//          daemon 对未知 hash 回 404 UNKNOWN_WORKSPACE,带上它会让全局记忆也无法编辑。
//          条目名与 hash 都做 URL 编码。
await run('记忆 REST 路径只在工作区作用域带 workspace 参数', () => {
  assert.equal(memoryOverviewPath(''), '/api/memory');
  assert.equal(memoryOverviewPath('__no_workspace__'), '/api/memory');
  assert.equal(memoryOverviewPath('__local__'), '/api/memory?workspace=__local__');
  assert.equal(memoryOverviewPath('hash 1'), '/api/memory?workspace=hash%201');
  assert.equal(memoryEntryPath('global', 'user_role', 'abc'), '/api/memory/global/user_role');
  assert.equal(memoryEntryPath('workspace', 'build-tips', 'abc'), '/api/memory/workspace/build-tips?workspace=abc');
  assert.equal(memoryEntryPath('workspace', 'a/b', ''), '/api/memory/workspace/a%2Fb');
  assert.deepEqual(memoryResetBody('global', 'abc'), { scope: 'global', workspace: '' });
  assert.deepEqual(memoryResetBody('workspace', ' abc '), { scope: 'workspace', workspace: 'abc' });
  assert.deepEqual(memoryResetBody('workspace', '__no_workspace__'), { scope: 'workspace', workspace: '' });
});

// ─── 设置 ──────────────────────────────────────────────────────────────────

// 触发场景:GET /api/config/memory 的完整响应,以及字段缺失 / 类型不对的旧响应。
// 期望行为:完整响应原样保留;缺字段时「使用记忆」按开启、「记忆摘要」按关闭(默认关)、
//          摘要模型为空(=当前模型)、summary_available 为 false。
await run('normalizeMemorySettings 规整设置载荷并采用安全默认值', () => {
  assert.deepEqual(normalizeMemorySettings({
    enabled: false,
    max_index_bytes: 32768,
    summary: { enabled: true, model_name: ' local ', idle_minutes: 30, max_session_age_days: 14 },
    summary_available: true,
  }), {
    enabled: false,
    max_index_bytes: 32768,
    summary: { enabled: true, model_name: 'local', idle_minutes: 30, max_session_age_days: 14 },
    summary_available: true,
  });
  assert.deepEqual(normalizeMemorySettings(null), {
    enabled: true,
    max_index_bytes: 0,
    summary: { enabled: false, model_name: '', idle_minutes: 0, max_session_age_days: 0 },
    summary_available: false,
  });
  const loose = normalizeMemorySettings({ enabled: 'no', summary: { enabled: 'yes', model_name: 7 } });
  assert.equal(loose.enabled, true, '非布尔的 enabled 不当成关闭');
  assert.equal(loose.summary.enabled, false, '非布尔的 summary.enabled 不当成开启');
  assert.equal(loose.summary.model_name, '');
});

// 触发场景:用户切「使用记忆」/「记忆摘要」开关、改摘要模型。
// 期望行为:PUT body 只带变化的那一个字段(summary 是局部对象),daemon 保留其余字段,
//          另一个窗口刚改的设置不会被这次保存覆盖;未知字段不发请求。
await run('memorySettingsPatch 只带变化的字段', () => {
  assert.deepEqual(memorySettingsPatch('enabled', false), { enabled: false });
  assert.deepEqual(memorySettingsPatch('enabled', true), { enabled: true });
  assert.deepEqual(memorySettingsPatch('summary.enabled', true), { summary: { enabled: true } });
  assert.deepEqual(memorySettingsPatch('summary.model_name', ' local-small '), { summary: { model_name: 'local-small' } });
  assert.deepEqual(memorySettingsPatch('summary.model_name', ''), { summary: { model_name: '' } });
  assert.equal(memorySettingsPatch('max_index_bytes', 1), null);
  assert.equal(memorySettingsPatch('enabled', 'true').enabled, false, '只有布尔 true 才算开启');
});

// 触发场景:点开关后、PUT 返回之前先展示新值(乐观更新)。
// 期望行为:PATCH 叠加到当前设置,summary 局部合并 —— 切「记忆摘要」不能把已选模型清掉。
await run('applyMemorySettingsPatch 乐观合并且保留未改动字段', () => {
  const current = normalizeMemorySettings({
    enabled: true,
    summary: { enabled: false, model_name: 'local', idle_minutes: 20 },
    summary_available: true,
  });
  const toggled = applyMemorySettingsPatch(current, memorySettingsPatch('summary.enabled', true));
  assert.equal(toggled.summary.enabled, true);
  assert.equal(toggled.summary.model_name, 'local');
  assert.equal(toggled.summary.idle_minutes, 20);
  assert.equal(toggled.summary_available, true);
  const disabled = applyMemorySettingsPatch(current, memorySettingsPatch('enabled', false));
  assert.equal(disabled.enabled, false);
  assert.equal(disabled.summary.model_name, 'local');
  assert.deepEqual(applyMemorySettingsPatch(current, null), current);
});

// 触发场景:设置读取中 / 保存中 / 「使用记忆」关闭 / daemon 不能运行记忆摘要。
// 期望行为:读取中与保存中整组禁用;「使用记忆」关闭时摘要控件禁用并给灰色原因;
//          daemon 不支持摘要时禁用并给警示色原因;状态区只在记忆摘要实际生效
//          (使用记忆与记忆摘要都开启)时出现。
await run('memorySettingsControls 按状态禁用记忆摘要控件并说明原因', () => {
  const ready = normalizeMemorySettings({ enabled: true, summary: { enabled: true }, summary_available: true });
  assert.deepEqual(memorySettingsControls(ready), {
    enabledDisabled: false, summaryDisabled: false, summaryHint: '', summaryHintTone: '', showStatus: true,
  });
  const loading = memorySettingsControls(null, { loading: true });
  assert.equal(loading.enabledDisabled, true);
  assert.equal(loading.summaryDisabled, true);
  assert.equal(loading.summaryHint, '', '读取中不提前下结论');
  assert.equal(memorySettingsControls(ready, { busy: true }).enabledDisabled, true);
  assert.equal(memorySettingsControls(ready, { busy: true }).summaryDisabled, true);

  const memoryOff = memorySettingsControls({ ...ready, enabled: false });
  assert.equal(memoryOff.enabledDisabled, false, '关闭后仍能重新开启');
  assert.equal(memoryOff.summaryDisabled, true);
  assert.equal(memoryOff.summaryHint, '需要先开启「使用记忆」');
  assert.equal(memoryOff.summaryHintTone, 'mute');
  assert.equal(memoryOff.showStatus, false, '「使用记忆」关闭时摘要不运行,不展示状态区');

  const unavailable = memorySettingsControls({ ...ready, summary_available: false });
  assert.equal(unavailable.summaryDisabled, true);
  assert.equal(unavailable.summaryHint, '当前后台服务无法运行记忆摘要');
  assert.equal(unavailable.summaryHintTone, 'warn');

  const summaryOff = memorySettingsControls({ ...ready, summary: { ...ready.summary, enabled: false } });
  assert.equal(summaryOff.showStatus, false);
  assert.equal(summaryOff.summaryDisabled, false, '摘要关闭时仍可预先选模型');
});

// 触发场景:GET /api/models 返回数组或 {models:[...]},可能含重复名与 daemon 合成的
//          "(session:<id>)" 临时模型。
// 期望行为:只留可保存的模型名,去重保序。
await run('memorySummaryModelNames 只列出已保存模型', () => {
  assert.deepEqual(memorySummaryModelNames([
    { name: 'chat' }, { name: 'local' }, { name: 'chat' }, { name: '(session:1)' }, { name: '' }, {},
  ]), ['chat', 'local']);
  assert.deepEqual(memorySummaryModelNames({ models: [{ name: 'a' }] }), ['a']);
  assert.deepEqual(memorySummaryModelNames(null), []);
});

// 触发场景:摘要模型下拉。已选模型被删除 / 改名后不在已保存列表里。
// 期望行为:第一项恒为「当前模型」(空值);已选但不存在的模型追加一项并标记不可用;
//          模型列表还没读到时不误判为不可用;已在列表里的已选项不重复出现。
await run('memorySummaryModelOptions 以「当前模型」开头并标出失效的已选模型', () => {
  const options = memorySummaryModelOptions(['chat', 'local'], 'gone');
  assert.deepEqual(options.map((option) => option.value), ['', 'chat', 'local', 'gone']);
  assert.equal(options[0].label, '当前模型');
  assert.equal(options[3].unavailable, true);
  assert.equal(memorySummaryModelOptions([], 'gone', { loaded: false })[1].unavailable, false);
  assert.deepEqual(memorySummaryModelOptions(['chat'], 'chat').map((option) => option.value), ['', 'chat']);
  assert.deepEqual(memorySummaryModelOptions(['chat'], '').map((option) => option.value), ['', 'chat']);
});

// ─── 条目与总览 ────────────────────────────────────────────────────────────

// 触发场景:/api/memory 返回的条目带非法名字、未知类型、重复的来源会话。
// 期望行为:名字不符合 [A-Za-z0-9_-]{1,64} 的条目丢掉(拼进 URL 只会 400);未知类型
//          置空(不硬套成四类之一);source 只有 summary / manual 两种;来源会话去重。
await run('normalizeMemoryEntry 过滤非法条目并规整字段', () => {
  assert.equal(normalizeMemoryEntry({ name: 'a/b' }), null);
  assert.equal(normalizeMemoryEntry({ name: '' }), null);
  assert.equal(normalizeMemoryEntry({ name: 'x'.repeat(65) }), null);
  assert.equal(normalizeMemoryEntry('user_role'), null);
  const entry = normalizeMemoryEntry({
    scope: 'workspace',
    name: 'build_tips',
    description: '构建技巧',
    type: 'mystery',
    created_at: '2026-09-01T00:00:00Z',
    updated_at: '2026-09-02T00:00:00Z',
    source: 'summary',
    source_sessions: ['s1', 's1', '', 3, 's2'],
  });
  assert.deepEqual(entry, {
    scope: 'workspace',
    name: 'build_tips',
    description: '构建技巧',
    type: '',
    created_at: '2026-09-01T00:00:00Z',
    updated_at: '2026-09-02T00:00:00Z',
    source: 'summary',
    source_sessions: ['s1', 's2'],
  });
  assert.equal(normalizeMemoryEntry({ name: 'n', source: 'bogus' }).source, 'manual');
  assert.equal(normalizeMemoryEntry({ name: 'n' }, 'workspace').scope, 'workspace');
  assert.equal(normalizeMemoryEntry({ name: 'n', type: 'feedback' }).type, 'feedback');
});

// 触发场景:条目列表排序。updated_at 可能为空(旧条目只有 created_at,甚至两者都没有)。
// 期望行为:最近更新的排最前;没有 updated_at 用 created_at;都没有的排最后;
//          同一时刻按名字排,结果稳定。
await run('sortMemoryEntries 最近更新在前,无时间的排最后', () => {
  const sorted = sortMemoryEntries([
    { name: 'no_time' },
    { name: 'old', updated_at: '2026-01-01T00:00:00Z' },
    { name: 'created_only', created_at: '2026-06-01T00:00:00Z' },
    { name: 'b_new', updated_at: '2026-09-01T00:00:00Z' },
    { name: 'a_new', updated_at: '2026-09-01T00:00:00Z' },
  ]);
  assert.deepEqual(sorted.map((entry) => entry.name), ['a_new', 'b_new', 'created_only', 'old', 'no_time']);
  assert.deepEqual(sortMemoryEntries(null), []);
  assert.equal(memoryTimeMs('not a date'), 0);
  assert.equal(memoryTimeMs(-5), 0);
  assert.equal(memoryTimeMs(1700000000000), 1700000000000);
});

// 触发场景:GET /api/memory 的完整响应(两种作用域 + 记忆摘要状态)。
// 期望行为:每个作用域内按名字去重、最新在前,条目的 scope 以所在作用域为准;
//          缺失的作用域视为不可用;状态字段规整为非负数字与字符串。
await run('normalizeMemoryOverview 分作用域规整条目与状态', () => {
  const overview = normalizeMemoryOverview({
    enabled: true,
    scopes: {
      global: {
        available: true,
        dir: 'C:/Users/me/.acecode/memory',
        entries: [
          { scope: 'global', name: 'older', updated_at: '2026-01-01T00:00:00Z' },
          { scope: 'global', name: 'newer', updated_at: '2026-09-01T00:00:00Z' },
          { scope: 'global', name: 'newer', updated_at: '2025-01-01T00:00:00Z' },
          { scope: 'global', name: 'bad name' },
        ],
      },
    },
    status: {
      summary_enabled: true,
      global_inbox: 3,
      workspace_inbox: -1,
      last_extraction_ms: 1700000000000,
      last_error: '  boom  ',
    },
  });
  assert.deepEqual(overview.scopes.global.entries.map((entry) => entry.name), ['newer', 'older']);
  assert.equal(overview.scopes.global.dir, 'C:/Users/me/.acecode/memory');
  assert.deepEqual(overview.scopes.workspace, { available: false, dir: '', entries: [] });
  assert.deepEqual(overview.status, {
    summary_enabled: true,
    global_inbox: 3,
    workspace_inbox: 0,
    last_extraction_ms: 1700000000000,
    last_consolidation_ms: 0,
    last_error: 'boom',
    last_error_ms: 0,
  });
  const mislabeled = normalizeMemoryOverview({
    scopes: { workspace: { available: true, entries: [{ scope: 'global', name: 'x' }] } },
  });
  assert.equal(mislabeled.scopes.workspace.entries[0].scope, 'workspace');
});

// 触发场景:当前工作区已从 daemon 移除 / 不是该 daemon 登记的,带 hash 读总览得到
//          404 UNKNOWN_WORKSPACE。
// 期望行为:只有「带了真实 hash + UNKNOWN_WORKSPACE」才不带工作区重读一次,把全局记忆
//          展示出来;旧版 daemon 没有这组路由的裸 404 不在此列(重读也没用)。
await run('shouldRetryMemoryOverviewWithoutWorkspace 只对未登记工作区退回全局', () => {
  const unknown = new ApiError(404, { error: 'UNKNOWN_WORKSPACE' });
  assert.equal(shouldRetryMemoryOverviewWithoutWorkspace(unknown, 'abc'), true);
  assert.equal(shouldRetryMemoryOverviewWithoutWorkspace(unknown, ''), false);
  assert.equal(shouldRetryMemoryOverviewWithoutWorkspace(unknown, '__no_workspace__'), false);
  assert.equal(shouldRetryMemoryOverviewWithoutWorkspace(new ApiError(404, ''), 'abc'), false);
  assert.equal(shouldRetryMemoryOverviewWithoutWorkspace(new ApiError(503, { error: 'UNAVAILABLE' }), 'abc'), false);
});

// 触发场景:「全局 / 当前工作区」页签。没有工作区、工作区未登记、工作区作用域不可用、
//          总览还没读到、一切正常。
// 期望行为:全局页签恒可用;工作区页签不可用时带原因并退回全局;可用时带条目数。
await run('memoryScopeTabs / resolveMemoryScope 处理工作区不可用的各种情况', () => {
  const overview = normalizeMemoryOverview({
    scopes: {
      global: { available: true, entries: [{ name: 'g1' }, { name: 'g2' }] },
      workspace: { available: true, entries: [{ name: 'w1' }] },
    },
  });
  const tabs = memoryScopeTabs(overview, { workspaceHash: 'abc' });
  assert.deepEqual(tabs.map((tab) => [tab.scope, tab.label, tab.count, tab.available, tab.hint]), [
    ['global', '全局', 2, true, ''],
    ['workspace', '当前工作区', 1, true, ''],
  ]);
  assert.equal(resolveMemoryScope('workspace', tabs), 'workspace');

  const noWorkspace = memoryScopeTabs(overview, { workspaceHash: '' });
  assert.equal(noWorkspace[1].available, false);
  assert.equal(noWorkspace[1].hint, '当前没有打开工作区');
  assert.equal(resolveMemoryScope('workspace', noWorkspace), 'global');

  const unknown = memoryScopeTabs(overview, { workspaceHash: 'abc', workspaceUnknown: true });
  assert.equal(unknown[1].available, false);
  assert.equal(unknown[1].hint, '当前工作区未在后台服务中登记');

  const unavailable = memoryScopeTabs(normalizeMemoryOverview({ scopes: { global: { available: true } } }), { workspaceHash: 'abc' });
  assert.equal(unavailable[1].available, false);
  assert.equal(unavailable[1].hint, '当前工作区的记忆不可用');

  const loading = memoryScopeTabs(null, { workspaceHash: 'abc' });
  assert.equal(loading[1].available, false, '总览没读到前不能选工作区页签');
  assert.equal(loading[1].hint, '', '读取中不显示不可用原因');
  assert.equal(resolveMemoryScope('global', loading), 'global');
  assert.equal(resolveMemoryScope('bogus', tabs), 'global');
});

// 触发场景:记忆摘要状态区。
// 期望行为:待整合观察数在工作区不可用时只报全局;最近错误早于之后一次成功的提炼 /
//          整合时标为 stale(已恢复,弱化显示),否则是当前问题;没有错误时为 null。
await run('memoryStatusView 整理待整合观察、时间与错误', () => {
  const status = {
    global_inbox: 2,
    workspace_inbox: 5,
    last_extraction_ms: 2000,
    last_consolidation_ms: 1000,
    last_error: 'model timeout',
    last_error_ms: 1500,
  };
  const withWorkspace = memoryStatusView(status, { workspaceAvailable: true });
  assert.deepEqual(withWorkspace.pending, [{ scope: 'global', count: 2 }, { scope: 'workspace', count: 5 }]);
  assert.equal(withWorkspace.pendingTotal, 7);
  assert.equal(withWorkspace.lastExtractionMs, 2000);
  assert.equal(withWorkspace.lastConsolidationMs, 1000);
  assert.deepEqual(withWorkspace.error, { message: 'model timeout', ms: 1500, stale: true });

  const globalOnly = memoryStatusView(status, { workspaceAvailable: false });
  assert.deepEqual(globalOnly.pending, [{ scope: 'global', count: 2 }]);
  assert.equal(globalOnly.pendingTotal, 2);

  assert.equal(memoryStatusView({ ...status, last_error_ms: 3000 }).error.stale, false);
  assert.equal(memoryStatusView({ ...status, last_error_ms: 0 }).error.stale, false, '没有错误时间时不判为已恢复');
  assert.equal(memoryStatusView({ ...status, last_error: '' }).error, null);
});

// ─── 编辑表单 ──────────────────────────────────────────────────────────────

// 触发场景:打开编辑框读到全文(GET 带 body / path),保存后得到 redactions。
// 期望行为:详情在条目字段之外带上 body / path / redactions(非负整数)。
await run('normalizeMemoryEntryDetail 带上正文、路径与脱敏次数', () => {
  assert.deepEqual(normalizeMemoryEntryDetail({
    scope: 'global', name: 'n', description: 'd', type: 'user', body: '# body', path: 'C:/m/n.md', redactions: 2,
  }), {
    scope: 'global', name: 'n', description: 'd', type: 'user', created_at: '', updated_at: '',
    source: 'manual', source_sessions: [], body: '# body', path: 'C:/m/n.md', redactions: 2,
  });
  assert.equal(normalizeMemoryEntryDetail({ name: 'n', redactions: -1 }).redactions, 0);
  assert.equal(normalizeMemoryEntryDetail({ name: '../x' }), null);
});

// 触发场景:编辑框的描述 / 类型 / 内容。
// 期望行为:与 daemon parse_memory_entry_edit 同口径 —— 描述必填(全空白算空),类型只能
//          是四类之一;描述写进 frontmatter,换行折成空格;类型不明时 PUT 不带 type,
//          daemon 保留原值;只改了描述两端空白不算改动。
await run('编辑表单的校验、改动判定与 PUT body', () => {
  const entry = { description: '用户偏好', type: 'user', body: '- 用中文回复' };
  const draft = memoryEntryDraft(entry);
  assert.deepEqual(draft, { description: '用户偏好', type: 'user', body: '- 用中文回复' });
  assert.deepEqual(validateMemoryEntryDraft(draft), {});
  assert.equal(memoryEntryDraftChanged(draft, entry), false);
  assert.equal(memoryEntryDraftChanged({ ...draft, description: '  用户偏好  ' }, entry), false);
  assert.equal(memoryEntryDraftChanged({ ...draft, body: '- 用英文回复' }, entry), true);
  assert.equal(memoryEntryDraftChanged({ ...draft, type: 'feedback' }, entry), true);

  assert.deepEqual(validateMemoryEntryDraft({ ...draft, description: ' \n ' }), { description: '描述不能为空' });
  assert.deepEqual(validateMemoryEntryDraft({ ...draft, type: 'bogus' }), { type: '类型无效' });
  assert.deepEqual(validateMemoryEntryDraft({ ...draft, type: '' }), {}, '未知类型保持原样可以保存');

  assert.deepEqual(memoryEntryUpdateBody({ description: ' 第一行\n第二行 ', type: 'project', body: 'x\n' }), {
    description: '第一行 第二行', body: 'x\n', type: 'project',
  });
  assert.deepEqual(memoryEntryUpdateBody({ description: 'd', type: '', body: '' }), { description: 'd', body: '' });
  assert.deepEqual(memoryEntryDraft({ type: 'mystery' }), { description: '', type: '', body: '' });
  assert.equal(memoryTypeLabel('reference'), '参考');
  assert.equal(memoryTypeLabel('mystery'), '');
  assert.deepEqual(MEMORY_TYPE_OPTIONS.map((option) => option.value), ['user', 'feedback', 'project', 'reference']);
});

// ─── 错误 ──────────────────────────────────────────────────────────────────

// 触发场景:各类失败 —— 旧版 daemon 没有这组路由(/api/* 未匹配时是不带错误体的裸 404)、
//          认证失效、记忆服务未初始化(503 UNAVAILABLE)、条目已被删除(NOT_FOUND)、
//          参数 / 写盘失败(带 daemon 说明)、网络断开(非 HTTP 异常)。
// 期望行为:结构化错误码优先;裸 404/405/501 → 不支持;401/403 → 认证;无码 5xx → 不可用;
//          已知码用固定文案,其余拼「操作失败:说明」。
// 回归表现:曾把 errors.js 的 NOT_FOUND(「该模型已不存在」)直接用在记忆上,提示文不对题。
await run('memoryErrorCode / memoryErrorMessage 区分旧版 daemon、结构化错误与网络错误', () => {
  assert.equal(memoryErrorCode(new ApiError(404, '')), 'MEMORY_UNSUPPORTED');
  assert.equal(memoryErrorCode(new ApiError(405, '')), 'MEMORY_UNSUPPORTED');
  assert.equal(memoryErrorCode(new ApiError(401, '')), 'MEMORY_AUTH_REQUIRED');
  assert.equal(memoryErrorCode(new ApiError(502, '')), 'MEMORY_UNAVAILABLE');
  assert.equal(memoryErrorCode(new ApiError(404, { error: 'NOT_FOUND' })), 'NOT_FOUND');
  assert.equal(memoryErrorCode(new ApiError(400, { error: 'BAD_REQUEST', message: 'x' })), 'BAD_REQUEST');
  assert.equal(memoryErrorCode(null), '');

  assert.equal(memoryErrorMessage(new ApiError(404, ''), '加载记忆失败'),
    '当前后台服务不支持记忆管理，请更新 ACECode 并重启后重试');
  assert.equal(memoryErrorMessage(new ApiError(404, { error: 'NOT_FOUND' }), '读取记忆失败'),
    '这条记忆已不存在，请刷新列表');
  assert.equal(memoryErrorMessage(new ApiError(503, { error: 'UNAVAILABLE' })), '记忆服务在当前后台进程中不可用');
  assert.equal(
    memoryErrorMessage(new ApiError(400, { error: 'WRITE_FAILED', message: 'disk full' }), '保存记忆失败'),
    '保存记忆失败:disk full',
  );
  assert.equal(
    memoryErrorMessage(new ApiError(500, { error: 'PERSIST_FAILED', message: 'denied' }), '保存记忆设置失败'),
    '保存记忆设置失败:denied',
  );
  assert.equal(memoryErrorMessage(new TypeError('Failed to fetch'), '加载记忆失败'), '加载记忆失败:Failed to fetch');
  assert.equal(memoryErrorMessage(new ApiError(400, { error: 'RESET_FAILED' }), '重置记忆失败'), '重置记忆失败');
  assert.equal(memoryErrorMessage(new ApiError(408, { error: 'TIMEOUT' }), '加载记忆失败'), '请求超时,请重试');
  assert.equal(memoryErrorMessage(null), '未知错误');
});

// ─── API 客户端 ────────────────────────────────────────────────────────────

// 触发场景:设置页经 api.js 调用七个记忆接口(带远程 Web token)。
// 期望行为:方法 / 路径 / 请求体与 daemon 路由一致;全局条目与全局重置不带工作区;
//          所有请求都带 X-ACECode-Token。
await run('记忆 API 客户端按 daemon 路由发请求', async () => {
  const previousFetch = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (url, options = {}) => {
    calls.push({ url, method: options.method, body: options.body ? JSON.parse(options.body) : undefined, headers: options.headers });
    return { ok: true, status: 200, headers: { get: () => 'application/json' }, json: async () => ({ ok: true }) };
  };
  try {
    const client = createApi({ origin: 'http://127.0.0.1:4567', token: 'tok' });
    await client.getMemorySettings();
    await client.setMemorySettings({ summary: { enabled: true } });
    await client.getMemoryOverview('ws1');
    await client.getMemoryEntry('global', 'user_role', 'ws1');
    await client.updateMemoryEntry('workspace', 'build_tips', { description: 'd', body: 'b', type: 'project' }, 'ws1');
    await client.deleteMemoryEntry('workspace', 'build_tips', 'ws1');
    await client.resetMemoryScope('global', 'ws1');
    await client.resetMemoryScope('workspace', 'ws1');
    assert.deepEqual(calls.map((call) => [call.method, call.url.replace('http://127.0.0.1:4567', '')]), [
      ['GET', '/api/config/memory'],
      ['PUT', '/api/config/memory'],
      ['GET', '/api/memory?workspace=ws1'],
      ['GET', '/api/memory/global/user_role'],
      ['PUT', '/api/memory/workspace/build_tips?workspace=ws1'],
      ['DELETE', '/api/memory/workspace/build_tips?workspace=ws1'],
      ['POST', '/api/memory/reset'],
      ['POST', '/api/memory/reset'],
    ]);
    assert.deepEqual(calls[1].body, { summary: { enabled: true } });
    assert.deepEqual(calls[4].body, { description: 'd', body: 'b', type: 'project' });
    assert.deepEqual(calls[6].body, { scope: 'global', workspace: '' });
    assert.deepEqual(calls[7].body, { scope: 'workspace', workspace: 'ws1' });
    assert.ok(calls.every((call) => call.headers['X-ACECode-Token'] === 'tok'));
  } finally {
    globalThis.fetch = previousFetch;
  }
});

// ─── 接线 ──────────────────────────────────────────────────────────────────

// 触发场景:设置 > 个性化。
// 期望行为:SectionPersonalization 在自定义指令之后挂 MemorySettings,并把 App 传入的
//          当前工作区(与斜杠命令同源的 commandWorkspaceHash)交给它。
await run('个性化页挂载记忆设置并传入当前工作区', () => {
  const settings = source('components/SettingsPage.jsx');
  const personalization = between(settings, 'function SectionPersonalization(', '// ─── 技能');
  assert.ok(personalization.includes('<MemorySettings workspaceHash={workspaceHash} />'));
  assert.ok(personalization.indexOf('api.setCustomInstructions') < personalization.indexOf('<MemorySettings'));
  assert.ok(settings.includes('<SectionPersonalization workspaceHash={activeWorkspaceHash} />'));
  assert.ok(source('App.jsx').includes('activeWorkspaceHash={commandWorkspaceHash}'));
});

// 触发场景:删除条目与重置作用域的确认框。
// 期望行为:删除确认框把「删除」标为默认操作(打开即选中,Enter 确认,与其它删除确认一致);
//          重置确认框的「重置」也是默认操作,但在勾选确认前禁用 —— 第二道确认,
//          避免一次误点 / 误按 Enter 就清空整个作用域。
await run('删除与重置确认框的默认操作与二次确认', () => {
  const component = source('components/MemorySettings.jsx');
  const primary = 'data-ace-dialog-primary="true"';
  const deleteDialog = between(component, 'id="memory-delete-title"', '</Modal>');
  assert.ok(deleteDialog.indexOf(primary) !== -1 && deleteDialog.indexOf(primary) < deleteDialog.indexOf('void deleteEntry()'));
  // MemoryResetDialog 是文件里最后一个函数:从它的声明一直取到文件末尾。
  const resetDialog = component.slice(component.indexOf('function MemoryResetDialog('));
  assert.ok(resetDialog.startsWith('function MemoryResetDialog('), 'missing MemoryResetDialog');
  assert.ok(resetDialog.includes('type="checkbox"'));
  assert.ok(resetDialog.includes('disabled={!acknowledged || busy}'));
  assert.ok(resetDialog.indexOf(primary) < resetDialog.indexOf('void resetScope()'));
  assert.ok(resetDialog.includes('if (!acknowledged || busy) return;'));
});

// 触发场景:设置窗口 / Ctrl+K 搜「使用记忆」「记忆摘要」「摘要模型」或英文 memory。
// 期望行为:三条都指向个性化页;标签在组件里是独立的文本节点(locateSetting 按文本
//          精确匹配来滚动定位并画波浪线),所以组件源码里要逐字出现。
await run('设置搜索能找到记忆设置并定位到个性化页', () => {
  const entries = settingsSearchEntries();
  for (const label of ['使用记忆', '记忆摘要', '摘要模型']) {
    const hit = searchSettings(entries, label)[0];
    assert.equal(hit.section, 'personalization', label);
    assert.equal(hit.label, label);
  }
  const memoryHits = searchSettings(entries, 'memory').filter((item) => item.section === 'personalization');
  assert.deepEqual(memoryHits.map((item) => item.label).sort(), ['使用记忆', '摘要模型', '记忆摘要'].sort());
  const component = source('components/MemorySettings.jsx');
  for (const label of ['>使用记忆<', '>记忆摘要<', '>摘要模型<']) assert.ok(component.includes(label), label);
});

// 触发场景:Web 输入框的斜杠下拉与发送路由。
// 期望行为:/memory 是内置命令 —— 后端命令清单没返回时 fallback 也有它(带本地化描述),
//          发送时走内置命令端点(daemon 以 memory_status / memory_flush_done 系统通知回显),
//          两种通知都有中英文标题。
await run('/memory 内置命令与记忆系统通知标题', () => {
  const memory = commandsWithFallback(null).find((item) => item.name === 'memory');
  assert.equal(memory.kind, 'builtin');
  assert.equal(memory.description, '列出、查看、删除记忆;整理记忆摘要;本会话关闭/开启记忆');
  assert.deepEqual(parseExecutableBuiltinCommand('/memory forget build_tips'), {
    name: 'memory', args: 'forget build_tips', display_text: '/memory forget build_tips',
  });
  for (const locale of ['zh-CN', 'en-US']) {
    const titles = translationCatalogs[locale].systemNotice.titles;
    assert.ok(titles.memory_status, `${locale} memory_status`);
    assert.ok(titles.memory_flush_done, `${locale} memory_flush_done`);
    assert.ok(translationCatalogs[locale].commands.descriptions.memory, `${locale} /memory description`);
  }
  assert.equal(translationCatalogs['zh-CN'].systemNotice.titles.memory_status, '记忆信息');
  assert.equal(translationCatalogs['en-US'].systemNotice.titles.memory_flush_done, 'Memory consolidation finished');
});
