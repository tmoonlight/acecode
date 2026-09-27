import { readCppSource } from './cppSourcePaths.testHelper.js';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const srcRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

function source(relativePath) {
  return fs.readFileSync(path.join(srcRoot, relativePath), 'utf8').replace(/\r\n?/g, '\n');
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
    /acceptFileIntake\(\{ source: 'drop', paths \}\)/,
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

run('desktop file drag activates on entry and acceptance without async foreground retries', () => {
  const inputBar = source('components/InputBar.jsx');
  const desktopMain = readCppSource('desktop/main.cpp').replace(/\r\n?/g, '\n');

  assert.match(
    inputBar,
    /if \(dragDepthRef\.current === 0\) requestDesktopFileDragActivation\(\);\s*dragDepthRef\.current \+= 1/,
  );
  const dragOver = inputBar.slice(inputBar.indexOf('const handleDragOver ='), inputBar.indexOf('const handleDragLeave ='));
  assert.doesNotMatch(dragOver, /requestDesktopFileDragActivation/);
  assert.match(inputBar, /acceptFileIntake\(\{ source: 'drop', paths: uriPaths, files \}\)/);
  assert.match(
    inputBar,
    /acceptFileIntake\(\{ source: 'drop', paths \}\)/,
  );
  assert.match(
    desktopMain,
    /host\.bind\("aceDesktop_activateFileDropWindow"[\s\S]{0,700}host\.set_visible\(true\)/,
  );
  const bridge = desktopMain.match(
    /host\.bind\("aceDesktop_activateFileDropWindow"[\s\S]*?\n\s*\}\);/,
  )?.[0] || '';
  assert.doesNotMatch(bridge, /bring_window_foreground|activate_notification_window|HWND_TOPMOST/);
  assert.match(desktopMain, /host\.bind\("aceDesktop_focusFileDropWindow"[\s\S]{0,200}host\.focus_after_file_drop\(\)/);
  const webHost = readCppSource('desktop/web_host.cpp').replace(/\r\n?/g, '\n');
  const dropFocus = webHost.slice(webHost.indexOf('bool WebHost::focus_after_file_drop()'), webHost.indexOf('bool WebHost::open_dev_tools()'));
  assert.match(dropFocus, /AttachThreadInput\(current_thread, foreground_thread, TRUE\)/);
  assert.match(dropFocus, /AttachThreadInput\(current_thread, foreground_thread, FALSE\)/);
  assert.match(dropFocus, /MoveFocus\(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC\)/);
  assert.doesNotMatch(dropFocus, /HWND_TOPMOST|activate_notification_window/);
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
  assert.match(chatView, /removeComposerAttachmentReference\(composerContentRef\.current, key\)/);
  const removeFlow = chatView.slice(chatView.indexOf('const removeComposerAttachment ='), chatView.indexOf('const removeComposerContext ='));
  assert.doesNotMatch(removeFlow, /releaseAttachmentReservation|revokeObjectURL|setComposerAttachments/,
    'deleting a reference must retain its resource until the draft is cleared so undo remains usable');
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

run('Desktop native filesystem items become path references before attachment staging', () => {
  const inputBar = source('components/InputBar.jsx');
  assert.match(inputBar, /result\.kind === 'paths'\) transfer\.insertPaths\(result\.items\)/);
  assert.match(inputBar, /onPasteFilesystemItems=\{handleFilesystemPaste\}/);
  assert.match(inputBar, /source: 'picker', items: picked\.folder \? \[picked\.folder\] : picked\.files/);
  assert.doesNotMatch(inputBar, /nativePickedFileToFile/);
});

run('file-tree Add to conversation inserts a path without reading file contents', () => {
  const chatView = source('components/ChatView.jsx');
  const start = chatView.indexOf(
    'if (action !== DESKTOP_CONTEXT_ACTIONS.ADD_FILE_CONTEXT) return;',
  );
  const end = chatView.indexOf(
    'if (action !== DESKTOP_CONTEXT_ACTIONS.ADD_DIRECTORY_CONTEXT) return;',
    start,
  );
  const fileContextFlow = chatView.slice(start, end);

  assert.match(fileContextFlow, /insertPathReference\?\.\(filePath, \{\s*directory: false/);
  assert.doesNotMatch(fileContextFlow, /api\.readFile|createFileContext/);
  assert.doesNotMatch(fileContextFlow, /无法引用二进制文件|文件过大，无法引用/);
});

run('drop overlay uses a themed blur fallback and Slate tags keep symmetric spacing', () => {
  const styles = source('styles/globals.css');

  assert.match(styles, /\.ace-chat-file-drop-overlay\s*\{[\s\S]*pointer-events:\s*none;[\s\S]*background:\s*rgba\(var\(--ace-bg-rgb\), 0\.7\);/);
  assert.match(styles, /@supports \(\(-webkit-backdrop-filter:[\s\S]*\.ace-chat-file-drop-overlay\s*\{[\s\S]*backdrop-filter:\s*blur\(2\.5px\) saturate\(0\.72\);/);
  assert.match(styles, /\.ace-chat-file-drop-prompt\s*\{[\s\S]*background:\s*rgba\(var\(--ace-surface-rgb\), 0\.94\);/);
  assert.match(styles, /@media \(prefers-reduced-motion: reduce\) \{\s*\.ace-chat-file-drop-overlay\s*\{\s*animation:\s*none;/);
  assert.match(styles, /\.ace-slate-inline-tag > \.ace-cmd-token\s*\{[^}]*margin:\s*1px 0;[^}]*padding:\s*0 5px;/);
  assert.match(styles, /\.ace-cmd-token\s*\{[\s\S]*margin-right:\s*1px;/);
});
