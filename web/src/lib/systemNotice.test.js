import assert from 'node:assert/strict';
import { createInstance } from 'i18next';
import { translationCatalogs } from '../i18n/catalogs.js';
import { presentSystemNotice, mergeLegacyGoalNotices } from './systemNotice.js';
import { projectCollapsedTranscriptItems } from './transcriptProjection.js';

const instance = createInstance();
await instance.init({ resources: Object.fromEntries(Object.entries(translationCatalogs).map(([locale, translation]) => [locale, { translation }])), lng: 'zh-CN', fallbackLng: false, initImmediate: false, interpolation: { escapeValue: false } });
const zh = instance.getFixedT('zh-CN');
const en = instance.getFixedT('en-US');
const notice = (code, params = {}, content = 'Original fallback') => ({ role: 'system', content, metadata: { system_notice: { version: 1, code, params } } });
const goal = { objective: '打开 windows\n检查 C:\\work', status: 'active', tokens_used: 0, token_budget: 1000, remaining_tokens: 1000, time_used_seconds: 0, goal_id: 'goal-1', thread_id: 'session-1' };
const summary = (objective = '打开 windows') => `Goal:\n  objective: ${objective}\n  status: active\n  tokens: 0 / 1000 (1000 remaining)\n  elapsed: 0s`;
const msg = (id, content, metadata) => ({ kind: 'msg', id, role: 'system', content, metadata });
function run(name, fn) { fn(); console.log(`[pass] ${name}`); }

