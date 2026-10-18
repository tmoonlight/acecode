import { readWithFallback, writeSafely } from './usePreference.js';

const binding = (code, key, extra = {}) => Object.freeze({ code, key, ...extra });
export const APP_SHORTCUTS = Object.freeze({
  newSession: binding('KeyN', 'N', { alt: true }),
  focusInput: binding('KeyI', 'I', { alt: true }),
  dictation: binding('KeyD', 'D', { control: true }),
  toggleSidebar: binding('KeyB', 'B', { alt: true }),
  toggleRightPanel: binding('KeyE', 'E', { alt: true }),
  previewPresentation: binding('F11', 'F11', { control: true }),
  settings: binding('Comma', ','), shortcuts: binding('Slash', '/'),
  stop: binding('Period', '.', { shift: true }),
  forward: binding('Minus', '-'), back: binding('Minus', '-', { shift: true }),
  reasoningUp: binding('Period', '>', { alt: true, optionalShift: true }),
  reasoningDown: binding('Comma', '<', { alt: true, optionalShift: true }),
  search: binding('KeyK', 'K'), console: binding('Backquote', '`', { control: true }),
});
export const CONTEXT_SHORTCUTS = Object.freeze({
  find: binding('KeyF', 'F', { optionalShift: true }),
  send: binding('Enter', 'Enter', { unmodified: true }),
  newline: binding('Enter', 'Enter', { unmodified: true, shift: true }),
  historyPrevious: binding('ArrowUp', '↑', { unmodified: true }),
  historyNext: binding('ArrowDown', '↓', { unmodified: true }),
  save: binding('KeyS', 'S'),
  dismiss: binding('Escape', 'Esc', { unmodified: true }),
});
export const DEFAULT_SHORTCUTS = Object.freeze({ ...APP_SHORTCUTS, ...CONTEXT_SHORTCUTS });
export const SHORTCUT_STORAGE_KEY = 'acecode.shortcuts.v1';
export const APP_SHORTCUT_BROWSER_GUARD = Symbol('ace-shortcut-browser-guard');
const EMPTY = Object.freeze({});
const KEY_NAMES = Object.freeze({
  Comma: ',', Period: '.', Slash: '/', Backslash: '\\', Semicolon: ';', Quote: "'",
  BracketLeft: '[', BracketRight: ']', Minus: '-', Equal: '=', Backquote: '`',
  Enter: 'Enter', Escape: 'Esc', Space: 'Space', Backspace: 'Backspace', Delete: 'Delete',
  ArrowUp: '↑', ArrowDown: '↓', ArrowLeft: '←', ArrowRight: '→',
  Home: 'Home', End: 'End', PageUp: 'PageUp', PageDown: 'PageDown', Insert: 'Insert',
});
const keyName = (code) => KEY_NAMES[code] || (/^Key[A-Z]$/.test(code) ? code.slice(3)
  : /^Digit[0-9]$/.test(code) ? code.slice(5) : /^F([1-9]|1[0-2])$/.test(code) ? code : '');
export const isMacShortcutHost = (win = globalThis.window) => win?.__ACECODE_OS__
  ? win.__ACECODE_OS__ === 'macos' : /Mac|iPhone|iPad/i.test(win?.navigator?.platform || '');

export function isShortcutRecording(event) {
  return !!event?.target?.closest?.('[data-shortcut-recorder]');
}
export function isShortcutComposing(event) {
  return !event || event.isComposing || event.nativeEvent?.isComposing
    || event.keyCode === 229 || event.which === 229 || event.nativeEvent?.keyCode === 229
    || event.nativeEvent?.which === 229 || event.getModifierState?.('AltGraph');
}

