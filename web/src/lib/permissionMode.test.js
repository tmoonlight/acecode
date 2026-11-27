import assert from 'node:assert/strict';
import { PERMISSION_MODES, normalizePermissionMode, permissionModeOption } from './permissionMode.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('permission mode normalizes daemon canonical names', () => {
  assert.equal(normalizePermissionMode('default'), 'default');
  assert.equal(normalizePermissionMode('auto'), 'auto');
  assert.equal(normalizePermissionMode('accept-edits'), 'auto');
  assert.equal(normalizePermissionMode('yolo'), 'yolo');
  // 自动模式（试验）的线上名。不在列表里的名字会被规范化成 default,界面会把它
  // 显示成「默认权限」,所以新模式必须在列表里。
  assert.equal(normalizePermissionMode('auto-review'), 'auto-review');
  assert.equal(normalizePermissionMode('auto_review'), 'default');
});

run('permission mode exposes the four desktop permission options', () => {
  assert.deepEqual(
    PERMISSION_MODES.map(({ id, label }) => ({ id, label })),
    [
      { id: 'default', label: '默认权限' },
      { id: 'auto', label: '自动模式' },
      { id: 'auto-review', label: '自动模式（试验）' },
      { id: 'yolo', label: '完全访问权限' },
    ],
  );
});

run('permission mode accepts legacy camelCase UI value', () => {
  assert.equal(normalizePermissionMode('acceptEdits'), 'auto');
});

run('permission mode falls back to default for invalid values', () => {
  assert.equal(normalizePermissionMode(''), 'default');
  assert.equal(normalizePermissionMode('ask'), 'default');
  assert.equal(normalizePermissionMode('plan'), 'default');
});

run('permission mode option returns display metadata', () => {
  const plan = permissionModeOption('plan');
  assert.equal(plan.label, '默认权限');
  assert.equal(plan.color, 'ok');

  const review = permissionModeOption('auto-review');
  assert.equal(review.label, '自动模式（试验）');
  assert.equal(review.color, 'warn');

  const option = permissionModeOption('yolo');
  assert.equal(option.label, '完全访问权限');
  assert.equal(option.color, 'danger');
});
