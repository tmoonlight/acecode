import { createContext, useLayoutEffect, useRef, useState } from 'react';
import { pruneSidebarSelection, selectSidebarRange, sidebarSelectionKey } from './sidebarSessionSelection.js';

export const SidebarSessionSelectionContext = createContext(null);
const emptySelection = () => ({ anchor: '', keys: new Set() });

// The rendered list is the authority for range order (pinning, pagination and
// tree disclosure all affect it). Never select hidden group members.
export function useSidebarSessionSelection(listRef, sessions, blockedRef) {
  const [state, setState] = useState(emptySelection);
  const current = useRef(state);
  const latestSessions = useRef(sessions);
  latestSessions.current = sessions;
  const commit = (next) => {
    if (next === current.current) return;
    current.current = next;
    setState(next);
  };
  const rows = () => Array.from(listRef.current?.querySelectorAll('[data-sidebar-selection-key]') || [])
    .filter(row => row.getBoundingClientRect().width > 0);
  const visibleKeys = () => rows().map(row => row.dataset.sidebarSelectionKey);
  const clear = () => commit(emptySelection());
  const entries = () => {
    const byKey = new Map(latestSessions.current.map(session => [sidebarSelectionKey(session), session]));
    return rows().filter(row => current.current.keys.has(row.dataset.sidebarSelectionKey))
      .map(row => ({
        key: row.dataset.sidebarSelectionKey,
        pinned: row.dataset.desktopSessionPinned === 'true',
        session: byKey.get(row.dataset.sidebarSelectionKey),
      })).filter(entry => entry.session);
  };
  useLayoutEffect(() => {
    commit(pruneSidebarSelection(current.current, visibleKeys()));
  });
  return {
    keys: state.keys, clear, entries,
    selectOnly(session) {
      const key = sidebarSelectionKey(session);
      commit({ anchor: key, keys: new Set(key ? [key] : []) });
    },
    retain(keys) {
      commit(pruneSidebarSelection({ ...current.current, keys: new Set(keys) }, visibleKeys()));
    },
    click(event, session) {
      if (blockedRef.current) return false;
      const key = sidebarSelectionKey(session);
      if (!key) return !(event.shiftKey || event.ctrlKey || event.metaKey);
      commit(selectSidebarRange(current.current, visibleKeys(), key, event));
      return !(event.shiftKey || event.ctrlKey || event.metaKey);
    },
    dragEntries(session, pinned) {
      const key = sidebarSelectionKey(session);
      if (key && current.current.keys.has(key)) return entries();
      if (key) commit({ anchor: key, keys: new Set([key]) });
      else clear();
      return [{ key, session, pinned }];
    },
  };
}
