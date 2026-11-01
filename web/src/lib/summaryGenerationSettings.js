import { apiConnectionScope } from './api.js';

export function summaryGenerationDraft(snapshot) {
  return { enabled: !!snapshot.enabled, model_name: snapshot.model_name || '' };
}

export function summaryGenerationPatch(draft, snapshot) {
  const previous = summaryGenerationDraft(snapshot);
  return Object.fromEntries(Object.entries(draft).filter(([key, value]) => value !== previous[key]));
}

function settingsError(error, action) {
  const status = error?.status;
  let code = error?.code;
  if (status === 404 || status === 405) code = 'SUMMARY_SETTINGS_UNSUPPORTED';
  else if (status === 401 || status === 403) code = 'SUMMARY_SETTINGS_AUTH_REQUIRED';
  else if (status >= 500 && (!code || code === 'UNAVAILABLE')) code = 'SUMMARY_SETTINGS_UNAVAILABLE';
  return { code, status, action };
}

// Keep saves ordered across Tools navigation, scoped to the current daemon.
const stores = new WeakMap();

export function summaryGenerationSettingsStore(client) {
  const scope = apiConnectionScope(client);
  if (stores.has(scope)) return stores.get(scope);
  let state = { snapshot: null, draft: null, loading: false, saving: false, error: null };
  const listeners = new Set();
  let reading = null;
  let writing = null;
  let requested = false;
  const publish = (patch) => {
    state = { ...state, ...patch };
    for (const listener of listeners) listener();
  };
  const hasChanges = () => state.snapshot && Object.keys(summaryGenerationPatch(state.draft, state.snapshot)).length > 0;
  const requireConnection = () => {
    if (apiConnectionScope(client) !== scope) throw new Error('Summary settings connection changed');
  };
  const store = {
    getSnapshot: () => state,
    subscribe: (listener) => { listeners.add(listener); return () => listeners.delete(listener); },
    update: (field, value) => {
      if (!state.draft || !['enabled', 'model_name'].includes(field)) return;
      publish({ draft: { ...state.draft, [field]: value }, error: null });
    },
    selectAddedModel: (model) => {
      if (!state.snapshot || !model?.name) return;
      const models = state.snapshot.models.filter((item) => item.name !== model.name);
      models.push({ name: model.name, provider: model.provider, model: model.model });
      publish({ snapshot: { ...state.snapshot, models },
        draft: { ...state.draft, model_name: model.name }, error: null });
    },
    load: () => {
      if (reading) return reading;
      reading = Promise.resolve().then(async () => {
        if (writing) await writing;
        if (hasChanges()) return;
        publish({ loading: true, error: null });
        try {
          requireConnection();
          const snapshot = await client.getSummaryGeneration();
          publish({ snapshot, draft: summaryGenerationDraft(snapshot) });
        } catch (error) {
          publish({ error: settingsError(error, 'load') });
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
          const submittedModels = state.snapshot.models;
          const patch = summaryGenerationPatch(submitted, state.snapshot);
          if (!Object.keys(patch).length) continue;
          publish({ saving: true, error: null });
          try {
            requireConnection();
            const snapshot = await client.setSummaryGeneration(patch);
            // Model creation can finish while an earlier settings save is in
            // flight. Keep newly added metadata until the next server snapshot.
            for (const model of state.snapshot.models) {
              if (!submittedModels.includes(model) && !snapshot.models.some((item) => item.name === model.name)) {
                snapshot.models = [...snapshot.models, model];
              }
            }
            const draft = summaryGenerationDraft(snapshot);
            for (const key of Object.keys(draft)) {
              if (state.draft[key] !== submitted[key]) draft[key] = state.draft[key];
            }
            publish({ snapshot, draft });
          } catch (error) {
            publish({ error: settingsError(error, 'save') });
            return false;
          }
        }
        return true;
      }).finally(() => { writing = null; publish({ saving: false }); });
      return writing;
    },
  };
  stores.set(scope, store);
  return store;
}
