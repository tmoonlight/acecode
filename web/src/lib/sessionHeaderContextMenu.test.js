import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import {
  applyContextMenuActionOverrides,
  buildDesktopContextMenuItems,
  CONTEXT_MENU_DELEGATE_EVENT,
  CONTEXT_MENU_DELEGATE_SELECTOR,
  contextMenuDelegateFromElement,
  DESKTOP_CONTEXT_ACTIONS,
  dispatchContextMenuDelegate,
  SESSION_HEADER_CONTEXT_MENU_DELEGATE,
} from './desktopContextMenu.js';
import { resolveSessionTitleRename, sessionTitleRenameWorkspaceHash } from './sessionTitleRename.js';

const srcRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

function source(relativePath) {
  return fs.readFileSync(path.join(srcRoot, relativePath), 'utf8').replace(/\r\n?/g, '\n');
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

// 鸭子类型假 DOM:closest 只认委托选择器,tagName 让 editableTargetFromElement 能判断输入框。
function delegateHost(name) {
  return {
    tagName: 'DIV',
    parentElement: null,
    closest(selector) {
      return selector === CONTEXT_MENU_DELEGATE_SELECTOR ? this : null;
    },
    getAttribute(attr) {
      return attr === 'data-desktop-context-menu-delegate' ? name : null;
    },
  };
}

function childOf(host, tagName = 'SPAN', extra = {}) {
  return {
    tagName,
    parentElement: host,
    closest: (selector) => host.closest(selector),
    getAttribute: () => null,
    ...extra,
  };
}

function recordingTarget(onEvent) {
  return {
    events: [],
    dispatchEvent(event) {
      this.events.push(event);
      onEvent?.(event);
      return true;
    },
  };
}

// 顶栏右键委托。
// 触发场景:用户在顶部导航栏任意位置(标题、空白处、「会话菜单」按钮本身)点右键。
// 期望行为:DesktopContextMenu 识别到委托区域,把点位广播给认领方,由会话头部打开
// 与「会话菜单」按钮左键完全相同的菜单。
// 回归(bug 表现):顶栏右键只出通用的「全选」菜单;「会话菜单」按钮右键因为带
// data-desktop-session-* 属性,出的是另一份没有「侧边聊天 / 查找」的会话菜单,左右键两套。

test('顶栏内任意子元素右键都解析到会话头部委托', () => {
  const topBar = delegateHost(SESSION_HEADER_CONTEXT_MENU_DELEGATE);
  const button = childOf(topBar, 'BUTTON');
  const delegate = contextMenuDelegateFromElement(button);
  assert.equal(delegate?.name, SESSION_HEADER_CONTEXT_MENU_DELEGATE);
  assert.equal(delegate?.element, topBar);
});

test('就地重命名的输入框里右键不委托,保留复制 / 粘贴菜单', () => {
  const topBar = delegateHost(SESSION_HEADER_CONTEXT_MENU_DELEGATE);
  const input = childOf(topBar, 'INPUT', { type: 'text', disabled: false, readOnly: false });
  assert.equal(contextMenuDelegateFromElement(input), null);
});

test('委托区域之外右键不委托', () => {
  const plain = { tagName: 'DIV', parentElement: null, closest: () => null, getAttribute: () => null };
  assert.equal(contextMenuDelegateFromElement(plain), null);
  assert.equal(contextMenuDelegateFromElement(null), null);
});

test('有认领方时 dispatch 返回 true 并带上光标点位', () => {
  const topBar = delegateHost(SESSION_HEADER_CONTEXT_MENU_DELEGATE);
  const target = recordingTarget((event) => {
    assert.equal(event.type, CONTEXT_MENU_DELEGATE_EVENT);
    assert.equal(event.detail.name, SESSION_HEADER_CONTEXT_MENU_DELEGATE);
    assert.equal(event.detail.element, topBar);
    assert.equal(event.detail.x, 120);
    assert.equal(event.detail.y, 18);
    event.detail.handled = true;
  });
  assert.equal(dispatchContextMenuDelegate({ name: SESSION_HEADER_CONTEXT_MENU_DELEGATE, element: topBar }, { x: 120, y: 18 }, target), true);
  assert.equal(target.events.length, 1);
});

test('无人认领(首页没有打开会话)时返回 false,调用方回落到通用菜单', () => {
  const target = recordingTarget();
  assert.equal(dispatchContextMenuDelegate({ name: SESSION_HEADER_CONTEXT_MENU_DELEGATE }, { x: 1, y: 1 }, target), false);
  assert.equal(dispatchContextMenuDelegate(null, { x: 1, y: 1 }, target), false);
});

// 头部菜单接管「重命名」。
// 触发场景:从会话头部菜单点「重命名」。
// 期望行为:只替换该项的 onSelect(在顶部标题处就地编辑),文案 / 分组 / 其余动作不变。
// 回归(bug 表现):重命名派发给侧栏行,输入框出现在左侧会话栏里,与点击位置脱节。

test('动作覆盖只给命中的项挂 onSelect,其它项原样返回', () => {
  const items = buildDesktopContextMenuItems({
    sessionTarget: { type: 'session', sessionId: 's1', title: 't', pinned: false, canArchive: true },
  });
  const startRename = () => {};
  const next = applyContextMenuActionOverrides(items, {
    [DESKTOP_CONTEXT_ACTIONS.RENAME_SESSION]: startRename,
  });
  assert.equal(next.length, items.length);
  const rename = next.find((item) => item.id === DESKTOP_CONTEXT_ACTIONS.RENAME_SESSION);
  assert.equal(rename.onSelect, startRename);
  assert.equal(rename.group, items.find((item) => item.id === DESKTOP_CONTEXT_ACTIONS.RENAME_SESSION).group);
  for (const [index, item] of next.entries()) {
    if (item.id !== DESKTOP_CONTEXT_ACTIONS.RENAME_SESSION) assert.equal(item, items[index]);
  }
});

test('没有覆盖表时原数组直接返回', () => {
  const items = [{ id: 'a' }];
  assert.equal(applyContextMenuActionOverrides(items, null), items);
  assert.equal(applyContextMenuActionOverrides(items, undefined), items);
});

// 顶栏重命名提交判定。
// 触发场景:就地编辑后回车 / 失焦。
// 期望行为:与进入编辑时显示的标题相同就不发请求;清空是合法提交(回落到 summary)。
// 回归(bug 表现):没有用户标题的会话显示的是 summary,原样回车会把 summary 固化成用户标题。

test('未改动(含首尾空白差异)不提交', () => {
  assert.deepEqual(resolveSessionTitleRename('  修复登录  ', '修复登录'), { changed: false, title: '修复登录' });
  assert.deepEqual(resolveSessionTitleRename('', ''), { changed: false, title: '' });
});

test('改动后提交去掉首尾空白的新标题,清空也算改动', () => {
  assert.deepEqual(resolveSessionTitleRename(' 新标题 ', '旧标题'), { changed: true, title: '新标题' });
  assert.deepEqual(resolveSessionTitleRename('   ', '旧标题'), { changed: true, title: '' });
});

test('no-workspace 与 __local__ 会话走不带 workspace 段的改名路由', () => {
  assert.equal(sessionTitleRenameWorkspaceHash({ workspaceHash: 'abc', noWorkspace: true }), '');
  assert.equal(sessionTitleRenameWorkspaceHash({ workspaceHash: '__local__' }), '');
  assert.equal(sessionTitleRenameWorkspaceHash({ workspaceHash: ' abc ' }), 'abc');
  assert.equal(sessionTitleRenameWorkspaceHash(), '');
});

// 接线合同:三处缺一不可,任何一处被删都会退回「左右键两套菜单 / 改名跑到侧栏」。
test('顶栏与内嵌头部都标记为会话头部委托区域', () => {
  assert.match(source('components/TopBar.jsx'), /data-desktop-context-menu-delegate=\{SESSION_HEADER_CONTEXT_MENU_DELEGATE\}/);
  assert.match(source('components/SessionTitleBar.jsx'), /data-desktop-context-menu-delegate=\{SESSION_HEADER_CONTEXT_MENU_DELEGATE\}/);
});

test('DesktopContextMenu 在构建通用菜单之前先尝试委托,并应用显式菜单的动作覆盖', () => {
  const menu = source('components/DesktopContextMenu.jsx');
  const delegateAt = menu.indexOf('dispatchContextMenuDelegate(delegate');
  const buildAt = menu.indexOf('const candidateTargets = contextTargetsFromElement(rawTarget)');
  assert.ok(delegateAt > 0 && buildAt > delegateAt);
  assert.match(menu, /applyContextMenuActionOverrides\([\s\S]*?explicit\?\.actionOverrides/);
});

test('ChatView 的按钮左键与顶栏右键共用同一个菜单构造,重命名改为就地编辑', () => {
  const chat = source('components/ChatView.jsx');
  assert.match(chat, /window\.addEventListener\(CONTEXT_MENU_DELEGATE_EVENT, handler\)/);
  assert.match(chat, /placement: 'pointer'/);
  assert.match(chat, /const openSessionContextMenu = useStableCallback\(\(event\) => \{[\s\S]*?openSessionMenu\(/);
  assert.match(chat, /\[DESKTOP_CONTEXT_ACTIONS\.RENAME_SESSION\]: startHeaderRename/);
  assert.match(chat, /renaming=\{headerRenaming/);
});
