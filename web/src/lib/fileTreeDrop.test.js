import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import {
  FILE_TREE_DRAG_PREVIEW_LIMIT,
  FILE_TREE_DROP_EVENT,
  dispatchFileTreeDrop,
  fileTreeDragGhostPosition,
  fileTreeDragPreview,
  fileTreeDropItems,
  fileTreeDropTargetFrom,
  fileTreeTerminalText,
  insertFileTreeReferences,
  mainComposerFileTreeDropTarget,
} from './fileTreeDrop.js';

function test(name, fn) {
  try { fn(); console.log('ok -', name); }
  catch (error) { console.error('not ok -', name); throw error; }
}

function source(path) {
  return readFileSync(fileURLToPath(new URL(path, import.meta.url)), 'utf8');
}

const entries = [
  { path: 'src/app', name: 'app', directory: true },
  { path: 'docs/设计 文档.md', name: '设计 文档.md', directory: false },
];
const items = fileTreeDropItems(entries, 'C:\\repo\\');

// 场景:把选中的行变成拖放携带的条目。
// 期望:同时带工作区相对路径和绝对路径(输入框用前者,终端用后者);没有路径的条目丢掉。
test('拖放条目带相对与绝对路径', () => {
  assert.deepEqual(items, [
    { path: 'src/app', absolutePath: 'C:/repo/src/app', name: 'app', directory: true },
    { path: 'docs/设计 文档.md', absolutePath: 'C:/repo/docs/设计 文档.md', name: '设计 文档.md', directory: false },
  ]);
  assert.deepEqual(fileTreeDropItems([{ path: '' }, null], 'C:/repo'), []);
});

// 场景:一次拖进两个条目(一个目录、一个带空格的文件),光标在「请看」后面。
// 期望:依次插入、各带一个空格分隔;目录以 / 结尾,带空格的路径加引号;光标停在最后一个引用之后。
test('在光标处依次插入多个 @路径 引用', () => {
  const result = insertFileTreeReferences('请看 之后', 2, items, { cwd: 'C:/repo', composerCwd: 'c:\\repo' });
  assert.equal(result.text, '请看 @src/app/ @"docs/设计 文档.md"  之后');
  assert.equal(result.count, 2);
  assert.equal(result.cursor, '请看 @src/app/ @"docs/设计 文档.md" '.length);
  assert.equal(insertFileTreeReferences('abc', 3, [], {}).count, 0);
});

// 场景:落点的输入框属于另一个工作区(相对路径在那边解析不到同一个文件)。
// 期望:改给绝对路径;同一个工作区(盘符大小写、斜杠方向不同也算同一个)仍给相对路径。
test('输入框不在同一个工作区时引用绝对路径', () => {
  const other = insertFileTreeReferences('', 0, items.slice(0, 1), { cwd: 'C:/repo', composerCwd: 'D:/elsewhere' });
  assert.equal(other.text, '@C:/repo/src/app/ ');
  const unknown = insertFileTreeReferences('', 0, items.slice(0, 1), { cwd: 'C:/repo', composerCwd: '' });
  assert.equal(unknown.text, '@src/app/ ');
});

// 场景:拖到控制台终端上。
// 期望:贴入绝对路径,空格分隔、末尾留一个空格;Windows 用反斜杠并给带空格的路径加双引号,
//       其它系统保持正斜杠、按 shell 规则加单引号。
test('终端里贴入加好引号的绝对路径', () => {
  assert.equal(
    fileTreeTerminalText(items, 'windows'),
    'C:\\repo\\src\\app "C:\\repo\\docs\\设计 文档.md" ',
  );
  const posix = fileTreeDropItems(entries, '/home/me/repo');
  assert.equal(
    fileTreeTerminalText(posix, 'linux'),
    "/home/me/repo/src/app '/home/me/repo/docs/设计 文档.md' ",
  );
  assert.equal(fileTreeTerminalText([], 'linux'), '');
});

// 场景:拖动途中判断指针下面是不是可以放下的地方。
// 期望:输入框(带 data-file-tree-drop="true")和当前显示的终端页签算落点;
//       被隐藏的终端页签、盖在上面的菜单 / 弹层、普通区域都不算。
test('落点只认输入框和当前显示的终端', () => {
  const composer = { id: 'composer' };
  const terminal = { id: 'terminal', style: { display: 'block' } };
  const hidden = { id: 'hidden', style: { display: 'none' } };
  const within = found => ({ closest: selector => found[selector] || null });

  assert.deepEqual(
    fileTreeDropTargetFrom(within({ '[data-file-tree-drop="true"]': composer })),
    { kind: 'composer', element: composer },
  );
  assert.deepEqual(
    fileTreeDropTargetFrom(within({ '.ace-console-term[data-tab-id]': terminal })),
    { kind: 'console', element: terminal },
  );
  assert.equal(fileTreeDropTargetFrom(within({ '.ace-console-term[data-tab-id]': hidden })), null);
  assert.equal(fileTreeDropTargetFrom(within({
    '[data-ace-native-overlay]': { id: 'menu' },
    '[data-file-tree-drop="true"]': composer,
  })), null);
  assert.equal(fileTreeDropTargetFrom(within({})), null);
  assert.equal(fileTreeDropTargetFrom(null), null);
});

