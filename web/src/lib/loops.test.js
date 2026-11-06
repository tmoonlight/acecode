import assert from 'node:assert/strict';
import {
  LOOP_TEMPLATES,
  buildLoopPayload,
  defaultLoopForm,
  formFromLoop,
  loopRunPresentation,
  loopScheduleLabel,
  loopSessionCleanupLabel,
  loopFormForTemplate,
  setLoopSessionCleanup,
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

// 旧通知清理:新建表单默认两个开关都关、保留 3 个;载荷原样带上三个字段。
assert.equal(defaults.autoArchiveOldSessions, false);
assert.equal(defaults.autoDeleteOldSessions, false);
assert.equal(defaults.keepRecentSessions, 3);
assert.equal(payload.auto_archive_old_sessions, false);
assert.equal(payload.auto_delete_old_sessions, false);
assert.equal(payload.keep_recent_sessions, 3);

// 两个开关互斥:勾上一个就取消另一个;取消勾选不动另一个。
let cleanup = setLoopSessionCleanup(form, 'autoArchiveOldSessions', true);
assert.equal(cleanup.autoArchiveOldSessions, true);
cleanup = setLoopSessionCleanup(cleanup, 'autoDeleteOldSessions', true);
assert.equal(cleanup.autoDeleteOldSessions, true);
assert.equal(cleanup.autoArchiveOldSessions, false);
cleanup = setLoopSessionCleanup(cleanup, 'autoDeleteOldSessions', false);
assert.equal(cleanup.autoDeleteOldSessions, false);
assert.equal(cleanup.autoArchiveOldSessions, false);

// 输入框里的数字是字符串;载荷转成整数。表单里两个都为 true(旧数据或手改)时只发删除,
// 不让服务端因互斥直接 400。
const deleting = buildLoopPayload({ ...form, autoArchiveOldSessions: true, autoDeleteOldSessions: true, keepRecentSessions: '5' }, []);
assert.equal(deleting.auto_archive_old_sessions, false);
assert.equal(deleting.auto_delete_old_sessions, true);
assert.equal(deleting.keep_recent_sessions, 5);

// 保留个数越界或清空:表单校验拦下,不发请求。
assert.equal(validateLoopForm({ ...form, keepRecentSessions: '' }), '保留会话数需在 1 到 100 之间');
assert.equal(validateLoopForm({ ...form, keepRecentSessions: 101 }), '保留会话数需在 1 到 100 之间');
assert.equal(validateLoopForm({ ...form, keepRecentSessions: '2.5' }), '保留会话数需在 1 到 100 之间');
assert.equal(validateLoopForm({ ...form, keepRecentSessions: '10' }), '');

// 编辑已有任务:回填服务端字段;缺字段的老任务按默认值回填。
const archivedLoop = formFromLoop({
  name: 'Existing', prompt: 'Review', model_name: 'model-a',
  auto_archive_old_sessions: true, keep_recent_sessions: 7,
  schedule: { kind: 'period', period: 'daily', hour: 9, minute: 0 },
});
assert.equal(archivedLoop.autoArchiveOldSessions, true);
assert.equal(archivedLoop.autoDeleteOldSessions, false);
assert.equal(archivedLoop.keepRecentSessions, 7);
assert.equal(edited.autoArchiveOldSessions, false);
assert.equal(edited.keepRecentSessions, 3);

// 任务卡片摘要:开着才显示,带保留个数;都关时为空串(join 前被 filter 掉)。
assert.equal(loopSessionCleanupLabel({ auto_archive_old_sessions: true, keep_recent_sessions: 7 }), '自动归档旧通知（保留 7 个）');
assert.equal(loopSessionCleanupLabel({ auto_delete_old_sessions: true }), '自动删除旧通知（保留 3 个）');
assert.equal(loopSessionCleanupLabel({}), '');
