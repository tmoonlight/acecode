export const NATIVE_SURFACE_OVERLAY_EVENT = 'acecode:native-surface-overlay-change';
export const NATIVE_SURFACE_IMPLICIT_OVERLAY_SELECTOR = [
  '[role="menu"]',
  '[role="listbox"]',
  '[role="tooltip"]',
].join(',');
export const NATIVE_SURFACE_OVERLAY_SELECTOR = [
  '[data-ace-native-overlay]',
  NATIVE_SURFACE_IMPLICIT_OVERLAY_SELECTOR,
].join(',');
const LAYOUT_REVISION_CLOCK_MULTIPLIER = 1024;
const LAYOUT_REVISION_STORAGE_KEY = 'acecode.agentBrowserLayoutRevision.v1';

function storedLayoutRevision() {
  try {
    return Math.max(0, Math.trunc(finiteNumber(
      globalThis.window?.sessionStorage?.getItem(LAYOUT_REVISION_STORAGE_KEY),
    )));
  } catch {
    return 0;
  }
}

function persistLayoutRevision(revision) {
  try {
    globalThis.window?.sessionStorage?.setItem(LAYOUT_REVISION_STORAGE_KEY, String(revision));
  } catch {
    // Private or restricted WebViews can reject sessionStorage writes.
  }
}

let lastAllocatedLayoutRevision = Math.max(
  Math.trunc(Date.now()) * LAYOUT_REVISION_CLOCK_MULTIPLIER,
  storedLayoutRevision(),
);

function finiteNumber(value, fallback = 0) {
  const number = Number(value);
  return Number.isFinite(number) ? number : fallback;
}

export function nativeSurfaceSupportsLocalOcclusion(os = '') {
  return os === 'windows' || os === 'macos';
}

function occlusionRectRight(rect) {
  return rect.x + rect.width;
}

function occlusionRectBottom(rect) {
  return rect.y + rect.height;
}

function occlusionRectContains(outer, inner) {
  return outer.x <= inner.x
    && outer.y <= inner.y
    && occlusionRectRight(outer) >= occlusionRectRight(inner)
    && occlusionRectBottom(outer) >= occlusionRectBottom(inner);
}

function mergeCompatibleOcclusionRects(first, second) {
  if (occlusionRectContains(first, second)) return first;
  if (occlusionRectContains(second, first)) return second;

  const sameVerticalSpan = first.y === second.y && first.height === second.height;
  const horizontallyConnected = first.x <= occlusionRectRight(second)
    && second.x <= occlusionRectRight(first);
  if (sameVerticalSpan && horizontallyConnected) {
    const x = Math.min(first.x, second.x);
    const right = Math.max(occlusionRectRight(first), occlusionRectRight(second));
    return { x, y: first.y, width: right - x, height: first.height };
  }

  const sameHorizontalSpan = first.x === second.x && first.width === second.width;
  const verticallyConnected = first.y <= occlusionRectBottom(second)
    && second.y <= occlusionRectBottom(first);
  if (sameHorizontalSpan && verticallyConnected) {
    const y = Math.min(first.y, second.y);
    const bottom = Math.max(occlusionRectBottom(first), occlusionRectBottom(second));
    return { x: first.x, y, width: first.width, height: bottom - y };
  }
  return null;
}

export function normalizedClientRect(rect = {}) {
  const left = finiteNumber(rect.left);
  const top = finiteNumber(rect.top);
  const right = finiteNumber(rect.right, left + Math.max(0, finiteNumber(rect.width)));
  const bottom = finiteNumber(rect.bottom, top + Math.max(0, finiteNumber(rect.height)));
  const width = Math.max(0, finiteNumber(rect.width, right - left));
  const height = Math.max(0, finiteNumber(rect.height, bottom - top));
  return {
    left,
    top,
    right: left + width,
    bottom: top + height,
    width,
    height,
  };
}

export function clientRectHasArea(rect) {
  const normalized = normalizedClientRect(rect);
  return normalized.width > 0 && normalized.height > 0;
}

export function intersectClientRects(first, second) {
  const a = normalizedClientRect(first);
  const b = normalizedClientRect(second);
  const left = Math.max(a.left, b.left);
  const top = Math.max(a.top, b.top);
  const right = Math.min(a.right, b.right);
  const bottom = Math.min(a.bottom, b.bottom);
  if (right <= left || bottom <= top) return null;
  return {
    left,
    top,
    right,
    bottom,
    width: right - left,
    height: bottom - top,
  };
}

export function clientRectsOverlap(first, second) {
  return intersectClientRects(first, second) !== null;
}

export function nativeSurfaceOverlayBlocks({
  mode,
  surfaceRect,
  overlayRect,
  overlayVisible = true,
  topmost = true,
} = {}) {
  if (!overlayVisible) return false;
  if (mode === 'blocking') return true;
  if (mode !== 'overlap') return false;
  return !!topmost && clientRectsOverlap(surfaceRect, overlayRect);
}

