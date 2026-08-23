export const SELECTION_CONTEXT_TYPE = 'selection';
export const SELECTION_PREVIEW_SELECTOR = '[data-desktop-preview-path]';
export const MAX_SELECTION_CONTEXT_CHARS = 40000;
export const MAX_SELECTION_ANNOTATION_CHARS = 4000;
export const MAX_SELECTION_CONTENT_REVISION_CHARS = 128;

let annotationSequence = 0;

function asString(value) {
  return value == null ? '' : String(value);
}

function positiveInt(value) {
  const number = Number(value);
  return Number.isFinite(number) && number > 0 ? Math.floor(number) : 0;
}

function offsetInt(value) {
  const number = Number(value);
  return Number.isFinite(number) && number >= 0 ? Math.floor(number) : -1;
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

export function selectionLineCount(text) {
  const value = asString(text);
  if (!value) return 0;
  return value.split(/\r\n|\r|\n/).length;
}

export function truncateSelectionText(text, limit = MAX_SELECTION_CONTEXT_CHARS) {
  const value = asString(text);
  const max = Math.max(0, Number(limit) || 0);
  if (!max || value.length <= max) return value;
  return `${value.slice(0, max)}\n[Selection truncated]`;
}

export function truncateSelectionAnchorText(text, limit = MAX_SELECTION_CONTEXT_CHARS) {
  const value = asString(text).replace(/\r\n|\r/g, '\n');
  const max = Math.max(0, Number(limit) || 0);
  return max && value.length > max ? value.slice(0, max) : value;
}

export function truncateSelectionAnnotation(text, limit = MAX_SELECTION_ANNOTATION_CHARS) {
  const value = asString(text);
  const max = Math.max(0, Number(limit) || 0);
  if (!max || value.length <= max) return value;
  return `${value.slice(0, max)}\n[Annotation truncated]`;
}

export function nextSelectionAnnotationId(now = Date.now()) {
  annotationSequence = (annotationSequence + 1) % 0x100000;
  return `annotation-${now}-${annotationSequence.toString(36)}-${Math.random().toString(16).slice(2, 8)}`;
}

export function createSelectionAnnotation({
  id = '',
  text = '',
  createdAt = '',
  now = Date.now(),
} = {}) {
  const clippedText = truncateSelectionAnnotation(text).trim();
  if (!clippedText) return null;
  return {
    id: asString(id) || nextSelectionAnnotationId(now),
    text: clippedText,
    created_at: asString(createdAt) || new Date(now).toISOString(),
  };
}

export function normalizeSelectionAnnotations(value) {
  const raw = Array.isArray(value)
    ? value
    : (value == null || value === '' ? [] : [value]);
  const normalized = [];
  const seen = new Set();
  for (const entry of raw) {
    const object = entry && typeof entry === 'object' ? entry : { text: entry };
    const text = truncateSelectionAnnotation(object.text).trim();
    if (!text) continue;
    const createdAt = asString(object.created_at ?? object.createdAt);
    const id = asString(object.id)
      || `annotation-${stableStringHash(`${text}\u001f${createdAt}`)}`;
    if (seen.has(id)) continue;
    seen.add(id);
    normalized.push({
      id,
      text,
      ...(createdAt ? { created_at: createdAt } : {}),
    });
  }
  return normalized;
}

export function mergeSelectionAnnotations(...values) {
  const combined = [];
  for (const value of values) {
    if (Array.isArray(value)) combined.push(...value);
    else if (value != null && value !== '') combined.push(value);
  }
  return normalizeSelectionAnnotations(combined);
}

export function basenameForPath(path) {
  const value = asString(path).replace(/\\/g, '/');
  const parts = value.split('/').filter(Boolean);
  return parts.length ? parts[parts.length - 1] : value;
}

function isAbsolutePath(path) {
  const value = asString(path);
  return /^[a-zA-Z]:[\\/]/.test(value) || value.startsWith('\\\\') || value.startsWith('/');
}

export function resolveSelectionSourcePath({ cwd = '', path = '' } = {}) {
  const filePath = asString(path);
  if (!filePath) return '';
  if (isAbsolutePath(filePath)) return filePath;
  const base = asString(cwd);
  if (!base) return filePath;
  const sep = base.includes('\\') ? '\\' : '/';
  const cleanBase = base.replace(/[\\/]+$/, '');
  const cleanPath = filePath.replace(/^[\\/]+/, '');
  const normalizedPath = sep === '\\' ? cleanPath.replace(/\//g, '\\') : cleanPath.replace(/\\/g, '/');
  if (!cleanBase) return `${sep}${normalizedPath}`;
  return `${cleanBase}${sep}${normalizedPath}`;
}

export function formatSelectionLineRange(source = {}) {
  const start = positiveInt(source.start_line ?? source.startLine);
  const end = positiveInt(source.end_line ?? source.endLine);
  if (!start) return '';
  if (!end || end === start) return String(start);
  return `${start}-${end}`;
}

export function formatSelectionContextLabel(ctx = {}) {
  const source = ctx.source && typeof ctx.source === 'object' ? ctx.source : {};
  const file = basenameForPath(source.path || ctx.path || ctx.label || '') || 'Selection';
  const range = formatSelectionLineRange(source);
  return range ? `${file}:${range}` : file;
}

export function formatSelectionContextNote(ctx = {}) {
  const source = ctx.source && typeof ctx.source === 'object' ? ctx.source : {};
  const lines = positiveInt(source.line_count ?? source.lineCount) || selectionLineCount(ctx.text);
  return lines > 0 ? `${lines} 行` : '';
}

export function selectionContextFingerprint(ctx = {}) {
  const source = ctx.source && typeof ctx.source === 'object' ? ctx.source : {};
  return [
    SELECTION_CONTEXT_TYPE,
    source.path || ctx.path || '',
    source.start_line || source.startLine || '',
    source.end_line || source.endLine || '',
    source.content_revision || source.contentRevision || '',
    asString(ctx.text),
  ].join('\u001f');
}

export function selectionContextLocationKey(ctx = {}) {
  if ((ctx.type || '') !== SELECTION_CONTEXT_TYPE) return '';
  const source = ctx.source && typeof ctx.source === 'object' ? ctx.source : {};
  const path = asString(source.path || ctx.path || '').replace(/\\/g, '/');
  const view = asString(source.view || ctx.view || 'source');
  const startOffset = offsetInt(source.start_offset ?? source.startOffset);
  const endOffset = offsetInt(source.end_offset ?? source.endOffset);
  const start = positiveInt(source.start_line ?? source.startLine);
  const end = positiveInt(source.end_line ?? source.endLine) || start;
  const contentRevision = asString(
    source.content_revision ?? source.contentRevision,
  ).slice(0, MAX_SELECTION_CONTENT_REVISION_CHARS);
  if (!path || (startOffset < 0 && !start)) return '';
  if (startOffset >= 0 && endOffset > startOffset) {
    return [
      SELECTION_CONTEXT_TYPE,
      path,
      view,
      startOffset,
      endOffset,
      contentRevision,
    ].join('\u001f');
  }
  return [
    SELECTION_CONTEXT_TYPE,
    path,
    view,
    start,
    end,
    contentRevision,
  ].join('\u001f');
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
  return asString(
    context.selected_text ?? context.selectedText ?? context.text ?? '',
  ).replace(/\r\n|\r/g, '\n');
}

export function selectionAnnotationGroupKey(context = {}) {
  if (context?.type !== SELECTION_CONTEXT_TYPE) return '';
  const normalized = normalizeComposerContext(context);
  if (!normalized) return '';
  const path = normalized.source?.path || '';
  const anchorText = selectionAnchorText(normalized);
  if (!path || !anchorText) return '';
  const location = selectionContextLocationKey(normalized);
  return [
    location || normalizeSelectionSourcePath(path),
    normalized.source?.view || 'source',
    anchorText,
  ].join('\u001f');
}

export function groupSelectionAnnotationContexts(contexts = [], {
  sourcePath = '',
  view = '',
} = {}) {
  const groups = [];
  const byKey = new Map();
  for (const context of Array.isArray(contexts) ? contexts : []) {
    if (context?.type !== SELECTION_CONTEXT_TYPE) continue;
    const normalized = normalizeComposerContext(context);
    if (!normalized) continue;
    const path = normalized.source?.path || '';
    if (sourcePath && !sameSelectionSourcePath(path, sourcePath)) continue;
    const contextView = normalized.source?.view || 'source';
    if (view && contextView !== view) continue;
    const key = selectionAnnotationGroupKey(normalized);
    if (!key) continue;
    let group = byKey.get(key);
    if (!group) {
      group = {
        id: normalized.id || `selection-decoration-${stableStringHash(key)}`,
        key,
        sourceKey: [
          normalizeSelectionSourcePath(path),
          contextView,
        ].join('\u001f'),
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

  const numberBySource = new Map();
  for (const group of groups) {
    if (group.annotations.length === 0) continue;
    const annotationNumber = (numberBySource.get(group.sourceKey) || 0) + 1;
    numberBySource.set(group.sourceKey, annotationNumber);
    group.annotationNumber = annotationNumber;
  }
  return groups;
}

export function selectionAnnotationPresentationMap(contexts = []) {
  return new Map(
    groupSelectionAnnotationContexts(contexts)
      .filter((group) => group.annotationNumber > 0 && group.annotations.length > 0)
      .map((group) => [
        group.key,
        {
          annotationNumber: group.annotationNumber,
          annotations: group.annotations,
        },
      ]),
  );
}

export function createSelectionContext({
  id = '',
  localId = '',
  text = '',
  selectedText = '',
  path = '',
  kind = '',
  view = '',
  startLine = 0,
  endLine = 0,
  lineCount = 0,
  startOffset = -1,
  endOffset = -1,
  contentRevision = '',
  annotations = [],
} = {}) {
  const clippedText = truncateSelectionText(text);
  const anchorText = truncateSelectionAnchorText(selectedText || text);
  const safeStart = positiveInt(startLine);
  const safeEnd = positiveInt(endLine);
  const safeStartOffset = offsetInt(startOffset);
  const safeEndOffset = offsetInt(endOffset);
  const safeLineCount = positiveInt(lineCount) || selectionLineCount(clippedText);
  const source = {
    path: asString(path),
    kind: asString(kind),
    line_count: safeLineCount,
  };
  if (view) source.view = asString(view);
  if (safeStart) source.start_line = safeStart;
  if (safeEnd || safeStart) source.end_line = safeEnd || safeStart;
  if (safeStartOffset >= 0 && safeEndOffset > safeStartOffset) {
    source.start_offset = safeStartOffset;
    source.end_offset = safeEndOffset;
  }
  const safeContentRevision = asString(contentRevision)
    .slice(0, MAX_SELECTION_CONTENT_REVISION_CHARS);
  if (safeContentRevision) source.content_revision = safeContentRevision;

  const context = {
    type: SELECTION_CONTEXT_TYPE,
    local_id: localId || id || '',
    id: id || localId || '',
    text: clippedText,
    selected_text: anchorText,
    source,
    annotations: normalizeSelectionAnnotations(annotations),
  };
  context.label = formatSelectionContextLabel(context);
  context.note = formatSelectionContextNote(context);
  return context;
}

export function createFileContext({ path = '', kind = '', text = '' } = {}) {
  const clippedText = truncateSelectionText(text);
  const safeLineCount = selectionLineCount(clippedText);
  const fileName = basenameForPath(path);
  return {
    type: SELECTION_CONTEXT_TYPE,
    local_id: '',
    id: '',
    text: clippedText,
    source: {
      path: asString(path),
      kind: asString(kind),
      line_count: safeLineCount,
    },
    label: fileName || 'File',
    note: safeLineCount > 0 ? `${safeLineCount} 行` : '',
  };
}

export function normalizeComposerContext(ctx = {}) {
  if (!ctx || typeof ctx !== 'object') return null;
  if ((ctx.type || '') !== SELECTION_CONTEXT_TYPE) {
    const normalized = {
      type: ctx.type || 'browser',
      label: ctx.label || 'Browser',
      note: ctx.note || '',
    };
    if (ctx.id) normalized.id = asString(ctx.id);
    if (ctx.kind) normalized.kind = asString(ctx.kind);
    if (ctx.content != null || ctx.value != null) {
      normalized.content = asString(ctx.content ?? ctx.value);
    }
    if (ctx.source && typeof ctx.source === 'object') {
      normalized.source = {
        page_id: asString(ctx.source.page_id ?? ctx.source.pageId),
        url: asString(ctx.source.url),
        title: asString(ctx.source.title),
      };
    }
    return normalized;
  }

  const source = ctx.source && typeof ctx.source === 'object' ? ctx.source : {};
  const normalized = createSelectionContext({
    id: ctx.id || ctx.local_id || '',
    localId: ctx.local_id || ctx.id || '',
    text: ctx.text || ctx.selected_text || ctx.selectedText || '',
    selectedText: ctx.selected_text || ctx.selectedText || ctx.text || '',
    path: source.path || ctx.path || '',
    kind: source.kind || ctx.kind || '',
    view: source.view || ctx.view || '',
    startLine: source.start_line ?? source.startLine,
    endLine: source.end_line ?? source.endLine,
    lineCount: source.line_count ?? source.lineCount,
    startOffset: source.start_offset ?? source.startOffset,
    endOffset: source.end_offset ?? source.endOffset,
    contentRevision: source.content_revision ?? source.contentRevision,
    annotations: ctx.annotations ?? ctx.annotation ?? [],
  });
  if (!normalized.text.trim()) return null;
  return {
    type: SELECTION_CONTEXT_TYPE,
    id: normalized.id || normalized.local_id || selectionContextFingerprint(normalized),
    label: normalized.label,
    note: normalized.note,
    text: normalized.text,
    selected_text: normalized.selected_text,
    source: normalized.source,
    annotations: normalized.annotations,
  };
}

export function upsertSelectionContext(items = [], incoming = {}) {
  const list = Array.isArray(items) ? items : [];
  const normalized = normalizeComposerContext(incoming);
  if (!normalized) return list;
  const localId = incoming.local_id || incoming.id || normalized.id;
  const nextContext = {
    ...normalized,
    local_id: localId,
    id: normalized.id || localId,
  };
  const locationKey = selectionContextLocationKey(nextContext);
  const index = locationKey
    ? list.findIndex((item) => selectionContextLocationKey(item) === locationKey)
    : -1;
  if (index < 0) return [...list, nextContext];

  const existing = normalizeComposerContext(list[index]) || list[index];
  const annotations = mergeSelectionAnnotations(existing.annotations, nextContext.annotations);
  if (
    annotations.length === normalizeSelectionAnnotations(existing.annotations).length
    && nextContext.annotations.length === 0
  ) {
    return list;
  }
  const merged = {
    ...existing,
    local_id: list[index].local_id || list[index].id || localId,
    id: existing.id || list[index].id || localId,
    annotations,
  };
  return list.map((item, itemIndex) => (itemIndex === index ? merged : item));
}

export function selectionContextsFromTranscriptItems(items = []) {
  const contexts = [];
  for (const item of Array.isArray(items) ? items : []) {
    const parts = Array.isArray(item?.contentParts)
      ? item.contentParts
      : (Array.isArray(item?.content_parts) ? item.content_parts : []);
    for (const part of parts) {
      if (part?.type !== 'selection_context' || !part.context) continue;
      const normalized = normalizeComposerContext({
        ...part.context,
        type: SELECTION_CONTEXT_TYPE,
      });
      if (normalized) contexts.push(normalized);
    }
  }
  return contexts;
}

export function contextPresentation(ctx = {}, annotationPresentations = null) {
  if ((ctx.type || '') === SELECTION_CONTEXT_TYPE) {
    const label = formatSelectionContextLabel(ctx);
    const note = formatSelectionContextNote(ctx);
    const localAnnotations = normalizeSelectionAnnotations(ctx.annotations ?? ctx.annotation);
    const grouped = localAnnotations.length > 0
      ? annotationPresentations?.get?.(selectionAnnotationGroupKey(ctx))
      : null;
    const annotations = normalizeSelectionAnnotations(
      grouped?.annotations?.length ? grouped.annotations : localAnnotations,
    );
    const annotationNumber = annotations.length > 0
      ? positiveInt(grouped?.annotationNumber) || 1
      : 0;
    const annotationText = annotations
      .map((annotation, index) => `${index + 1}. ${annotation.text}`)
      .join('\n');
    return {
      icon: 'info',
      label,
      note,
      title: [label, note, annotationText].filter(Boolean).join('\n'),
      annotations,
      annotationNumber,
      annotationCount: annotations.length,
      annotationText,
      removeLabel: '移除引用上下文',
    };
  }
  return {
    icon: ctx.kind === 'console' ? 'terminal' : (ctx.kind === 'element' ? 'Inspect' : 'search'),
    label: ctx.label || '浏览器',
    note: ctx.note || '',
    title: ctx.label || '浏览器',
    removeLabel: '移除浏览器上下文',
  };
}

function elementFromNode(node) {
  if (!node) return null;
  if (node.nodeType === 1) return node;
  return node.parentElement || null;
}

function closestPreviewElement(node) {
  const el = elementFromNode(node);
  return el && typeof el.closest === 'function'
    ? el.closest(SELECTION_PREVIEW_SELECTOR)
    : null;
}

function sourceElementForPreview(preview) {
  return preview?.querySelector?.('.ace-preview')
    || preview?.querySelector?.('.ace-side-markdown-preview')
    || preview;
}

function textControlFromTarget(target) {
  const element = elementFromNode(target);
  if (!element) return null;
  if (element.matches?.('[data-ace-editable-preview-text="true"]')) return element;
  return element.closest?.('[data-ace-editable-preview-text="true"]') || null;
}

function lineNumberAtTextOffset(text, offset) {
  return selectionLineCount(String(text || '').slice(0, Math.max(0, Number(offset) || 0)));
}

export function textControlSelection(target) {
  const control = textControlFromTarget(target);
  if (!control) return null;
  const value = String(control.value || '');
  const startOffset = Math.max(0, Number(control.selectionStart) || 0);
  const endOffset = Math.max(startOffset, Number(control.selectionEnd) || startOffset);
  if (endOffset <= startOffset) return null;
  return {
    target: control,
    text: value.slice(startOffset, endOffset),
    value,
    startOffset,
    endOffset,
    startLine: lineNumberAtTextOffset(value, startOffset),
    endLine: lineNumberAtTextOffset(value, endOffset),
    view: 'source',
  };
}

export function selectionPreviewKindSupportsActions(kind) {
  return kind === 'text' || kind === 'markdown';
}

function textOffsetWithinElement(root, node, offset) {
  if (!root || !node) return -1;
  try {
    const range = document.createRange();
    range.selectNodeContents(root);
    range.setEnd(node, offset);
    const length = range.toString().length;
    range.detach?.();
    return length;
  } catch {
    return -1;
  }
}

function managedSlateTextNodes(root) {
  if (!root?.ownerDocument?.createTreeWalker) return [];
  const doc = root.ownerDocument;
  const view = doc.defaultView || globalThis.window || {};
  const nodeFilter = view.NodeFilter || globalThis.NodeFilter || {};
  const walker = doc.createTreeWalker(
    root,
    nodeFilter.SHOW_TEXT || 4,
    {
      acceptNode(node) {
        const parent = node?.parentElement;
        return parent?.closest?.('[data-slate-string="true"]')
          ? (nodeFilter.FILTER_ACCEPT || 1)
          : (nodeFilter.FILTER_REJECT || 2);
      },
    },
  );
  const nodes = [];
  let node = walker.nextNode();
  while (node) {
    nodes.push(node);
    node = walker.nextNode();
  }
  return nodes;
}

function managedSlateText(root) {
  return managedSlateTextNodes(root).map((node) => String(node.nodeValue || '')).join('');
}

function managedSlateTextOffsetWithinElement(root, pointNode, pointOffset) {
  if (!root?.contains?.(pointNode) || !root.ownerDocument?.createRange) return -1;
  const prefix = root.ownerDocument.createRange();
  try {
    prefix.selectNodeContents(root);
    prefix.setEnd(pointNode, pointOffset);
    let length = 0;
    for (const node of managedSlateTextNodes(root)) {
      const textLength = String(node.nodeValue || '').length;
      const startRelation = prefix.comparePoint(node, 0);
      if (startRelation > 0) break;
      const endRelation = prefix.comparePoint(node, textLength);
      if (endRelation <= 0) {
        length += textLength;
      } else if (node === pointNode) {
        length += Math.max(0, Math.min(textLength, Number(pointOffset) || 0));
      }
    }
    return length;
  } catch {
    return -1;
  } finally {
    prefix.detach?.();
  }
}

function sourceOffsetsForRange(source, range) {
  if (!source || !range) return { startOffset: -1, endOffset: -1, view: '' };
  const startElement = elementFromNode(range.startContainer);
  const endElement = elementFromNode(range.endContainer);
  const startCell = startElement?.closest?.('.ace-line-code');
  const endCell = endElement?.closest?.('.ace-line-code');
  if (startCell && endCell && source.contains(startCell) && source.contains(endCell)) {
    const startBase = offsetInt(startCell.getAttribute('data-source-start'));
    const endBase = offsetInt(endCell.getAttribute('data-source-start'));
    const startLength = offsetInt(startCell.getAttribute('data-source-length'));
    const endLength = offsetInt(endCell.getAttribute('data-source-length'));
    const localStart = textOffsetWithinElement(startCell, range.startContainer, range.startOffset);
    const localEnd = textOffsetWithinElement(endCell, range.endContainer, range.endOffset);
    if (
      startBase >= 0
      && endBase >= 0
      && startLength >= 0
      && endLength >= 0
      && localStart >= 0
      && localEnd >= 0
    ) {
      return {
        startOffset: startBase + Math.min(localStart, startLength),
        endOffset: endBase + Math.min(localEnd, endLength),
        view: 'source',
      };
    }
  }
  const managedSlate = source.matches?.('[data-ace-managed-inactive-selection="true"]');
  const startOffset = managedSlate
    ? managedSlateTextOffsetWithinElement(source, range.startContainer, range.startOffset)
    : textOffsetWithinElement(source, range.startContainer, range.startOffset);
  const endOffset = managedSlate
    ? managedSlateTextOffsetWithinElement(source, range.endContainer, range.endOffset)
    : textOffsetWithinElement(source, range.endContainer, range.endOffset);
  return {
    startOffset,
    endOffset,
    view: source.classList?.contains('ace-side-markdown-preview') ? 'rendered' : 'source',
  };
}

export function selectionSourceTextFromCells(source) {
  const cells = Array.from(
    source?.querySelectorAll?.('.ace-line-code[data-source-length]') || [],
  );
  if (cells.length === 0) return '';
  return cells.map((cell) => {
    const length = offsetInt(cell.getAttribute('data-source-length'));
    return asString(cell.textContent).slice(0, Math.max(0, length));
  }).join('\n');
}

export function selectionLineNumberAt(source, node, offset) {
  if (!source || !node) return 0;
  const nodeElement = elementFromNode(node);
  const sourceLineCell = nodeElement?.closest?.('.ace-line-code[data-source-line]');
  if (sourceLineCell && source.contains?.(sourceLineCell)) {
    const sourceLine = positiveInt(sourceLineCell.getAttribute('data-source-line'));
    if (sourceLine) return sourceLine;
  }
  if (source.matches?.('[data-ace-managed-inactive-selection="true"]')) {
    const textOffset = managedSlateTextOffsetWithinElement(source, node, offset);
    return textOffset >= 0 ? selectionLineCount(managedSlateText(source).slice(0, textOffset)) : 0;
  }
  if (nodeElement && !source.contains(nodeElement) && source !== nodeElement) return 0;
  try {
    const range = document.createRange();
    range.selectNodeContents(source);
    range.setEnd(node, offset);
    const before = range.toString();
    range.detach?.();
    return before ? selectionLineCount(before) : 1;
  } catch {
    return 0;
  }
}

export function selectionContextFromWindowSelection({
  target = null,
  selectedText = '',
  id = '',
  localId = '',
} = {}) {
  if (typeof window === 'undefined' || typeof document === 'undefined') return null;
  const selection = window.getSelection?.();
  const controlSelection = textControlSelection(target);
  let text = controlSelection?.text || selectedText || selection?.toString?.() || '';
  if (!text.trim()) return null;

  let preview = closestPreviewElement(target);
  let range = null;
  if (!controlSelection && selection?.rangeCount) {
    range = selection.getRangeAt(0);
    const startPreview = closestPreviewElement(range.startContainer);
    const endPreview = closestPreviewElement(range.endContainer);
    if (startPreview && startPreview === endPreview) {
      preview = startPreview;
    }
  }
  if (!preview) return null;

  const displayPath = preview.getAttribute('data-desktop-preview-path') || '';
  const path = preview.getAttribute('data-desktop-preview-source-path')
    || resolveSelectionSourcePath({
      cwd: preview.getAttribute('data-desktop-preview-cwd') || '',
      path: displayPath,
    });
  if (!path) return null;
  const kind = preview.getAttribute('data-desktop-preview-kind') || '';
  if (!selectionPreviewKindSupportsActions(kind)) return null;
  const contentRevision = preview.getAttribute('data-selection-source-revision') || '';

  let startLine = controlSelection?.startLine || 0;
  let endLine = controlSelection?.endLine || 0;
  const source = sourceElementForPreview(preview);
  const offsets = controlSelection || sourceOffsetsForRange(source, range);
  if (offsets.view === 'source' && offsets.startOffset >= 0 && offsets.endOffset > offsets.startOffset) {
    const sourceText = selectionSourceTextFromCells(source);
    const exactText = sourceText.slice(offsets.startOffset, offsets.endOffset);
    if (exactText.trim()) text = exactText;
  }
  if (range && source) {
    startLine = selectionLineNumberAt(source, range.startContainer, range.startOffset);
    endLine = selectionLineNumberAt(source, range.endContainer, range.endOffset);
    if (startLine && endLine && endLine < startLine) {
      [startLine, endLine] = [endLine, startLine];
    }
  }

  return createSelectionContext({
    id,
    localId,
    text,
    path,
    kind,
    view: offsets.view,
    startLine,
    endLine,
    lineCount: selectionLineCount(text),
    startOffset: offsets.startOffset,
    endOffset: offsets.endOffset,
    contentRevision,
  });
}
