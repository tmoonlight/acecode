// WindowControls:frameless desktop 模式下的标题栏窗口控制三连
// (minimize / maximize ↔ restore / close)。

import { VsIcon } from './Icon.jsx';
import { useEffect, useState } from 'react';
import { clsx } from '../lib/format.js';
import { isMacDesktopShell } from '../lib/desktopShellMode.js';

export function isFramelessDesktop() {
  return typeof window !== 'undefined'
    && window.__ACECODE_FRAMELESS_WINDOW__ === true
    && typeof window.aceDesktop_startWindowDrag === 'function';
}

export function isInteractiveTarget(target) {
  return !!target?.closest?.('button,a,input,textarea,select,[contenteditable="true"],[role="button"],[data-ace-no-window-drag="true"]');
}

export function nativePointerEvent(event) {
  return {
    button: (event?.button ?? 0) + 1,
    screenX: event?.screenX ?? 0,
    screenY: event?.screenY ?? 0,
    time: Math.max(0, Math.floor(event?.timeStamp ?? 0)),
  };
}

// App 统一订阅窗口状态，供标题栏和缩放边缘共用。
export function useFramelessWindowState() {
  const framelessDesktop = isFramelessDesktop();
  const [isMaximized, setIsMaximized] = useState(null);
  const [isFullscreen, setIsFullscreen] = useState(null);

  useEffect(() => {
    if (!framelessDesktop) return undefined;
    let cancelled = false;
    const fetchInitial = async () => {
      let state;
      try {
        const raw = await window.aceDesktop_isWindowMaximized?.();
        state = typeof raw === 'string' ? JSON.parse(raw) : raw;
      } catch {
        // 兼容无法提供初始状态的桌面壳，按普通窗口处理。
      }
      if (!cancelled) {
        // 查询期间收到的原生推送比初始快照更新。
        setIsMaximized(current => current ?? !!state?.maximized);
        setIsFullscreen(current => current ?? !!state?.fullscreen);
      }
    };

    const prev = window.aceDesktop_onMaximizeStateChanged;
    const prevFullscreen = window.aceDesktop_onFullscreenStateChanged;
    window.aceDesktop_onMaximizeStateChanged = (m) => {
      setIsMaximized(!!m);
      if (typeof prev === 'function') prev(m);
    };
    window.aceDesktop_onFullscreenStateChanged = (fullscreen) => {
      setIsFullscreen(!!fullscreen);
      if (typeof prevFullscreen === 'function') prevFullscreen(fullscreen);
    };
    fetchInitial();

    return () => {
      cancelled = true;
      window.aceDesktop_onMaximizeStateChanged = prev || undefined;
      window.aceDesktop_onFullscreenStateChanged = prevFullscreen || undefined;
    };
  }, [framelessDesktop]);

  return { framelessDesktop, isMaximized, isFullscreen };
}

const WINDOW_GLYPHS = Object.freeze({
  minimize: 'Minimize',
  maximize: 'Maximize',
  restore: 'Restore',
  close: 'close',
});

function WindowGlyph({ type }) {
  return <VsIcon name={WINDOW_GLYPHS[type] || 'close'} size={14} />;
}

export function WindowControl({ type, title, onClick }) {
  return (
    <button
      type="button"
      data-ace-no-window-drag="true"
      title={title}
      aria-label={title}
      onClick={onClick}
      className={clsx('ace-window-control', type === 'close' && 'ace-window-control-close')}
    >
      <WindowGlyph type={type} />
    </button>
  );
}

// 三连组合 — 用得多就直接用这个,不用三连各自写一遍。
export function WindowControls({ isMaximized }) {
  // macOS keeps AppKit's native traffic lights in the transparent title bar.
  if (isMacDesktopShell()) return null;

  return (
    <div className="ace-window-controls" data-ace-no-window-drag="true">
      <WindowControl type="minimize" title="最小化" onClick={() => window.aceDesktop_minimizeWindow?.()} />
      <WindowControl
        type={isMaximized ? 'restore' : 'maximize'}
        title={isMaximized ? '还原' : '最大化'}
        onClick={() => window.aceDesktop_toggleMaximizeWindow?.()}
      />
      <WindowControl type="close" title="关闭" onClick={() => window.aceDesktop_closeWindow?.()} />
    </div>
  );
}
