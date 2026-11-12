import assert from 'node:assert/strict';
import {
  FILE_TREE_BATCH_ACTIONS,
  emptyFileTreeSelection,
  fileTreeBatchMenu,
  followActiveFileTreeRow,
  outermostFileTreeEntries,
  pruneFileTreeSelection,
  selectAllFileTreeRows,
  selectFileTreeRow,
  selectedFileTreeEntries,
  toggleFileTreeDirectories,
  visibleFileTreeEntries,
} from './fileTreeSelection.js';

function test(name, fn) {
  try { fn(); console.log('ok -', name); }
  catch (error) { console.error('not ok -', name); throw error; }
}

// 一棵小树:根下 docs/(已展开,含 a.md、sub/)、src/(未展开)、README.md。
// sub/ 没展开,但缓存里有它的子项 —— 用来确认「缓存里有」不等于「画出来了」。
const treeCache = new Map([
  ['', [
    { path: 'docs', name: 'docs', kind: 'dir' },
    { path: 'src', name: 'src', kind: 'dir' },
    { path: 'README.md', name: 'README.md', kind: 'file' },
  ]],
  ['docs', [
    { path: 'docs/sub', name: 'sub', kind: 'dir' },
    { path: 'docs/a.md', name: 'a.md', kind: 'file' },
  ]],
  ['docs/sub', [{ path: 'docs/sub/deep.md', name: 'deep.md', kind: 'file' }]],
  ['src', [{ path: 'src/main.js', name: 'main.js', kind: 'file' }]],
]);
const expanded = new Set(['docs']);
const rows = visibleFileTreeEntries(treeCache, expanded);
const paths = rows.map(row => row.path);

// 场景:Shift 选范围要按「屏幕上从上到下」的顺序算。
// 期望:只有展开的目录才把子项排进去,顺序与渲染一致;没展开的目录即使缓存过子项也不算。
test('可见行的顺序与渲染一致', () => {
  assert.deepEqual(paths, ['docs', 'docs/sub', 'docs/a.md', 'src', 'README.md']);
  assert.deepEqual(rows[0], { path: 'docs', name: 'docs', directory: true });
  assert.deepEqual(rows[2], { path: 'docs/a.md', name: 'a.md', directory: false });
  assert.deepEqual(visibleFileTreeEntries(new Map(), expanded), []);
  assert.deepEqual(visibleFileTreeEntries(null, null), []);
});

// 场景:依次普通点击、Shift 点击、Ctrl 点击。
// 期望:普通点击只选这一行并成为范围起点;Shift 选中起点到这一行之间的全部可见行;
//       Ctrl 逐个加减且不动其它行;Shift 之后起点不变,可以反方向重选。
test('Shift 选范围、Ctrl 逐个加减', () => {
  let state = selectFileTreeRow(emptyFileTreeSelection(), paths, 'docs/sub');
  assert.deepEqual([...state.keys], ['docs/sub']);
  assert.equal(state.anchor, 'docs/sub');

  state = selectFileTreeRow(state, paths, 'src', { shiftKey: true });
  assert.deepEqual([...state.keys], ['docs/sub', 'docs/a.md', 'src']);
  assert.equal(state.anchor, 'docs/sub');

  state = selectFileTreeRow(state, paths, 'docs', { shiftKey: true });
  assert.deepEqual([...state.keys], ['docs', 'docs/sub']);

  state = selectFileTreeRow(state, paths, 'README.md', { ctrlKey: true });
  assert.deepEqual([...state.keys], ['docs', 'docs/sub', 'README.md']);
  state = selectFileTreeRow(state, paths, 'docs', { metaKey: true });
  assert.deepEqual([...state.keys], ['docs/sub', 'README.md']);

  // 不在可见行里的路径(已被折叠或删除)不改变选区。
  assert.equal(selectFileTreeRow(state, paths, 'docs/sub/deep.md', { ctrlKey: true }), state);
});

