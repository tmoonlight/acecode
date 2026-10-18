import { useEffect, useMemo, useRef, useState, useSyncExternalStore } from 'react';
import { apiConnectionScope } from './api.js';
import { captureSpeechAudio } from './speechAudio.js';
import { createSpeechSession } from './speechSession.js';
import { speechAvailabilityStore, speechEnabled } from './speechRecognition.js';
import { speechDeviceStore } from './speechDevices.js';
import { useAppShortcuts } from './useAppShortcuts.js';
import { appShortcutContextAllows } from './appShortcuts.js';

export function useSpeechRecognition({ client, scope, editorRef, rootRef, mainComposer, disabled, onSend, onError }) {
  const connection = apiConnectionScope(client);
  const availability = speechAvailabilityStore(client);
  const config = useSyncExternalStore(availability.subscribe, availability.getSnapshot);
  const inputStore = speechDeviceStore();
  const preferences = useSyncExternalStore(inputStore.subscribe, inputStore.getSnapshot);
  const [state, setState] = useState({ phase: 'idle', levels: [], elapsed: 0 });
  const callbacks = useRef(null);
  const identity = [scope, config.generation, preferences.deviceId, preferences.holdToRecord, disabled ? 'locked' : 'editable'].join(':');
  callbacks.current = { identity, onSend, onError, config, preferences, disabled, editorRef };
  const mounted = useRef(false);
  const sendEpoch = useRef(0);
  const held = useRef(null);
  const session = useMemo(() => createSpeechSession({
    capture: captureSpeechAudio,
    transcribe: (id, audio, options) => client.transcribeSpeech(id, audio, options),
    cancelRequest: (id) => client.cancelSpeech(id),
    createId: () => globalThis.crypto?.randomUUID?.() || ('voice_' + Date.now() + '_' + Math.random().toString(36).slice(2)),
    onState: (next) => { if (mounted.current) setState(next); },
    onError: (error) => callbacks.current.onError(error),
    onCommit: ({ send }) => {
      const committedIdentity = callbacks.current.identity;
      const epoch = ++sendEpoch.current;
      requestAnimationFrame(() => {
        if (!mounted.current || epoch !== sendEpoch.current || committedIdentity !== callbacks.current.identity) return;
        if (send && !callbacks.current.disabled) callbacks.current.onSend();
        else callbacks.current.editorRef.current?.focus({
          canFocus: () => mounted.current && epoch === sendEpoch.current
            && committedIdentity === callbacks.current.identity
            && appShortcutContextAllows('dictation', document.activeElement),
        });
      });
    },
  }), [connection]);
  useEffect(() => {
    mounted.current = true;
    setState(session.getState());
    void availability.load();
    const reload = () => { void availability.load(); };
    window.addEventListener('focus', reload);
    return () => {
      mounted.current = false; held.current = null; sendEpoch.current += 1; session.cancel();
      window.removeEventListener('focus', reload);
    };
  }, [session, availability]);
  useEffect(() => { held.current = null; sendEpoch.current += 1; session.cancel(); }, [session, identity, config.enabled, config.configured]);
  const releaseHold = () => {
    if (!held.current) return;
    held.current = null;
    if (session.getState().phase === 'requesting') { sendEpoch.current += 1; session.cancel(); }
    else void session.stop(false);
  };
  const latestRelease = useRef(releaseHold);
  latestRelease.current = releaseHold;
  useEffect(() => {
    const keyUp = (event) => {
      if (held.current?.kind === 'key' && (event.code === held.current.code || ['Control', 'Meta', 'Alt', 'Shift'].includes(event.key))) latestRelease.current();
    };
    const pointerUp = (event) => { if (held.current?.kind === 'pointer' && held.current.id === event.pointerId) latestRelease.current(); };
    const release = () => latestRelease.current();
    window.addEventListener('keyup', keyUp, true);
    window.addEventListener('pointerup', pointerUp, true);
    window.addEventListener('pointercancel', pointerUp, true);
    window.addEventListener('blur', release);
    window.addEventListener('pagehide', release);
    return () => {
      window.removeEventListener('keyup', keyUp, true);
      window.removeEventListener('pointerup', pointerUp, true);
      window.removeEventListener('pointercancel', pointerUp, true);
      window.removeEventListener('blur', release);
      window.removeEventListener('pagehide', release);
    };
  }, [session]);
  const controls = {
    ...state, active: state.phase !== 'idle', available: speechEnabled(config),
    preferences, configureInput: inputStore.update,
    start() {
      if (callbacks.current.disabled || !speechEnabled(callbacks.current.config)) return false;
      sendEpoch.current += 1;
      const startedIdentity = callbacks.current.identity;
      void session.start({ ...callbacks.current.config, device_id: callbacks.current.preferences.deviceId }, () => {
        const insertion = callbacks.current.editorRef.current?.beginFileTransfer?.();
        return {
          isActive: () => callbacks.current.identity === startedIdentity && insertion?.isActive(),
          insertText: (text) => insertion?.insertText(text),
          dispose: () => insertion?.dispose(),
        };
      });
      return true;
    },
    stop: (send) => { held.current = null; return session.stop(send); },
    cancel: () => { held.current = null; sendEpoch.current += 1; session.cancel(); },
    toggle() {
      const phase = session.getState().phase;
      if (phase === 'idle') return controls.start();
      if (phase === 'requesting') controls.cancel();
      if (phase === 'recording') void controls.stop(false);
      return true;
    },
    beginHold(value) {
      if (session.getState().phase === 'idle' && controls.start()) held.current = value;
    },
    interceptSubmit() {
      if (session.getState().phase === 'idle') return false;
      void session.stop(true);
      return true;
    },
  };
  useAppShortcuts(controls.available && !disabled ? {
    dictation: (event) => {
      const root = rootRef?.current;
      if (!root || !root.getClientRects().length) return false;
      const owner = event.target?.closest?.('[data-speech-composer]');
      if (owner ? owner !== root : !mainComposer) return false;
      if (preferences.holdToRecord) controls.beginHold({ kind: 'key', code: event.code });
      else controls.toggle();
    },
  } : {}, { capture: true });
  return controls;
}
