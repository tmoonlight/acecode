import assert from 'node:assert/strict';
import {
  CHANGE_LIST_VIEW_FLAT,
  CHANGE_LIST_VIEW_TREE,
  DEFAULT_CHANGE_LIST_VIEW,
  DEFAULT_CHANGE_LIST_VIEW_BY_CWD,
  buildChangeFileTree,
  changeListViewForCwd,
  changeTreeAncestorPaths,
  effectiveChangeListView,
  flattenVisibleChangeTree,
  normalizeChangeListViewCwd,
  normalizeChangeTreePath,
  updateChangeListViewForCwd,
  validateChangeListView,
  validateChangeListViewByCwd,
} from './changeFileTree.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

function child(node, name, type) {
  return node.find((entry) => entry.name === name && entry.type === type);
}

run('Changes view preference accepts flat/tree and falls back to tree', () => {
  assert.equal(validateChangeListView(CHANGE_LIST_VIEW_FLAT), true);
  assert.equal(validateChangeListView(CHANGE_LIST_VIEW_TREE), true);
  assert.equal(validateChangeListView('grid'), false);
  assert.equal(DEFAULT_CHANGE_LIST_VIEW, CHANGE_LIST_VIEW_TREE);
  assert.equal(effectiveChangeListView('grid'), DEFAULT_CHANGE_LIST_VIEW);
  assert.equal(effectiveChangeListView(CHANGE_LIST_VIEW_TREE), CHANGE_LIST_VIEW_TREE);
});

run('Changes view preference validates cwd-keyed persisted values', () => {
  assert.equal(validateChangeListViewByCwd(DEFAULT_CHANGE_LIST_VIEW_BY_CWD), true);
  assert.equal(validateChangeListViewByCwd({ 'c:/repo': CHANGE_LIST_VIEW_FLAT }), true);
  assert.equal(validateChangeListViewByCwd({ 'c:/repo': 'grid' }), false);
  assert.equal(validateChangeListViewByCwd([]), false);
  assert.equal(validateChangeListViewByCwd(null), false);
});

run('Changes view preference normalizes equivalent Windows cwd spellings', () => {
  assert.equal(normalizeChangeListViewCwd('C:\\Repo\\Project\\'), 'c:/repo/project');
  assert.equal(normalizeChangeListViewCwd('c:/repo//project'), 'c:/repo/project');
  assert.equal(normalizeChangeListViewCwd('\\\\Server\\Share\\Repo\\'), '//server/share/repo');
  assert.equal(normalizeChangeListViewCwd('/Work/Repo/'), '/Work/Repo');
});

run('Changes view preference keeps working-directory selections isolated', () => {
  let preferences = DEFAULT_CHANGE_LIST_VIEW_BY_CWD;
  assert.equal(changeListViewForCwd(preferences, 'C:\\Repo'), CHANGE_LIST_VIEW_TREE);
  assert.equal(changeListViewForCwd(preferences, 'D:\\Other'), CHANGE_LIST_VIEW_TREE);

  preferences = updateChangeListViewForCwd(
    preferences,
    'C:\\Repo\\',
    CHANGE_LIST_VIEW_FLAT,
  );
  assert.equal(changeListViewForCwd(preferences, 'c:/repo'), CHANGE_LIST_VIEW_FLAT);
  assert.equal(changeListViewForCwd(preferences, 'D:\\Other'), CHANGE_LIST_VIEW_TREE);

  preferences = updateChangeListViewForCwd(
    preferences,
    'D:\\Other',
    CHANGE_LIST_VIEW_TREE,
  );
  const samePreferences = updateChangeListViewForCwd(
    preferences,
    'd:/other/',
    CHANGE_LIST_VIEW_TREE,
  );
  assert.equal(samePreferences, preferences);
  assert.deepEqual(preferences, {
    'c:/repo': CHANGE_LIST_VIEW_FLAT,
    'd:/other': CHANGE_LIST_VIEW_TREE,
  });
});

run('Changes view default follows workspace context while saved choices win', () => {
  const noSavedPreference = DEFAULT_CHANGE_LIST_VIEW_BY_CWD;
  assert.equal(
    changeListViewForCwd(noSavedPreference, '', CHANGE_LIST_VIEW_FLAT),
    CHANGE_LIST_VIEW_FLAT,
  );
  assert.equal(
    changeListViewForCwd(noSavedPreference, 'C:\\Repo', CHANGE_LIST_VIEW_TREE),
    CHANGE_LIST_VIEW_TREE,
  );

  const savedPreferences = {
    '': CHANGE_LIST_VIEW_TREE,
    'c:/repo': CHANGE_LIST_VIEW_FLAT,
  };
  assert.equal(
    changeListViewForCwd(savedPreferences, '', CHANGE_LIST_VIEW_FLAT),
    CHANGE_LIST_VIEW_TREE,
  );
  assert.equal(
    changeListViewForCwd(savedPreferences, 'C:\\Repo', CHANGE_LIST_VIEW_TREE),
    CHANGE_LIST_VIEW_FLAT,
  );
});

run('buildChangeFileTree groups nested and root files without losing rows', () => {
  const rows = [
    { path: 'README.md', status: 'M' },
    { path: 'src/app/main.cpp', status: 'A' },
    { path: 'src/lib/util.cpp', status: 'D' },
  ];
  const tree = buildChangeFileTree(rows);
  const src = child(tree, 'src', 'directory');
  assert.ok(src);
  assert.equal(child(tree, 'README.md', 'file').row, rows[0]);
  assert.equal(child(child(src.children, 'app', 'directory').children, 'main.cpp', 'file').row, rows[1]);
  assert.equal(child(child(src.children, 'lib', 'directory').children, 'util.cpp', 'file').path, 'src/lib/util.cpp');
});

