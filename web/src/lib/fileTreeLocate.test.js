import assert from 'node:assert/strict';
import { fileTreeLocatePlan, pathAncestors, previewFileLocatePlan } from './fileTreeLocate.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('pathAncestors lists parent directories without the leaf', () => {
  assert.deepEqual(pathAncestors('src/headless/headless_runner.cpp'), ['src', 'src/headless']);
  assert.deepEqual(pathAncestors('src/worktree'), ['src']);
  assert.deepEqual(pathAncestors('AGENTS.md'), []);
});

run('fileTreeLocatePlan expands ancestors for a file', () => {
  assert.deepEqual(fileTreeLocatePlan('src/headless/headless_runner.cpp'), {
    selectedPath: 'src/headless/headless_runner.cpp',
    expandedDirs: ['src', 'src/headless'],
  });
});

run('fileTreeLocatePlan can include the directory itself', () => {
  assert.deepEqual(fileTreeLocatePlan('src/worktree/', '', { includeSelf: true }), {
    selectedPath: 'src/worktree',
    expandedDirs: ['src', 'src/worktree'],
  });
});

run('fileTreeLocatePlan strips cwd from absolute directory paths', () => {
  assert.deepEqual(fileTreeLocatePlan(
    'C:\\Users\\shao\\acecode\\src\\headless\\',
    'C:/Users/shao/acecode',
    { includeSelf: true },
  ), {
    selectedPath: 'src/headless',
    expandedDirs: ['src', 'src/headless'],
  });
});

run('fileTreeLocatePlan returns null for empty paths', () => {
  assert.equal(fileTreeLocatePlan(''), null);
  assert.equal(fileTreeLocatePlan('C:/repo', 'C:/repo', { includeSelf: true }), null);
});

run('preview file locate uses the actual workspace and expands only ancestors', () => {
  assert.deepEqual(previewFileLocatePlan({ workspaceCwd: 'C:/worktree', cwd: 'c:/WORKTREE/src', path: './lib/../app.js' }), {
    selectedPath: 'src/app.js', expandedDirs: ['src'],
  });
  assert.deepEqual(previewFileLocatePlan({ workspaceCwd: 'C:/repo', path: 'README.md' }), {
    selectedPath: 'README.md', expandedDirs: [],
  });
  assert.deepEqual(previewFileLocatePlan({ workspaceCwd: '//server/share/repo', path: '\\\\?\\UNC\\SERVER\\share\\repo\\src\\a.js' }), {
    selectedPath: 'src/a.js', expandedDirs: ['src'],
  });
});

run('preview file locate rejects outside, sibling-prefix and missing workspaces', () => {
  for (const input of [
    { workspaceCwd: 'C:/repo', path: 'C:/repo-other/a.js' },
    { workspaceCwd: 'C:/repo', path: 'src/../../a.js' },
    { workspaceCwd: 'C:/repo', cwd: 'D:/outside', path: 'a.js' },
    { workspaceCwd: 'C:/worktree', cwd: 'C:/repo', path: 'a.js' },
    { workspaceCwd: '', cwd: 'C:/repo', path: 'a.js' },
    { workspaceCwd: '/repo', path: '/REPO/a.js' },
    { workspaceCwd: '//server/share/repo', path: '//server/other/repo/a.js' },
    { workspaceCwd: 'C:/repo', path: '' },
    { workspaceCwd: 'C:/repo', path: 'C:/repo' },
  ]) assert.equal(previewFileLocatePlan(input), null, JSON.stringify(input));
});
