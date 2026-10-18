import { useCallback, useEffect, useLayoutEffect, useMemo, useRef, useState } from 'react';
import { sidebarPopupEventIsInside, solveResponsiveSidebars } from './responsiveSidebars.js';

export function useResponsiveSidebars({ shellRef, owner, enabled, ...options }) {
  const [shellWidth, setShellWidth] = useState(0);
  const [content, setContent] = useState(null);
  const [popupState, setPopup] = useState(null);
  const previous = useRef(null);
  const latest = useRef(null);
  const reportContent = useCallback((next) => {
    setContent((old) => old?.owner === owner
      && old.previewAvailable === next.previewAvailable
      && old.subagentOpen === next.subagentOpen ? old : { owner, ...next });
  }, [owner]);
  useLayoutEffect(() => {
    const shell = shellRef.current;
    if (!shell || !enabled) return undefined;
    const measure = () => setShellWidth(Math.floor(shell.getBoundingClientRect().width));
    measure();
    const observer = typeof ResizeObserver === 'function' ? new ResizeObserver(measure) : null;
    observer?.observe(shell);
    window.addEventListener('resize', measure);
    return () => { observer?.disconnect(); window.removeEventListener('resize', measure); };
  }, [enabled, shellRef]);
  const decision = solveResponsiveSidebars({
    ...options, shellWidth, enabled,
    previewAvailable: (content?.owner === owner && content.previewAvailable)
      || (popupState?.owner === owner && popupState.previewRequested),
    subagentOpen: content?.owner === owner && content.subagentOpen,
    previous: previous.current?.owner === owner ? previous.current : {},
  });
  useLayoutEffect(() => { previous.current = { owner, ...decision }; });
  latest.current = { ...options, shellWidth, enabled, owner, content, previous: decision };
  useLayoutEffect(() => {
    if (content?.owner === owner && content.previewAvailable && popupState?.previewRequested) {
      setPopup((old) => old && { ...old, previewRequested: false });
    }
  }, [content, owner, popupState?.previewRequested]);

  const popup = enabled && popupState?.owner === owner
    && !(popupState.side === 'left' ? decision.canDockLeft : decision.canDockRight)
    ? popupState.side : null;
  useEffect(() => { if (!popup) setPopup(null); }, [popup]);
  const togglePopup = useCallback((side) => setPopup((old) => (
    old?.owner === owner && old.side === side ? null : { owner, side }
  )), [owner]);
  const closePopup = useCallback(() => setPopup(null), []);
  const requestRightPopup = useCallback((previewRequested = false) => {
    const current = latest.current;
    const needsPopup = solveResponsiveSidebars({
      ...current,
      previewAvailable: previewRequested || (current.content?.owner === current.owner && current.content.previewAvailable),
      subagentOpen: current.content?.owner === current.owner && current.content.subagentOpen,
    }).rightAutoCollapsed;
    if (needsPopup) setPopup({ owner: current.owner, side: 'right', previewRequested });
    return needsPopup;
  }, []);

  useEffect(() => {
    if (!popup) return undefined;
    const outside = (event) => {
      if (sidebarPopupEventIsInside(event, popup)
        || event.target?.closest?.('[data-sidebar-toggle]')) return;
      closePopup();
    };
    const escape = (event) => {
      if (event.key !== 'Escape' || event.defaultPrevented || event.isComposing) return;
      // Modal handlers use window/capture. Do not close the sidebar beneath a
      // modal that owns this key, even when that handler does not preventDefault.
      if (document.querySelector('[aria-modal="true"], [data-shortcut-menu="true"]')) return;
      event.preventDefault();
      closePopup();
      document.querySelector(`[data-sidebar-toggle="${popup}"]`)?.focus();
    };
    document.addEventListener('pointerdown', outside);
    document.addEventListener('keydown', escape);
    return () => {
      document.removeEventListener('pointerdown', outside);
      document.removeEventListener('keydown', escape);
    };
  }, [popup, closePopup]);

  return useMemo(() => ({ ...decision, shellWidth, popup, reportContent, togglePopup, closePopup, requestRightPopup }), [
    decision.active, decision.transcriptMinimum, decision.minimumChatWidth,
    decision.leftAutoCollapsed, decision.rightAutoCollapsed, decision.leftDocked, decision.rightDocked,
    decision.canDockLeft, decision.canDockRight,
    shellWidth, popup, reportContent, togglePopup, closePopup, requestRightPopup,
  ]);
}
