import { isDismissShortcut } from '../lib/shortcutBindings.js';
// 侧栏「扩展」入口:点击向右弹出菜单(未勾选的扩展条目 + 「自定义」),
// 「自定义」打开对话框,勾选的条目直接固定到侧栏主导航,拖动行调整顺序。
// 顺序与勾选状态的纯逻辑在 lib/sidebarNavigation.js,这里只负责交互与渲染。

import { useCallback, useEffect, useLayoutEffect, useRef, useState } from 'react';
import { api } from '../lib/api.js';
import { clsx } from '../lib/format.js';
import { usePreference } from '../lib/usePreference.js';
import {
  DEFAULT_SIDEBAR_EXTENSION_PREFS,
  moveSidebarExtension,
  normalizeSidebarExtensionPrefs,
  sidebarExtensionDropIndex,
  sidebarExtensionLayout,
  toggleSidebarExtensionPinned,
  validateSidebarExtensionPrefs,
} from '../lib/sidebarNavigation.js';
import { AnchoredMenu } from './AnchoredMenu.jsx';
import { Modal } from './Modal.jsx';
import { VsIcon } from './Icon.jsx';

const SIDEBAR_EXTENSION_PREFS_STORAGE_KEY = 'acecode.sidebarExtensions.v1';
const DRAG_START_PX = 4;

function countObjectKeys(value) {
  if (!value || typeof value !== 'object' || Array.isArray(value)) return 0;
  return Object.keys(value).length;
}

function useSidebarExtensionCounts(workspaceHash) {
  const [counts, setCounts] = useState({ models: null, skills: null, mcp: null, experts: null });

  const refreshCounts = useCallback(async () => {
    const [models, skills, mcp, experts] = await Promise.allSettled([
      api.listModels(),
      api.listSkills(),
      api.getMcp(),
      api.listExperts(workspaceHash || '__local__'),
    ]);
    setCounts((previous) => ({
      models: models.status === 'fulfilled' && Array.isArray(models.value)
        ? models.value.length
        : previous.models,
      skills: skills.status === 'fulfilled' && Array.isArray(skills.value)
        ? skills.value.length
        : previous.skills,
      mcp: mcp.status === 'fulfilled' ? countObjectKeys(mcp.value) : previous.mcp,
      experts: experts.status === 'fulfilled'
        ? (Array.isArray(experts.value?.experts) ? experts.value.experts.length : 0)
        : previous.experts,
    }));
  }, [workspaceHash]);

  useEffect(() => {
    refreshCounts().catch(() => {});
    const timer = window.setInterval(() => refreshCounts().catch(() => {}), 15000);
    return () => window.clearInterval(timer);
  }, [refreshCounts]);

  return counts;
}

function ExtensionCount({ count, className = '' }) {
  if (!Number.isFinite(count)) return null;
  return <span className={clsx('shrink-0 tabular-nums text-[11px] text-fg-mute', className)}>{count}</span>;
}

function PinnedExtensionItem({ item, count, onClick }) {
  return (
    <button
      type="button"
      onClick={onClick}
      data-sidebar-custom-item={item.id}
      className="ace-sidebar-primary-text w-full flex items-center gap-[7px] pl-[9px] pr-[13px] py-[3px] rounded-md text-[14px] text-fg hover:bg-surface-hi transition text-left"
    >
      <span className="w-6 h-6 flex items-center justify-center shrink-0">
        <VsIcon name={item.icon} size={18} className="ace-sidebar-custom-icon" />
      </span>
      <span className="flex-1 min-w-0 truncate">{item.label}</span>
      <ExtensionCount count={count} className="mr-2" />
    </button>
  );
}

function ExtensionCheckmark({ checked }) {
  return (
    <span
      aria-hidden="true"
      className={clsx(
        'w-[18px] h-[18px] shrink-0 rounded-full flex items-center justify-center transition',
        checked ? 'bg-accent text-white' : 'border-[1.5px] border-fg-mute',
      )}
    >
      {checked && <VsIcon name="check" size={12} strong />}
    </span>
  );
}

