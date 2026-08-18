import assert from 'node:assert/strict';
import { fileTreeLocatePlan, pathAncestors } from './fileTreeLocate.js';

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
  assert.deepEqual(pathAncestors('AGENT.md'), []);
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
