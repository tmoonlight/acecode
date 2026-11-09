import {
  DEFAULT_SINGLE_LAYOUT,
  DEFAULT_SUMMARY_PANEL_WIDTH,
  normalizeSummaryPanelWidth,
  solveSingleContentLayout,
} from './singleLayout.js';
import { rightPanelHidden, summaryPanelHidden } from './uiPrefs.js';
import { NARROW_LAYOUT_MAX_WIDTH } from './mobileShell.js';

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
  summaryAvailable = false,
  previous = {},
  enabled = true,
  touch = false,
} = {}) {
  const transcriptMinimum = MIN_READABLE_TRANSCRIPT_WIDTH;
  const minimumChatWidth = transcriptMinimum + TRANSCRIPT_COLUMN_GUTTER
    + (subagentOpen ? widths.subagentPanel || DEFAULT_SINGLE_LAYOUT.subagentPanel : 0);
  const leftWanted = !preferences.sidebarCollapsed;
  const rightWanted = !rightPanelHidden(preferences, previewAvailable);
  const active = enabled && shellWidth > 0
    && !(preferences.sidePanelMaximized && previewAvailable && rightWanted);
  const leftWidth = widths.sidebar || DEFAULT_SINGLE_LAYOUT.sidebar;
  const phoneDrawers = enabled && touch && shellWidth > 0
    && (shellWidth <= NARROW_LAYOUT_MAX_WIDTH ||
        shellWidth < minimumChatWidth + (widths.sidePanel || DEFAULT_SINGLE_LAYOUT.sidePanel));
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

  const leftAutoCollapsed = phoneDrawers || (active && !fitsWithLeft(
    shellWidth - leftWidth,
    minimumChatWidth + (previous.leftAutoCollapsed ? SIDEBAR_RESTORE_MARGIN : 0),
  ));
  const leftDocked = leftWanted && !leftAutoCollapsed;

  // 摘要面板最先让位:左侧会话列表的判定不看它;它只在「左侧该停靠的都停靠了,
  // 再放下它对话区仍够宽」时才停靠。左侧想要却被挤掉时它一定也收起。
  const summaryWidth = normalizeSummaryPanelWidth(widths.summaryPanel ?? DEFAULT_SUMMARY_PANEL_WIDTH);
  const summaryFits = summaryAvailable && !phoneDrawers && (!active || (
    !(leftWanted && leftAutoCollapsed)
    && fitsWithLeft(
      shellWidth - (leftWanted ? leftWidth : 0) - summaryWidth,
      minimumChatWidth + (previous.summaryAutoCollapsed ? SIDEBAR_RESTORE_MARGIN : 0),
    )
  ));
  const summaryWanted = summaryAvailable && !summaryPanelHidden(preferences);
  const summaryAutoCollapsed = summaryWanted && !summaryFits;
  return {
    active, transcriptMinimum, minimumChatWidth,
    leftAutoCollapsed, leftDocked, canDockLeft: !leftAutoCollapsed,
    summaryAvailable, summaryAutoCollapsed, summaryDocked: summaryWanted && summaryFits, canDockSummary: summaryFits,
    rightAutoCollapsed: phoneDrawers, rightDocked: rightWanted && !phoneDrawers, canDockRight: !phoneDrawers,
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
