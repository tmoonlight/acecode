export const TOKEN_BUDGET_PANEL_VIEWPORT_MARGIN_PX = 8;
export const TOKEN_BUDGET_PANEL_POINTER_GAP_PX = 10;
export const TOKEN_BUDGET_PANEL_POINTER_EDGE_PX = 10;
export const TOKEN_BUDGET_PANEL_PREFERRED_HEIGHT_PX = 310;

function finiteNumber(value, fallback = 0) {
  return Number.isFinite(value) ? value : fallback;
}

function nonNegativeNumber(value, fallback = 0) {
  return Math.max(0, finiteNumber(value, fallback));
}

function clamp(value, min, max) {
  return Math.min(max, Math.max(min, value));
}

export function computeTokenBudgetPanelLayout({
  panelWidth,
  pointerX,
  pointerY,
  viewportWidth,
  viewportHeight,
  preferredHeight = TOKEN_BUDGET_PANEL_PREFERRED_HEIGHT_PX,
  margin = TOKEN_BUDGET_PANEL_VIEWPORT_MARGIN_PX,
  gap = TOKEN_BUDGET_PANEL_POINTER_GAP_PX,
  edgeOffset = TOKEN_BUDGET_PANEL_POINTER_EDGE_PX,
} = {}) {
  const safeViewportWidth = nonNegativeNumber(viewportWidth);
  const safeViewportHeight = nonNegativeNumber(viewportHeight);
  const safeMargin = nonNegativeNumber(margin);
  const horizontalMargin = Math.min(safeMargin, safeViewportWidth / 2);
  const verticalMargin = Math.min(safeMargin, safeViewportHeight / 2);
  const safeGap = nonNegativeNumber(gap);
  const safeEdgeOffset = nonNegativeNumber(edgeOffset);
  const safePreferredHeight = nonNegativeNumber(preferredHeight);
  const width = Math.min(
    nonNegativeNumber(panelWidth),
    Math.max(0, safeViewportWidth - (horizontalMargin * 2)),
  );
  const x = clamp(
    finiteNumber(pointerX, safeViewportWidth / 2),
    horizontalMargin,
    safeViewportWidth - horizontalMargin,
  );
  const y = clamp(
    finiteNumber(pointerY, safeViewportHeight / 2),
    verticalMargin,
    safeViewportHeight - verticalMargin,
  );
  const maxLeft = Math.max(
    horizontalMargin,
    safeViewportWidth - horizontalMargin - width,
  );
  const preferredLeft = x >= safeViewportWidth / 2
    ? x + safeEdgeOffset - width
    : x - safeEdgeOffset;
  const left = clamp(preferredLeft, horizontalMargin, maxLeft);
  const roomAbove = Math.max(0, y - safeGap - verticalMargin);
  const roomBelow = Math.max(
    0,
    safeViewportHeight - y - safeGap - verticalMargin,
  );
  const placement = roomAbove >= Math.min(safePreferredHeight, roomBelow)
    ? 'above'
    : 'below';
  const availableHeight = placement === 'above' ? roomAbove : roomBelow;

  return {
    placement,
    left: Math.round(left),
    width: Math.round(width),
    maxHeight: Math.max(0, Math.floor(availableHeight)),
    top: placement === 'below' ? Math.round(y + safeGap) : undefined,
    bottom: placement === 'above'
      ? Math.round(safeViewportHeight - y + safeGap)
      : undefined,
  };
}
