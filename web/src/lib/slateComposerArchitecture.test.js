import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const srcRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const webRoot = path.resolve(srcRoot, '..');

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

run('composer runtime depends on Slate and no longer depends on Lexical', () => {
  const packageJson = JSON.parse(fs.readFileSync(path.join(webRoot, 'package.json'), 'utf8'));
  const dependencies = packageJson.dependencies || {};
  assert.ok(dependencies.slate);
  assert.ok(dependencies['slate-react']);
  assert.ok(dependencies['slate-history']);
  assert.equal(dependencies.lexical, undefined);
  assert.equal(dependencies['@lexical/react'], undefined);

  const composer = source('components/RichComposer.jsx');
  assert.match(composer, /from 'slate';/);
  assert.match(composer, /from 'slate-react';/);
  assert.match(composer, /from 'slate-history';/);
  assert.doesNotMatch(composer, /lexical/i);
});

run('command, skill, path, session, and attachment tags use Slate inline void elements', () => {
  const composer = source('components/RichComposer.jsx');
  assert.match(composer, /editor\.isInline = \(element\) => \(\s+isComposerInlineTag\(element\) \? true : isInline\(element\)/s);
  assert.match(composer, /editor\.isVoid = \(element\) => \(\s+isComposerInlineTag\(element\) \? true : isVoid\(element\)/s);
  assert.equal((composer.match(/draggable=\{false\}/g) || []).length, 4);
  assert.match(composer, /types\.includes\('application\/x-slate-fragment'\)/);
});

run('composer command tag reuses the sent-message markup without a visible slash', () => {
  const composer = source('components/RichComposer.jsx');
  const message = source('components/Message.jsx');
  const styles = source('styles/globals.css');

  assert.match(composer, /replace\(\/\^\\\/\+\/, ''\)/);
  assert.match(composer, /className="ace-slate-inline-tag"/);
  assert.match(composer, /className="ace-cmd-token"/);
  assert.match(composer, /<CommandGlyph[^>]*className="ace-cmd-token-glyph"/s);
  assert.match(composer, /className="ace-cmd-token-name">\{displayName\}/);
  assert.match(message, /className="ace-cmd-token"/);
  assert.match(styles, /\.ace-cmd-token\s*\{/);
  assert.doesNotMatch(styles, /\.ace-rich-command-token\s*\{/);
});

run('composer tags use unboxed accent text and native selection colors', () => {
  const styles = source('styles/globals.css');
  assert.match(styles, /--ace-composer-selection-bg:\s*Highlight;/);
  assert.match(styles, /--ace-composer-selection-fg:\s*HighlightText;/);
  assert.match(styles, /\.ace-slate-inline-tag \*\s*\{[^}]*transition:\s*none;/);
  assert.match(styles, /\.ace-slate-inline-tag > \.ace-cmd-token\s*\{[^}]*align-items:\s*baseline;[^}]*margin:\s*0;[^}]*padding:\s*0;[^}]*border:\s*0;[^}]*border-radius:\s*0;[^}]*background:\s*transparent;[^}]*font:\s*inherit;[^}]*font-weight:\s*600;/);
  assert.match(styles, /\.ace-slate-inline-tag \.ace-cmd-token-name\s*\{[^}]*font:\s*inherit;/);
  assert.match(styles, /\.ace-slate-inline-tag \.ace-cmd-token-glyph\s*\{[^}]*align-self:\s*center;/);
  assert.match(styles, /\.ace-slate-inline-tag \.ace-cmd-token-name\s*\{[^}]*color:\s*inherit;/);
  assert.match(styles, /\.ace-slate-inline-tag \.ace-cmd-token-glyph\s*\{[^}]*color:\s*inherit !important;/);
  assert.match(styles, /\.ace-slate-inline-tag\[data-composer-selected="true"\] > \.ace-cmd-token\s*\{[^}]*color:\s*var\(--ace-composer-selection-fg\);[^}]*opacity:\s*1;/);
  assert.match(styles, /\.ace-slate-inline-tag\[data-composer-selected="true"\]::before\s*\{[^}]*background:\s*var\(--ace-composer-selection-bg\);/);
});

