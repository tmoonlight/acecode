import { useCallback, useEffect, useLayoutEffect, useMemo, useRef, useState } from 'react';
import { createPortal } from 'react-dom';
import { usePreference } from '../lib/usePreference.js';
import { clsx } from '../lib/format.js';
import {
  PREVIEW_TAB_TYPES,
  previewAbsolutePath,
  previewTabHasUnsavedDraft,
  resolveSessionChangesTabContent,
} from '../lib/previewTabs.js';
import { scrollLeftForVisibleTab } from '../lib/previewTabScroll.js';
import { DESKTOP_CONTEXT_ACTION_EVENT, DESKTOP_CONTEXT_ACTIONS } from '../lib/desktopContextMenu.js';
import { FilePreviewContent } from './FilePreviewContent.jsx';
import { AgentBrowserPanel } from './AgentBrowserPanel.jsx';
import { SessionChangeDetails } from './ChangeReview.jsx';
import { GitChangeDetails } from './GitChangeReview.jsx';
import { FileTypeIcon, PanelToggleIcon, VsIcon } from './Icon.jsx';

const FILE_PREVIEW_WRAP_STORAGE_KEY = 'acecode.filePreviewWrap.v1';

function validateBooleanPreference(value) {
  return typeof value === 'boolean';
}

function fileName(path) {
  const name = String(path || '').split(/[\\/]/).filter(Boolean).pop();
  return name || String(path || '文件');
}

function tabLabel(tab) {
  if (!tab) return '';
  if (tab.type === PREVIEW_TAB_TYPES.SESSION_CHANGES) {
    const count = Number(tab.fileCount || 0);
    return `会话变更(${count} 文件)`;
  }
  if (tab.type === PREVIEW_TAB_TYPES.GIT_CHANGES) {
    const count = Number(tab.fileCount || 0);
    return count > 0 ? `变更(${count} 文件)` : '变更';
  }
  return tab.title || fileName(tab.path);
}

function BrowserTabIcon({ favicon }) {
  const [failed, setFailed] = useState(false);

  useEffect(() => {
    setFailed(false);
  }, [favicon]);

  if (!favicon || failed) {
    return <VsIcon name="globe" size={17} className="ace-preview-details-tab-icon" />;
  }
  return (
    <img
      src={favicon}
      alt=""
      aria-hidden="true"
      draggable="false"
      referrerPolicy="no-referrer"
      className="ace-preview-details-tab-icon ace-preview-details-tab-favicon"
      onError={() => setFailed(true)}
    />
  );
}

function PreviewAddMenu({
  anchorRef,
  onClose,
  onOpenTerminal,
  onOpenBrowser,
  onOpenSideChat,
}) {
  const menuRef = useRef(null);
  const [position, setPosition] = useState({ left: 0, top: 0 });
  const items = useMemo(() => [
    { key: 'terminal', label: '终端', icon: 'terminal', action: onOpenTerminal },
    { key: 'browser', label: '浏览器', icon: 'globe', action: onOpenBrowser },
    { key: 'side-chat', label: '侧边聊天', icon: 'brain', action: onOpenSideChat },
  ], [onOpenBrowser, onOpenSideChat, onOpenTerminal]);

  useLayoutEffect(() => {
    const anchor = anchorRef.current;
    if (!anchor) return;
    const rect = anchor.getBoundingClientRect();
    const width = 190;
    setPosition({
      left: Math.max(8, Math.min(rect.left, window.innerWidth - width - 8)),
      top: Math.min(rect.bottom + 6, window.innerHeight - 146),
    });
  }, [anchorRef]);

  useEffect(() => {
    const menu = menuRef.current;
    menu?.querySelector('button:not(:disabled)')?.focus();
    const handlePointerDown = (event) => {
      if (menu?.contains(event.target) || anchorRef.current?.contains(event.target)) return;
      onClose?.();
    };
    const handleKeyDown = (event) => {
      if (event.key === 'Escape') {
        event.preventDefault();
        onClose?.();
        anchorRef.current?.focus();
      }
    };
    const handleViewportChange = () => onClose?.();
    document.addEventListener('pointerdown', handlePointerDown, true);
    document.addEventListener('keydown', handleKeyDown, true);
    window.addEventListener('resize', handleViewportChange);
    window.addEventListener('scroll', handleViewportChange, true);
    return () => {
      document.removeEventListener('pointerdown', handlePointerDown, true);
      document.removeEventListener('keydown', handleKeyDown, true);
      window.removeEventListener('resize', handleViewportChange);
      window.removeEventListener('scroll', handleViewportChange, true);
    };
  }, [anchorRef, onClose]);

  const moveFocus = (event, direction) => {
    const buttons = Array.from(menuRef.current?.querySelectorAll('button:not(:disabled)') || []);
    if (buttons.length === 0) return;
    const current = buttons.indexOf(document.activeElement);
    const next = direction === 'first'
      ? 0
      : direction === 'last'
        ? buttons.length - 1
        : (current + direction + buttons.length) % buttons.length;
    event.preventDefault();
    buttons[next]?.focus();
  };

  return createPortal(
    <div
      ref={menuRef}
      className="ace-preview-add-menu"
      role="menu"
      aria-label="打开工具"
      data-ace-native-overlay="overlap"
      style={{ left: position.left, top: position.top }}
      onKeyDown={(event) => {
        if (event.key === 'ArrowDown') moveFocus(event, 1);
        else if (event.key === 'ArrowUp') moveFocus(event, -1);
        else if (event.key === 'Home') moveFocus(event, 'first');
        else if (event.key === 'End') moveFocus(event, 'last');
      }}
    >
      {items.map((item) => (
        <button
          key={item.key}
          type="button"
          role="menuitem"
          className="ace-preview-add-menu-item"
          disabled={!item.action}
          onClick={() => {
            onClose?.();
            item.action?.();
          }}
        >
          <VsIcon name={item.icon} size={16} />
          <span>{item.label}</span>
        </button>
      ))}
    </div>,
    document.body,
  );
}

