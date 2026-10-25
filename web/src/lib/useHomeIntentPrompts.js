import { useEffect, useState } from 'react';
import { apiConnectionScope } from './api.js';
import { createHomeIntentController, homeIntentErrorMessage, isHomeIntentRefreshShortcut } from './homeIntentPrompts.js';
import { toast } from '../components/Toast.jsx';

export function useHomeIntentPrompts(api, enabled) {
  const [state, setState] = useState({ suggestions: [] });
  const scope = apiConnectionScope(api);
  useEffect(() => {
    if (!enabled) return undefined;
    setState({ suggestions: [] });
    const controller = createHomeIntentController({ api, onChange: setState,
      onError: (error) => toast({ kind: 'err', text: homeIntentErrorMessage(error) }) });
    const wake = () => { if (!document.hidden) void controller.wake(); };
    // TODO(home-intent-prompts): Remove Ctrl+Y after validating intent generation.
    // Capture before Slate/browser redo; one key press must not edit the draft.
    const onKey = (event) => {
      if (!isHomeIntentRefreshShortcut(event)) return;
      event.preventDefault();
      event.stopImmediatePropagation();
      if (!event.repeat) void controller.refresh();
    };
    window.addEventListener('keydown', onKey, true);
    window.addEventListener('focus', wake);
    document.addEventListener('visibilitychange', wake);
    void controller.start();
    return () => {
      window.removeEventListener('keydown', onKey, true);
      window.removeEventListener('focus', wake);
      document.removeEventListener('visibilitychange', wake);
      controller.dispose();
    };
  }, [enabled, scope]);
  return state;
}
