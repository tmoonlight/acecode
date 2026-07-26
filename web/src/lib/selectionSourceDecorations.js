import { unwrapMarks, wrapTextNodeRange } from './domTextMarks.js';
import {
  normalizeComposerContext,
  normalizeSelectionAnnotations,
  selectionContextLocationKey,
} from './selectionChatContext.js';

export const SELECTION_REFERENCE_MARK_CLASS = 'ace-selection-reference-mark';

function asString(value) {
  return value == null ? '' : String(value);
}

function offsetInt(value) {
  const number = Number(value);
  return Number.isFinite(number) && number >= 0 ? Math.floor(number) : -1;
}

function positiveInt(value) {
  const number = Number(value);
  return Number.isFinite(number) && number > 0 ? Math.floor(number) : 0;
}

function stableStringHash(value) {
  let hash = 2166136261;
  const text = asString(value);
  for (let index = 0; index < text.length; index += 1) {
    hash ^= text.charCodeAt(index);
    hash = Math.imul(hash, 16777619);
  }
  return (hash >>> 0).toString(36);
}

export function normalizeSelectionSourceText(value) {
  return asString(value).replace(/\r\n|\r/g, '\n');
}

export function normalizeSelectionSourcePath(value) {
  const path = asString(value).replace(/\\/g, '/');
  return /^(?:[a-zA-Z]:\/|\/\/)/.test(path) ? path.toLowerCase() : path;
}

export function sameSelectionSourcePath(left, right) {
  const normalizedLeft = normalizeSelectionSourcePath(left);
  const normalizedRight = normalizeSelectionSourcePath(right);
  return !!normalizedLeft && normalizedLeft === normalizedRight;
}

export function selectionAnchorText(context = {}) {
  return normalizeSelectionSourceText(
    context.selected_text ?? context.selectedText ?? context.text ?? '',
  );
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

export function groupSelectionDecorations(contexts = [], sourcePath = '', view = '') {
  const groups = [];
  const byKey = new Map();
  for (const context of Array.isArray(contexts) ? contexts : []) {
    if (context?.type !== 'selection') continue;
    const normalized = normalizeComposerContext(context);
    if (!normalized) continue;
    const path = normalized.source?.path || '';
    if (!sameSelectionSourcePath(path, sourcePath)) continue;
    const contextView = normalized.source?.view || 'source';
    if (view && contextView !== view) continue;
    const anchorText = selectionAnchorText(normalized);
    if (!anchorText) continue;
    const location = selectionContextLocationKey(normalized);
    const key = [
      location || normalizeSelectionSourcePath(path),
      normalized.source?.view || 'source',
      anchorText,
    ].join('\u001f');
    let group = byKey.get(key);
    if (!group) {
      group = {
        id: normalized.id || `selection-decoration-${stableStringHash(key)}`,
        key,
        context: normalized,
        annotations: [],
        annotationNumber: 0,
      };
      byKey.set(key, group);
      groups.push(group);
    }
    const existingIds = new Set(group.annotations.map((annotation) => annotation.id));
    for (const annotation of normalizeSelectionAnnotations(normalized.annotations)) {
      if (existingIds.has(annotation.id)) continue;
      existingIds.add(annotation.id);
      group.annotations.push(annotation);
    }
  }

  let annotationNumber = 0;
  for (const group of groups) {
    if (group.annotations.length === 0) continue;
    annotationNumber += 1;
    group.annotationNumber = annotationNumber;
  }
  return groups;
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

function wrapElementTextRange(root, start, end, groupId) {
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
    marks.push(mark);
  }
  return marks.reverse();
}

function sourceRangeMarks(root, anchor, groupId) {
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
    ));
  }
  return marks;
}

function ignoredRenderedTextNode(node) {
  const parent = node?.parentElement;
  return !!parent?.closest?.(
    '.ace-code-actions, button, script, style, .ace-selection-annotation-layer',
  );
}

export function renderedPreviewTextIndex(root) {
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

function renderedRangeMarks(index, anchor, groupId) {
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
  rendered = false,
} = {}) {
  clearSelectionSourceDecorations(root);
  const currentView = rendered ? 'rendered' : 'source';
  const groups = groupSelectionDecorations(contexts, sourcePath, currentView);
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
      ? renderedRangeMarks(renderedIndex, anchor, group.id)
      : sourceRangeMarks(root, anchor, group.id);
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
