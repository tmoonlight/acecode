import { useLayoutEffect, useRef } from 'react';
import { createPortal } from 'react-dom';
import { anchoredMenuPosition } from '../lib/anchoredMenuPosition.js';
import { notifyNativeSurfaceOverlayChange } from '../lib/agentBrowserSurfaceCoordinator.js';

// Own placement and dismissal together so menus escape clipped ancestors while
// still following their trigger as the window, content, or dock changes size.
export function AnchoredMenu({ anchorRef, onClose, children, className = '', width, maxHeightRatio = 1, preferredPlacement = 'below', ...props }) {
  const menuRef = useRef(null);
  const updatePositionRef = useRef(null);
  const closeRef = useRef(onClose);
  closeRef.current = onClose;

  useLayoutEffect(() => {
    const anchor = anchorRef.current;
    const menu = menuRef.current;
    if (!anchor || !menu) return undefined;
    const previousFocus = document.activeElement;
    const ancestors = [];
    for (let node = anchor.parentElement; node; node = node.parentElement) ancestors.push(node);
    let lastPosition = '';
    const update = () => {
      const rect = anchor.getBoundingClientRect();
      const viewportWidth = document.documentElement.clientWidth;
      const viewportHeight = window.innerHeight;
      let visibleTop = 0;
      let visibleBottom = viewportHeight;
      for (const parent of ancestors) {
        if (/(auto|scroll|hidden|clip)/.test(getComputedStyle(parent).overflowY)) {
          const bounds = parent.getBoundingClientRect();
          visibleTop = Math.max(visibleTop, bounds.top);
          visibleBottom = Math.min(visibleBottom, bounds.bottom);
        }
      }
      if (rect.bottom <= visibleTop || rect.top >= visibleBottom || rect.right <= 0 || rect.left >= viewportWidth) {
        closeRef.current?.();
        return;
      }
      const position = anchoredMenuPosition({
        anchorRect: rect,
        menuWidth: width ?? menu.getBoundingClientRect().width,
        menuHeight: menu.scrollHeight + menu.offsetHeight - menu.clientHeight,
        viewportWidth,
        viewportHeight,
        maxHeight: viewportHeight * maxHeightRatio,
        preferredPlacement,
      });
      const key = JSON.stringify(position);
      if (lastPosition === key) return;
      lastPosition = key;
      Object.assign(menu.style, {
        left: `${position.left}px`, top: `${position.top}px`,
        width: `${position.width}px`, maxHeight: `${position.maxHeight}px`,
        visibility: 'visible',
      });
      menu.dataset.placement = position.placement;
      notifyNativeSurfaceOverlayChange();
    };
    updatePositionRef.current = update;
    const buttons = () => [...menu.querySelectorAll('button:not([disabled])')];
    const onPointerDown = (event) => {
      if (!menu.contains(event.target) && !anchor.contains(event.target)) closeRef.current?.();
    };
    const onKeyDown = (event) => {
      if (event.key === 'Escape') {
        event.preventDefault();
        event.stopImmediatePropagation();
        closeRef.current?.();
        return;
      }
      if (!menu.contains(document.activeElement)) return;
      const items = buttons();
      const index = items.indexOf(document.activeElement);
      let next;
      if (event.key === 'ArrowDown') next = (index + 1) % items.length;
      if (event.key === 'ArrowUp') next = (index - 1 + items.length) % items.length;
      if (event.key === 'Home') next = 0;
      if (event.key === 'End') next = items.length - 1;
      if (next !== undefined && items[next]) {
        event.preventDefault();
        items[next].focus();
      }
      if (event.key === 'Tab' && (event.shiftKey ? index === 0 : index === items.length - 1)) {
        closeRef.current?.();
      }
    };
    const onScroll = (event) => {
      if (!menu.contains(event.target)) update();
    };
    update();
    buttons()[0]?.focus({ preventScroll: true });
    const observer = new ResizeObserver(update);
    [anchor, menu, ...ancestors].forEach((node) => observer.observe(node));
    window.addEventListener('resize', update);
    document.addEventListener('scroll', onScroll, true);
    document.addEventListener('pointerdown', onPointerDown, true);
    document.addEventListener('keydown', onKeyDown, true);
    return () => {
      updatePositionRef.current = null;
      observer.disconnect();
      window.removeEventListener('resize', update);
      document.removeEventListener('scroll', onScroll, true);
      document.removeEventListener('pointerdown', onPointerDown, true);
      document.removeEventListener('keydown', onKeyDown, true);
      if (menu.contains(document.activeElement)) previousFocus?.focus?.({ preventScroll: true });
      notifyNativeSurfaceOverlayChange();
    };
  }, [anchorRef, width, maxHeightRatio, preferredPlacement]);

  // A sibling insertion can move the anchor without resizing any observed box.
  // Recheck after parent renders as well as on resize/scroll notifications.
  useLayoutEffect(() => { updatePositionRef.current?.(); });

  return createPortal(
    <div
      {...props}
      ref={menuRef}
      data-ace-native-overlay="overlap"
      data-shortcut-menu="true"
      className={`ace-anchored-menu ace-scrollbar ${className}`}
      style={{ position: 'fixed', top: 0, left: 0, width, visibility: 'hidden' }}
    >
      {children}
    </div>,
    document.body,
  );
}