function wheelDeltaForTabs(event, pageWidth) {
  const deltaX = Number(event.deltaX) || 0;
  const deltaY = Number(event.deltaY) || 0;
  let delta = Math.abs(deltaX) > Math.abs(deltaY) ? deltaX : deltaY;
  if (!delta) return 0;
  if (event.deltaMode === 1) delta *= 16;
  else if (event.deltaMode === 2) delta *= Math.max(1, pageWidth);
  return delta;
}

const TAB_DRAG_START_PX = 5;
const TAB_EDGE_SCROLL_PX = 36;
const TAB_EDGE_SCROLL_STEP = 16;

function PreviewTabScrollbar({ scrollRef }) {
  const dragRef = useRef(null);
  const cleanupDragRef = useRef(null);
  const [metrics, setMetrics] = useState({ overflow: false, left: 0, width: 0 });

  const updateMetrics = useCallback(() => {
    const el = scrollRef.current;
    if (!el) return;
    const client = el.clientWidth;
    const scroll = el.scrollWidth;
    const overflow = scroll > client + 1;
    if (!overflow) {
      setMetrics((prev) => (
        !prev.overflow && prev.left === 0 && prev.width === 0
          ? prev
          : { overflow: false, left: 0, width: 0 }
      ));
      return;
    }
    const maxScroll = Math.max(1, scroll - client);
    const width = Math.max(24, Math.round((client / scroll) * client));
    const maxLeft = Math.max(0, client - width);
    const left = Math.round((el.scrollLeft / maxScroll) * maxLeft);
    setMetrics((prev) => (
      prev.overflow && prev.left === left && prev.width === width
        ? prev
        : { overflow: true, left, width }
    ));
  }, [scrollRef]);

  useEffect(() => {
    const el = scrollRef.current;
    if (!el) return undefined;
    updateMetrics();
    el.addEventListener('scroll', updateMetrics, { passive: true });
    const resizeObserver = typeof ResizeObserver !== 'undefined'
      ? new ResizeObserver(updateMetrics)
      : null;
    resizeObserver?.observe(el);
    return () => {
      el.removeEventListener('scroll', updateMetrics);
      resizeObserver?.disconnect();
    };
  }, [scrollRef, updateMetrics]);

  useEffect(() => {
    updateMetrics();
  });

  useEffect(() => () => {
    cleanupDragRef.current?.();
  }, []);

  const onPointerDown = useCallback((event) => {
    const el = scrollRef.current;
    if (!el || !metrics.overflow) return;
    event.preventDefault();
    cleanupDragRef.current?.();
    const rect = event.currentTarget.getBoundingClientRect();
    const thumb = event.currentTarget.querySelector('.ace-preview-details-tab-scrollbar-thumb');
    const thumbRect = thumb?.getBoundingClientRect();
    const onThumb = thumbRect
      && event.clientX >= thumbRect.left
      && event.clientX <= thumbRect.right;
    const pointerOffset = onThumb && thumbRect
      ? event.clientX - thumbRect.left
      : metrics.width / 2;
    dragRef.current = {
      trackLeft: rect.left,
      trackWidth: rect.width,
      thumbWidth: metrics.width,
      pointerOffset,
    };
    document.body.classList.add('ace-tab-scroll-dragging');
    const onMove = (moveEvent) => {
      const drag = dragRef.current;
      if (!drag) return;
      const maxLeft = Math.max(1, drag.trackWidth - drag.thumbWidth);
      const nextLeft = Math.min(
        Math.max(0, moveEvent.clientX - drag.trackLeft - drag.pointerOffset),
        maxLeft,
      );
      const maxScroll = Math.max(1, el.scrollWidth - el.clientWidth);
      el.scrollLeft = (nextLeft / maxLeft) * maxScroll;
    };
    const onUp = () => {
      dragRef.current = null;
      document.body.classList.remove('ace-tab-scroll-dragging');
      window.removeEventListener('pointermove', onMove);
      window.removeEventListener('pointerup', onUp);
      window.removeEventListener('pointercancel', onUp);
      cleanupDragRef.current = null;
    };
    cleanupDragRef.current = onUp;
    window.addEventListener('pointermove', onMove);
    window.addEventListener('pointerup', onUp);
    window.addEventListener('pointercancel', onUp);
    onMove(event);
  }, [metrics.overflow, metrics.width, scrollRef]);

  if (!metrics.overflow) return null;

  return (
    <div
      className="ace-preview-details-tab-scrollbar"
      aria-hidden="true"
      onPointerDown={onPointerDown}
    >
      <div
        className="ace-preview-details-tab-scrollbar-thumb"
        style={{
          transform: `translateX(${metrics.left}px)`,
          width: `${metrics.width}px`,
        }}
      />
    </div>
  );
}

