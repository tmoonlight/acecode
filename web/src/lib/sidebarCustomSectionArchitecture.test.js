import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const srcRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

function source(relativePath) {
  return fs.readFileSync(path.join(srcRoot, relativePath), 'utf8');
}

function test(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

test('sidebar restores one default-collapsed extension section with settled counts', () => {
  const sidebar = source('components/Sidebar.jsx');
  assert.match(sidebar, /acecode\.sidebarCustomSectionExpanded\.v2/);
  assert.match(sidebar, /DEFAULT_SIDEBAR_CUSTOM_EXPANDED/);
  assert.match(sidebar, /Promise\.allSettled\(\[/);
  assert.match(sidebar, /api\.listSkills\(\)/);
  assert.match(sidebar, /api\.getMcp\(\)/);
  assert.match(sidebar, /api\.listExperts\(workspaceHash \|\| '__local__'\)/);
  assert.match(sidebar, /api\.listModels\(\)/);
  assert.match(sidebar, /const totalCount = sidebarCustomTotalCount\(counts\)/);
  assert.match(sidebar, /<VsIcon name="extension" size=\{16\} \/>/);
  assert.match(sidebar, />扩展<\/span>/);
  assert.match(sidebar, /data-sidebar-custom-section="true"/);
});

test('custom shortcuts reuse the existing settings-section callback', () => {
  const sidebar = source('components/Sidebar.jsx');
  const app = source('App.jsx');
  assert.match(sidebar, /SIDEBAR_CUSTOM_ITEMS\.map/);
  assert.doesNotMatch(sidebar, /id: 'models'/);
  assert.match(sidebar, /onOpenSettingsSection\?\.\(item\.settingsSection\)/);
  assert.match(app, /onOpenSettingsSection=\{openSettingsSection\}/);
  assert.match(sidebar, /onOpenExpertComponents\?\.\(\)/);
  assert.match(app, /onOpenExpertComponents=\{openExpertComponents\}/);
});

test('brand and settings live outside the scrolling task list, with extensions in primary navigation', () => {
  const sidebar = source('components/Sidebar.jsx');
  const topbar = source('components/TopBar.jsx');
  const tour = source('lib/desktopGuidedTour.js');
  const brand = sidebar.indexOf('data-sidebar-brand="true"');
  const nav = sidebar.indexOf('className="ace-sidebar-fixed-nav');
  const extensions = sidebar.indexOf('<CustomSidebarSection', nav);
  const taskList = sidebar.indexOf('className="ace-sidebar-scroll', nav);
  const settings = sidebar.indexOf('data-tour-target="sidebar-settings"');
  assert.ok(brand > 0 && brand < nav && nav < extensions && extensions < taskList);
  assert.ok(settings > taskList);
  assert.match(tour, /settings: '\[data-tour-target="sidebar-settings"\]'/);
  assert.doesNotMatch(topbar, /acecode-logo\.png|topbar-settings/);
  assert.doesNotMatch(sidebar, /onSearchTasks/);
  assert.match(topbar, /onClick=\{onOpenSearch\}/);
  // 收起态只留宽度归零的工具类;visibility 与宽度过渡都由 globals.css 接管。
  // 回归:曾把 transition-[width,min-width] duration-250 写在 className 上,被
  // globals.css 里无 layer 的 `:where(html, body, *)` 颜色过渡整条盖掉(无 layer
  // 规则胜过 @layer utilities),表现为顶栏那半边有滑动动画、侧栏却瞬间消失。
  assert.match(sidebar, /data-collapsed=\{collapsed \? 'true' : 'false'\}/);
  assert.match(sidebar, /collapsed \? 'w-0 min-w-0' : ''/);
  assert.doesNotMatch(sidebar, /transition-\[width,min-width\]/);
  const css = source('styles/globals.css');
  assert.match(css, /\.ace-sidebar \{[^}]*transition: width 250ms ease, min-width 250ms ease, visibility 0s,/s);
  assert.match(css, /\.ace-sidebar\[data-collapsed="true"\] \{[^}]*visibility: hidden;[^}]*visibility 0s linear 250ms/s);
});

test('extension disclosure supports accessible buttons and a stable hover icon slot', () => {
  const sidebar = source('components/Sidebar.jsx');
  const css = source('styles/globals.css');
  assert.match(sidebar, /aria-expanded=\{expanded\}\s+aria-controls=\{listId\}/);
  assert.match(sidebar, /id=\{listId\} className="ace-sidebar-custom-list/);
  assert.match(sidebar, /ace-sidebar-extensions-arrow absolute inset-0/);
  assert.match(css, /\.ace-sidebar-extensions-trigger:is\(:hover, :focus-visible, \[aria-expanded="true"\]\) \.ace-sidebar-extensions-arrow\s*\{\s*opacity: 1;/);
  assert.match(css, /\.ace-sidebar-fixed-nav\s*\{\s*max-height: 55%;/);
});

test('compact title bar shares its height with click targets and quick menu anchoring', () => {
  const topbar = source('components/TopBar.jsx');
  const css = source('styles/globals.css');
  assert.match(css, /--ace-topbar-height: 30px;/);
  assert.match(css, /--ace-topbar-control-size: calc\(var\(--ace-topbar-height\) - 6px\);/);
  assert.match(css, /\.ace-topbar-action\s*\{\s*width: var\(--ace-topbar-control-size\);\s*height: var\(--ace-topbar-control-size\);/);
  assert.match(css, /\.ace-topbar \.ace-window-control\s*\{[^}]*height: var\(--ace-topbar-control-size\);/);
  assert.match(topbar, /top: 'var\(--ace-topbar-height\)'/);
  assert.match(topbar, /<VsIcon name="search" size=\{16\}/);
});
