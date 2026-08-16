import assert from 'node:assert/strict';
import { createPendingActionGuard } from './pendingActionGuard.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('pending action guard 同步拒绝同一键的重复 acquire', () => {
  const guard = createPendingActionGuard();

  assert.equal(guard.acquire('workspace-a'), true);
  assert.equal(guard.acquire('workspace-a'), false);
  assert.equal(guard.isPending('workspace-a'), true);
  assert.equal(guard.size, 1);
});

run('pending action guard 允许独立键并行', () => {
  const guard = createPendingActionGuard();

  assert.equal(guard.acquire('workspace-a'), true);
  assert.equal(guard.acquire('workspace-b'), true);
  assert.equal(guard.isPending('workspace-a'), true);
  assert.equal(guard.isPending('workspace-b'), true);
  assert.equal(guard.size, 2);
});

run('pending action guard release 后允许同一键重试', () => {
  const guard = createPendingActionGuard();

  assert.equal(guard.acquire('fork'), true);
  assert.equal(guard.release('fork'), true);
  assert.equal(guard.isPending('fork'), false);
  assert.equal(guard.acquire('fork'), true);
  assert.equal(guard.size, 1);
});

run('pending action guard 拒绝空键且 release 未持有键安全返回', () => {
  const guard = createPendingActionGuard();

  assert.equal(guard.acquire(''), false);
  assert.equal(guard.acquire(null), false);
  assert.equal(guard.release('missing'), false);
  assert.equal(guard.size, 0);
});
