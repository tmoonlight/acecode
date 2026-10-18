import { readWithFallback, writeSafely } from './usePreference.js';

export const SPEECH_DEVICE_PREFS_KEY = 'acecode.speechInput.v1';
const defaults = Object.freeze({ deviceId: 'default', holdToRecord: false });
const normalize = (value) => Object.freeze({
  deviceId: typeof value?.deviceId === 'string' && value.deviceId ? value.deviceId : 'default',
  holdToRecord: value?.holdToRecord === true,
});

export function createSpeechDeviceStore(storage, events) {
  const read = () => normalize(readWithFallback(SPEECH_DEVICE_PREFS_KEY, defaults, null, storage));
  let state = read();
  const listeners = new Set();
  const publish = () => { for (const listener of listeners) listener(); };
  const onStorage = (event) => {
    if (event.key !== SPEECH_DEVICE_PREFS_KEY && event.key !== null) return;
    state = read(); publish();
  };
  return {
    getSnapshot: () => state,
    subscribe(listener) {
      if (!listeners.size) events?.addEventListener('storage', onStorage);
      listeners.add(listener);
      return () => { listeners.delete(listener); if (!listeners.size) events?.removeEventListener('storage', onStorage); };
    },
    update(patch) {
      state = normalize({ ...state, ...patch });
      writeSafely(SPEECH_DEVICE_PREFS_KEY, state, storage);
      publish();
    },
  };
}
let store;
export function speechDeviceStore() {
  if (!store) {
    let storage;
    try { storage = globalThis.localStorage; } catch {}
    store = createSpeechDeviceStore(storage, globalThis.window);
  }
  return store;
}

export const speechAudioConstraints = (deviceId) => ({
  audio: { channelCount: 1, ...(deviceId && deviceId !== 'default' ? { deviceId: { exact: deviceId } } : {}) },
  video: false,
});

export async function listSpeechDevices({ signal, requestPermission = true } = {}, environment = globalThis) {
  const check = () => { if (signal?.aborted) throw new DOMException('Cancelled', 'AbortError'); };
  if (environment.isSecureContext === false) throw { code: 'SPEECH_INSECURE_CONTEXT' };
  const media = environment.navigator?.mediaDevices;
  if (!media?.enumerateDevices || !media?.getUserMedia) throw { code: 'SPEECH_UNSUPPORTED' };
  check();
  let devices = (await media.enumerateDevices()).filter((device) => device.kind === 'audioinput');
  check();
  if (requestPermission && (!devices.length || devices.some((device) => !device.label || !device.deviceId))) {
    let permission;
    try { permission = await media.getUserMedia({ audio: true, video: false }); check(); }
    finally { permission?.getTracks().forEach((track) => track.stop()); }
    devices = (await media.enumerateDevices()).filter((device) => device.kind === 'audioinput');
    check();
  }
  const seen = new Set();
  const result = devices.filter((device) => device.deviceId && !seen.has(device.deviceId) && seen.add(device.deviceId))
    .map((device) => ({ id: device.deviceId, label: device.label }));
  if (!result.some((device) => device.id === 'default')) result.unshift({ id: 'default', label: '' });
  return result;
}
