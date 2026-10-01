import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const srcRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

function source(relativePath) {
  return fs.readFileSync(path.join(srcRoot, relativePath), 'utf8');
}

function between(text, start, end) {
  const startIndex = text.indexOf(start);
  const endIndex = text.indexOf(end, startIndex);
  assert.notEqual(startIndex, -1, `missing start marker: ${start}`);
  assert.notEqual(endIndex, -1, `missing end marker: ${end}`);
  return text.slice(startIndex, endIndex);
}

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('Settings uses a blocking mask and an accessible expandable dialog', () => {
  const settings = source('components/SettingsPage.jsx');

  assert.match(settings, /data-ace-native-overlay="blocking"/);
  assert.match(settings, /data-settings-mask="true"/);
  assert.match(settings, /if \(event\.target === event\.currentTarget\) close\(\);/);
  assert.match(settings, /role="dialog"/);
  assert.match(settings, /aria-modal="true"/);
  assert.match(settings, /aria-labelledby="settings-window-title"/);
  assert.match(settings, /data-expanded=\{expanded \? 'true' : 'false'\}/);
  assert.match(settings, /name=\{expanded \? 'screenNormal' : 'screenFull'\}/);
  assert.match(settings, /id="settings-window-title" className="sr-only"/);
  assert.doesNotMatch(settings, /ace-settings-titlebar/);
  assert.match(settings, /<nav className="[^"]*shrink-0 select-none"/);
  assert.doesNotMatch(settings, /<WindowControls/);
});

// 触发场景:设置窗口打开后按 Esc。
// 期望行为:
//   1. 设置窗口在 document 冒泡阶段监听 keydown 并调用 close()(与 Modal.jsx 同一层级,
//      这样 window capture 层的 browserDefaults 守卫、AnchoredMenu 的 capture 拦截都先于它生效);
//   2. 上面还开着子对话框([data-ace-modal-dialog])时让位,由子对话框自己关;
//   3. 已被别的浮层 preventDefault 消费过的 Esc 不再处理;
//   4. 面板本身可聚焦(tabIndex=-1)且打开时把焦点收进来,关闭时还回去。
// 修复前的表现:设置窗口只能点遮罩或关闭按钮关闭,按 Esc 没有任何反应。
run('Settings window closes on Escape but yields to nested modals', () => {
  const settings = source('components/SettingsPage.jsx');
  const escapeEffect = between(settings, "if (event.key !== 'Escape'", 'const onMaskClick');

  assert.match(escapeEffect, /event\.defaultPrevented \|\| event\.isComposing\) return;/);
  assert.match(escapeEffect, /document\.querySelector\('\[data-ace-modal-dialog="true"\]'\)\) return;/);
  assert.match(escapeEffect, /close\(\);/);
  assert.match(escapeEffect, /document\.addEventListener\('keydown', onKeyDown\);/);
  assert.doesNotMatch(escapeEffect, /addEventListener\('keydown', onKeyDown, true\)/);
  assert.match(settings, /ref=\{windowRef\}[\s\S]*role="dialog"[\s\S]*tabIndex=\{-1\}/);
  assert.match(settings, /windowRef\.current\?\.focus\?\.\(\{ preventScroll: true \}\);/);
  assert.match(settings, /return \(\) => \{ previouslyFocused\?\.focus\?\.\(\); \};/);
});

// 触发场景:焦点在设置搜索框里按 Esc。
// 期望行为:搜索框有内容时只清空搜索并 stopPropagation(不关窗口);已经为空时不拦截,
//   事件冒泡到 document 由设置窗口关闭 —— 与 VS Code 设置页「先清空、再关闭」的手感一致。
// 修复前的表现:搜索框无条件 stopPropagation,焦点在搜索框时 Esc 永远关不掉设置窗口。
run('Settings search only swallows Escape while it has a query to clear', () => {
  const search = source('components/SettingsSearch.jsx');

  assert.match(search, /if \(event\.key === 'Escape' && query\) \{ event\.stopPropagation\(\); onQuery\(''\); \}/);
});

run('Settings panel keeps normal caps and an exact 13px expanded inset', () => {
  const styles = source('styles/globals.css');
  const panel = between(styles, '.ace-settings-panel {', '/* Desktop shell');
  const mask = between(styles, '.ace-settings-mask {', '.ace-settings-panel {');

  assert.match(mask, /background: rgba\(0, 0, 0, 0\.35\);/);
  assert.doesNotMatch(mask, /backdrop-filter/);
  assert.match(panel, /width: calc\(100vw - 240px\);/);
  assert.match(panel, /--ace-settings-inset-y: clamp\(24px, calc\(25vh - 150px\), 120px\);/);
  assert.match(panel, /height: calc\(100vh - 2 \* var\(--ace-settings-inset-y\)\);/);
  assert.match(panel, /min-width: min\(880px, calc\(100vw - 26px\)\);/);
  assert.match(panel, /min-height: min\(500px, calc\(100vh - 26px\)\);/);
  assert.match(panel, /max-width: 1440px;/);
  assert.match(panel, /max-height: 960px;/);
  assert.match(panel, /\.ace-settings-panel\[data-expanded="true"\] \{[\s\S]*width: calc\(100vw - 26px\);[\s\S]*height: calc\(100vh - 26px\);[\s\S]*max-width: none;[\s\S]*max-height: none;/);
});

run('upgrade URL and personalization editors save on blur without save buttons', () => {
  const settings = source('components/SettingsPage.jsx');
  const config = source('components/SettingsConfigSection.jsx');
  const personalization = between(settings, 'function SectionPersonalization(', '// ─── 技能');

  assert.match(config, /onBlur=\{\(\) => \{ void saveUpgradeUrl\(\); \}\}/);
  assert.match(config, /api\.setUpgradeConfig\(\{ base_url: baseUrl \}\)/);
  assert.doesNotMatch(config, /onClick=\{saveUpgradeUrl\}/);
  assert.match(personalization, /onBlur=\{\(\) => \{ void save\(\); \}\}/);
  assert.match(personalization, /api\.setCustomInstructions\(\{ text: candidate \}\)/);
  assert.doesNotMatch(personalization, /onClick=\{save\}/);
});

run('MCP editor saves on blur and flushes before runtime actions', () => {
  const settings = source('components/SettingsPage.jsx');
  const mcp = between(settings, 'function SectionMCP()', 'function SectionConnectors()');

  assert.match(mcp, /onBlur=\{\(\) => \{ void save\(\); \}\}/);
  assert.match(mcp, /await api\.putMcp\(parsed, workspace\);/);
  assert.equal((mcp.match(/if \(!await save\(\)\) return;/g) || []).length, 2);
  assert.doesNotMatch(mcp, /onClick=\{save\}/);
});

run('managed hooks render one canonical status badge', () => {
  const settings = source('components/SettingsPage.jsx');
  const hookItem = between(settings, 'function HookListItem(', 'function HookBadge(');

  assert.equal((hookItem.match(/<HookBadge hook=\{hook\} \/>/g) || []).length, 1);
  assert.doesNotMatch(hookItem, /hook\.managed &&/);
  assert.doesNotMatch(hookItem, />受管理</);
});
