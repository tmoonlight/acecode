let captureOwner = null;
const idle = () => ({ phase: 'idle', levels: [], elapsed: 0 });

// Injectable capture/transport keeps permission and request ordering testable.
export function createSpeechSession({ capture, transcribe, cancelRequest, onState, onCommit, onError, createId }) {
  let state = idle(), active = null;
  const owner = {};
  const publish = (patch) => { state = { ...state, ...patch }; onState?.(state); };
  const releaseOwner = () => { if (captureOwner === owner) captureOwner = null; };
  const disposeInsertion = (session) => { session?.insertion?.dispose?.(); };
  const current = (session) => active === session && !session.controller.signal.aborted;
  const finish = (session) => {
    if (!current(session)) return;
    active = null; releaseOwner(); disposeInsertion(session); publish(idle());
  };
  const fail = (session, error) => {
    if (!current(session)) return;
    const pending = state.phase === 'transcribing';
    session.recorder?.cancel(); session.controller.abort(); releaseOwner();
    active = null; disposeInsertion(session); publish(idle());
    if (pending) Promise.resolve(cancelRequest(session.id)).catch(() => {});
    if (error?.name !== 'AbortError') onError?.(error);
  };
  const controller = {
    getState: () => state,
    async start(config, createInsertion) {
      if (active) return;
      if (captureOwner) { onError?.({ code: 'SPEECH_BUSY' }); return; }
      captureOwner = owner;
      const session = { controller: new AbortController(), config, id: createId(), recorder: null, insertion: null };
      active = session;
      publish({ phase: 'requesting', levels: [], elapsed: 0 });
      try {
        session.insertion = createInsertion();
        if (!session.insertion?.isActive()) throw new DOMException('Editor changed', 'AbortError');
        const recorder = await capture({
          signal: session.controller.signal,
          maxSeconds: config.max_recording_seconds,
          deviceId: config.device_id,
          onLevel: (level, elapsed) => {
            if (current(session) && state.phase === 'recording')
              publish({ levels: [...state.levels.slice(-95), level], elapsed });
          },
          onLimit: () => { if (current(session)) void controller.stop(false); },
          onError: (error) => fail(session, error),
        });
        if (!current(session)) { recorder.cancel(); return; }
        session.recorder = recorder;
        publish({ phase: 'recording' });
      } catch (error) { fail(session, error); }
    },
    async stop(send = false) {
      const session = active;
      if (!session || state.phase !== 'recording') return;
      publish({ phase: 'transcribing' });
      releaseOwner();
      try {
        const audio = session.recorder.stop();
        session.recorder = null;
        const result = await transcribe(session.id, audio, {
          signal: session.controller.signal, timeoutMs: session.config.timeout_ms + 10000,
        });
        if (!current(session) || !session.insertion?.isActive()) { finish(session); return; }
        const text = typeof result?.text === 'string' ? result.text.trim() : '';
        if (!text || result.status === 'no_speech') throw Object.assign(new Error('No speech'), { code: 'SPEECH_NO_SPEECH' });
        const inserted = session.insertion.insertText(text);
        if (inserted) onCommit?.({ text, send });
        finish(session);
      } catch (error) { fail(session, error); }
    },
    cancel() {
      const session = active;
      if (!session) return;
      const pending = state.phase === 'transcribing';
      active = null;
      session.controller.abort(); session.recorder?.cancel(); disposeInsertion(session);
      releaseOwner(); publish(idle());
      if (pending) Promise.resolve(cancelRequest(session.id)).catch(() => {});
    },
  };
  return controller;
}