export function normalizeAgentBrowserOcclusionRects(rects = []) {
  const pending = (Array.isArray(rects) ? rects : [])
    .map((rect) => ({
      x: Math.max(0, Math.round(finiteNumber(rect?.x))),
      y: Math.max(0, Math.round(finiteNumber(rect?.y))),
      width: Math.max(0, Math.round(finiteNumber(rect?.width))),
      height: Math.max(0, Math.round(finiteNumber(rect?.height))),
    }))
    .filter((rect) => rect.width > 0 && rect.height > 0)
    .sort((a, b) => (
      a.x - b.x
      || a.y - b.y
      || a.width - b.width
      || a.height - b.height
    ));
  const merged = [];
  for (const rect of pending) {
    let candidate = rect;
    let mergedAnother = true;
    while (mergedAnother) {
      mergedAnother = false;
      for (let index = 0; index < merged.length; index += 1) {
        const combined = mergeCompatibleOcclusionRects(merged[index], candidate);
        if (!combined) continue;
        merged.splice(index, 1);
        candidate = combined;
        mergedAnother = true;
        break;
      }
    }
    merged.push(candidate);
  }
  return merged.sort((a, b) => (
    a.x - b.x
    || a.y - b.y
    || a.width - b.width
    || a.height - b.height
  ));
}

export function nativeSurfaceOcclusionRectsFromClientRects(
  surfaceRect,
  overlayRects = [],
  devicePixelRatio = 1,
) {
  const surface = normalizedClientRect(surfaceRect);
  if (!clientRectHasArea(surface)) return [];
  const scale = finiteNumber(devicePixelRatio, 1) > 0
    ? finiteNumber(devicePixelRatio, 1)
    : 1;
  const surfacePixelLeft = Math.round(surface.left * scale);
  const surfacePixelTop = Math.round(surface.top * scale);
  const surfacePixelWidth = Math.max(0, Math.round(surface.width * scale));
  const surfacePixelHeight = Math.max(0, Math.round(surface.height * scale));
  const occlusions = [];
  for (const overlayRect of Array.isArray(overlayRects) ? overlayRects : []) {
    const intersection = intersectClientRects(surface, overlayRect);
    if (!intersection) continue;
    const left = Math.max(
      0,
      Math.min(surfacePixelWidth, Math.round(intersection.left * scale) - surfacePixelLeft),
    );
    const top = Math.max(
      0,
      Math.min(surfacePixelHeight, Math.round(intersection.top * scale) - surfacePixelTop),
    );
    const right = Math.max(
      left,
      Math.min(surfacePixelWidth, Math.round(intersection.right * scale) - surfacePixelLeft),
    );
    const bottom = Math.max(
      top,
      Math.min(surfacePixelHeight, Math.round(intersection.bottom * scale) - surfacePixelTop),
    );
    occlusions.push({
      x: left,
      y: top,
      width: right - left,
      height: bottom - top,
    });
  }
  return normalizeAgentBrowserOcclusionRects(occlusions);
}

export function nativeSurfaceShouldShow({
  applicationVisible = true,
  detailsVisible = true,
  tabActive = true,
  pageLive = false,
  documentVisible = true,
  surfaceRect,
  viewportRect,
  overlayBlocked = false,
} = {}) {
  if (!applicationVisible
      || !detailsVisible
      || !tabActive
      || !pageLive
      || !documentVisible
      || overlayBlocked
      || !clientRectHasArea(surfaceRect)) {
    return false;
  }
  return !viewportRect || clientRectsOverlap(surfaceRect, viewportRect);
}

export function agentBrowserLayoutSignature(layout = {}) {
  const bounds = [
    Math.max(0, Math.round(finiteNumber(layout.x))),
    Math.max(0, Math.round(finiteNumber(layout.y))),
    Math.max(0, Math.round(finiteNumber(layout.width))),
    Math.max(0, Math.round(finiteNumber(layout.height))),
    layout.visible ? 1 : 0,
  ].join(':');
  const occlusions = normalizeAgentBrowserOcclusionRects(layout.occlusion_rects)
    .map((rect) => `${rect.x},${rect.y},${rect.width},${rect.height}`)
    .join(';');
  return `${bounds}:${occlusions}`;
}

export function nativeSurfaceLayoutWithOcclusionFallback(
  layout = {},
  failedOcclusionSignature = '',
) {
  const normalizedLayout = {
    ...layout,
    occlusion_rects: normalizeAgentBrowserOcclusionRects(layout.occlusion_rects),
  };
  const idealSignature = agentBrowserLayoutSignature(normalizedLayout);
  const fallback = !!normalizedLayout.visible
    && normalizedLayout.occlusion_rects.length > 0
    && idealSignature === failedOcclusionSignature;
  return {
    fallback,
    idealSignature,
    layout: fallback
      ? { ...normalizedLayout, visible: false, occlusion_rects: [] }
      : normalizedLayout,
  };
}

