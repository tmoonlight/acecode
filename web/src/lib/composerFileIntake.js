import { fileSourcePath } from './composerFileTransfer.js';
import { isRasterImageMimeType } from './composerImagePresentation.js';
import { isDesktopShell } from './desktopShellMode.js';
import { nativePickedFileToFile, parseNativeFilesystemItemsResult } from './desktopContextPicker.js';
import {
  desktopHostOs,
  hasNativeFilesystemClipboard,
  hasNativeFilesystemMaterializer,
  localPathsFromUriList,
  materializeNativeFilesystemPaths,
  readNativeClipboardFilesystemItems,
} from './desktopFilesystemTransfer.js';

const MAX_LOCAL_DATA_BYTES = 25 * 1024 * 1024;

async function storeLocalFiles(files, win) {
  if (typeof win?.aceDesktop_storeContextFiles !== 'function') {
    throw new Error('当前桌面版本无法保存文件，请更新客户端后重试');
  }
  const payload = [];
  for (const file of files) {
    if (file.size > MAX_LOCAL_DATA_BYTES) throw new Error('无本地路径的文件数据不能超过 25 MiB');
    const bytes = new Uint8Array(await file.arrayBuffer());
    let binary = '';
    for (let offset = 0; offset < bytes.length; offset += 8192) {
      binary += String.fromCharCode(...bytes.subarray(offset, offset + 8192));
    }
    payload.push({ name: file.name || 'clipboard-file', data_base64: btoa(binary) });
  }
  return parseNativeFilesystemItemsResult(await win.aceDesktop_storeContextFiles(payload));
}

// Raster images never become path references: the daemon refuses image
// references, only a snapshot reaches the model as an image, and the composer
// and transcript thumbnails need the bytes. The native layer returns the bytes
// (data_base64) for raster images within the attachment limit; a larger or
// unreadable image, and every other file or folder, stays path-native.
function splitNativeItems(items) {
  const paths = [];
  const files = [];
  for (const item of Array.from(items || [])) {
    if (item?.kind === 'file' && typeof item.data_base64 === 'string' && isRasterImageMimeType(item.mime_type)) {
      files.push(nativePickedFileToFile(item));
    } else {
      paths.push(item);
    }
  }
  return { paths, files };
}

// `upload` = attachments only; `paths` = path references, plus `files` when the
// same gesture also carried raster images. An empty native result stays
// `paths` so a filesystem paste is still consumed instead of falling back to text.
function nativeIntake({ paths, files }) {
  if (!paths.length && files.length) return { kind: 'upload', files };
  return files.length ? { kind: 'paths', items: paths, files } : { kind: 'paths', items: paths };
}

// Acquisition differs by host; classification and the returned insertion
// contract do not. Never infer a local path from ordinary clipboard text.
export async function resolveComposerFileIntake({
  source, files = [], paths = [], uriList = '', items = null,
} = {}, win = globalThis.window) {
  if (items) return nativeIntake(splitNativeItems(items));
  const list = Array.from(files || []).filter(Boolean);
  const desktop = isDesktopShell(win) || hasNativeFilesystemMaterializer(win)
    || hasNativeFilesystemClipboard(win);
  if (!desktop) return list.length ? { kind: 'upload', files: list } : { kind: 'none' };

  const localPaths = paths.length ? paths : localPathsFromUriList(uriList, desktopHostOs(win));
  if (localPaths.length) {
    const result = await materializeNativeFilesystemPaths(localPaths, win);
    return nativeIntake(splitNativeItems(result.items));
  }
  if (source === 'paste' && hasNativeFilesystemClipboard(win)) {
    const result = await readNativeClipboardFilesystemItems(win);
    if (result.filesystemItems) return nativeIntake(splitNativeItems(result.items));
  }
  if (!list.length) return { kind: 'none' };

  // A trusted source path always wins for ordinary files. Raster images upload
  // their bytes directly (a source path mark travels with the File). Keep mixed
  // batches in source order without sending known ordinary files as bytes.
  const resolved = [];
  const uploads = [];
  for (const file of list) {
    if (isRasterImageMimeType(file.type)) {
      uploads.push(file);
      continue;
    }
    const path = fileSourcePath(file);
    const result = path
      ? await materializeNativeFilesystemPaths([path], win)
      : await storeLocalFiles([file], win);
    const split = splitNativeItems(result.items);
    resolved.push(...split.paths);
    uploads.push(...split.files);
  }
  return nativeIntake({ paths: resolved, files: uploads });
}