run('path tags keep canonical text while using inline tag markup', () => {
  const composer = source('components/RichComposer.jsx');
  assert.match(composer, /data-composer-inline-tag="path"/);
  assert.match(composer, /className="ace-slate-inline-tag ace-slate-path-tag"/);
  assert.match(composer, /element\?\.directory\s+\? <VsIcon name="folder"/s);
  assert.match(composer, /<FileTypeIcon path=\{path\} size="1em"/);
});

run('session tags keep stable identity while reusing inline tag markup', () => {
  const composer = source('components/RichComposer.jsx');
  assert.match(composer, /data-composer-inline-tag="session"/);
  assert.match(composer, /className="ace-slate-inline-tag ace-slate-session-tag"/);
  assert.match(composer, /<VsIcon name="newSession" size="1em"/);
});

run('atomic deletion is routed through the plain-text tag range helper', () => {
  const composer = source('components/RichComposer.jsx');
  const model = source('lib/richComposerModel.js');
  assert.match(model, /export function composerAdjacentTagDeletionRange/);
  assert.match(composer, /composerAdjacentTagDeletionRange\(editor\.children, editor\.selection, direction\)/);
  assert.match(composer, /Transforms\.select\(editor, composerSelectionFromPlainTextRange/);
  assert.match(composer, /Transforms\.delete\(editor\)/);
});

run('imperative focus retries after an external Slate document replacement', () => {
  const composer = source('components/RichComposer.jsx');
  assert.match(composer, /const focusEditor = \(\) =>/);
  assert.match(composer, /try \{\s*if \(focusEditor\(\)\) return;\s*\} catch \{/s);
  assert.match(composer, /window\.requestAnimationFrame\(\(\) => \{\s*try \{ focusEditor\(\); \} catch \{\}/s);
});

run('desktop Ctrl+Enter inserts a line break while plain Enter still submits', () => {
  const composer = source('components/RichComposer.jsx');
  const enter = composer.indexOf("event.key === 'Enter'");
  const desktopCtrlEnter = composer.indexOf('event.ctrlKey && isDesktopShell()', enter);
  const insertBreak = composer.indexOf('editor.insertBreak()', desktopCtrlEnter);
  const plainEnter = composer.indexOf('if (!event.shiftKey)', insertBreak);
  const submit = composer.indexOf('onSubmit?.()', plainEnter);

  assert.ok(enter >= 0);
  assert.ok(desktopCtrlEnter > enter);
  assert.ok(insertBreak > desktopCtrlEnter);
  assert.ok(plainEnter > insertBreak);
  assert.ok(submit > plainEnter);
});

run('placeholder stays top-aligned without the inset shorthand', () => {
  const composer = source('components/RichComposer.jsx');
  const styles = source('styles/globals.css');

  assert.match(composer, /ace-rich-composer-placeholder/);
  assert.doesNotMatch(composer, /inset-0/);
  assert.match(
    styles,
    /\.ace-rich-composer-placeholder\s*\{[^}]*position: absolute;[^}]*top: 0;[^}]*right: 0;[^}]*left: 0;/s,
  );
  assert.doesNotMatch(
    styles.slice(styles.indexOf('.ace-rich-composer-placeholder')),
    /\.ace-rich-composer-placeholder\s*\{[^}]*inset:/s,
  );
});

run('slash candidate confirmation commits the command with a trailing space and caret after it', () => {
  const inputBar = source('components/InputBar.jsx');
  const start = inputBar.indexOf('const handleSelectCommand = (item) => {');
  const end = inputBar.indexOf('\n  };', start);
  const handler = inputBar.slice(start, end);

  assert.ok(start >= 0 && end > start);
  assert.match(handler, /if \(!commandQuery\.leading\) return/);
  assert.match(handler, /insertSkill\?\.\(item, commandQuery\.begin, commandQuery\.end\)/);
  assert.match(handler, /value\.slice\(commandQuery\.end\)/);
  assert.match(handler, /updateValue\(next, undefined, commandQuery, \{ goalMode: selectedGoal \}\)/);
  assert.match(handler, /setSelectionRange\(cursor, cursor\)/);
});

run('composer external sync is composition-safe, generation-aware, and semantic', () => {
  const inputBar = source('components/InputBar.jsx');
  const composer = source('components/RichComposer.jsx');
  const decisionIndex = composer.indexOf('const decision = classifyComposerExternalSync');
  const effectStart = composer.lastIndexOf('useEffect(() => {', decisionIndex);
  const effectEnd = composer.indexOf('const handleValueChange', decisionIndex);
  const syncEffect = composer.slice(effectStart, effectEnd);

  assert.ok(effectStart >= 0);
  assert.ok(effectEnd > effectStart);
  assert.match(inputBar, /syncKey=\{fileIntakeScope\}/);
  assert.match(inputBar, /fileIntakeScope = JSON\.stringify\(\[cwd, currentSessionId\]\)/);
  assert.doesNotMatch(inputBar, /<RichComposer[\s\S]*?key=\{currentSessionId\}/);
  assert.match(composer, /syncIdentityRef\.current\.generation \+ 1/);
  assert.match(composer, /documentSyncGenerationRef\.current !== activeSyncGeneration/);
  assert.match(composer, /documentSyncGenerationRef\.current !== activeGeneration\) return/);
  assert.match(composer, /appendComposerLocalEcho\(localEchoes, text\)/);
  assert.match(composer, /compositionStateRef\.current\.active = true/);
  assert.match(composer, /compositionStateRef\.current\.settling = true/);
  assert.match(composer, /window\.setTimeout\(\(\) => \{[\s\S]*setSyncRevision/s);
  assert.match(composer, /onCompositionStart=\{handleCompositionStart\}/);
  assert.match(composer, /onCompositionEnd=\{handleCompositionEnd\}/);
  assert.match(syncEffect, /ReactEditor\.isComposing\(editor\)/);
  assert.match(syncEffect, /compositionStateRef\.current\.active[\s\S]*compositionStateRef\.current\.settling/);
  assert.match(syncEffect, /classifyComposerExternalSync\(\{/);
  assert.match(
    syncEffect,
    /\}, \[\s*activeSyncGeneration,\s*attachmentSignature,\s*cancelFileTransfers,\s*commandSignature,\s*editor,\s*externalSignature,\s*hasExternalContent,\s*normalizedValue,\s*publishSelection,\s*syncRevision,\s*\]\);/s,
  );
  assert.doesNotMatch(syncEffect, /\[attachmentSignature, attachments/);
  assert.doesNotMatch(syncEffect, /commandSignature, commands/);
});

run('composer document replacement never removes the last root before inserting recovery content', () => {
  const composer = source('components/RichComposer.jsx');
  const replaceStart = composer.indexOf('function replaceEditorDocument');
  const replaceEnd = composer.indexOf('function deleteAdjacentTag', replaceStart);
  const replacement = composer.slice(replaceStart, replaceEnd);
  const insertIndex = replacement.indexOf('Transforms.insertNodes(editor, replacementDocument');
  const removeIndex = replacement.indexOf('Transforms.removeNodes(editor');

  assert.ok(replaceStart >= 0);
  assert.ok(replaceEnd > replaceStart);
  assert.ok(insertIndex >= 0);
  assert.ok(removeIndex > insertIndex);
  assert.match(composer, /function legalComposerDocument\(document\)/);
  assert.match(composer, /function ensureLegalEditorDocument\(editor/);
  assert.match(composer, /editor\.children = fallbackDocument/);
  assert.match(replacement, /return replaced/);
});

run('file resources feed Slate while active references determine image previews and send gating', () => {
  const inputBar = source('components/InputBar.jsx');
  const composer = source('components/RichComposer.jsx');
  const imagePreviewIndex = inputBar.indexOf('data-composer-image-preview="true"');
  const editorIndex = inputBar.indexOf('<RichComposer');
  const footerIndex = inputBar.indexOf('<ComposerSessionControls', editorIndex);
  assert.ok(imagePreviewIndex >= 0 && imagePreviewIndex < editorIndex);
  assert.ok(footerIndex > editorIndex);
  assert.match(inputBar, /composerContentAttachments\(composerContent, attachmentItems\)/);
  assert.match(inputBar, /activeAttachmentItems\.filter\(isComposerThumbnailAttachment\)/);
  assert.match(inputBar, /const hasExtras = activeAttachmentItems\.length > 0/);
  assert.match(inputBar.slice(editorIndex, footerIndex), /attachments=\{editorAttachmentItems\}/);
  assert.match(inputBar.slice(editorIndex, footerIndex), /composerContent=\{editorContent\}/);
  assert.match(composer, /data-composer-inline-tag="attachment"/);
  assert.match(composer, /seenAttachmentKeysRef/);
  assert.match(composer, /Transforms\.setNodes\(editor, metadata, \{ at: path \}\)/);
  assert.match(composer, /removeAttachmentReference\(editor, null, attachmentPath\)/);
  assert.doesNotMatch(composer, /ComposerSessionControls|AttachmentStrip|ComposerSelectionCard/);
});

run('image previews retain image rendering, file-link metadata, and existing transfer entrypoints', () => {
  const inputBar = source('components/InputBar.jsx');
  const composer = source('components/RichComposer.jsx');
  const previewStart = inputBar.indexOf('{imageAttachments.length > 0');
  const previewEnd = inputBar.indexOf('{(selectionPreview', previewStart);
  const preview = inputBar.slice(previewStart, previewEnd);

  assert.ok(previewStart >= 0);
  assert.ok(previewEnd > previewStart);
  assert.match(preview, /const linkPath = context\.sourcePath \|\| context\.path/);
  assert.match(preview, /<img[\s\S]*src=\{context\.url\}[\s\S]*alt=\{context\.name\}/);
  assert.match(preview, /data-desktop-attachment-id=\{context\.id\}/);
  assert.match(preview, /data-desktop-attachment-name=\{context\.name\}/);
  assert.match(preview, /data-desktop-attachment-url=\{context\.url \|\| undefined\}/);
  assert.match(preview, /data-desktop-attachment-path=\{linkPath \|\| undefined\}/);
  assert.match(preview, /data-desktop-attachment-preview-url=\{context\.url \|\| undefined\}/);
  assert.match(preview, /data-desktop-attachment-mime-type=\{mimeType \|\| undefined\}/);
  assert.match(preview, /data-desktop-attachment-kind="image"/);
  assert.match(preview, /data-desktop-attachment-mutable="true"/);
  assert.match(preview, /setAttachmentPreview\(\{ src: context\.url, alt: context\.name \}\)/);
  assert.match(preview, /removeAttachment\(context\.key\)/);
  assert.match(composer, /data-desktop-attachment-id=\{`composer:\$\{attachmentKey\}`\}/);
  assert.match(composer, /data-desktop-attachment-preview-url=\{element\?\.url \|\| undefined\}/);
  assert.match(composer, /onDoubleClick=\{previewable \? \(\) => onPreviewAttachment\?\.\(element\) : undefined\}/);
  assert.match(inputBar, /onPreviewAttachment=\{previewComposerAttachment\}/);
  assert.match(inputBar, /onPasteFilesystemItems=\{handleFilesystemPaste\}/);
  assert.match(inputBar, /postWindowsNativeFilesystemDrop\(event\.dataTransfer\)/);
  assert.match(
    inputBar,
    /acceptFileIntake\(\{ source: 'drop', paths \}\)/,
  );
});

run('desktop context-menu paste captures Slate selection and bridges before DOM fallback', () => {
  const menu = source('components/DesktopContextMenu.jsx');
  const insertStart = menu.indexOf('function insertTextIntoEditable');
  const insertEnd = menu.indexOf('async function copySelectionFromTarget', insertStart);
  const insertBody = menu.slice(insertStart, insertEnd);

  assert.match(menu, /captureRichComposerContextSelection\(editableTarget\)/);
  assert.match(menu, /openWithSwitchGap\(\{[\s\S]*richComposerSelection,[\s\S]*\}\);/);
  assert.match(menu, /pasteIntoTarget\(target, rememberedRichComposerSelection\)/);
  assert.match(menu, /pasteRichComposerContextClipboard\(editable, richComposerSelection\)/);
  assert.match(menu, /editableTargetFromElement\(target\)/);
  assert.doesNotMatch(menu, /\.isContentEditable/);
  assert.ok(insertBody.indexOf('insertRichComposerContextText') >= 0);
  assert.ok(insertBody.indexOf('richComposerRootFromTarget') >= 0);
  assert.ok(insertBody.indexOf('insertRichComposerContextText') < insertBody.indexOf("document.execCommand('insertText'"));
  assert.ok(insertBody.indexOf('richComposerRootFromTarget') < insertBody.indexOf("document.execCommand('insertText'"));
  assert.match(menu, /if \(richComposerRootFromTarget\(editable\)\) return;[\s\S]*document\.execCommand\('paste'\)/);
});

run('rich context paste mutates Slate state while send gating reads the controlled value', () => {
  const composer = source('components/RichComposer.jsx');
  const inputBar = source('components/InputBar.jsx');
  const chatView = source('components/ChatView.jsx');

  assert.match(composer, /addEventListener\(RICH_COMPOSER_CONTEXT_PASTE_EVENT, handleContextPasteAction\)/);
  assert.match(composer, /CAPTURE_SELECTION[\s\S]*capturePasteSelection\(\)/);
  assert.match(composer, /READ_CLIPBOARD[\s\S]*contextClipboardPasteRef\.current\?\.\(detail\.selection\)/);
  assert.match(composer, /const transfer = beginFileTransfer\(captured\);[\s\S]*await readComposerClipboardData\(\);[\s\S]*if \(!transfer\.isActive\(\)\) return;[\s\S]*await handleClipboardPaste/);
  assert.match(composer, /INSERT_TEXT[\s\S]*applyPlainTextPaste\(detail\.text, detail\.selection\)/);
  assert.match(composer, /const applyPlainTextPaste = useCallback\([\s\S]*ensureLegalEditorDocument\(editor\)[\s\S]*Transforms\.select[\s\S]*insertPlainText\(editor, normalizedText\)/);
  assert.doesNotMatch(composer, /execCommand/);
  assert.match(inputBar, /getInputBarActionState\(\{ value: draftValue, disabled, busy, hasExtras, submitting, canRetryLastUserMessage, queuePaused \}\)/);
  assert.match(inputBar, /<RichComposer[\s\S]*onChange=\{handleComposerChange\}/);
  assert.match(chatView, /const handleComposerChange = useCallback\(\(next, content[^)]*\) => \{[\s\S]*setComposerValue\(next, normalized\)/);
});

run('keyboard paste uses native capture, React fallback, and beforeinput through one Slate transaction', () => {
  const composer = source('components/RichComposer.jsx');
  const handleStart = composer.indexOf('const handleClipboardPaste = useCallback');
  const handleEnd = composer.indexOf('const markPasteHandled = useCallback', handleStart);
  const handleBody = composer.slice(handleStart, handleEnd);

  assert.ok(handleStart >= 0);
  assert.ok(handleEnd > handleStart);
  assert.match(handleBody, /plainTextFromClipboardData\(clipboardData\)/);
  assert.match(handleBody, /clipboardHasTextFormat\(clipboardData\)/);
  assert.match(handleBody, /applyPlainTextPaste\(text, capturedSelection\)/);
  assert.match(handleBody, /requestClipboardTextFallback\(capturedSelection\)/);
  assert.match(composer, /ReactEditor\.toSlateRange\(editor, domSelection,[\s\S]*suppressThrow: true/);
  assert.match(composer, /handledPasteEventsRef = useRef\(new WeakSet\(\)\)/);
  assert.match(composer, /addEventListener\('paste', handleNativePaste, true\)/);
  assert.match(composer, /removeEventListener\('paste', handleNativePaste, true\)/);
  assert.match(composer, /event\.inputType !== 'insertFromPaste'/);
  assert.match(composer, /onPaste=\{handlePaste\}/);
  assert.match(composer, /onDOMBeforeInput=\{handleDOMBeforeInput\}/);
  assert.match(composer, /data-ace-rich-composer="true"/);
});

run('paste format and context bridge fallbacks preserve plain state ownership', () => {
  const model = source('lib/richComposerModel.js');
  const bridge = source('lib/richComposerContextPaste.js');

  assert.match(model, /clipboardDataValue\(clipboardData, 'text\/plain'\)/);
  assert.match(model, /clipboardDataValue\(clipboardData, 'text'\)/);
  assert.match(model, /plainTextFromClipboardHtml\(clipboardDataValue\(clipboardData, 'text\/html'\), options\)/);
  assert.match(model, /new DOMParser\(\)\.parseFromString\(html, 'text\/html'\)/);
  assert.match(bridge, /RICH_COMPOSER_ROOT_ATTRIBUTE = 'data-ace-rich-composer'/);
  assert.match(bridge, /\{ detail, bubbles: true \}/);
  assert.match(bridge, /richComposerRootFromTarget/);
});
