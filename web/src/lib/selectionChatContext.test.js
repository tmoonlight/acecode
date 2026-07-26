import assert from 'node:assert/strict';
import {
  contextPresentation,
  createFileContext,
  createSelectionAnnotation,
  createSelectionContext,
  formatSelectionContextLabel,
  formatSelectionContextNote,
  mergeSelectionAnnotations,
  normalizeComposerContext,
  normalizeSelectionAnnotations,
  resolveSelectionSourcePath,
  selectionContextLocationKey,
  selectionContextsFromTranscriptItems,
  selectionLineNumberAt,
  selectionLineCount,
  selectionPreviewKindSupportsActions,
  selectionSourceTextFromCells,
  truncateSelectionAnchorText,
  truncateSelectionAnnotation,
  truncateSelectionText,
  upsertSelectionContext,
} from './selectionChatContext.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('selection context formats file line ranges and line counts', () => {
  const ctx = createSelectionContext({
    id: 'sel1',
    text: 'first\nsecond',
    path: 'C:/repo/docs/README.md',
    kind: 'markdown',
    startLine: 23,
    endLine: 24,
  });
  assert.equal(formatSelectionContextLabel(ctx), 'README.md:23-24');
  assert.equal(formatSelectionContextNote(ctx), '2 行');
  assert.equal(ctx.source.path, 'C:/repo/docs/README.md');
  assert.equal(ctx.source.line_count, 2);
});

run('source selections read actual row numbers instead of counting rendered table text', () => {
  const sourceLineCell = {
    getAttribute(name) {
      return name === 'data-source-line' ? '17' : null;
    },
  };
  const parentElement = {
    closest(selector) {
      return selector === '.ace-line-code[data-source-line]' ? sourceLineCell : null;
    },
  };
  const source = {
    contains(element) {
      return element === sourceLineCell;
    },
  };
  assert.equal(selectionLineNumberAt(source, { nodeType: 3, parentElement }, 4), 17);
});

run('source selections rebuild blank lines without preview placeholder spaces', () => {
  const cells = [
    { textContent: '# Title', getAttribute: () => '7' },
    { textContent: ' ', getAttribute: () => '0' },
    { textContent: 'Paragraph', getAttribute: () => '9' },
    { textContent: ' ', getAttribute: () => '0' },
  ];
  const source = {
    querySelectorAll(selector) {
      assert.equal(selector, '.ace-line-code[data-source-length]');
      return cells;
    },
  };
  assert.equal(selectionSourceTextFromCells(source), '# Title\n\nParagraph\n');
});

run('selection contexts keep text when normalized for composer payload', () => {
  const payload = normalizeComposerContext({
    type: 'selection',
    local_id: 'local-sel',
    text: 'const a = 1;',
    source: { path: 'src/a.js', start_line: 7, end_line: 7, line_count: 1 },
  });
  assert.equal(payload.type, 'selection');
  assert.equal(payload.id, 'local-sel');
  assert.equal(payload.label, 'a.js:7');
  assert.equal(payload.text, 'const a = 1;');
  assert.equal(payload.selected_text, 'const a = 1;');
  assert.deepEqual(payload.annotations, []);
  assert.deepEqual(payload.source, {
    path: 'src/a.js',
    kind: '',
    line_count: 1,
    start_line: 7,
    end_line: 7,
  });
});

run('selection contexts preserve source offsets, rendered view, and persisted anchor text', () => {
  const ctx = createSelectionContext({
    id: 'sel-offset',
    text: 'first\r\nsecond',
    selectedText: 'first\r\nsecond',
    path: 'docs/a.md',
    kind: 'markdown',
    view: 'rendered',
    startLine: 2,
    endLine: 3,
    startOffset: 15,
    endOffset: 27,
  });
  assert.equal(ctx.selected_text, 'first\nsecond');
  assert.equal(ctx.source.view, 'rendered');
  assert.equal(ctx.source.start_offset, 15);
  assert.equal(ctx.source.end_offset, 27);
});