function SidebarExtensionCustomizer({ prefs, onChange, onClose }) {
  const titleId = 'sidebar-extension-customizer-title';
  const layout = sidebarExtensionLayout(prefs);
  const listRef = useRef(null);
  const dragRef = useRef(null);
  const prefsRef = useRef(prefs);
  const suppressClickRef = useRef(false);
  const [draggingId, setDraggingId] = useState('');
  prefsRef.current = prefs;

  // 行随重排跳到新槽位后,被拖行的位移要按新槽位重新算,指针下的行才不会跳。
  const applyDragTransform = useCallback(() => {
    const drag = dragRef.current;
    const list = listRef.current;
    if (!drag?.active || !list) return;
    const row = list.querySelector(`[data-sidebar-extension-row="${drag.id}"]`);
    if (!row) return;
    const desiredTop = drag.startOffsetTop + drag.pointerY - drag.startY;
    const clampedTop = Math.max(0, Math.min(desiredTop, list.clientHeight - row.offsetHeight));
    row.style.transform = `translateY(${clampedTop - row.offsetTop}px)`;
  }, []);

  useLayoutEffect(() => { applyDragTransform(); });
  useEffect(() => () => dragRef.current?.cleanup?.(), []);

  const onRowPointerDown = (event, id) => {
    if (event.button !== 0 || event.isPrimary === false || dragRef.current) return;
    const row = event.currentTarget;
    const list = listRef.current;
    if (!list) return;
    const pointerId = event.pointerId;
    const startPrefs = prefsRef.current;
    const previousSelect = document.body.style.userSelect;
    const drag = {
      id,
      active: false,
      startY: event.clientY,
      startX: event.clientX,
      pointerY: event.clientY,
      startOffsetTop: row.offsetTop,
    };

    const cleanup = () => {
      window.removeEventListener('pointermove', move);
      window.removeEventListener('pointerup', up);
      window.removeEventListener('pointercancel', cancel);
      window.removeEventListener('keydown', key, true);
      window.removeEventListener('blur', cancel);
      if (drag.active) {
        document.body.style.userSelect = previousSelect;
        try { row.releasePointerCapture(pointerId); } catch { /* Already released. */ }
        row.style.transform = '';
        // 拖完松手会在行内按钮上补一个 click,不能把它当成勾选。
        suppressClickRef.current = true;
        window.setTimeout(() => { suppressClickRef.current = false; }, 0);
      }
      dragRef.current = null;
      setDraggingId('');
    };
    const move = (moveEvent) => {
      if (moveEvent.pointerId !== pointerId) return;
      drag.pointerY = moveEvent.clientY;
      if (!drag.active) {
        if (Math.hypot(moveEvent.clientX - drag.startX, moveEvent.clientY - drag.startY) < DRAG_START_PX) return;
        drag.active = true;
        document.body.style.userSelect = 'none';
        // 只在真正开始拖动后才捕获指针:提前捕获会把普通点击的 click 目标改成整行,
        // 行内的勾选按钮就收不到点击了。
        try { row.setPointerCapture(pointerId); } catch { /* Window listeners still track. */ }
        setDraggingId(id);
      }
      moveEvent.preventDefault();
      const others = [...list.querySelectorAll('[data-sidebar-extension-row]')]
        .filter((element) => element.dataset.sidebarExtensionRow !== id)
        .map((element) => element.getBoundingClientRect());
      const dropIndex = sidebarExtensionDropIndex(others, drag.pointerY);
      const current = prefsRef.current;
      const currentIndex = normalizeSidebarExtensionPrefs(current).order.indexOf(id);
      if (dropIndex !== currentIndex) {
        const next = moveSidebarExtension(current, id, dropIndex);
        // 重渲染之前可能还会来几次 pointermove,先把 ref 推进,避免按旧顺序重复计算。
        prefsRef.current = next;
        onChange(next);
      }
      applyDragTransform();
    };
    const up = (upEvent) => {
      if (upEvent.pointerId !== pointerId) return;
      cleanup();
    };
    const cancel = () => {
      if (drag.active) onChange(startPrefs);
      cleanup();
    };
    const key = (keyEvent) => {
      if (!isDismissShortcut(keyEvent) || !drag.active) return;
      // Esc 只撤销这次拖动,不能顺带把对话框关掉。
      keyEvent.preventDefault();
      keyEvent.stopImmediatePropagation();
      cancel();
    };

    drag.cleanup = cleanup;
    dragRef.current = drag;
    window.addEventListener('pointermove', move, { passive: false });
    window.addEventListener('pointerup', up);
    window.addEventListener('pointercancel', cancel);
    window.addEventListener('keydown', key, true);
    window.addEventListener('blur', cancel);
  };

  const onHandleKeyDown = (event, id, index) => {
    if (event.key !== 'ArrowUp' && event.key !== 'ArrowDown') return;
    event.preventDefault();
    event.stopPropagation();
    const nextIndex = index + (event.key === 'ArrowUp' ? -1 : 1);
    if (nextIndex < 0 || nextIndex >= layout.entries.length) return;
    onChange(moveSidebarExtension(prefsRef.current, id, nextIndex));
  };

  return (
    <Modal onClose={onClose} width={320} labelledBy={titleId}>
      <div className="p-2" data-sidebar-extension-customizer="true">
        <div className="flex items-center justify-between pl-2 pr-1 pt-1 pb-1.5">
          <h2 id={titleId} className="text-[13px] font-normal text-fg-mute">自定义</h2>
          <button
            type="button"
            data-ace-dialog-primary="true"
            onClick={onClose}
            className="h-7 px-2 rounded-md text-[13px] font-medium text-accent hover:bg-surface-hi transition"
          >
            完成
          </button>
        </div>
        <p className="sr-only">勾选的条目固定显示在侧栏,其余条目收在「扩展」菜单里;拖动行可调整顺序。</p>
        <div ref={listRef} role="list" className="relative">
          {layout.entries.map(({ item, pinned }, index) => (
            <div
              key={item.id}
              role="listitem"
              data-sidebar-extension-row={item.id}
              onPointerDown={(event) => onRowPointerDown(event, item.id)}
              onDragStart={(event) => event.preventDefault()}
              className={clsx(
                'relative flex items-center gap-1 rounded-md bg-surface touch-none select-none',
                draggingId === item.id ? 'z-10 ace-shadow-lg cursor-grabbing' : 'hover:bg-surface-hi',
              )}
            >
              <button
                type="button"
                role="checkbox"
                aria-checked={pinned}
                onClick={() => {
                  if (suppressClickRef.current) return;
                  onChange(toggleSidebarExtensionPinned(prefsRef.current, item.id));
                }}
                className="flex-1 min-w-0 h-9 pl-2 flex items-center gap-3 text-left text-[14px] text-fg rounded-md focus:outline-none focus-visible:ring-1 focus-visible:ring-accent"
              >
                <ExtensionCheckmark checked={pinned} />
                <span className="w-5 shrink-0 flex items-center justify-center text-fg-2">
                  <VsIcon name={item.icon} size={18} className="ace-sidebar-custom-icon" />
                </span>
                <span className="flex-1 min-w-0 truncate">{item.label}</span>
              </button>
              <button
                type="button"
                aria-label={`调整 ${item.label} 的顺序`}
                title="拖动排序，也可按上下方向键"
                onKeyDown={(event) => onHandleKeyDown(event, item.id, index)}
                onClick={(event) => event.preventDefault()}
                className={clsx(
                  'w-7 h-9 shrink-0 flex items-center justify-center rounded-md text-fg-mute hover:text-fg focus:outline-none focus-visible:ring-1 focus-visible:ring-accent',
                  draggingId === item.id ? 'cursor-grabbing' : 'cursor-grab',
                )}
              >
                <VsIcon name="GripVertical" size={16} />
              </button>
            </div>
          ))}
        </div>
      </div>
    </Modal>
  );
}

