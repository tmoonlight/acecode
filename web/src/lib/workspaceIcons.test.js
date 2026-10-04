import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import {
  WORKSPACE_ICONS,
  WORKSPACE_ICON_COLORS,
  filterWorkspaceIcons,
  isDefaultWorkspaceIcon,
  resolveWorkspaceIcon,
  workspaceIconColorValue,
} from './workspaceIcons.js';

const srcRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

function source(relativePath) {
  return fs.readFileSync(path.join(srcRoot, relativePath), 'utf8');
}

function test(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (err) {
    console.error(`[fail] ${name}`);
    throw err;
  }
}

// 场景:图标表与色板是写进 workspace.json 的键。期望:与后端 is_valid_workspace_icon
// 的字符集一致(id [a-z0-9-]{1,40},color [a-z0-9-]{0,24}),且 id 不重复 —— 否则
// 前端选得出、后端存不进(400)。图二的 30 个图标与 8 个颜色都在。
test('图标与颜色键满足后端字符集约束', () => {
  assert.equal(WORKSPACE_ICONS.length, 30);
  assert.equal(WORKSPACE_ICON_COLORS.length, 8);
  const ids = new Set();
  for (const icon of WORKSPACE_ICONS) {
    assert.match(icon.id, /^[a-z0-9-]{1,40}$/);
    assert.equal(ids.has(icon.id), false, `duplicate icon id ${icon.id}`);
    ids.add(icon.id);
    assert.ok(icon.closed.startsWith('<'), `icon ${icon.id} has closed svg body`);
    assert.ok(icon.open.startsWith('<'), `icon ${icon.id} has open svg body`);
  }
  for (const color of WORKSPACE_ICON_COLORS) {
    assert.match(color.id, /^[a-z0-9-]{0,24}$/);
  }
  assert.equal(WORKSPACE_ICONS[0].id, 'folder');
  assert.equal(WORKSPACE_ICON_COLORS[0].id, 'default');
});

// 场景:后端返回 icon 为 null / 未知 id / 未知颜色。期望:null 与未知 id 视为未设置;
// 未知颜色退回默认色;默认色取文字颜色(空值)。
test('解析后端图标字段', () => {
  assert.equal(resolveWorkspaceIcon(null), null);
  assert.equal(resolveWorkspaceIcon({ id: 'rocket', color: 'red' }), null);
  assert.deepEqual(resolveWorkspaceIcon({ id: 'brain', color: 'violet' }), { id: 'brain', color: 'default' });
  assert.equal(workspaceIconColorValue('default'), '');
  assert.equal(workspaceIconColorValue('nope'), '');
  assert.equal(workspaceIconColorValue('blue'), '#0a84ff');
});

// 场景:判断是否等价于「未设置」。期望:只有文件夹 + 默认色才是;换色或换图都不是。
test('默认图标判定', () => {
  assert.equal(isDefaultWorkspaceIcon(null), true);
  assert.equal(isDefaultWorkspaceIcon({ id: 'folder', color: 'default' }), true);
  assert.equal(isDefaultWorkspaceIcon({ id: 'folder', color: 'red' }), false);
  assert.equal(isDefaultWorkspaceIcon({ id: 'music', color: 'default' }), false);
});

// 场景:搜索框输入。期望:空查询返回全部;按中文名、英文关键词不区分大小写匹配;
// 多个词须全部命中。
test('搜索图标按名称与关键词过滤', () => {
  assert.equal(filterWorkspaceIcons('').length, WORKSPACE_ICONS.length);
  assert.deepEqual(filterWorkspaceIcons('音乐').map((icon) => icon.id), ['music']);
  assert.deepEqual(filterWorkspaceIcons('TERMINAL').map((icon) => icon.id), ['terminal']);
  assert.deepEqual(filterWorkspaceIcons('fitness gym').map((icon) => icon.id), ['dumbbell']);
  assert.deepEqual(filterWorkspaceIcons('zzz-not-found'), []);
});

// 场景:侧栏项目折叠 / 展开、选图标网格选中格要在两态之间切换。期望:30 个图标每个都有
// 独立的展开态,且与折叠态不是同一张图 —— 漏画一个,展开时就看不出变化。
test('每个图标都有不同于折叠态的展开态', () => {
  for (const icon of WORKSPACE_ICONS) {
    assert.equal(typeof icon.open, 'string', `icon ${icon.id} missing open`);
    assert.notEqual(icon.open, icon.closed, `icon ${icon.id} open === closed`);
  }
});

// 场景:用户要求图标保持线条风格、不做实心填充。期望:两态标记里只有 path / circle /
// rect 描边元素,不出现任何 fill 属性(调色板的颜料点曾用 fill="currentColor" 画实心点,
// 现在靠圆角线帽画点);也不能带 stroke-width,线宽统一由 WorkspaceIcon 给。
test('图标两态都是纯描边,不含填充', () => {
  for (const icon of WORKSPACE_ICONS) {
    for (const [state, body] of [['closed', icon.closed], ['open', icon.open]]) {
      assert.doesNotMatch(body, /fill=/, `icon ${icon.id} ${state} has fill`);
      assert.doesNotMatch(body, /stroke-width=/, `icon ${icon.id} ${state} overrides stroke width`);
      const tags = [...body.matchAll(/<([a-z]+)\b/g)].map((m) => m[1]);
      assert.ok(tags.length > 0, `icon ${icon.id} ${state} is empty`);
      for (const tag of tags) {
        assert.ok(['path', 'circle', 'rect'].includes(tag), `icon ${icon.id} ${state} uses <${tag}>`);
      }
    }
  }
});

// 场景:组件按展开态选图形。期望:WorkspaceIcon 的 open 决定用哪一态,SVG 根上 fill="none";
// 侧栏项目行与拖动预览按 expanded 传 open,选图标网格按选中传 open。
test('折叠 / 展开两态接到侧栏与选图标网格', () => {
  const iconComponent = source('components/WorkspaceIcon.jsx');
  assert.ok(iconComponent.includes('open ? icon.open : icon.closed'));
  assert.ok(iconComponent.includes('fill="none"'));
  const sidebar = source('components/Sidebar.jsx');
  assert.ok(sidebar.includes('open={expanded}'));
  assert.ok(sidebar.includes('<SidebarWorkspaceGlyph icon={ws.icon} expanded={expanded} />'));
  const picker = source('components/WorkspaceIconPicker.jsx');
  assert.ok(picker.includes('open={selected}'));
});

// 选择层可以因外部点击收起,但编辑项目对话框仍保留草稿。
// 真实点击、焦点和草稿保留由 test-context-image-paste.mjs 覆盖。
test('收起选图标层保留编辑项目对话框', () => {
  const picker = source('components/WorkspaceIconPicker.jsx');
  assert.ok(picker.includes("event.key !== 'Escape'"));
  const modal = source('components/EditWorkspaceModal.jsx');
  assert.match(modal, /<Modal[\s\S]*?dismissOnBackdrop=\{false\}[\s\S]*?labelledBy="ace-edit-workspace-title"/);
  assert.ok(modal.includes('setPickerOpen((open) => !open)'));
});
