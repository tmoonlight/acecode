import {
  groupSelectionDecorations,
  resolveSelectionAnchor,
} from './selectionSourceDecorations.js';

function pathKey(path) {
  return Array.isArray(path) ? path.join('.') : '';
}

function comparePaths(left = [], right = []) {
  const count = Math.min(left.length, right.length);
  for (let index = 0; index < count; index += 1) {
    if (left[index] !== right[index]) return left[index] - right[index];
  }
  return left.length - right.length;
}

function comparePoints(left = {}, right = {}) {
  const pathOrder = comparePaths(left.path, right.path);
  if (pathOrder) return pathOrder;
  return Number(left.offset || 0) - Number(right.offset || 0);
}

export function slateTextEntries(document = []) {
  const entries = [];
  let cursor = 0;
  const visit = (nodes, parentPath = []) => {
    for (let index = 0; index < (Array.isArray(nodes) ? nodes.length : 0); index += 1) {
      const node = nodes[index];
      const path = [...parentPath, index];
      if (node && Object.prototype.hasOwnProperty.call(node, 'text')) {
        const text = String(node.text || '');
        entries.push({ node, path, text, start: cursor, end: cursor + text.length });
        cursor += text.length;
      } else if (Array.isArray(node?.children)) {
        visit(node.children, path);
      }
    }
  };
  visit(document);
  return entries;
}

export function slateDocumentText(document = []) {
  return slateTextEntries(document).map((entry) => entry.text).join('');
}

function appendRange(rangesByPath, path, range) {
  const key = pathKey(path);
  const ranges = rangesByPath.get(key) || [];
  ranges.push(range);
  rangesByPath.set(key, ranges);
}

function appendPersistedRanges(rangesByPath, entries, groups, documentText) {
  return groups.map((group) => {
    const anchor = resolveSelectionAnchor(documentText, group.context);
    if (anchor.status !== 'resolved') return { ...group, anchor };
    for (const entry of entries) {
      if (entry.end <= anchor.start || entry.start >= anchor.end || !entry.text.length) continue;
      const start = Math.max(0, anchor.start - entry.start);
      const end = Math.min(entry.text.length, anchor.end - entry.start);
      appendRange(rangesByPath, entry.path, {
        anchor: { path: entry.path, offset: start },
        focus: { path: entry.path, offset: end },
        selectionDecorationId: group.id,
        selectionAnnotated: group.annotations.length > 0,
      });
    }
    return { ...group, anchor };
  });
}

function appendInactiveRanges(rangesByPath, entries, selection) {
  if (!selection?.anchor?.path || !selection?.focus?.path) return;
  let start = selection.anchor;
  let end = selection.focus;
  if (comparePoints(start, end) > 0) [start, end] = [end, start];
  if (comparePoints(start, end) === 0) return;

  for (const entry of entries) {
    if (comparePaths(entry.path, start.path) < 0 || comparePaths(entry.path, end.path) > 0) continue;
    const localStart = comparePaths(entry.path, start.path) === 0
      ? Math.max(0, Math.min(entry.text.length, Number(start.offset || 0)))
      : 0;
    const localEnd = comparePaths(entry.path, end.path) === 0
      ? Math.max(0, Math.min(entry.text.length, Number(end.offset || 0)))
      : entry.text.length;
    if (localEnd <= localStart) continue;
    appendRange(rangesByPath, entry.path, {
      anchor: { path: entry.path, offset: localStart },
      focus: { path: entry.path, offset: localEnd },
      inactiveSelection: true,
    });
  }
}

export function slateSelectionDecorationModel(document = [], {
  contexts = [],
  sourcePath = '',
  contentRevision = '',
  inactiveSelection = null,
} = {}) {
  const entries = slateTextEntries(document);
  const documentText = entries.map((entry) => entry.text).join('');
  const groups = groupSelectionDecorations(
    contexts,
    sourcePath,
    'rendered',
    contentRevision,
  );
  const rangesByPath = new Map();
  const appliedGroups = appendPersistedRanges(
    rangesByPath,
    entries,
    groups,
    documentText,
  );
  appendInactiveRanges(rangesByPath, entries, inactiveSelection);
  return { documentText, groups: appliedGroups, rangesByPath };
}
