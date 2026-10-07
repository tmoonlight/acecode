import { useEffect, useRef, useState, useSyncExternalStore } from 'react';
import { api } from './api.js';
import { connection } from './connection.js';
import { focusSession } from './desktopNotify.js';
import { createDesktopOfficeController } from './desktopOfficeController.js';
import { createDesktopOfficePreferences } from './desktopOfficePreferences.js';

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
    const onMessage = event => controller?.onEvent(event.detail || {});
    const onOpen = () => controller?.reconnected();
    const onDisconnect = () => controller?.disconnected();
    const onAction = event => {
      const action = event.detail || {};
      if (action.type === 'select') controller?.select(action);
      else if (action.type === 'follow') controller?.setFollow(action.follow);
      else if (action.type === 'open') focusSession(action.workspace_hash, action.session_id);
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
          try {Promise.resolve(window.aceDesktop_updateOffice(JSON.stringify(state))).catch(() => {});} catch {}
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
      disposed = true; controller?.dispose(); controllerRef.current = null;
      connection.removeEventListener('message', onMessage);
      connection.removeEventListener('open', onOpen);
      connection.removeEventListener('disconnect', onDisconnect);
      window.removeEventListener('ace-desktop-office-action', onAction);
    };
  }, [state.enabled, state.available]);
  useEffect(() => {controllerRef.current?.setActive(activeRef);}, [activeRef]);
  return { ...state, setEnabled: preferences.setEnabled, claimWelcome: preferences.claimWelcome };
}
