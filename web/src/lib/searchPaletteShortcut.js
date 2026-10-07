// 全局搜索面板(SearchPalette)的默认快捷键:Ctrl+K(macOS 上 ⌘K)。
//
// 这是唯一的定义点 —— App.jsx 的全局监听、TopBar / 快捷菜单里的提示文案、
// 控制台(xterm)的放行名单都从这里取,改键位只改这一个文件。

import { APP_SHORTCUTS, matchAppShortcut } from './appShortcuts.js';

export const SEARCH_PALETTE_SHORTCUT = Object.freeze({ key: APP_SHORTCUTS.search.key.toLowerCase(), ctrl: true, meta: true });

// 判定 keydown 是否为搜索面板快捷键。
// - Ctrl 与 ⌘ 任一即可(matchShortcut 的 ctrl/meta 语义),Alt / Shift 额外按下不算。
// - 兼容 e.code === 'KeyK':非拉丁键盘布局(如俄语)或部分输入法下 e.key 不是 'k',
//   但物理键位不变 —— 与 Ctrl+` 用 e.code 兜底是同一个理由。
export function isSearchPaletteShortcut(event) {
  return matchAppShortcut(event) === 'search';
}

function isMacHost(win) {
  if (!win) return false;
  if (win.__ACECODE_OS__ === 'macos') return true;
  if (win.__ACECODE_OS__ === 'windows' || win.__ACECODE_OS__ === 'linux') return false;
  const platform = win.navigator?.platform || '';
  const ua = win.navigator?.userAgent || '';
  return /mac/i.test(platform) || /Macintosh/i.test(ua);
}

// 面向用户的按键文案:macOS 显示 ⌘K,其它平台 Ctrl+K。
export function searchPaletteShortcutLabel(win = typeof window !== 'undefined' ? window : undefined) {
  return isMacHost(win) ? '⌘K' : 'Ctrl+K';
}

// 给按钮 title / 菜单项拼「文案 (快捷键)」,与「打开控制台 (Ctrl+`)」同款格式。
export function withSearchPaletteShortcutHint(label, win) {
  const text = String(label || '').trim();
  const shortcut = searchPaletteShortcutLabel(win);
  return text ? `${text} (${shortcut})` : shortcut;
}
