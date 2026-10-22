import { useLayoutEffect, useRef, useState } from 'react';
import { clsx } from '../lib/format.js';
import {
  AGENT_BROWSER_COOKIE_MODES,
  agentBrowserClearBrowsingDataDialog,
  agentBrowserCookieModeOption,
  clearAgentBrowserBrowsingData,
  setAgentBrowserCookieMode,
  waitForAgentBrowserStorageEvent,
} from '../lib/agentBrowserCookieMode.js';
import { notifyNativeSurfaceOverlayChange } from '../lib/agentBrowserSurfaceCoordinator.js';
import { submenuPosition } from '../lib/submenuPosition.js';
import { useAgentBrowserCookieMode } from '../lib/useAgentBrowserCookieMode.js';
import { AnchoredMenu } from './AnchoredMenu.jsx';
import { VsIcon } from './Icon.jsx';
import { Modal } from './Modal.jsx';
import { toast } from './Toast.jsx';

const MENU_ITEM_CLASS = 'w-full h-8 px-2 rounded-md flex items-center gap-2 text-[13px] text-fg-2 hover:bg-surface-hi hover:text-fg transition text-left disabled:opacity-50 disabled:cursor-default';

// Agent Browser 工具栏最右侧的「⋮」菜单(仿 Claude 桌面版内置浏览器的设置菜单):
// 控制台日志进聊天、「保留 Cookie」三档(子菜单)、清除浏览数据。cookie 两项只在
// Desktop 提供对应 bridge 时出现。
export function AgentBrowserMoreMenu({ pageId, ready, onAddConsoleLogs }) {
  const anchorRef = useRef(null);
  const submenuTriggerRef = useRef(null);
  const submenuRef = useRef(null);
  const [open, setOpen] = useState(false);
  const [submenuOpen, setSubmenuOpen] = useState(false);
  const [confirmOpen, setConfirmOpen] = useState(false);
  const [clearing, setClearing] = useState(false);
  const cookie = useAgentBrowserCookieMode();
  const currentMode = cookie.pending || cookie.mode;
  const currentOption = agentBrowserCookieModeOption(currentMode);
  const dialog = agentBrowserClearBrowsingDataDialog(cookie.mode);

  const closeMenu = () => {
    setSubmenuOpen(false);
    setOpen(false);
  };

  // 子菜单是主菜单里的 fixed 浮层(role="menu",原生页面遮挡登记会把它算进去),
  // 每次渲染按主菜单与触发项的实际位置重新摆放。
  useLayoutEffect(() => {
    if (!submenuOpen) return;
    const trigger = submenuTriggerRef.current;
    const submenu = submenuRef.current;
    const menu = trigger?.closest('.ace-anchored-menu');
    if (!trigger || !submenu || !menu) return;
    const submenuRect = submenu.getBoundingClientRect();
    const position = submenuPosition({
      menuRect: menu.getBoundingClientRect(),
      itemRect: trigger.getBoundingClientRect(),
      submenuWidth: submenuRect.width,
      submenuHeight: submenuRect.height,
      viewportWidth: document.documentElement.clientWidth,
      viewportHeight: window.innerHeight,
    });
    Object.assign(submenu.style, {
      left: `${position.left}px`,
      top: `${position.top}px`,
      visibility: 'visible',
    });
  });

  useLayoutEffect(() => {
    if (!submenuOpen) return undefined;
    notifyNativeSurfaceOverlayChange();
    return () => notifyNativeSurfaceOverlayChange();
  }, [submenuOpen]);

  const chooseMode = async (mode) => {
    closeMenu();
    if (mode === cookie.mode) return;
    const option = agentBrowserCookieModeOption(mode);
    const result = await setAgentBrowserCookieMode(mode);
    if (result?.ok !== true) {
      toast({ kind: 'err', text: result?.error || '无法保存 Cookie 设置' });
      return;
    }
    toast({
      kind: 'ok',
      text: mode === 'until_quit'
        ? `Cookie 已改为「${option.label}」,已保存的 Cookie 已清除`
        : `Cookie 已改为「${option.label}」`,
    });
  };

  const addConsoleLogs = () => {
    closeMenu();
    onAddConsoleLogs?.();
  };

  const confirmClear = async () => {
    setClearing(true);
    const finished = waitForAgentBrowserStorageEvent(
      (detail) => detail.type === 'browsing_data_cleared' && detail.page_id === pageId,
    );
    const started = await clearAgentBrowserBrowsingData(pageId);
    const result = started?.ok === true
      ? await finished
      : { ok: false, error: started?.error };
    setClearing(false);
    setConfirmOpen(false);
    toast(result?.ok === true
      ? { kind: 'ok', text: '浏览数据已清除' }
      : { kind: 'err', text: result?.error || '无法清除浏览数据' });
  };

  return (
    <>
      <button
        ref={anchorRef}
        type="button"
        className={clsx('ace-agent-browser-toolbar-button', open && 'is-checked')}
        aria-haspopup="menu"
        aria-expanded={open ? 'true' : 'false'}
        title="更多"
        aria-label="更多"
        onClick={() => {
          setSubmenuOpen(false);
          setOpen((value) => !value);
        }}
      >
        <VsIcon name="ellipsisVertical" size={17} />
      </button>
      {open && (
        <AnchoredMenu
          anchorRef={anchorRef}
          onClose={closeMenu}
          width={240}
          role="menu"
          aria-label="浏览器更多操作"
          className="z-50 rounded-lg border border-border bg-surface p-1 ace-shadow-lg"
        >
          <button
            type="button"
            role="menuitem"
            disabled={!ready}
            className={MENU_ITEM_CLASS}
            onMouseEnter={() => setSubmenuOpen(false)}
            onClick={addConsoleLogs}
          >
            <span className="w-5 shrink-0 flex items-center justify-center">
              <VsIcon name="Output" size={16} />
            </span>
            <span className="flex-1 min-w-0 truncate">将控制台日志添加到聊天</span>
          </button>
          {cookie.supported && (
            <>
              <div role="separator" className="h-px bg-border mx-1 my-1" />
              <div className="px-2 pt-1 pb-0.5 text-[11.5px] text-fg-mute select-none">Cookie</div>
              <button
                ref={submenuTriggerRef}
                type="button"
                role="menuitem"
                aria-haspopup="menu"
                aria-expanded={submenuOpen ? 'true' : 'false'}
                disabled={!!cookie.pending}
                className={clsx(MENU_ITEM_CLASS, submenuOpen && 'bg-surface-hi text-fg')}
                onMouseEnter={() => setSubmenuOpen(true)}
                onClick={() => setSubmenuOpen((value) => !value)}
                onKeyDown={(event) => {
                  if (event.key === 'ArrowRight' || event.key === 'ArrowLeft') {
                    event.preventDefault();
                    setSubmenuOpen(true);
                    requestAnimationFrame(() => {
                      submenuRef.current?.querySelector('[aria-checked="true"]')?.focus();
                    });
                  }
                }}
              >
                <span className="flex-1 min-w-0 truncate pl-1">保留 Cookie</span>
                <span className="shrink-0 text-[12px] text-fg-mute">{currentOption.label}</span>
                <VsIcon name="expandRight" size={14} className="shrink-0 text-fg-mute" />
              </button>
              <button
                type="button"
                role="menuitem"
                disabled={!ready}
                className={clsx(MENU_ITEM_CLASS, 'text-danger hover:text-danger')}
                onMouseEnter={() => setSubmenuOpen(false)}
                onClick={() => {
                  closeMenu();
                  setConfirmOpen(true);
                }}
              >
                <span className="flex-1 min-w-0 truncate pl-1">清除浏览数据</span>
              </button>
              {submenuOpen && (
                <div
                  ref={submenuRef}
                  role="menu"
                  aria-label="保留 Cookie"
                  className="fixed z-50 w-[280px] rounded-lg border border-border bg-surface p-1 ace-shadow-lg"
                  style={{ left: 0, top: 0, visibility: 'hidden' }}
                  onKeyDown={(event) => {
                    if (event.key === 'ArrowLeft' || event.key === 'ArrowRight') {
                      event.preventDefault();
                      setSubmenuOpen(false);
                      submenuTriggerRef.current?.focus();
                    }
                  }}
                >
                  {AGENT_BROWSER_COOKIE_MODES.map((option) => {
                    const checked = option.value === currentMode;
                    return (
                      <button
                        key={option.value}
                        type="button"
                        role="menuitemradio"
                        aria-checked={checked ? 'true' : 'false'}
                        className="w-full px-2 py-1.5 rounded-md flex items-start gap-2 text-left text-fg-2 hover:bg-surface-hi hover:text-fg transition"
                        onClick={() => void chooseMode(option.value)}
                      >
                        <span className="flex-1 min-w-0">
                          <span className="block text-[13px] text-fg">{option.label}</span>
                          <span className="block text-[12px] text-fg-mute">{option.hint}</span>
                        </span>
                        <span className="w-4 shrink-0 pt-0.5 flex justify-center text-accent">
                          {checked && <VsIcon name="check" size={14} />}
                        </span>
                      </button>
                    );
                  })}
                </div>
              )}
            </>
          )}
        </AnchoredMenu>
      )}
      {confirmOpen && (
        <Modal
          onClose={clearing ? undefined : () => setConfirmOpen(false)}
          width={420}
          dismissOnBackdrop={!clearing}
          dismissOnEscape={!clearing}
          layerClassName="z-[400]"
          labelledBy="agent-browser-clear-title"
        >
          <div className="p-4">
            <div id="agent-browser-clear-title" className="text-[14px] font-semibold mb-2">
              {dialog.title}
            </div>
            <div className="text-[12.5px] text-fg-mute leading-relaxed mb-4">{dialog.body}</div>
            <div className="flex justify-end gap-2">
              <button
                type="button"
                disabled={clearing}
                onClick={() => setConfirmOpen(false)}
                className="px-3 py-1.5 text-[12.5px] rounded-lg border border-border hover:bg-surface-hi transition-colors disabled:opacity-50"
              >
                {dialog.cancelLabel}
              </button>
              <button
                type="button"
                data-ace-dialog-primary="true"
                disabled={clearing}
                onClick={() => void confirmClear()}
                className="px-3 py-1.5 text-[12.5px] rounded-lg border border-danger/40 bg-danger-bg text-danger hover:opacity-80 disabled:opacity-50"
              >
                {clearing ? '正在清除…' : dialog.confirmLabel}
              </button>
            </div>
          </div>
        </Modal>
      )}
    </>
  );
}
