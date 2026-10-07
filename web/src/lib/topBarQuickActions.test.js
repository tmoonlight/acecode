import assert from 'node:assert/strict';
import {
  QUICK_ACTION_SHORTCUT_SEARCH_PALETTE,
  TOPBAR_QUICK_ACTIONS,
  invokeTopBarQuickAction,
  topBarQuickActionNeedsSeparator,
  topBarQuickActionShortcutLabel,
  topBarQuickActionsMenuWidth,
} from './topBarQuickActions.js';

function test(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

test('top-bar quick actions keep the requested labels and order', () => {
  assert.deepEqual(
    TOPBAR_QUICK_ACTIONS.map(({ id, label, group }) => ({ id, label, group })),
    [
      { id: 'new-session', label: '新对话', group: 'navigation' },
      { id: 'new-loop', label: '定时任务', group: 'navigation' },
      { id: 'find-content', label: '查找内容', group: 'navigation' },
      { id: 'settings', label: '设置', group: 'application' },
      { id: 'virtual-office', label: '显示虚拟办公室', group: 'application' },
      { id: 'appearance', label: '外观', group: 'application' },
      { id: 'about', label: '关于 ACECode', group: 'application' },
      { id: 'check-updates', label: '检查更新', group: 'application' },
      { id: 'exit', label: '退出 ACECode', group: 'exit' },
    ],
  );
});

test('top-bar quick actions separate navigation, application, and exit groups', () => {
  assert.deepEqual(
    TOPBAR_QUICK_ACTIONS.map((_, index) => topBarQuickActionNeedsSeparator(index)),
    [false, false, false, true, false, false, false, false, true],
  );
});

test('top-bar quick actions invoke only their matching callbacks', () => {
  const calls = [];
  const callbacks = {
    onNewSession: () => calls.push('new-session'),
    onOpenLoop: () => calls.push('new-loop'),
    onOpenSearch: () => calls.push('find-content'),
    onSettings: () => calls.push('settings'),
    onToggleOffice: () => calls.push('virtual-office'),
    onAppearance: () => calls.push('appearance'),
    onAbout: () => calls.push('about'),
    onCheckUpdates: () => calls.push('check-updates'),
    onExit: () => calls.push('exit'),
  };

  for (const action of TOPBAR_QUICK_ACTIONS) {
    assert.equal(invokeTopBarQuickAction(action.id, callbacks), true);
  }
  assert.deepEqual(calls, [
    'new-session',
    'new-loop',
    'find-content',
    'settings',
    'virtual-office',
    'appearance',
    'about',
    'check-updates',
    'exit',
  ]);
});

test('top-bar quick actions ignore unknown actions and absent callbacks', () => {
  assert.equal(invokeTopBarQuickAction('missing', {}), false);
  assert.equal(invokeTopBarQuickAction('settings', {}), false);
});

test('top-bar quick-actions menu follows the project-sidebar width', () => {
  assert.equal(topBarQuickActionsMenuWidth(212), 212);
  assert.equal(topBarQuickActionsMenuWidth(212.6), 213);
});

test('top-bar quick-actions menu uses the default sidebar width for invalid input', () => {
  assert.equal(topBarQuickActionsMenuWidth(undefined), 270);
  assert.equal(topBarQuickActionsMenuWidth(Number.NaN), 270);
  assert.equal(topBarQuickActionsMenuWidth(0), 270);
});

test('only the find-content quick action carries the search-palette shortcut', () => {
  // 触发场景:侧栏快捷菜单渲染各项右侧的按键提示。
  // 搜索保留 Ctrl+K，设置显示自己的已注册快捷键。
  const findContent = TOPBAR_QUICK_ACTIONS.find((action) => action.id === 'find-content');
  assert.equal(findContent.shortcut, QUICK_ACTION_SHORTCUT_SEARCH_PALETTE);
  assert.equal(topBarQuickActionShortcutLabel(findContent, { __ACECODE_OS__: 'windows' }), 'Ctrl+K');
  assert.equal(topBarQuickActionShortcutLabel(findContent, { __ACECODE_OS__: 'macos' }), '⌘K');
  for (const action of TOPBAR_QUICK_ACTIONS) {
    if (action.id === 'find-content') continue;
    if (action.id === 'settings') {
      assert.equal(topBarQuickActionShortcutLabel(action, { __ACECODE_OS__: 'windows' }), 'Ctrl+,');
      assert.equal(topBarQuickActionShortcutLabel(action, { __ACECODE_OS__: 'macos' }), 'Cmd+,');
      continue;
    }
    assert.equal(action.shortcut, undefined);
    assert.equal(topBarQuickActionShortcutLabel(action, { __ACECODE_OS__: 'windows' }), '');
  }
  assert.equal(topBarQuickActionShortcutLabel(null), '');
});
