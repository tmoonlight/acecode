import { fileUriToLocalPath, parseUriList } from './consoleDropPaths.js';
import { parseNativeFilesystemItemsResult } from './desktopContextPicker.js';
import { insertPathReferenceAtCaret } from './pathReference.js';

export function desktopHostOs(win = globalThis.window) {
  const os = win?.__ACECODE_OS__;
  if (os === 'windows' || os === 'macos' || os === 'linux') return os;
  const ua = win?.navigator?.userAgent || globalThis.navigator?.userAgent || '';
  if (/Windows/i.test(ua)) return 'windows';
  if (/Mac/i.test(ua)) return 'macos';
  return 'linux';
}

export function nativeFileDropEnabled(win = globalThis.window) {
  return win?.__ACECODE_NATIVE_FILE_DROP__ === true;
}

export function hasNativeFilesystemMaterializer(win = globalThis.window) {
  return typeof win?.aceDesktop_materializeContextItems === 'function';
}

export function hasNativeFilesystemClipboard(win = globalThis.window) {
  return typeof win?.aceDesktop_readClipboardContextItems === 'function';
}

export function localPathsFromUriList(text, os = desktopHostOs()) {
  return parseUriList(text)
    .map((item) => fileUriToLocalPath(item, os))
    .filter(Boolean);
}

export function localPathsFromDropPayload(items, os = desktopHostOs()) {
  return Array.from(items || [])
    .map((item) => fileUriToLocalPath(item, os))
    .filter(Boolean);
}

export function uriListFromTransfer(dataTransfer) {
  if (!dataTransfer || typeof dataTransfer.getData !== 'function') return '';
  try {
    return dataTransfer.getData('text/uri-list') || '';
  } catch {
    return '';
  }
}

export async function materializeNativeFilesystemPaths(
  paths,
  win = globalThis.window,
) {
  if (!hasNativeFilesystemMaterializer(win)) {
    throw new Error('原生文件系统桥接不可用');
  }
  const raw = await win.aceDesktop_materializeContextItems(Array.from(paths || []));
  return parseNativeFilesystemItemsResult(raw);
}

export async function readNativeClipboardFilesystemItems(win = globalThis.window) {
  if (!hasNativeFilesystemClipboard(win)) {
    return { items: [], files: [], folders: [], filesystemItems: false };
  }
  const raw = await win.aceDesktop_readClipboardContextItems();
  return parseNativeFilesystemItemsResult(raw);
}

export function insertAbsoluteFolderReferences(text, caret, folderItems) {
  let next = {
    text: String(text || ''),
    cursor: Number.isFinite(caret) ? caret : String(text || '').length,
  };
  for (const item of Array.from(folderItems || [])) {
    if (item?.kind !== 'folder' || !item.path) continue;
    next = insertPathReferenceAtCaret(next.text, next.cursor, item.path);
  }
  return next;
}
