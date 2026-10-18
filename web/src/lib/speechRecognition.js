import { apiConnectionScope } from './api.js';

export const SPEECH_DEFAULTS = Object.freeze({
  enabled: false, protocol: 'sensevoice', base_url: '', model: '', api_key: '',
  language: 'auto', timeout_ms: 120000, max_recording_seconds: 60, use_proxy: false,
});
export const speechEnabled = (value) => !!value?.enabled && !!value?.configured;

export function speechPayload(draft) {
  return Object.fromEntries(Object.keys(SPEECH_DEFAULTS).map((key) => {
    const value = draft[key];
    return [key, typeof SPEECH_DEFAULTS[key] === 'number' ? Number(value)
      : typeof SPEECH_DEFAULTS[key] === 'string' ? String(value ?? '').trim() : !!value];
  }));
}

export function speechDraftValid(draft, requireComplete = false) {
  if (!draft || !['sensevoice', 'openai', 'whisper_cpp'].includes(draft.protocol)) return false;
  const value = speechPayload(draft);
  if (!Number.isInteger(value.timeout_ms) || value.timeout_ms < 5000 || value.timeout_ms > 600000
    || !Number.isInteger(value.max_recording_seconds) || value.max_recording_seconds < 1 || value.max_recording_seconds > 300) return false;
  if (value.base_url) {
    try {
      const url = new URL(value.base_url);
      if (!['http:', 'https:'].includes(url.protocol) || !url.hostname || url.username || url.password
        || url.search || url.hash || /\s|\\/.test(value.base_url) || url.port === '0') return false;
    } catch { return false; }
  }
  return !requireComplete || (!!value.base_url && (value.protocol !== 'openai' || !!value.model));
}

export function speechErrorMessage(error) {
  const code = error?.code || error?.name || error;
  return ({
    SPEECH_INVALID_CONFIG: '请检查语音识别地址、端口和配置。',
    PERSIST_FAILED: '保存语音识别配置失败，请重试。',
    SPEECH_NOT_CONFIGURED: '请先在设置的工具页配置并启用语音识别。',
    SPEECH_CONNECTION_FAILED: '无法连接语音识别服务，请检查地址、端口和网络。',
    SPEECH_AUTH_FAILED: '语音服务认证失败，请检查 API Key。',
    SPEECH_NOT_READY: '语音模型尚未就绪，请稍后重试。',
    SPEECH_BUSY: '语音识别正在处理其他录音，请稍后重试。',
    SPEECH_UPSTREAM_FAILED: '语音识别服务返回错误，请稍后重试。',
    SPEECH_INVALID_AUDIO: '录音格式或时长不符合服务要求，请重新录制。',
    SPEECH_INVALID_RESPONSE: '语音服务返回的内容无法识别，请检查接口协议。',
    SPEECH_TIMEOUT: '语音识别超时，请重试或调整请求超时。',
    TIMEOUT: '语音识别超时，请稍后重试。',
    SPEECH_NO_SPEECH: '没有识别到语音，请重新录制。',
    SPEECH_INSECURE_CONTEXT: '当前网页无法使用麦克风，请使用 ACECode Desktop、localhost 或 HTTPS 页面。',
    SPEECH_UNSUPPORTED: '当前环境不支持麦克风录音，请使用 ACECode Desktop 或支持录音的浏览器。',
    NotAllowedError: '麦克风权限被拒绝，请在系统或浏览器中允许访问。',
    NotFoundError: '没有找到可用的麦克风。',
    OverconstrainedError: '所选麦克风不可用，请右键重新选择。',
    NotReadableError: '无法读取麦克风，请检查设备是否被其他程序占用。',
    SPEECH_DEVICE_ENDED: '麦克风已断开，请检查设备后重试。',
  })[code] || '语音识别失败，请重试。';
}

