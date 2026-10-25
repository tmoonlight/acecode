import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import {
  FILE_LINK_ROOT_SELECTOR,
  FILE_LINK_TARGET_SELECTOR,
  fileLinkName,
  fileLinkTargetFromElement,
  fileLinkWorkspaceRelativePath,
  resolveFileLinkAbsolutePath,
} from './fileLinkContextMenu.js';
import {
  buildDesktopContextMenuItems,
  contextTargetsFromElement,
  DESKTOP_CONTEXT_ACTIONS,
  filterContextMenuItemsForRuntime,
} from './desktopContextMenu.js';

const here = dirname(fileURLToPath(import.meta.url));
const source = (relative) => readFileSync(join(here, '..', relative), 'utf8');

function test(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (err) {
    console.error(`[fail] ${name}`);
    throw err;
  }
}

// 鸭子类型的假 DOM:右键点在链接内部的文字节点上,链接挂在一个声明了工作目录的区域里。
function fakeLinkTarget({ path, kind = '', line = '', cwd = '', attach = false, root = true }) {
  const rootAttrs = {
    'data-file-link-root': 'true',
    'data-file-link-cwd': cwd,
    'data-file-link-attach': attach ? 'true' : null,
  };
  const rootElement = root ? { getAttribute: (name) => rootAttrs[name] ?? null, closest: () => null } : null;
  const linkAttrs = { 'data-file-path': path, 'data-file-kind': kind || null, 'data-file-line': line || null };
  const link = {
    getAttribute: (name) => linkAttrs[name] ?? null,
    closest: (selector) => {
      if (selector === FILE_LINK_TARGET_SELECTOR) return link;
      if (selector === FILE_LINK_ROOT_SELECTOR) return rootElement;
      return null;
    },
  };
  return { closest: (selector) => (selector === FILE_LINK_TARGET_SELECTOR ? link : null) };
}

function ids(items) {
  return items.map((item) => item.id);
}

// 场景:模型在 Windows 工作区里写了相对链接 `src/foo.ts`。期望:按会话工作目录拼成
// 反斜杠形态的绝对路径,与资源管理器地址栏 / 终端里粘贴的一致。
test('Windows 工作目录下的相对链接还原成反斜杠绝对路径', () => {
  assert.equal(resolveFileLinkAbsolutePath('N:\\Users\\shao\\acecode', 'src/foo.ts'), 'N:\\Users\\shao\\acecode\\src\\foo.ts');
  // Desktop 注册的工作目录是正斜杠形态(E:/SS项目/...),结果仍按 Windows 习惯输出反斜杠。
  assert.equal(resolveFileLinkAbsolutePath('E:/SS项目数据库/app/', './docs/说明.md'), 'E:\\SS项目数据库\\app\\docs\\说明.md');
});

// 场景:链接里带 . / .. 段,或本身就是绝对路径(含混用分隔符)。期望:消掉 . / ..,
// 绝对路径不再拼工作目录;.. 越过根时停在根上而不是产生残缺路径。
test('点段被消掉,绝对链接不拼工作目录', () => {
  assert.equal(resolveFileLinkAbsolutePath('N:\\repo\\web', '../src/./a.cpp'), 'N:\\repo\\src\\a.cpp');
  assert.equal(resolveFileLinkAbsolutePath('N:\\repo', 'C:/Users/x/../y/z.txt'), 'C:\\Users\\y\\z.txt');
  assert.equal(resolveFileLinkAbsolutePath('N:\\repo', 'C:/../../a.txt'), 'C:\\a.txt');
  assert.equal(resolveFileLinkAbsolutePath('/home/me/proj', 'lib/../README.md'), '/home/me/proj/README.md');
  assert.equal(resolveFileLinkAbsolutePath('', '/etc/hosts'), '/etc/hosts');
  assert.equal(resolveFileLinkAbsolutePath('', '\\\\server\\share\\dir\\..\\f.txt'), '\\\\server\\share\\f.txt');
});

// 场景:相对链接所在区域没有工作目录(例如无工作区会话还没解析出目录)。期望:返回空串,
// 菜单据此把「绝对路径」「在资源管理器中显示」置灰,而不是拼出一个错误路径。
test('相对链接没有工作目录时不猜测绝对路径', () => {
  assert.equal(resolveFileLinkAbsolutePath('', 'src/foo.ts'), '');
  assert.equal(resolveFileLinkAbsolutePath('relative/cwd', 'src/foo.ts'), '');
  assert.equal(resolveFileLinkAbsolutePath('N:\\repo', ''), '');
});