// 场景:松手(或右键「添加到会话」)时派发事件,由落点所在的一方认领。
// 期望:认领后返回 true;没人认领、没有落点、没有条目都返回 false,调用方据此提示「输入框当前不可用」。
test('事件派发与认领', () => {
  const composer = { id: 'composer' };
  const listeners = [];
  const win = {
    CustomEvent: class { constructor(type, init) { this.type = type; this.detail = init.detail; } },
    dispatchEvent(event) { listeners.forEach(fn => fn(event)); return true; },
  };
  listeners.push((event) => {
    assert.equal(event.type, FILE_TREE_DROP_EVENT);
    assert.equal(event.detail.cwd, 'C:/repo');
    if (event.detail.kind === 'composer' && event.detail.target === composer) event.detail.handled = true;
  });
  const target = { kind: 'composer', element: composer };
  assert.equal(dispatchFileTreeDrop(target, items, { cwd: 'C:/repo', clientX: 1, clientY: 2 }, win), true);
  assert.equal(dispatchFileTreeDrop({ kind: 'console', element: {} }, items, { cwd: 'C:/repo' }, win), false);
  assert.equal(dispatchFileTreeDrop(null, items, { cwd: 'C:/repo' }, win), false);
  assert.equal(dispatchFileTreeDrop(target, [], { cwd: 'C:/repo' }, win), false);

  // 右键菜单没有落点:找主输入框;它被禁用(不带属性)时找不到。
  const doc = { querySelector: selector => (selector === '[data-main-composer="true"][data-file-tree-drop="true"]' ? composer : null) };
  assert.deepEqual(mainComposerFileTreeDropTarget(doc), target);
  assert.equal(mainComposerFileTreeDropTarget({ querySelector: () => null }), null);
});

// 场景:一次拖几十个文件。期望:预览只列前几个名字,总数交给角标。
test('拖动预览只列前几个名字', () => {
  const many = Array.from({ length: 12 }, (_, index) => ({ path: `f${index}.txt`, name: `f${index}.txt`, directory: false }));
  const preview = fileTreeDragPreview(many);
  assert.equal(preview.count, 12);
  assert.equal(preview.rows.length, FILE_TREE_DRAG_PREVIEW_LIMIT);
  assert.equal(preview.rows[0].name, 'f0.txt');
});

// 场景:把文件拖向贴着窗口底边的输入框 / 控制台,或拖到窗口右边缘。
// 期望:平时预览在指针右下方;下方放不下就翻到指针上方,右边放不下就收回窗口内。
// 回归点:最初固定放在右下方,拖到输入框上时预览有一半在窗口外面。
test('拖动预览不会被窗口边缘切掉', () => {
  const size = { width: 120, height: 60, viewportWidth: 800, viewportHeight: 600 };
  assert.deepEqual(fileTreeDragGhostPosition({ x: 100, y: 100 }, size), { left: 112, top: 114 });
  assert.deepEqual(fileTreeDragGhostPosition({ x: 100, y: 570 }, size), { left: 112, top: 500 });
  assert.deepEqual(fileTreeDragGhostPosition({ x: 790, y: 100 }, size), { left: 676, top: 114 });
  // 还没量到尺寸 / 窗口大小时退回右下方,不出 NaN。
  assert.deepEqual(fileTreeDragGhostPosition({ x: 10, y: 20 }), { left: 22, top: 34 });
});

// 接线守卫:文件树的行要声明右键委托、选中状态并接上点击 / 按下;输入框与控制台要认领放下事件。
// 任何一环断开,多选就会退化成「选得上、但右键和拖动没反应」。
test('文件树、输入框与控制台三处接线', () => {
  const sidePanel = source('../components/SidePanel.jsx');
  assert.match(sidePanel, /data-desktop-context-menu-delegate=\{FILE_TREE_SELECTION_DELEGATE\}/);
  assert.match(sidePanel, /data-file-multi-selected=\{selection\.keys\.has\(e\.path\) \? 'true' : undefined\}/);
  assert.match(sidePanel, /if \(!selection\.click\(event, rowEntry\)\) return;/);
  assert.match(sidePanel, /onPointerDown=\{\(event\) => selection\.pointerDown\(event, rowEntry\)\}/);
  assert.match(sidePanel, /onKeyDown=\{selection\.keyDown\}/);

  const hook = source('./useFileTreeSelection.js');
  assert.match(hook, /window\.addEventListener\(CONTEXT_MENU_DELEGATE_EVENT, handleContextMenuDelegate\)/);
  assert.match(hook, /fileTreeDropTargetAt\(doc, moveEvent\.clientX, moveEvent\.clientY\)/);
  assert.match(hook, /dispatchFileTreeDrop\(target, items,/);

  const inputBar = source('../components/InputBar.jsx');
  assert.match(inputBar, /data-file-tree-drop=\{disabled \? undefined : 'true'\}/);
  assert.match(inputBar, /window\.addEventListener\(FILE_TREE_DROP_EVENT, acceptFileTreeDrop\)/);
  assert.match(inputBar, /insertFileTreeReferences\(value, caret, detail\.items,/);

  const consoleDock = source('../components/ConsoleDock.jsx');
  assert.match(consoleDock, /window\.addEventListener\(FILE_TREE_DROP_EVENT, onDrop\)/);
  assert.match(consoleDock, /injectTextToTab\(tabId, fileTreeTerminalText\(detail\.items, HOST_OS\)\)/);
});

console.log('fileTreeDrop tests passed');
