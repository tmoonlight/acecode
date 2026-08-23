import assert from 'node:assert/strict';
import {
  buildReviewStatusMap,
  entriesWithReviewStatuses,
  normalizeTreePath,
  normalizeWorkspaceRelativePath,
  reviewStatusForGroup,
  statusForTreeEntry,
} from './fileTreeChangeStatus.js';

function run(name, fn) {
  try {
    fn();
    console.log('ok - ' + name);
  } catch (err) {
    console.error('not ok - ' + name);
    throw err;
  }
}

run('reviewStatusForGroup: 空到非空标记为 U', () => {
  assert.equal(reviewStatusForGroup({
    file: '333.txt',
    totalAdditions: 1,
    totalDeletions: 0,
    hunks: [{ old_count: 0, new_count: 1 }],
  }), 'U');
});

run('reviewStatusForGroup: 非空到空标记为 D', () => {
  assert.equal(reviewStatusForGroup({
    file: 'old.txt',
    totalAdditions: 0,
    totalDeletions: 2,
    hunks: [{ old_count: 2, new_count: 0 }],
  }), 'D');
});

run('reviewStatusForGroup: 普通编辑标记为 M', () => {
  assert.equal(reviewStatusForGroup({
    file: 'main.cpp',
    totalAdditions: 2,
    totalDeletions: 1,
    hunks: [{ old_count: 3, new_count: 4 }],
  }), 'M');
});

run('statusForTreeEntry: 文件 exact match,目录聚合子级状态', () => {
  const statuses = buildReviewStatusMap([
    { file: 'src/deep/main.cpp', totalAdditions: 1, totalDeletions: 1, hunks: [{ old_count: 1, new_count: 1 }] },
    { file: 'src/new.txt', totalAdditions: 1, totalDeletions: 0, hunks: [{ old_count: 0, new_count: 1 }] },
  ]);

  assert.equal(statusForTreeEntry({ path: 'src', kind: 'dir' }, statuses), 'M');
  assert.equal(statusForTreeEntry({ path: 'src/new.txt', kind: 'file' }, statuses), 'U');
});

run('normalizeTreePath: 定位路径与文件树路径使用同一形态', () => {
  assert.equal(normalizeTreePath('.\\src\\deep\\main.cpp'), 'src/deep/main.cpp');
  assert.equal(normalizeTreePath('./src//deep/main.cpp/'), 'src/deep/main.cpp');
});

run('normalizeWorkspaceRelativePath: 绝对路径按 cwd 裁成文件树相对路径', () => {
  assert.equal(
    normalizeWorkspaceRelativePath('C:\\Users\\shao\\acecode\\web\\src\\InputBar.jsx', 'c:/users/shao/acecode'),
    'web/src/InputBar.jsx',
  );
  assert.equal(
    normalizeWorkspaceRelativePath('\\\\?\\C:\\Users\\shao\\acecode\\src\\main.cpp', 'C:/Users/shao/acecode'),
    'src/main.cpp',
  );
});

run('buildReviewStatusMap: 绝对路径变更状态映射到相对文件树路径', () => {
  const statuses = buildReviewStatusMap([
    { file: 'C:/repo/src/deep/main.cpp', totalAdditions: 1, totalDeletions: 1, hunks: [{ old_count: 1, new_count: 1 }] },
  ], 'c:/repo');

  assert.equal(statuses.get('src/deep/main.cpp'), 'M');
});

run('entriesWithReviewStatuses: 只标记当前文件列表中的条目,不补历史文件', () => {
  const statuses = buildReviewStatusMap([
    { file: 'Editor161_export.ps1', totalAdditions: 1, totalDeletions: 0, hunks: [{ old_count: 0, new_count: 1 }] },
    { file: 'Editor161_zip.ps1', totalAdditions: 1, totalDeletions: 0, hunks: [{ old_count: 0, new_count: 1 }] },
    { file: 'gone.txt', totalAdditions: 0, totalDeletions: 1, hunks: [{ old_count: 1, new_count: 0 }] },
    { file: 'present.txt', totalAdditions: 1, totalDeletions: 1, hunks: [{ old_count: 1, new_count: 1 }] },
  ]);

  const visible = entriesWithReviewStatuses([{
    name: 'present.txt',
    path: 'present.txt',
    kind: 'file',
  }, {
    name: 'unchanged.txt',
    path: 'unchanged.txt',
    kind: 'file',
  }], statuses);
  assert.deepEqual(visible, [{
    name: 'present.txt',
    path: 'present.txt',
    kind: 'file',
    review_status: 'M',
  }, {
    name: 'unchanged.txt',
    path: 'unchanged.txt',
    kind: 'file',
  }]);
  assert.deepEqual(entriesWithReviewStatuses([], statuses), []);
});
