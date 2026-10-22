// 二级菜单(flyout)位置:贴着父菜单的左侧或右侧展开,顶边对齐触发项。
// Agent Browser 的「⋮」菜单在工具栏最右边,所以默认向左展开(与 Claude 桌面版一致),
// 左侧放不下才放右侧;都放不下时贴视口边缘。纵向不超出视口。
export const SUBMENU_GAP = 4;
export const SUBMENU_VIEWPORT_MARGIN = 8;

export function submenuPosition({
  menuRect,
  itemRect,
  submenuWidth,
  submenuHeight,
  viewportWidth,
  viewportHeight,
  preferredSide = 'left',
}) {
  const width = Math.max(0, Number(submenuWidth) || 0);
  const height = Math.max(0, Number(submenuHeight) || 0);
  const leftCandidate = menuRect.left - SUBMENU_GAP - width;
  const rightCandidate = menuRect.right + SUBMENU_GAP;
  const fitsLeft = leftCandidate >= SUBMENU_VIEWPORT_MARGIN;
  const fitsRight = rightCandidate + width <= viewportWidth - SUBMENU_VIEWPORT_MARGIN;
  let side = preferredSide === 'right' ? 'right' : 'left';
  if (side === 'left' && !fitsLeft && fitsRight) side = 'right';
  if (side === 'right' && !fitsRight && fitsLeft) side = 'left';
  let left = side === 'left' ? leftCandidate : rightCandidate;
  const maxLeft = viewportWidth - SUBMENU_VIEWPORT_MARGIN - width;
  left = Math.max(SUBMENU_VIEWPORT_MARGIN, Math.min(left, maxLeft));
  // 子菜单第一项的文字与触发项对齐:减去子菜单自身的内边距(约 4px)。
  let top = itemRect.top - SUBMENU_GAP;
  const maxTop = viewportHeight - SUBMENU_VIEWPORT_MARGIN - height;
  top = Math.max(SUBMENU_VIEWPORT_MARGIN, Math.min(top, maxTop));
  return { left: Math.round(left), top: Math.round(top), side };
}
