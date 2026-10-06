import { sessionDisplayTitle } from './sessionTitle.js';

export function archivedSessionWorkspace(item = {}) {
  if (item?.no_workspace || item?.noWorkspace) {
    return { key: '__no_workspace__', name: '无工作区', path: '' };
  }
  const key = String(item?.workspace_hash || item?.workspaceHash || '__local__').trim();
  const path = String(item?.workspace_cwd || item?.cwd || '').trim();
  const name = String(item?.workspaceName || item?.workspace_name || '').trim()
    || path.replace(/\\/g, '/').split('/').filter(Boolean).pop()
    || (key === '__local__' ? '当前会话' : key);
  return { key, name, path };
}

function archivedSessionTime(item) {
  for (const value of [item?.updated_at, item?.created_at]) {
    const time = typeof value === 'number' ? value : Date.parse(value || '');
    if (Number.isFinite(time)) return time;
  }
  return null;
}

export function groupArchivedSessions(items, {
  query = '',
  workspaceKey = '',
  sortOrder = 'newest',
  workspaceOrder = [],
} = {}) {
  const search = String(query).trim().toLowerCase();
  const rows = (Array.isArray(items) ? items : [])
    .map((item) => ({ item, workspace: archivedSessionWorkspace(item), time: archivedSessionTime(item) }))
    .filter(({ item, workspace }) => {
      if (workspaceKey && workspace.key !== workspaceKey) return false;
      if (!search) return true;
      return [sessionDisplayTitle(item, item?.name || ''), workspace.name, workspace.path]
        .some((value) => value.toLowerCase().includes(search));
    });
  const groups = new Map();
  for (const row of rows) {
    const { workspace } = row;
    if (!groups.has(workspace.key)) {
      groups.set(workspace.key, { ...workspace, items: [] });
    }
    groups.get(workspace.key).items.push(row);
  }
  const order = Array.isArray(workspaceOrder) ? workspaceOrder : [];
  const ranks = new Map(order.map((key, index) => [key, index]));
  // The sidebar places no-workspace tasks before workspace folders.
  ranks.set('__no_workspace__', -1);
  return [...groups.values()]
    .sort((a, b) => (ranks.get(a.key) ?? order.length) - (ranks.get(b.key) ?? order.length)
      || a.key.localeCompare(b.key))
    .map((group) => ({
      ...group,
      items: group.items.sort((a, b) => {
        if (a.time === null) return b.time === null ? 0 : 1;
        if (b.time === null) return -1;
        return sortOrder === 'oldest' ? a.time - b.time : b.time - a.time;
      }).map(({ item }) => item),
    }));
}

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

export function retainArchivedSessionSelection(items, selectedKeys) {
  const selected = keySet(selectedKeys);
  const visibleKeys = new Set(selectableArchivedSessionKeys(items));
  const retained = new Set([...selected].filter((key) => visibleKeys.has(key)));
  return retained.size === selected.size ? selected : retained;
}

export function removeArchivedSessionsByKey(items, removedKeys) {
  const removed = keySet(removedKeys);
  return (Array.isArray(items) ? items : []).filter((item) => {
    const key = archivedSessionKey(item);
    return !key || !removed.has(key);
  });
}
