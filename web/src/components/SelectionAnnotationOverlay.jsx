import { useLayoutEffect, useRef, useState } from 'react';
import {
  applySelectionSourceDecorations,
  applyInactiveSourceSelection,
  clearInactiveSourceSelection,
  clearSelectionSourceDecorations,
  groupSelectionDecorations,
  renderedPreviewTextIndex,
  resolveSelectionAnchor,
  selectionAnnotationAnchorRect,
  selectionAnnotationBubbleLeft,
} from '../lib/selectionSourceDecorations.js';

const BUBBLE_GAP = 28;
const BUBBLE_LEFT = 6;
const FIRST_MARKER_TOP = 38;
const STALE_TOP = 38;

function measuredMarkers(frame, host, groups) {
  const frameRect = frame.getBoundingClientRect();
  const hostRect = host.getBoundingClientRect();
  const maxTop = Math.max(FIRST_MARKER_TOP, frameRect.height - 30);
  const resolved = [];
  const stale = [];
  for (const group of groups) {
    if (!group.annotations?.length || !group.annotationNumber) continue;
    const firstMark = group.marks?.find((mark) => mark?.isConnected);
    if (group.anchor?.status === 'resolved' && firstMark) {
      const rect = selectionAnnotationAnchorRect(firstMark);
      if (!rect) continue;
      if (
        rect.bottom < hostRect.top
        || rect.top > hostRect.bottom
        || rect.right < hostRect.left
        || rect.left > hostRect.right
      ) continue;
      resolved.push({
        id: group.id,
        number: group.annotationNumber,
        annotations: group.annotations,
        stale: false,
        left: selectionAnnotationBubbleLeft(rect, frameRect),
        top: Math.min(
          maxTop,
          Math.max(FIRST_MARKER_TOP, rect.top - frameRect.top + rect.height / 2 - 11),
        ),
      });
    } else {
      stale.push({
        id: group.id,
        number: group.annotationNumber,
        annotations: group.annotations,
        stale: true,
        left: BUBBLE_LEFT,
      });
    }
  }

  resolved.sort((left, right) => left.top - right.top || left.number - right.number);
  let previousTop = -BUBBLE_GAP;
  for (const marker of resolved) {
    marker.top = Math.min(maxTop, Math.max(marker.top, previousTop + BUBBLE_GAP));
    previousTop = marker.top;
  }
  const staleStart = Math.max(STALE_TOP, previousTop + BUBBLE_GAP);
  stale.forEach((marker, index) => {
    marker.top = Math.min(maxTop, staleStart + index * BUBBLE_GAP);
  });
  return [...resolved, ...stale];
}

export function SelectionAnnotationOverlay({
  hostRef,
  contexts = [],
  sourcePath = '',
  sourceText = '',
  contentRevision = '',
  rendered = false,
  inactiveRange = null,
  managedDecorations = false,
}) {
  const frameRef = useRef(0);
  const [appliedGroups, setAppliedGroups] = useState([]);
  const [markers, setMarkers] = useState([]);

  useLayoutEffect(() => {
    const host = hostRef?.current;
    if (!host) {
      setAppliedGroups([]);
      return undefined;
    }
    let groups;
    if (managedDecorations) {
      const targetText = renderedPreviewTextIndex(host).text;
      const marks = Array.from(
        host.querySelectorAll?.('[data-selection-decoration-id]') || [],
      );
      groups = groupSelectionDecorations(
        contexts,
        sourcePath,
        rendered ? 'rendered' : 'source',
        contentRevision,
      ).map((group) => ({
        ...group,
        anchor: resolveSelectionAnchor(targetText, group.context),
        marks: marks.filter(
          (mark) => mark.getAttribute('data-selection-decoration-id') === group.id,
        ),
      }));
    } else {
      groups = applySelectionSourceDecorations(host, {
        contexts,
        sourcePath,
        sourceText,
        contentRevision,
        rendered,
      });
      if (!rendered && inactiveRange) applyInactiveSourceSelection(host, inactiveRange);
    }
    setAppliedGroups(groups);
    return () => {
      if (!managedDecorations) {
        clearInactiveSourceSelection(host);
        clearSelectionSourceDecorations(host);
      }
    };
  }, [
    contentRevision,
    contexts,
    hostRef,
    inactiveRange,
    managedDecorations,
    rendered,
    sourcePath,
    sourceText,
  ]);

  useLayoutEffect(() => {
    const host = hostRef?.current;
    const frame = host?.closest?.('.ace-copyable-code');
    if (!host || !frame) {
      setMarkers([]);
      return undefined;
    }

    const measure = () => {
      frameRef.current = 0;
      setMarkers(measuredMarkers(frame, host, appliedGroups));
    };
    const scheduleMeasure = () => {
      if (frameRef.current) cancelAnimationFrame(frameRef.current);
      frameRef.current = requestAnimationFrame(measure);
    };
    scheduleMeasure();
    host.addEventListener('scroll', scheduleMeasure, { passive: true });
    window.addEventListener('resize', scheduleMeasure);
    const observer = typeof ResizeObserver === 'function'
      ? new ResizeObserver(scheduleMeasure)
      : null;
    observer?.observe(host);
    observer?.observe(frame);
    return () => {
      if (frameRef.current) cancelAnimationFrame(frameRef.current);
      frameRef.current = 0;
      host.removeEventListener('scroll', scheduleMeasure);
      window.removeEventListener('resize', scheduleMeasure);
      observer?.disconnect();
    };
  }, [appliedGroups, hostRef]);

  if (markers.length === 0) return null;
  return (
    <div className="ace-selection-annotation-layer">
      {markers.map((marker) => {
        const label = marker.stale
          ? `批注 ${marker.number}，原文已变化`
          : `批注 ${marker.number}`;
        return (
          <button
            key={`${marker.id}-${marker.number}`}
            type="button"
            className="ace-selection-annotation-bubble"
            data-stale={marker.stale ? 'true' : 'false'}
            style={{ left: marker.left, top: marker.top }}
            aria-label={label}
          >
            <span className="ace-selection-annotation-bubble-number">{marker.number}</span>
            <span className="ace-selection-annotation-bubble-tail" aria-hidden="true" />
            <span
              className="ace-selection-annotation-bubble-tooltip"
              data-ace-native-overlay="overlap"
              role="tooltip"
            >
              <span className="ace-selection-annotation-bubble-title">
                {marker.stale ? `${marker.number} · 原文已变化` : `批注 ${marker.number}`}
              </span>
              {marker.annotations.map((annotation, index) => (
                <span
                  className="ace-selection-annotation-bubble-item"
                  key={annotation.id || index}
                >
                  <span>{index + 1}</span>
                  <span>{annotation.text}</span>
                </span>
              ))}
            </span>
          </button>
        );
      })}
    </div>
  );
}
