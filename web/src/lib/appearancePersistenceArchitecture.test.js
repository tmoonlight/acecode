import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const srcRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const repoRoot = path.resolve(srcRoot, '..', '..');

function sourceFromSrc(relativePath) {
  return fs.readFileSync(path.join(srcRoot, relativePath), 'utf8');
}

function sourceFromRepo(relativePath) {
  return fs.readFileSync(path.join(repoRoot, relativePath), 'utf8');
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

run('Desktop injects stable appearance before WebUI modules execute', () => {
  const desktop = sourceFromRepo('src/desktop/main.cpp');
  const injection = desktop.indexOf('window.__ACECODE_APPEARANCE__=');
  const navigation = desktop.indexOf('host.navigate(url);');
  assert.ok(injection >= 0);
  assert.ok(navigation > injection);
  for (const field of ['desktop_cfg.web_ui.theme', 'desktop_cfg.web_ui.color_theme', 'desktop_cfg.web_ui.font_size']) {
    assert.ok(desktop.includes(field), `bootstrap missing ${field}`);
  }
});

run('App restores daemon appearance and owns durable mutations', () => {
  const app = sourceFromSrc('App.jsx');
  assert.match(app, /createAppearancePersistenceController\(\{/);
  assert.match(app, /save: \(payload\) => api\.setUiPreferences\(payload\)/);
  assert.match(app, /api\.getUiPreferences\(\)\.then\(\(preferences\) => \{/);
  assert.match(app, /appearanceControllerRef\.current\.restore\(preferences\)/);
  assert.match(app, /onThemeToggle=\{toggleAppearanceTheme\}/);
  assert.match(app, /onThemeChange=\{\(nextTheme\) => changeAppearance\(\{ theme: nextTheme \}\)\}/);
  assert.match(app, /onColorThemeChange=\{\(nextColorTheme\) => \(/);
  assert.match(app, /onFontSizeChange=\{\(nextFontSize\) => changeAppearance\(\{ fontSize: nextFontSize \}\)\}/);
});

run('TopBar and Settings retain cache fallbacks but accept durable handlers', () => {
  const topBar = sourceFromSrc('components/TopBar.jsx');
  const settings = sourceFromSrc('components/SettingsPage.jsx');
  assert.match(topBar, /const toggleTheme = onThemeToggle \|\| toggle;/);
  assert.match(topBar, /onClick=\{toggleTheme\}/);
  assert.match(settings, /const setTheme = onThemeChange \|\| setThemeCache;/);
  assert.match(settings, /const setColorTheme = onColorThemeChange \|\| setColorThemeCache;/);
});

run('appearance writer queues complete snapshots and rolls back latest failure', () => {
  const helper = sourceFromSrc('lib/appearancePreferences.js');
  assert.match(helper, /queue = queue\s*\.then\(async \(\) => \{/);
  assert.match(helper, /appearancePreferencesToApi\(target, scope\)/);
  assert.match(helper, /if \(changeRevision !== revision\) return;/);
  assert.match(helper, /visible = confirmed;\s*applyIfActive\(confirmed\);/);
});