// 场景:README.md 正在预览(行上有高亮),用户还没在文件树里点过任何行,直接 Ctrl / Shift 点别的行。
// 期望:看起来已经选中的预览文件算进选区 —— Ctrl 点 src 得到两行,Shift 点 docs/a.md 从预览文件拉范围。
// 回归点:不这样处理的话,用户看到两行高亮,右键却只对一行生效。
test('还没有选区时,预览中的文件算作已选', () => {
  let state = selectFileTreeRow(emptyFileTreeSelection(), paths, 'src', { ctrlKey: true }, 'README.md');
  assert.deepEqual([...state.keys].sort(), ['README.md', 'src']);

  state = selectFileTreeRow(emptyFileTreeSelection(), paths, 'docs/a.md', { shiftKey: true }, 'README.md');
  assert.deepEqual([...state.keys], ['docs/a.md', 'src', 'README.md']);
  assert.equal(state.anchor, 'README.md');

  // Ctrl 点预览文件自己:就是选中它,而不是「先算已选再取消」。
  state = selectFileTreeRow(emptyFileTreeSelection(), paths, 'README.md', { ctrlKey: true }, 'README.md');
  assert.deepEqual([...state.keys], ['README.md']);

  // 已经有选区时不再把预览文件掺进来。
  state = selectFileTreeRow({ anchor: 'docs', keys: new Set(['docs']) }, paths, 'src', { ctrlKey: true }, 'README.md');
  assert.deepEqual([...state.keys], ['docs', 'src']);

  // 普通点击不受预览文件影响。
  state = selectFileTreeRow(emptyFileTreeSelection(), paths, 'src', {}, 'README.md');
  assert.deepEqual([...state.keys], ['src']);
});

// 场景:选中 docs 下面的行之后把 docs 折叠,或刷新后文件被删。
// 期望:看不见的行从选区里去掉(批量操作不能落在看不见的行上);没有变化时返回原对象,不引起重渲染。
test('折叠 / 刷新后裁掉看不见的行', () => {
  const state = { anchor: 'docs/a.md', keys: new Set(['docs/a.md', 'src', 'README.md']) };
  const collapsed = visibleFileTreeEntries(treeCache, new Set()).map(row => row.path);
  const pruned = pruneFileTreeSelection(state, collapsed);
  assert.deepEqual([...pruned.keys], ['src', 'README.md']);
  assert.equal(pruned.anchor, '');
  assert.equal(pruneFileTreeSelection(state, paths), state);
});

// 场景:用户点预览页签或「在文件列表展示」,预览中的文件换了。
// 期望:只选了一行(或没选)时选区跟到新文件,旧行不再留着选中底色;已经多选时不打扰。
test('单选时选区跟随预览中的文件', () => {
  const single = { anchor: 'src', keys: new Set(['src']) };
  const followed = followActiveFileTreeRow(single, paths, 'README.md');
  assert.deepEqual([...followed.keys], ['README.md']);
  assert.equal(followed.anchor, 'README.md');
  assert.equal(followActiveFileTreeRow(followed, paths, 'README.md'), followed);

  const multiple = { anchor: 'src', keys: new Set(['src', 'docs']) };
  assert.equal(followActiveFileTreeRow(multiple, paths, 'README.md'), multiple);
  // 新文件在折叠的目录里(此刻看不见)或没有预览文件:不动。
  assert.equal(followActiveFileTreeRow(single, paths, 'docs/sub/deep.md'), single);
  assert.equal(followActiveFileTreeRow(single, paths, ''), single);
});

// 场景:Ctrl+A。期望:选中全部可见行;原来的范围起点还在就保留,空树不动。
test('全选可见行', () => {
  const state = selectAllFileTreeRows({ anchor: 'src', keys: new Set(['src']) }, paths);
  assert.deepEqual([...state.keys], paths);
  assert.equal(state.anchor, 'src');
  assert.equal(selectAllFileTreeRows(emptyFileTreeSelection(), paths).anchor, 'docs');
  const empty = emptyFileTreeSelection();
  assert.equal(selectAllFileTreeRows(empty, []), empty);
});

