import assert from 'node:assert/strict';
import {
  CHANGE_LIST_VIEW_FLAT,
  CHANGE_LIST_VIEW_TREE,
  DEFAULT_CHANGE_LIST_VIEW,
  buildChangeFileTree,
  changeTreeAncestorPaths,
  effectiveChangeListView,
  flattenVisibleChangeTree,
  normalizeChangeTreePath,
  validateChangeListView,
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

run('Changes view preference accepts flat/tree and falls back to flat', () => {
  assert.equal(validateChangeListView(CHANGE_LIST_VIEW_FLAT), true);
  assert.equal(validateChangeListView(CHANGE_LIST_VIEW_TREE), true);
  assert.equal(validateChangeListView('grid'), false);
  assert.equal(effectiveChangeListView('grid'), DEFAULT_CHANGE_LIST_VIEW);
  assert.equal(effectiveChangeListView(CHANGE_LIST_VIEW_TREE), CHANGE_LIST_VIEW_TREE);
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

console.log('changeFileTree.test.js: all tests passed');
