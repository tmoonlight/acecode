import { readCppSource } from './cppSourcePaths.testHelper.js';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const srcRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

function sourceFromSrc(relativePath) {
  return fs.readFileSync(path.join(srcRoot, relativePath), 'utf8');
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
  const desktop = readCppSource('desktop/main.cpp');
  const injection = desktop.indexOf('window.__ACECODE_APPEARANCE__=');
  const navigation = desktop.indexOf('host.navigate(url);');
  assert.ok(injection >= 0);
  assert.ok(navigation > injection);
  for (const field of ['desktop_cfg.web_ui.theme', 'desktop_cfg.web_ui.color_theme', 'desktop_cfg.web_ui.font_size', 'desktop_cfg.web_ui.message_auto_collapse']) {
    assert.ok(desktop.includes(field), `bootstrap missing ${field}`);
  }
});

run('App restores daemon appearance and owns durable mutations', () => {
  const app = sourceFromSrc('App.jsx');
  assert.match(app, /createAppearancePersistenceController\(\{/);
  assert.match(app, /const applyAppearanceRef = useRef\(applyAppearance\);/);
  assert.match(app, /applyAppearanceRef\.current = applyAppearance;/);
  assert.match(app, /apply: \(next\) => applyAppearanceRef\.current\(next\),/);
  assert.match(app, /save: \(payload\) => api\.setUiPreferences\(payload\)/);
  assert.match(app, /api\.getUiPreferences\(\)\.then\(\(preferences\) => \{/);
  assert.match(app, /appearanceControllerRef\.current\.restore\(preferences\)/);
  assert.doesNotMatch(app, /applyStartupTheme|claimStartupTheme/);
  assert.match(app, /onThemeChange=\{\(nextTheme\) => changeAppearance\(\{ theme: nextTheme \}\)\}/);
  assert.match(app, /onColorThemeChange=\{\(nextColorTheme\) => \(/);
  assert.match(app, /onFontSizeChange=\{\(nextFontSize\) => changeAppearance\(\{ fontSize: nextFontSize \}\)\}/);
});

run('Settings retains durable theme controls after the top-bar shortcut is removed', () => {
  const topBar = sourceFromSrc('components/TopBar.jsx');
  const settings = sourceFromSrc('components/SettingsPage.jsx');
  assert.doesNotMatch(topBar, /useTheme|onThemeToggle|toggleTheme/);
  assert.match(settings, /const setTheme = onThemeChange \|\| setThemeCache;/);
  assert.match(settings, /const setColorTheme = onColorThemeChange \|\| setColorThemeCache;/);
});

run('appearance writer queues complete snapshots and rolls back latest failure', () => {
  const helper = sourceFromSrc('lib/appearancePreferences.js');
  assert.match(helper, /queue = queue\s*\.then\(async \(\) => \{/);
  assert.match(helper, /appearancePreferencesToApi\(withoutDeletedTheme\(target\), scope\)/);
  assert.match(helper, /if \(changeRevision !== revision\) return;/);
  assert.match(helper, /visible = confirmed;\s*applyIfActive\(confirmed\);/);
});
