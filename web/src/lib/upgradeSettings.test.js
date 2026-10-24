import assert from 'node:assert/strict';
import { upgradeSettingsStore } from './upgradeSettings.js';

const flush = async () => { for (let i = 0; i < 15; i += 1) await Promise.resolve(); };
const deferred = () => { let resolve; const promise = new Promise((r) => { resolve = r; }); return { promise, resolve }; };
async function test(name, fn) { await fn(); console.log(`[pass] ${name}`); }

await test('upgrade settings default off and serialize edits across navigation', async () => {
  let saved = { base_url: 'https://updates.example/' };
  const patches = []; const first = deferred();
  const client = { getUpgradeConfig: async () => saved, setUpgradeConfig: async (patch) => {
    patches.push(patch); if (patches.length === 1) await first.promise;
    saved = { ...saved, ...patch }; return saved;
  } };
  const store = upgradeSettingsStore(client); await store.load();
  assert.equal(store.getSnapshot().draft.auto_update, false);
  store.update('auto_update', true); const saving = store.save(); await flush();
  store.update('auto_update', false); store.save();
  store.update('base_url', 'https://new.example/'); store.save();
  const reopened = upgradeSettingsStore(client); assert.equal(reopened, store);
  const reload = reopened.load();
  assert.equal(patches.length, 1);
  first.resolve(); await Promise.all([saving, reload]);
  assert.deepEqual(patches, [{ auto_update: true }, { base_url: 'https://new.example/', auto_update: false }]);
  assert.equal(store.getSnapshot().draft.auto_update, false);
  assert.equal(store.getSnapshot().snapshot.base_url, 'https://new.example/');
});

await test('upgrade settings retain failed drafts and retry without a success illusion', async () => {
  let fail = true;
  const client = { getUpgradeConfig: async () => ({ base_url: 'https://updates.example/', auto_update: false }),
    setUpgradeConfig: async (patch) => { if (fail) throw new Error('disk full'); return { base_url: 'https://updates.example/', ...patch }; },
  };
  const store = upgradeSettingsStore(client); await store.load();
  store.update('auto_update', true); assert.equal(await store.save(), false);
  assert.equal(store.getSnapshot().snapshot.auto_update, false);
  assert.equal(store.getSnapshot().draft.auto_update, true);
  assert.equal(store.getSnapshot().error, 'disk full');
  await store.load(); assert.equal(store.getSnapshot().draft.auto_update, true);
  fail = false; assert.equal(await store.save(), true);
  assert.equal(store.getSnapshot().snapshot.auto_update, true);
});

await test('load and unmount flush in the same tick do not deadlock', async () => {
  const client = { getUpgradeConfig: async () => ({ base_url: 'https://updates.example/' }), setUpgradeConfig: async (value) => value };
  const store = upgradeSettingsStore(client);
  await Promise.all([store.load(), store.save()]);
  assert.equal(store.getSnapshot().loading, false);
  store.update('base_url', 'invalid'); assert.equal(await store.save(), false);
  assert.equal(store.getSnapshot().draft.base_url, 'invalid');
});
