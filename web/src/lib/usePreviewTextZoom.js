import { useEffect, useRef } from 'react';
import { useWorkbenchState } from './useWorkbenchState.js';
import { clampOfficePreviewZoom, officePreviewZoomForWheel } from './officePreviewZoom.js';

export function usePreviewTextZoom(owner) {
  const panelRef = useRef(null);
  const [storedZoom, setZoom] = useWorkbenchState(owner, 'previewTextZoom', 1);
  const zoom = clampOfficePreviewZoom(storedZoom);
  useEffect(() => {
    const panel = panelRef.current;
    if (!panel) return undefined;
    const wheel = (event) => {
      if (!event.ctrlKey || event.defaultPrevented) return;
      // A React wheel listener is passive in Chromium; attach locally so even
      // a wheel at the zoom limit or on detail chrome cannot scale the app.
      // Office consumes its own wheel first; native viewers own their content.
      event.preventDefault();
      event.stopPropagation();
      if (!panel.querySelector('.ace-side-preview-code, [data-desktop-review-kind]')) return;
      setZoom((current) => officePreviewZoomForWheel(current, event));
    };
    panel.addEventListener('wheel', wheel, { passive: false });
    return () => panel.removeEventListener('wheel', wheel);
  }, [setZoom]);
  return { panelRef, zoom };
}
