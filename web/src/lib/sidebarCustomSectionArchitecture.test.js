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

// 「扩展」改为向右弹出菜单 + 「自定义」对话框:计数请求仍并行且互不阻塞,
// 条目点击仍走原来的设置分节 / 专家组件回调。
test('sidebar extensions open a flyout menu with settled counts', () => {
  const sidebar = source('components/Sidebar.jsx');
  const extensions = source('components/SidebarExtensions.jsx');
  assert.match(sidebar, /import \{ SidebarExtensions \} from '\.\/SidebarExtensions\.jsx'/);
  assert.doesNotMatch(sidebar, /CustomSidebarSection|sidebarCustomSectionExpanded/);
  assert.match(extensions, /useSyncExternalStore\(store\.subscribe, store\.getSnapshot\)/);
  assert.match(extensions, /Promise\.allSettled\(\[/);
  assert.match(extensions, /api\.listSkills\(\)/);
  assert.match(extensions, /api\.getMcp\(\)/);
  assert.match(extensions, /api\.listExperts\(workspaceHash \|\| '__local__'\)/);
  assert.match(extensions, /api\.listModels\(\)/);
  assert.match(extensions, /<VsIcon name="extension" size=\{18\} \/>/);
  assert.match(extensions, />扩展<\/span>/);
  assert.match(extensions, /data-sidebar-custom-section="true"/);
  assert.match(extensions, /aria-haspopup="menu"/);
  assert.match(extensions, /<AnchoredMenu[\s\S]*preferredPlacement="right"/);
  assert.match(extensions, />自定义<\/span>/);
});

test('custom shortcuts reuse the existing settings-section callback', () => {
  const sidebar = source('components/Sidebar.jsx');
  const extensions = source('components/SidebarExtensions.jsx');
  const app = source('App.jsx');
  assert.match(extensions, /sidebarExtensionLayout\(prefs\)/);
  assert.doesNotMatch(extensions, /id: 'models'/);
  assert.match(extensions, /onOpenSettingsSection\?\.\(item\.settingsSection\)/);
  assert.match(extensions, /onOpenExpertComponents\?\.\(\)/);
  assert.match(sidebar, /onOpenSettingsSection\?\.\(section\)/);
  assert.match(app, /onOpenSettingsSection=\{openSettingsSection\}/);
  assert.match(app, /onOpenExpertComponents=\{openExpertComponents\}/);
});

// 「自定义」必须走共享 Modal(键盘约定),「完成」是默认操作;拖动中的 Esc
// 只撤销拖动,不能冒泡到 Modal 把对话框关掉。
test('extension customizer uses the shared modal with checkable, draggable rows', () => {
  const extensions = source('components/SidebarExtensions.jsx');
  assert.match(extensions, /<Modal onClose=\{onClose\}/);
  assert.match(extensions, /data-ace-dialog-primary="true"[\s\S]*?完成/);
  assert.match(extensions, /role="checkbox"\s+aria-checked=\{pinned\}/);
  assert.match(extensions, /toggleSidebarExtensionPinned\(/);
  assert.match(extensions, /moveSidebarExtension\(/);
  assert.match(extensions, /keyEvent\.stopImmediatePropagation\(\)/);
  assert.match(extensions, /<VsIcon name="GripVertical"/);
});

test('settings live outside the scrolling task list, with extensions in primary navigation and no brand header', () => {
  const sidebar = source('components/Sidebar.jsx');
  const topbar = source('components/TopBar.jsx');
  const tour = source('lib/desktopGuidedTour.js');
  const nav = sidebar.indexOf('className="ace-sidebar-fixed-nav');
  const extensions = sidebar.indexOf('<SidebarExtensions', nav);
  const taskList = sidebar.indexOf('className="ace-sidebar-scroll', nav);
  const settings = sidebar.indexOf('data-tour-target="sidebar-settings"');
  assert.ok(nav > 0 && nav < extensions && extensions < taskList);
  assert.equal(sidebar.indexOf('data-sidebar-brand'), -1);
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

test('extension trigger shows a trailing chevron and the fixed nav keeps its height cap', () => {
  const extensions = source('components/SidebarExtensions.jsx');
  const css = source('styles/globals.css');
  assert.match(extensions, /aria-expanded=\{menuOpen\}/);
  assert.match(extensions, /<VsIcon name="expandRight" size=\{16\}/);
  assert.doesNotMatch(css, /ace-sidebar-extensions-arrow/);
  assert.match(css, /\.ace-sidebar-fixed-nav\s*\{\s*max-height: 55%;/);
});

test('padded title bar keeps its click targets while the footer owns the anchored menu', () => {
  const topbar = source('components/TopBar.jsx');
  const menu = source('components/SidebarQuickMenu.jsx');
  const css = source('styles/globals.css');
  assert.match(css, /--ace-topbar-height: 41px;/);
  assert.match(css, /--ace-topbar-control-size: 24px;/);
  assert.match(css, /\.ace-topbar\s*\{[^}]*padding-top: 5px;\s*padding-bottom: 6px;/);
  assert.match(css, /\.ace-topbar-action\s*\{\s*width: var\(--ace-topbar-control-size\);\s*height: var\(--ace-topbar-control-size\);/);
  assert.match(css, /\.ace-topbar \.ace-window-control\s*\{[^}]*height: var\(--ace-topbar-control-size\);/);
  assert.doesNotMatch(topbar, /topbar-quick-actions-menu|aria-haspopup="menu"/);
  assert.match(menu, /<AnchoredMenu\s+anchorRef=\{anchorRef\}/);
  assert.match(topbar, /<VsIcon name="search" size=\{16\}/);
});

test('macOS fullscreen eases the top-bar actions out of the traffic-light inset', () => {
  const topbar = source('components/TopBar.jsx');
  const css = source('styles/globals.css');
  assert.match(topbar, /shouldInsetMacTopBar\(isFullscreen\) && 'ace-desktop-macos-topbar'/);
  assert.match(
    css,
    /\.ace-desktop-frameless-topbar\s*\{[^}]*padding-left 160ms cubic-bezier\(0\.2, 0, 0, 1\)/s,
  );
});
