// 共享 modal 容器:遮罩 + 居中 + esc 关闭。效率工具中的弹窗立即出现/关闭,
// 不做进入/退出动画,也不延迟 onClose。
// 不依赖 bootstrap modal,纯 Tailwind + 内联状态。

import { useEffect, useLayoutEffect, useRef, useState } from 'react';
import { clsx } from '../lib/format.js';
import { notifyNativeSurfaceOverlayChange } from '../lib/agentBrowserSurfaceCoordinator.js';

export function Modal({
  children,
  onClose,
  width = 460,
  dismissOnBackdrop = true,
  dismissOnEscape = true,
  layerClassName = 'z-[200]',
  labelledBy,
}) {
  const dialogRef = useRef(null);
  const closeRef = useRef(onClose);
  const dismissOnEscapeRef = useRef(dismissOnEscape);
  closeRef.current = onClose;
  dismissOnEscapeRef.current = dismissOnEscape;

  useLayoutEffect(() => {
    notifyNativeSurfaceOverlayChange();
    const previouslyFocused = document.activeElement;
    const focusableSelector = [
      'button:not([disabled])',
      '[href]',
      'input:not([disabled])',
      'select:not([disabled])',
      'textarea:not([disabled])',
      '[tabindex]:not([tabindex="-1"])',
    ].join(',');
    const focusFirst = () => {
      const dialog = dialogRef.current;
      if (!dialog) return;
      const target = dialog.querySelector('[autofocus]') || dialog.querySelector(focusableSelector);
      (target || dialog).focus?.();
    };
    focusFirst();
    const onKey = (event) => {
      const dialog = dialogRef.current;
      const modalDialogs = [...document.querySelectorAll('[data-ace-modal-dialog="true"]')];
      if (!dialog || modalDialogs[modalDialogs.length - 1] !== dialog) return;
      if (event.key === 'Escape' && dismissOnEscapeRef.current) {
        event.stopImmediatePropagation();
        closeRef.current?.();
        return;
      }
      if (event.key !== 'Tab') return;
      const focusable = [...dialog.querySelectorAll(focusableSelector)]
        .filter((element) => !element.hidden && element.getAttribute('aria-hidden') !== 'true');
      if (focusable.length === 0) {
        event.preventDefault();
        dialog.focus();
        return;
      }
      const first = focusable[0];
      const last = focusable[focusable.length - 1];
      if (event.shiftKey && document.activeElement === first) {
        event.preventDefault();
        last.focus();
      } else if (!event.shiftKey && document.activeElement === last) {
        event.preventDefault();
        first.focus();
      }
    };
    document.addEventListener('keydown', onKey);
    return () => {
      document.removeEventListener('keydown', onKey);
      previouslyFocused?.focus?.();
      notifyNativeSurfaceOverlayChange();
    };
  }, []);

  const handleClose = () => onClose?.();

  return (
    <div
      data-ace-native-overlay="blocking"
      className={clsx('fixed inset-0 flex items-center justify-center p-4', layerClassName)}
      style={{ backgroundColor: 'rgba(0, 0, 0, 0.35)' }}
      onClick={() => dismissOnBackdrop && handleClose()}
    >
      <div
        ref={dialogRef}
        data-ace-modal-dialog="true"
        role="dialog"
        aria-modal="true"
        aria-labelledby={labelledBy}
        tabIndex={-1}
        className="bg-surface border border-border rounded-xl ace-shadow-lg overflow-hidden"
        style={{ width }}
        onClick={(e) => e.stopPropagation()}
      >
        {typeof children === 'function' ? children({ close: handleClose }) : children}
      </div>
    </div>
  );
}

// 右侧滑出面板(MCPPanel 用)
export function SlideOver({ children, onClose, width = 380 }) {
  const [show, setShow] = useState(false);

  useEffect(() => {
    requestAnimationFrame(() => setShow(true));
    const onKey = (e) => { if (e.key === 'Escape') handleClose(); };
    document.addEventListener('keydown', onKey);
    return () => document.removeEventListener('keydown', onKey);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  useLayoutEffect(() => {
    notifyNativeSurfaceOverlayChange();
    return () => notifyNativeSurfaceOverlayChange();
  }, []);

  const handleClose = () => {
    setShow(false);
    setTimeout(() => onClose?.(), 240);
  };

  return (
    <div
      data-ace-native-overlay="blocking"
      className="fixed inset-0 z-[250] transition-colors duration-250"
      style={{ backgroundColor: show ? 'rgba(0, 0, 0, 0.25)' : 'rgba(0, 0, 0, 0)' }}
      onClick={handleClose}
    >
      <div
        className={clsx(
          'absolute top-0 right-0 bottom-0 bg-surface border-l border-border ace-shadow-lg flex flex-col transition-transform duration-250 ease-out',
          show ? 'translate-x-0' : 'translate-x-full',
        )}
        style={{ width }}
        onClick={(e) => e.stopPropagation()}
      >
        {typeof children === 'function' ? children({ close: handleClose }) : children}
      </div>
    </div>
  );
}

// Toggle switch — Modal/Panels 复用
export function Toggle({ on, onChange, disabled, ariaLabel }) {
  return (
    <button
      type="button"
      role="switch"
      aria-checked={on}
      aria-label={ariaLabel}
      disabled={disabled}
      onClick={() => onChange?.(!on)}
      className={clsx(
        'w-9 h-5 rounded-full relative transition-colors shrink-0 disabled:opacity-50',
        on ? 'bg-accent border border-accent' : 'bg-surface-hi border border-border',
      )}
    >
      <span
        className={clsx(
          'absolute top-px left-px w-[15px] h-[15px] rounded-full bg-white shadow transition-transform',
          on ? 'translate-x-[16px]' : 'translate-x-0',
        )}
      />
    </button>
  );
}
