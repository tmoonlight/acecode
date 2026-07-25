export function archivedSessionTarget(item = {}) {
  const id = String(item?.id || item?.session_id || item?.sessionId || '').trim();
  const workspaceHash = String(
    item?.workspace_hash || item?.workspaceHash || '',
  ).trim();
  if (!id) return { id: '', workspaceHash, key: '' };
  const scope = workspaceHash || '__local__';
  return {
    id,
    workspaceHash,
    key: JSON.stringify([scope, id]),
  };
}

export function archivedSessionKey(item) {
  return archivedSessionTarget(item).key;
}

export function shouldToggleArchivedSessionRow(target) {
  if (!target || typeof target.closest !== 'function') return true;
  return !target.closest('button, input, a, select, textarea');
}

function keySet(keys) {
  if (keys instanceof Set) return keys;
  return new Set(Array.isArray(keys) ? keys : []);
}

export function selectableArchivedSessionKeys(items) {
  return (Array.isArray(items) ? items : [])
    .map((item) => archivedSessionKey(item))
    .filter(Boolean);
}

export function allArchivedSessionsSelected(items, selectedKeys) {
  const selectableKeys = selectableArchivedSessionKeys(items);
  if (selectableKeys.length === 0) return false;
  const selected = keySet(selectedKeys);
  return selectableKeys.every((key) => selected.has(key));
}

export function toggleAllArchivedSessionSelection(items, selectedKeys) {
  const selectableKeys = selectableArchivedSessionKeys(items);
  if (selectableKeys.length === 0) return new Set();
  if (allArchivedSessionsSelected(items, selectedKeys)) return new Set();
  return new Set(selectableKeys);
}

export function selectedArchivedSessions(items, selectedKeys) {
  const selected = keySet(selectedKeys);
  return (Array.isArray(items) ? items : []).filter((item) => {
    const key = archivedSessionKey(item);
    return key && selected.has(key);
  });
}

export function removeArchivedSessionsByKey(items, removedKeys) {
  const removed = keySet(removedKeys);
  return (Array.isArray(items) ? items : []).filter((item) => {
    const key = archivedSessionKey(item);
    return !key || !removed.has(key);
  });
}
