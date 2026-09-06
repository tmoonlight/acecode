import assert from 'node:assert/strict';
import { imageGenerationCanTest, imageGenerationDraft, imageGenerationPayload, imageGenerationSettingsStore } from './imageGenerationSettings.js';
import { createApi } from './api.js';

const snapshot = {
  enabled: true, source: 'inline', base_url: 'https://example.invalid/v1',
  api_key: 'saved-test-key', has_api_key: true, configured: true,
  models: { standard: 'image', high: 'image-hd', ultra: 'image-ultra' },
  default_quality: 'standard', timeout_ms: 180000,
  connections: [{ name: 'gateway', has_api_key: true }],
};
const draft = imageGenerationDraft(snapshot);
assert.equal(draft.apiKey, 'saved-test-key');
assert.equal(imageGenerationPayload(draft).api_key, 'saved-test-key');
assert.equal(imageGenerationCanTest(draft, snapshot), true);
assert.equal(imageGenerationPayload({ ...draft, apiKey: ' new-key ' }).api_key, 'new-key');
assert.equal(imageGenerationPayload({ ...draft, apiKey: '' }).api_key, undefined);
assert.equal(imageGenerationCanTest({ ...draft, apiKey: '' }, snapshot), true);
const emptySnapshot = { ...snapshot, api_key: '', has_api_key: false };
assert.equal(imageGenerationCanTest(imageGenerationDraft(emptySnapshot), emptySnapshot), false);
assert.equal(imageGenerationCanTest({ ...draft, source: 'saved_model', saved_model_name: 'gateway' }, snapshot), true);
assert.equal(imageGenerationCanTest({ ...draft, source: 'saved_model', saved_model_name: 'deleted' }, snapshot), false);
draft.models.high = 'custom';
assert.equal(snapshot.models.high, 'image-hd');
assert.equal(imageGenerationPayload(draft).has_api_key, undefined);

const copy = (value) => structuredClone(value);
const tick = () => new Promise((resolve) => setImmediate(resolve));
let savedConfig = copy(snapshot);
const writes = [];
const finishWrites = [];
const delayedClient = {
  getImageGeneration: async () => copy(savedConfig),
  setImageGeneration: (patch) => new Promise((resolve) => {
    writes.push(copy(patch));
    finishWrites.push(() => {
      savedConfig = { ...savedConfig, ...patch, models: { ...savedConfig.models, ...patch.models } };
      resolve(copy(savedConfig));
    });
  }),
};
const settings = imageGenerationSettingsStore(delayedClient);
await settings.load();
assert.equal(settings.getSnapshot().draft.apiKey, 'saved-test-key');
settings.update('apiKey', 'first-edit');
assert.equal(writes.length, 0, 'typing waits for blur');
const firstSave = settings.flush();
await tick();
assert.deepEqual(writes, [{ api_key: 'first-edit' }]);
settings.update('apiKey', 'latest-edit');
settings.update('models.high', 'new-hd');
const nextSave = settings.flush();
const reopened = imageGenerationSettingsStore(delayedClient);
const reload = reopened.load();
assert.equal(reopened, settings, 'navigation retains the pending save queue');
assert.equal(writes.length, 1, 'a second save must wait for the first');
finishWrites.shift()();
await tick();
assert.equal(settings.getSnapshot().draft.apiKey, 'latest-edit', 'old response preserves newer input');
assert.deepEqual(writes[1], { api_key: 'latest-edit', models: { high: 'new-hd' } });
finishWrites.shift()();
assert.equal(await firstSave, true);
assert.equal(await nextSave, true);
await reload;
assert.equal(reopened.getSnapshot().draft.apiKey, 'latest-edit');
await settings.flush();
assert.equal(writes.length, 2, 'unchanged blur and navigation do not write again');
settings.update('apiKey', '');
await settings.flush();
assert.equal(settings.getSnapshot().draft.apiKey, 'latest-edit', 'blank input retains the saved key');
assert.equal(writes.length, 2);

let failWrite = true;
let retryCount = 0;
const retryClient = {
  getImageGeneration: async () => copy(snapshot),
  setImageGeneration: async (patch) => {
    retryCount += 1;
    if (failWrite) throw Object.assign(new Error('fixture failure'), { code: 'PERSIST_FAILED' });
    return { ...copy(snapshot), ...patch };
  },
};
const retrySettings = imageGenerationSettingsStore(retryClient);
await retrySettings.load();
retrySettings.update('apiKey', 'retained-after-failure');
assert.equal(await retrySettings.flush(), false);
await retrySettings.load();
assert.equal(retrySettings.getSnapshot().draft.apiKey, 'retained-after-failure');
assert.equal(retrySettings.getSnapshot().error.code, 'PERSIST_FAILED');
failWrite = false;
assert.equal(await retrySettings.flush(), true);
assert.equal(retrySettings.getSnapshot().snapshot.api_key, 'retained-after-failure');
retrySettings.update('timeout_ms', '');
assert.equal(await retrySettings.flush(), false);
assert.equal(retryCount, 2, 'invalid timeout is never silently clamped or persisted');
assert.equal(settings.getSnapshot().draft.apiKey, 'latest-edit', 'separate connections never share keys');

// Prove the test action uses the explicit cost flag and a timeout long enough
// for the maximum configured image request; reads/saves never invoke it.
const originalFetch = globalThis.fetch;
const originalSetTimeout = globalThis.setTimeout;
const originalLocation = globalThis.location;
const calls = [];
const timers = [];
try {
  globalThis.location = { protocol: 'http:' };
  globalThis.fetch = async (url, options) => {
    calls.push({ url, options });
    return { ok: true, status: 200, headers: new Headers({ 'Content-Type': 'application/json' }), json: async () => snapshot };
  };
  globalThis.setTimeout = (fn, ms) => { timers.push(ms); return originalSetTimeout(fn, ms); };
  const client = createApi({ port: 9876, token: 'fixture-token' });
  await client.getImageGeneration();
  await client.setImageGeneration({ enabled: false });
  await client.testImageGeneration(imageGenerationPayload(draft));
  assert.deepEqual(calls.map((item) => item.options.method), ['GET', 'PUT', 'POST']);
  assert.equal(calls[1].options.keepalive, true);
  assert.equal(calls[2].url, 'http://127.0.0.1:9876/api/config/image-generation/test');
  assert.equal(JSON.parse(calls[2].options.body).confirm_cost, true);
  assert.equal(JSON.parse(calls[2].options.body).config.api_key, 'saved-test-key');
  assert.ok(timers[2] > 600000);
} finally {
  globalThis.fetch = originalFetch;
  globalThis.setTimeout = originalSetTimeout;
  if (originalLocation === undefined) delete globalThis.location;
  else globalThis.location = originalLocation;
}
console.log('[pass] image generation settings refill saved keys and isolate paid tests');