// Stored overrides use explicit modifiers. Legacy defaults keep Ctrl/Cmd parity.
export function matchesShortcutBinding(event, spec) {
  if (!spec || isShortcutComposing(event) || isShortcutRecording(event)) return false;
  if (spec.modifiers) {
    if (Boolean(event.ctrlKey) !== spec.modifiers.ctrl || Boolean(event.metaKey) !== spec.modifiers.meta) return false;
  } else if (spec.unmodified) {
    if (event.ctrlKey || event.metaKey) return false;
  } else if (spec.control ? (!event.ctrlKey || event.metaKey)
    : ((!event.ctrlKey && !event.metaKey) || (event.ctrlKey && event.metaKey))) return false;
  if (Boolean(event.altKey) !== Boolean(spec.alt)) return false;
  if (!spec.optionalShift && Boolean(event.shiftKey) !== Boolean(spec.shift)) return false;
  // Captured chords are physical and exact. Defaults retain their existing
  // logical-key fallback (including alternate layouts and the numpad minus).
  if (event.code === spec.code) return true;
  if (spec.modifiers && event.code) return false;
  const key = String(event.key || '').toLowerCase();
  const aliases = { Period: ['.', '>'], Comma: [',', '<'], Minus: ['-', '_'], Escape: ['escape', 'esc'],
    ArrowUp: ['arrowup', '↑'], ArrowDown: ['arrowdown', '↓'] };
  return key === spec.key.toLowerCase() || aliases[spec.code]?.includes(key) || false;
}

export function bindingLabel(spec, mac = isMacShortcutHost()) {
  if (!spec) return '';
  const modifiers = spec.modifiers
    ? [spec.modifiers.ctrl && 'Ctrl', spec.modifiers.meta && 'Cmd']
    : [!spec.unmodified && (spec.control || !mac ? 'Ctrl' : 'Cmd')];
  return [...modifiers, spec.alt && (mac ? 'Option' : 'Alt'), spec.shift && 'Shift', spec.key].filter(Boolean).join('+');
}

export function captureShortcut(event) {
  if (isShortcutComposing(event) || event.repeat) return null;
  const code = event.code || (event.key === 'Escape' ? 'Escape' : event.key);
  const key = keyName(code);
  if (!key) return null;
  return { code, key, modifiers: { ctrl: !!event.ctrlKey, meta: !!event.metaKey },
    alt: !!event.altKey, shift: !!event.shiftKey };
}

export function shortcutEventVariants(spec) {
  const variants = [];
  for (const ctrlKey of [false, true]) for (const metaKey of [false, true]) {
    for (const altKey of [false, true]) for (const shiftKey of [false, true]) {
      const event = { key: spec.key, code: spec.code, ctrlKey, metaKey, altKey, shiftKey };
      if (matchesShortcutBinding(event, spec)) variants.push(event);
    }
  }
  return variants;
}

function validOverride(value) {
  return value && keyName(value.code) && value.key === keyName(value.code)
    && typeof value.modifiers?.ctrl === 'boolean' && typeof value.modifiers?.meta === 'boolean'
    && typeof value.alt === 'boolean' && typeof value.shift === 'boolean'
    && !(value.modifiers.ctrl && value.modifiers.meta);
}

function keyReservation(id, spec) {
  const primary = spec.modifiers?.ctrl || spec.modifiers?.meta;
  if (!primary && !spec.alt && !/^F\d+$/.test(spec.code)) {
    const allowed = { send: ['Enter'], newline: ['Enter'], historyPrevious: ['ArrowUp', 'ArrowDown'],
      historyNext: ['ArrowUp', 'ArrowDown'], dismiss: ['Escape'] };
    if (!allowed[id]?.includes(spec.code)) return 'modifier';
  }
  // These chords are browser/OS focus, editing or native-window commands.
  if (spec.code === 'Tab' || (spec.code === 'F11' && !primary && !spec.alt)
      || (spec.alt && spec.code === 'F4') || (spec.modifiers?.meta && !isMacShortcutHost())
      || (primary && !spec.alt && ['KeyC', 'KeyV', 'KeyX', 'KeyA', 'KeyZ', 'KeyY', 'KeyL', 'KeyT', 'KeyW', 'KeyN', 'KeyR', 'KeyO'].includes(spec.code))
      || (!primary && spec.alt && ['ArrowLeft', 'ArrowRight'].includes(spec.code))) return 'reserved';
  if (spec.code === 'F3' && id !== 'find') return 'reserved';
  return null;
}

export function shortcutBindingsConflict(a, b) {
  if (a.code !== b.code) return false;
  for (const ctrlKey of [false, true]) for (const metaKey of [false, true]) {
    for (const altKey of [false, true]) for (const shiftKey of [false, true]) {
      const event = { code: a.code, ctrlKey, metaKey, altKey, shiftKey };
      if (matchesShortcutBinding(event, a) && matchesShortcutBinding(event, b)) return true;
    }
  }
  return false;
}

