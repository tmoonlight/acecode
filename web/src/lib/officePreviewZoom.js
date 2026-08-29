export const OFFICE_PREVIEW_ZOOM_MIN = 0.5;
export const OFFICE_PREVIEW_ZOOM_MAX = 2;
export const OFFICE_PREVIEW_ZOOM_STEP = 0.1;
export const OFFICE_PREVIEW_ZOOM_DEFAULT = 1;

function roundedZoom(value) {
  return Math.round(value * 100) / 100;
}

export function clampOfficePreviewZoom(value) {
  const numeric = Number(value);
  if (!Number.isFinite(numeric)) return OFFICE_PREVIEW_ZOOM_DEFAULT;
  return roundedZoom(Math.min(
    OFFICE_PREVIEW_ZOOM_MAX,
    Math.max(OFFICE_PREVIEW_ZOOM_MIN, numeric),
  ));
}

export function stepOfficePreviewZoom(value, direction) {
  const delta = Math.sign(Number(direction) || 0);
  if (delta === 0) return clampOfficePreviewZoom(value);
  return clampOfficePreviewZoom(
    clampOfficePreviewZoom(value) + (delta * OFFICE_PREVIEW_ZOOM_STEP),
  );
}

export function officePreviewZoomForWheel(value, wheel = {}) {
  if (!wheel.ctrlKey) return clampOfficePreviewZoom(value);
  const deltaY = Number(wheel.deltaY) || 0;
  if (deltaY === 0) return clampOfficePreviewZoom(value);
  return stepOfficePreviewZoom(value, deltaY < 0 ? 1 : -1);
}

export function officePreviewLogicalSize(physicalSize, zoom, minimum = 1) {
  const size = Number(physicalSize);
  const floor = Math.max(1, Number(minimum) || 1);
  if (!Number.isFinite(size) || size <= 0) return floor;
  return Math.max(floor, Math.floor(size / clampOfficePreviewZoom(zoom)));
}
