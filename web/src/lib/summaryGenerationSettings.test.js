import assert from 'node:assert/strict';
import { ApiError, createApi } from './api.js';
import { lookupErrorMessage } from './errors.js';
import { summaryGenerationDraft, summaryGenerationPatch, summaryGenerationSettingsStore } from './summaryGenerationSettings.js';

const snapshot = { enabled: false, model_name: '', configured: false,
  models: [{ name: 'chat', provider: 'openai', model: 'large' },
    { name: 'local', provider: 'openai', model: 'small' }] };
const copy = (value) => structuredClone(value);
const tick = () => new Promise((resolve) => setImmediate(resolve));
assert.deepEqual(summaryGenerationDraft(snapshot), { enabled: false, model_name: '' });
assert.deepEqual(summaryGenerationPatch({ enabled: true, model_name: 'local' }, snapshot),
  { enabled: true, model_name: 'local' });

let saved = copy(snapshot);
const writes = [];
const finishes = [];
const client = {
  getSummaryGeneration: async () => copy(saved),
  setSummaryGeneration: (patch) => new Promise((resolve) => {
    writes.push(copy(patch));
    finishes.push(() => { saved = { ...saved, ...patch }; resolve(copy(saved)); });
  }),
};
const store = summaryGenerationSettingsStore(client);
await store.load();
store.update('enabled', true);
const automaticSave = store.flush();
await tick();
assert.deepEqual(writes.shift(), { enabled: true }, 'empty selection saves automatic mode');
finishes.shift()();
assert.equal(await automaticSave, true);
assert.equal(store.getSnapshot().error, null);
store.update('model_name', 'local');
const firstSave = store.flush();
await tick();
assert.deepEqual(writes, [{ model_name: 'local' }]);
store.update('enabled', false);
store.update('model_name', 'chat');
const nextSave = store.flush();
const reopened = summaryGenerationSettingsStore(client);
assert.equal(reopened, store);
const reload = reopened.load();
assert.equal(writes.length, 1, 'writes are serialized across navigation');
finishes.shift()();
await tick();
assert.deepEqual(store.getSnapshot().draft, { enabled: false, model_name: 'chat' });
assert.deepEqual(writes[1], { enabled: false, model_name: 'chat' });
finishes.shift()();
assert.equal(await firstSave, true);
assert.equal(await nextSave, true);
await reload;
await store.flush();
assert.equal(writes.length, 2, 'unchanged navigation does not write');
assert.deepEqual(store.getSnapshot().draft, { enabled: false, model_name: 'chat' });

let failWrite = true;
const retry = summaryGenerationSettingsStore({
  getSummaryGeneration: async () => copy(snapshot),
  setSummaryGeneration: async (patch) => {
    if (failWrite) throw new ApiError(500, { error: 'PERSIST_FAILED' });
    return { ...copy(snapshot), ...patch };
  },
});
await retry.load();
retry.update('model_name', 'local');
assert.equal(await retry.flush(), false);
await retry.load();
assert.equal(retry.getSnapshot().draft.model_name, 'local');
assert.equal(retry.getSnapshot().snapshot.model_name, '');
failWrite = false;
assert.equal(await retry.flush(), true);
assert.equal(retry.getSnapshot().snapshot.model_name, 'local');

let missing = { ...copy(snapshot), enabled: true, model_name: 'deleted' };
const deleted = summaryGenerationSettingsStore({
  getSummaryGeneration: async () => copy(missing),
  setSummaryGeneration: async (patch) => { missing = { ...missing, ...patch }; return copy(missing); },
});
await deleted.load();
deleted.update('enabled', false);
assert.equal(await deleted.flush(), true);
assert.equal(deleted.getSnapshot().snapshot.model_name, 'deleted', 'disabling retains an unavailable selection');
deleted.update('enabled', true);
assert.equal(await deleted.flush(), true, 'removed selection uses silent runtime fallback');
deleted.selectAddedModel({ name: 'new-local', provider: 'openai', model: 'tiny', api_key: 'private' });
assert.equal(deleted.getSnapshot().draft.model_name, 'new-local');
assert.equal(JSON.stringify(deleted.getSnapshot()).includes('private'), false);
assert.equal(await deleted.flush(), true, 'newly added models can satisfy a pending enable');

