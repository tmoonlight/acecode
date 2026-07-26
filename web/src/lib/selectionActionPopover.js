function finite(value, fallback = 0) {
  const number = Number(value);
  return Number.isFinite(number) ? number : fallback;
}

function clamp(value, min, max) {
  return Math.min(Math.max(value, min), Math.max(min, max));
}

export function selectionActionPopoverPosition(
  anchorRect,
  popoverSize,
  viewportSize,
  {
    gap = 8,
    margin = 8,
  } = {},
) {
  const anchor = anchorRect || {};
  const popover = popoverSize || {};
  const viewport = viewportSize || {};
  const width = Math.max(1, finite(popover.width, 180));
  const height = Math.max(1, finite(popover.height, 36));
  const viewportWidth = Math.max(width + margin * 2, finite(viewport.width, width + margin * 2));
  const viewportHeight = Math.max(height + margin * 2, finite(viewport.height, height + margin * 2));
  const anchorLeft = finite(anchor.left);
  const anchorTop = finite(anchor.top);
  const anchorWidth = Math.max(0, finite(anchor.width, finite(anchor.right) - anchorLeft));
  const anchorBottom = finite(anchor.bottom, anchorTop + Math.max(0, finite(anchor.height)));

  let placement = 'below';
  let top = anchorBottom + gap;
  if (top + height > viewportHeight - margin && anchorTop - gap - height >= margin) {
    placement = 'above';
    top = anchorTop - gap - height;
  }
  const left = clamp(
    anchorLeft + anchorWidth / 2 - width / 2,
    margin,
    viewportWidth - margin - width,
  );
  top = clamp(top, margin, viewportHeight - margin - height);
  return {
    left: Math.round(left),
    top: Math.round(top),
    placement,
  };
}

export function selectionRangeViewportRect(selection = globalThis.window?.getSelection?.()) {
  if (!selection || selection.rangeCount <= 0 || selection.isCollapsed) return null;
  try {
    const range = selection.getRangeAt(0);
    const rect = range.getBoundingClientRect?.();
    if (!rect) return null;
    const fallbackRect = Array.from(range.getClientRects?.() || [])
      .find((candidate) => candidate.width || candidate.height);
    const chosen = (rect.width || rect.height) ? rect : fallbackRect;
    if (!chosen) return null;
    return {
      left: finite(chosen.left),
      top: finite(chosen.top),
      right: finite(chosen.right, finite(chosen.left) + finite(chosen.width)),
      bottom: finite(chosen.bottom, finite(chosen.top) + finite(chosen.height)),
      width: Math.max(0, finite(chosen.width)),
      height: Math.max(0, finite(chosen.height)),
    };
  } catch {
    return null;
  }
}