// 场景:「添加到会话」优先插入工作目录内的相对 @路径。期望:Windows 路径按大小写不敏感
// 比较;工作目录外、或只是前缀相同的兄弟目录(repo2 对 repo)不算在内。
test('工作目录内的绝对路径给出相对形态', () => {
  assert.equal(fileLinkWorkspaceRelativePath('N:/Repo', 'n:\\repo\\src\\a.ts'), 'src/a.ts');
  assert.equal(fileLinkWorkspaceRelativePath('N:\\repo', 'N:\\repo2\\a.ts'), '');
  assert.equal(fileLinkWorkspaceRelativePath('/home/me/proj', '/home/me/Proj/a.ts'), '');
  assert.equal(fileLinkWorkspaceRelativePath('', 'N:\\repo\\a.ts'), '');
  assert.equal(fileLinkName('N:\\repo\\src\\'), 'src');
});

// 场景:右键主对话里的文件链接 `src/foo.ts:12`。期望:目标带原始路径、行号、绝对路径、
// 相对路径、文件名,并读取区域上的「可添加到会话」声明。
test('从链接元素读出文件链接目标', () => {
  const target = fileLinkTargetFromElement(fakeLinkTarget({
    path: 'src/foo.ts', line: '12', cwd: 'N:\\repo', attach: true,
  }));
  assert.deepEqual(target, {
    type: 'file-link',
    path: 'src/foo.ts',
    kind: 'file',
    line: 12,
    absolutePath: 'N:\\repo\\src\\foo.ts',
    relativePath: 'src/foo.ts',
    name: 'foo.ts',
    canAddContext: true,
  });
  const outside = fileLinkTargetFromElement(fakeLinkTarget({ path: 'D:/other/a.md', cwd: 'N:\\repo' }));
  assert.equal(outside.relativePath, '');
  assert.equal(outside.absolutePath, 'D:\\other\\a.md');
  assert.equal(outside.canAddContext, false);
});

// 场景:文件预览里的 markdown 也有 a[data-file-path],但不在声明了工作目录的区域里。
// 期望:不识别成文件链接目标,沿用原来的通用菜单(那里的相对路径基准是预览文件所在目录,
// 按会话工作目录拼会得到错误路径)。
test('区域外的文件链接不进入文件链接菜单', () => {
  assert.equal(fileLinkTargetFromElement(fakeLinkTarget({ path: 'a.md', root: false })), null);
  assert.equal(fileLinkTargetFromElement(null), null);
  assert.equal(contextTargetsFromElement(fakeLinkTarget({ path: 'a.md', root: false })).fileLinkTarget, null);
  assert.equal(contextTargetsFromElement(fakeLinkTarget({ path: 'a.md', cwd: '/w' })).fileLinkTarget.absolutePath, '/w/a.md');
});

// 场景:Desktop 里右键主对话的文件链接。期望:与 Claude 桌面版同形 —— 添加到会话、
// 复制 ▸(绝对路径 / 文件名)、分隔线、在资源管理器中显示;不再混入复制消息 / 分叉 / 全选。
test('文件链接菜单:添加到会话、复制二级菜单、在资源管理器中显示', () => {
  const fileLinkTarget = fileLinkTargetFromElement(fakeLinkTarget({ path: 'src/foo.ts', cwd: 'N:\\repo', attach: true }));
  const items = buildDesktopContextMenuItems({
    allowNativeActions: true,
    fileLinkTarget,
    messageTarget: { role: 'assistant', text: 'x', messageId: 'm', canFork: true },
  });
  assert.deepEqual(ids(items), [
    DESKTOP_CONTEXT_ACTIONS.ADD_FILE_CONTEXT,
    DESKTOP_CONTEXT_ACTIONS.COPY_SUBMENU,
    DESKTOP_CONTEXT_ACTIONS.OPEN_IN_EXPLORER,
  ]);
  assert.equal(items[2].separatorBefore, true);
  assert.equal(items[2].label, '在资源管理器中显示');
  assert.deepEqual(items[2].target, { path: 'N:\\repo\\src\\foo.ts', kind: 'file' });
  const copy = items[1];
  assert.deepEqual(copy.children.map((child) => [child.id, child.label, child.enabled]), [
    [DESKTOP_CONTEXT_ACTIONS.COPY_ABSOLUTE_PATH, '绝对路径', true],
    [DESKTOP_CONTEXT_ACTIONS.COPY_FILE_NAME, '文件名', true],
  ]);
  assert.equal(copy.children[0].target.absolutePath, 'N:\\repo\\src\\foo.ts');
});

