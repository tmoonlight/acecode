import { unwrapMarks, wrapTextNodeRange } from './domTextMarks.js';
import {
  groupSelectionAnnotationContexts,
  normalizeSelectionSourcePath,
  normalizeSelectionAnnotations,
  sameSelectionSourcePath,
  selectionAnchorText,
} from './selectionChatContext.js';

export {
  normalizeSelectionSourcePath,
  sameSelectionSourcePath,
  selectionAnchorText,
};

export const SELECTION_REFERENCE_MARK_CLASS = 'ace-selection-reference-mark';
export const INACTIVE_SOURCE_SELECTION_MARK_CLASS = 'ace-inactive-selection-mark';
export const SELECTION_ANNOTATION_BUBBLE_WIDTH = 23;

function asString(value) {
  return value == null ? '' : String(value);
}

function offsetInt(value) {
  const number = Number(value);
  return Number.isFinite(number) && number >= 0 ? Math.floor(number) : -1;
}

function finiteNumber(value, fallback = 0) {
  const number = Number(value);
  return Number.isFinite(number) ? number : fallback;
}

function nonEmptyClientRect(rect) {
  if (!rect) return false;
  const left = finiteNumber(rect.left);
  const top = finiteNumber(rect.top);
  const width = finiteNumber(rect.width, finiteNumber(rect.right) - left);
  const height = finiteNumber(rect.height, finiteNumber(rect.bottom) - top);
  return width > 0 && height > 0;
}

export function selectionAnnotationAnchorRect(mark) {
  try {
    const fragment = Array.from(mark?.getClientRects?.() || [])
      .find(nonEmptyClientRect);
    if (fragment) return fragment;
  } catch { /* Fall through to the union rectangle. */ }
  try {
    return mark?.getBoundingClientRect?.() || null;
  } catch {
    return null;
  }
}

export function selectionAnnotationBubbleLeft(markRect = {}, frameRect = {}, {
  bubbleWidth = SELECTION_ANNOTATION_BUBBLE_WIDTH,
  gap = 8,
  margin = 6,
} = {}) {
  const frameLeft = finiteNumber(frameRect.left);
  const frameWidth = Math.max(
    0,
    finiteNumber(frameRect.width, finiteNumber(frameRect.right) - frameLeft),
  );
  const rawLeft = finiteNumber(markRect.left) - frameLeft - bubbleWidth - gap;
  const maxLeft = Math.max(margin, frameWidth - bubbleWidth - margin);
  return Math.round(Math.min(Math.max(rawLeft, margin), maxLeft));
}

function positiveInt(value) {
  const number = Number(value);
  return Number.isFinite(number) && number > 0 ? Math.floor(number) : 0;
}

export function normalizeSelectionSourceText(value) {
  return asString(value).replace(/\r\n|\r/g, '\n');
}

// Fast deterministic fingerprint of the complete decoded document. Two
// independently mixed 32-bit lanes plus the exact length make accidental
// collisions negligible without requiring async Web Crypto during selection.
export function selectionSourceContentRevision(value) {
  const text = asString(value);
  let forward = 2166136261;
  let reverse = 2246822519 ^ text.length;
  for (let index = 0; index < text.length; index += 1) {
    forward = Math.imul(forward ^ text.charCodeAt(index), 16777619);
    reverse = Math.imul(
      reverse ^ text.charCodeAt(text.length - index - 1),
      3266489917,
    );
  }
  const hex = (number) => (number >>> 0).toString(16).padStart(8, '0');
  return `content-v1:${text.length.toString(36)}:${hex(forward)}${hex(reverse)}`;
}

function contextContentRevision(context = {}) {
  const source = context?.source && typeof context.source === 'object'
    ? context.source
    : {};
  return asString(source.content_revision ?? source.contentRevision);
}

export function selectionContextsForContentRevision(contexts = [], contentRevision = '') {
  const currentRevision = asString(contentRevision);
  const list = Array.isArray(contexts) ? contexts : [];
  if (!currentRevision) return list;
  return list.filter((context) => {
    const annotations = normalizeSelectionAnnotations(
      context?.annotations ?? context?.annotation,
    );
    if (annotations.length === 0) return true;
    const storedRevision = contextContentRevision(context);
    return !storedRevision || storedRevision === currentRevision;
  });
}

export function sourceLineStartOffset(text, lineNumber) {
  const source = normalizeSelectionSourceText(text);
  const target = positiveInt(lineNumber);
  if (!target || target === 1) return 0;
  let line = 1;
  for (let index = 0; index < source.length; index += 1) {
    if (source[index] !== '\n') continue;
    line += 1;
    if (line === target) return index + 1;
  }
  return source.length;
}

function lineNumberAtOffset(text, offset) {
  const source = normalizeSelectionSourceText(text);
  const end = Math.min(Math.max(0, offsetInt(offset)), source.length);
  let line = 1;
  for (let index = 0; index < end; index += 1) {
    if (source[index] === '\n') line += 1;
  }
  return line;
}

