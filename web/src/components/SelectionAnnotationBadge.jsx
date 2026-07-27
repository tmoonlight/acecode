import { useCallback, useEffect, useId, useRef, useState } from 'react';
import { createPortal } from 'react-dom';
import { normalizeSelectionAnnotations } from '../lib/selectionChatContext.js';

function tooltipPosition(anchor) {
  const rect = anchor?.getBoundingClientRect?.();
  if (!rect || typeof window === 'undefined') return null;
  const viewportMargin = 12;
  const gap = 7;
  const width = Math.min(280, Math.max(0, window.innerWidth - viewportMargin * 2));
  const left = Math.min(
    Math.max(viewportMargin, rect.right - width),
    Math.max(viewportMargin, window.innerWidth - width - viewportMargin),
  );
  if (rect.top < 112) {
    return {
      placement: 'below',
      left: Math.round(left),
      top: Math.round(rect.bottom + gap),
    };
  }
  return {
    placement: 'above',
    left: Math.round(left),
    bottom: Math.round(window.innerHeight - rect.top + gap),
  };
}

export function SelectionAnnotationBadge({
  number = 0,
  annotations = [],
  compact = false,
}) {
  const items = normalizeSelectionAnnotations(annotations);
  const annotationNumber = Math.max(1, Math.floor(Number(number) || 0));
  const label = `${annotationNumber} 号批注位置，共 ${items.length} 条批注`;
  const tooltipId = useId();
  const anchorRef = useRef(null);
  const [position, setPosition] = useState(null);
  const showTooltip = useCallback(() => {
    setPosition(tooltipPosition(anchorRef.current));
  }, []);
  const hideTooltip = useCallback(() => setPosition(null), []);

  useEffect(() => {
    if (!position) return undefined;
    window.addEventListener('resize', showTooltip);
    window.addEventListener('scroll', showTooltip, true);
    return () => {
      window.removeEventListener('resize', showTooltip);
      window.removeEventListener('scroll', showTooltip, true);
    };
  }, [position, showTooltip]);

  if (items.length === 0) return null;

  return (
    <span
      ref={anchorRef}
      className="ace-selection-annotation-badge"
      data-compact={compact ? 'true' : 'false'}
      data-annotation-number={annotationNumber}
      tabIndex={0}
      aria-label={label}
      aria-describedby={position ? tooltipId : undefined}
      onMouseEnter={showTooltip}
      onMouseLeave={hideTooltip}
      onFocus={showTooltip}
      onBlur={hideTooltip}
    >
      <span className="ace-selection-annotation-badge-count">{annotationNumber}</span>
      {position && typeof document !== 'undefined'
        ? createPortal(
            <span
              id={tooltipId}
              className="ace-selection-annotation-tooltip"
              role="tooltip"
              data-placement={position.placement}
              style={{
                left: position.left,
                top: position.placement === 'below' ? position.top : undefined,
                bottom: position.placement === 'above' ? position.bottom : undefined,
              }}
            >
              <span className="ace-selection-annotation-tooltip-title">{label}</span>
              {items.map((annotation, index) => (
                <span className="ace-selection-annotation-tooltip-item" key={annotation.id || index}>
                  <span>{index + 1}</span>
                  <span>{annotation.text}</span>
                </span>
              ))}
            </span>,
            document.body,
          )
        : null}
    </span>
  );
}
