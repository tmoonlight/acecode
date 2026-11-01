import { useEffect, useRef, useState, useSyncExternalStore } from 'react';
import { api } from './api.js';
import { connection } from './connection.js';
import { focusSession } from './desktopNotify.js';
import { createDesktopOfficeController } from './desktopOfficeController.js';
import { createDesktopOfficePreferences } from './desktopOfficePreferences.js';
import { createMiniTaskController } from './miniTaskController.js';
import { captureMiniSpeechAudio } from './miniSpeechAudio.js';
import { speechAvailabilityStore } from './speechRecognition.js';
import { speechDeviceStore } from './speechDevices.js';
import { notifySessionListChanged } from './sessionListEvents.js';
import { readComposerSendAppearance } from './composerActionAppearance.js';

// Companion-pet cards use exactly the transcript body font: the live family and the size
// chosen by the font-size setting (small / medium / large set --ace-font-size-13 on <html>).
function transcriptFont() {
  try {
    const style = getComputedStyle(document.documentElement);
    return {size: style.getPropertyValue('--ace-font-size-13').trim(), family: style.getPropertyValue('--font-sans').trim()};
  } catch {
    return null;
  }
}

export function useDesktopOffice(activeRef) {
  const [preferences] = useState(() => createDesktopOfficePreferences(window));
  const state = useSyncExternalStore(preferences.subscribe, preferences.getSnapshot);
  useEffect(() => { preferences.start(); return () => preferences.dispose(); }, [preferences]);
  const current = useRef(activeRef);
  current.current = activeRef;
  const controllerRef = useRef(null);
  useEffect(() => {
    if (!state.enabled || !state.available || typeof window.aceDesktop_updateOffice !== 'function') return undefined;
    let disposed = false, controller = null;
    let latest = { version: 1, connected: false, agents: [], offices: [], activities: [] };
    let mini = null;
    const publish = () => {
      if (disposed) return;
      try { Promise.resolve(window.aceDesktop_updateOffice(JSON.stringify({ ...latest,
        transcriptFont: transcriptFont(), composerAppearance: readComposerSendAppearance(), mini: mini?.getSnapshot() }))).catch(() => {}); } catch {}
    };
    if (state.petKind === 'mini') mini = createMiniTaskController({
      client: api, getContext: () => current.current || {}, capture: captureMiniSpeechAudio, publish,
      getSpeechConfig: async () => {
        const store = speechAvailabilityStore(api);
        await store.load();
        return { ...store.getSnapshot(), device_id: speechDeviceStore().getSnapshot().deviceId };
      },
      onCreated: ref => notifySessionListChanged({ reason: 'session-created', sessionId: ref.sessionId,
        workspaceHash: ref.workspaceHash || '', noWorkspace: !!ref.noWorkspace, session: { ...ref, id: ref.sessionId } }),
      refresh: () => controller?.refresh(),
    });
    const onMessage = event => controller?.onEvent(event.detail || {});
    const onOpen = () => controller?.reconnected();
    const onDisconnect = () => controller?.disconnected();
    const onAction = event => {
      const action = event.detail || {};
      if (action.type === 'select') controller?.select(action);
      else if (action.type === 'follow') controller?.setFollow(action.follow);
      else if (action.type === 'open') focusSession(action.workspace_hash, action.session_id);
      else if (action.type?.startsWith('mini-')) void mini?.handle(action);
      // Companion-pet session cards: ■ stops that session's turn, × on a finished card marks it read.
      else if (action.type === 'stop' && action.session_id) connection.sendAbort(action.session_id);
      else if (action.type === 'read' && action.session_id) {
        connection.markSessionRead({sessionId: action.session_id, workspaceHash: action.workspace_hash || ''});
        controller?.refresh();
      }
    };
    const start = async () => {
      let initial = {};
      try {
        const value = await window.aceDesktop_getOfficeState?.();
        initial = typeof value === 'string' ? JSON.parse(value) : value || {};
      } catch {}
      if (disposed) return;
      controller = createDesktopOfficeController({
        initial, fetchSnapshot: api.getDesktopOffice, fetchModel: api.getSessionModel,
        publish: state => {
          latest = state; mini?.observeActivities(state.activities); publish();
        },
        retainSession: id => connection.retainSession(id),
        releaseSession: id => connection.releaseSession(id),
      });
      controllerRef.current = controller;
      controller.setActive(current.current);
      controller.start();
    };
    connection.addEventListener('message', onMessage);
    connection.addEventListener('open', onOpen);
    connection.addEventListener('disconnect', onDisconnect);
    window.addEventListener('ace-desktop-office-action', onAction);
    void start();
    return () => {
      disposed = true; mini?.dispose(); controller?.dispose(); controllerRef.current = null;
      connection.removeEventListener('message', onMessage);
      connection.removeEventListener('open', onOpen);
      connection.removeEventListener('disconnect', onDisconnect);
      window.removeEventListener('ace-desktop-office-action', onAction);
    };
  }, [state.enabled, state.available, state.petKind]);
  useEffect(() => {controllerRef.current?.setActive(activeRef);}, [activeRef]);
  return { ...state, setEnabled: preferences.setEnabled, setPetKind: preferences.setPetKind };
}
