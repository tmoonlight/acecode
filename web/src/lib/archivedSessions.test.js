import assert from 'node:assert/strict';
import {
  allArchivedSessionsSelected,
  archivedSessionKey,
  archivedSessionTarget,
  archivedSessionWorkspace,
  groupArchivedSessions,
  removeArchivedSessionsByKey,
  retainArchivedSessionSelection,
  selectableArchivedSessionKeys,
  selectedArchivedSessions,
  shouldToggleArchivedSessionRow,
  toggleAllArchivedSessionSelection,
} from './archivedSessions.js';

function test(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

test('archived session identity includes workspace and accepts legacy field names', () => {
  assert.deepEqual(
    archivedSessionTarget({ session_id: 'session-a', workspaceHash: 'workspace-1' }),
    {
      id: 'session-a',
      workspaceHash: 'workspace-1',
      key: '["workspace-1","session-a"]',
    },
  );
  assert.equal(
    archivedSessionKey({ id: 'session-a' }),
    '["__local__","session-a"]',
  );
});

test('same session id in different workspaces remains independently selectable', () => {
  const items = [
    { id: 'same-id', workspace_hash: 'workspace-1' },
    { id: 'same-id', workspace_hash: 'workspace-2' },
  ];
  const selected = new Set([archivedSessionKey(items[1])]);
  assert.deepEqual(selectedArchivedSessions(items, selected), [items[1]]);
  assert.deepEqual(removeArchivedSessionsByKey(items, selected), [items[0]]);
});

test('select all and select none use every valid workspace-qualified row', () => {
  const items = [
    { id: 'same-id', workspace_hash: 'workspace-1' },
    { id: 'same-id', workspace_hash: 'workspace-2' },
    { workspace_hash: 'workspace-3' },
  ];
  const firstKey = archivedSessionKey(items[0]);
  const secondKey = archivedSessionKey(items[1]);

  assert.deepEqual(selectableArchivedSessionKeys(items), [firstKey, secondKey]);
  assert.equal(allArchivedSessionsSelected(items, new Set([firstKey])), false);

  const selectedAll = toggleAllArchivedSessionSelection(
    items,
    new Set([firstKey]),
  );
  assert.deepEqual([...selectedAll], [firstKey, secondKey]);
  assert.equal(allArchivedSessionsSelected(items, selectedAll), true);

  const selectedNone = toggleAllArchivedSessionSelection(items, selectedAll);
  assert.deepEqual([...selectedNone], []);
  assert.equal(allArchivedSessionsSelected(items, selectedNone), false);
});

test('empty or invalid archived lists have no selectable all-state', () => {
  const staleSelection = new Set(['["workspace","missing"]']);

  assert.deepEqual(selectableArchivedSessionKeys(null), []);
  assert.equal(allArchivedSessionsSelected([], staleSelection), false);
  assert.deepEqual(
    [...toggleAllArchivedSessionSelection([{ workspace_hash: 'workspace' }], staleSelection)],
    [],
  );
});

test('invalid archived rows do not participate in selection or removal', () => {
  const invalid = { workspace_hash: 'workspace-1' };
  assert.equal(archivedSessionKey(invalid), '');
  assert.deepEqual(selectedArchivedSessions([invalid], new Set([''])), []);
  assert.deepEqual(removeArchivedSessionsByKey([invalid], new Set([''])), [invalid]);
});

test('archived row toggles outside interactive controls', () => {
  const target = (interactiveMatch) => ({
    closest: () => interactiveMatch,
  });

  assert.equal(shouldToggleArchivedSessionRow(target(null)), true);
  assert.equal(shouldToggleArchivedSessionRow(target({ tagName: 'BUTTON' })), false);
  assert.equal(shouldToggleArchivedSessionRow(target({ tagName: 'INPUT' })), false);
  assert.equal(shouldToggleArchivedSessionRow(null), true);
});

const archive = [
  { id: 'same', workspace_hash: 'one', workspaceName: 'Project', cwd: 'C:\\one', title: 'First draft', updated_at: '2026-09-01T12:00:00Z' },
  { id: 'same', workspace_hash: 'two', workspaceName: 'Project', cwd: 'C:\\two', title: 'Latest draft', updated_at: '2026-09-09T12:00:00Z' },
  { id: 'middle', workspace_hash: 'one', workspaceName: 'Project', cwd: 'C:\\one', title: '修复归档搜索', updated_at: '2026-09-05T12:00:00Z' },
  { id: 'task', no_workspace: true, title: 'Task draft', created_at: '2026-09-03T12:00:00Z' },
];

test('archive groups keep same-name workspaces and no-workspace tasks separate', () => {
  const groups = groupArchivedSessions(archive);
  assert.deepEqual(groups.map(({ key }) => key), ['__no_workspace__', 'one', 'two']);
  assert.deepEqual(groups[1].items.map(({ id }) => id), ['middle', 'same']);
  assert.deepEqual(groups.map(({ items }) => items.length), [1, 2, 1]);
  assert.equal(groups[1].name, groups[2].name);
  assert.notEqual(groups[1].path, groups[2].path);
  assert.equal(groups[0].name, '无工作区');
});

test('archive workspace labels support paths, legacy local rows, and explicit no-workspace flags', () => {
  assert.deepEqual(archivedSessionWorkspace({ workspaceHash: 'hash', cwd: 'C:\\work\\demo\\' }), {
    key: 'hash', name: 'demo', path: 'C:\\work\\demo\\',
  });
  assert.deepEqual(archivedSessionWorkspace({}), { key: '__local__', name: '当前会话', path: '' });
  assert.deepEqual(archivedSessionWorkspace({ noWorkspace: true, workspace_hash: 'stale', cwd: '/stale' }), {
    key: '__no_workspace__', name: '无工作区', path: '',
  });
  assert.deepEqual(groupArchivedSessions([{ id: 'local' }, { id: 'task', no_workspace: true }])
    .map(({ key }) => key), ['__no_workspace__', '__local__']);
});

test('archive search combines trimmed case-insensitive titles, workspace names, paths, and workspace filter', () => {
  assert.deepEqual(groupArchivedSessions(archive, { query: '  DRAFT  ', workspaceKey: 'one' })[0].items, [archive[0]]);
  assert.equal(groupArchivedSessions(archive, { query: 'project' }).length, 2);
  assert.deepEqual(groupArchivedSessions(archive, { query: 'c:\\two' })[0].items, [archive[1]]);
  assert.deepEqual(groupArchivedSessions(archive, { query: '归档搜索' })[0].items, [archive[2]]);
  assert.deepEqual(groupArchivedSessions(archive, { workspaceKey: '__no_workspace__' })[0].items, [archive[3]]);
  assert.deepEqual(groupArchivedSessions(archive, { query: 'missing' }), []);
  assert.deepEqual(groupArchivedSessions(archive, { query: 'draft', workspaceKey: 'missing' }), []);
  assert.deepEqual(groupArchivedSessions(null), []);
});

test('archive search uses the same fallback title displayed in the list', () => {
  const items = [{ id: 'summary', summary: 'Search fallback summary' }, { id: 'display', displayTitle: 'Local name' }];
  assert.deepEqual(groupArchivedSessions(items, { query: 'fallback' })[0].items, [items[0]]);
  assert.deepEqual(groupArchivedSessions(items, { query: 'local name' })[0].items, [items[1]]);
});

test('archive sorting uses creation fallback, keeps undated rows last, and leaves input unchanged', () => {
  const items = Object.freeze([
    Object.freeze({ id: 'unknown', workspace_hash: 'one', updated_at: 'invalid' }),
    Object.freeze({ id: 'fallback', workspace_hash: 'one', updated_at: 'invalid', created_at: '2026-09-04T12:00:00Z' }),
    ...archive.map((item) => Object.freeze({ ...item })),
  ]);
  const newest = groupArchivedSessions(items);
  const oldest = groupArchivedSessions(items, { sortOrder: 'oldest' });
  assert.deepEqual(newest.map(({ key }) => key), ['__no_workspace__', 'one', 'two']);
  assert.deepEqual(oldest.map(({ key }) => key), ['__no_workspace__', 'one', 'two']);
  assert.deepEqual(newest[1].items.map(({ id }) => id), ['middle', 'fallback', 'same', 'unknown']);
  assert.deepEqual(oldest[1].items.map(({ id }) => id), ['same', 'fallback', 'middle', 'unknown']);
  assert.equal(items[0].id, 'unknown');
});

test('sidebar workspace order is independent of session times and sort direction', () => {
  for (const workspaceOrder of [Object.freeze(['one', 'two']), Object.freeze(['two', 'one'])]) {
    for (const sortOrder of ['newest', 'oldest']) {
      const groups = groupArchivedSessions([...archive].reverse(), { workspaceOrder, sortOrder });
      assert.deepEqual(groups.map(({ key }) => key), ['__no_workspace__', ...workspaceOrder]);
      assert.deepEqual(groups.find(({ key }) => key === 'one').items.map(({ id }) => id),
        sortOrder === 'oldest' ? ['same', 'middle'] : ['middle', 'same']);
    }
  }
});

test('search and archive removal preserve workspace order without empty groups', () => {
  const options = { workspaceOrder: ['two', 'empty', 'one'] };
  assert.deepEqual(groupArchivedSessions(archive, { ...options, query: 'draft', sortOrder: 'oldest' })
    .map(({ key }) => key), ['__no_workspace__', 'two', 'one']);
  assert.deepEqual(groupArchivedSessions(archive, { ...options, query: 'project' })
    .map(({ key }) => key), ['two', 'one']);
  const remaining = removeArchivedSessionsByKey(archive, [archivedSessionKey(archive[2])]);
  assert.deepEqual(groupArchivedSessions(remaining, options).map(({ key }) => key),
    ['__no_workspace__', 'two', 'one']);
  assert.deepEqual(groupArchivedSessions(remaining, { ...options, workspaceKey: 'one' })[0].items,
    [archive[0]]);
  assert.deepEqual(groupArchivedSessions(remaining, { ...options, workspaceKey: 'empty' }), []);
});

test('unlisted and local archive groups have stable order after listed workspaces', () => {
  const items = [{ id: 'local', updated_at: '2030-01-01' }, ...archive];
  for (const sortOrder of ['newest', 'oldest']) {
    assert.deepEqual(groupArchivedSessions(items, { workspaceOrder: ['two'], sortOrder })
      .map(({ key }) => key), ['__no_workspace__', 'two', '__local__', 'one']);
    const reversed = [...items].reverse();
    assert.deepEqual(groupArchivedSessions(reversed, { workspaceOrder: ['two'], sortOrder })
      .map(({ key }) => key), ['__no_workspace__', 'two', '__local__', 'one']);
    assert.deepEqual(groupArchivedSessions(reversed, { workspaceOrder: null, sortOrder })
      .map(({ key }) => key), ['__no_workspace__', '__local__', 'one', 'two']);
  }
});

test('filtered selection excludes hidden sessions and preserves visible selection through sorting and failures', () => {
  const selected = toggleAllArchivedSessionSelection(archive, new Set());
  const filtered = groupArchivedSessions(archive, { workspaceKey: 'one' }).flatMap(({ items }) => items);
  const retained = retainArchivedSessionSelection(filtered, selected);
  assert.equal(retained.size, 2);
  assert.deepEqual(selectedArchivedSessions(filtered, retained), [archive[2], archive[0]]);
  assert.equal(retainArchivedSessionSelection([...filtered].reverse(), retained), retained);
  const afterSuccess = removeArchivedSessionsByKey(filtered, [archivedSessionKey(archive[2])]);
  assert.deepEqual([...retainArchivedSessionSelection(afterSuccess, retained)], [archivedSessionKey(archive[0])]);
  assert.deepEqual([...retainArchivedSessionSelection([], retained)], []);
  assert.equal(selected.size, archive.length);
  assert.deepEqual([...toggleAllArchivedSessionSelection(filtered, new Set())], filtered.map(archivedSessionKey));
});
