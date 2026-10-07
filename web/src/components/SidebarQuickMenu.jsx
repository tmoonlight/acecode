import { Fragment, useRef, useState } from 'react';
import {
  TOPBAR_QUICK_ACTIONS,
  invokeTopBarQuickAction,
  topBarQuickActionNeedsSeparator,
  topBarQuickActionShortcutLabel,
  topBarQuickActionsMenuWidth,
} from '../lib/topBarQuickActions.js';
import { AnchoredMenu } from './AnchoredMenu.jsx';
import { VsIcon } from './Icon.jsx';

export function SidebarQuickMenu({
  sidebarWidth,
  updateChecking = false,
  onBeforeOpen,
  onNewSession,
  onOpenLoop,
  onOpenSearch,
  onSettings,
  office,
  onAppearance,
  onAbout,
  onCheckUpdates,
  onExit,
  ...buttonProps
}) {
  const [open, setOpen] = useState(false);
  const anchorRef = useRef(null);
  const selectAction = (actionId) => {
    setOpen(false);
    invokeTopBarQuickAction(actionId, {
      onNewSession, onOpenLoop, onOpenSearch, onSettings, onAppearance,
      onToggleOffice: () => office?.setEnabled(!office.enabled),
      onAbout, onCheckUpdates, onExit,
    });
  };

  return (
    <>
      <button
        {...buttonProps}
        ref={anchorRef}
        type="button"
        title="打开快捷菜单"
        aria-label="ACECode 快捷菜单"
        aria-haspopup="menu"
        aria-expanded={open}
        aria-controls="sidebar-quick-actions-menu"
        onClick={() => {
          if (!open) onBeforeOpen?.();
          setOpen((value) => !value);
        }}
        className="w-6 h-8 shrink-0 flex items-center justify-center rounded-md text-fg-mute hover:text-fg hover:bg-surface-hi transition focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-accent/20"
      >
        <VsIcon name="settings" size={18} />
      </button>
      {open && (
        <AnchoredMenu
          anchorRef={anchorRef}
          onClose={() => setOpen(false)}
          width={topBarQuickActionsMenuWidth(sidebarWidth) - 16}
          id="sidebar-quick-actions-menu"
          role="menu"
          aria-label="快捷操作"
          className="z-50 rounded-lg border border-border bg-surface p-1 ace-shadow-lg"
        >
          {TOPBAR_QUICK_ACTIONS.map((action, index) => {
            if (action.id === 'virtual-office' && !office?.available) return null;
            const checkingUpdates = action.id === 'check-updates' && updateChecking;
            const officeAction = action.id === 'virtual-office';
            const shortcutLabel = topBarQuickActionShortcutLabel(action);
            return (
              <Fragment key={action.id}>
                {topBarQuickActionNeedsSeparator(index) && (
                  <div role="separator" className="h-px bg-border mx-1 my-1" />
                )}
                <button
                  type="button"
                  role="menuitem"
                  onClick={() => selectAction(action.id)}
                  disabled={checkingUpdates || (officeAction && office.busy)}
                  aria-busy={checkingUpdates || undefined}
                  className="w-full h-8 px-2 rounded-md flex items-center gap-2 text-[13px] text-fg-2 hover:bg-surface-hi hover:text-fg transition text-left disabled:opacity-50 disabled:cursor-wait"
                >
                  <span className="w-5 shrink-0 flex items-center justify-center">
                    <VsIcon name={action.icon} size={18} className={checkingUpdates ? 'animate-spin' : ''} />
                  </span>
                  <span className="flex-1 min-w-0 truncate">{officeAction && office.enabled ? '隐藏虚拟办公室' : action.label}</span>
                  {shortcutLabel && (
                    <kbd
                      aria-label={shortcutLabel}
                      className="shrink-0 font-sans text-[11px] leading-none text-fg-mute"
                    >
                      {shortcutLabel}
                    </kbd>
                  )}
                </button>
              </Fragment>
            );
          })}
        </AnchoredMenu>
      )}
    </>
  );
}
