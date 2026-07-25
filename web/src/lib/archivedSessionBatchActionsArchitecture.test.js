import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const srcRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

function source(relativePath) {
  return fs.readFileSync(path.join(srcRoot, relativePath), 'utf8');
}

function between(text, start, end) {
  const startIndex = text.indexOf(start);
  const endIndex = text.indexOf(end, startIndex);
  assert.notEqual(startIndex, -1, `missing start marker: ${start}`);
  assert.notEqual(endIndex, -1, `missing end marker: ${end}`);
  return text.slice(startIndex, endIndex);
}

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

const settingsFile = source('components/SettingsPage.jsx');
const archivedSection = between(
  settingsFile,
  'function SectionArchived()',
  '// ─── 使用情况',
);

run('archived footer exposes exactly three ordered batch actions', () => {
  const actionStart = archivedSection.indexOf(
    '<div className="mt-3 flex flex-wrap items-center gap-2">',
  );
  const actionEnd = archivedSection.indexOf('</div>', actionStart);
  assert.ok(actionStart >= 0);
  assert.ok(actionEnd > actionStart);
  const actions = archivedSection.slice(actionStart, actionEnd);

  assert.equal((actions.match(/<button\b/g) || []).length, 3);
  assert.ok(actions.indexOf("{allSelected ? '全不选' : '全选'}") >= 0);
  assert.ok(
    actions.indexOf("{allSelected ? '全不选' : '全选'}")
      < actions.indexOf('取消选中会话的归档'),
  );
  assert.ok(
    actions.indexOf('取消选中会话的归档')
      < actions.indexOf('删除选中会话'),
  );
  assert.match(actions, /onClick=\{toggleAllSelected\}/);
  assert.match(actions, /onClick=\{unarchiveSelected\}/);
  assert.match(actions, /onClick=\{purgeSelected\}/);
  assert.match(actions, /disabled=\{operationBusy\}/);
  assert.match(
    actions,
    /disabled=\{selectedItems\.length === 0 \|\| operationBusy\}/,
  );
  assert.doesNotMatch(actions, /删除选中的所有会话/);
});

run('batch unarchive settles per workspace and retains failed selection', () => {
  const unarchive = between(
    archivedSection,
    'const unarchiveArchivedItems',
    'const purgeArchivedItems',
  );

  assert.match(unarchive, /Promise\.allSettled/);
  assert.match(unarchive, /api\.unarchiveWorkspaceSession/);
  assert.match(unarchive, /api\.unarchiveSession/);
  assert.match(unarchive, /removeArchivedSessionsByKey\(previous, succeeded\)/);
  assert.match(unarchive, /removeSelectionKeys\(succeeded\)/);
  assert.match(unarchive, /new Event\('ace-session-archive-changed'\)/);
  assert.match(unarchive, /failedCount/);
  assert.match(unarchive, /void unarchiveArchivedItems\(\[\.\.\.selectedItems\], \{ batch: true \}\)/);
});

run('archived operations share busy guards and deletion keeps the shared Modal', () => {
  assert.match(archivedSection, /const \[unarchivingKeys, setUnarchivingKeys\]/);
  assert.match(
    archivedSection,
    /const operationBusy = unarchivingKeys\.size > 0 \|\| deletingKeys\.size > 0/,
  );
  assert.match(archivedSection, /const busy = unarchiving \|\| deleting/);
  assert.match(
    archivedSection,
    /<Modal onClose=\{\(\) => setPurgeConfirmation\(null\)\}/,
  );
  assert.doesNotMatch(archivedSection, /window\.confirm|window\.alert/);
});
