// 右侧文件树多选的交互:点击(Shift / Ctrl)、键盘、多选右键菜单的委托、指针拖动。
// 规则都在 fileTreeSelection.js / fileTreeDrop.js 的纯函数里,这里只接事件。
import { useEffect, useMemo, useRef, useState } from 'react';
import { CONTEXT_MENU_DELEGATE_EVENT } from './desktopContextMenu.js';
import {
  FILE_TREE_DRAG_START_PX,
  FILE_TREE_DROP_ACTIVE_CLASS,
  dispatchFileTreeDrop,
  fileTreeDragGhostPosition,
  fileTreeDragPreview,
  fileTreeDropItems,
  fileTreeDropTargetAt,
} from './fileTreeDrop.js';
import { normalizeTreePath } from './fileTreeChangeStatus.js';
import {
  emptyFileTreeSelection,
  followActiveFileTreeRow,
  outermostFileTreeEntries,
  pruneFileTreeSelection,
  selectAllFileTreeRows,
  selectFileTreeRow,
  selectedFileTreeEntries,
  visibleFileTreeEntries,
} from './fileTreeSelection.js';
import { isDismissShortcut } from './shortcutBindings.js';
import { useStableCallback } from './useStableCallback.js';
import { useWorkbenchState } from './useWorkbenchState.js';

export const FILE_TREE_SELECTION_DELEGATE = 'file-tree-selection';
// 拖动期间加在 body 上:换光标、禁止选中文字;后一个表示指针正停在可放下的位置。
const DRAGGING_CLASS = 'ace-file-tree-dragging';
const DROP_READY_CLASS = 'ace-file-tree-drop-ready';

