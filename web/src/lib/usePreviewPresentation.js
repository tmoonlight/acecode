import { useCallback, useEffect, useRef, useState } from 'react';
import { mergeNextValue } from './usePreference.js';

const PRESENTATION_LAYOUT = Object.freeze({
  sidebarCollapsed: true,
  sidePanelCollapsed: false,
  sidePanelListCollapsed: true,
  sidePanelMaximized: true,
});

// Presentation is a temporary view of the existing preferences. Never persist
// its hidden sidebars, and never apply one workbench's view to another owner.
export function usePreviewPresentation(owner, preferences, setPreferences, enabled) {
  const [presentation, setPresentation] = useState(null);
  const active = enabled && presentation?.owner === owner;
  const uiPrefs = active ? { ...preferences, ...presentation.layout } : preferences;
  const latest = useRef(null);
  latest.current = { owner, preferences, setPreferences, enabled, active, uiPrefs, presentation };

  useEffect(() => {
    if (!enabled || presentation?.owner !== owner) setPresentation(null);
  }, [enabled, owner, presentation?.owner]);

  const toggle = useCallback(() => {
    const current = latest.current;
    if (!current.enabled) return false;
    const next = current.active ? null : { owner: current.owner, layout: { ...PRESENTATION_LAYOUT } };
    // Also guard synchronous repeated menu/keyboard dispatch before React paints.
    current.active = !!next;
    current.presentation = next;
    current.uiPrefs = next ? { ...current.preferences, ...next.layout } : current.preferences;
    setPresentation(next);
    return true;
  }, []);

  const setUiPrefs = useCallback((updater) => {
    const current = latest.current;
    if (!current.active) return current.setPreferences(updater);
    const next = mergeNextValue(current.uiPrefs, updater);
    const savedChanges = {};
    const layout = { ...current.presentation.layout };
    for (const [key, value] of Object.entries(next)) {
      if (value === current.uiPrefs[key]) continue;
      if (Object.hasOwn(PRESENTATION_LAYOUT, key)) layout[key] = value;
      else savedChanges[key] = value;
    }
    if (Object.keys(savedChanges).length) current.setPreferences(savedChanges);
    // Hiding the entire workspace must survive leaving presentation; the other
    // layout fields still return to their ordinary preferences.
    if (layout.sidePanelCollapsed) {
      current.setPreferences({ ...savedChanges, sidePanelCollapsed: true });
      current.active = false;
      current.presentation = null;
      setPresentation(null);
      return;
    }
    current.presentation = { owner: current.owner, layout };
    current.uiPrefs = { ...next, ...layout };
    setPresentation(current.presentation);
  }, []);

  return { uiPrefs, setUiPrefs, active: !!active, toggle };
}