export function validateShortcut(id, spec, overrides = EMPTY) {
  if (!Object.hasOwn(DEFAULT_SHORTCUTS, id)) return { reason: 'invalid' };
  if (spec !== DEFAULT_SHORTCUTS[id] && !validOverride(spec)) return { reason: 'invalid' };
  const reserved = spec.modifiers && keyReservation(id, spec);
  if (reserved) return { reason: reserved };
  for (const [other, original] of Object.entries(DEFAULT_SHORTCUTS)) {
    if (other !== id && shortcutBindingsConflict(spec, overrides[other] || original)) return { reason: 'conflict', conflict: other };
  }
  return null;
}

function normalizeOverrides(value) {
  if (!value || typeof value !== 'object' || Array.isArray(value)) return EMPTY;
  const result = {};
  for (const [id, spec] of Object.entries(value)) {
    if (!Object.hasOwn(DEFAULT_SHORTCUTS, id) || !validOverride(spec) || keyReservation(id, spec)) continue;
    result[id] = Object.freeze({ code: spec.code, key: spec.key,
      modifiers: Object.freeze({ ctrl: spec.modifiers.ctrl, meta: spec.modifiers.meta }), alt: spec.alt, shift: spec.shift });
  }
  if (Object.entries(result).some(([id, spec]) => validateShortcut(id, spec, result))) return EMPTY;
  return Object.freeze(result);
}

export function createShortcutStore(storage, events) {
  const read = () => normalizeOverrides(readWithFallback(SHORTCUT_STORAGE_KEY, EMPTY, null, storage));
  let current = read();
  const listeners = new Set();
  const notify = () => { for (const listener of listeners) listener(); };
  const onStorage = (event) => {
    if (event.key !== SHORTCUT_STORAGE_KEY && event.key !== null) return;
    current = read(); notify();
  };
  return {
    getSnapshot: () => current,
    subscribe(listener) {
      if (!listeners.size) { events?.addEventListener('storage', onStorage); current = read(); }
      listeners.add(listener);
      return () => { listeners.delete(listener); if (!listeners.size) events?.removeEventListener('storage', onStorage); };
    },
    save(id, value) {
      // Read again before write so other windows' changes are not overwritten.
      const latest = read();
      const issue = validateShortcut(id, value || DEFAULT_SHORTCUTS[id], latest);
      if (issue) return { ok: false, ...issue };
      const next = { ...latest };
      if (value) next[id] = value; else delete next[id];
      if (!writeSafely(SHORTCUT_STORAGE_KEY, next, storage)) return { ok: false, reason: 'storage' };
      current = normalizeOverrides(next); notify();
      return { ok: true };
    },
  };
}

let singleton;
export function shortcutStore() {
  if (!singleton) {
    let storage = null;
    try { storage = globalThis.localStorage; } catch { /* Saving reports unavailable storage. */ }
    singleton = createShortcutStore(storage, globalThis.window);
  }
  return singleton;
}
export const getShortcutOverrides = () => shortcutStore().getSnapshot();
export const effectiveShortcut = (id, overrides = getShortcutOverrides()) => overrides[id] || DEFAULT_SHORTCUTS[id];
export const isCustomShortcut = (id) => Object.hasOwn(getShortcutOverrides(), id);
export const isShortcut = (event, id) => matchesShortcutBinding(event, effectiveShortcut(id));
export const isDismissShortcut = (event) => isShortcut(event, 'dismiss');
export const shortcutEventConsumed = (event) => event.defaultPrevented
  && !(event.nativeEvent || event)[APP_SHORTCUT_BROWSER_GUARD];

export function composerShortcutAction(event, desktop = false, overrides = getShortcutOverrides()) {
  if (isShortcutComposing(event)) return null;
  const matches = (id) => matchesShortcutBinding(event, effectiveShortcut(id, overrides));
  const custom = (id) => Object.hasOwn(overrides, id);
  if (matches('send')) return 'send';
  if (matches('newline')) {
    // Preserve the editor's native Shift+Enter/soft-break behavior by default.
    if (!custom('newline') && event.key === 'Enter' && event.shiftKey
        && !event.ctrlKey && !event.metaKey && !event.altKey) return null;
    return 'newline';
  }
  if (event.key === 'Enter' && event.ctrlKey && desktop) return custom('newline') ? 'ignore' : 'newline';
  if (event.key === 'Enter' && !event.shiftKey && !custom('send')) return 'send';
  if (custom('newline') && matchesShortcutBinding(event, CONTEXT_SHORTCUTS.newline)) return 'ignore';
  return null;
}
