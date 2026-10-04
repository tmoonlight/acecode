import assert from 'node:assert/strict';
import { fileBreadcrumbs, breadcrumbDirectoryEntries } from './fileBreadcrumbs.js';

function run(name, fn) {
  fn();
  console.log(`  PASS fileBreadcrumbs: ${name}`);
}

run('directory segments browse their parent and file segment browses siblings', () => {
  const model = fileBreadcrumbs({ workspaceCwd: '/repo', path: 'web/src/index.html' });
  assert.equal(model.root, '/repo');
  assert.deepEqual(model.crumbs.map(({ label, directory, selectedPath }) => [label, directory, selectedPath]), [
    ['repo', '', 'web'], ['web', '', 'web'], ['src', 'web', 'web/src'],
    ['index.html', 'web/src', 'web/src/index.html'],
  ]);
});

run('Windows case, extended paths, UNC roots and root-level files', () => {
  for (const [workspaceCwd, path] of [
    ['C:\\Repo', 'c:\\repo\\README.md'],
    ['\\\\?\\C:\\Repo', '\\\\?\\C:\\Repo\\README.md'],
    ['\\\\server\\Share\\Repo', '\\\\SERVER\\share\\repo\\README.md'],
    ['\\\\?\\UNC\\server\\Share\\Repo', '\\\\server\\share\\repo\\README.md'],
    ['C:/', 'C:/README.md'], ['/', '/README.md'],
  ]) {
    const model = fileBreadcrumbs({ workspaceCwd, path });
    assert.equal(model?.relativePath, 'README.md', workspaceCwd);
    assert.equal(model.crumbs.length, 2);
  }
});

run('external, sibling-prefix, missing-workspace and POSIX case boundaries', () => {
  for (const args of [
    { workspaceCwd: '/repo', cwd: '/tmp', path: 'image.png' },
    { workspaceCwd: '/repo', path: '/repo-other/a.txt' },
    { workspaceCwd: '/repo', path: '/Repo/a.txt' },
    { cwd: '/tmp/artifacts', path: 'a.xlsx' },
    { workspaceCwd: 'C:/repo', path: 'D:/repo/a.txt' },
    { workspaceCwd: '/repo', path: '/repo' },
    { workspaceCwd: '/repo', path: '' },
  ]) assert.equal(fileBreadcrumbs(args), null, JSON.stringify(args));
});

run('normalizes dot segments before testing containment', () => {
  assert.equal(fileBreadcrumbs({ workspaceCwd: '/repo', path: '../outside/a.txt' }), null);
  assert.equal(fileBreadcrumbs({ workspaceCwd: '/repo', path: '/repo/../other/a.txt' }), null);
  assert.equal(fileBreadcrumbs({ workspaceCwd: '/repo', path: 'web/../a.txt' }).relativePath, 'a.txt');
});

run('worktree navigation stays in the actual execution root for every format', () => {
  for (const path of ['a.cpp', 'a.md', 'a.png', 'a.pdf', 'a.docx', 'a.xlsx', 'a.pptx']) {
    const model = fileBreadcrumbs({ workspaceCwd: '/trees/feature', cwd: '/trees/feature', path });
    assert.equal(model.crumbs[0].label, 'feature');
    assert.equal(model.relativePath, path);
  }
  assert.equal(fileBreadcrumbs({ workspaceCwd: '/trees/feature', cwd: '/repo', path: 'a.cpp' }), null);
});

run('lists directories first with natural ordering and excludes invalid names', () => {
  const entries = breadcrumbDirectoryEntries([
    { name: 'file10.txt', kind: 'file', path: '/outside' },
    { name: 'file2.txt', kind: 'file' }, { name: 'web', kind: 'dir' },
    { name: '..', kind: 'dir' }, { name: '../bad', kind: 'file' },
    { name: 'bad\\path', kind: 'file' }, { name: 'device', kind: 'other' },
  ], 'src');
  assert.deepEqual(entries.map((entry) => entry.path), ['src/web', 'src/file2.txt', 'src/file10.txt']);
});
