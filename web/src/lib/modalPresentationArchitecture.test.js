import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const srcRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

function source(relativePath) {
  return fs.readFileSync(path.join(srcRoot, relativePath), 'utf8');
}

function between(text, start, end) {
  const startIndex = text.indexOf(start);
  const endIndex = text.indexOf(end, startIndex);
  assert.notEqual(startIndex, -1, `missing start marker: ${start}`);
  assert.notEqual(endIndex, -1, `missing end marker: ${end}`);
  return text.slice(startIndex, endIndex);
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

run('shared Modal appears and closes without staged transitions or delay', () => {
  const modalFile = source('components/Modal.jsx');
  const modal = between(
    modalFile,
    'export function Modal',
    '// 右侧滑出面板',
  );

  assert.doesNotMatch(modal, /requestAnimationFrame/);
  assert.doesNotMatch(modal, /setTimeout/);
  assert.doesNotMatch(modal, /transition-/);
  assert.doesNotMatch(modal, /duration-/);
  assert.doesNotMatch(modal, /\bscale-/);
  assert.doesNotMatch(modal, /\btranslate-/);
  assert.match(modal, /const handleClose = \(\) => onClose\?\.\(\);/);
  assert.match(modal, /backgroundColor: 'rgba\(0, 0, 0, 0\.35\)'/);
});

run('archived deletion and unsaved-preview confirmations share Modal', () => {
  const settingsFile = source('components/SettingsPage.jsx');
  const archived = between(
    settingsFile,
    'function SectionArchived()',
    '// ─── 使用情况',
  );
  const chatView = source('components/ChatView.jsx');

  assert.match(archived, /<Modal onClose=\{\(\) => setPurgeConfirmation\(null\)\}/);
  assert.match(archived, />\s*彻底删除\s*</);
  assert.doesNotMatch(archived, /window\.confirm|window\.alert/);
  assert.match(chatView, /<Modal[\s\S]*if \(!previewCloseConfirm\.saving\) setPreviewCloseConfirm\(null\)/);
  assert.match(chatView, />保存文件后关闭？</);
  assert.match(chatView, /previewTabsWithUnsavedDrafts\(affected\)/);
});

run('loop delete and desktop context-menu confirmations share Modal', () => {
  const loopPage = source('components/LoopPage.jsx');
  const contextMenu = source('components/DesktopContextMenu.jsx');

  assert.match(loopPage, /<Modal onClose=\{\(\) => setDeleteConfirm\(null\)\}/);
  assert.match(loopPage, />\s*删除循环\s*</);
  assert.doesNotMatch(loopPage, /window\.confirm|window\.alert/);
  assert.match(contextMenu, /<Modal onClose=\{\(\) => setPendingConfirm\(null\)\}/);
  assert.match(contextMenu, /pendingConfirm\.action\.confirm/);
  assert.doesNotMatch(contextMenu, /window\.confirm|window\.alert/);
});

run('update dialog keeps release history bounded and renders notes as plain text', () => {
  const updateDialog = source('components/UpdateDialog.jsx');

  assert.match(updateDialog, /updateDialogMode\(job, updateStatus\)/);
  assert.match(updateDialog, /aria-label="版本更新记录"/);
  assert.match(updateDialog, /max-h-64 overflow-y-auto/);
  assert.match(updateDialog, /whitespace-pre-wrap break-words/);
  assert.match(updateDialog, /mode === 'up_to_date'/);
  assert.doesNotMatch(updateDialog, /dangerouslySetInnerHTML/);
});

run('update progress is monotonic, accessible, striped, and reduced-motion safe', () => {
  const updateDialog = source('components/UpdateDialog.jsx');
  const styles = source('styles/globals.css');

  assert.match(updateDialog, /nondecreasingUpdateProgress/);
  assert.match(updateDialog, /key=\{job\?\.job_id \|\| 'starting'\}/);
  assert.match(updateDialog, /role="progressbar"/);
  assert.match(updateDialog, /aria-label="ACECode 升级"/);
  assert.match(updateDialog, /aria-valuenow=\{renderedProgress\}/);
  assert.match(updateDialog, /ace-update-progress-fill/);
  assert.match(styles, /@keyframes ace-update-progress-stripes/);
  assert.match(styles, /\.ace-update-progress-fill \{[\s\S]*transition: width 260ms/);
  assert.match(
    styles,
    /@media \(prefers-reduced-motion: reduce\) \{[\s\S]*\.ace-update-progress-fill \{[\s\S]*animation: none;[\s\S]*transition: none;/,
  );
});

run('update UI exposes safe cancellation and a dual-foreground topbar progress pill', () => {
  const app = source('App.jsx');
  const updateDialog = source('components/UpdateDialog.jsx');
  const topBar = source('components/TopBar.jsx');

  assert.match(app, /api\.cancelUpdate\(updateJob\.job_id\)/);
  assert.match(updateDialog, /取消升级/);
  assert.match(updateDialog, /job\?\.can_cancel === false/);
  assert.match(updateDialog, /mode === 'cancelled'/);
  assert.match(topBar, /updateProgress = 0/);
  assert.match(topBar, /100 - boundedUpdateProgress/);
  assert.match(topBar, /absolute inset-0 flex items-center justify-center text-accent/);
  assert.match(topBar, /absolute inset-0 flex items-center justify-center text-white/);
  assert.match(topBar, /relative h-7 min-w-\[44px\] overflow-hidden px-3 rounded-full/);
});
