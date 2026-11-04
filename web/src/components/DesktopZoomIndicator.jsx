import { useSyncExternalStore } from 'react';
import {
  desktopZoomController,
  desktopZoomPercent,
  isDefaultDesktopZoom,
} from '../lib/desktopZoom.js';
import '../styles/desktop-zoom.css';

// Ctrl+滚轮缩放时弹出的比例浮层;停止滚动 1.5 秒后消失,悬停或聚焦时保持。
export function DesktopZoomIndicator() {
  const controller = desktopZoomController();
  const { factor, visible } = useSyncExternalStore(
    controller.subscribe,
    controller.getSnapshot,
    controller.getSnapshot,
  );
  if (!visible) return null;

  return (
    <div
      className="ace-desktop-zoom"
      data-ace-native-overlay="overlap"
      onPointerEnter={() => controller.setPinned(true)}
      onPointerLeave={() => controller.setPinned(false)}
      onFocus={() => controller.setPinned(true)}
      onBlur={() => controller.setPinned(false)}
    >
      <span className="ace-desktop-zoom-value" aria-live="polite">
        {desktopZoomPercent(factor)}
      </span>
      <span className="ace-desktop-zoom-divider" aria-hidden="true" />
      <button
        type="button"
        className="ace-desktop-zoom-reset"
        onClick={() => controller.reset()}
        disabled={isDefaultDesktopZoom(factor)}
      >重置</button>
    </div>
  );
}
