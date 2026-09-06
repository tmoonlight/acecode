import { apiConnectionScope } from './api.js';

export function imageGenerationDraft(snapshot) {
  return {
    enabled: snapshot.enabled,
    source: snapshot.source,
    saved_model_name: snapshot.saved_model_name || '',
    base_url: snapshot.base_url || '',
    models: { ...snapshot.models },
    default_quality: snapshot.default_quality,
    timeout_ms: snapshot.timeout_ms,
    apiKey: snapshot.api_key || '',
  };
}

export function imageGenerationPayload(draft) {
  const payload = {
    enabled: draft.enabled,
    source: draft.source,
    saved_model_name: draft.saved_model_name,
    base_url: draft.base_url.trim(),
    models: Object.fromEntries(Object.entries(draft.models).map(([key, value]) => [key, value.trim()])),
    default_quality: draft.default_quality,
    timeout_ms: Number(draft.timeout_ms),
  };
  // Match model settings: refill the saved key and only submit nonempty input.
  if (draft.apiKey.trim()) payload.api_key = draft.apiKey.trim();
  return payload;
}

export function imageGenerationCanTest(draft, snapshot) {
  if (!draft) return false;
  if (draft.source === 'saved_model') {
    return !!snapshot.connections?.some((item) => item.name === draft.saved_model_name && item.has_api_key);
  }
  return !!draft.base_url.trim() &&
    (!!draft.apiKey.trim() || snapshot.has_api_key);
}

export function imageGenerationPatch(draft, snapshot) {
  const next = imageGenerationPayload(draft);
  const previous = imageGenerationPayload(imageGenerationDraft(snapshot));
  const patch = {};
  for (const [key, value] of Object.entries(next)) {
    if (key === 'models') {
      const models = Object.fromEntries(Object.entries(value).filter(([quality, model]) => model !== previous.models[quality]));
      if (Object.keys(models).length) patch.models = models;
    } else if (value !== previous[key]) patch[key] = value;
  }
  return patch;
}

function mergeSavedDraft(current, submitted, snapshot) {
  const saved = imageGenerationDraft(snapshot);
  for (const key of Object.keys(saved)) {
    if (key === 'models') {
      for (const quality of Object.keys(saved.models)) {
        if (current.models[quality] !== submitted.models[quality]) saved.models[quality] = current.models[quality];
      }
    } else if (current[key] !== submitted[key]) saved[key] = current[key];
  }
  return saved;
}

// A connection-scoped queue survives settings navigation. It keeps pending
// edits in memory only and prevents reopening from racing an unfinished write.
const settingsStores = new WeakMap();

export function imageGenerationSettingsStore(client) {
  const scope = apiConnectionScope(client);
  if (settingsStores.has(scope)) return settingsStores.get(scope);
  let state = { snapshot: null, draft: null, loading: false, saving: false, error: null };
  const listeners = new Set();
  let reading = null;
  let writing = null;
  let requested = false;
  const publish = (patch) => {
    state = { ...state, ...patch };
    for (const listener of listeners) listener();
  };
  const hasChanges = () => state.snapshot && Object.keys(imageGenerationPatch(state.draft, state.snapshot)).length > 0;
  const requireConnection = () => {
    if (apiConnectionScope(client) !== scope) throw new Error('Image settings connection changed');
  };
  const store = {
    getSnapshot: () => state,
    subscribe: (listener) => { listeners.add(listener); return () => listeners.delete(listener); },
    update: (field, value) => {
      if (!state.draft) return;
      const [key, quality] = field.split('.');
      const draft = quality
        ? { ...state.draft, models: { ...state.draft.models, [quality]: value } }
        : { ...state.draft, [key]: value };
      publish({ draft, error: null });
    },
    load: () => {
      if (reading) return reading;
      reading = Promise.resolve().then(async () => {
        if (writing) await writing;
        if (hasChanges()) return;
        publish({ loading: true, error: null });
        try {
          requireConnection();
          const snapshot = await client.getImageGeneration();
          publish({ snapshot, draft: imageGenerationDraft(snapshot) });
        } catch (error) {
          publish({ error: { code: error.code, action: 'load' } });
        } finally { publish({ loading: false }); }
      }).finally(() => { reading = null; });
      return reading;
    },
    flush: () => {
      requested = true;
      if (writing) return writing;
      writing = Promise.resolve().then(async () => {
        while (requested && state.snapshot) {
          requested = false;
          const submitted = state.draft;
          const patch = imageGenerationPatch(submitted, state.snapshot);
          if (!Object.keys(patch).length) {
            publish({ draft: imageGenerationDraft(state.snapshot) });
            continue;
          }
          if ((patch.models && Object.values(patch.models).some((value) => !value)) ||
              ('timeout_ms' in patch && (!Number.isInteger(patch.timeout_ms) || patch.timeout_ms < 30000 || patch.timeout_ms > 600000))) {
            publish({ error: { code: 'BAD_REQUEST', action: 'save' } });
            return false;
          }
          publish({ saving: true, error: null });
          try {
            requireConnection();
            const snapshot = await client.setImageGeneration(patch);
            publish({ snapshot, draft: mergeSavedDraft(state.draft, submitted, snapshot) });
          } catch (error) {
            publish({ error: { code: error.code, action: 'save' } });
            return false;
          }
        }
        return true;
      }).finally(() => { writing = null; publish({ saving: false }); });
      return writing;
    },
  };
  settingsStores.set(scope, store);
  return store;
}
