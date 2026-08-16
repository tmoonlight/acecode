import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const srcRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const repoRoot = path.resolve(srcRoot, '..', '..');

function source(relativePath) {
  return fs.readFileSync(path.join(srcRoot, relativePath), 'utf8').replace(/\r\n?/g, '\n');
}

function repoSource(relativePath) {
  return fs.readFileSync(path.join(repoRoot, relativePath), 'utf8').replace(/\r\n?/g, '\n');
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

run('home and session chat columns delegate file drops to the single InputBar pipeline', () => {
  const chatView = source('components/ChatView.jsx');
  const inputBar = source('components/InputBar.jsx');

  assert.equal((chatView.match(/data-chat-file-drop-scope="true"/g) || []).length, 2);
  assert.match(chatView, /handleChatFileDragEnter[\s\S]*inputRef\.current\?\.handleFileDragEnter\?\.\(event\)/);
  assert.match(chatView, /handleChatFileDragOver[\s\S]*inputRef\.current\?\.handleFileDragOver\?\.\(event\)/);
  assert.match(chatView, /handleChatFileDragLeave[\s\S]*inputRef\.current\?\.handleFileDragLeave\?\.\(event\)/);
  assert.match(chatView, /handleChatFileDrop[\s\S]*inputRef\.current\?\.handleFileDrop\?\.\(event\)/);
  assert.equal((chatView.match(/fileDropManagedExternally/g) || []).length, 2);
  assert.equal((chatView.match(/onFileDragActiveChange=\{setChatFileDropActive\}/g) || []).length, 2);
  assert.match(chatView, /function chatFileDropEventIsInsideScope\(event\)[\s\S]*scope\.contains\(target\)/);
  assert.equal((chatView.match(/if \(!chatFileDropEventIsInsideScope\(event\)\) return;/g) || []).length, 4);

  assert.match(inputBar, /handleFileDragEnter:\s*handleDragEnter/);
  assert.match(inputBar, /handleFileDragOver:\s*handleDragOver/);
  assert.match(inputBar, /handleFileDragLeave:\s*handleDragLeave/);
  assert.match(inputBar, /handleFileDrop:\s*handleDrop/);
  assert.match(inputBar, /onDrop=\{fileDropManagedExternally \? undefined : handleDrop\}/);
  assert.match(inputBar, /postWindowsNativeFilesystemDrop\(event\.dataTransfer\)/);
  assert.match(
    inputBar,
    /addMaterializedPaths\(paths, savedCursor, \{ requestNativeFocus: false \}\)/,
  );
});

run('chat-wide drop feedback remains active across children and always clears', () => {
  const chatView = source('components/ChatView.jsx');
  const inputBar = source('components/InputBar.jsx');

  assert.match(chatView, /function ChatFileDropOverlay\(\{ active \}\)[\s\S]*if \(!active\) return null/);
  assert.match(chatView, /data-chat-file-drop-overlay="true"/);
  assert.match(chatView, /role="status"[\s\S]*tr\('fileDrop\.releaseToAdd'\)/);
  assert.equal((chatView.match(/<ChatFileDropOverlay active=\{chatFileDropActive\} \/>/g) || []).length, 2);
  assert.match(inputBar, /dragDepthRef\.current \+= 1/);
  assert.match(inputBar, /dragDepthRef\.current = Math\.max\(0, dragDepthRef\.current - 1\)/);
  assert.match(inputBar, /window\.addEventListener\('dragend', resetDragState\)/);
  assert.match(inputBar, /window\.addEventListener\('drop', resetDragState\)/);
  assert.match(inputBar, /window\.addEventListener\('blur', resetDragState\)/);
  assert.match(inputBar, /onFileDragActiveChange\?\.\(next\)/);
});

run('desktop file drag activates once on entry and never re-foregrounds after drop', () => {
  const inputBar = source('components/InputBar.jsx');
  const desktopMain = repoSource('src/desktop/main.cpp');

  assert.match(
    inputBar,
    /if \(dragDepthRef\.current === 0\) requestDesktopFileDragActivation\(\);\s*dragDepthRef\.current \+= 1/,
  );
  assert.doesNotMatch(inputBar, /handleDragOver[\s\S]*requestDesktopFileDragActivation/);
  assert.match(inputBar, /addMediaFiles\(files, \{ requestNativeFocus: false \}\)/);
  assert.match(
    inputBar,
    /addMaterializedPaths\(paths, savedCursor, \{ requestNativeFocus: false \}\)/,
  );
  assert.match(
    desktopMain,
    /host\.bind\("aceDesktop_activateFileDropWindow"[\s\S]{0,700}host\.set_visible\(true\)/,
  );
  const bridge = desktopMain.match(
    /host\.bind\("aceDesktop_activateFileDropWindow"[\s\S]*?\n\s*\}\);/,
  )?.[0] || '';
  assert.doesNotMatch(bridge, /bring_window_foreground|activate_notification_window|HWND_TOPMOST/);
});

run('ChatView stages home attachments without creating or navigating a session', () => {
  const chatView = source('components/ChatView.jsx');
  const handleStart = chatView.indexOf('const handleMediaFiles = useCallback((files) => {');
  const handleEnd = chatView.indexOf('\n\n  const removeComposerAttachment', handleStart);
  const handleFlow = chatView.slice(handleStart, handleEnd);
  const submitStart = chatView.indexOf('const submit = useCallback((text) => {');
  const submitEnd = chatView.indexOf('\n\n  const drainQueuedInput', submitStart);
  const submitFlow = chatView.slice(submitStart, submitEnd);

  assert.match(
    handleFlow,
    /const reservedFiles = reserveUniqueComposerFiles\(files\);\s*if \(reservedFiles\.length === 0\) return;/,
  );
  assert.match(handleFlow, /stageMediaFiles\(reservedFiles\);\s*if \(!sid\) return;/);
  assert.match(handleFlow, /persistMediaFilesToSession\(sid, reservedFiles\)/);
  assert.doesNotMatch(handleFlow, /createHomeComposerSession|createWorkspaceSession|createSession\(/);
  assert.match(chatView, /attachment_identity: identity,\s*pending_upload: true,\s*uploading: false/);
  assert.match(submitFlow, /payloadHasExtras\(payload\) \|\| hasPendingAttachments/);
  assert.match(submitFlow, /persistMediaFilesToSession\(id, pendingAttachmentFiles\)/);
  assert.match(submitFlow, /payloadWithAttachmentIds\(payload, materializedAttachments\)/);
  assert.ok(
    submitFlow.indexOf('createHomeComposerSession')
      < submitFlow.indexOf('persistMediaFilesToSession(id, pendingAttachmentFiles)'),
    'home submit must create the session before materializing staged files',
  );
  assert.ok(
    submitFlow.indexOf('persistMediaFilesToSession(id, pendingAttachmentFiles)')
      < submitFlow.indexOf('sendInputOrBuiltin(id, sendPayload)'),
    'home submit must materialize staged files before sending',
  );
  assert.match(chatView, /pending_upload: true,[\s\S]*upload_error: error\?\.message/);
  assert.match(chatView, /if \(removed\?\.local_id\) releaseAttachmentReservation\(removed\.local_id\)/);
  assert.match(chatView, /const clearComposerExtras = useCallback\(\(\) => \{\s*clearAttachmentReservations\(\)/);
});

run('Desktop ordinary-file references bypass image normalization and Base64 upload', () => {
  const chatView = source('components/ChatView.jsx');
  const persistence = chatView.match(
    /const persistAttachment = sourceReference[\s\S]*?await Promise\.resolve\(persistAttachment\)/,
  )?.[0] || '';

  assert.match(persistence, /api\.createSessionAttachmentReference\(targetSid, \{[\s\S]*reference_only: true/);
  assert.match(persistence, /:\s*normalizeImageFile\(file\)[\s\S]*fileToBase64\(uploadFile\)/);
  assert.ok(
    persistence.indexOf('createSessionAttachmentReference') < persistence.indexOf('normalizeImageFile'),
  );
});

run('drop overlay uses a themed blur fallback and Slate tags own their gutter', () => {
  const styles = source('styles/globals.css');

  assert.match(styles, /\.ace-chat-file-drop-overlay\s*\{[\s\S]*pointer-events:\s*none;[\s\S]*background:\s*rgba\(var\(--ace-bg-rgb\), 0\.7\);/);
  assert.match(styles, /@supports \(\(-webkit-backdrop-filter:[\s\S]*\.ace-chat-file-drop-overlay\s*\{[\s\S]*backdrop-filter:\s*blur\(2\.5px\) saturate\(0\.72\);/);
  assert.match(styles, /\.ace-chat-file-drop-prompt\s*\{[\s\S]*background:\s*rgba\(var\(--ace-surface-rgb\), 0\.94\);/);
  assert.match(styles, /@media \(prefers-reduced-motion: reduce\) \{\s*\.ace-chat-file-drop-overlay\s*\{\s*animation:\s*none;/);
  assert.match(styles, /\.ace-cmd-token\.ace-slate-inline-tag\s*\{\s*margin:\s*1px 5px 1px 0;/);
  assert.match(styles, /\.ace-cmd-token\s*\{[\s\S]*margin-right:\s*1px;/);
});
