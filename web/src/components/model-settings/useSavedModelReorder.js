import { isDismissShortcut } from '../../lib/shortcutBindings.js';
import { useEffect, useRef, useState } from 'react';
import { reorderSavedModels, savedModelDropTarget } from '../../lib/savedModelOrder.js';

function scrollParent(element) {
  for (let parent = element?.parentElement; parent; parent = parent.parentElement) {
    if (/(auto|scroll)/.test(getComputedStyle(parent).overflowY)
      && parent.scrollHeight > parent.clientHeight) return parent;
  }
  return document.scrollingElement;
}

export function useSavedModelReorder({ models, filtered, query, disabled, onReorder }) {
  const listRef = useRef(null);
  const cancelRef = useRef(null);
  const [drag, setDrag] = useState(null);

  useEffect(() => () => cancelRef.current?.(), [models, query, disabled]);

  const onPointerDown = (event, model, handle = false) => {
    if (disabled || !onReorder || filtered.length < 2 || event.button !== 0
      || event.isPrimary === false || cancelRef.current) return;
    if (!handle && event.target.closest('button, a, input, textarea, select')) return;
    event.preventDefault();
    event.stopPropagation();
    const owner = event.currentTarget;
    // The handle captures the pointer, but the preview must measure the full row.
    const row = owner.closest('[data-saved-model-name]');
    if (!row) return;
    const cardRect = row.getBoundingClientRect();
    const pointerId = event.pointerId;
    const start = { x: event.clientX, y: event.clientY };
    const point = { ...start };
    const scroller = scrollParent(listRef.current);
    const previousCursor = document.body.style.cursor;
    const previousSelect = document.body.style.userSelect;
    let active = false;
    let target = null;
    let frame = 0;
    let previousTime = 0;

    const viewport = () => (scroller === document.scrollingElement
      ? { left: 0, top: 0, right: window.innerWidth, bottom: window.innerHeight }
      : scroller.getBoundingClientRect());

    const updateTarget = () => {
      const list = listRef.current;
      if (!list) return;
      const rect = list.getBoundingClientRect();
      const view = viewport();
      const rows = Array.from(list.querySelectorAll('[data-saved-model-name]')).map((row) => {
        const bounds = row.getBoundingClientRect();
        return { name: row.dataset.savedModelName, top: bounds.top, bottom: bounds.bottom };
      });
      target = savedModelDropTarget(rows, {
        left: Math.max(rect.left, view.left), right: Math.min(rect.right, view.right),
        top: Math.max(rect.top, view.top), bottom: Math.min(rect.bottom, view.bottom),
      }, point.x, point.y);
      if (target && reorderSavedModels(models, model.name, target.name, target.placement) === models) {
        target = null;
      }
      const left = cardRect.left + point.x - start.x;
      const top = cardRect.top + point.y - start.y;
      setDrag((current) => current?.source === model.name
        && current?.target === target?.name && current?.placement === target?.placement
        && current.left === left && current.top === top
        ? current : {
          source: model.name, target: target?.name, placement: target?.placement,
          left, top, width: cardRect.width, height: cardRect.height,
        });
    };

    const scroll = (time) => {
      const view = viewport();
      const rect = listRef.current?.getBoundingClientRect();
      const elapsed = previousTime ? Math.min(time - previousTime, 32) : 16;
      previousTime = time;
      if (rect && point.x >= rect.left && point.x <= rect.right
        && point.y >= view.top && point.y <= view.bottom) {
        const edge = Math.min(40, (view.bottom - view.top) / 4);
        const speed = point.y < view.top + edge
          ? -(view.top + edge - point.y) / edge
          : point.y > view.bottom - edge ? (point.y - view.bottom + edge) / edge : 0;
        if (speed) scroller.scrollTop += speed * elapsed * 0.6;
      }
      updateTarget();
      frame = requestAnimationFrame(scroll);
    };

    const cleanup = () => {
      cancelAnimationFrame(frame);
      window.removeEventListener('pointermove', move);
      window.removeEventListener('pointerup', up);
      window.removeEventListener('pointercancel', cancelPointer);
      window.removeEventListener('keydown', key, true);
      window.removeEventListener('blur', cancel);
      owner.removeEventListener('lostpointercapture', cancelPointer);
      cancelRef.current = null;
      if (active) {
        document.body.style.cursor = previousCursor;
        document.body.style.userSelect = previousSelect;
      }
      try { owner.releasePointerCapture(pointerId); } catch { /* Already released. */ }
      setDrag(null);
    };
    const cancel = () => cleanup();
    const cancelPointer = (pointerEvent) => {
      if (pointerEvent.pointerId === pointerId) cancel();
    };
    const key = (keyEvent) => {
      if (!isDismissShortcut(keyEvent)) return;
      keyEvent.preventDefault();
      keyEvent.stopPropagation();
      cancel();
    };
    const move = (moveEvent) => {
      if (moveEvent.pointerId !== pointerId) return;
      point.x = moveEvent.clientX;
      point.y = moveEvent.clientY;
      if (!active && Math.hypot(point.x - start.x, point.y - start.y) < 5) return;
      if (!active) {
        active = true;
        document.body.style.cursor = 'grabbing';
        document.body.style.userSelect = 'none';
        frame = requestAnimationFrame(scroll);
      }
      moveEvent.preventDefault();
      updateTarget();
    };
    const up = (upEvent) => {
      if (upEvent.pointerId !== pointerId) return;
      point.x = upEvent.clientX;
      point.y = upEvent.clientY;
      if (active) updateTarget();
      const drop = active ? target : null;
      cleanup();
      if (drop) onReorder(reorderSavedModels(models, model.name, drop.name, drop.placement));
    };

    cancelRef.current = cancel;
    window.addEventListener('pointermove', move, { passive: false });
    window.addEventListener('pointerup', up);
    window.addEventListener('pointercancel', cancelPointer);
    window.addEventListener('keydown', key, true);
    window.addEventListener('blur', cancel);
    owner.addEventListener('lostpointercapture', cancelPointer);
    try { owner.setPointerCapture(pointerId); } catch { /* Window listeners still clean up. */ }
    if (handle) owner.focus({ preventScroll: true });
  };

  const onKeyDown = (event, model) => {
    if (!['ArrowUp', 'ArrowDown'].includes(event.key)) return;
    event.preventDefault();
    event.stopPropagation();
    if (disabled || !onReorder || cancelRef.current) return;
    const index = filtered.findIndex((entry) => entry.name === model.name);
    const target = filtered[index + (event.key === 'ArrowUp' ? -1 : 1)];
    if (!target) return;
    onReorder(reorderSavedModels(models, model.name, target.name,
      event.key === 'ArrowUp' ? 'before' : 'after'));
  };

  return { listRef, drag, onPointerDown, onKeyDown };
}
