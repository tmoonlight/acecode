import assert from 'node:assert/strict';
import { speechAvailabilityStore, speechDraftValid, speechEnabled, speechErrorCode, speechErrorMessage, speechPayload, speechSettingsStore, SPEECH_DEFAULTS } from './speechRecognition.js';
import { encodeSpeechWav, captureSpeechAudio, resampleSpeechAudio } from './speechAudio.js';
import { createSpeechSession } from './speechSession.js';
import { createSpeechDeviceStore, listSpeechDevices, speechAudioConstraints, SPEECH_DEVICE_PREFS_KEY } from './speechDevices.js';

const deferred = () => {
  let resolve, reject;
  const promise = new Promise((yes, no) => { resolve = yes; reject = no; });
  return { promise, resolve, reject };
};
const configured = { ...SPEECH_DEFAULTS, enabled: true, configured: true, base_url: 'http://192.168.1.50:9876', generation: 1 };

{
  const values = new Map();
  const storage = {getItem:key=>values.get(key),setItem:(key,value)=>values.set(key,value)};
  const first = createSpeechDeviceStore(storage);
  first.update({deviceId:'usb-microphone',holdToRecord:true});
  assert.equal(JSON.parse(values.get(SPEECH_DEVICE_PREFS_KEY)).deviceId,'usb-microphone');
  assert.deepEqual(createSpeechDeviceStore(storage).getSnapshot(),{deviceId:'usb-microphone',holdToRecord:true});
  assert.deepEqual(speechAudioConstraints('usb-microphone'),{audio:{channelCount:1,deviceId:{exact:'usb-microphone'}},video:false});
  assert.deepEqual(speechAudioConstraints('default'),{audio:{channelCount:1},video:false});
}
{
  let calls=0,stopped=0;
  const permission=deferred();
  const media={enumerateDevices:async()=>[{kind:'audioinput',deviceId:'',label:''}],
    getUserMedia:()=>{calls++;return permission.promise;}};
  const abort=new AbortController();
  const listing=listSpeechDevices({signal:abort.signal},{navigator:{mediaDevices:media}});
  await Promise.resolve();
  assert.equal(calls,1);
  abort.abort();
  permission.resolve({getTracks:()=>[{stop(){stopped++;}}]});
  await assert.rejects(listing,{name:'AbortError'});
  assert.equal(stopped,1,'closing the menu releases a late permission stream');
  const devices=await listSpeechDevices({}, {navigator:{mediaDevices:{
    enumerateDevices:async()=>[{kind:'audioinput',deviceId:'usb',label:'USB'},{kind:'videoinput',deviceId:'camera',label:'Camera'}],
    getUserMedia(){throw Error('labels already available');},
  }}});
  assert.deepEqual(devices,[{id:'default',label:''},{id:'usb',label:'USB'}]);
}

assert.equal(speechEnabled(SPEECH_DEFAULTS), false);
assert.equal(speechEnabled({ ...configured, enabled: false }), false);
assert.equal(speechEnabled({ ...configured, configured: false }), false);
assert.equal(speechDraftValid(configured, true), true);
assert.equal(speechDraftValid({ ...configured, protocol: 'openai' }, true), false);
assert.equal(speechDraftValid({ ...configured, protocol: 'openai', model: 'whisper' }, true), true);
for (const base_url of ['file:///audio', 'http://user:key@server', 'http://server:0', 'http://server:65536', 'http://server?secret=value']) {
  assert.equal(speechDraftValid({ ...configured, base_url }, true), false, base_url);
}
assert.equal(speechPayload({ ...configured, api_key: '' }).api_key, '', 'clearing a saved key must be explicit');