// Only availability is shared with composers. Credentials stay in the settings
// view and authenticated API, never in localStorage or global DOM events.
const availabilityStores = new WeakMap();
export function speechAvailabilityStore(client) {
  const scope = apiConnectionScope(client);
  if (availabilityStores.has(scope)) return availabilityStores.get(scope);
  let state = Object.freeze({ enabled: false, configured: false, generation: -1 });
  const listeners = new Set();
  let reading = null;
  let mutation = 0;
  const store = {
    getSnapshot: () => state,
    subscribe: (listener) => { listeners.add(listener); return () => listeners.delete(listener); },
    publish(value) {
      mutation += 1;
      state = Object.freeze({
        enabled: !!value.enabled, configured: !!value.configured, generation: Number(value.generation) || 0,
        max_recording_seconds: Number(value.max_recording_seconds) || 60, timeout_ms: Number(value.timeout_ms) || 120000,
      });
      for (const listener of listeners) listener();
    },
    load() {
      if (reading) return reading;
      const revision = mutation;
      reading = Promise.resolve().then(() => client.getSpeechAvailability())
        .then((data) => {
          if (revision === mutation && apiConnectionScope(client) === scope) store.publish(data);
        }).catch(() => {
          if (revision === mutation) store.publish({ enabled: false, configured: false });
        }).finally(() => { reading = null; });
      return reading;
    },
  };
  availabilityStores.set(scope, store);
  return store;
}

const settingsStores = new WeakMap();
export function speechSettingsStore(client) {
  const scope = apiConnectionScope(client);
  if (settingsStores.has(scope)) return settingsStores.get(scope);
  let state = { draft: null, snapshot: null, loading: false, saving: false, error: null };
  let reading = null, writing = null, requested = false;
  const listeners = new Set();
  const publish = (patch) => {
    state = { ...state, ...patch };
    for (const listener of listeners) listener();
  };
  const changed = () => state.draft && state.snapshot && JSON.stringify(speechPayload(state.draft)) !== JSON.stringify(speechPayload(state.snapshot));
  const store = {
    getSnapshot: () => state,
    subscribe: (listener) => { listeners.add(listener); return () => listeners.delete(listener); },
    update(field, value) {
      if (state.draft) publish({ draft: { ...state.draft, [field]: value }, error: null });
    },
    load() {
      if (reading) return reading;
      reading = Promise.resolve().then(async () => {
        if (writing) await writing;
        if (changed()) return;
        publish({ loading: true, error: null });
        try {
          const snapshot = await client.getSpeechRecognition();
          if (apiConnectionScope(client) !== scope) return;
          publish({ snapshot, draft: { ...SPEECH_DEFAULTS, ...snapshot } });
          speechAvailabilityStore(client).publish(snapshot);
        } catch (error) { publish({ error: { code: error?.code || error?.name, action: 'load' } }); }
        finally { publish({ loading: false }); }
      }).finally(() => { reading = null; });
      return reading;
    },
    flush() {
      requested = true;
      if (writing) return writing;
      writing = Promise.resolve().then(async () => {
        while (requested && state.snapshot) {
          requested = false;
          if (!changed()) continue;
          const submitted = state.draft;
          if (!speechDraftValid(submitted)) {
            publish({ error: { code: 'SPEECH_INVALID_CONFIG', action: 'save' } });
            return false;
          }
          publish({ saving: true, error: null });
          try {
            if (apiConnectionScope(client) !== scope) throw new Error('Connection changed');
            const snapshot = await client.setSpeechRecognition(speechPayload(submitted));
            const draft = { ...SPEECH_DEFAULTS, ...snapshot };
            for (const key of Object.keys(SPEECH_DEFAULTS)) {
              if (state.draft[key] !== submitted[key]) draft[key] = state.draft[key];
            }
            publish({ snapshot, draft });
            speechAvailabilityStore(client).publish(snapshot);
          } catch (error) {
            publish({ error: { code: error?.code || error?.name, action: 'save' } });
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