export function useFileTreeSelection({
  owner, cwd, treeRef, treeCache, expandedDirs, activePath = '', onBatchMenu,
}) {
  // $ 前缀:只在本窗口内按会话保留(切页签 / 切会话回来还在),不写进浏览器存储。
  const [state, setState] = useWorkbenchState(owner, `$treeSelection:${cwd}`, emptyFileTreeSelection);
  const [ghost, setGhost] = useState(null);
  const visibleEntries = useMemo(
    () => visibleFileTreeEntries(treeCache, expandedDirs),
    [treeCache, expandedDirs],
  );
  const visiblePaths = useMemo(() => visibleEntries.map(entry => entry.path), [visibleEntries]);
  const activeRowPath = useMemo(() => {
    const active = normalizeTreePath(activePath);
    return active ? visibleEntries.find(entry => normalizeTreePath(entry.path) === active)?.path || '' : '';
  }, [activePath, visibleEntries]);

  const latest = useRef(null);
  latest.current = { state, visibleEntries, visiblePaths, activeRowPath, cwd };
  const dragRef = useRef(null);
  const suppressClickRef = useRef(false);
  const ghostElementRef = useRef(null);
  const ghostPointRef = useRef({ x: 0, y: 0 });

  const placeGhost = () => {
    const element = ghostElementRef.current;
    if (!element) return;
    const view = element.ownerDocument.defaultView;
    const position = fileTreeDragGhostPosition(ghostPointRef.current, {
      width: element.offsetWidth,
      height: element.offsetHeight,
      viewportWidth: view?.innerWidth,
      viewportHeight: view?.innerHeight,
    });
    element.style.transform = `translate(${position.left}px, ${position.top}px)`;
  };
  // 位置直接写到元素上:指针每动一下只改一个 transform,不让整棵文件树跟着重渲染。
  const ghostRef = useStableCallback((element) => {
    ghostElementRef.current = element;
    placeGhost();
  });

  const rootLoaded = treeCache.has('');
  const followedActiveRef = useRef(activeRowPath);
  useEffect(() => {
    // 根目录还没拉回来时没有任何可见行,这时裁剪会把刚恢复的选区清空。
    if (!rootLoaded) return;
    const activeChanged = followedActiveRef.current !== activeRowPath;
    followedActiveRef.current = activeRowPath;
    setState((previous) => {
      const pruned = pruneFileTreeSelection(previous, visiblePaths);
      return activeChanged ? followActiveFileTreeRow(pruned, visiblePaths, activeRowPath) : pruned;
    });
  }, [activeRowPath, rootLoaded, setState, visiblePaths]);

  const selectedEntries = useStableCallback(() => (
    selectedFileTreeEntries(latest.current.visibleEntries, latest.current.state.keys)
  ));

  const clear = useStableCallback(() => {
    setState(previous => (previous.keys.size === 0 && !previous.anchor ? previous : emptyFileTreeSelection()));
  });

  // 返回 true = 这是一次普通点击,调用方照常展开目录 / 打开文件;false = 只改了选区。
  const click = useStableCallback((event, entry) => {
    if (suppressClickRef.current) return false;
    const current = latest.current;
    setState(selectFileTreeRow(current.state, current.visiblePaths, entry.path, event, current.activeRowPath));
    return !(event.shiftKey || event.ctrlKey || event.metaKey);
  });

  const keyDown = useStableCallback((event) => {
    const current = latest.current;
    if (isDismissShortcut(event) && current.state.keys.size > 1) {
      event.preventDefault();
      event.stopPropagation();
      clear();
      return;
    }
    if ((event.ctrlKey || event.metaKey) && !event.shiftKey && !event.altKey
        && String(event.key || '').toLowerCase() === 'a' && current.visiblePaths.length) {
      event.preventDefault();
      event.stopPropagation();
      setState(selectAllFileTreeRows(current.state, current.visiblePaths));
    }
  });

  const handleContextMenuDelegate = useStableCallback((event) => {
    const detail = event.detail;
    if (detail?.name !== FILE_TREE_SELECTION_DELEGATE || !treeRef.current?.contains(detail.element)) return;
    const path = detail.element.getAttribute('data-desktop-file-path') || '';
    const current = latest.current;
    if (!current.state.keys.has(path)) {
      // 右键点在选区外:选区换成这一行,由这一行自己的菜单接手。
      if (current.visiblePaths.includes(path)) setState({ anchor: path, keys: new Set([path]) });
      return;
    }
    const entries = selectedFileTreeEntries(current.visibleEntries, current.state.keys);
    if (entries.length < 2) return;
    detail.handled = true;
    onBatchMenu?.({ entries, x: detail.x, y: detail.y, element: detail.element });
  });
  useEffect(() => {
    window.addEventListener(CONTEXT_MENU_DELEGATE_EVENT, handleContextMenuDelegate);
    return () => window.removeEventListener(CONTEXT_MENU_DELEGATE_EVENT, handleContextMenuDelegate);
  }, [handleContextMenuDelegate]);

  const pointerDown = useStableCallback((event, entry) => {
    // 带修饰键的按下是在改选区;触屏上按住滑动留给滚动。
    if (event.button !== 0 || event.shiftKey || event.ctrlKey || event.metaKey) return;
    if (event.pointerType === 'touch' || dragRef.current) return;
    const row = event.currentTarget;
    const doc = row.ownerDocument;
    const win = doc.defaultView;
    const pointerId = event.pointerId;
    const drag = {
      startX: event.clientX, startY: event.clientY, dragging: false, target: null, items: [], cwd: '',
    };

    const setTarget = (target) => {
      if (drag.target?.element === target?.element) return;
      drag.target?.element.classList.remove(FILE_TREE_DROP_ACTIVE_CLASS);
      drag.target = target;
      target?.element.classList.add(FILE_TREE_DROP_ACTIVE_CLASS);
      doc.body.classList.toggle(DROP_READY_CLASS, !!target);
    };

    const begin = () => {
      const current = latest.current;
      let entries = selectedFileTreeEntries(current.visibleEntries, current.state.keys);
      if (!current.state.keys.has(entry.path)) {
        // 从选区外的行拖起:只拖这一行,选区也换成它(与资源管理器一致)。
        entries = [entry];
        setState({ anchor: entry.path, keys: new Set([entry.path]) });
      }
      const outermost = outermostFileTreeEntries(entries);
      drag.dragging = true;
      drag.cwd = current.cwd;
      drag.items = fileTreeDropItems(outermost, current.cwd);
      suppressClickRef.current = true;
      doc.body.classList.add(DRAGGING_CLASS);
      // 捕获指针:经过内嵌页面(预览、Agent Browser)时事件仍然送到这里。
      try { row.setPointerCapture?.(pointerId); } catch { /* 行可能已卸载 */ }
      setGhost(fileTreeDragPreview(outermost));
    };

    const finish = (released) => {
      if (dragRef.current !== drag) return;
      dragRef.current = null;
      win.removeEventListener('pointermove', onMove);
      win.removeEventListener('pointerup', onUp);
      win.removeEventListener('pointercancel', onCancel);
      win.removeEventListener('keydown', onKeyDown, true);
      win.removeEventListener('blur', cancel);
      row.removeEventListener('lostpointercapture', onCancel);
      setTarget(null);
      if (!drag.dragging) return;
      try { row.releasePointerCapture?.(pointerId); } catch { /* 指针可能已释放 */ }
      doc.body.classList.remove(DRAGGING_CLASS, DROP_READY_CLASS);
      setGhost(null);
      // 松手后浏览器还会给这一行派发一次 click,等它过去再放开点击;
      // 按 Esc 取消时按键还没松,要等到真正松手之后。
      const allowClicks = () => win.setTimeout(() => { suppressClickRef.current = false; }, 0);
      if (released) allowClicks();
      else win.addEventListener('pointerup', allowClicks, { once: true, capture: true });
    };
    const cancel = () => finish(false);

    function onMove(moveEvent) {
      if (moveEvent.pointerId !== pointerId) return;
      if (!drag.dragging) {
        const distance = Math.hypot(moveEvent.clientX - drag.startX, moveEvent.clientY - drag.startY);
        if (distance < FILE_TREE_DRAG_START_PX) return;
        begin();
      }
      moveEvent.preventDefault();
      ghostPointRef.current = { x: moveEvent.clientX, y: moveEvent.clientY };
      placeGhost();
      setTarget(fileTreeDropTargetAt(doc, moveEvent.clientX, moveEvent.clientY));
    }
    function onUp(upEvent) {
      if (upEvent.pointerId !== pointerId) return;
      const { dragging, target, items, cwd: dragCwd } = drag;
      finish(true);
      if (!dragging) return;
      upEvent.preventDefault();
      if (target) {
        dispatchFileTreeDrop(target, items, {
          cwd: dragCwd, clientX: upEvent.clientX, clientY: upEvent.clientY,
        }, win);
      }
    }
    function onCancel(cancelEvent) {
      if (cancelEvent.pointerId === pointerId) cancel();
    }
    function onKeyDown(keyEvent) {
      if (!drag.dragging || !isDismissShortcut(keyEvent)) return;
      keyEvent.preventDefault();
      keyEvent.stopPropagation();
      cancel();
    }

    drag.cancel = cancel;
    dragRef.current = drag;
    win.addEventListener('pointermove', onMove);
    win.addEventListener('pointerup', onUp);
    win.addEventListener('pointercancel', onCancel);
    win.addEventListener('keydown', onKeyDown, true);
    win.addEventListener('blur', cancel);
    row.addEventListener('lostpointercapture', onCancel);
  });
  useEffect(() => () => dragRef.current?.cancel?.(), []);

  return {
    keys: state.keys,
    multiple: state.keys.size > 1,
    ghost,
    ghostRef,
    click,
    clear,
    keyDown,
    pointerDown,
    selectedEntries,
  };
}
