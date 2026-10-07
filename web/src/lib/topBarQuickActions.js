import { DEFAULT_SINGLE_LAYOUT } from './singleLayout.js';
import { searchPaletteShortcutLabel } from './searchPaletteShortcut.js';

// 快捷操作里带默认快捷键的项:值是逻辑名而不是按键文案,按键文案随平台变
// (macOS 显示 ⌘K),渲染时经 topBarQuickActionShortcutLabel 解析。
export const QUICK_ACTION_SHORTCUT_SEARCH_PALETTE = 'search-palette';

export const TOPBAR_QUICK_ACTIONS = Object.freeze([
  Object.freeze({
    id: 'new-session',
    label: '新对话',
    icon: 'newSession',
    iconSize: 16,
    callback: 'onNewSession',
    group: 'navigation',
  }),
  Object.freeze({
    id: 'new-loop',
    label: '定时任务',
    icon: 'alarm',
    iconSize: 16,
    callback: 'onOpenLoop',
    group: 'navigation',
  }),
  Object.freeze({
    id: 'find-content',
    label: '查找内容',
    icon: 'search',
    iconSize: 14,
    callback: 'onOpenSearch',
    group: 'navigation',
    shortcut: QUICK_ACTION_SHORTCUT_SEARCH_PALETTE,
  }),
  Object.freeze({
    id: 'settings',
    label: '设置',
    icon: 'settings',
    iconSize: 16,
    callback: 'onSettings',
    group: 'application',
  }),
  Object.freeze({
    id: 'virtual-office',
    label: '显示虚拟办公室',
    icon: 'office',
    iconSize: 16,
    callback: 'onToggleOffice',
    group: 'application',
  }),
  Object.freeze({
    id: 'appearance',
    label: '外观',
    icon: 'palette',
    iconSize: 16,
    callback: 'onAppearance',
    group: 'application',
  }),
  Object.freeze({
    id: 'about',
    label: '关于 ACECode',
    icon: 'info',
    iconSize: 16,
    callback: 'onAbout',
    group: 'application',
  }),
  Object.freeze({
    id: 'check-updates',
    label: '检查更新',
    icon: 'refresh',
    iconSize: 16,
    callback: 'onCheckUpdates',
    group: 'application',
  }),
  Object.freeze({
    id: 'exit',
    label: '退出 ACECode',
    icon: 'close',
    iconSize: 16,
    callback: 'onExit',
    group: 'exit',
  }),
]);

export function topBarQuickActionNeedsSeparator(index, actions = TOPBAR_QUICK_ACTIONS) {
  return Number.isInteger(index)
    && index > 0
    && index < actions.length
    && actions[index - 1]?.group !== actions[index]?.group;
}

export function topBarQuickActionsMenuWidth(sidebarWidth) {
  return typeof sidebarWidth === 'number' && Number.isFinite(sidebarWidth) && sidebarWidth > 0
    ? Math.round(sidebarWidth)
    : DEFAULT_SINGLE_LAYOUT.sidebar;
}

// 菜单项右侧的按键提示;没有快捷键的项返回空串,调用方据此不渲染。
export function topBarQuickActionShortcutLabel(action, win) {
  if (action?.shortcut === QUICK_ACTION_SHORTCUT_SEARCH_PALETTE) {
    return searchPaletteShortcutLabel(win);
  }
  return '';
}

export function invokeTopBarQuickAction(actionId, callbacks = {}) {
  const action = TOPBAR_QUICK_ACTIONS.find((candidate) => candidate.id === actionId);
  if (!action) return false;
  const handler = callbacks[action.callback];
  if (typeof handler !== 'function') return false;
  handler();
  return true;
}
