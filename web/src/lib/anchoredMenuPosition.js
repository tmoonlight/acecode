// Sizes are border-box CSS pixels. Return a scrollable menu rectangle contained
// in the viewport, keeping the preferred below-anchor placement when it fits.
export function anchoredMenuPosition({
  anchorRect,
  menuWidth,
  menuHeight,
  viewportWidth,
  viewportHeight,
  maxHeight = viewportHeight,
  preferredPlacement = 'below',
  align = 'start',
  gap = 4,
  margin = 8,
}) {
  const width = Math.min(menuWidth, Math.max(0, viewportWidth - margin * 2));
  const clamp = (value, min, max) => Math.max(min, Math.min(value, max));
  if (preferredPlacement === 'right') {
    // 侧栏弹出菜单:贴在触发项右侧、顶边对齐;右侧放不下才翻到左侧。
    // 高度只受视口约束,顶边在放不下时往上推。
    const heightLimit = Math.max(0, Math.min(maxHeight, viewportHeight - margin * 2));
    const height = Math.min(menuHeight, heightLimit);
    const rightLeft = anchorRect.right + gap;
    const fitsRight = rightLeft + width <= viewportWidth - margin;
    const leftLeft = anchorRect.left - gap - width;
    const placement = fitsRight || leftLeft < margin ? 'right' : 'left';
    return {
      left: clamp(placement === 'right' ? rightLeft : leftLeft, margin, viewportWidth - margin - width),
      top: clamp(anchorRect.top, margin, viewportHeight - margin - height),
      width,
      maxHeight: heightLimit,
      placement,
    };
  }
  const desiredHeight = Math.min(menuHeight, maxHeight);
  const below = Math.max(0, viewportHeight - margin - anchorRect.bottom - gap);
  const above = Math.max(0, anchorRect.top - gap - margin);
  const placement = preferredPlacement === 'above'
    ? (desiredHeight <= above || above >= below ? 'above' : 'below')
    : (desiredHeight <= below || below >= above ? 'below' : 'above');
  const heightLimit = Math.min(maxHeight, placement === 'below' ? below : above);
  const height = Math.min(menuHeight, heightLimit);
  return {
    left: clamp(align === 'end' ? anchorRect.right - width : anchorRect.left, margin, viewportWidth - margin - width),
    top: clamp(
      placement === 'below' ? anchorRect.bottom + gap : anchorRect.top - gap - height,
      margin,
      viewportHeight - margin - height,
    ),
    width,
    maxHeight: heightLimit,
    placement,
  };
}
