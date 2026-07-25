import assert from 'node:assert/strict';
import {
  allArchivedSessionsSelected,
  archivedSessionKey,
  archivedSessionTarget,
  removeArchivedSessionsByKey,
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
