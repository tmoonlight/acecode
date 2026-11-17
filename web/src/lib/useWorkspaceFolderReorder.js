import { useEffect, useRef, useState } from 'react';
import { reorderWorkspaceFolders, workspaceFolderDropTarget } from './workspaceFolderOrder.js';
import { pointerDragButtonReleased } from './pointerDragGuard.js';

export function useWorkspaceFolderReorder({
  workspaces, scrollRef, disabled, onReorder, conflictingDragRefs = [],
}) {
  const listRef = useRef(null);
  const gestureRef = useRef(null);
  const suppressClickRef = useRef(false);
  const [drag, setDrag] = useState(null);
  const identity = JSON.stringify(workspaces.map((workspace) => workspace.hash));

  useEffect(() => () => gestureRef.current?.cancel(), [identity, disabled]);

  const onPointerDown = (event, workspace) => {
    suppressClickRef.current = false;
    if (disabled || workspaces.length < 2 || workspace.hash === '__local__'
      || event.button !== 0 || event.isPrimary === false || gestureRef.current
      || conflictingDragRefs.some((ref) => ref.current)
      || event.target.closest('button, a, input, textarea, select, [contenteditable="true"]')) return;
    const owner = event.currentTarget;
    const list = listRef.current;
    const scroller = scrollRef.current;
    if (!list || !scroller) return;
    const rowRect = owner.getBoundingClientRect();
    const label = owner.querySelector('[data-sidebar-workspace-folder-label]');
    const labelRange = document.createRange();
    if (label) labelRange.selectNodeContents(label);
    const width = Math.min(rowRect.width, (label ? labelRange.getBoundingClientRect().width : rowRect.width) + 58);
    const offsetX = Math.min(event.clientX - rowRect.left, width - 12);
    const pointerId = event.pointerId;
    const start = { x: event.clientX, y: event.clientY };
    const point = { ...start };
    let active = false;
    let target = null;
    let frame = 0;
    let lastTime = 0;

    const update = () => {
      const bounds = list.getBoundingClientRect();
      const viewport = scroller.getBoundingClientRect();
      const rows = Array.from(list.querySelectorAll('[data-sidebar-workspace-folder-hash]'))
        .map((group) => {
          const rect = group.getBoundingClientRect();
          return { hash: group.dataset.sidebarWorkspaceFolderHash, top: rect.top, bottom: rect.bottom };
        });
      target = workspaceFolderDropTarget(rows, {
        left: Math.max(bounds.left, viewport.left), right: Math.min(bounds.right, viewport.right),
        top: Math.max(bounds.top, viewport.top), bottom: Math.min(bounds.bottom, viewport.bottom),
      }, point.x, point.y);
      if (target && reorderWorkspaceFolders(workspaces, workspace.hash, target.hash, target.placement) === workspaces) {
        target = null;
      }
      const left = point.x - offsetX;
      const top = rowRect.top + point.y - start.y;
      setDrag((previous) => previous?.left === left && previous?.top === top
        && previous?.target === target?.hash && previous?.placement === target?.placement
        ? previous : {
          source: workspace.hash, name: workspace.name || workspace.hash,
          target: target?.hash, placement: target?.placement,
          left, top, width, height: rowRect.height,
        });
    };

    const scroll = (time) => {
      const viewport = scroller.getBoundingClientRect();
      const bounds = list.getBoundingClientRect();
      const elapsed = lastTime ? Math.min(time - lastTime, 32) : 16;
      lastTime = time;
      if (point.x >= bounds.left && point.x <= bounds.right
        && point.y >= viewport.top && point.y <= viewport.bottom) {
        const edge = Math.min(40, viewport.height / 4);
        const speed = point.y < viewport.top + edge ? -(viewport.top + edge - point.y) / edge
          : point.y > viewport.bottom - edge ? (point.y - viewport.bottom + edge) / edge : 0;
        if (speed) scroller.scrollTop += speed * elapsed * 0.6;
      }
      update();
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
      gestureRef.current = null;
      if (active) suppressClickRef.current = true;
      document.body.classList.remove('ace-workspace-folder-reordering');
      try { owner.releasePointerCapture(pointerId); } catch { /* Capture may already be lost. */ }
      setDrag(null);
    };
    const cancel = () => cleanup();
    const cancelPointer = (pointerEvent) => {
      if (pointerEvent.pointerId === pointerId) cancel();
    };
    const key = (keyEvent) => {
      if (keyEvent.key !== 'Escape') return;
      keyEvent.preventDefault();
      keyEvent.stopPropagation();
      cancel();
    };
    const move = (moveEvent) => {
      if (moveEvent.pointerId !== pointerId) return;
      // 主键早已松开(疑似 macOS「轻点以点按」的松手没送到),不按键的移动不能把文件夹拖起来。
      // 松手已经发生过,后面不会再有松手 click,所以不保留吞 click 的标记,免得吞掉用户下一次点击。
      if (pointerDragButtonReleased(moveEvent)) {
        cleanup();
        suppressClickRef.current = false;
        return;
      }
      point.x = moveEvent.clientX;
      point.y = moveEvent.clientY;
      if (!active && Math.hypot(point.x - start.x, point.y - start.y) < 5) return;
      if (!active) {
        active = true;
        document.body.classList.add('ace-workspace-folder-reordering');
        try { owner.setPointerCapture(pointerId); } catch { /* Window listeners still handle cancellation. */ }
        frame = requestAnimationFrame(scroll);
      }
      moveEvent.preventDefault();
      update();
    };
    const up = (upEvent) => {
      if (upEvent.pointerId !== pointerId) return;
      point.x = upEvent.clientX;
      point.y = upEvent.clientY;
      if (active) {
        upEvent.preventDefault();
        update();
      }
      const drop = active ? target : null;
      cleanup();
      if (drop) onReorder(reorderWorkspaceFolders(workspaces, workspace.hash, drop.hash, drop.placement));
    };

    gestureRef.current = { cancel };
    window.addEventListener('pointermove', move, { passive: false });
    window.addEventListener('pointerup', up);
    window.addEventListener('pointercancel', cancelPointer);
    window.addEventListener('keydown', key, true);
    window.addEventListener('blur', cancel);
    owner.addEventListener('lostpointercapture', cancelPointer);
  };

  const onClickCapture = (event) => {
    if (!suppressClickRef.current || event.detail === 0
      || !event.target.closest('[data-desktop-workspace-id]')) return;
    suppressClickRef.current = false;
    event.preventDefault();
    event.stopPropagation();
  };
  const onKeyDown = (event, workspace) => {
    if (!event.altKey || !['ArrowUp', 'ArrowDown'].includes(event.key)) return;
    event.preventDefault();
    event.stopPropagation();
    if (disabled || gestureRef.current || conflictingDragRefs.some((ref) => ref.current)) return;
    const delta = event.key === 'ArrowUp' ? -1 : 1;
    const index = workspaces.findIndex((item) => item.hash === workspace.hash);
    const target = workspaces[index + delta];
    if (target) onReorder(reorderWorkspaceFolders(workspaces, workspace.hash, target.hash, delta < 0 ? 'before' : 'after'));
  };

  return { listRef, gestureRef, drag, onPointerDown, onClickCapture, onKeyDown };
}
