import assert from 'node:assert/strict';
import {
  desktopUpdateRestartAvailable,
  formatUpdatePublishedDate,
  nondecreasingUpdateProgress,
  normalizeUpdateReleases,
  requestDesktopUpdateRestart,
  updateDialogMode,
  updateJobCanCancel,
  updateDialogAllowsRollback,
  updateJobIsActive,
  updateJobIsRollback,
  updateJobPhaseLabel,
  updateJobProgress,
  updateRestartMessage,
} from './updateJob.js';

async function test(name, fn) {
  try {
    await fn();
    console.log(`[pass] ${name}`);
  } catch (err) {
    console.error(`[fail] ${name}`);
    throw err;
  }
}

await test('active update jobs include pending and running only', () => {
  assert.equal(updateJobIsActive({ state: 'pending' }), true);
  assert.equal(updateJobIsActive({ state: 'running' }), true);
  assert.equal(updateJobIsActive({ state: 'failed' }), false);
  assert.equal(updateJobIsActive({ state: 'cancelled' }), false);
});

await test('only safe active update phases remain cancellable', () => {
  assert.equal(updateJobCanCancel({ state: 'running', phase: 'downloading', can_cancel: true }), true);
  assert.equal(updateJobCanCancel({ state: 'running', phase: 'downloading', cancel_requested: true }), false);
  assert.equal(updateJobCanCancel({ state: 'running', phase: 'installing', can_cancel: false }), false);
  assert.equal(updateJobCanCancel({ state: 'cancelled', can_cancel: false }), false);
});

await test('download progress occupies the visible middle of the full lifecycle', () => {
  assert.equal(updateJobProgress({ phase: 'checking', state: 'running' }), 5);
  assert.equal(updateJobProgress({ phase: 'downloading', state: 'running', percent: 50 }), 40);
  assert.equal(updateJobProgress({ phase: 'verifying', state: 'running' }), 78);
  assert.equal(updateJobProgress({ phase: 'installing', state: 'running' }), 95);
  assert.equal(updateJobProgress({ phase: 'complete', state: 'succeeded' }), 100);
});

await test('rendered update progress never regresses within the same job', () => {
  assert.equal(nondecreasingUpdateProgress(40, 55), 55);
  assert.equal(nondecreasingUpdateProgress(55, 42), 55);
  assert.equal(nondecreasingUpdateProgress(95, 101), 100);
  assert.equal(nondecreasingUpdateProgress(-8, '12'), 12);
  assert.equal(nondecreasingUpdateProgress(12, 'not-a-number'), 12);
});

await test('phase labels and terminal dialog modes are stable', () => {
  assert.equal(updateJobPhaseLabel({ phase: 'extracting', state: 'running' }), '正在解压安装包');
  assert.equal(updateJobPhaseLabel({ phase: 'verifying', state: 'failed' }), '升级失败');
  assert.equal(updateJobPhaseLabel({ phase: 'downloading', state: 'running', cancel_requested: true }), '正在取消升级');
  assert.equal(updateJobPhaseLabel({ phase: 'cancelled', state: 'cancelled' }), '升级已取消');
  assert.equal(updateDialogMode(null), 'confirm');
  assert.equal(updateDialogMode(null, { status: 'available' }), 'confirm');
  assert.equal(updateDialogMode(null, { status: 'up_to_date' }), 'up_to_date');
  assert.equal(updateDialogMode({ state: 'running' }), 'running');
  assert.equal(updateDialogMode({ state: 'succeeded' }), 'success');
  assert.equal(updateDialogMode({ state: 'failed' }), 'failure');
  assert.equal(updateDialogMode({ state: 'cancelled' }), 'cancelled');
});

await test('Store-managed updates suppress stale ZIP jobs and never become an install confirmation', () => {
  const status = { status: 'store_managed', current_version: '0.9.30', update_available: false };
  for (const job of [
    null,
    { state: 'pending' },
    { state: 'running', phase: 'installing' },
    { state: 'succeeded', restart_required: true },
    { state: 'failed', target_version: '0.9.31' },
  ]) {
    assert.equal(updateDialogMode(job, status), 'store_managed');
  }
});

