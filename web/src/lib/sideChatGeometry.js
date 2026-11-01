const MARGIN = 12;
const MIN_WIDTH = 300;
const MIN_HEIGHT = 260;
const DEFAULT_WIDTH = 600;
const DEFAULT_HEIGHT = 620;

const finite = (value, fallback = 0) => Number.isFinite(value) ? value : fallback;
const clamp = (value, min, max) => Math.max(min, Math.min(value, max));

function bounds(viewport = {}) {
  const width = Math.max(0, finite(viewport.width));
  const height = Math.max(0, finite(viewport.height));
  const marginX = Math.min(MARGIN, width / 2);
  const marginY = Math.min(MARGIN, height / 2);
  const left = finite(viewport.left) + marginX;
  const top = finite(viewport.top) + marginY;
  return {
    left,
    top,
    right: left + Math.max(0, width - marginX * 2),
    bottom: top + Math.max(0, height - marginY * 2),
    minWidth: Math.min(MIN_WIDTH, Math.max(0, width - marginX * 2)),
    minHeight: Math.min(MIN_HEIGHT, Math.max(0, height - marginY * 2)),
  };
}

// Keep every edge reachable after resizing the viewport or opening a soft keyboard.
export function clampSideChatGeometry(rect = {}, viewport = {}) {
  const area = bounds(viewport);
  const width = clamp(finite(rect.width, DEFAULT_WIDTH), area.minWidth, area.right - area.left);
  const height = clamp(finite(rect.height, DEFAULT_HEIGHT), area.minHeight, area.bottom - area.top);
  return {
    left: clamp(finite(rect.left, area.left), area.left, area.right - width),
    top: clamp(finite(rect.top, area.top), area.top, area.bottom - height),
    width,
    height,
  };
}

export function createSideChatGeometry(viewport = {}) {
  const rect = clampSideChatGeometry({}, viewport);
  return clampSideChatGeometry({
    ...rect,
    left: finite(viewport.left) + (finite(viewport.width) - rect.width) / 2,
    top: finite(viewport.top) + (finite(viewport.height) - rect.height) / 2,
  }, viewport);
}

export function anchorSideChatGeometry(rect, anchor, viewport) {
  // Keep the toolbar toggle reachable when the window must shrink to fit.
  const area = bounds(viewport);
  const top = clamp(finite(anchor.top, area.top), area.top, area.bottom - area.minHeight);
  return clampSideChatGeometry({
    ...rect,
    left: anchor.left,
    top,
    height: Math.min(rect.height, area.bottom - top),
  }, viewport);
}

export function moveSideChatGeometry(rect, deltaX, deltaY, viewport) {
  return clampSideChatGeometry({
    ...rect,
    left: rect.left + finite(deltaX),
    top: rect.top + finite(deltaY),
  }, viewport);
}

// Resizing clamps the moving edge while preserving the opposite edge. Applying
// a generic rectangle clamp afterward would make west/north resizing drift.
export function resizeSideChatGeometry(rect, direction, deltaX, deltaY, viewport) {
  const area = bounds(viewport);
  const initial = clampSideChatGeometry(rect, viewport);
  let { left, top } = initial;
  let right = left + initial.width;
  let bottom = top + initial.height;
  const dx = finite(deltaX);
  const dy = finite(deltaY);
  if (direction.includes('w')) left = clamp(left + dx, area.left, right - area.minWidth);
  if (direction.includes('e')) right = clamp(right + dx, left + area.minWidth, area.right);
  if (direction.includes('n')) top = clamp(top + dy, area.top, bottom - area.minHeight);
  if (direction.includes('s')) bottom = clamp(bottom + dy, top + area.minHeight, area.bottom);
  return { left, top, width: right - left, height: bottom - top };
}