run('selection location keys match the same file range despite text changes', () => {
  const first = createSelectionContext({
    text: 'first selected text',
    path: 'src/a.js',
    startLine: 7,
    endLine: 7,
  });
  const second = createSelectionContext({
    text: 'different selected text',
    path: 'src/a.js',
    startLine: 7,
    endLine: 7,
  });
  const otherLine = createSelectionContext({
    text: 'first selected text',
    path: 'src/a.js',
    startLine: 8,
    endLine: 8,
  });

  assert.equal(selectionContextLocationKey(first), selectionContextLocationKey(second));
  assert.notEqual(selectionContextLocationKey(first), selectionContextLocationKey(otherLine));
});

run('selection location keys use precise offsets when available', () => {
  const first = createSelectionContext({
    text: 'first',
    path: 'src/a.js',
    startLine: 7,
    endLine: 7,
    startOffset: 20,
    endOffset: 25,
  });
  const second = createSelectionContext({
    text: 'second',
    path: 'src/a.js',
    startLine: 7,
    endLine: 7,
    startOffset: 30,
    endOffset: 36,
  });
  assert.notEqual(selectionContextLocationKey(first), selectionContextLocationKey(second));
});

run('selection source paths resolve preview-relative paths against cwd', () => {
  assert.equal(
    resolveSelectionSourcePath({ cwd: 'C:/repo', path: 'src/a.js' }),
    'C:/repo/src/a.js',
  );
  assert.equal(
    resolveSelectionSourcePath({ cwd: 'C:\\repo\\', path: 'src/a.js' }),
    'C:\\repo\\src\\a.js',
  );
  assert.equal(
    resolveSelectionSourcePath({ cwd: 'C:/repo', path: 'D:/other/a.js' }),
    'D:/other/a.js',
  );
});

run('non-selection contexts keep the existing browser payload shape', () => {
  assert.deepEqual(normalizeComposerContext({ type: 'browser', label: 'Browser', note: 'Context' }), {
    type: 'browser',
    label: 'Browser',
    note: 'Context',
  });
});

run('selection text helpers count and truncate predictably', () => {
  assert.equal(selectionLineCount('a\r\nb\nc'), 3);
  assert.equal(truncateSelectionText('abcdef', 3), 'abc\n[Selection truncated]');
  assert.equal(truncateSelectionAnchorText('a\r\nb', 3), 'a\nb');
  assert.equal(truncateSelectionAnnotation('abcdef', 3), 'abc\n[Annotation truncated]');
});

run('selection annotations normalize, bound, deduplicate, and preserve stable metadata', () => {
  const created = createSelectionAnnotation({
    id: 'ann-1',
    text: '  explain this branch  ',
    createdAt: '2026-07-26T00:00:00.000Z',
  });
  assert.deepEqual(created, {
    id: 'ann-1',
    text: 'explain this branch',
    created_at: '2026-07-26T00:00:00.000Z',
  });
  assert.deepEqual(
    normalizeSelectionAnnotations([
      created,
      created,
      { text: 'second', created_at: '2026-07-26T00:01:00.000Z' },
      { text: '   ' },
    ]).map((item) => item.text),
    ['explain this branch', 'second'],
  );
  assert.equal(mergeSelectionAnnotations([created], [{ id: 'ann-2', text: 'second' }]).length, 2);
});

run('annotated contexts survive composer normalization and expose hover presentation', () => {
  const payload = normalizeComposerContext({
    type: 'selection',
    selected_text: 'persisted selection',
    source: { path: 'src/a.js', start_line: 3, end_line: 3 },
    annotations: [{ id: 'ann-1', text: 'Check the fallback' }],
  });
  assert.equal(payload.text, 'persisted selection');
  assert.equal(payload.selected_text, 'persisted selection');
  assert.equal(payload.annotations.length, 1);
  const presentation = contextPresentation(payload);
  assert.equal(presentation.annotationCount, 1);
  assert.match(presentation.annotationText, /Check the fallback/);
  assert.match(presentation.title, /a\.js:3/);
});

