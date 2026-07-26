import assert from 'node:assert/strict';
import {
  groupSelectionDecorations,
  normalizeSelectionSourcePath,
  resolveSelectionAnchor,
  sameSelectionSourcePath,
  sourceLineStartOffset,
} from './selectionSourceDecorations.js';
import { createSelectionContext } from './selectionChatContext.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('stored selection offsets win when their text still matches', () => {
  const context = createSelectionContext({
    text: 'beta',
    path: 'src/a.txt',
    startLine: 2,
    endLine: 2,
    startOffset: 6,
    endOffset: 10,
  });
  assert.deepEqual(resolveSelectionAnchor('alpha\nbeta\ngamma', context), {
    status: 'resolved',
    start: 6,
    end: 10,
    startLine: 2,
    endLine: 2,
    moved: false,
  });
});

run('anchor resolution follows the nearest exact text after edits', () => {
  const context = createSelectionContext({
    text: 'target',
    path: 'src/a.txt',
    startLine: 2,
    endLine: 2,
    startOffset: 8,
    endOffset: 14,
  });
  const resolved = resolveSelectionAnchor('prefix target\nother target', context);
  assert.equal(resolved.status, 'resolved');
  assert.equal(resolved.start, 7);
  assert.equal(resolved.moved, true);
});

run('anchor resolution marks changed original text stale without fuzzy matching', () => {
  const context = createSelectionContext({
    text: 'old branch',
    path: 'src/a.txt',
    startLine: 1,
    endLine: 1,
    startOffset: 0,
    endOffset: 10,
  });
  assert.deepEqual(resolveSelectionAnchor('new branch', context), {
    status: 'stale',
    reason: 'text_changed',
    start: -1,
    end: -1,
  });
});

run('line start fallback resolves a legacy context near its recorded line', () => {
  const context = createSelectionContext({
    text: 'repeat',
    path: 'src/a.txt',
    startLine: 3,
    endLine: 3,
  });
  const source = 'repeat\nother\nrepeat';
  assert.equal(sourceLineStartOffset(source, 3), 13);
  assert.equal(resolveSelectionAnchor(source, context).start, 13);
});

run('Windows source paths match case-insensitively with normalized separators', () => {
  assert.equal(normalizeSelectionSourcePath('C:\\Repo\\A.cpp'), 'c:/repo/a.cpp');
  assert.equal(sameSelectionSourcePath('C:\\Repo\\A.cpp', 'c:/repo/a.cpp'), true);
  assert.equal(
    sameSelectionSourcePath('\\\\Server\\Share\\A.cpp', '//server/share/a.cpp'),
    true,
  );
  assert.equal(sameSelectionSourcePath('/Repo/A.cpp', '/repo/a.cpp'), false);
});

run('decoration groups merge same-anchor annotations and number annotated passages per file', () => {
  const base = {
    text: 'first',
    path: 'C:/repo/a.txt',
    startLine: 1,
    endLine: 1,
    startOffset: 0,
    endOffset: 5,
  };
  const contexts = [
    createSelectionContext({
      ...base,
      id: 'plain',
    }),
    createSelectionContext({
      ...base,
      id: 'annotated-1',
      annotations: [{ id: 'ann-1', text: 'First note' }],
    }),
    createSelectionContext({
      ...base,
      id: 'annotated-2',
      annotations: [{ id: 'ann-2', text: 'Second note' }],
    }),
    createSelectionContext({
      id: 'other',
      text: 'second',
      path: 'C:/repo/a.txt',
      startLine: 2,
      endLine: 2,
      startOffset: 6,
      endOffset: 12,
      annotations: [{ id: 'ann-3', text: 'Other passage' }],
    }),
    createSelectionContext({
      id: 'different-file',
      text: 'ignored',
      path: 'C:/repo/b.txt',
      startLine: 1,
      endLine: 1,
      annotations: [{ id: 'ann-4', text: 'Ignored' }],
    }),
  ];
  const groups = groupSelectionDecorations(contexts, 'c:\\repo\\a.txt');
  assert.equal(groups.length, 2);
  assert.deepEqual(groups[0].annotations.map((item) => item.text), ['First note', 'Second note']);
  assert.deepEqual(groups.map((group) => group.annotationNumber), [1, 2]);
});

run('source and rendered Markdown anchors stay isolated to their captured view', () => {
  const contexts = [
    createSelectionContext({
      id: 'source',
      text: 'shared phrase',
      path: 'C:/repo/readme.md',
      view: 'source',
      startOffset: 0,
      endOffset: 13,
      annotations: [{ id: 'source-note', text: 'Source note' }],
    }),
    createSelectionContext({
      id: 'rendered',
      text: 'shared phrase',
      path: 'C:/repo/readme.md',
      view: 'rendered',
      startOffset: 0,
      endOffset: 13,
      annotations: [{ id: 'rendered-note', text: 'Rendered note' }],
    }),
  ];
  const sourceGroups = groupSelectionDecorations(
    contexts,
    'C:/repo/readme.md',
    'source',
  );
  const renderedGroups = groupSelectionDecorations(
    contexts,
    'C:/repo/readme.md',
    'rendered',
  );
  assert.deepEqual(sourceGroups.map((group) => group.id), ['source']);
  assert.deepEqual(renderedGroups.map((group) => group.id), ['rendered']);
});

run('plain references create a decoration group without an annotation bubble number', () => {
  const groups = groupSelectionDecorations([
    createSelectionContext({
      id: 'plain-only',
      text: 'plain reference',
      path: 'C:/repo/a.txt',
      view: 'source',
      startOffset: 0,
      endOffset: 15,
    }),
  ], 'C:/repo/a.txt', 'source');
  assert.equal(groups.length, 1);
  assert.deepEqual(groups[0].annotations, []);
  assert.equal(groups[0].annotationNumber, 0);
});
