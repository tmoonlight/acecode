import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import {
  SETTINGS_NAV_GROUPS,
  SETTINGS_NAV_ITEMS,
  settingsNavIndexForKey,
} from './settingsNavigation.js';

function test(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

test('settings navigation uses the confirmed Codex-style groups', () => {
  assert.deepEqual(
    SETTINGS_NAV_GROUPS.map((group) => ({
      key: group.key,
      label: group.label,
      items: group.items.map((item) => item.label),
    })),
    [
      {
        key: 'personal',
        label: '个人',
        items: ['常规', '外观', '配置', '个性化', '虚拟办公室', '使用情况'],
      },
      {
        key: 'integrations',
        label: '集成',
        items: ['技能', 'MCP 服务器', '插件', '消息通道'],
      },
      {
        key: 'coding',
        label: '编码',
        items: ['模型', '工具', '钩子', '安全中心'],
      },
      {
        key: 'archived',
        label: '已归档',
        items: ['已归档会话'],
      },
      {
        key: 'support',
        label: '支持',
        items: ['问题反馈', '关于'],
      },
    ],
  );
});

test('flattened settings routes remain unique and complete', () => {
  const keys = SETTINGS_NAV_ITEMS.map((item) => item.key);
  assert.deepEqual(keys, [
    'general',
    'appearance',
    'config',
    'personalization',
    'virtual-office',
    'usage',
    'skills',
    'mcp',
    'connectors',
    'channels',
    'models',
    'tools',
    'hooks',
    'security',
    'archived',
    'feedback',
    'about',
  ]);
  assert.equal(new Set(keys).size, keys.length);
});

test('settings deep links use grouped indexes and fall back to general', () => {
  SETTINGS_NAV_ITEMS.forEach((item, index) => {
    assert.equal(settingsNavIndexForKey(item.key), index);
  });
  assert.equal(settingsNavIndexForKey('missing-section'), 0);
  assert.equal(settingsNavIndexForKey(''), 0);
});

test('SettingsPage renders accessible groups inside the scrollable navigation', () => {
  const source = readFileSync(
    new URL('../components/SettingsPage.jsx', import.meta.url),
    'utf8',
  );
  assert.match(source, /navGroups\.map/);
  assert.match(source, /role="group"/);
  assert.match(source, /aria-labelledby=\{headingId\}/);
  assert.match(source, /aria-current=\{active \? 'page' : undefined\}/);
  assert.match(source, /<div className="ace-settings-nav-list overflow-y-auto">/);
  assert.match(source, /text-\[11px\] font-normal text-fg-mute/);
});

// 场景:设置项很多、导航需要滚动(图中「已归档」一组在最下面)。期望:搜索框固定在导航
// 顶部不跟着滚,只有下面的分组导航 / 搜索结果在 ace-settings-nav-list 里滚动。回归:原来
// 整个 <nav> 是滚动容器,搜索框和导航一起滚出视野,找设置还得先滚回顶部。
test('SettingsPage keeps the search box fixed above the scrolling navigation list', () => {
  const settings = readFileSync(new URL('../components/SettingsPage.jsx', import.meta.url), 'utf8');
  const styles = readFileSync(new URL('../styles/globals.css', import.meta.url), 'utf8');
  const nav = settings.slice(settings.indexOf('<nav className="ace-settings-nav'), settings.indexOf('</nav>'));
  assert.doesNotMatch(nav.slice(0, nav.indexOf('>')), /overflow/);
  const searchAt = nav.indexOf('<SettingsSearch ');
  const listAt = nav.indexOf('<div className="ace-settings-nav-list overflow-y-auto">');
  assert.ok(searchAt > 0 && listAt > searchAt, 'search input renders before (outside) the scrolling list');
  assert.ok(nav.indexOf('<SettingsSearchResults') > listAt, 'search results scroll inside the list');
  assert.ok(nav.indexOf('navGroups.map') > listAt, 'nav groups scroll inside the list');
  assert.match(styles, /\.ace-settings-nav \{[^}]*display: flex;[^}]*flex-direction: column;/);
  assert.match(styles, /\.ace-settings-nav > \.ace-settings-search \{[^}]*flex: none;/);
  assert.match(styles, /\.ace-settings-nav-list \{[^}]*flex: 1 1 auto;[^}]*min-height: 0;/);
});

test('SettingsPage keeps search and labels available with compact content padding', () => {
  const source = readFileSync(
    new URL('../components/SettingsPage.jsx', import.meta.url),
    'utf8',
  );
  assert.match(source, /<nav className="ace-settings-nav/);
  assert.match(source, /aria-label=\{item\.label\}/);
  assert.match(source, /<SettingsSearch/);
  assert.match(source, /<span className="truncate">\{item.label\}/);
  assert.match(source, /px-4 py-3 sm:px-6 sm:py-5/);
});
