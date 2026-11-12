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
  for (const field of ['desktop_cfg.web_ui.theme', 'desktop_cfg.web_ui.color_theme', 'desktop_cfg.web_ui.font_size', 'desktop_cfg.web_ui.message_auto_collapse', 'desktop_cfg.web_ui.subagent_avatar_style']) {
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

// 场景:设置 > 外观 > 子代理头像。
// 期望:这个选择只走 daemon 配置这一条路(和主题、字号一样由外观持久化控制器发出、由它回写),
// App 把当前值放进 Provider,头像组件从那里读;不落浏览器本地存储。
// 回归防护:桌面版每次启动端口都变,本地存储留不住,只存本地的话重启就回到默认的花瓣。
run('sub-agent avatar style is a daemon-backed appearance preference', () => {
  const app = sourceFromSrc('App.jsx');
  const helper = sourceFromSrc('lib/appearancePreferences.js');
  const avatar = sourceFromSrc('components/PixelAvatar.jsx');
  assert.match(app, /const \[subagentAvatarStyle, setSubagentAvatarStyle\] = useState\(initialAppearance\.subagentAvatarStyle\);/);
  assert.match(app, /setSubagentAvatarStyle\(next\.subagentAvatarStyle\);/);
  assert.match(app, /<SubagentAvatarStyleProvider value=\{subagentAvatarStyle\}>/);
  assert.match(app, /onSubagentAvatarStyleChange=\{\(next\) => changeAppearance\(\{ subagentAvatarStyle: next \}\)\}/);
  assert.doesNotMatch(app, /usePreference\([^)]*[aA]vatarStyle/);
  assert.match(helper, /subagent_avatar_style: normalized\.subagentAvatarStyle,/);
  assert.match(avatar, /const geometric = useSubagentAvatarStyle\(\) === 'geometric' && !expert\?\.avatar_identity;/);
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
