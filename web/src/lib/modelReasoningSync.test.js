import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { refreshSavedModelReasoning, requestSavedModelReasoningSync, subscribeModelProfileUpdates } from './modelReasoningSync.js';
import { composerReasoningOptions } from './modelReasoning.js';
import { normalizeModelOptions, withCreateSessionPreferences } from './sessionModel.js';

const connection = new EventTarget();
let updates = 0;
const unsubscribe = subscribeModelProfileUpdates(connection, () => { updates += 1; });
const message = (type) => {
  const event = new Event('message');
  event.detail = { type };
  connection.dispatchEvent(event);
};
message('session_status');
assert.equal(updates, 0);
message('model_profiles_updated');
connection.dispatchEvent(new Event('open'));
assert.equal(updates, 2);
unsubscribe();
message('model_profiles_updated');
connection.dispatchEvent(new Event('open'));
assert.equal(updates, 2);

await requestSavedModelReasoningSync({ refreshModelReasoning: async () => { throw new Error('offline'); } });
await requestSavedModelReasoningSync({ refreshModelReasoning() { throw new Error('unavailable'); } });
await requestSavedModelReasoningSync({});

// A new conversation reads saved profiles, which can lack a declaration even
// when existing sessions still have one. Discovery restores the home control.
let savedModels = [{ name: 'ACEModel-starrylight', provider: 'openai' }];
let homeModels = normalizeModelOptions(savedModels);
let syncRequests = 0;
const offHome = subscribeModelProfileUpdates(connection, () => {
  homeModels = normalizeModelOptions(savedModels);
});
assert.equal(composerReasoningOptions(homeModels[0]), null);
await requestSavedModelReasoningSync({ refreshModelReasoning: async () => {
  syncRequests += 1;
  savedModels = [{ ...savedModels[0], reasoning: {
    supported: true, default_enabled: true,
    supported_efforts: ['low', 'medium', 'high'], default_effort: 'medium',
  } }];
  message('model_profiles_updated');
} });
assert.equal(composerReasoningOptions(homeModels[0]).label, '中');
assert.equal(composerReasoningOptions(homeModels[0], 'high').label, '高');
const creation = withCreateSessionPreferences({}, {
  modelName: homeModels[0].name, reasoningEffort: 'high',
});
assert.equal(creation.reasoning_effort, 'high');
assert.equal(savedModels[0].reasoning.default_effort, 'medium');
message('model_profiles_updated');
assert.equal(syncRequests, 1, 'profile notifications must not queue more remote syncs');
offHome();

let release;
const blocked = new Promise((resolve) => { release = resolve; });
let localReloads = 0;
const refresh = refreshSavedModelReasoning(
  { refreshModelReasoning: () => blocked },
  () => { localReloads += 1; },
);
await Promise.resolve();
await Promise.resolve();
assert.equal(localReloads, 1, 'local list refresh must not wait for remote work');
release();
await refresh;
await refreshSavedModelReasoning(
  { refreshModelReasoning() { throw new Error('offline'); } },
  async () => { throw new Error('disconnected'); },
);

// Exercise the UI wiring contract in addition to the event lifecycle above.
const settings = readFileSync(new URL('../components/model-settings/ModelSettingsSection.jsx', import.meta.url), 'utf8');
assert.match(settings, /refreshSavedModelReasoning\(api,/);
assert.match(settings, /subscribeModelProfileUpdates\(connection,/);
assert.match(settings, /loadSavedModels\(\{ quiet: true, silent: true \}\)/);
const app = readFileSync(new URL('../App.jsx', import.meta.url), 'utf8');
assert.match(app, /subscribeModelProfileUpdates\(connection,[\s\S]*?setModelProfileRevision/);
const chat = readFileSync(new URL('../components/ChatView.jsx', import.meta.url), 'utf8');
assert.match(chat, /useEffect\(\(\) => \{\s*if \(sid\) return;[\s\S]*?requestSavedModelReasoningSync\(api\);\s*\}, \[api, sid\]\)/);
assert.match(chat, /const refreshSessionModels = useCallback\(async \(\) => \{[\s\S]*?requestSavedModelReasoningSync\(api\)/);
console.log('modelReasoningSync.test.js: all tests passed');