// 场景:Shift 一路选下来,把展开的 docs 目录和它里面的行都选上了,然后拖进输入框。
// 期望:引用 docs/ 已经包含里面的内容,只给最外层;名字只是前缀相同的兄弟(docs-old)不能被误删。
test('目录和它里面的行一起选中时只留最外层', () => {
  const selected = [
    { path: 'docs', name: 'docs', directory: true },
    { path: 'docs/sub', name: 'sub', directory: true },
    { path: 'docs/a.md', name: 'a.md', directory: false },
    { path: 'docs-old/b.md', name: 'b.md', directory: false },
    { path: 'README.md', name: 'README.md', directory: false },
  ];
  assert.deepEqual(outermostFileTreeEntries(selected).map(entry => entry.path), ['docs', 'docs-old/b.md', 'README.md']);
  assert.deepEqual(outermostFileTreeEntries([]), []);
});

// 场景:多选后右键。菜单每一项后面的数字是它实际处理的条数,不是选中的总数。
// 期望:添加到会话按最外层计数;预览只数文件;展开 / 折叠分别只数还没展开 / 已展开的目录;
//       没有文件就没有「预览」,没有目录就没有展开 / 折叠;只选一行不出批量菜单。
test('多选右键菜单的项目与数量', () => {
  const selected = selectedFileTreeEntries(rows, new Set(['docs', 'docs/a.md', 'src', 'README.md']));
  assert.deepEqual(selected.map(entry => entry.path), ['docs', 'docs/a.md', 'src', 'README.md']);
  const menu = fileTreeBatchMenu(selected, expanded);
  assert.deepEqual(menu.map(item => [item.action, item.count]), [
    [FILE_TREE_BATCH_ACTIONS.ADD_CONTEXT, 3],
    [FILE_TREE_BATCH_ACTIONS.PREVIEW, 2],
    [FILE_TREE_BATCH_ACTIONS.COPY_RELATIVE_PATHS, 4],
    [FILE_TREE_BATCH_ACTIONS.COPY_ABSOLUTE_PATHS, 4],
    [FILE_TREE_BATCH_ACTIONS.EXPAND, 1],
    [FILE_TREE_BATCH_ACTIONS.COLLAPSE, 1],
  ]);
  // 展开与折叠同属一组,只在组的第一项前面画分隔线。
  assert.deepEqual(menu.filter(item => item.separatorBefore).map(item => item.action), [
    FILE_TREE_BATCH_ACTIONS.COPY_RELATIVE_PATHS,
    FILE_TREE_BATCH_ACTIONS.EXPAND,
  ]);

  const filesOnly = fileTreeBatchMenu(selected.filter(entry => !entry.directory), expanded);
  assert.deepEqual(filesOnly.map(item => item.action), [
    FILE_TREE_BATCH_ACTIONS.ADD_CONTEXT,
    FILE_TREE_BATCH_ACTIONS.PREVIEW,
    FILE_TREE_BATCH_ACTIONS.COPY_RELATIVE_PATHS,
    FILE_TREE_BATCH_ACTIONS.COPY_ABSOLUTE_PATHS,
  ]);

  const expandedOnly = fileTreeBatchMenu(selected.filter(entry => entry.directory), new Set(['docs', 'src']));
  assert.deepEqual(expandedOnly.map(item => item.action), [
    FILE_TREE_BATCH_ACTIONS.ADD_CONTEXT,
    FILE_TREE_BATCH_ACTIONS.COPY_RELATIVE_PATHS,
    FILE_TREE_BATCH_ACTIONS.COPY_ABSOLUTE_PATHS,
    FILE_TREE_BATCH_ACTIONS.COLLAPSE,
  ]);
  assert.equal(expandedOnly.at(-1).separatorBefore, true);
  assert.deepEqual(fileTreeBatchMenu(selected.slice(0, 1), expanded), []);
});

// 场景:批量展开 / 折叠选中的目录。
// 期望:只动选中的目录本身(别的已展开目录保留);什么都没变时返回原集合,不触发一次空更新。
test('批量展开与折叠', () => {
  const opened = toggleFileTreeDirectories(expanded, ['src', 'docs'], true);
  assert.deepEqual([...opened].sort(), ['docs', 'src']);
  assert.equal(toggleFileTreeDirectories(opened, ['src'], true), opened);
  const closed = toggleFileTreeDirectories(new Set(['docs', 'docs/sub', 'src']), ['docs'], false);
  assert.deepEqual([...closed].sort(), ['docs/sub', 'src']);
  assert.equal(toggleFileTreeDirectories(expanded, ['src'], false), expanded);
});

console.log('fileTreeSelection tests passed');
