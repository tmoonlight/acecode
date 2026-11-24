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
export const NATIVE_SURFACE_INTERACTION_SELECTOR = '[data-ace-native-interaction="disabled"]';
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
  // Square corners describe which side of a hole the surface cut, so they only
  // stay valid for the exact rectangle they were measured on.
  if (first.square_corners || second.square_corners) return null;
  const sameRadius = (first.radius || 0) === (second.radius || 0);
  if (!sameRadius) return null;

  const sameVerticalSpan = first.y === second.y && first.height === second.height;
  const horizontallyConnected = first.x <= occlusionRectRight(second)
    && second.x <= occlusionRectRight(first);
  if (sameVerticalSpan && horizontallyConnected) {
    const x = Math.min(first.x, second.x);
    const right = Math.max(occlusionRectRight(first), occlusionRectRight(second));
    const merged = { x, y: first.y, width: right - x, height: first.height };
    if (first.radius) merged.radius = first.radius;
    return merged;
  }

  const sameHorizontalSpan = first.x === second.x && first.width === second.width;
  const verticallyConnected = first.y <= occlusionRectBottom(second)
    && second.y <= occlusionRectBottom(first);
  if (sameHorizontalSpan && verticallyConnected) {
    const y = Math.min(first.y, second.y);
    const bottom = Math.max(occlusionRectBottom(first), occlusionRectBottom(second));
    const merged = { x: first.x, y, width: first.width, height: bottom - y };
    if (first.radius) merged.radius = first.radius;
    return merged;
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

// A WebUI overlay is painted by the main WebView underneath the native browser
// HWND; the native view is clipped to an occlusion "hole" so the overlay shows
// through. When that hole is exactly the overlay's border box, a non-antialiased
// native region clips the overlay's own 1px border/shadow at the edges and
// rounded corners. `data-ace-native-overlay-bleed` grows the hole (in CSS px) so
// the border and separation shadow are drawn on the intact WebUI surface.
export function expandClientRect(rect, amount = 0) {
  const normalized = normalizedClientRect(rect);
  const bleed = Math.max(0, Math.min(8, finiteNumber(amount)));
  if (!bleed) return normalized;
  return {
    left: normalized.left - bleed,
    top: normalized.top - bleed,
    right: normalized.right + bleed,
    bottom: normalized.bottom + bleed,
    width: normalized.width + bleed * 2,
    height: normalized.height + bleed * 2,
  };
}

// Menus, listboxes and tooltips are thin floating surfaces whose 1px outline
// sits on the border box itself. The hole is now snapped outward, so the
// outline stays inside it on its own; a single extra CSS pixel only widens the
// cut far enough that a menu hanging over the Browser keeps its bottom and
// right edge after DPI scaling. Anything bigger shows the WebUI surface as a
// light halo around the surface once the page behind it is not white.
export const NATIVE_SURFACE_IMPLICIT_OVERLAY_BLEED = 1;

export function overlayUsesSquareHole(element) {
  return element?.getAttribute?.('data-ace-native-overlay-square') === 'true';
}

// A native window region is a 1-bit bitmap, so a rounded hole always has
// stepped corners no matter how closely its radius matches the CSS surface.
// Annotation surfaces opt into a rectangular hole and let Chromium draw the
// rounded border; the extra ring shows the page background instead of a
// sawtooth of native region pixels.
export const NATIVE_SURFACE_SQUARE_OVERLAY_BLEED = 1;

export function overlayBleed(element) {
  const declared = element?.getAttribute?.('data-ace-native-overlay-bleed');
  if (declared !== null && declared !== undefined) {
    return Math.max(0, Math.min(8, Math.round(finiteNumber(declared))));
  }
  if (overlayUsesSquareHole(element)) return NATIVE_SURFACE_SQUARE_OVERLAY_BLEED;
  return element?.matches?.(NATIVE_SURFACE_IMPLICIT_OVERLAY_SELECTOR)
    ? NATIVE_SURFACE_IMPLICIT_OVERLAY_BLEED
    : 0;
}

export const OCCLUSION_CORNER_TOP_LEFT = 1;
export const OCCLUSION_CORNER_TOP_RIGHT = 2;
export const OCCLUSION_CORNER_BOTTOM_RIGHT = 4;
export const OCCLUSION_CORNER_BOTTOM_LEFT = 8;
const CORNER_EDGE_TOLERANCE = 0.5;

// A window region is exclusive on its far edges: a hole that ends on the last
// pixel column of the overlay still leaves that column under the native view,
// which is where the overlay's 1px outline and its rounded corner live. One
// extra device pixel on the right and bottom keeps that edge whole without
// opening a visible gap around the other two sides.
const HOLE_FAR_EDGE_GUARD = 1;

// The occlusion hole is the overlay's border box clipped to the surface, so a
// corner may belong to the surface edge instead of the overlay. Rounding such a
// clipped corner paints a quarter-circle of Browser content across the overlay
// (a tab-strip menu hanging over the page gets a curved seam where the page
// begins). Corners that the surface cut are reported so the native side keeps
// them square.
export function occlusionSquareCorners(
  overlayRect,
  holeRect,
  tolerance = CORNER_EDGE_TOLERANCE,
) {
  const overlay = normalizedClientRect(overlayRect);
  const hole = normalizedClientRect(holeRect);
  if (!clientRectHasArea(hole)) return 0;
  const clippedLeft = hole.left - overlay.left > tolerance;
  const clippedTop = hole.top - overlay.top > tolerance;
  const clippedRight = overlay.right - hole.right > tolerance;
  const clippedBottom = overlay.bottom - hole.bottom > tolerance;
  let corners = 0;
  if (clippedLeft || clippedTop) corners |= OCCLUSION_CORNER_TOP_LEFT;
  if (clippedRight || clippedTop) corners |= OCCLUSION_CORNER_TOP_RIGHT;
  if (clippedRight || clippedBottom) corners |= OCCLUSION_CORNER_BOTTOM_RIGHT;
  if (clippedLeft || clippedBottom) corners |= OCCLUSION_CORNER_BOTTOM_LEFT;
  return corners;
}

export function occlusionSquareCornerMask(value) {
  return Math.max(0, Math.min(15, Math.round(finiteNumber(value))));
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
    .map((rect) => {
      const normalized = {
        x: Math.max(0, Math.round(finiteNumber(rect?.x))),
        y: Math.max(0, Math.round(finiteNumber(rect?.y))),
        width: Math.max(0, Math.round(finiteNumber(rect?.width))),
        height: Math.max(0, Math.round(finiteNumber(rect?.height))),
      };
      const radius = Math.max(0, Math.round(finiteNumber(rect?.radius)));
      if (radius > 0) normalized.radius = radius;
      const squareCorners = occlusionSquareCornerMask(rect?.square_corners);
      if (squareCorners) normalized.square_corners = squareCorners;
      return normalized;
    })
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
    // Snap the hole outward. A window region is a whole-pixel bitmap, so the
    // overlay edge never lands on a pixel boundary; rounding to the nearest
    // pixel drops every device pixel that only partly belongs to the overlay,
    // which is exactly the row/column that carries its 1px outline.
    // Left/top take the enclosing pixel. Right/bottom need one more: a region
    // is exclusive there, so ending on the overlay's last pixel column still
    // leaves that column under the native view and erases the outline.
    const left = Math.max(
      0,
      Math.min(surfacePixelWidth, Math.floor(intersection.left * scale) - surfacePixelLeft),
    );
    const top = Math.max(
      0,
      Math.min(surfacePixelHeight, Math.floor(intersection.top * scale) - surfacePixelTop),
    );
    const right = Math.max(
      left,
      Math.min(
        surfacePixelWidth,
        Math.ceil(intersection.right * scale) - surfacePixelLeft + HOLE_FAR_EDGE_GUARD,
      ),
    );
    const bottom = Math.max(
      top,
      Math.min(
        surfacePixelHeight,
        Math.ceil(intersection.bottom * scale) - surfacePixelTop + HOLE_FAR_EDGE_GUARD,
      ),
    );
    const width = right - left;
    const height = bottom - top;
    const occlusion = {
      x: left,
      y: top,
      width,
      height,
    };
    const radius = Math.max(0, Math.round(finiteNumber(overlayRect?.radius) * scale));
    const clampedRadius = Math.min(radius, Math.floor(Math.min(width, height) / 2));
    if (clampedRadius > 0) occlusion.radius = clampedRadius;
    const squareCorners = occlusionSquareCornerMask(overlayRect?.square_corners);
    if (squareCorners) occlusion.square_corners = squareCorners;
    occlusions.push(occlusion);
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
    layout.interaction_blocked ? 1 : 0,
  ].join(':');
  const occlusions = normalizeAgentBrowserOcclusionRects(layout.occlusion_rects)
    .map((rect) => {
      const geometry = `${rect.x},${rect.y},${rect.width},${rect.height}`;
      const radius = rect.radius ? `,${rect.radius}` : '';
      const square = rect.square_corners ? `,s${rect.square_corners}` : '';
      return `${geometry}${radius}${square}`;
    })
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

function computedPixelValue(style, property, camelCase) {
  const direct = style?.[camelCase];
  const value = direct !== undefined && direct !== ''
    ? direct
    : style?.getPropertyValue?.(property);
  const pixels = Number.parseFloat(String(value ?? ''));
  return Number.isFinite(pixels) && pixels > 0 ? pixels : 0;
}

function overlayCornerRadius(element, win) {
  const style = win?.getComputedStyle?.(element);
  if (!style) return 0;
  return Math.max(
    computedPixelValue(style, 'border-top-left-radius', 'borderTopLeftRadius'),
    computedPixelValue(style, 'border-top-right-radius', 'borderTopRightRadius'),
    computedPixelValue(style, 'border-bottom-right-radius', 'borderBottomRightRadius'),
    computedPixelValue(style, 'border-bottom-left-radius', 'borderBottomLeftRadius'),
    computedPixelValue(style, 'border-radius', 'borderRadius'),
  );
}

export function nativeSurfaceInteractionBlockedByDocument(
  surfaceRect,
  doc = globalThis.document,
  win = globalThis.window,
) {
  if (!doc?.querySelectorAll) return false;
  for (const element of doc.querySelectorAll(NATIVE_SURFACE_INTERACTION_SELECTOR)) {
    if (!overlayElementIsVisible(element, win)) continue;
    const rect = element.getBoundingClientRect?.();
    if (intersectClientRects(surfaceRect, rect)) return true;
  }
  return false;
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
    const bleed = overlayBleed(overlay);
    const measured = overlay.getBoundingClientRect?.();
    // Implicit menus/tooltips and square-hole annotation surfaces grow by one
    // CSS pixel; other registered overlays grow only when they ask for it.
    const overlayRect = bleed > 0 ? expandClientRect(measured, bleed) : measured;
    const intersection = intersectClientRects(surfaceRect, overlayRect);
    if (!intersection) continue;
    // Registration is authoritative. The Browser is a separate native view and
    // has no faithful representative in document.elementFromPoint(), so DOM
    // hit testing must not veto a floating surface that explicitly promises to
    // appear above it.
    const radius = overlayUsesSquareHole(overlay)
      ? 0
      : overlayCornerRadius(overlay, win) + bleed;
    const occlusion = { ...intersection };
    if (radius > 0) occlusion.radius = radius;
    const squareCorners = radius > 0 ? occlusionSquareCorners(overlayRect, intersection) : 0;
    if (squareCorners) occlusion.square_corners = squareCorners;
    result.occlusionRects.push(occlusion);
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
