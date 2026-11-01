import { sessionRefFromCreateResponse } from './newSession.js';
import { createSpeechSession } from './speechSession.js';
import { speechEnabled, speechErrorMessage } from './speechRecognition.js';

export const MINI_FILE_LIMIT = 25 * 1024 * 1024;
const validId = id => typeof id === 'string' && /^[\w-]{1,100}$/.test(id);

// Owns background input independently of App navigation and all editor DOM.
export function createMiniTaskController({ client, getContext, getSpeechConfig, capture,
  publish, onCreated = () => {}, refresh = () => {}, createId = () => crypto.randomUUID() }) {
  let disposed = false, busy = false, voice = null, voiceEpoch = 0, stopRequested = false;
  let state = { sending: false, activeRequestId: '', speechPhase: 'idle', error: '', files: [], pendingTasks: [], lastSubmission: null, recovery: null };
  const files = new Map(), requests = new Map();
  const emit = patch => { if (!disposed) { state = { ...state, ...patch }; publish(state); } };
  const fileState = () => [...files.values()].map(({ id, name, ready, error }) => ({ id, name, ready, error }));
  const errorText = error => error?.code?.startsWith('SPEECH_') || error?.name === 'NotAllowedError'
    || error?.name === 'NotFoundError' ? speechErrorMessage(error) : String(error?.message || error?.code || '发送失败，请重试。');
  const speech = createSpeechSession({ capture,
    transcribe: (id, audio, options) => client.transcribeSpeech(id, audio, options),
    cancelRequest: id => client.cancelSpeech(id), createId,
    onState: next => {
      emit({ speechPhase: next.phase });
      if (next.phase === 'recording' && stopRequested) { stopRequested = false; void speech.stop(true); }
    },
    onError: error => { voice = null; emit({ speechPhase: 'idle', error: errorText(error) }); },
    onCommit: ({ text }) => {
      const recorded = voice; voice = null;
      if (recorded && !disposed) void submit({ request_id: recorded.id, text, files: [] }, recorded.context, true);
    },
  });

  async function submit(action, context = { ...getContext() }, recorded = false) {
    if (disposed || busy || !validId(action.request_id)) return;
    const text = typeof action.text === 'string' ? action.text.trim() : '';
    const ids = Array.isArray(action.files) ? [...new Set(action.files)] : [];
    if ((!text && !ids.length) || text.length > 100000 || ids.length > 10) return;
    const existing = requests.get(action.request_id);
    if (existing?.accepted) { emit({ lastSubmission: existing.accepted }); return; }
    if (existing && (existing.text !== text || JSON.stringify(existing.ids) !== JSON.stringify(ids))) {
      emit({ error: '重试内容已变化，请重新发送。' }); return;
    }
    if (ids.some(id => !files.get(id)?.ready)) { emit({ error: '附件尚未就绪，请重试。' }); return; }
    const request = existing || { text, ids, context, ref: null, uploaded: new Map() };
    requests.set(action.request_id, request);
    busy = true; emit({ sending: true, activeRequestId: action.request_id, error: '', recovery: null });
    try {
      if (!request.ref) {
        const hash = request.context.noWorkspace ? '' : request.context.workspaceHash || request.context.workspace_hash || '';
        const options = { auto_start: false, title: text.slice(0, 200) || files.get(ids[0])?.name || '附件消息' };
        const result = hash && hash !== '__local__'
          ? await client.createWorkspaceSession(hash, options)
          : await client.createSession({ ...options, ...(request.context.noWorkspace ? { no_workspace: true } : {}) });
        request.ref = sessionRefFromCreateResponse(result, request.context);
      }
      if (disposed) return;
      for (const id of ids) {
        if (request.uploaded.has(id)) continue;
        const file = files.get(id);
        const uploaded = await client.uploadSessionAttachment(request.ref.sessionId,
          { name: file.name, mime_type: file.mime, data_base64: file.chunks.join('') });
        request.uploaded.set(id, uploaded);
        if (disposed) return;
      }
      const attachments = [...request.uploaded.values()].map(item => ({ id: item.id }));
      const response = await client.sendInput(request.ref.sessionId, { text, attachments });
      if (response?.queued !== true) throw new Error('任务未被接受，请重试。');
      request.accepted = { requestId: action.request_id, sessionId: request.ref.sessionId, workspaceHash: request.ref.workspaceHash || '' };
      if (disposed) return;
      for (const id of ids) files.delete(id);
      const task = { ...request.accepted, title: text || '附件消息', body: '正在启动任务', status: 'running', key: request.ref.sessionId };
      emit({ lastSubmission: request.accepted, files: fileState(), pendingTasks: [...state.pendingTasks, task].slice(-12), recovery: null });
      onCreated(request.ref);
      refresh();
      // Completed requests retain only small idempotency receipts.
      requests.set(action.request_id, { text, ids, accepted: request.accepted });
      while (requests.size > 32) requests.delete(requests.keys().next().value);
    } catch (error) {
      emit({ error: errorText(error), ...(recorded ? { recovery: { requestId: action.request_id, text } } : {}) });
    } finally { busy = false; emit({ sending: false }); }
  }

  async function startRecording() {
    if (disposed || busy || state.speechPhase !== 'idle') return;
    const epoch = ++voiceEpoch;
    const context = { ...getContext() };
    stopRequested = false; emit({ speechPhase: 'requesting', error: '', recovery: null });
    try {
      const config = await getSpeechConfig();
      if (disposed || epoch !== voiceEpoch) return;
      if (!speechEnabled(config)) throw { code: 'SPEECH_NOT_CONFIGURED' };
      voice = { id: createId(), context };
      await speech.start(config, () => ({ isActive: () => !disposed && epoch === voiceEpoch,
        insertText: () => true, dispose() {} }));
    } catch (error) { if (!disposed && epoch === voiceEpoch) emit({ speechPhase: 'idle', error: errorText(error) }); }
  }

  function fileAction(action) {
    if (!validId(action.id) || busy) return;
    const file = files.get(action.id);
    if (action.type === 'mini-file-remove') { files.delete(action.id); emit({ files: fileState() }); return; }
    if (action.type === 'mini-file-begin') {
      const size = Number(action.size);
      if (file || files.size >= 10 || !Number.isInteger(size) || size < 0 ||
          [...files.values()].reduce((sum, item) => sum + item.size, size) > MINI_FILE_LIMIT) {
        emit({ error: '附件总大小不能超过 25MiB。' }); return;
      }
      files.set(action.id, { id: action.id, name: String(action.name || 'attachment').slice(0, 255),
        mime: String(action.mime || '').slice(0, 128), size, chunks: [], length: 0, ready: false });
      emit({ files: fileState(), error: '' }); return;
    }
    if (!file || file.ready || file.error) return;
    if (action.type === 'mini-file-chunk') {
      if (typeof action.data !== 'string' || action.data.length > 262144 || action.offset !== file.length ||
          file.length + action.data.length > Math.ceil(file.size / 3) * 4 || !/^[A-Za-z0-9+/]*={0,2}$/.test(action.data)) {
        file.chunks = []; file.error = '附件读取失败，请重新选择。'; emit({ files: fileState(), error: file.error }); return;
      }
      file.chunks.push(action.data); file.length += action.data.length;
    } else if (action.type === 'mini-file-end') {
      file.ready = file.length === Math.ceil(file.size / 3) * 4;
      if (!file.ready) file.error = '附件读取失败，请重新选择。';
      emit({ files: fileState(), ...(file.error ? { error: file.error } : {}) });
    }
  }

  return {
    getSnapshot: () => state,
    observeActivities(activities = []) {
      const live = new Set(activities.map(item => item.sessionId));
      state = { ...state, pendingTasks: state.pendingTasks.filter(item => !live.has(item.sessionId)) };
      return state;
    },
    handle(action) {
      if (disposed) return;
      if (action.type === 'mini-submit' && state.speechPhase === 'idle') return submit(action);
      if (action.type === 'mini-record-start') return startRecording();
      if (action.type === 'mini-record-stop') {
        if (state.speechPhase === 'requesting') stopRequested = true;
        else return speech.stop(true);
      }
      if (action.type === 'mini-record-cancel') {
        ++voiceEpoch; voice = null; stopRequested = false; speech.cancel();
        if (!busy && state.recovery) requests.delete(state.recovery.requestId);
        emit({ speechPhase: 'idle', error: '', recovery: null });
      }
      if (action.type?.startsWith('mini-file-')) fileAction(action);
    },
    dispose() { disposed = true; ++voiceEpoch; voice = null; speech.cancel(); files.clear(); requests.clear(); },
  };
}
