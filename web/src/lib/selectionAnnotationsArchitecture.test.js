import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const srcRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

function source(relativePath) {
  return fs.readFileSync(path.join(srcRoot, relativePath), 'utf8');
}

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('selection toolbar keeps the exact quote and annotation contract', () => {
  const popover = source('components/SelectionActionPopover.jsx');
  const quoteIndex = popover.indexOf('<span>引用到聊天</span>');
  const annotationIndex = popover.indexOf('<span>批注</span>');
  assert.ok(quoteIndex >= 0);
  assert.ok(annotationIndex > quoteIndex);
  assert.match(popover, /role="toolbar" aria-label="选中文本操作"/);
  assert.match(popover, /event\.key === 'Enter' && !event\.shiftKey/);
  assert.match(popover, /event\.key === 'Escape'/);
  assert.match(popover, /maxLength=\{MAX_SELECTION_ANNOTATION_CHARS\}/);
  assert.match(popover, /document\.addEventListener\('pointerdown'/);
});

run('ChatView scopes preview decorations to transcript and composer contexts', () => {
  const chat = source('components/ChatView.jsx');
  assert.match(chat, /selectionContextsFromTranscriptItems\(rawItems\)/);
  assert.match(chat, /\[\.\.\.sentSelectionContexts, \.\.\.composerContexts\]/);
  assert.match(chat, /selectionContexts=\{previewSelectionContexts\}/);
  assert.match(chat, /upsertSelectionContext\(items, pinned\)/);
  assert.match(chat, /clearPreviewSelection\(\)/);
  assert.match(chat, /<SelectionActionPopover/);
  assert.match(chat, /target\?\.closest\?\.\(SELECTION_PREVIEW_SELECTOR\)/);
  assert.match(chat, /if \(!event\.shiftKey \|\| !target\?\.closest\?\.\(SELECTION_PREVIEW_SELECTOR\)\) return/);
});

run('file previews expose precise source offsets and both supported decoration surfaces', () => {
  const preview = source('components/FilePreviewContent.jsx');
  assert.match(preview, /data-source-line=/);
  assert.match(preview, /data-source-start=/);
  assert.match(preview, /data-source-length=/);
  assert.equal((preview.match(/<SelectionAnnotationOverlay/g) || []).length, 2);
  assert.match(preview, /className="h-full overflow-auto ace-md ace-side-markdown-preview"/);
  assert.match(preview, /className="h-full overflow-auto text-\[11px\] ace-preview"/);
  assert.match(preview, /rendered\s*\/>/);
  const styles = source('styles/globals.css');
  assert.match(styles, /\.ace-preview,\s*\.ace-side-markdown-preview\s*\{[^}]*user-select:\s*text;/s);
});

run('composer and sent selection cards share the compact annotation badge', () => {
  const input = source('components/InputBar.jsx');
  const sent = source('components/AttachmentStrip.jsx');
  assert.match(input, /<SelectionAnnotationBadge annotations=\{presentation\.annotations\} compact \/>/);
  assert.match(sent, /<SelectionAnnotationBadge annotations=\{presentation\.annotations\} compact \/>/);
});

run('plain references get source marks while only annotated groups get bubbles', () => {
  const overlay = source('components/SelectionAnnotationOverlay.jsx');
  const decorations = source('lib/selectionSourceDecorations.js');
  const styles = source('styles/globals.css');
  assert.match(decorations, /SELECTION_REFERENCE_MARK_CLASS/);
  assert.match(decorations, /dataset\.selectionAnnotated = annotated/);
  assert.match(decorations, /group\.annotationNumber = annotationNumber/);
  assert.match(overlay, /if \(!group\.annotations\?\.length \|\| !group\.annotationNumber\) continue/);
  assert.match(overlay, /selectionAnnotationBubbleLeft\(rect, frameRect\)/);
  assert.match(overlay, /const staleStart = Math\.max\(STALE_TOP, previousTop \+ BUBBLE_GAP\)/);
  assert.match(styles, /\.ace-selection-reference-mark\[data-selection-annotated="true"\]/);
  assert.match(styles, /\[data-theme="dark"\] \.ace-selection-reference-mark\[data-selection-annotated="true"\]/);
  assert.match(styles, /\.ace-selection-annotation-bubble-tooltip\s*\{[^}]*left: calc\(100% \+ 9px\)/s);
});

console.log('selectionAnnotationsArchitecture tests passed');