export function PreviewDetailsPanel({
  api,
  cwd,
  tabs = [],
  activeTab = null,
  changeGroups = [],
  changeSummary = null,
  turnChangeSets = [],
  maximized = false,
  busy = false,
  selectionContexts = [],
  sidePanelListCollapsed = false,
  onActivateTab,
  onCloseTab,
  onCloseOthers,
  onCloseToRight,
  onCloseAll,
  onRefreshTab,
  onEditFileTab,
  onReorderTab,
  onToggleMaximize,
  onToggleSidePanelList,
  onOpenTerminal,
  onOpenBrowser,
  onOpenSideChat,
  onHide,
  agentBrowserActive = false,
  nativeSurfacesVisible = true,
  onAddBrowserContext,
  onSelectChangeFile,
  onSelectGitChangeFile,
  onOpenFilePreview,
}) {
  const tabListRef = useRef(null);
  const tabDragRef = useRef(null);
  const tabAutoScrollFrameRef = useRef(null);
  const suppressTabClickRef = useRef(false);
  const addButtonRef = useRef(null);
  const [tabDragState, setTabDragState] = useState(null);
  const [tabDragGhost, setTabDragGhost] = useState(null);
  const [addMenuOpen, setAddMenuOpen] = useState(false);
  const [wrapPreview, setWrapPreview] = usePreference(
    FILE_PREVIEW_WRAP_STORAGE_KEY,
    false,
    validateBooleanPreference,
  );
  const active = activeTab || tabs[0] || null;

  // 预览标签右键菜单复用全局 DesktopContextMenu:菜单项命中 close_* action 后,
  // 由 DesktopContextMenu 通过 DESKTOP_CONTEXT_ACTION_EVENT 派发回来,这里执行关闭逻辑。
  useEffect(() => {
    const handler = (event) => {
      const detail = event.detail || {};
      const { action, target } = detail;
      if (target?.type !== 'preview-tab') return;
      const tabKey = target.key;
      if (!tabKey) return;
      if (action === DESKTOP_CONTEXT_ACTIONS.CLOSE_PREVIEW_TAB) {
        detail.handled = true;
        onCloseTab?.(tabKey);
      } else if (action === DESKTOP_CONTEXT_ACTIONS.CLOSE_OTHER_PREVIEW_TABS) {
        detail.handled = true;
        onCloseOthers?.(tabKey);
      } else if (action === DESKTOP_CONTEXT_ACTIONS.CLOSE_PREVIEW_TABS_TO_RIGHT) {
        detail.handled = true;
        onCloseToRight?.(tabKey);
      } else if (action === DESKTOP_CONTEXT_ACTIONS.CLOSE_ALL_PREVIEW_TABS) {
        detail.handled = true;
        onCloseAll?.();
      }
    };
    window.addEventListener(DESKTOP_CONTEXT_ACTION_EVENT, handler);
    return () => window.removeEventListener(DESKTOP_CONTEXT_ACTION_EVENT, handler);
  }, [onCloseAll, onCloseOthers, onCloseTab, onCloseToRight]);

  const renderedBody = useMemo(() => {
    if (!active) return null;
    if (active.type === PREVIEW_TAB_TYPES.BROWSER) {
      return (
        <AgentBrowserPanel
          key={active.pageId}
          pageId={active.pageId}
          agentActive={agentBrowserActive && active.pageId === agentBrowserActive}
          surfaceEnabled={nativeSurfacesVisible}
          onAddContext={onAddBrowserContext}
        />
      );
    }
    if (active.type === PREVIEW_TAB_TYPES.SESSION_CHANGES) {
      const scopedChanges = resolveSessionChangesTabContent(active, {
        changeGroups,
        changeSummary,
        turnChangeSets,
      });
      return (
        <SessionChangeDetails
          groups={scopedChanges.groups}
          summary={scopedChanges.summary}
          cwd={cwd}
          expandedFile={active.expandedFile || ''}
          expandedFileRevision={active.expandedFileRevision || 0}
          reloadRevision={active.reloadRevision || 0}
          onSelectFile={(filePath) => onSelectChangeFile?.(
            filePath,
            scopedChanges.turnUserMessageId,
          )}
          onOpenFilePreview={onOpenFilePreview}
          onRefresh={() => onRefreshTab?.(active.key)}
        />
      );
    }
    if (active.type === PREVIEW_TAB_TYPES.GIT_CHANGES) {
      return (
        <GitChangeDetails
          api={api}
          cwd={active.cwd || cwd}
          base={active.base || ''}
          expandedFile={active.expandedFile || ''}
          expandedFileRevision={active.expandedFileRevision || 0}
          reloadRevision={active.reloadRevision || 0}
          busy={busy}
          onSelectFile={onSelectGitChangeFile}
          onOpenFilePreview={onOpenFilePreview}
          onRefresh={() => onRefreshTab?.(active.key)}
        />
      );
    }
    const activeCwd = active.type === PREVIEW_TAB_TYPES.FILE ? (active.cwd || cwd) : cwd;
    return (
      <FilePreviewContent
        api={api}
        cwd={activeCwd}
        path={active.path}
        focusLine={active.line || null}
        focusLineRevision={active.lineRevision || 0}
        reloadRevision={active.reloadRevision || 0}
        wrapPreview={wrapPreview}
        selectionContexts={selectionContexts}
        editState={active.edit || null}
        onEditStateChange={(patch) => onEditFileTab?.(active.key, patch)}
        onToggleWrapPreview={() => setWrapPreview((prev) => !prev)}
        onRefresh={() => onRefreshTab?.(active.key)}
      />
    );
  }, [active, agentBrowserActive, api, busy, changeGroups, changeSummary, cwd, nativeSurfacesVisible, onAddBrowserContext, onEditFileTab, onOpenFilePreview, onRefreshTab, onSelectChangeFile, onSelectGitChangeFile, selectionContexts, setWrapPreview, turnChangeSets, wrapPreview]);

  const handleTabWheel = useCallback((event) => {
    const el = tabListRef.current;
    if (!el) return;
    const maxScroll = Math.max(0, el.scrollWidth - el.clientWidth);
    if (maxScroll <= 1) return;
    const delta = wheelDeltaForTabs(event, el.clientWidth);
    if (!delta) return;
    const previous = el.scrollLeft;
    const next = Math.min(Math.max(0, previous + delta), maxScroll);
    if (next === previous) return;
    event.preventDefault();
    el.scrollLeft = next;
  }, []);

  const stopTabAutoScroll = useCallback(() => {
    if (tabAutoScrollFrameRef.current != null) {
      window.cancelAnimationFrame(tabAutoScrollFrameRef.current);
      tabAutoScrollFrameRef.current = null;
    }
  }, []);

  const suppressNextTabClick = useCallback(() => {
    suppressTabClickRef.current = true;
    window.setTimeout(() => {
      suppressTabClickRef.current = false;
    }, 0);
  }, []);

  const updateTabDragGhost = useCallback((clientX, clientY) => {
    const drag = tabDragRef.current;
    if (!drag?.dragging) return;
    setTabDragGhost({
      left: clientX - drag.offsetX,
      top: clientY - drag.offsetY,
      width: drag.width,
      label: drag.label,
      isFileTab: drag.isFileTab,
      path: drag.path,
    });
  }, []);

  const insertionTargetForPointer = useCallback((clientX) => {
    const list = tabListRef.current;
    if (!list) return null;
    const tabEls = Array.from(list.querySelectorAll('.ace-preview-details-tab[data-preview-tab-key]'));
    if (tabEls.length === 0) return null;

    const firstRect = tabEls[0].getBoundingClientRect();
    if (clientX <= firstRect.left) {
      return { targetKey: tabEls[0].dataset.previewTabKey, placement: 'before' };
    }

    const lastEl = tabEls[tabEls.length - 1];
    const lastRect = lastEl.getBoundingClientRect();
    if (clientX >= lastRect.right) {
      return { targetKey: lastEl.dataset.previewTabKey, placement: 'after' };
    }

    for (const tabEl of tabEls) {
      const rect = tabEl.getBoundingClientRect();
      if (clientX >= rect.left && clientX <= rect.right) {
        return {
          targetKey: tabEl.dataset.previewTabKey,
          placement: clientX < rect.left + rect.width / 2 ? 'before' : 'after',
        };
      }
    }

    for (const tabEl of tabEls) {
      const rect = tabEl.getBoundingClientRect();
      if (clientX < rect.left + rect.width / 2) {
        return { targetKey: tabEl.dataset.previewTabKey, placement: 'before' };
      }
    }
    return { targetKey: lastEl.dataset.previewTabKey, placement: 'after' };
  }, []);

  const updateDragTarget = useCallback((clientX) => {
    const drag = tabDragRef.current;
    if (!drag) return;
    const target = insertionTargetForPointer(clientX);
    if (!target) return;
    drag.targetKey = target.targetKey;
    drag.placement = target.placement;
    setTabDragState({
      sourceKey: drag.sourceKey,
      targetKey: target.targetKey,
      placement: target.placement,
    });
  }, [insertionTargetForPointer]);

  const ensureTabAutoScroll = useCallback(() => {
    if (tabAutoScrollFrameRef.current != null) return;
    const tick = () => {
      const drag = tabDragRef.current;
      if (!drag?.dragging || !drag.autoScrollDir) {
        tabAutoScrollFrameRef.current = null;
        return;
      }
      const list = tabListRef.current;
      if (list) {
        list.scrollLeft += drag.autoScrollDir * TAB_EDGE_SCROLL_STEP;
        updateDragTarget(drag.lastClientX);
      }
      tabAutoScrollFrameRef.current = window.requestAnimationFrame(tick);
    };
    tabAutoScrollFrameRef.current = window.requestAnimationFrame(tick);
  }, [updateDragTarget]);

  const updateTabAutoScroll = useCallback((clientX) => {
    const drag = tabDragRef.current;
    const list = tabListRef.current;
    if (!drag || !list) return;
    const rect = list.getBoundingClientRect();
    let dir = 0;
    if (clientX < rect.left + TAB_EDGE_SCROLL_PX && list.scrollLeft > 0) dir = -1;
    else if (
      clientX > rect.right - TAB_EDGE_SCROLL_PX
      && list.scrollLeft < list.scrollWidth - list.clientWidth - 1
    ) dir = 1;
    drag.autoScrollDir = dir;
    if (dir) ensureTabAutoScroll();
  }, [ensureTabAutoScroll]);

  const finishTabDrag = useCallback((commit) => {
    const drag = tabDragRef.current;
    if (!drag) return;
    drag.cleanup?.();
    tabDragRef.current = null;
    document.body.classList.remove('ace-preview-tab-reordering');
    setTabDragState(null);
    setTabDragGhost(null);
    if (drag.dragging) suppressNextTabClick();
    if (commit && drag.dragging && drag.targetKey) {
      onReorderTab?.(drag.sourceKey, drag.targetKey, drag.placement || 'before');
    }
  }, [onReorderTab, suppressNextTabClick]);

  const handleTabPointerDown = useCallback((event, tabKey) => {
    if (event.button !== 0) return;
    if (tabDragRef.current) return;
    if (event.target?.closest?.('.ace-preview-details-tab-close')) return;

    finishTabDrag(false);
    const tab = tabs.find((item) => item.key === tabKey);
    if (!tab) return;
    const pointerId = event.pointerId;
    const pointerTarget = event.currentTarget;
    const rect = pointerTarget.getBoundingClientRect();
    const label = tabLabel(tab);
    const isFileTab = tab.type === PREVIEW_TAB_TYPES.FILE;
    try {
      pointerTarget.setPointerCapture?.(pointerId);
    } catch {
      // Best effort only; window-level listeners below still own the drag.
    }

    const cleanup = () => {
      stopTabAutoScroll();
      window.removeEventListener('pointermove', onMove);
      window.removeEventListener('pointerup', onUp);
      window.removeEventListener('pointercancel', onCancel);
      try {
        pointerTarget.releasePointerCapture?.(pointerId);
      } catch {
        // The pointer may already be released if the element unmounted.
      }
    };

    const beginDragIfNeeded = (moveEvent) => {
      const drag = tabDragRef.current;
      if (!drag || drag.dragging) return true;
      const dx = moveEvent.clientX - drag.startX;
      const dy = moveEvent.clientY - drag.startY;
      if (Math.hypot(dx, dy) < TAB_DRAG_START_PX) return false;
      drag.dragging = true;
      document.body.classList.add('ace-preview-tab-reordering');
      setTabDragState({
        sourceKey: drag.sourceKey,
        targetKey: drag.sourceKey,
        placement: 'before',
      });
      updateTabDragGhost(moveEvent.clientX, moveEvent.clientY);
      return true;
    };

    function onMove(moveEvent) {
      const drag = tabDragRef.current;
      if (!drag || moveEvent.pointerId !== pointerId) return;
      drag.lastClientX = moveEvent.clientX;
      if (!beginDragIfNeeded(moveEvent)) return;
      moveEvent.preventDefault();
      updateTabDragGhost(moveEvent.clientX, moveEvent.clientY);
      updateDragTarget(moveEvent.clientX);
      updateTabAutoScroll(moveEvent.clientX);
    }

    function onUp(upEvent) {
      const drag = tabDragRef.current;
      if (!drag || upEvent.pointerId !== pointerId) return;
      if (drag.dragging) upEvent.preventDefault();
      finishTabDrag(true);
    }

    function onCancel(cancelEvent) {
      const drag = tabDragRef.current;
      if (!drag || cancelEvent.pointerId !== pointerId) return;
      finishTabDrag(false);
    }

    tabDragRef.current = {
      sourceKey: tabKey,
      targetKey: tabKey,
      placement: 'before',
      startX: event.clientX,
      startY: event.clientY,
      lastClientX: event.clientX,
      offsetX: event.clientX - rect.left,
      offsetY: event.clientY - rect.top,
      width: Math.max(104, rect.width || (rect.right - rect.left) || 104),
      label,
      isFileTab,
      path: isFileTab ? (tab.path || label) : '',
      dragging: false,
      autoScrollDir: 0,
      cleanup,
    };
    window.addEventListener('pointermove', onMove);
    window.addEventListener('pointerup', onUp);
    window.addEventListener('pointercancel', onCancel);
  }, [
    finishTabDrag,
    stopTabAutoScroll,
    tabs,
    updateDragTarget,
    updateTabDragGhost,
    updateTabAutoScroll,
  ]);

  const handleTabMouseDown = useCallback((event, tabKey) => {
    if (event.button !== 0) return;
    if (tabDragRef.current) return;
    if (event.target?.closest?.('.ace-preview-details-tab-close')) return;

    finishTabDrag(false);
    const tab = tabs.find((item) => item.key === tabKey);
    if (!tab) return;
    const rect = event.currentTarget.getBoundingClientRect();
    const label = tabLabel(tab);
    const isFileTab = tab.type === PREVIEW_TAB_TYPES.FILE;

    const cleanup = () => {
      stopTabAutoScroll();
      window.removeEventListener('mousemove', onMove);
      window.removeEventListener('mouseup', onUp);
    };

    const beginDragIfNeeded = (moveEvent) => {
      const drag = tabDragRef.current;
      if (!drag || drag.dragging) return true;
      const dx = moveEvent.clientX - drag.startX;
      const dy = moveEvent.clientY - drag.startY;
      if (Math.hypot(dx, dy) < TAB_DRAG_START_PX) return false;
      drag.dragging = true;
      document.body.classList.add('ace-preview-tab-reordering');
      setTabDragState({
        sourceKey: drag.sourceKey,
        targetKey: drag.sourceKey,
        placement: 'before',
      });
      updateTabDragGhost(moveEvent.clientX, moveEvent.clientY);
      return true;
    };

    function onMove(moveEvent) {
      const drag = tabDragRef.current;
      if (!drag) return;
      drag.lastClientX = moveEvent.clientX;
      if (!beginDragIfNeeded(moveEvent)) return;
      moveEvent.preventDefault();
      updateTabDragGhost(moveEvent.clientX, moveEvent.clientY);
      updateDragTarget(moveEvent.clientX);
      updateTabAutoScroll(moveEvent.clientX);
    }

    function onUp(upEvent) {
      const drag = tabDragRef.current;
      if (!drag) return;
      if (drag.dragging) upEvent.preventDefault();
      finishTabDrag(true);
    }

    tabDragRef.current = {
      sourceKey: tabKey,
      targetKey: tabKey,
      placement: 'before',
      startX: event.clientX,
      startY: event.clientY,
      lastClientX: event.clientX,
      offsetX: event.clientX - rect.left,
      offsetY: event.clientY - rect.top,
      width: Math.max(104, rect.width || (rect.right - rect.left) || 104),
      label,
      isFileTab,
      path: isFileTab ? (tab.path || label) : '',
      dragging: false,
      autoScrollDir: 0,
      cleanup,
    };
    window.addEventListener('mousemove', onMove);
    window.addEventListener('mouseup', onUp);
  }, [
    finishTabDrag,
    stopTabAutoScroll,
    tabs,
    updateDragTarget,
    updateTabDragGhost,
    updateTabAutoScroll,
  ]);

  useEffect(() => () => {
    const drag = tabDragRef.current;
    drag?.cleanup?.();
    tabDragRef.current = null;
    document.body.classList.remove('ace-preview-tab-reordering');
    stopTabAutoScroll();
  }, [stopTabAutoScroll]);

  useLayoutEffect(() => {
    const el = tabListRef.current;
    if (!el || !active?.key) return;
    const activeTabEl = el.querySelector('.ace-preview-details-tab[aria-selected="true"]');
    if (!activeTabEl) return;
    const next = scrollLeftForVisibleTab({
      scrollLeft: el.scrollLeft,
      clientWidth: el.clientWidth,
      scrollWidth: el.scrollWidth,
      tabOffsetLeft: activeTabEl.offsetLeft,
      tabOffsetWidth: activeTabEl.offsetWidth,
    });
    if (next !== el.scrollLeft) el.scrollLeft = next;
  }, [active?.key, tabs]);

  if (!active || tabs.length === 0) return null;

  return (
    <div className="ace-preview-details-panel" data-maximized={maximized ? 'true' : 'false'}>
      <div className="ace-preview-details-tabs">
        <div className="ace-preview-details-tab-scroll-shell" onWheel={handleTabWheel}>
          <div ref={tabListRef} className="ace-preview-details-tab-list" role="tablist" aria-label="预览标签页">
            {tabs.map((tab, tabIndex) => {
              const selected = active.key === tab.key;
              const label = tabLabel(tab);
              const isFileTab = tab.type === PREVIEW_TAB_TYPES.FILE;
              const isBrowserTab = tab.type === PREVIEW_TAB_TYPES.BROWSER;
              const dirty = previewTabHasUnsavedDraft(tab);
              const tabAbsolutePath = isFileTab
                ? previewAbsolutePath({ cwd: tab.cwd || cwd || '', path: tab.path || '' })
                : '';
              return (
                <button
                  key={tab.key}
                  type="button"
                  role="tab"
                  aria-selected={selected}
                  data-preview-tab-key={tab.key}
                  data-desktop-preview-tab-key={tab.key}
                  data-desktop-preview-tab-type={isFileTab ? 'file' : tab.type}
                  data-desktop-preview-tab-path={isFileTab ? (tab.path || '') : undefined}
                  data-desktop-preview-tab-absolute-path={isFileTab ? tabAbsolutePath : undefined}
                  data-desktop-preview-tab-has-others={tabs.length > 1 ? 'true' : 'false'}
                  data-desktop-preview-tab-has-right={tabIndex < tabs.length - 1 ? 'true' : 'false'}
                  data-preview-tab-dirty={dirty ? 'true' : undefined}
                  className={clsx(
                    'ace-preview-details-tab',
                    selected && 'is-active',
                    tabDragState?.sourceKey === tab.key && 'is-dragging',
                    tabDragState?.targetKey === tab.key
                      && tabDragState.placement === 'before'
                      && 'is-drop-before',
                    tabDragState?.targetKey === tab.key
                      && tabDragState.placement === 'after'
                      && 'is-drop-after',
                  )}
                  title={tab.path || label}
                  onPointerDown={(event) => handleTabPointerDown(event, tab.key)}
                  onMouseDown={(event) => handleTabMouseDown(event, tab.key)}
                  onClick={(event) => {
                    if (suppressTabClickRef.current) {
                      event.preventDefault();
                      event.stopPropagation();
                      return;
                    }
                    onActivateTab?.(tab.key);
                  }}
                >
                  {isFileTab && (
                    <FileTypeIcon
                      path={tab.path || label}
                      size={20}
                      className="ace-preview-details-tab-icon"
                    />
                  )}
                  {isBrowserTab && (
                    <BrowserTabIcon favicon={tab.favicon} />
                  )}
                  <span className="ace-preview-details-tab-label">{label}</span>
                  <span
                    role="button"
                    tabIndex={-1}
                    className={clsx('ace-preview-details-tab-close', dirty && 'is-dirty')}
                    title="关闭标签页"
                    aria-label={`关闭 ${label}`}
                    onPointerDown={(event) => {
                      event.stopPropagation();
                    }}
                    onMouseDown={(event) => {
                      event.stopPropagation();
                    }}
                    onClick={(event) => {
                      event.preventDefault();
                      event.stopPropagation();
                      onCloseTab?.(tab.key);
                    }}
                  >
                    {dirty && <span className="ace-preview-details-tab-dirty-dot" aria-hidden="true" />}
                    <VsIcon name="close" size={18} className="ace-preview-details-tab-close-icon" />
                  </span>
                </button>
              );
            })}
          </div>
          <PreviewTabScrollbar scrollRef={tabListRef} />
        </div>
        <button
          ref={addButtonRef}
          type="button"
          className="ace-preview-details-add"
          title="打开工具"
          aria-label="打开工具"
          aria-haspopup="menu"
          aria-expanded={addMenuOpen}
          onClick={() => setAddMenuOpen((open) => !open)}
        >
          <VsIcon name="add" size={15} />
        </button>
        <div className="ace-preview-details-actions">
          {sidePanelListCollapsed && onToggleSidePanelList && (
            <button
              type="button"
              className="ace-preview-details-action"
              onClick={onToggleSidePanelList}
              title="展开列表面板"
              aria-label="展开列表面板"
              aria-expanded="false"
            >
              <PanelToggleIcon side="right" size={15} />
            </button>
          )}
          <button
            type="button"
            className="ace-preview-details-action"
            onClick={onToggleMaximize}
            title={maximized ? '还原预览面板' : '最大化预览面板'}
            aria-label={maximized ? '还原预览面板' : '最大化预览面板'}
            aria-pressed={maximized}
          >
            <VsIcon name={maximized ? 'screenNormal' : 'screenFull'} size={14} />
          </button>
          <button
            type="button"
            className="ace-preview-details-action"
            onClick={onHide}
            title="隐藏详情面板"
            aria-label="隐藏详情面板"
          >
            <VsIcon name="close" size={14} />
          </button>
        </div>
      </div>
      <div className="ace-preview-details-body">
        {renderedBody}
      </div>
      {tabDragGhost && createPortal(
        <div
          className="ace-preview-details-tab-drag-ghost"
          data-ace-native-overlay="overlap"
          style={{
            left: tabDragGhost.left,
            top: tabDragGhost.top,
            width: tabDragGhost.width,
          }}
        >
          {tabDragGhost.isFileTab && (
            <FileTypeIcon
              path={tabDragGhost.path || tabDragGhost.label}
              size={20}
              className="ace-preview-details-tab-icon"
            />
          )}
          <span className="ace-preview-details-tab-label">{tabDragGhost.label}</span>
        </div>,
        document.body,
      )}
      {addMenuOpen && (
        <PreviewAddMenu
          anchorRef={addButtonRef}
          onClose={() => setAddMenuOpen(false)}
          onOpenTerminal={onOpenTerminal}
          onOpenBrowser={onOpenBrowser}
          onOpenSideChat={onOpenSideChat}
        />
      )}
    </div>
  );
}
