import { useEffect, useLayoutEffect, useRef, useState } from 'react';
import { useTranslation } from 'react-i18next';
import { sessionContentLoadingAnchorFrame } from '../lib/sessionContentLoading.js';

const DEFAULT_REVEAL_DELAY_MS = 160;

export function SessionContentLoading({
  phase = '',
  title = '',
  revealDelayMs = DEFAULT_REVEAL_DELAY_MS,
  anchorSelector = '',
}) {
  const { t } = useTranslation();
  const overlayRef = useRef(null);
  const [noticeVisible, setNoticeVisible] = useState(phase === 'error');
  const [anchorFrame, setAnchorFrame] = useState(null);

  useLayoutEffect(() => {
    const overlay = overlayRef.current;
    if (!phase || !anchorSelector || !overlay) {
      setAnchorFrame(null);
      return undefined;
    }
    const anchor = overlay.parentElement?.querySelector(anchorSelector);
    if (!anchor) {
      setAnchorFrame(null);
      return undefined;
    }

    const update = () => {
      const next = sessionContentLoadingAnchorFrame(
        overlay.getBoundingClientRect(),
        anchor.getBoundingClientRect(),
      );
      setAnchorFrame((current) => (
        current?.left === next?.left
        && current?.top === next?.top
        && current?.width === next?.width
        && current?.height === next?.height
          ? current
          : next
      ));
    };
    update();
    const resizeObserver = typeof window.ResizeObserver === 'function'
      ? new window.ResizeObserver(update)
      : null;
    resizeObserver?.observe(overlay);
    resizeObserver?.observe(anchor);
    window.addEventListener('resize', update);
    return () => {
      resizeObserver?.disconnect();
      window.removeEventListener('resize', update);
    };
  }, [anchorSelector, phase]);

  useEffect(() => {
    if (!phase) {
      setNoticeVisible(false);
      return undefined;
    }
    overlayRef.current?.focus({ preventScroll: true });
    if (phase === 'error' || revealDelayMs <= 0) {
      setNoticeVisible(true);
      return undefined;
    }
    setNoticeVisible(false);
    const timer = window.setTimeout(() => setNoticeVisible(true), revealDelayMs);
    return () => window.clearTimeout(timer);
  }, [phase, revealDelayMs]);

  if (!phase) return null;

  const displayTitle = title || t('sessionNavigation.conversation');
  const label = phase === 'queued'
    ? t('sessionNavigation.sidebarQueued', { title: displayTitle })
    : phase === 'error'
      ? t('sessionNavigation.transcriptError')
        : phase === 'transcript'
          ? t('sessionNavigation.transcriptLoading')
          : t('sessionNavigation.sidebarLoading', { title: displayTitle });
  const noticeStyle = anchorFrame
    ? {
        position: 'absolute',
        left: anchorFrame.left,
        top: anchorFrame.top,
        width: Math.min(420, Math.max(192, anchorFrame.width * 0.8)),
        transform: 'translate(-50%, -50%)',
      }
    : { width: '80%', minWidth: 192, maxWidth: 420 };

  return (
    <div
      ref={overlayRef}
      className="absolute inset-0 z-[80] flex items-center justify-center outline-none"
      style={{
        background: 'rgba(var(--ace-bg-rgb), 0.76)',
        backdropFilter: 'blur(2px)',
        WebkitBackdropFilter: 'blur(2px)',
      }}
      role="status"
      aria-live="polite"
      aria-busy={phase === 'error' ? undefined : 'true'}
      aria-label={label}
      data-ace-native-overlay="blocking"
      data-session-content-loading={phase}
      tabIndex={-1}
      onPointerDown={(event) => event.preventDefault()}
    >
      {noticeVisible && (
        <div
          className="flex flex-col items-center gap-2.5 rounded-xl border border-border bg-surface px-6 py-5 text-center ace-shadow-lg"
          style={noticeStyle}
        >
          {phase !== 'error' && (
            <span className="ace-spinner text-[24px]" aria-hidden="true" />
          )}
          <span className="text-[13px] font-medium text-fg">{label}</span>
          {phase === 'queued' && (
            <span className="text-[11px] leading-5 text-fg-mute">
              {t('sessionNavigation.sidebarQueueHint')}
            </span>
          )}
        </div>
      )}
    </div>
  );
}
