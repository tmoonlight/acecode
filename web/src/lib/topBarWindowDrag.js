// Keep the pre-compaction drag height, independent of the 30px visible chrome.
export const TOPBAR_WINDOW_DRAG_HEIGHT = 44;

export function topBarWindowDragAction(event, bounds, excluded = false) {
  if (!bounds || bounds.width <= 0 || bounds.height <= 0 || excluded
    || event.defaultPrevented || event.button !== 0) return null;
  const { clientX: x, clientY: y } = event;
  if (!Number.isFinite(x) || !Number.isFinite(y)
    || x < bounds.left || x >= bounds.right
    || y < bounds.top || y >= bounds.top + TOPBAR_WINDOW_DRAG_HEIGHT) return null;
  return event.detail >= 2 ? 'maximize' : 'drag';
}

// The extra strip belongs to content: never cover it with an input-catching layer.
export function isTopBarDragExcludedTarget(target) {
  return !!target?.closest?.(
    '[data-ace-native-overlay],[role="dialog"],[role="tab"],[role="separator"],'
    + '[draggable="true"],[tabindex],summary,.ace-resize-handle,.ace-console-resize-handle',
  );
}
