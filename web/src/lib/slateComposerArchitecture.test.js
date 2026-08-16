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

run('command, path, session, and attachment tags are Slate inline void elements with fixed order', () => {
  const composer = source('components/RichComposer.jsx');
  assert.match(composer, /editor\.isInline = \(element\) => \(\s+isComposerInlineTag\(element\) \? true : isInline\(element\)/s);
  assert.match(composer, /editor\.isVoid = \(element\) => \(\s+isComposerInlineTag\(element\) \? true : isVoid\(element\)/s);
  assert.equal((composer.match(/draggable=\{false\}/g) || []).length, 4);
  assert.match(composer, /types\.includes\('application\/x-slate-fragment'\)/);
});

run('composer command tag reuses the sent-message badge without a visible slash', () => {
  const composer = source('components/RichComposer.jsx');
  const message = source('components/Message.jsx');
  const styles = source('styles/globals.css');

  assert.match(composer, /replace\(\/\^\\\/\+\/, ''\)/);
  assert.match(composer, /className="ace-cmd-token ace-slate-inline-tag"/);
  assert.match(composer, /<CommandGlyph[^>]*className="ace-cmd-token-glyph"/s);
  assert.match(composer, /className="ace-cmd-token-name">\{displayName\}/);
  assert.match(message, /className="ace-cmd-token"/);
  assert.match(styles, /\.ace-cmd-token\s*\{/);
  assert.doesNotMatch(styles, /\.ace-rich-command-token\s*\{/);
});

run('path tags keep canonical text while using the compact badge surface', () => {
  const composer = source('components/RichComposer.jsx');
  assert.match(composer, /data-composer-inline-tag="path"/);
  assert.match(composer, /className="ace-cmd-token ace-slate-inline-tag ace-slate-path-tag"/);
  assert.match(composer, /element\?\.directory\s+\? <VsIcon name="folder"/s);
  assert.match(composer, /<FileTypeIcon path=\{path\} size=\{12\}/);
});

run('session tags keep stable identity while reusing the compact badge surface', () => {
  const composer = source('components/RichComposer.jsx');
  assert.match(composer, /data-composer-inline-tag="session"/);
  assert.match(composer, /className="ace-cmd-token ace-slate-inline-tag ace-slate-session-tag"/);
  assert.match(composer, /<VsIcon name="newSession" size=\{12\}/);
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
  assert.match(composer, /try \{\s*focusEditor\(\);\s*\} catch \{/s);
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

run('composer external sync is composition-safe, generation-aware, and semantic', () => {
  const inputBar = source('components/InputBar.jsx');
  const composer = source('components/RichComposer.jsx');
  const decisionIndex = composer.indexOf('const decision = classifyComposerExternalSync');
  const effectStart = composer.lastIndexOf('useEffect(() => {', decisionIndex);
  const effectEnd = composer.indexOf('const handleValueChange', decisionIndex);
  const syncEffect = composer.slice(effectStart, effectEnd);

  assert.ok(effectStart >= 0);
  assert.ok(effectEnd > effectStart);
  assert.match(inputBar, /syncKey=\{currentSessionId\}/);
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
    /\}, \[\s*activeSyncGeneration,\s*attachmentSignature,\s*commandSignature,\s*editor,\s*normalizedValue,\s*publishSelection,\s*syncRevision,\s*\]\);/s,
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

run('ordinary files render inside Slate while images use linked previews outside the editor', () => {
  const inputBar = source('components/InputBar.jsx');
  const composer = source('components/RichComposer.jsx');
  const imagePreviewIndex = inputBar.indexOf('data-composer-image-preview="true"');
  const editorIndex = inputBar.indexOf('<RichComposer');
  const footerIndex = inputBar.indexOf('<ComposerSessionControls', editorIndex);

  assert.ok(imagePreviewIndex >= 0);
  assert.ok(editorIndex >= 0);
  assert.ok(imagePreviewIndex < editorIndex);
  assert.ok(footerIndex > editorIndex);
  assert.match(inputBar.slice(0, editorIndex), /selectionContextItems\.map/);
  assert.match(inputBar, /const \{ imageAttachments, fileAttachments \} = useMemo\(\(\) => \(\{/);
  assert.match(inputBar, /imageAttachments: attachmentItems\.filter\(isComposerImageAttachment\)/);
  assert.match(inputBar, /fileAttachments: attachmentItems\.filter\(\(item\) => !isComposerImageAttachment\(item\)\)/);
  assert.match(inputBar, /\}\), \[attachmentItems\]\);/);
  assert.match(inputBar.slice(0, editorIndex), /imageAttachments\.map/);
  assert.match(inputBar.slice(editorIndex, footerIndex), /attachments=\{fileAttachments\}/);
  assert.doesNotMatch(inputBar.slice(editorIndex, footerIndex), /attachments=\{attachmentItems\}/);
  assert.match(composer, /data-composer-inline-tag="attachment"/);
  assert.match(composer, /contentEditable=\{false\}[\s\S]*draggable=\{false\}[\s\S]*ace-slate-attachment-tag/);
  assert.match(composer, /composerAdjacentAttachmentKey\([\s\S]*onRemoveAttachment\(attachmentKey\)/);
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
  assert.match(preview, /onRemoveAttachment\?\.\(context\.key\)/);
  assert.match(composer, /data-desktop-attachment-id=\{`composer:\$\{attachmentKey\}`\}/);
  assert.match(composer, /data-desktop-attachment-preview-url=\{element\?\.url \|\| undefined\}/);
  assert.match(composer, /onClick=\{previewable \? \(\) => onPreviewAttachment\?\.\(element\) : undefined\}/);
  assert.match(inputBar, /onPreviewAttachment=\{previewComposerAttachment\}/);
  assert.match(inputBar, /onPasteFiles=\{addMediaFiles\}/);
  assert.match(inputBar, /postWindowsNativeFilesystemDrop\(event\.dataTransfer\)/);
  assert.match(
    inputBar,
    /addMaterializedPaths\(paths, savedCursor, \{ requestNativeFocus: false \}\)/,
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
  assert.ok(insertBody.indexOf('insertRichComposerContextText') >= 0);
  assert.ok(insertBody.indexOf('insertRichComposerContextText') < insertBody.indexOf("document.execCommand('insertText'"));
});

run('rich context paste mutates Slate state while send gating reads the controlled value', () => {
  const composer = source('components/RichComposer.jsx');
  const inputBar = source('components/InputBar.jsx');
  const chatView = source('components/ChatView.jsx');

  assert.match(composer, /addEventListener\(RICH_COMPOSER_CONTEXT_PASTE_EVENT, handleContextPasteAction\)/);
  assert.match(composer, /CAPTURE_SELECTION[\s\S]*currentPlainSelection\(editor\.children, editor\.selection\)/);
  assert.match(composer, /INSERT_TEXT[\s\S]*composerSelectionFromPlainTextRange[\s\S]*insertPlainText\(editor, detail\.text\)/);
  assert.doesNotMatch(composer, /execCommand/);
  assert.match(inputBar, /getInputBarActionState\(\{ value, disabled, busy, hasExtras \}\)/);
  assert.match(inputBar, /<RichComposer[\s\S]*onChange=\{handleComposerChange\}/);
  assert.match(chatView, /const handleComposerChange = useCallback\(\(next\) => \{[\s\S]*setComposerValue\(next\)/);
});