run('structured goal titles and complete details use the current locale without translating user content', () => {
  const message = notice('goal_started', { goal });
  const cn = presentSystemNotice(message, zh);
  const english = presentSystemNotice(message, en);
  assert.equal(cn.title, '目标开始');
  assert.equal(english.title, 'Goal started');
  assert.match(cn.text, /状态: 运行中/);
  assert.match(english.text, /Status: Active/);
  for (const text of [cn.text, english.text]) {
    assert.ok(text.includes(goal.objective));
    assert.ok(text.includes('1000'));
    assert.ok(text.includes('goal-1'));
    assert.ok(text.includes('session-1'));
    assert.doesNotMatch(text, /\{\{|Original fallback/);
  }
  const extra = presentSystemNotice(notice('goal_overview', { goal: { ...goal, future_field: { value: 'keep' } } }), zh);
  assert.match(extra.text, /future_field:[\s\S]*keep/);
  const wireGoal = { created_at_ms: 1760000000123, goal_id: 'goal-1', ...goal };
  const ordered = presentSystemNotice(notice('goal_overview', { goal: wireGoal }), zh);
  assert.ok(ordered.text.startsWith(`目标: ${goal.objective}`));
  assert.ok(ordered.text.includes(new Date(wireGoal.created_at_ms).toISOString()));
});

run('historical goal and remote control messages receive meaningful localized titles', () => {
  assert.equal(presentSystemNotice({ content: '[Goal] Started: 打开windows' }, zh).title, '目标开始');
  assert.equal(presentSystemNotice({ content: summary() }, zh).title, '目标概览');
  assert.match(presentSystemNotice({ content: summary() }, zh).text, /已用 tokens: 0/);
  for (const metadata of [undefined, notice('remote_control_not_running').metadata]) {
    const result = presentSystemNotice({ content: 'Remote control is not running.', metadata }, zh);
    assert.equal(result.title, '远程控制未运行');
    assert.equal(result.text, '远程控制未运行。');
  }
  const rawWarning = 'timeout\n'.repeat(300);
  const stopped = presentSystemNotice(notice('remote_control_stopped', { warning: rawWarning }), zh);
  assert.equal(stopped.title, '远程控制已停止');
  assert.ok(stopped.text.endsWith(rawWarning));
});

run('remote control status fields and connection text are localized with values preserved', () => {
  const content = "Channel 'demo' connected. Existing runtime reused.\nThis session is now bound to the channel (replaced session old).\nRemote control : ON\nDefault channel: demo\nActive channel : demo\nBound session  : next\nInbound        : POST http://127.0.0.1:9000/rc/send\nOutbound       : (not configured)\nStats          : in 1 ok / 2 rejected | out 3 sent / 4 failed / 5 dropped";
  const result = presentSystemNotice(notice('remote_control_connected', {}, content), zh);
  assert.match(result.text, /通道“demo”已连接/);
  assert.match(result.text, /替换会话 old/);
  assert.match(result.text, /默认通道: demo/);
  assert.match(result.text, /出站地址: 未配置/);
  assert.match(result.text, /1 成功 \/ 2 拒绝/);
  assert.ok(result.text.includes('http://127.0.0.1:9000/rc/send'));
});

run('same-operation historical goal duplicates merge and unrelated notices remain independent', () => {
  const pair = [msg(1, summary()), msg(2, '[Goal] Started: 打开 windows', { goal_audit: true, goal_action: 'create' })];
  const merged = mergeLegacyGoalNotices(pair);
  assert.equal(merged.length, 1);
  assert.equal(presentSystemNotice(merged[0], zh).title, '目标开始');
  assert.match(presentSystemNotice(merged[0], zh).text, /剩余 tokens: 1000/);
  assert.ok(merged[0].content.includes(pair[0].content));
  assert.ok(merged[0].content.includes(pair[1].content));
  assert.deepEqual(merged[0].coveredItemIds, [1, 2]);
  assert.equal(mergeLegacyGoalNotices([pair[0], msg(3, '[Goal] Started: different')]).length, 2);
  assert.equal(mergeLegacyGoalNotices([pair[0], msg(3, 'other notice'), pair[1]]).length, 3);
  for (const messageAutoCollapse of [false, true]) {
    const inputs = [...pair, msg(3, 'Remote control is not running.'), { kind: 'msg', id: 4, role: 'assistant', content: 'done' }];
    const projected = projectCollapsedTranscriptItems(inputs, { messageAutoCollapse });
    const systems = projected.filter((item) => item.role === 'system');
    assert.equal(systems.length, 2);
    assert.deepEqual(systems.map((item) => presentSystemNotice(item, zh).title), ['目标开始', '远程控制未运行']);
  }
});

run('compaction groups only its operation and retains full summary and diagnostics regardless of global setting', () => {
  const compact = (id, operation, stage, code, params, complete = false) => ({
    ...msg(id, `raw-${id}`), metadata: { ...notice(code, params).metadata, compact_notice: true, compact_notice_id: operation, compact_notice_stage: stage, compact_notice_complete: complete },
  });
  const summaryText = 'Summary\n'.repeat(500);
  const rawError = 'raw diagnostic\n'.repeat(300);
  const entries = [
    compact(1, 'op1', 'progress', 'context_compacting', {}),
    msg(2, 'Remote control is not running.'),
    compact(3, 'op1', 'summary', 'context_compacted', { summary: summaryText }, true),
    compact(4, 'op2', 'error', 'context_compact_failed', { error: rawError }),
  ];
  for (const messageAutoCollapse of [false, true]) {
    const result = projectCollapsedTranscriptItems(entries, { messageAutoCollapse });
    assert.equal(result.length, 3);
    assert.deepEqual(result.map((entry) => presentSystemNotice(entry, zh).title), ['上下文已压缩', '远程控制未运行', '上下文压缩失败']);
    assert.ok(presentSystemNotice(result[0], zh).text.includes(summaryText));
    assert.ok(presentSystemNotice(result[2], zh).text.endsWith(rawError));
  }
});

run('unknown formats, versions and structured content retain all text', () => {
  const text = 'Plugin diagnostics\n' + 'full details\n'.repeat(500);
  for (const metadata of [null, { system_notice: { version: 9, code: 'goal_started' } }, { system_notice: { version: 1, code: 'future_code', params: { error: 'future' } } }]) {
    const result = presentSystemNotice({ content: text, metadata }, zh);
    assert.equal(result.title, 'Plugin diagnostics');
    assert.equal(result.text, text);
  }
  const content = { diagnostic: 'a\nb', code: 5 };
  assert.deepEqual(JSON.parse(presentSystemNotice({ content }, zh).text), content);
  for (const system_notice of [{ version: 9, code: 'goal_resumed' }, { version: 1, code: 'future_code' }]) {
    assert.equal(presentSystemNotice({ content: 'Goal resumed.', metadata: { system_notice } }, zh).text, 'Goal resumed.');
  }
});

run('all notice catalogs have matching translated keys', () => {
  function keys(value, prefix = '') { return Object.entries(value).flatMap(([key, child]) => typeof child === 'string' ? [`${prefix}${key}`] : keys(child, `${prefix}${key}.`)); }
  const cnKeys = keys(translationCatalogs['zh-CN'].systemNotice);
  assert.deepEqual(cnKeys, keys(translationCatalogs['en-US'].systemNotice));
  for (const key of cnKeys) {
    assert.notEqual(zh(`systemNotice.${key}`), `systemNotice.${key}`);
    assert.notEqual(en(`systemNotice.${key}`), `systemNotice.${key}`);
  }
});

// 场景：模型把工具调用写成正文文本（fix-feedback-0924 第 3 条），AgentLoop 注入纠正提示重试时
// 发出 response_text_tool_call_retry 通知。期望：中英文标题与详情都来自目录，带上重试次数，
// 不出现未替换的占位符。回归表现：新通知代码缺目录项时界面只能显示原始的回退文本。
run('text-form tool call retry notice is localized with attempt counters', () => {
  const message = notice('response_text_tool_call_retry', { attempt: 1, attempts: 2, error: 'tool "Bash" is not available' });
  const cn = presentSystemNotice(message, zh);
  const english = presentSystemNotice(message, en);
  assert.equal(cn.title, '文本工具调用重试中');
  assert.equal(english.title, 'Retrying text-form tool call');
  assert.match(cn.text, /原生工具调用重发 1\/2/);
  assert.match(english.text, /\(1\/2\)/);
  for (const text of [cn.text, english.text]) assert.doesNotMatch(text, /\{\{|Original fallback/);
});

// 场景：PA 兜底 / 摘要失败后的机械兜底改为「先清工具输出、再精简旧回合、最后才整组丢弃」，
// 新增 context_history_thinned 与带 thinned 参数的 context_compact_warning。
// 期望：两种语言都带出精简组数、清除条数与丢弃组数；旧通知（没有 thinned）仍走原文案。
run('context repair notices report condensed turns and keep the legacy wording', () => {
  const thinned = notice('context_history_thinned', { round: 1, groups: 0, thinned: 2, outputs: 5 });
  const cn = presentSystemNotice(thinned, zh);
  const english = presentSystemNotice(thinned, en);
  assert.equal(cn.title, '旧回合已精简');
  assert.equal(english.title, 'Old turns condensed');
  assert.match(cn.text, /清除 5 条旧工具输出、精简 2 组旧回合/);
  assert.match(english.text, /condensed 2 old turns/);
  const fallback = presentSystemNotice(notice('context_compact_warning', { groups: 0, thinned: 3, outputs: 4, error: 'summary failed' }), zh);
  assert.match(fallback.text, /精简 3 组旧回合/);
  assert.ok(fallback.text.includes('summary failed'));
  const legacy = presentSystemNotice(notice('context_compact_warning', { groups: 1, outputs: 2, error: 'summary failed' }), zh);
  assert.match(legacy.text, /丢弃最旧的 1 组历史、清除 2 条旧工具输出/);
  for (const text of [cn.text, english.text, fallback.text, legacy.text]) assert.doesNotMatch(text, /\{\{|Original fallback/);
});
