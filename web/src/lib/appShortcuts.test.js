import assert from 'node:assert/strict';
import { APP_SHORTCUTS, appShortcutContextAllows, filterShortcuts, matchAppShortcut, shortcutCatalog, shortcutLabel } from './appShortcuts.js';
import { nextReasoningEffort } from './modelReasoning.js';

const key = (code, extra = {}) => ({ code, ctrlKey: true, ...extra });
assert.equal(matchAppShortcut(key('Minus')), 'forward');
assert.equal(matchAppShortcut(key('Minus', { key: '_', shiftKey: true })), 'back');
assert.equal(matchAppShortcut(key('Minus', { altKey: true })), null);
assert.equal(matchAppShortcut(key('Period', { altKey: true })), 'reasoningUp');
assert.equal(matchAppShortcut(key('Period', { altKey: true, shiftKey: true, key: '>' })), 'reasoningUp');
assert.equal(matchAppShortcut(key('Comma', { altKey: true, key: '<' })), 'reasoningDown');
assert.equal(matchAppShortcut({ ctrlKey: true, altKey: true, key: '>' }), 'reasoningUp');
assert.equal(matchAppShortcut(key('Period', { shiftKey: true })), 'stop');
assert.equal(matchAppShortcut(key('Period')), null);
assert.equal(matchAppShortcut(key('KeyK', { altKey: true })), null);
assert.equal(matchAppShortcut(key('KeyK', { shiftKey: true })), null);
for (const id of Object.keys(APP_SHORTCUTS)) {
  const spec = APP_SHORTCUTS[id];
  const event = key(spec.code, { key: spec.key, altKey: !!spec.alt, shiftKey: !!spec.shift });
  assert.equal(matchAppShortcut(event), id);
  assert.equal(matchAppShortcut({ ...event, isComposing: true }), null);
  assert.equal(matchAppShortcut({ ...event, keyCode: 229 }), null);
  assert.equal(matchAppShortcut({ ...event, getModifierState: () => true }), null);
  assert.equal(matchAppShortcut({ ...event, ctrlKey: false, metaKey: true }), spec.control ? null : id);
}
assert.equal(shortcutLabel('back', false), 'Ctrl+Shift+-');
assert.equal(shortcutLabel('reasoningUp', true), 'Cmd+Option+>');
assert.equal(shortcutLabel('console', true), 'Ctrl+`');
assert.deepEqual(filterShortcuts(shortcutCatalog(false), 'ctrl + shift + -').map((x) => x.id), ['back']);
assert.deepEqual(filterShortcuts(shortcutCatalog(false), '思维 深度').map((x) => x.id), ['reasoningUp', 'reasoningDown']);
assert.equal(filterShortcuts(shortcutCatalog(false), '不存在的功能').length, 0);
const overlay = (selector, show = true) => ({ matches: (s) => s.includes(selector), getClientRects: () => show ? [{}] : [] });
const doc = (...overlays) => ({ querySelectorAll: () => overlays });
assert.equal(appShortcutContextAllows('newSession', null, doc(overlay('dialog'))), false);
assert.equal(appShortcutContextAllows('newSession', null, doc(overlay('dialog', false))), true);
assert.equal(appShortcutContextAllows('shortcuts', null, doc(overlay('[data-settings-mask]'))), true);
assert.equal(appShortcutContextAllows('shortcuts', null, doc(overlay('[data-settings-mask]'), overlay('dialog'))), false);
assert.equal(appShortcutContextAllows('search', null, doc(overlay('[data-search-palette]'))), true);
const model = { provider: 'openai', reasoning: { supported: true, default_enabled: true,
  supported_efforts: ['max', 'low', 'high'], default_effort: 'high' } };
assert.equal(nextReasoningEffort(model, 1), 'max');
assert.equal(nextReasoningEffort(model, -1), 'low');
assert.equal(nextReasoningEffort(model, 1, 'max'), null);
assert.equal(nextReasoningEffort(model, -1, 'low'), null);
assert.equal(nextReasoningEffort(model, 1, 'low'), 'high');
assert.equal(nextReasoningEffort({ ...model, reasoning_effort: 'max' }, -1), 'high');
assert.equal(nextReasoningEffort(null, 1), null);
assert.equal(nextReasoningEffort({ ...model, reasoning: { ...model.reasoning, enabled: false } }, 1), null);
console.log('[pass] app shortcuts: exact bindings, platform labels, search, scopes and reasoning bounds');