{
  const pending = deferred();
  const client = { getSpeechAvailability: () => pending.promise };
  const store = speechAvailabilityStore(client);
  const load = store.load();
  store.publish({ ...configured, generation: 2, api_key: 'never-share' });
  pending.resolve({ enabled: false, configured: false, generation: 1 });
  await load;
  assert.equal(store.getSnapshot().enabled, true, 'an older GET cannot overwrite a successful save');
  assert.equal(store.getSnapshot().generation, 2);
  assert.equal('api_key' in store.getSnapshot(), false);
}
{
  const saved = deferred();
  let snapshot = configured;
  const client = {
    getSpeechRecognition: async () => snapshot,
    setSpeechRecognition: async (payload) => { await saved.promise; snapshot = { ...payload, configured: true, generation: 2 }; return snapshot; },
  };
  const store = speechSettingsStore(client);
  await store.load();
  store.update('base_url', 'http://speech.internal:7777');
  const write = store.flush();
  await Promise.resolve();
  store.update('model', 'an-edit-made-during-save');
  saved.resolve();
  assert.equal(await write, true);
  assert.equal(store.getSnapshot().draft.model, 'an-edit-made-during-save');
  assert.equal(speechAvailabilityStore(client).getSnapshot().generation, 2);
}
{
  const client = {
    getSpeechRecognition: async () => configured,
    setSpeechRecognition: async () => { throw { code: 'PERSIST_FAILED' }; },
  };
  const store = speechSettingsStore(client);
  await store.load();
  store.update('base_url', 'http://speech.internal:7777');
  assert.equal(await store.flush(), false);
  await store.load();
  assert.equal(store.getSnapshot().draft.base_url, 'http://speech.internal:7777', 'failed saves keep edits on reopen');
}
{
  const first = deferred();
  const calls = [];
  let snapshot = configured, active = 0, peak = 0;
  const client = {
    getSpeechRecognition: async () => snapshot,
    setSpeechRecognition: async (payload) => {
      calls.push(payload); peak = Math.max(peak, ++active);
      if (calls.length === 1) await first.promise;
      snapshot = { ...payload, configured: true, generation: snapshot.generation + 1 };
      active -= 1;
      return snapshot;
    },
  };
  const store = speechSettingsStore(client);
  await store.load();
  assert.equal(await store.flush(), true);
  assert.equal(calls.length, 0, 'closing an unchanged form does not persist or cancel recognition');
  store.update('base_url', 'http://speech.internal:7777');
  const write = store.flush();
  await Promise.resolve();
  store.update('base_url', 'http://speech.internal:8888');
  store.update('api_key', 'edited-during-request');
  assert.equal(store.flush(), write);
  const reopened = store.load();
  first.resolve();
  assert.equal(await write, true);
  await reopened;
  assert.equal(calls.length, 2, 'blur during a pending request queues the latest draft');
  assert.equal(peak, 1, 'automatic writes are serialized');
  assert.equal(snapshot.base_url, 'http://speech.internal:8888');
  assert.equal(store.getSnapshot().draft.api_key, 'edited-during-request');
  store.update('api_key', '');
  await store.flush();
  assert.equal(snapshot.api_key, '', 'clearing the key autosaves an explicit empty value');
}
{
  let fail = true, calls = 0;
  const client = {
    getSpeechRecognition: async () => configured,
    setSpeechRecognition: async (payload) => {
      calls += 1;
      if (fail) throw { code: 'PERSIST_FAILED' };
      return { ...payload, configured: true, generation: 2 };
    },
  };
  const store = speechSettingsStore(client);
  await store.load();
  store.update('timeout_ms', '');
  assert.equal(await store.flush(), false);
  assert.equal(calls, 0, 'incomplete numeric input never reaches the backend');
  store.update('timeout_ms', 30000);
  assert.equal(await store.flush(), false);
  assert.equal(store.getSnapshot().draft.timeout_ms, 30000);
  fail = false;
  assert.equal(await store.flush(), true, 'retry persists the retained draft');
  assert.equal(store.getSnapshot().error, null);
}
{
  const wav = encodeSpeechWav([new Float32Array(48000).fill(0.5), new Float32Array(48000).fill(-0.5)], 48000, 1);
  const bytes = await wav.arrayBuffer();
  const view = new DataView(bytes);
  assert.equal(wav.type, 'audio/wav');
  assert.equal(bytes.byteLength, 32044);
  assert.equal(view.getUint32(24, true), 16000);
  assert.equal(view.getUint16(22, true), 1);
  assert.equal(view.getUint16(34, true), 16);
  // 带限降采样的权重归一化后直流增益为 1:常量 0.5 编码后仍是 16384(浮点舍入允许差 1)。
  assert.ok(Math.abs(view.getInt16(44, true) - 16384) <= 1);
  assert.ok(Math.abs(view.getInt16(44 + 15999 * 2, true) - 16384) <= 1, '末尾样本按边界延拓,不会淡出');
  assert.throws(() => encodeSpeechWav([], 48000), (error) => error.code === 'SPEECH_NO_SPEECH');
}
{
  const sine = (rate, freq, seconds, amplitude = 0.5) => Float32Array.from({ length: Math.round(rate * seconds) },
    (_, i) => amplitude * Math.sin(2 * Math.PI * freq * i / rate));
  // 只看中间部分,避开首尾边界延拓带来的过渡。
  const rms = (values) => {
    const middle = values.subarray(Math.floor(values.length * 0.1), Math.floor(values.length * 0.9));
    return Math.sqrt(middle.reduce((sum, value) => sum + value * value, 0) / middle.length);
  };
  const toDb = (ratio) => 20 * Math.log10(ratio);

  // 场景(回归):48 kHz 麦克风里有 12 kHz 的高频噪声(键盘敲击、电流嘶声的典型频段)。
  // 旧实现按 3 个样本取平均降到 16 kHz,12 kHz 只衰减约 9.5 dB,并折叠成 4 kHz 落进语音频段。
  // 期望:带限降采样后残留低于 -60 dB。
  for (const [rate, freq] of [[48000, 12000], [48000, 9500], [44100, 12000], [44100, 9000]]) {
    const out = resampleSpeechAudio(sine(rate, freq, 1), rate);
    assert.ok(toDb(rms(out) / rms(sine(rate, freq, 1))) < -60, `${rate} Hz 输入的 ${freq} Hz 噪声应被滤除`);
  }

  // 场景:人声主要能量所在的 300 Hz–5 kHz。期望:幅度基本不变(±0.1 dB),且波形没有相位延迟,
  // 与 16 kHz 下直接采样的理想正弦逐点误差小于 0.005。覆盖常见的 48k / 44.1k 以及蓝牙耳机的 8k 上采样。
  for (const rate of [48000, 44100, 8000]) {
    for (const freq of rate === 8000 ? [300, 1000, 2500] : [300, 1000, 5000]) {
      const out = resampleSpeechAudio(sine(rate, freq, 1), rate);
      const ideal = sine(16000, freq, 1);
      assert.equal(out.length, 16000);
      assert.ok(Math.abs(toDb(rms(out) / rms(ideal))) < 0.1, `${rate} Hz 输入的 ${freq} Hz 应保持幅度`);
      let worst = 0;
      for (let i = 1600; i < 14400; i += 1) worst = Math.max(worst, Math.abs(out[i] - ideal[i]));
      assert.ok(worst < 0.005, `${rate} Hz 输入的 ${freq} Hz 逐点误差 ${worst}`);
    }
  }

  // 场景:设备本身就是 16 kHz。期望:原样输出,不做任何滤波。
  const direct = sine(16000, 440, 0.5);
  assert.deepEqual(resampleSpeechAudio(direct, 16000), direct);
  // 场景:输出长度与旧实现一致(floor(样本数 × 16000 / 采样率)),后端按 WAV 长度校验最长录音。
  assert.equal(resampleSpeechAudio(new Float32Array(44101), 44100).length, 16000);
  assert.equal(resampleSpeechAudio(new Float32Array(2), 48000).length, 0);
}
{
  let asked = 0;
  await assert.rejects(captureSpeechAudio({ signal: new AbortController().signal }, {
    isSecureContext: false, navigator: { mediaDevices: { getUserMedia() { asked += 1; } } },
  }), (error) => error.code === 'SPEECH_INSECURE_CONTEXT');
  assert.equal(asked, 0);
  const permission = deferred();
  let tracksStopped = 0, contextsClosed = 0;
  class Context {
    resume() { return Promise.resolve(); }
    close() { contextsClosed += 1; return Promise.resolve(); }
  }
  const abort = new AbortController();
  const capture = captureSpeechAudio({ signal: abort.signal, maxSeconds: 60 }, {
    isSecureContext: true, AudioContext: Context,
    navigator: { mediaDevices: { getUserMedia: () => permission.promise } },
  });
  abort.abort();
  permission.resolve({ getTracks: () => [{ stop() { tracksStopped += 1; } }] });
  await assert.rejects(capture, { name: 'AbortError' });
  assert.equal(tracksStopped, 1);
  assert.equal(contextsClosed, 1);
}

