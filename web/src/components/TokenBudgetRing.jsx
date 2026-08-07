import {
  useCallback,
  useEffect,
  useId,
  useRef,
  useState,
} from 'react';
import { createPortal } from 'react-dom';
import { clsx } from '../lib/format.js';
import { computeTokenBudgetPanelLayout } from '../lib/tokenBudgetPanelLayout.js';
import { resolveTokenBudgetPanelState } from '../lib/tokenBudgetPanelState.js';
import { VsIcon } from './Icon.jsx';

const RING_COLOR = {
  safe: 'var(--ace-accent)',
  warning: 'var(--ace-warn)',
  danger: 'var(--ace-danger)',
  unknown: 'var(--ace-fg-mute)',
};

const HOVER_CLOSE_DELAY_MS = 140;
const FULL_CONTEXT_PANEL_WIDTH_PX = 420;
const AGGREGATE_CONTEXT_PANEL_WIDTH_PX = 240;

function panelGeometry(anchor, pointer = null, preferredWidth = FULL_CONTEXT_PANEL_WIDTH_PX) {
  const host = anchor.closest('.ace-composer-card') || anchor;
  const hostRect = host.getBoundingClientRect();
  const anchorRect = anchor.getBoundingClientRect();
  const viewportWidth = window.innerWidth || document.documentElement.clientWidth || 0;
  const viewportHeight = window.innerHeight || document.documentElement.clientHeight || 0;
  const pointerX = Number.isFinite(pointer?.x)
    ? pointer.x
    : anchorRect.left + (anchorRect.width / 2);
  const pointerY = Number.isFinite(pointer?.y)
    ? pointer.y
    : anchorRect.top + (anchorRect.height / 2);

  return computeTokenBudgetPanelLayout({
    panelWidth: Math.min(hostRect.width, preferredWidth),
    pointerX,
    pointerY,
    viewportWidth,
    viewportHeight,
  });
}

