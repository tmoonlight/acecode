import assert from 'node:assert/strict';
import { ApiError } from './api.js';
import {
  editableFileConflict,
  editableFileError,
  editableStatePatch,
  saveEditableFileDraft,
  saveEditableFileDraftBatch,
} from './editableFileDraft.js';

async function run(name, fn) {
  try {
    await fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

await run('editable state patch keeps the editor open and establishes a clean baseline', () => {
  const patch = editableStatePatch({
    text: 'const answer = 42;\n',
    read_id: 'read-2',
    encoding: 'utf-8',
    line_ending: 'lf',
    has_bom: false,
    size: 19,
  });
  assert.equal(patch.editing, true);
  assert.equal(patch.text, patch.baselineText);
  assert.equal(patch.readId, 'read-2');
  assert.equal(patch.saving, false);
  assert.equal(patch.error, '');
});

await run('saving a draft uses its version and returns a clean editor patch', async () => {
  const calls = [];
  const api = {
    async saveEditableFile(cwd, path, text, readId) {
      calls.push({ cwd, path, text, readId });
      return { read_id: 'read-2', encoding: 'utf-8', line_ending: 'lf', size: 5 };
    },
  };
  const saved = await saveEditableFileDraft(api, {
    cwd: 'C:/repo',
    path: 'a.js',
    edit: { baselineText: 'old', text: 'newer', readId: 'read-1' },
  });
  assert.deepEqual(calls, [{ cwd: 'C:/repo', path: 'a.js', text: 'newer', readId: 'read-1' }]);
  assert.equal(saved.patch.text, 'newer');
  assert.equal(saved.patch.baselineText, 'newer');
  assert.equal(saved.patch.readId, 'read-2');
  assert.equal(saved.patch.editing, true);
});

await run('conflicts have a stable message and are detectable by close-save flow', () => {
  const error = new ApiError(409, { error: 'file changed' });
  assert.equal(editableFileConflict(error), true);
  assert.match(editableFileError(error, '保存失败'), /磁盘内容已变化/);
});

await run('batch close-save stops at the first failure and reports earlier successful saves', async () => {
  const calls = [];
  const savedTabs = [];
  const conflict = new ApiError(409, { error: 'file changed' });
  const api = {
    async saveEditableFile(cwd, path) {
      calls.push({ cwd, path });
      if (path === 'b.js') throw conflict;
      return { read_id: `saved-${path}`, encoding: 'utf-8', line_ending: 'lf', size: 1 };
    },
  };
  const tabs = ['a.js', 'b.js', 'c.js'].map((path) => ({
    key: path,
    cwd: 'C:/repo',
    path,
    edit: { text: path, readId: `read-${path}` },
  }));
  const result = await saveEditableFileDraftBatch(api, {
    tabs,
    onSaved: (tab, saved) => savedTabs.push({ key: tab.key, patch: saved.patch }),
  });
  assert.equal(result.ok, false);
  assert.equal(result.savedCount, 1);
  assert.equal(result.tab.key, 'b.js');
  assert.equal(result.error, conflict);
  assert.deepEqual(calls.map((call) => call.path), ['a.js', 'b.js']);
  assert.deepEqual(savedTabs.map((item) => item.key), ['a.js']);
  assert.equal(savedTabs[0].patch.baselineText, 'a.js');
});
