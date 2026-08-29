import { useCallback, useEffect, useState } from 'react';
import {
  OFFICE_PREVIEW_ZOOM_DEFAULT,
  OFFICE_PREVIEW_ZOOM_MAX,
  OFFICE_PREVIEW_ZOOM_MIN,
  clampOfficePreviewZoom,
  officePreviewZoomForWheel,
  stepOfficePreviewZoom,
} from '../lib/officePreviewZoom.js';
import { VsIcon } from './Icon.jsx';

export function useOfficePreviewZoom(identity = '', surfaceRef = null) {
  const [zoom, setZoomState] = useState(OFFICE_PREVIEW_ZOOM_DEFAULT);

  useEffect(() => {
    setZoomState(OFFICE_PREVIEW_ZOOM_DEFAULT);
  }, [identity]);

  const setZoom = useCallback((value) => {
    setZoomState((current) => clampOfficePreviewZoom(
      typeof value === 'function' ? value(current) : value,
    ));
  }, []);

  const zoomIn = useCallback(() => {
    setZoomState((current) => stepOfficePreviewZoom(current, 1));
  }, []);

  const zoomOut = useCallback(() => {
    setZoomState((current) => stepOfficePreviewZoom(current, -1));
  }, []);

  const onWheel = useCallback((event) => {
    if (!event.ctrlKey) return;
    event.preventDefault();
    event.stopPropagation();
    setZoomState((current) => officePreviewZoomForWheel(current, event));
  }, []);

  useEffect(() => {
    const surface = surfaceRef?.current;
    if (!surface) return undefined;
    surface.addEventListener('wheel', onWheel, { passive: false, capture: true });
    return () => surface.removeEventListener('wheel', onWheel, true);
  }, [identity, onWheel, surfaceRef]);

  return {
    zoom,
    setZoom,
    zoomIn,
    zoomOut,
    onWheel,
  };
}

function MagnifierZoomIcon({ operation }) {
  return (
    <span className="ace-office-preview-zoom-glyph" aria-hidden="true">
      <VsIcon name="search" size={16} />
      <span className="ace-office-preview-zoom-modifier">
        {operation === 'in' ? '+' : '−'}
      </span>
    </span>
  );
}

export function OfficePreviewControls({ zoom, onZoomIn, onZoomOut }) {
  return (
    <div className="ace-office-preview-controls" data-ace-office-preview-controls="true">
      <button
        type="button"
        className="ace-office-preview-zoom-button"
        title="缩小"
        aria-label="缩小"
        disabled={zoom <= OFFICE_PREVIEW_ZOOM_MIN}
        onClick={onZoomOut}
      >
        <MagnifierZoomIcon operation="out" />
      </button>
      <button
        type="button"
        className="ace-office-preview-zoom-button"
        title="放大"
        aria-label="放大"
        disabled={zoom >= OFFICE_PREVIEW_ZOOM_MAX}
        onClick={onZoomIn}
      >
        <MagnifierZoomIcon operation="in" />
      </button>
    </div>
  );
}
