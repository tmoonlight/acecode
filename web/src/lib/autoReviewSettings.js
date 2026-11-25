import { apiConnectionScope } from './api.js';

// 自动模式（试验）的裁决模型设置(设置 > 工具)。只有一个值:已保存模型的名字,
// 空串 = 跟随会话当前模型。改了就存,没有单独的保存按钮。

// 下拉的选项:第一项固定是「跟随当前模型」;之后是已保存模型;配置的名字已经
// 不在已保存模型里时(模型被删了)补一条禁用项,让用户看得到当前存的是什么。
export function autoReviewModelOptions(snapshot, selected) {
  const models = Array.isArray(snapshot?.models) ? snapshot.models : [];
  const options = [{ value: '', label: '跟随当前模型', disabled: false }];
  if (selected && !models.some((model) => model.name === selected)) {
    options.push({ value: selected, label: selected, disabled: true });
  }
  for (const model of models) options.push({ value: model.name, label: model.name, disabled: false });
  return options;
}

function settingsError(error, action) {
  const status = error?.status;
  let code = error?.code;
  if (status === 404 || status === 405) code = 'AUTO_REVIEW_SETTINGS_UNSUPPORTED';
  else if (status === 401 || status === 403) code = 'AUTO_REVIEW_SETTINGS_AUTH_REQUIRED';
  else if (status >= 500 && (!code || code === 'UNAVAILABLE')) code = 'AUTO_REVIEW_SETTINGS_UNAVAILABLE';
  return { code, status, action };
}

// 每个 daemon 连接一份:切换设置页签再回来不重新加载,保存按先后顺序串行发出。
const stores = new WeakMap();

export function autoReviewSettingsStore(client) {
  const scope = apiConnectionScope(client);
  if (stores.has(scope)) return stores.get(scope);
  let state = { snapshot: null, modelName: '', loading: false, saving: false, error: null };
  const listeners = new Set();
  let reading = null;
  let writing = null;
  const publish = (patch) => {
    state = { ...state, ...patch };
    for (const listener of listeners) listener();
  };
  const requireConnection = () => {
    if (apiConnectionScope(client) !== scope) throw new Error('Auto review settings connection changed');
  };
  const store = {
    getSnapshot: () => state,
    subscribe: (listener) => { listeners.add(listener); return () => listeners.delete(listener); },
    load: () => {
      if (reading) return reading;
      reading = Promise.resolve().then(async () => {
        if (writing) await writing;
        publish({ loading: true, error: null });
        try {
          requireConnection();
          const snapshot = await client.getAutoReview();
          publish({ snapshot, modelName: snapshot.model_name || '' });
        } catch (error) {
          publish({ error: settingsError(error, 'load') });
        } finally { publish({ loading: false }); }
      }).finally(() => { reading = null; });
      return reading;
    },
    // 选了就存。连续改动时只把最后一次的值发出去;失败则退回服务端已保存的值。
    choose: (modelName) => {
      if (!state.snapshot) return Promise.resolve(false);
      publish({ modelName, error: null });
      if (writing) return writing;
      writing = Promise.resolve().then(async () => {
        while (state.snapshot && state.modelName !== (state.snapshot.model_name || '')) {
          const submitted = state.modelName;
          publish({ saving: true });
          try {
            requireConnection();
            const snapshot = await client.setAutoReview({ model_name: submitted });
            publish({ snapshot });
            if (state.modelName === submitted) publish({ modelName: snapshot.model_name || '' });
          } catch (error) {
            publish({ modelName: state.snapshot.model_name || '', error: settingsError(error, 'save') });
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
