import assert from 'node:assert/strict';
import fs from 'node:fs';
import { TOPBAR_WINDOW_DRAG_HEIGHT, topBarWindowDragAction, isTopBarDragExcludedTarget } from './topBarWindowDrag.js';

const bounds = { left: 0, right: 1280, top: 0, width: 1280, height: 30 };
const event = { button: 0, clientX: 600, clientY: 15, detail: 1 };
assert.equal(TOPBAR_WINDOW_DRAG_HEIGHT, 44);
for (const clientY of [0, 15, 29.9, 30, 43.9]) {
  assert.equal(topBarWindowDragAction({ ...event, clientY }, bounds), 'drag');
}
for (const clientY of [-1, 44, 60, NaN]) {
  assert.equal(topBarWindowDragAction({ ...event, clientY }, bounds), null);
}
for (const clientX of [-1, 1280, NaN]) {
  assert.equal(topBarWindowDragAction({ ...event, clientX }, bounds), null);
}
assert.equal(topBarWindowDragAction({ ...event, clientY: 40, detail: 2 }, bounds), 'maximize');
assert.equal(topBarWindowDragAction({ ...event, button: 2 }, bounds), null);
assert.equal(topBarWindowDragAction({ ...event, defaultPrevented: true }, bounds), null);
assert.equal(topBarWindowDragAction(event, bounds, true), null);
assert.equal(topBarWindowDragAction(event, null), null);
assert.equal(topBarWindowDragAction(event, { ...bounds, height: 0 }), null);
assert.equal(topBarWindowDragAction({ ...event, clientY: 49 }, { ...bounds, top: 10 }), 'drag');
assert.equal(isTopBarDragExcludedTarget(null), false);
for (const selector of ['[data-ace-native-overlay]', '[role="tab"]', '[role="separator"]', '[draggable="true"]', '.ace-resize-handle']) {
  assert.equal(isTopBarDragExcludedTarget({ closest: (list) => list.split(',').includes(selector) }), true);
}
console.log('[pass] compact title-bar drag keeps the original band and excludes consumed interactions');

const topbar = fs.readFileSync(new URL('../components/TopBar.jsx', import.meta.url), 'utf8');
const icon = fs.readFileSync(new URL('../components/Icon.jsx', import.meta.url), 'utf8');
assert.match(topbar, /isInteractiveTarget\(event.target\) \|\| isTopBarDragExcludedTarget\(event.target\)/);
assert.match(topbar, /document.addEventListener\('mousedown', onWindowDragMouseDown\)/);
assert.match(topbar, /document.removeEventListener\('mousedown', onWindowDragMouseDown\)/);
assert.doesNotMatch(topbar, /onMouseDown=\{onTopBarMouseDown\}/);
assert.match(topbar, /panelToggle \? 'ace-topbar-panel-toggle' : 'ace-topbar-toggle-btn'/);
assert.match(topbar, /side="left" size=\{16\} expanded=\{!sidebarCollapsed\}/);
assert.match(topbar, /side="right" size=\{15\} expanded=\{!rightPanelCollapsed\}/);
assert.match(icon, /name=\{expanded \? `\$\{name\}Filled` : name\}/);
for (const side of ['Left', 'Right']) {
  const svg = fs.readFileSync(new URL(`../../public/vs-icons/Panel${side}Filled.svg`, import.meta.url), 'utf8');
  assert.match(svg, /viewBox="0 0 48 48"/);
  assert.match(svg, /width="36" height="36"/);
  assert.match(svg, /<path[^>]+fill="#333"/);
}
console.log('[pass] panel toggles use matching filled assets without the blue pressed class');