run('upsert merges annotations into one pending card and keeps plain duplicates deduplicated', () => {
  const original = createSelectionContext({
    id: 'sel-1',
    localId: 'sel-1',
    text: 'selected',
    path: 'src/a.js',
    startLine: 3,
    endLine: 3,
    startOffset: 10,
    endOffset: 18,
  });
  const originalList = [original];
  const plainDuplicate = upsertSelectionContext(originalList, original);
  assert.equal(plainDuplicate.length, 1);
  assert.equal(plainDuplicate, originalList);

  const annotated = {
    ...original,
    annotations: [{ id: 'ann-1', text: 'First note' }],
  };
  const once = upsertSelectionContext([original], annotated);
  const twice = upsertSelectionContext(once, {
    ...original,
    annotations: [{ id: 'ann-2', text: 'Second note' }],
  });
  assert.equal(twice.length, 1);
  assert.deepEqual(twice[0].annotations.map((item) => item.text), ['First note', 'Second note']);
});

run('transcript content parts restore session selection contexts in message order', () => {
  const contexts = selectionContextsFromTranscriptItems([
    {
      kind: 'msg',
      contentParts: [
        { type: 'image', attachment: { id: 'image-1' } },
        {
          type: 'selection_context',
          context: {
            id: 'sel-1',
            selected_text: 'first',
            source: { path: 'src/a.js', start_line: 1, end_line: 1 },
          },
        },
      ],
    },
    {
      kind: 'msg',
      content_parts: [
        {
          type: 'selection_context',
          context: {
            id: 'sel-2',
            selected_text: 'second',
            source: { path: 'src/a.js', start_line: 2, end_line: 2 },
            annotations: [{ id: 'ann-2', text: 'Second annotation' }],
          },
        },
      ],
    },
  ]);
  assert.deepEqual(contexts.map((context) => context.id), ['sel-1', 'sel-2']);
  assert.equal(contexts[1].annotations[0].text, 'Second annotation');
});

run('selection action scope stays limited to text and markdown previews', () => {
  assert.equal(selectionPreviewKindSupportsActions('text'), true);
  assert.equal(selectionPreviewKindSupportsActions('markdown'), true);
  assert.equal(selectionPreviewKindSupportsActions('word'), false);
  assert.equal(selectionPreviewKindSupportsActions('pdf'), false);
  assert.equal(selectionPreviewKindSupportsActions('spreadsheet'), false);
});

run('createFileContext builds context with file name label and no line range', () => {
  const ctx = createFileContext({
    path: 'C:/repo/src/main.cpp',
    kind: 'text',
    text: 'int main() {\n  return 0;\n}\n',
  });
  assert.equal(ctx.type, 'selection');
  assert.equal(ctx.label, 'main.cpp');
  assert.equal(ctx.note, '4 行');
  assert.equal(ctx.source.path, 'C:/repo/src/main.cpp');
  assert.equal(ctx.source.kind, 'text');
  assert.equal(ctx.source.line_count, 4);
  assert.equal(ctx.source.start_line, undefined);
  assert.equal(ctx.source.end_line, undefined);
  assert.equal(ctx.text, 'int main() {\n  return 0;\n}\n');
});

run('createFileContext truncates large file content', () => {
  const big = 'x'.repeat(50000);
  const ctx = createFileContext({ path: 'big.txt', text: big });
  assert.ok(ctx.text.length <= 40001 + '[Selection truncated]'.length);
  assert.ok(ctx.text.endsWith('[Selection truncated]'));
});

run('createFileContext returns empty note for empty content', () => {
  const ctx = createFileContext({ path: 'empty.txt', text: '' });
  assert.equal(ctx.text, '');
  assert.equal(ctx.note, '');
  assert.equal(ctx.label, 'empty.txt');
});
