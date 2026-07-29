export const DROPDOWN_GAP_PX = 8;
export const DROPDOWN_VIEWPORT_MARGIN_PX = 8;
export const DROPDOWN_PLACEMENT_HYSTERESIS_PX = 8;

function finiteNumber(value, fallback = 0) {
  return Number.isFinite(value) ? value : fallback;
}

function nonNegativeNumber(value, fallback = 0) {
  return Math.max(0, finiteNumber(value, fallback));
}

function isPlacement(value) {
  return value === 'above' || value === 'below';
}

function stabilizePlacement({
  placement,
  previousPlacement,
  availableAbove,
  availableBelow,
  preferredHeight,
  hysteresis,
}) {
  if (!isPlacement(previousPlacement) || previousPlacement === placement) {
    return placement;
  }

  if (previousPlacement === 'above') {
    const lowerAdvantage = availableBelow >= preferredHeight
      ? preferredHeight - availableAbove
      : availableBelow - availableAbove;
    return lowerAdvantage > hysteresis ? 'below' : 'above';
  }

  if (availableAbove >= preferredHeight) {
    const upperRecovery = availableAbove - preferredHeight;
    return upperRecovery > hysteresis ? 'above' : 'below';
  }

  const upperAdvantage = availableAbove - availableBelow;
  return upperAdvantage > hysteresis ? 'above' : 'below';
}

export function computeAnchoredDropdownLayout({
  anchorTop,
  anchorBottom,
  viewportTop = 0,
  viewportHeight,
  preferredHeight,
  gap = DROPDOWN_GAP_PX,
  margin = DROPDOWN_VIEWPORT_MARGIN_PX,
  previousPlacement,
  hysteresis = DROPDOWN_PLACEMENT_HYSTERESIS_PX,
} = {}) {
  const safeViewportTop = finiteNumber(viewportTop);
  const safeViewportHeight = nonNegativeNumber(viewportHeight);
  const safeAnchorTop = finiteNumber(anchorTop, safeViewportTop);
  const safeAnchorBottom = Math.max(
    safeAnchorTop,
    finiteNumber(anchorBottom, safeAnchorTop),
  );
  const safePreferredHeight = nonNegativeNumber(preferredHeight);
  const safeGap = nonNegativeNumber(gap);
  const safeMargin = nonNegativeNumber(margin);
  const safeHysteresis = nonNegativeNumber(
    hysteresis,
    DROPDOWN_PLACEMENT_HYSTERESIS_PX,
  );
  const viewportBottom = safeViewportTop + safeViewportHeight;

  const availableAbove = Math.max(
    0,
    safeAnchorTop - safeViewportTop - safeMargin - safeGap,
  );
  const availableBelow = Math.max(
    0,
    viewportBottom - safeMargin - safeAnchorBottom - safeGap,
  );

  let placement = 'above';
  if (availableAbove < safePreferredHeight) {
    if (availableBelow >= safePreferredHeight || availableBelow > availableAbove) {
      placement = 'below';
    }
  }
  placement = stabilizePlacement({
    placement,
    previousPlacement,
    availableAbove,
    availableBelow,
    preferredHeight: safePreferredHeight,
    hysteresis: safeHysteresis,
  });

  const availableHeight = placement === 'above' ? availableAbove : availableBelow;
  return {
    placement,
    availableAbove,
    availableBelow,
    maxHeight: Math.min(safePreferredHeight, availableHeight),
    constrained: availableHeight < safePreferredHeight,
  };
}
