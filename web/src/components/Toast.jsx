// 极简 toast:模块级订阅,任何地方 import { toast } 触发,Toaster 是渲染容器。
// 不引入 react-hot-toast 等依赖。

import { useEffect, useLayoutEffect, useRef, useState } from 'react';
import { createPortal } from 'react-dom';
import { clsx } from '../lib/format.js';
import { notifyNativeSurfaceOverlayChange } from '../lib/agentBrowserSurfaceCoordinator.js';
import { tr } from '../i18n/index.js';
import { VsIcon } from './Icon.jsx';

const listeners = new Set();
let nextId = 1;

export function toast({ kind = 'info', text, duration = 3500, actions = [], icon, dismissible = false }) {
  const id = nextId++;
  const item = { id, kind, text, duration, actions, icon, dismissible };
  for (const fn of listeners) fn({ type: 'add', item });
  if (duration > 0 && actions.length === 0) setTimeout(() => dismissToast(id), duration);
  return id;
}

function dismissToast(id) {
  for (const fn of listeners) fn({ type: 'remove', id });
}

function ToastItem({ item: it }) {
  const [hovered, setHovered] = useState(false);
  const [focused, setFocused] = useState(false);
  const [busy, setBusy] = useState(false);
  const runningRef = useRef(false);
  const hasActions = it.actions.length > 0;

  useEffect(() => {
    if (!hasActions || it.duration <= 0 || hovered || focused || busy) return undefined;
    const timer = setTimeout(() => dismissToast(it.id), it.duration);
    return () => clearTimeout(timer);
  }, [hasActions, it.id, it.duration, hovered, focused, busy]);

  const runAction = async (action) => {
    if (runningRef.current) return;
    runningRef.current = true;
    setBusy(true);
    try {
      await action.onClick();
      dismissToast(it.id);
    } catch (error) {
      toast({ kind: 'err', text: `${action.errorText || ''}${error?.message || error}` });
    } finally {
      runningRef.current = false;
      setBusy(false);
    }
  };

  return (
    <div
      className="pointer-events-auto flex min-w-0 items-start gap-2.5 rounded-lg border border-border bg-surface px-4 py-3 font-sans text-[13px] font-medium text-fg ace-shadow-lg animate-[ace-pulse_.18s_ease-out] motion-reduce:animate-none"
      role="status"
      aria-busy={busy || undefined}
      onMouseEnter={() => setHovered(true)}
      onMouseLeave={() => setHovered(false)}
      onFocusCapture={() => setFocused(true)}
      onBlurCapture={(event) => { if (!event.currentTarget.contains(event.relatedTarget)) setFocused(false); }}
    >
      <span
        className={clsx(
          'flex h-5 w-5 shrink-0 items-center justify-center rounded-full',
          it.icon ? 'text-fg' : it.kind === 'ok' ? 'bg-ok text-white' : it.kind === 'err' ? 'bg-danger text-white' : 'text-fg-mute',
        )}
        aria-hidden="true"
      >
        <VsIcon
          name={it.icon || (it.kind === 'ok' ? 'check' : it.kind === 'err' ? 'close' : 'info')}
          size={!it.icon && (it.kind === 'ok' || it.kind === 'err') ? 12 : 20}
          className={!it.icon && (it.kind === 'ok' || it.kind === 'err') ? 'brightness-0 invert' : ''}
        />
      </span>
      <span className="min-w-0 flex-1 whitespace-pre-wrap leading-5 [overflow-wrap:anywhere]">{it.text}</span>
      {(hasActions || it.dismissible) && (
        <span className="-my-1 ml-2 flex shrink-0 items-center gap-2">
          {it.actions.map((action) => (
            <button
              key={action.label}
              type="button"
              disabled={busy}
              onClick={() => runAction(action)}
              className={clsx(
                'h-7 rounded-md px-2 text-[13px] font-normal leading-5 transition-colors focus-visible:outline focus-visible:outline-2 focus-visible:outline-offset-2 focus-visible:outline-accent disabled:cursor-wait disabled:opacity-50',
                action.primary ? 'bg-accent text-white hover:opacity-90' : 'bg-surface-alt text-fg hover:bg-surface-hi',
              )}
            >{action.label}</button>
          ))}
          {it.dismissible && (
            <button
              type="button"
              aria-label={tr('关闭提示')}
              onClick={() => dismissToast(it.id)}
              className="flex h-7 w-6 shrink-0 items-center justify-center rounded-md text-fg hover:bg-surface-hi focus-visible:outline focus-visible:outline-2 focus-visible:outline-accent"
            >
              <VsIcon name="close" size={18} />
            </button>
          )}
        </span>
      )}
    </div>
  );
}

export function Toaster() {
  const [items, setItems] = useState([]);

  useEffect(() => {
    const handler = (evt) => {
      if (evt.type === 'add')    setItems((prev) => [evt.item, ...prev]);
      if (evt.type === 'remove') setItems((prev) => prev.filter((x) => x.id !== evt.id));
    };
    listeners.add(handler);
    return () => listeners.delete(handler);
  }, []);

  useLayoutEffect(() => {
    notifyNativeSurfaceOverlayChange();
    return () => notifyNativeSurfaceOverlayChange();
  }, [items]);

  // Escape the isolated app shell and keep feedback above global dialogs and menus.
  const toastLayer = (
    <div
      className="pointer-events-none fixed left-1/2 top-6 z-[2147483647] flex w-max max-w-[min(480px,calc(100vw-32px))] -translate-x-1/2 flex-col gap-2"
      data-ace-native-overlay="overlap"
    >
      {items.map((it) => <ToastItem key={it.id} item={it} />)}
    </div>
  );

  return typeof document === 'undefined'
    ? toastLayer
    : createPortal(toastLayer, document.body);
}