await test('release history normalization keeps ordered non-empty plain-text notes', () => {
  assert.deepEqual(normalizeUpdateReleases(null), []);
  assert.deepEqual(normalizeUpdateReleases([
    {
      version: ' 0.8.0 ',
      published_at: ' 2026-07-20T08:00:00Z ',
      notes: ' First line\nSecond line ',
      packages: [{ sha256: 'must-not-leak' }],
    },
    { version: '0.7.9', notes: '   ' },
    { version: '', notes: 'missing version' },
    null,
    { version: '0.7.8', notes: 'Earlier tip', rollback_available: true },
  ]), [
    {
      version: '0.8.0',
      published_at: '2026-07-20T08:00:00Z',
      notes: 'First line\nSecond line',
      rollback_available: false,
    },
    {
      version: '0.7.8',
      published_at: '',
      notes: 'Earlier tip',
      rollback_available: true,
    },
  ]);
});

await test('release publish dates use stable ISO dates and preserve unknown text', () => {
  assert.equal(formatUpdatePublishedDate('2026-07-20T08:00:00Z'), '2026-07-20');
  assert.equal(formatUpdatePublishedDate(' 2026-07-19 '), '2026-07-19');
  assert.equal(formatUpdatePublishedDate('legacy date'), 'legacy date');
  assert.equal(formatUpdatePublishedDate(null), '');
});

await test('successful job explains that the running window is still old', () => {
  const message = updateRestartMessage({ state: 'succeeded', restart_required: true });
  assert.match(message, /完全退出并重新启动/);
  assert.match(message, /当前窗口仍在运行旧版本/);
  assert.equal(updateRestartMessage({ state: 'failed' }), '');
});

await test('desktop restart capability requires the native lifecycle bridge', () => {
  assert.equal(desktopUpdateRestartAvailable({}), false);
  assert.equal(desktopUpdateRestartAvailable({ aceDesktop_restartApp: () => {} }), true);
});

await test('desktop restart helper accepts structured native success', async () => {
  const calls = [];
  const result = await requestDesktopUpdateRestart({
    aceDesktop_restartApp: async () => {
      calls.push('restart');
      return JSON.stringify({ ok: true });
    },
  });
  assert.deepEqual(calls, ['restart']);
  assert.deepEqual(result, { ok: true });
});

// 场景:版本更新记录里点了「回滚到此版本」,daemon 返回带 rollback: true 的任务。
// 期望:标题 / 阶段文字说「回滚」而不是「升级」;重启提示不再说「旧版本」;
//      只有没有任务在跑、也没有等待重启的安装时才给出回滚按钮。
await test('rollback jobs use rollback copy and only idle dialogs offer rollback', () => {
  const rollback = { rollback: true };
  assert.equal(updateJobIsRollback(rollback), true);
  assert.equal(updateJobIsRollback({ rollback: 'true' }), false);
  assert.equal(updateJobPhaseLabel({ ...rollback, state: 'succeeded' }), '回滚完成');
  assert.equal(updateJobPhaseLabel({ ...rollback, state: 'failed' }), '回滚失败');
  assert.equal(updateJobPhaseLabel({ ...rollback, state: 'cancelled' }), '回滚已取消');
  assert.equal(
    updateJobPhaseLabel({ ...rollback, state: 'running', cancel_requested: true }),
    '正在取消回滚',
  );
  assert.equal(
    updateJobPhaseLabel({ ...rollback, state: 'running', phase: 'downloading' }),
    '正在下载安装包',
  );
  assert.equal(updateJobPhaseLabel({ state: 'succeeded' }), '升级安装完成');
  assert.equal(
    updateRestartMessage({ ...rollback, state: 'succeeded', restart_required: true }),
    '回滚已安装。请完全退出并重新启动 ACECode。',
  );
  for (const mode of ['confirm', 'up_to_date', 'failure', 'cancelled']) {
    assert.equal(updateDialogAllowsRollback(mode), true, mode);
  }
  for (const mode of ['running', 'success', 'store_managed']) {
    assert.equal(updateDialogAllowsRollback(mode), false, mode);
  }
  assert.equal(updateDialogAllowsRollback('confirm', { starting: true }), false);
  assert.equal(updateDialogAllowsRollback('failure', { restarting: true }), false);
});

await test('desktop restart helper rejects unavailable and failed native restarts', async () => {
  await assert.rejects(
    () => requestDesktopUpdateRestart({}),
    /不支持自动重启/,
  );
  await assert.rejects(
    () => requestDesktopUpdateRestart({
      aceDesktop_restartApp: async () => ({ ok: false, error: 'replacement missing' }),
    }),
    /replacement missing/,
  );
});