// 场景:普通浏览器(没有资源管理器能力)、侧边对话(不可添加到会话)、目录链接、
// 无法还原绝对路径。期望:分别隐藏资源管理器项 / 隐藏添加项 / 换成目录版文案 / 置灰。
test('文件链接菜单随运行环境与链接类型收缩', () => {
  const browser = buildDesktopContextMenuItems({
    allowNativeActions: false,
    fileLinkTarget: fileLinkTargetFromElement(fakeLinkTarget({ path: 'a.md', cwd: '/w', attach: true })),
  });
  assert.deepEqual(ids(browser), [DESKTOP_CONTEXT_ACTIONS.ADD_FILE_CONTEXT, DESKTOP_CONTEXT_ACTIONS.COPY_SUBMENU]);

  const sideChat = buildDesktopContextMenuItems({
    allowNativeActions: true,
    fileLinkTarget: fileLinkTargetFromElement(fakeLinkTarget({ path: 'a.md', cwd: '/w' })),
  });
  assert.deepEqual(ids(sideChat), [DESKTOP_CONTEXT_ACTIONS.COPY_SUBMENU, DESKTOP_CONTEXT_ACTIONS.OPEN_IN_EXPLORER]);

  const directory = buildDesktopContextMenuItems({
    allowNativeActions: true,
    fileLinkTarget: fileLinkTargetFromElement(fakeLinkTarget({ path: 'src', kind: 'directory', cwd: '/w', attach: true })),
  });
  assert.equal(directory[0].id, DESKTOP_CONTEXT_ACTIONS.ADD_DIRECTORY_CONTEXT);
  assert.equal(directory[1].children[1].label, '文件夹名');
  assert.equal(directory[2].label, undefined);
  assert.equal(directory[2].target.kind, 'directory');

  const unresolved = buildDesktopContextMenuItems({
    allowNativeActions: true,
    fileLinkTarget: fileLinkTargetFromElement(fakeLinkTarget({ path: 'a.md', cwd: '' })),
  });
  assert.equal(unresolved[0].children[0].enabled, false);
  assert.equal(unresolved[0].children[1].enabled, true);
  assert.equal(unresolved[1].enabled, false);

  // 链接文字被选中时保留「复制」,方便复制选区。
  const selected = buildDesktopContextMenuItems({
    hasSelection: true,
    fileLinkTarget: fileLinkTargetFromElement(fakeLinkTarget({ path: 'a.md', cwd: '/w' })),
  });
  assert.equal(selected.at(-1).id, DESKTOP_CONTEXT_ACTIONS.COPY);
});

// 场景:二级菜单的子项全部是只有 Desktop 才能执行的动作,却运行在普通浏览器里。
// 期望:子项被滤掉后,空的二级菜单连同触发项一起消失,不留一个点开是空白的「复制 ▸」。
test('运行环境过滤会递归到二级菜单', () => {
  const items = filterContextMenuItemsForRuntime([
    { id: DESKTOP_CONTEXT_ACTIONS.COPY_SUBMENU, children: [{ id: DESKTOP_CONTEXT_ACTIONS.OPEN_IN_EXPLORER }] },
    { id: DESKTOP_CONTEXT_ACTIONS.COPY_SUBMENU, children: [
      { id: DESKTOP_CONTEXT_ACTIONS.OPEN_IN_EXPLORER },
      { id: DESKTOP_CONTEXT_ACTIONS.COPY_FILE_NAME },
    ] },
  ], { allowNativeActions: false });
  assert.equal(items.length, 1);
  assert.deepEqual(ids(items[0].children), [DESKTOP_CONTEXT_ACTIONS.COPY_FILE_NAME]);
});

// 场景:接线。期望:主对话转录区声明工作目录且允许添加到会话;侧边对话只声明工作目录;
// 右键菜单能执行「复制文件名」并渲染二级菜单。回归:漏掉区域声明时右键链接只剩通用菜单。
test('转录区与侧边对话声明文件链接区域,菜单渲染二级菜单', () => {
  const chat = source('components/ChatView.jsx');
  assert.match(chat, /data-file-link-root="true"\s+data-file-link-cwd=\{sidePanelCwd\}\s+data-file-link-attach="true"\s+className="ace-chat-transcript-scroll/);
  assert.match(chat, /fileLinkCwd=\{sidePanelCwd\}/);
  const sideChat = source('components/SideChatWindow.jsx');
  assert.match(sideChat, /data-file-link-root="true"\s+data-file-link-cwd=\{fileLinkCwd\}/);
  assert.doesNotMatch(sideChat, /data-file-link-attach/);
  const menu = source('components/DesktopContextMenu.jsx');
  assert.match(menu, /case DESKTOP_CONTEXT_ACTIONS\.COPY_FILE_NAME:\s+await copyTextWithToast\(actionTarget\?\.name \|\| ''\);/);
  assert.match(menu, /aria-haspopup=\{children \? 'menu' : undefined\}/);
  assert.match(menu, /preferredSide: 'right'/);
});