export function SidebarExtensions({ workspaceHash = '', onOpenSettingsSection, onOpenExpertComponents }) {
  const anchorRef = useRef(null);
  const [menuOpen, setMenuOpen] = useState(false);
  const [customizing, setCustomizing] = useState(false);
  const [storedPrefs, setStoredPrefs] = usePreference(
    SIDEBAR_EXTENSION_PREFS_STORAGE_KEY,
    DEFAULT_SIDEBAR_EXTENSION_PREFS,
    validateSidebarExtensionPrefs,
  );
  const prefs = normalizeSidebarExtensionPrefs(storedPrefs);
  const layout = sidebarExtensionLayout(prefs);
  const counts = useSidebarExtensionCounts(workspaceHash);

  const openItem = (item) => {
    setMenuOpen(false);
    if (item.action === 'experts') onOpenExpertComponents?.();
    else onOpenSettingsSection?.(item.settingsSection);
  };

  return (
    <>
      {layout.pinned.map((item) => (
        <PinnedExtensionItem
          key={item.id}
          item={item}
          count={counts[item.id]}
          onClick={() => openItem(item)}
        />
      ))}
      <button
        ref={anchorRef}
        type="button"
        onClick={() => setMenuOpen((value) => !value)}
        data-sidebar-custom-section="true"
        aria-haspopup="menu"
        aria-expanded={menuOpen}
        aria-controls={menuOpen ? 'sidebar-extensions-menu' : undefined}
        className={clsx(
          'ace-sidebar-extensions-trigger ace-sidebar-primary-text w-full flex items-center gap-[7px] pl-[9px] pr-[13px] py-[3px] rounded-md text-[14px] text-fg hover:bg-surface-hi transition',
          menuOpen && 'bg-surface-hi',
        )}
      >
        <span className="w-6 h-6 flex items-center justify-center shrink-0">
          <VsIcon name="extension" size={18} />
        </span>
        <span className="flex-1 min-w-0 text-left truncate">扩展</span>
        <VsIcon name="expandRight" size={16} className="shrink-0 text-fg-mute" />
      </button>
      {menuOpen && (
        <AnchoredMenu
          anchorRef={anchorRef}
          onClose={() => setMenuOpen(false)}
          width={200}
          preferredPlacement="right"
          id="sidebar-extensions-menu"
          role="menu"
          aria-label="扩展"
          className="z-50 rounded-xl border border-border bg-surface p-1 ace-shadow-lg"
        >
          {layout.menu.map((item) => (
            <button
              key={item.id}
              type="button"
              role="menuitem"
              data-sidebar-extension-menu-item={item.id}
              onClick={() => openItem(item)}
              className="w-full h-8 px-2 rounded-md flex items-center gap-2 text-[13px] text-fg-2 hover:bg-surface-hi hover:text-fg transition text-left"
            >
              <span className="w-5 shrink-0 flex items-center justify-center">
                <VsIcon name={item.icon} size={18} className="ace-sidebar-custom-icon" />
              </span>
              <span className="flex-1 min-w-0 truncate">{item.label}</span>
              <ExtensionCount count={counts[item.id]} />
            </button>
          ))}
          {layout.menu.length > 0 && <div role="separator" className="h-px bg-border mx-1 my-1" />}
          <button
            type="button"
            role="menuitem"
            data-sidebar-extension-customize="true"
            onClick={() => {
              setMenuOpen(false);
              setCustomizing(true);
            }}
            className="w-full h-8 px-2 rounded-md flex items-center gap-2 text-[13px] text-fg-2 hover:bg-surface-hi hover:text-fg transition text-left"
          >
            <span className="w-5 shrink-0 flex items-center justify-center">
              <VsIcon name="sliders" size={18} />
            </span>
            <span className="flex-1 min-w-0 truncate">自定义</span>
          </button>
        </AnchoredMenu>
      )}
      {customizing && (
        <SidebarExtensionCustomizer
          prefs={prefs}
          onChange={setStoredPrefs}
          onClose={() => setCustomizing(false)}
        />
      )}
    </>
  );
}
