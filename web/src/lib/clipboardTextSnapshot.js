import { isRasterImageMimeType } from './composerImagePresentation.js';
import { plainTextFromClipboardData, plainTextFromClipboardHtml } from './richComposerModel.js';

function clipboardValue(clipboardData, type) {
  if (!clipboardData || typeof clipboardData.getData !== 'function') return '';
  try {
    return String(clipboardData.getData(type) || '');
  } catch {
    return '';
  }
}

function clipboardHasType(clipboardData, wanted) {
  return Array.from(clipboardData?.types || [])
    .some((type) => String(type || '').toLowerCase() === wanted);
}

// OneNote / Word / Excel / PowerPoint / WPS put a bitmap of the selection on
// the clipboard next to its HTML and plain text, and Chromium exposes that
// bitmap as a pasted image file. A browser "Copy image" also carries HTML
// (only an <img>) and sometimes the image URL as plain text, so the presence
// of text alone cannot decide. The HTML decides: visible text means the image
// is just a rendering of copied text. Without HTML, RTF marks an Office-style
// source.
export function clipboardImagesAreTextSnapshot(clipboardData, files, options) {
  const list = Array.from(files || []);
  if (!list.length || !list.every((file) => isRasterImageMimeType(file?.type))) return false;

  const html = clipboardValue(clipboardData, 'text/html');
  if (html) return plainTextFromClipboardHtml(html, options).trim() !== '';
  if (!clipboardHasType(clipboardData, 'text/rtf')) return false;
  return plainTextFromClipboardData(clipboardData, options).trim() !== '';
}
