import { pinnedOrderKey, sessionPinScope } from './pinnedSessions.js';

export function sidebarSelectionKey(session) {
  return session?.sidebar_group ? '' : pinnedOrderKey(sessionPinScope(session), session?.id);
}

export function selectSidebarRange(state, visibleKeys, key, { shiftKey, ctrlKey, metaKey } = {}) {
  const visible = new Set(visibleKeys);
  if (!visible.has(key)) return state;
  const additive = ctrlKey || metaKey;
  if (shiftKey) {
    const anchor = visible.has(state.anchor) ? state.anchor : key;
    const from = visibleKeys.indexOf(anchor);
    const to = visibleKeys.indexOf(key);
    const range = visibleKeys.slice(Math.min(from, to), Math.max(from, to) + 1);
    const keys = new Set(additive ? [...state.keys].filter(item => visible.has(item)) : []);
    range.forEach(item => keys.add(item));
    return { anchor, keys };
  }
  const keys = new Set(additive ? [...state.keys].filter(item => visible.has(item)) : []);
  if (additive && keys.has(key)) keys.delete(key);
  else keys.add(key);
  return { anchor: key, keys };
}

export function pruneSidebarSelection(state, visibleKeys) {
  const visible = new Set(visibleKeys);
  const keys = new Set([...state.keys].filter(key => visible.has(key)));
  const anchor = visible.has(state.anchor) ? state.anchor : '';
  return keys.size === state.keys.size && anchor === state.anchor ? state : { keys, anchor };
}

export function reorderSidebarSelection(items, selectedKeys, targetKey, placement, keyOf) {
  const selected = new Set(selectedKeys);
  if (!['before', 'after'].includes(placement) || selected.has(targetKey)) return items;
  const moving = items.filter(item => selected.has(keyOf(item)));
  const remaining = items.filter(item => !selected.has(keyOf(item)));
  const index = remaining.findIndex(item => keyOf(item) === targetKey);
  if (!moving.length || moving.length !== selected.size || index < 0) return items;
  const at = index + (placement === 'after' ? 1 : 0);
  return [...remaining.slice(0, at), ...moving, ...remaining.slice(at)];
}
