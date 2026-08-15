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

run('attachments render inside Slate while contexts and footer controls stay outside', () => {
  const inputBar = source('components/InputBar.jsx');
  const composer = source('components/RichComposer.jsx');
  const editorIndex = inputBar.indexOf('<RichComposer');
  const footerIndex = inputBar.indexOf('<ComposerSessionControls', editorIndex);

  assert.ok(editorIndex >= 0);
  assert.ok(footerIndex > editorIndex);
  assert.match(inputBar.slice(0, editorIndex), /selectionContextItems\.map/);
  assert.match(inputBar.slice(editorIndex, footerIndex), /attachments=\{attachmentItems\}/);
  assert.doesNotMatch(inputBar, /imageAttachments\.map|fileAttachments\.map/);
  assert.match(composer, /data-composer-inline-tag="attachment"/);
  assert.match(composer, /contentEditable=\{false\}[\s\S]*draggable=\{false\}[\s\S]*ace-slate-attachment-tag/);
  assert.match(composer, /composerAdjacentAttachmentKey\([\s\S]*onRemoveAttachment\(attachmentKey\)/);
  assert.doesNotMatch(composer, /ComposerSessionControls|AttachmentStrip|ComposerSelectionCard/);
});

run('attachment tags retain preview, context-menu metadata, and existing transfer entrypoints', () => {
  const inputBar = source('components/InputBar.jsx');
  const composer = source('components/RichComposer.jsx');

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
