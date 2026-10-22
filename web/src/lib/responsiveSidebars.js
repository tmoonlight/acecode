import { DEFAULT_SINGLE_LAYOUT, solveSingleContentLayout } from './singleLayout.js';
import { rightPanelHidden } from './uiPrefs.js';

export const MIN_READABLE_TRANSCRIPT_WIDTH = 350;
export const SIDEBAR_RESTORE_MARGIN = 64;
// The parent transcript's existing pr-1.5 gutter also consumes real space.
export const TRANSCRIPT_COLUMN_GUTTER = 6;

export function popupWidthRange(availableWidth, minimumWidth) {
  const max = Math.max(0, Math.floor(Number(availableWidth) || 0) - 24);
  return { min: Math.min(max, minimumWidth), max };
}

export function clampPopupWidth(width, range) {
  const value = Number.isFinite(width) ? Math.round(width) : range.min;
  return Math.max(range.min, Math.min(range.max, value));
}

export function solveResponsiveSidebars({
  shellWidth = 0,
  widths = DEFAULT_SINGLE_LAYOUT,
  preferences = {},
  previewAvailable = false,
  previewAutoFit = false,
  subagentOpen = false,
  previous = {},
  enabled = true,
} = {}) {
  const transcriptMinimum = MIN_READABLE_TRANSCRIPT_WIDTH;
  const minimumChatWidth = transcriptMinimum + TRANSCRIPT_COLUMN_GUTTER
    + (subagentOpen ? widths.subagentPanel || DEFAULT_SINGLE_LAYOUT.subagentPanel : 0);
  const leftWanted = !preferences.sidebarCollapsed;
  const rightWanted = !rightPanelHidden(preferences, previewAvailable);
  const active = enabled && shellWidth > 0
    && !(preferences.sidePanelMaximized && previewAvailable && rightWanted);
  const leftWidth = widths.sidebar || DEFAULT_SINGLE_LAYOUT.sidebar;
  // Only the left sidebar responds to transcript pressure. Account for the
  // right workspace's manual visibility when predicting the expanded layout.
  const fitsWithLeft = (available, required) => solveSingleContentLayout({
    contentWidth: available,
    sidePanelWidth: widths.sidePanel,
    previewPanelWidth: widths.previewPanel,
    sidePanelVisible: rightWanted,
    sidePanelCollapsed: !!preferences.sidePanelListCollapsed,
    previewPanelVisible: rightWanted && previewAvailable,
    previewPanelAutoFit: previewAutoFit,
    minimumChatWidth: required,
  }).chatWidth >= required;

  const leftAutoCollapsed = active && !fitsWithLeft(
    shellWidth - leftWidth,
    minimumChatWidth + (previous.leftAutoCollapsed ? SIDEBAR_RESTORE_MARGIN : 0),
  );
  const leftDocked = leftWanted && !leftAutoCollapsed;
  return {
    active, transcriptMinimum, minimumChatWidth,
    leftAutoCollapsed, leftDocked, canDockLeft: !leftAutoCollapsed,
    rightAutoCollapsed: false, rightDocked: rightWanted, canDockRight: true,
  };
}

// React capture also sees events from descendant portals. DOM containment alone
// would mistake a sidebar's own context menu for an outside click.
const popupEventOwner = new WeakMap();
export function markSidebarPopupEvent(event, side) {
  popupEventOwner.set(event.nativeEvent || event, side);
}
export function sidebarPopupEventIsInside(event, side) {
  return popupEventOwner.get(event.nativeEvent || event) === side;
}
