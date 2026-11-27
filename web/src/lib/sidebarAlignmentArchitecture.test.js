import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const srcRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const sidebar = fs.readFileSync(path.join(srcRoot, 'components/Sidebar.jsx'), 'utf8');
const extensions = fs.readFileSync(path.join(srcRoot, 'components/SidebarExtensions.jsx'), 'utf8');
const sidebarQuickMenu = fs.readFileSync(path.join(srcRoot, 'components/SidebarQuickMenu.jsx'), 'utf8');
const topbar = fs.readFileSync(path.join(srcRoot, 'components/TopBar.jsx'), 'utf8');

assert.match(sidebar, /SidebarNavItem[\s\S]*?pl-\[9px\] pr-\[13px\] py-\[3px\]/);
assert.match(extensions, /PinnedExtensionItem[\s\S]*?pl-\[9px\] pr-\[13px\] py-\[3px\]/);
assert.match(sidebar, /SidebarNavItem[\s\S]*?items-center justify-center shrink-0/);
assert.match(extensions, /PinnedExtensionItem[\s\S]*?items-center justify-center shrink-0/);
assert.match(sidebar, /ace-sidebar-meta-text whitespace-nowrap text-right text-\[13px\] text-fg-mute shrink-0 tabular-nums/);
assert.doesNotMatch(sidebar, /ace-sidebar-meta-text w-\[76px\] text-right text-\[13px\]/);
assert.match(sidebar, /grid-cols-\[24px_minmax\(0,1fr\)_auto\][\s\S]*?gap-x-\[7px\] ml-1\.5 mr-0 my-px pl-\[3px\] pr-\[1px\]/);
assert.match(sidebar, /ace-sidebar-row-idle-slot ace-sidebar-meta-text[^\n]*text-right/);
assert.doesNotMatch(sidebar, /ace-sidebar-row-idle-slot ace-sidebar-meta-text w-\[58px\]/);
assert.doesNotMatch(sidebar, /!pinned && !sessionMarker[\s\S]*?ace-sidebar-row-idle-slot w-\[18px\] shrink-0/);
assert.match(sidebar, /flex w-full min-w-0 items-center justify-end gap-2/);
assert.match(sidebar, /ace-session-pin-btn ace-sidebar-row-hover-action w-\[18px\]/);
assert.match(sidebar, /ace-sidebar-row-hover-action w-\[18px\] h-7/);
assert.match(sidebar, /ace-sidebar-session-title-button ace-sidebar-tree-content[^\n]*px-0/);
assert.match(sidebar, /ace-sidebar-tree-content min-w-0 truncate/);
assert.match(sidebar, /ace-sidebar-tree-content ace-sidebar-meta-text px-0 py-\[5px\]/);
assert.doesNotMatch(topbar, /className="ml-\[8px\]"/);
assert.match(sidebar, /SidebarSectionHeader[\s\S]*?px-0 pt-\[6px\] pb-1/);
// 分节标题的 hover 色块必须与工作区行同宽同高:同样的 mx-1.5 内缩、py-[3px] 行高、rounded-md 圆角。
assert.match(sidebar, /SidebarSectionHeader[\s\S]*?flex min-w-0 flex-1 items-center mx-1\.5 pl-\[7px\] pr-\[6px\] py-\[3px\] rounded-md/);
assert.match(sidebar, /SidebarSectionHeader[\s\S]*?inline-flex min-w-0 items-center gap-0\.5/);
assert.match(sidebar, /data-sidebar-section-disclosure=\{sectionId\}[\s\S]*?inline-flex w-5 h-6/);
const sectionHeader = sidebar.match(/function SidebarSectionHeader[\s\S]*?\r?\n}\r?\n/)[0];
assert.equal((sectionHeader.match(/<button/g) || []).length, 1);
assert.doesNotMatch(sectionHeader, /opacity-0/);
assert.doesNotMatch(sidebar, /ace-sidebar-section-title text-\[12px\]/);
assert.match(sidebar, /ace-sidebar-fixed-nav shrink-0 overflow-y-auto pb-2/);
assert.match(sidebar, /<SidebarExtensions/);
assert.match(sidebar, /mt-px mb-\[10px\]/);
assert.match(sidebar, /my-1/);
assert.doesNotMatch(sidebar, /-mt-1/);
assert.match(sidebar, /ace-sidebar-workspace-row[\s\S]*?grid-cols-\[24px_minmax\(0,1fr\)_auto\][\s\S]*?gap-x-\[7px\] mx-1\.5 pl-\[3px\] pr-\[14px\] py-\[3px\]/);
// 侧栏顶部不再放 logo + 名称 + 版本号;版本号改显示在底部菜单「关于 ACECode」右侧。
assert.doesNotMatch(sidebar, /data-sidebar-brand/);
assert.doesNotMatch(sidebar, /<BrandLogo/);
assert.match(sidebar, /versionLabel=\{appVersionLabel\}/);
assert.match(sidebarQuickMenu, /action\.id === 'about' \? versionLabel : ''/);
assert.match(sidebarQuickMenu, /text-\[11px\] leading-none text-fg-mute tabular-nums/);
assert.match(sidebar, /ace-sidebar-footer shrink-0 pl-\[9px\]/);
assert.match(sidebarQuickMenu, /w-6 h-8 shrink-0 flex items-center justify-center/);
assert.doesNotMatch(sidebarQuickMenu, /w-6 h-8 shrink-0 flex items-center justify-start/);

// 顶栏会话标题必须落在侧栏右边框的右侧。导航块宽度由 --ace-topbar-leading-inset 推出,
// 它必须等于顶栏自身的左内边距,否则导航块会提前结束、把标题顶到边框上,显得很挤。
const styles = fs.readFileSync(path.join(srcRoot, 'styles/globals.css'), 'utf8');
const topbarRule = styles.match(/\.ace-topbar \{[\s\S]*?\n\}/)[0];
const navigationRule = styles.match(/\.ace-topbar-navigation \{[\s\S]*?\n\}/)[0];
const sessionTitleRule = styles.match(/\.ace-topbar-session-title \{[^}]*\}/)[0];
const leadingInset = Number(topbarRule.match(/--ace-topbar-leading-inset:\s*(\d+)px/)[1]);
const topbarPaddingLeft = Number(topbar.match(/ace-topbar (?:px|pl)-(\d+)/)[1]) * 4;
const sessionTitlePadding = Number(sessionTitleRule.match(/padding-left:\s*(\d+)px/)[1]);
assert.equal(leadingInset, topbarPaddingLeft);
assert.ok(sessionTitlePadding > 4, 'session title must clear the sidebar border by its own padding');
// 导航块的宽度只能由侧栏宽度推出。按视口加的上限或断点会在窗口变窄时提前结束留白,
// 把标题重新拽回边框左侧。
assert.doesNotMatch(navigationRule.replace(/\/\*[\s\S]*?\*\//g, ''), /max-width/);
assert.equal(
  (styles.match(/\.ace-topbar-session-title \{[^}]*\}/g) || []).length,
  1,
  'only one .ace-topbar-session-title rule may exist',
);

console.log('[pass] sidebar alignment and vertical rhythm preserve shared baselines');
