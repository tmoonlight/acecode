import { isDismissShortcut } from '../lib/shortcutBindings.js';
// 共享 modal 容器:遮罩 + 居中 + 键盘约定。效率工具中的弹窗立即出现/关闭,
// 不做进入/退出动画,也不延迟 onClose。
// 不依赖 bootstrap modal,纯 Tailwind + 内联状态。
//
// 键盘约定(全部对话框共用,判定逻辑见 lib/dialogKeyboard.js):
//   - Esc 取消(dismissOnEscape 允许时);
//   - Tab / Shift+Tab 只在对话框内循环;
//   - Enter 触发标了 `data-ace-dialog-primary` 的默认操作按钮(焦点没落在会自己消费
//     Enter 的元素上时);按住不放的 auto-repeat Enter 一律拦掉;
//   - 打开时初始焦点:已有焦点 > 第一个文本输入框 > 默认操作按钮 > 第一个可聚焦元素。
//     删除确认框只要把「删除」标成 primary,打开就默认选中它,Enter 即确认。

import { useEffect, useLayoutEffect, useRef, useState } from 'react';
import { createPortal } from 'react-dom';
import { clsx } from '../lib/format.js';
import { notifyNativeSurfaceOverlayChange } from '../lib/agentBrowserSurfaceCoordinator.js';
import {
  dialogEnterAction,
  resolveDialogInitialFocus,
  resolveDialogTabTarget,
} from '../lib/dialogKeyboard.js';

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
    const focusInitial = () => {
      const dialog = dialogRef.current;
      if (!dialog) return;
      // 子组件的 React autoFocus 在 commit 阶段已经先跑过,这里尊重它,不再抢焦点。
      const target = resolveDialogInitialFocus(dialog, document.activeElement);
      if (target && target !== document.activeElement) target.focus?.();
    };
    focusInitial();
    const onKey = (event) => {
      const dialog = dialogRef.current;
      const modalDialogs = [...document.querySelectorAll('[data-ace-modal-dialog="true"]')];
      if (!dialog || modalDialogs[modalDialogs.length - 1] !== dialog) return;
      if (isDismissShortcut(event)) {
        // 输入法合成中的 Esc 只是取消候选词,不能把对话框一起关掉。
        if (event.isComposing || !dismissOnEscapeRef.current) return;
        event.stopImmediatePropagation();
        closeRef.current?.();
        return;
      }
      if (event.key === 'Enter') {
        const action = dialogEnterAction(event, dialog);
        if (action.type === 'block') {
          event.preventDefault();
        } else if (action.type === 'activate') {
          event.preventDefault();
          action.element.click();
        }
        return;
      }
      if (event.key !== 'Tab') return;
      const target = resolveDialogTabTarget(dialog, document.activeElement, event.shiftKey);
      if (!target) return;
      event.preventDefault();
      target.focus?.();
    };
    document.addEventListener('keydown', onKey);
    return () => {
      document.removeEventListener('keydown', onKey);
      previouslyFocused?.focus?.();
      notifyNativeSurfaceOverlayChange();
    };
  }, []);

  const handleClose = () => onClose?.();

  const modalLayer = (
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
        className="ace-modal-dialog ace-scrollbar bg-surface border border-border rounded-xl ace-shadow-lg"
        style={{ width }}
        onClick={(e) => e.stopPropagation()}
      >
        {typeof children === 'function' ? children({ close: handleClose }) : children}
      </div>
    </div>
  );

  return typeof document === 'undefined'
    ? modalLayer
    : createPortal(modalLayer, document.body);
}

// 右侧滑出面板(MCPPanel 用)
export function SlideOver({ children, onClose, width = 380 }) {
  const [show, setShow] = useState(false);

  useEffect(() => {
    requestAnimationFrame(() => setShow(true));
    const onKey = (e) => { if (isDismissShortcut(e)) handleClose(); };
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
