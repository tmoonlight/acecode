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
  assert.match(settings, /border-b border-border shrink-0 select-none/);
  assert.match(settings, /<nav className="[^"]*shrink-0 select-none"/);
  assert.doesNotMatch(settings, /<WindowControls/);
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
  const config = between(settings, 'function SectionConfig()', '// ─── 个性化');
  const personalization = between(settings, 'function SectionPersonalization()', '// ─── 技能');

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
  assert.match(mcp, /await api\.putMcp\(parsed\);/);
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
