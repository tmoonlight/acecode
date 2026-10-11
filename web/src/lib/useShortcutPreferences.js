import { useSyncExternalStore } from 'react';
import { shortcutStore } from './shortcutBindings.js';

export function useShortcutPreferences() {
  const store = shortcutStore();
  return useSyncExternalStore(store.subscribe, store.getSnapshot, store.getSnapshot);
}
