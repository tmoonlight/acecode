import { clampPopupWidth, markSidebarPopupEvent } from '../lib/responsiveSidebars.js';
import { useCallback, useLayoutEffect, useRef } from 'react';
import { notifyNativeSurfaceOverlayChange } from '../lib/agentBrowserSurfaceCoordinator.js';
import '../styles/responsive-sidebars.css';

// Keep the same subtree mounted while changing its place in the layout. This
// preserves editor DOM state and sidebar caches across resize/popup transitions.
export function ResponsiveSidebar({ side, popup = false, hidden = false, style, children, onResize, resizeRange }) {
  const rootRef = useRef(null);
  const dragCleanupRef = useRef(null);
  const latest = useRef(null);
  latest.current = { onResize, resizeRange };
  const stopResize = useCallback(() => dragCleanupRef.current?.(), []);
  useLayoutEffect(() => {
    if (!popup || hidden) stopResize();
    return stopResize;
  }, [popup, hidden, stopResize]);
  useLayoutEffect(() => {
    notifyNativeSurfaceOverlayChange();
    return () => notifyNativeSurfaceOverlayChange();
  }, [popup, hidden, style?.width]);
  const startResize = (event) => {
    if (event.button !== 0 || event.isPrimary === false || dragCleanupRef.current) return;
    const handle = event.currentTarget;
    const pointerId = event.pointerId;
    const startX = event.clientX;
    // Saved widths can be larger than the current viewport. Start from the
    // rendered edge so the first movement never jumps to that old preference.
    const startWidth = rootRef.current.getBoundingClientRect().width;
    const direction = side === 'left' ? 1 : -1;
    event.preventDefault();
    handle.setPointerCapture?.(pointerId);
    document.body.classList.add('ace-resizing');
    const move = (next) => {
      if (next.pointerId !== pointerId) return;
      latest.current.onResize(clampPopupWidth(startWidth + direction * (next.clientX - startX), latest.current.resizeRange));
    };
    const end = (next) => { if (next.pointerId === pointerId) stopResize(); };
    const cleanup = () => {
      dragCleanupRef.current = null;
      window.removeEventListener('pointermove', move);
      window.removeEventListener('pointerup', end);
      window.removeEventListener('pointercancel', end);
      window.removeEventListener('blur', stopResize);
      handle.removeEventListener('lostpointercapture', end);
      if (handle.hasPointerCapture?.(pointerId)) handle.releasePointerCapture(pointerId);
      document.body.classList.remove('ace-resizing');
    };
    dragCleanupRef.current = cleanup;
    window.addEventListener('pointermove', move);
    window.addEventListener('pointerup', end);
    window.addEventListener('pointercancel', end);
    window.addEventListener('blur', stopResize);
    handle.addEventListener('lostpointercapture', end);
  };
  const onResizeKeyDown = (event) => {
    if (event.altKey || event.ctrlKey || event.metaKey || !['ArrowLeft', 'ArrowRight'].includes(event.key)) return;
    event.preventDefault();
    const direction = (event.key === 'ArrowRight' ? 1 : -1) * (side === 'left' ? 1 : -1);
    onResize(clampPopupWidth(rootRef.current.getBoundingClientRect().width + direction * (event.shiftKey ? 32 : 12), resizeRange));
  };
  return (
    <div
      ref={rootRef}
      className={`ace-responsive-sidebar ace-responsive-sidebar-${side}`}
      data-sidebar-popup={popup ? side : undefined}
      data-ace-native-overlay={popup && side === 'left' ? 'overlap' : undefined}
      data-sidebar-hidden={hidden ? 'true' : undefined}
      style={style}
      onPointerDownCapture={(event) => markSidebarPopupEvent(event, side)}
    >
      {children}
      {popup && !hidden && onResize && resizeRange && (
        <div
          role="separator"
          aria-label={side === 'left' ? '调整左侧栏宽度' : '调整右侧栏宽度'}
          aria-orientation="vertical"
          aria-valuemin={resizeRange.min}
          aria-valuemax={resizeRange.max}
          aria-valuenow={Math.round(Number(style?.width) || 0)}
          tabIndex={0}
          data-popup-resize={side}
          data-ace-native-overlay="overlap"
          className="ace-resize-handle ace-popup-resize-handle"
          onPointerDown={startResize}
          onKeyDown={onResizeKeyDown}
          title={side === 'left' ? '拖动调整左侧栏宽度' : '拖动调整右侧栏宽度'}
        />
      )}
    </div>
  );
}