export function TokenBudgetRing({ budget, className = '' }) {
  const anchorRef = useRef(null);
  const panelRef = useRef(null);
  const closeTimerRef = useRef(null);
  const panelId = useId();
  const titleId = useId();
  const [panel, setPanel] = useState(null);

  const radius = 6;
  const circumference = 2 * Math.PI * radius;
  const known = !!budget?.known;
  const ratio = Math.min(1, Math.max(0, Number(budget?.usedRatio) || 0));
  const dashOffset = circumference * (1 - ratio);
  const color = RING_COLOR[budget?.severity] || RING_COLOR.unknown;
  const label = budget?.ariaLabel || budget?.title || 'Token usage unavailable';
  const aggregateOnly = known && !budget?.breakdownKnown;

  const clearCloseTimer = useCallback(() => {
    if (closeTimerRef.current !== null) {
      window.clearTimeout(closeTimerRef.current);
      closeTimerRef.current = null;
    }
  }, []);

  const closePanel = useCallback(() => {
    clearCloseTimer();
    setPanel(null);
  }, [clearCloseTimer]);

  const showPanel = useCallback((mode = 'hover', pointer = null) => {
    const anchor = anchorRef.current;
    if (!anchor) return;
    clearCloseTimer();
    const geometry = panelGeometry(
      anchor,
      pointer,
      aggregateOnly
        ? AGGREGATE_CONTEXT_PANEL_WIDTH_PX
        : FULL_CONTEXT_PANEL_WIDTH_PX,
    );
    setPanel((current) => resolveTokenBudgetPanelState(current, geometry, mode));
  }, [aggregateOnly, clearCloseTimer]);

  const scheduleHoverClose = useCallback(() => {
    clearCloseTimer();
    closeTimerRef.current = window.setTimeout(() => {
      closeTimerRef.current = null;
      setPanel((current) => (current?.mode === 'click' ? current : null));
    }, HOVER_CLOSE_DELAY_MS);
  }, [clearCloseTimer]);

  useEffect(() => () => clearCloseTimer(), [clearCloseTimer]);

  useEffect(() => {
    if (!panel) return undefined;

    const onPointerDown = (event) => {
      if (anchorRef.current?.contains(event.target)) return;
      if (panelRef.current?.contains(event.target)) return;
      closePanel();
    };
    const onKeyDown = (event) => {
      if (event.key === 'Escape') closePanel();
    };
    const onViewportChange = () => closePanel();

    document.addEventListener('pointerdown', onPointerDown);
    document.addEventListener('keydown', onKeyDown);
    window.addEventListener('resize', onViewportChange);
    document.addEventListener('scroll', onViewportChange, true);
    return () => {
      document.removeEventListener('pointerdown', onPointerDown);
      document.removeEventListener('keydown', onKeyDown);
      window.removeEventListener('resize', onViewportChange);
      document.removeEventListener('scroll', onViewportChange, true);
    };
  }, [closePanel, panel]);

  if (!budget) return null;

  return (
    <button
      ref={anchorRef}
      type="button"
      className={clsx('ace-token-budget-button inline-flex items-center justify-center shrink-0', className)}
      aria-label={label}
      aria-haspopup="dialog"
      aria-expanded={!!panel}
      aria-controls={panel ? panelId : undefined}
      onMouseEnter={(event) => showPanel('hover', {
        x: event.clientX,
        y: event.clientY,
      })}
      onMouseLeave={scheduleHoverClose}
      onFocus={() => showPanel('hover')}
      onBlur={scheduleHoverClose}
      onClick={(event) => {
        event.stopPropagation();
        showPanel(
          'click',
          event.detail > 0
            ? { x: event.clientX, y: event.clientY }
            : null,
        );
      }}
      data-panel-mode={panel?.mode || undefined}
      data-token-budget-severity={budget?.severity || 'unknown'}
    >
      <svg width="16" height="16" viewBox="0 0 16 16" aria-hidden="true" className="block">
        <circle
          cx="8"
          cy="8"
          r={radius}
          fill="none"
          stroke="rgba(var(--ace-fg-mute-rgb), 0.28)"
          strokeWidth="2"
        />
        {known ? (
          <circle
            cx="8"
            cy="8"
            r={radius}
            fill="none"
            stroke={color}
            strokeWidth="2"
            strokeLinecap="round"
            strokeDasharray={`${circumference} ${circumference}`}
            strokeDashoffset={dashOffset}
            transform="rotate(-90 8 8)"
          />
        ) : (
          <circle
            cx="8"
            cy="8"
            r={radius}
            fill="none"
            stroke={color}
            strokeWidth="1.6"
            strokeLinecap="round"
            strokeDasharray="1.5 2.4"
            opacity="0.85"
          />
        )}
      </svg>
      {panel
        ? createPortal(
            <div
              ref={panelRef}
              id={panelId}
              role="dialog"
              aria-labelledby={aggregateOnly ? undefined : titleId}
              aria-label={aggregateOnly ? label : undefined}
              className={clsx(
                'ace-context-usage-panel',
                aggregateOnly && 'ace-context-usage-panel-aggregate',
              )}
              data-ace-native-overlay="overlap"
              data-placement={panel.placement}
              data-mode={panel.mode}
              data-severity={budget?.severity || 'unknown'}
              style={{
                left: panel.left,
                width: panel.width,
                maxHeight: panel.maxHeight,
                top: panel.placement === 'below' ? panel.top : undefined,
                bottom: panel.placement === 'above' ? panel.bottom : undefined,
              }}
              onMouseEnter={clearCloseTimer}
              onMouseLeave={scheduleHoverClose}
              onFocusCapture={clearCloseTimer}
              onBlurCapture={scheduleHoverClose}
              onClick={(event) => event.stopPropagation()}
            >
              {aggregateOnly ? (
                <div
                  className="ace-context-usage-track ace-context-usage-track-aggregate"
                  role="img"
                  aria-label={`${budget.percent}% 已用`}
                >
                  <span
                    className="ace-context-usage-segment"
                    data-context-category="aggregate"
                    style={{ width: `${ratio * 100}%` }}
                  />
                </div>
              ) : (
                <>
                  <div className="ace-context-usage-header">
                    <div id={titleId} className="ace-context-usage-title">上下文用量</div>
                    <button
                      type="button"
                      className="ace-context-usage-close"
                      aria-label="关闭上下文用量"
                      onClick={closePanel}
                    >
                      <VsIcon name="close" size={13} />
                    </button>
                  </div>

                  {known ? (
                    <>
                      <div className="ace-context-usage-summary">
                        <span>{budget.percent}% 已用</span>
                        <span>
                          {budget.compactUsedTokens} / {budget.compactLimitTokens} Tokens
                        </span>
                      </div>
                      <div
                        className="ace-context-usage-track"
                        role="img"
                        aria-label={`${budget.percent}% 已用`}
                      >
                        {budget.categories.map((category) => (
                          <span
                            key={category.key}
                            className="ace-context-usage-segment"
                            data-context-category={category.tone}
                            style={{
                              width: `${Math.min(1, Math.max(0, category.windowShare)) * 100}%`,
                            }}
                          />
                        ))}
                      </div>

                      <div className="ace-context-usage-rows">
                        {budget.categories.map((category) => (
                          <div
                            key={category.key}
                            className="ace-context-usage-row"
                            data-context-category={category.tone}
                          >
                            <span className="ace-context-usage-swatch" aria-hidden="true" />
                          <span className="ace-context-usage-label">{category.label}</span>
                          <span className="ace-context-usage-value">
                            {category.compactTokens}
                          </span>
                          </div>
                        ))}
                      </div>

                      {budget.cacheHitPercent !== null && (
                        <div
                          className="ace-context-usage-cache"
                          title={budget.cacheTitle}
                        >
                          <span className="ace-context-usage-label">提示词缓存命中率</span>
                          <span className="ace-context-usage-value">
                            {budget.cacheHitPercent}%
                          </span>
                        </div>
                      )}
                    </>
                  ) : (
                    <div className="ace-context-usage-unavailable">
                      {budget.title || '尚未收到 token 用量数据'}
                    </div>
                  )}
                </>
              )}
            </div>,
            document.body,
          )
        : null}
    </button>
  );
}