export function failedNativeSurfaceOcclusionSignature({
  result,
  request,
  requestSignature = '',
  currentRevision = 0,
} = {}) {
  if (!request || request.layout_revision !== currentRevision) return '';
  if (result?.ok === true || !request.visible) return '';
  if (normalizeAgentBrowserOcclusionRects(request.occlusion_rects).length === 0) return '';
  return requestSignature || agentBrowserLayoutSignature(request);
}

export function allocateAgentBrowserLayoutRevision(minimum = 0) {
  const clockRevision = Math.trunc(Date.now()) * LAYOUT_REVISION_CLOCK_MULTIPLIER;
  lastAllocatedLayoutRevision = Math.max(
    lastAllocatedLayoutRevision,
    Math.max(0, Math.trunc(finiteNumber(minimum))),
    clockRevision,
  ) + 1;
  persistLayoutRevision(lastAllocatedLayoutRevision);
  return lastAllocatedLayoutRevision;
}

export function nextAgentBrowserLayoutRequest(
  layout,
  previous = {},
  { force = false, allocateRevision } = {},
) {
  const signature = agentBrowserLayoutSignature(layout);
  const previousRevision = Math.max(0, Math.trunc(finiteNumber(previous.revision)));
  if (!force && signature === previous.signature) {
    return {
      changed: false,
      signature,
      revision: previousRevision,
      request: null,
    };
  }
  const revision = typeof allocateRevision === 'function'
    ? allocateRevision(previousRevision)
    : previousRevision + 1;
  return {
    changed: true,
    signature,
    revision,
    request: {
      ...layout,
      layout_revision: revision,
    },
  };
}

export function nativeSurfaceViewportRect(win = globalThis.window) {
  const visualViewport = win?.visualViewport;
  if (visualViewport) {
    return normalizedClientRect({
      left: visualViewport.offsetLeft,
      top: visualViewport.offsetTop,
      width: visualViewport.width,
      height: visualViewport.height,
    });
  }
  return normalizedClientRect({
    left: 0,
    top: 0,
    width: win?.innerWidth,
    height: win?.innerHeight,
  });
}

function overlayElementIsVisible(element, win) {
  if (!element || element.hidden || element.getAttribute?.('aria-hidden') === 'true') return false;
  const style = win?.getComputedStyle?.(element);
  if (style?.display === 'none' || style?.visibility === 'hidden' || style?.visibility === 'collapse') {
    return false;
  }
  if (style && finiteNumber(style.opacity, 1) <= 0) return false;
  return true;
}

export function nativeSurfaceOverlayGeometryByDocument(
  surfaceRect,
  doc = globalThis.document,
  win = globalThis.window,
) {
  const result = { blocking: false, occlusionRects: [] };
  if (!doc?.querySelectorAll) return result;
  const overlays = doc.querySelectorAll(NATIVE_SURFACE_OVERLAY_SELECTOR);
  for (const overlay of overlays) {
    const declaredMode = overlay.getAttribute?.('data-ace-native-overlay') || '';
    const implicitOverlap = !declaredMode
      && overlay.matches?.(NATIVE_SURFACE_IMPLICIT_OVERLAY_SELECTOR);
    const mode = declaredMode || (implicitOverlap ? 'overlap' : '');
    if (!overlayElementIsVisible(overlay, win)) continue;
    if (mode === 'blocking') return { blocking: true, occlusionRects: [] };
    if (mode !== 'overlap') continue;
    const overlayRect = overlay.getBoundingClientRect?.();
    const intersection = intersectClientRects(surfaceRect, overlayRect);
    if (!intersection) continue;
    // Registration is authoritative. The Browser is a separate native view and
    // has no faithful representative in document.elementFromPoint(), so DOM
    // hit testing must not veto a floating surface that explicitly promises to
    // appear above it.
    result.occlusionRects.push(intersection);
  }
  return result;
}

export function nativeSurfaceBlockedByDocument(
  surfaceRect,
  doc = globalThis.document,
  win = globalThis.window,
) {
  const geometry = nativeSurfaceOverlayGeometryByDocument(surfaceRect, doc, win);
  return geometry.blocking || geometry.occlusionRects.length > 0;
}

export function notifyNativeSurfaceOverlayChange(win = globalThis.window) {
  const EventConstructor = win?.Event;
  if (!win?.dispatchEvent || typeof EventConstructor !== 'function') return;
  win.dispatchEvent(new EventConstructor(NATIVE_SURFACE_OVERLAY_EVENT));
}
