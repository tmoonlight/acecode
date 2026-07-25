const DEFAULT_MARGIN = 12;
const DEFAULT_GAP = 4;
const DEFAULT_WIDTH = 440;

function finite(value, fallback = 0) {
  const number = Number(value);
  return Number.isFinite(number) ? number : fallback;
}

function clamp(value, minimum, maximum) {
  if (maximum < minimum) return minimum;
  return Math.min(Math.max(value, minimum), maximum);
}

export function placeExpertSubmenu({
  anchorRect,
  menuRect,
  viewportWidth,
  viewportHeight,
  margin = DEFAULT_MARGIN,
  gap = DEFAULT_GAP,
  preferredWidth = DEFAULT_WIDTH,
} = {}) {
  const vw = Math.max(0, finite(viewportWidth));
  const vh = Math.max(0, finite(viewportHeight));
  const safeMargin = Math.max(0, finite(margin, DEFAULT_MARGIN));
  const safeGap = Math.max(0, finite(gap, DEFAULT_GAP));
  const anchor = anchorRect || {};
  const menu = menuRect || {};
  const availableWidth = Math.max(0, vw - safeMargin * 2);
  const availableHeight = Math.max(0, vh - safeMargin * 2);
  const width = Math.min(
    Math.max(0, finite(preferredWidth, DEFAULT_WIDTH)),
    availableWidth,
  );
  const measuredHeight = Math.max(0, finite(menu.height, 0));
  const height = Math.min(measuredHeight, availableHeight);
  const anchorLeft = finite(anchor.left);
  const anchorRight = finite(anchor.right, anchorLeft);
  const anchorBottom = finite(anchor.bottom);
  const rightLeft = anchorRight + safeGap;
  const leftLeft = anchorLeft - safeGap - width;
  const rightFits = rightLeft + width <= vw - safeMargin;
  const leftFits = leftLeft >= safeMargin;
  const side = rightFits || !leftFits ? 'right' : 'left';
  const naturalLeft = side === 'right' ? rightLeft : leftLeft;
  const left = clamp(naturalLeft, safeMargin, vw - safeMargin - width);
  const top = clamp(
    anchorBottom - height,
    safeMargin,
    vh - safeMargin - height,
  );

  return {
    top: Math.round(top),
    left: Math.round(left),
    width: Math.round(width),
    maxHeight: Math.round(availableHeight),
    side,
  };
}

export function nextExpertMenuItemIndex(key, currentIndex, itemCount) {
  const count = Math.max(0, Number(itemCount) || 0);
  if (count === 0) return -1;
  const current = Math.min(Math.max(Number(currentIndex) || 0, 0), count - 1);
  if (key === 'Home') return 0;
  if (key === 'End') return count - 1;
  if (key === 'ArrowDown') return (current + 1) % count;
  if (key === 'ArrowUp') return (current - 1 + count) % count;
  return current;
}