function fixture() {
  const permission = deferred(), response = deferred();
  const events = { inserted: [], commits: [], errors: [], uploads: [], cancels: [], released: 0, stopped: 0, disposed: 0 };
  let captureOptions, valid = true;
  const recorder = {
    stop() { events.stopped += 1; return new Blob(['wav']); },
    cancel() { events.released += 1; },
  };
  const insertion = {
    isActive: () => valid,
    insertText(text) { events.inserted.push(text); return true; },
    dispose() { events.disposed += 1; },
  };
  const session = createSpeechSession({
    capture(options) { captureOptions = options; return permission.promise; },
    transcribe(id, audio, options) { events.uploads.push({ id, audio, options }); return response.promise; },
    cancelRequest(id) { events.cancels.push(id); },
    onCommit(value) { events.commits.push(value); },
    onError(error) { events.errors.push(error); },
    createId: () => 'test-request',
  });
  return { session, events, permission, response, recorder, insertion,
    start: () => session.start(configured, () => insertion),
    invalidate: () => { valid = false; },
    limit: () => captureOptions.onLimit(),
  };
}
{
  const f = fixture();
  const start = f.start();
  await f.start(); // Repeated microphone clicks cannot request twice.
  f.session.cancel();
  f.permission.resolve(f.recorder);
  await start;
  assert.equal(f.events.released, 1, 'late permission is released');
  assert.equal(f.events.uploads.length, 0);
  assert.equal(f.events.inserted.length, 0);
  assert.equal(f.session.getState().phase, 'idle');
}
for (const send of [false, true]) {
  const f = fixture();
  const start = f.start(); f.permission.resolve(f.recorder); await start;
  const stop = f.session.stop(send);
  await f.session.stop(!send);
  f.response.resolve({ status: 'ok', text: ' recognized text ' });
  await stop;
  assert.equal(f.events.stopped, 1);
  assert.equal(f.events.uploads.length, 1);
  assert.deepEqual(f.events.inserted, ['recognized text']);
  assert.deepEqual(f.events.commits, [{ text: 'recognized text', send }]);
  assert.equal(f.events.disposed, 1);
}
{
  const f = fixture();
  const start = f.start(); f.permission.resolve(f.recorder); await start;
  const stop = f.session.stop(true);
  f.session.cancel();
  f.response.resolve({ text: 'late text', status: 'ok' });
  await stop;
  assert.equal(f.events.inserted.length, 0);
  assert.equal(f.events.commits.length, 0);
  assert.deepEqual(f.events.cancels, ['test-request']);
  assert.equal(f.events.uploads[0].options.signal.aborted, true);
}
{
  const f = fixture();
  const start = f.start(); f.permission.resolve(f.recorder); await start;
  const stop = f.session.stop(true);
  f.invalidate(); f.response.resolve({ text: 'wrong session', status: 'ok' });
  await stop;
  assert.equal(f.events.inserted.length, 0);
  assert.equal(f.events.commits.length, 0);
}
for (const result of [{ status: 'no_speech', text: '' }, { status: 'ok', text: '   ' }]) {
  const f = fixture();
  const start = f.start(); f.permission.resolve(f.recorder); await start;
  const stop = f.session.stop(true); f.response.resolve(result); await stop;
  assert.equal(f.events.commits.length, 0);
  assert.equal(f.events.errors[0].code, 'SPEECH_NO_SPEECH');
}
{
  const f = fixture();
  const start = f.start(); f.permission.reject({ name: 'NotAllowedError' }); await start;
  assert.equal(f.session.getState().phase, 'idle');
  assert.equal(f.events.errors[0].name, 'NotAllowedError');
}
{
  const first = fixture(), second = fixture();
  const pending = first.start();
  await second.start();
  assert.equal(second.events.errors[0].code, 'SPEECH_BUSY');
  first.session.cancel();
  first.permission.resolve(first.recorder); await pending;
}
{
  const f = fixture();
  const start = f.start(); f.permission.resolve(f.recorder); await start;
  f.limit();
  f.response.resolve({ text: 'time limit', status: 'ok' });
  await new Promise((resolve) => setTimeout(resolve, 0));
  assert.deepEqual(f.events.commits, [{ text: 'time limit', send: false }]);
}
// 场景:Windows Desktop 点麦克风,getUserMedia 抛 DOMException NotFoundError(没有可用麦克风,
// 远程桌面未开音频录制重定向时常见)。
// 期望:按 name 映射成"没有找到可用的麦克风"。
// 回归:旧实现先取 error.code,DOMException 的旧式数字码 NotFoundError=8 被当成错误码,
// 落到兜底文案"语音识别失败，请重试。",用户看不出真实原因。
{
  const notFound = new DOMException('Requested device not found', 'NotFoundError');
  assert.equal(notFound.code, 8);
  assert.equal(speechErrorCode(notFound), 'NotFoundError');
  assert.match(speechErrorMessage(notFound), /没有找到可用的麦克风/);
  // NotAllowedError 的旧式数字码是 0,修复前后都按 name 映射。
  assert.match(speechErrorMessage(new DOMException('Permission denied', 'NotAllowedError')), /麦克风权限被拒绝/);
}
// 场景:后端 ApiError 的 code 是字符串,字符串 code 优先于 name。
// 期望:SPEECH_CONNECTION_FAILED 走已知文案,纯字符串错误也能识别。
{
  const apiError = Object.assign(new Error('HTTP 502'), { code: 'SPEECH_CONNECTION_FAILED' });
  assert.equal(speechErrorCode(apiError), 'SPEECH_CONNECTION_FAILED');
  assert.match(speechErrorMessage(apiError), /无法连接语音识别服务/);
  assert.equal(speechErrorCode('SPEECH_BUSY'), 'SPEECH_BUSY');
}
// 场景:未映射的错误(例如 AudioContext 抛 InvalidStateError,数字码 11)。
// 期望:兜底文案带上原始错误名与消息,现场截图就能定位;没有任何信息时保持原兜底文案。
{
  const unknown = new DOMException('The AudioContext was not allowed to start', 'InvalidStateError');
  assert.equal(speechErrorMessage(unknown), '语音识别失败（InvalidStateError: The AudioContext was not allowed to start），请重试。');
  assert.equal(speechErrorMessage(null), '语音识别失败，请重试。');
  assert.equal(speechErrorMessage({}), '语音识别失败，请重试。');
  // 超长消息截断到 160 字符,避免撑爆 toast。
  const long = Object.assign(new Error('x'.repeat(500)), { name: 'TypeError' });
  assert.ok(speechErrorMessage(long).length < 180);
}
console.log('speechRecognition tests passed');
