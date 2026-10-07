// Shared by keyboard dispatch, settings help and terminal forwarding.
const binding = (code, key, extra = {}) => ({ code, key, ...extra });
export const APP_SHORTCUT_BROWSER_GUARD = Symbol('ace-shortcut-browser-guard');
export const APP_SHORTCUTS = Object.freeze({
  newSession: binding('KeyN', 'N', { alt: true }),
  focusInput: binding('KeyI', 'I', { alt: true }),
  toggleSidebar: binding('KeyB', 'B', { alt: true }),
  toggleRightPanel: binding('KeyE', 'E', { alt: true }),
  settings: binding('Comma', ','),
  shortcuts: binding('Slash', '/'),
  stop: binding('Period', '.', { shift: true }),
  forward: binding('Minus', '-'),
  back: binding('Minus', '-', { shift: true }),
  reasoningUp: binding('Period', '>', { alt: true, optionalShift: true }),
  reasoningDown: binding('Comma', '<', { alt: true, optionalShift: true }),
  search: binding('KeyK', 'K'),
  console: binding('Backquote', '`', { control: true }),
});

export function isMacShortcutHost(win = globalThis.window) {
  if (win?.__ACECODE_OS__) return win.__ACECODE_OS__ === 'macos';
  return /Mac|iPhone|iPad/i.test(win?.navigator?.platform || '');
}

export function matchAppShortcut(event) {
  if (!event || event.isComposing || event.nativeEvent?.isComposing
      || event.keyCode === 229 || event.which === 229 || event.getModifierState?.('AltGraph')) return null;
  for (const [id, spec] of Object.entries(APP_SHORTCUTS)) {
    if (spec.control ? (!event.ctrlKey || event.metaKey) : (!event.ctrlKey && !event.metaKey)) continue;
    if (!spec.control && event.ctrlKey && event.metaKey) continue;
    if (Boolean(event.altKey) !== Boolean(spec.alt)) continue;
    if (!spec.optionalShift && Boolean(event.shiftKey) !== Boolean(spec.shift)) continue;
    const key = String(event.key || '').toLowerCase();
    const punctuation = { Period: ['.', '>'], Comma: [',', '<'], Minus: ['-', '_'] };
    if (event.code === spec.code || key === spec.key.toLowerCase()
        || punctuation[spec.code]?.includes(key)) return id;
  }
  return null;
}

export function shortcutLabel(id, mac = isMacShortcutHost()) {
  const spec = APP_SHORTCUTS[id];
  if (!spec) return '';
  return [spec.control || !mac ? 'Ctrl' : 'Cmd', spec.alt && (mac ? 'Option' : 'Alt'),
    spec.shift && 'Shift', spec.key].filter(Boolean).join('+');
}

export function withAppShortcutHint(label, id) {
  return `${label} (${shortcutLabel(id)})`;
}

const visible = (element) => !element.hidden && element.getAttribute?.('aria-hidden') !== 'true'
  && (!element.getClientRects || element.getClientRects().length > 0);

export function appShortcutContextAllows(id, target, doc = globalThis.document) {
  const overlays = doc?.querySelectorAll?.(
    '[role="dialog"], [role="menu"], [data-ace-native-overlay="blocking"], [data-ace-native-overlay="overlap"][role="listbox"], [data-shortcut-menu="true"]',
  ) || [];
  for (const overlay of overlays) {
    if (!visible(overlay)) continue;
    if (['settings', 'shortcuts'].includes(id)
        && (overlay.matches?.('[data-settings-mask], [data-settings-window]'))) continue;
    if (id === 'search' && overlay.matches?.('[data-search-palette]')) continue;
    return false;
  }
  if (['reasoningUp', 'reasoningDown', 'stop'].includes(id)) {
    if (target?.closest?.('.xterm, [data-side-chat-window], [data-ace-editable-preview-text]')) return false;
    const editor = target?.closest?.('input, textarea, select, [contenteditable="true"]');
    if (editor && !editor.closest?.('[data-main-composer="true"]')) return false;
  }
  return true;
}

export function shortcutCatalog(mac = isMacShortcutHost()) {
  const mod = mac ? 'Cmd' : 'Ctrl';
  const rows = [
    ['newSession', '会话操作', '新建会话', '在当前工作区新建会话'],
    ['focusInput', '会话操作', '聚焦消息输入框', '回到当前输入框，保留草稿'],
    ['stop', '会话操作', '停止当前会话执行', '等同当前会话的停止按钮'],
    ['reasoningUp', '会话操作', '增加思维深度', '逐档增加，到最高档停止'],
    ['reasoningDown', '会话操作', '降低思维深度', '逐档降低，到最低档停止'],
    ['forward', '界面导航', '前进', '前往导航历史中的下一项'],
    ['back', '界面导航', '后退', '返回导航历史中的上一项'],
    ['search', '界面导航', '全局搜索', '搜索会话、工作区和设置'],
    ['toggleSidebar', '界面导航', '展开／收起左侧项目栏', '切换项目和会话列表的显示'],
    ['toggleRightPanel', '界面导航', '展开／收起右侧面板', '切换文件及预览面板的显示'],
    ['console', '界面导航', '展开／收起控制台', '控制台可用时生效'],
    ['settings', '界面导航', '打开设置', '打开设置窗口'],
    ['shortcuts', '界面导航', '打开快捷键页签', '进入快捷键并聚焦搜索栏'],
    ['find', '输入与编辑', '当前会话内查找', '在打开的会话中查找文本', `${mod}+F`],
    ['send', '输入与编辑', '发送消息', '在消息输入框中生效', 'Enter'],
    ['newline', '输入与编辑', '输入框换行', '在消息输入框中生效', 'Shift+Enter'],
    ['history', '输入与编辑', '浏览输入历史', '输入框为空时向上浏览，历史浏览中可向下返回', '↑ / ↓'],
    ['save', '输入与编辑', '保存正在编辑的文件', '文件编辑器聚焦时生效', `${mod}+S`],
    ['dismiss', '输入与编辑', '关闭当前浮层／弹窗', '优先关闭最上层可关闭的浮层', 'Esc'],
  ];
  return rows.map(([id, group, title, description, keys]) => ({ id, group, title, description,
    keys: keys || shortcutLabel(id, mac) }));
}

export function filterShortcuts(rows, query) {
  const normalize = (value) => String(value || '').normalize('NFKC').toLowerCase()
    .replace(/command|⌘/g, 'cmd').replace(/control|⌃/g, 'ctrl').replace(/⌥/g, 'option')
    .replace(/\s*\+\s*/g, '+').trim();
  const words = normalize(query).split(/\s+/).filter(Boolean);
  return rows.filter((row) => {
    const text = normalize([row.title, row.description, row.group, row.keys, row.id].join(' '));
    return words.every((word) => text.includes(word));
  });
}