let finishEarlierSave;
let creationWrites = 0;
const duringCreation = summaryGenerationSettingsStore({
  getSummaryGeneration: async () => copy(snapshot),
  setSummaryGeneration: async (patch) => {
    creationWrites++;
    if (creationWrites === 1) return new Promise((resolve) => {
      finishEarlierSave = () => resolve({ ...copy(snapshot), ...patch });
    });
    return { ...copy(snapshot), ...patch, models: [...snapshot.models, { name: 'just-added' }] };
  },
});
await duringCreation.load();
duringCreation.update('model_name', 'local');
const earlierSave = duringCreation.flush();
await tick();
duringCreation.selectAddedModel({ name: 'just-added', provider: 'openai', model: 'tiny' });
duringCreation.update('enabled', true);
const createdSave = duringCreation.flush();
finishEarlierSave();
assert.equal(await earlierSave, true);
assert.equal(await createdSave, true);
assert.equal(creationWrites, 2);
assert.deepEqual(duringCreation.getSnapshot().draft, { enabled: true, model_name: 'just-added' });

for (const [status, code] of [[404, 'SUMMARY_SETTINGS_UNSUPPORTED'], [405, 'SUMMARY_SETTINGS_UNSUPPORTED'],
  [401, 'SUMMARY_SETTINGS_AUTH_REQUIRED'], [403, 'SUMMARY_SETTINGS_AUTH_REQUIRED'], [503, 'SUMMARY_SETTINGS_UNAVAILABLE']]) {
  let unavailable = true, writesOnFailure = 0;
  const failed = summaryGenerationSettingsStore({
    getSummaryGeneration: async () => { if (unavailable) throw new ApiError(status, 'failure'); return copy(snapshot); },
    setSummaryGeneration: async () => { writesOnFailure++; return copy(snapshot); },
  });
  await failed.load();
  assert.deepEqual(failed.getSnapshot().error, { status, code, action: 'load' });
  assert.equal(failed.getSnapshot().snapshot, null);
  assert.notEqual(lookupErrorMessage(code, 'fallback'), 'fallback');
  await failed.flush();
  assert.equal(writesOnFailure, 0);
  unavailable = false;
  await failed.load();
  assert.equal(failed.getSnapshot().error, null);
}

const originalFetch = globalThis.fetch;
const calls = [];
try {
  globalThis.fetch = async (url, options) => {
    calls.push({ url, options });
    return { ok: true, status: 200, headers: new Headers({ 'Content-Type': 'application/json' }), json: async () => copy(snapshot) };
  };
  const base = { origin: 'http://127.0.0.1:9876', token: 'fixture-token' };
  const api = createApi(base);
  await api.getSummaryGeneration();
  await api.setSummaryGeneration({ enabled: true, model_name: 'local' });
  assert.deepEqual(calls.map(({ options }) => options.method), ['GET', 'PUT']);
  assert.equal(calls[1].options.keepalive, true);
  assert.equal(calls[1].options.headers['X-ACECode-Token'], 'fixture-token');
  assert.equal(calls[1].url, 'http://127.0.0.1:9876/api/config/summary-generation');
  assert.deepEqual(JSON.parse(calls[1].options.body), { enabled: true, model_name: 'local' });
  const originalStore = summaryGenerationSettingsStore(api);
  await originalStore.load();
  originalStore.update('model_name', 'local');
  Object.assign(base, { origin: 'http://127.0.0.1:9877', token: 'other-token' });
  assert.notEqual(summaryGenerationSettingsStore(api), originalStore);
  const priorCalls = calls.length;
  assert.equal(await originalStore.flush(), false);
  assert.equal(calls.length, priorCalls, 'an old draft cannot write to a different daemon');
} finally { globalThis.fetch = originalFetch; }

console.log('[pass] summary generation selection, ordered saves, retry, backend compatibility and API scope');