export function resolveSelectionAnchor(sourceText, context = {}) {
  const source = normalizeSelectionSourceText(sourceText);
  const selectedText = selectionAnchorText(context);
  const metadata = context.source && typeof context.source === 'object'
    ? context.source
    : {};
  if (!selectedText) {
    return { status: 'stale', reason: 'missing_anchor', start: -1, end: -1 };
  }

  const storedStart = offsetInt(metadata.start_offset ?? metadata.startOffset);
  const storedEnd = offsetInt(metadata.end_offset ?? metadata.endOffset);
  if (
    storedStart >= 0
    && storedEnd > storedStart
    && source.slice(storedStart, storedStart + selectedText.length) === selectedText
  ) {
    return {
      status: 'resolved',
      start: storedStart,
      end: storedStart + selectedText.length,
      startLine: lineNumberAtOffset(source, storedStart),
      endLine: lineNumberAtOffset(source, storedStart + selectedText.length),
      moved: storedEnd !== storedStart + selectedText.length,
    };
  }

  const preferredStart = storedStart >= 0
    ? storedStart
    : sourceLineStartOffset(source, metadata.start_line ?? metadata.startLine);
  let bestStart = -1;
  let bestDistance = Number.POSITIVE_INFINITY;
  let cursor = source.indexOf(selectedText);
  while (cursor >= 0) {
    const distance = Math.abs(cursor - preferredStart);
    if (distance < bestDistance) {
      bestStart = cursor;
      bestDistance = distance;
    }
    cursor = source.indexOf(selectedText, cursor + Math.max(1, selectedText.length));
  }
  if (bestStart < 0) {
    return { status: 'stale', reason: 'text_changed', start: -1, end: -1 };
  }
  return {
    status: 'resolved',
    start: bestStart,
    end: bestStart + selectedText.length,
    startLine: lineNumberAtOffset(source, bestStart),
    endLine: lineNumberAtOffset(source, bestStart + selectedText.length),
    moved: storedStart >= 0 ? bestStart !== storedStart : true,
  };
}

export function groupSelectionDecorations(
  contexts = [],
  sourcePath = '',
  view = '',
  contentRevision = '',
) {
  return groupSelectionAnnotationContexts(
    selectionContextsForContentRevision(contexts, contentRevision),
    { sourcePath, view },
  );
}

function textNodeParts(root, start, end) {
  if (!root?.ownerDocument?.createTreeWalker || end <= start) return [];
  const doc = root.ownerDocument;
  const view = doc.defaultView || globalThis.window || {};
  const nodeFilter = view.NodeFilter || globalThis.NodeFilter || {};
  const walker = doc.createTreeWalker(root, nodeFilter.SHOW_TEXT || 4);
  const parts = [];
  let cursor = 0;
  let node = walker.nextNode();
  while (node) {
    const text = asString(node.nodeValue);
    const nodeStart = cursor;
    const nodeEnd = nodeStart + text.length;
    if (nodeEnd > start && nodeStart < end) {
      parts.push({
        node,
        start: Math.max(0, start - nodeStart),
        end: Math.min(text.length, end - nodeStart),
      });
    }
    cursor = nodeEnd;
    node = walker.nextNode();
  }
  return parts;
}

function wrapElementTextRange(root, start, end, groupId, annotated = false) {
  const parts = textNodeParts(root, start, end);
  const marks = [];
  for (const part of parts.reverse()) {
    const mark = wrapTextNodeRange(
      part.node,
      part.start,
      part.end,
      SELECTION_REFERENCE_MARK_CLASS,
    );
    if (!mark) continue;
    mark.dataset.selectionDecorationId = groupId;
    mark.dataset.selectionAnnotated = annotated ? 'true' : 'false';
    marks.push(mark);
  }
  return marks.reverse();
}

function sourceRangeMarks(root, anchor, groupId, annotated = false) {
  const marks = [];
  const cells = Array.from(root?.querySelectorAll?.('.ace-line-code[data-source-start]') || []);
  for (const cell of cells) {
    const cellStart = offsetInt(cell.getAttribute('data-source-start'));
    const cellLength = offsetInt(cell.getAttribute('data-source-length'));
    if (cellStart < 0 || cellLength < 0) continue;
    const cellEnd = cellStart + cellLength;
    if (cellEnd <= anchor.start || cellStart >= anchor.end || cellLength === 0) continue;
    marks.push(...wrapElementTextRange(
      cell,
      Math.max(0, anchor.start - cellStart),
      Math.min(cellLength, anchor.end - cellStart),
      groupId,
      annotated,
    ));
  }
  return marks;
}

export function clearInactiveSourceSelection(root) {
  unwrapMarks(root, INACTIVE_SOURCE_SELECTION_MARK_CLASS);
}

