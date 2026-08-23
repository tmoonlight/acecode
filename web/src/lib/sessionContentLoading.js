function finiteRect(rect) {
  if (!rect || typeof rect !== 'object') return null;
  const left = Number(rect.left);
  const top = Number(rect.top);
  const width = Number(rect.width);
  const height = Number(rect.height);
  if (![left, top, width, height].every(Number.isFinite)) return null;
  if (width <= 0 || height <= 0) return null;
  return { left, top, width, height };
}

export function sessionContentLoadingAnchorFrame(overlayRect, anchorRect) {
  const overlay = finiteRect(overlayRect);
  const anchor = finiteRect(anchorRect);
  if (!overlay || !anchor) return null;

  const left = Math.max(overlay.left, anchor.left);
  const top = Math.max(overlay.top, anchor.top);
  const right = Math.min(overlay.left + overlay.width, anchor.left + anchor.width);
  const bottom = Math.min(overlay.top + overlay.height, anchor.top + anchor.height);
  if (right <= left || bottom <= top) return null;

  return {
    left: left - overlay.left + (right - left) / 2,
    top: top - overlay.top + (bottom - top) / 2,
    width: right - left,
    height: bottom - top,
  };
}
