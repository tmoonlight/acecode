import assert from 'node:assert/strict';
import {
  LOOP_TEMPLATES,
  buildLoopPayload,
  defaultLoopForm,
  formFromLoop,
  loopRunPresentation,
  loopScheduleLabel,
  loopFormForTemplate,
  validateLoopForm,
} from './loops.js';

assert.equal(LOOP_TEMPLATES.length, 4);
assert.deepEqual(LOOP_TEMPLATES.map((item) => item.name), [
  '每日代码健康巡检', '定时测试与问题修复', '每周代码变更总结', '依赖与技术债巡检',
]);
const defaults = defaultLoopForm('model-a', Date.UTC(2026, 6, 13));
assert.equal(defaults.permissionMode, 'yolo');
assert.equal(defaults.useWorktree, false);
assert.equal(defaults.scheduleKind, 'period');
assert.equal(validateLoopForm(defaults), '请输入定时任务名称');

const form = { ...defaults, name: 'Review', prompt: 'Review code', workspaceHash: 'h1' };
assert.equal(validateLoopForm(form), '');
const payload = buildLoopPayload(form, [{ hash: 'h1', cwd: 'C:/repo' }]);
assert.equal(payload.workspace_cwd, 'C:/repo');
assert.equal(payload.use_worktree, false);
assert.equal(payload.permission_mode, 'yolo');
assert.equal(payload.schedule.kind, 'period');
assert.equal(Object.hasOwn(payload, 'schedule_expr'), false);

const worktreePayload = buildLoopPayload(
  { ...form, useWorktree: true },
  [{ hash: 'h1', cwd: 'C:/repo' }],
);
assert.equal(worktreePayload.use_worktree, true);
assert.equal(buildLoopPayload({ ...form, workspaceHash: '', useWorktree: true }, []).use_worktree, false);

const edited = formFromLoop({
  name: 'Existing',
  prompt: 'Review',
  workspace_hash: 'h1',
  model_name: 'model-a',
  use_worktree: true,
  schedule: { kind: 'period', period: 'daily', hour: 9, minute: 0 },
});
assert.equal(edited.useWorktree, true);

assert.equal(loopScheduleLabel({ kind: 'period', period: 'workdays', hour: 9, minute: 5 }), '工作日 09:05');
assert.match(loopScheduleLabel({ kind: 'interval', interval_value: 2, interval_unit: 'hours' }), /每 2 小时/);
assert.match(loopRunPresentation({ status: 'missed', reason: 'workspace_busy' }).reason, /不会补跑/);
assert.deepEqual(loopRunPresentation({ status: 'running' }), {
  label: '执行中', reason: '', tone: 'active', workspaceTouched: [], workspaceTouchedSummary: '',
});
assert.equal(loopRunPresentation({ status: 'waiting_user' }).label, '等待用户');
// 写边界事后检测:run 记录了 worktree 之外的改动时,给出可显示的警告摘要;
// 字段缺失 / 非数组 / 含非字符串项时不炸,过滤后为空。
const touched = loopRunPresentation({
  status: 'completed',
  workspace_touched: ['electron/src/im/chat.js', '', 7, 'docs/notes.md'],
});
assert.deepEqual(touched.workspaceTouched, ['electron/src/im/chat.js', 'docs/notes.md']);
assert.match(touched.workspaceTouchedSummary, /2 处 worktree 之外的改动/);
assert.equal(touched.tone, 'ok');
assert.equal(loopRunPresentation({ status: 'completed', workspace_touched: 'nope' }).workspaceTouchedSummary, '');
const weekly = loopFormForTemplate(LOOP_TEMPLATES[2], 'model-a', Date.UTC(2026, 6, 13));
assert.equal(weekly.period, 'weekly');
assert.deepEqual(weekly.weekdays, [5]);
