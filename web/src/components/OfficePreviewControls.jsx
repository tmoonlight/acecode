import { useCallback, useEffect, useState } from 'react';
import {
  OFFICE_PREVIEW_ZOOM_DEFAULT,
  OFFICE_PREVIEW_ZOOM_MAX,
  OFFICE_PREVIEW_ZOOM_MIN,
  clampOfficePreviewZoom,
  officePreviewZoomForWheel,
  stepOfficePreviewZoom,
} from '../lib/officePreviewZoom.js';

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

export function officeZoomActions(zoom, zoomIn, zoomOut) {
  return [
    { label: '缩小', icon: 'ZoomOut', disabled: zoom <= OFFICE_PREVIEW_ZOOM_MIN, onSelect: zoomOut },
    { label: '放大', icon: 'ZoomIn', disabled: zoom >= OFFICE_PREVIEW_ZOOM_MAX, onSelect: zoomIn },
  ];
}
