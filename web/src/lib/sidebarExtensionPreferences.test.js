import assert from 'node:assert/strict';
import {
  createSidebarExtensionPreferences,
  SIDEBAR_EXTENSION_PREFS_STORAGE_KEY as key,
} from './sidebarExtensionPreferences.js';

const layout = { order: ['skills', 'experts', 'models', 'mcp'], pinned: ['skills', 'models'] };
const unpinned = { ...layout, pinned: [] };

function origin(initial) {
  const values = new Map(initial ? [[key, JSON.stringify(initial)]] : []);
  return { getItem: (k) => values.get(k) ?? null, setItem: (k, v) => values.set(k, v) };
}

// Each fresh origin has its own empty localStorage but uses the same daemon state.
{
  let saved = null;
  const desktopApi = {
    getSidebarExtensions: async () => saved,
    async setSidebarExtensions(next, { ifAbsent } = {}) {
      if (!ifAbsent || !saved) saved = next;
      return saved;
    },
  };
  const first = createSidebarExtensionPreferences({ storage: origin(layout), desktopApi });
  await first.restore();
  assert.deepEqual(saved, layout);
  const second = createSidebarExtensionPreferences({ storage: origin(), desktopApi });
  await second.restore();
  assert.deepEqual(second.getSnapshot(), layout);
  second.change(unpinned);
  await second.flush();
  const stale = createSidebarExtensionPreferences({ storage: origin(layout), desktopApi });
  await stale.restore();
  assert.deepEqual(stale.getSnapshot(), unpinned);
  const browser = origin();
  const local = createSidebarExtensionPreferences({ storage: browser });
  local.change(layout);
  await local.flush();
  assert.deepEqual(createSidebarExtensionPreferences({ storage: browser }).getSnapshot(), layout);
  assert.deepEqual(saved, unpinned);
}

// Keep the latest edit while a write is in flight and allow Done to retry it.
{
  let finishFirst;
  const written = [];
  const store = createSidebarExtensionPreferences({
    storage: origin(),
    desktopApi: {
      getSidebarExtensions: async () => null,
      async setSidebarExtensions(next) {
        written.push(next);
        if (written.length === 1) await new Promise((resolve) => { finishFirst = resolve; });
        if (written.length === 2) throw new Error('disk full');
        return next;
      },
    },
  });
  await store.restore();
  store.change(layout);
  store.change({ ...layout, pinned: ['experts'] });
  store.change(unpinned);
  assert.deepEqual(written, [layout]);
  const closing = store.flush();
  finishFirst();
  await assert.rejects(closing, /disk full/);
  assert.deepEqual(store.getSnapshot(), unpinned);
  await store.flush();
  assert.deepEqual(written, [layout, unpinned, unpinned]);
}

console.log('[pass] sidebar extension preferences survive new origins, preserve browser layouts, and retry the latest edit');