run('workspace absolute Windows paths become relative display paths only', () => {
  const row = { path: 'C:\\repo\\src\\main.cpp', status: 'M' };
  assert.equal(normalizeChangeTreePath(row.path, 'c:/repo'), 'src/main.cpp');
  const tree = buildChangeFileTree([row], 'c:/repo');
  const file = child(child(tree, 'src', 'directory').children, 'main.cpp', 'file');
  assert.equal(file.path, row.path);
  assert.equal(file.row, row);
});

run('tree sorting is directory-first and case-insensitive while same names survive', () => {
  const tree = buildChangeFileTree([
    { path: 'z.txt' },
    { path: 'Beta/file.txt' },
    { path: 'alpha/file.txt' },
    { path: 'alpha' },
  ]);
  assert.deepEqual(
    tree.map((node) => `${node.type}:${node.name}`),
    ['directory:alpha', 'directory:Beta', 'file:alpha', 'file:z.txt'],
  );
});

run('empty paths retain a stable usable file label', () => {
  const row = { path: '', status: 'M' };
  const tree = buildChangeFileTree([row]);
  assert.equal(tree.length, 1);
  assert.equal(tree[0].type, 'file');
  assert.equal(tree[0].name, '未命名文件 1');
  assert.equal(tree[0].row, row);
});

run('flattenVisibleChangeTree hides only collapsed descendants', () => {
  const tree = buildChangeFileTree([
    { path: 'src/app/main.cpp' },
    { path: 'src/lib/util.cpp' },
    { path: 'README.md' },
  ]);
  const all = flattenVisibleChangeTree(tree);
  assert.deepEqual(all.map(({ node, depth }) => `${depth}:${node.name}`), [
    '0:src',
    '1:app',
    '2:main.cpp',
    '1:lib',
    '2:util.cpp',
    '0:README.md',
  ]);
  const collapsed = flattenVisibleChangeTree(tree, new Set(['src/app']));
  assert.equal(collapsed.some(({ node }) => node.name === 'main.cpp'), false);
  assert.equal(collapsed.some(({ node }) => node.name === 'util.cpp'), true);
});

run('changeTreeAncestorPaths returns the selected file directory chain', () => {
  assert.deepEqual(
    changeTreeAncestorPaths('C:\\repo\\src\\app\\main.cpp', 'c:/repo'),
    ['src', 'src/app'],
  );
});

run('single-child directory chains compact without consuming the file leaf', () => {
  const tree = buildChangeFileTree([{ path: 'a/b/c/file.ts' }]);
  assert.equal(tree.length, 1);
  assert.equal(tree[0].type, 'directory');
  assert.equal(tree[0].name, 'a/b/c');
  assert.equal(tree[0].path, 'a/b/c');
  assert.equal(tree[0].key, 'directory:a/b/c');
  assert.deepEqual(tree[0].segments, ['a', 'b', 'c']);
  assert.deepEqual(
    tree[0].children.map((node) => `${node.type}:${node.name}`),
    ['file:file.ts'],
  );
  assert.deepEqual(
    flattenVisibleChangeTree(tree).map(({ node, depth }) => `${depth}:${node.name}`),
    ['0:a/b/c', '1:file.ts'],
  );
});

run('directory compaction stops at changed-path branches', () => {
  const tree = buildChangeFileTree([
    { path: 'a/b/c/file.ts' },
    { path: 'a/b/d/other.ts' },
  ]);
  assert.equal(tree.length, 1);
  assert.equal(tree[0].name, 'a/b');
  assert.equal(tree[0].path, 'a/b');
  assert.deepEqual(
    tree[0].children.map((node) => `${node.type}:${node.name}`),
    ['directory:c', 'directory:d'],
  );
});

run('directory compaction stops before a direct changed file', () => {
  const tree = buildChangeFileTree([
    { path: 'a/root.ts' },
    { path: 'a/b/deep.ts' },
  ]);
  assert.equal(tree.length, 1);
  assert.equal(tree[0].name, 'a');
  assert.deepEqual(
    tree[0].children.map((node) => `${node.type}:${node.name}`),
    ['directory:b', 'file:root.ts'],
  );
});

run('Unicode rows create only real changed-path nodes', () => {
  const tree = buildChangeFileTree([{ path: '项目/文档/待办.md' }]);
  assert.deepEqual(tree.map((node) => node.name), ['项目/文档']);
  assert.deepEqual(tree[0].segments, ['项目', '文档']);
  assert.equal(tree[0].children[0].name, '待办.md');
  assert.equal(
    flattenVisibleChangeTree(tree).some(({ node }) => /^\d{3}$/.test(node.name)),
    false,
  );
});

run('compact endpoint paths drive collapse and selected-file expansion', () => {
  const tree = buildChangeFileTree([{ path: 'a/b/c/file.ts' }]);
  const collapsed = flattenVisibleChangeTree(tree, new Set(['a/b/c']));
  assert.deepEqual(collapsed.map(({ node }) => node.name), ['a/b/c']);
  assert.deepEqual(
    changeTreeAncestorPaths('a/b/c/file.ts'),
    ['a', 'a/b', 'a/b/c'],
  );
});

console.log('changeFileTree.test.js: all tests passed');