export function applyInactiveSourceSelection(root, range = null) {
  clearInactiveSourceSelection(root);
  const start = offsetInt(range?.start);
  const end = offsetInt(range?.end);
  if (start < 0 || end <= start) return [];
  const marks = [];
  const cells = Array.from(root?.querySelectorAll?.('.ace-line-code[data-source-start]') || []);
  for (const cell of cells) {
    const cellStart = offsetInt(cell.getAttribute('data-source-start'));
    const cellLength = offsetInt(cell.getAttribute('data-source-length'));
    if (cellStart < 0 || cellLength < 0) continue;
    const cellEnd = cellStart + cellLength;
    if (cellEnd <= start || cellStart >= end || cellLength === 0) continue;
    const parts = textNodeParts(
      cell,
      Math.max(0, start - cellStart),
      Math.min(cellLength, end - cellStart),
    );
    for (const part of parts.reverse()) {
      const mark = wrapTextNodeRange(
        part.node,
        part.start,
        part.end,
        INACTIVE_SOURCE_SELECTION_MARK_CLASS,
      );
      if (mark) marks.push(mark);
    }
  }
  return marks.reverse();
}

function ignoredRenderedTextNode(node) {
  const parent = node?.parentElement;
  return !!parent?.closest?.(
    '.ace-code-actions, button, script, style, .ace-selection-annotation-layer, '
      + '.ace-markdown-code-highlight, .ace-markdown-opaque-children',
  );
}

export function renderedPreviewTextIndex(root) {
  if (root?.matches?.('[data-ace-managed-inactive-selection="true"]')) {
    const strings = Array.from(root.querySelectorAll?.('[data-slate-string="true"]') || []);
    return {
      text: strings.map((element) => asString(element.textContent)).join(''),
      nodes: [],
    };
  }
  if (!root?.ownerDocument?.createTreeWalker) return { text: '', nodes: [] };
  const doc = root.ownerDocument;
  const view = doc.defaultView || globalThis.window || {};
  const nodeFilter = view.NodeFilter || globalThis.NodeFilter || {};
  const walker = doc.createTreeWalker(
    root,
    nodeFilter.SHOW_TEXT || 4,
    {
      acceptNode(node) {
        if (!asString(node.nodeValue) || ignoredRenderedTextNode(node)) {
          return nodeFilter.FILTER_REJECT || 2;
        }
        return nodeFilter.FILTER_ACCEPT || 1;
      },
    },
  );
  const nodes = [];
  let text = '';
  let node = walker.nextNode();
  while (node) {
    const value = normalizeSelectionSourceText(node.nodeValue);
    const start = text.length;
    text += value;
    nodes.push({ node, start, end: text.length });
    node = walker.nextNode();
  }
  return { text, nodes };
}

function renderedRangeMarks(index, anchor, groupId, annotated = false) {
  const marks = [];
  const parts = [];
  for (const entry of index.nodes) {
    if (entry.end <= anchor.start || entry.start >= anchor.end) continue;
    parts.push({
      node: entry.node,
      start: Math.max(0, anchor.start - entry.start),
      end: Math.min(asString(entry.node.nodeValue).length, anchor.end - entry.start),
    });
  }
  for (const part of parts.reverse()) {
    const mark = wrapTextNodeRange(
      part.node,
      part.start,
      part.end,
      SELECTION_REFERENCE_MARK_CLASS,
    );
    if (!mark) continue;
    mark.dataset.selectionDecorationId = groupId;
    mark.dataset.selectionAnnotated = annotated ? 'true' : 'false';
    marks.push(mark);
  }
  return marks.reverse();
}

function bindGroupHover(marks) {
  const setHovered = (hovered) => {
    for (const mark of marks) mark.classList.toggle('is-hovered', hovered);
  };
  for (const mark of marks) {
    mark.addEventListener('mouseenter', () => setHovered(true));
    mark.addEventListener('mouseleave', () => setHovered(false));
  }
}

export function clearSelectionSourceDecorations(root) {
  unwrapMarks(root, SELECTION_REFERENCE_MARK_CLASS);
}

export function applySelectionSourceDecorations(root, {
  contexts = [],
  sourcePath = '',
  sourceText = '',
  contentRevision = '',
  rendered = false,
} = {}) {
  clearSelectionSourceDecorations(root);
  const currentView = rendered ? 'rendered' : 'source';
  const groups = groupSelectionDecorations(
    contexts,
    sourcePath,
    currentView,
    contentRevision,
  );
  const normalizedSource = normalizeSelectionSourceText(sourceText);
  const renderedIndex = rendered ? renderedPreviewTextIndex(root) : null;
  const targetText = rendered ? renderedIndex.text : normalizedSource;
  const applied = [];
  for (const group of groups) {
    const anchor = resolveSelectionAnchor(targetText, group.context);
    if (anchor.status !== 'resolved') {
      applied.push({ ...group, anchor, marks: [] });
      continue;
    }
    const marks = rendered
      ? renderedRangeMarks(renderedIndex, anchor, group.id, group.annotations.length > 0)
      : sourceRangeMarks(root, anchor, group.id, group.annotations.length > 0);
    if (marks.length === 0) {
      applied.push({
        ...group,
        anchor: { status: 'stale', reason: 'dom_unavailable', start: -1, end: -1 },
        marks: [],
      });
      continue;
    }
    bindGroupHover(marks);
    applied.push({ ...group, anchor, marks });
  }
  return applied;
}
