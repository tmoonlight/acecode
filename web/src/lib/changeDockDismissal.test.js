import assert from 'node:assert/strict';
import {
  dismissChangeDockSignature,
  dismissedDockSignatureFor,
  dockDismissalKey,
  validateDockDismissals,
} from './changeDockDismissal.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('dockDismissalKey 按 workspace 和 session 隔离', () => {
  assert.equal(dockDismissalKey({ workspaceHash: 'w1' }, 's1'), 'w1:s1');
  assert.equal(dockDismissalKey({ workspaceHash: '' }, 's1'), 's1');
  assert.equal(dockDismissalKey({ sessionId: 's2' }), 's2');
  assert.equal(dockDismissalKey({}, ''), '');
});

run('dock dismissal 只隐藏同一 session 的同一签名', () => {
  const first = dockDismissalKey({ workspaceHash: 'w' }, 's1');
  const second = dockDismissalKey({ workspaceHash: 'w' }, 's2');
  const state = dismissChangeDockSignature({}, first, 'sig-a');
  assert.equal(dismissedDockSignatureFor(state, first), 'sig-a');
  assert.equal(dismissedDockSignatureFor(state, second), '');

  const replaced = dismissChangeDockSignature(state, first, 'sig-b');
  assert.equal(dismissedDockSignatureFor(replaced, first), 'sig-b');
});

run('dock dismissal state 可 JSON 持久化并恢复', () => {
  const key = dockDismissalKey({ workspaceHash: 'w' }, 's1');
  const state = dismissChangeDockSignature({}, key, 'sig-a');
  const restored = JSON.parse(JSON.stringify(state));
  assert.equal(validateDockDismissals(restored), true);
  assert.equal(dismissedDockSignatureFor(restored, key), 'sig-a');
});

run('validateDockDismissals 拒绝无效结构', () => {
  assert.equal(validateDockDismissals({ a: 'sig' }), true);
  assert.equal(validateDockDismissals(null), false);
  assert.equal(validateDockDismissals([]), false);
  assert.equal(validateDockDismissals({ a: '' }), false);
  assert.equal(validateDockDismissals({ a: 1 }), false);
});