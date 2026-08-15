import {
  markFileSourcePath,
  markFileSourceReference,
} from './composerFileTransfer.js';

function parseBridgeJson(raw) {
  if (typeof raw !== 'string') return raw;
  const text = raw.trim();
  return text ? JSON.parse(text) : null;
}

export function hasNativeContextPicker(win = globalThis.window) {
  return typeof win?.aceDesktop_pickContextItems === 'function';
}

export function parseNativeContextPickerResult(raw) {
  const body = parseBridgeJson(raw);
  if (!body || typeof body !== 'object') throw new Error('原生选择器返回无效结果');
  if (body.ok === false) throw new Error(String(body.error || '原生选择器不可用'));
  if (body.cancelled) return { cancelled: true, files: [], folder: null };

  const items = Array.isArray(body.items) ? body.items : [];
  const folders = items.filter((item) => item?.kind === 'folder' && item.path);
  const files = items.filter(isValidNativeFileItem);
  if (folders.length > 1 || (folders.length > 0 && files.length > 0)) {
    throw new Error('原生选择器返回了冲突的文件和文件夹结果');
  }
  return {
    cancelled: items.length === 0,
    files,
    folder: folders[0] || null,
  };
}

export function parseNativeFilesystemItemsResult(raw) {
  const body = parseBridgeJson(raw);
  if (!body || typeof body !== 'object') throw new Error('原生文件系统返回无效结果');
  if (body.ok === false) throw new Error(String(body.error || '原生文件系统不可用'));

  const rawItems = Array.isArray(body.items) ? body.items : [];
  const items = rawItems.map((item) => {
    if (item?.kind === 'folder' && typeof item.path === 'string' && item.path) {
      return item;
    }
    if (isValidNativeFileItem(item)) {
      return item;
    }
    throw new Error('原生文件系统条目无效');
  });
  return {
    items,
    files: items.filter((item) => item.kind === 'file'),
    folders: items.filter((item) => item.kind === 'folder'),
    filesystemItems: body.filesystem_items === true || items.length > 0,
  };
}

export function nativePickedFileToFile(item, {
  FileCtor = globalThis.File,
  decodeBase64 = globalThis.atob,
} = {}) {
  if (!isValidNativeFileItem(item)) {
    throw new Error('原生文件数据无效');
  }
  if (item.reference_only === true) {
    const file = {
      name: String(item.name || 'attachment'),
      type: String(item.mime_type || ''),
      size: Number(item.size_bytes),
      lastModified: 0,
    };
    return markFileSourceReference(file, item.path, item.size_bytes);
  }
  if (typeof FileCtor !== 'function' || typeof decodeBase64 !== 'function') {
    throw new Error('当前环境无法还原原生文件');
  }
  const binary = decodeBase64(item.data_base64);
  const bytes = new Uint8Array(binary.length);
  for (let index = 0; index < binary.length; index += 1) {
    bytes[index] = binary.charCodeAt(index);
  }
  const file = new FileCtor([bytes], String(item.name || 'attachment'), {
    type: String(item.mime_type || ''),
    lastModified: Date.now(),
  });
  return markFileSourcePath(file, item.path);
}

function isValidNativeFileItem(item) {
  if (
    item?.kind !== 'file'
    || typeof item.path !== 'string' || !item.path
    || typeof item.name !== 'string' || !item.name
  ) {
    return false;
  }
  if (item.reference_only === true) {
    const size = Number(item.size_bytes);
    return Number.isFinite(size) && size >= 0 && item.data_base64 == null;
  }
  return typeof item.data_base64 === 'string';
}

function normalizedAbsolutePath(value) {
  let path = String(value || '').trim().replaceAll('\\', '/');
  const unc = path.startsWith('//');
  path = path.replace(/\/{2,}/g, '/');
  if (unc) path = `/${path}`;
  if (path.length > 1 && !/^[A-Za-z]:\/$/.test(path)) path = path.replace(/\/+$/, '');
  return path;
}

function isAbsolutePath(path) {
  return /^[A-Za-z]:\//.test(path) || path.startsWith('//') || path.startsWith('/');
}

export function folderReferencePath(cwd, selectedPath) {
  const selected = normalizedAbsolutePath(selectedPath);
  if (!selected || !isAbsolutePath(selected)) throw new Error('文件夹路径无效');

  const root = normalizedAbsolutePath(cwd);
  if (!root || !isAbsolutePath(root)) return selected;

  const windowsPath = /^[A-Za-z]:\//.test(root) || root.startsWith('//');
  const selectedWindowsPath = /^[A-Za-z]:\//.test(selected) || selected.startsWith('//');
  if (windowsPath !== selectedWindowsPath) return selected;

  const comparedRoot = windowsPath ? root.toLowerCase() : root;
  const comparedSelected = windowsPath ? selected.toLowerCase() : selected;
  if (comparedSelected === comparedRoot) return '';
  if (comparedSelected.startsWith(`${comparedRoot}/`)) {
    return selected.slice(root.length).replace(/^\/+/, '');
  }
  return selected;
}

export function nativeFolderReferencePath(cwd, item) {
  if (typeof item?.relative_path === 'string') {
    const relative = item.relative_path.replaceAll('\\', '/').replace(/^\.\//, '').replace(/\/+$/, '');
    if (relative.startsWith('/') || /^[A-Za-z]:/.test(relative) || relative.split('/').includes('..')) {
      throw new Error('原生文件夹相对路径无效');
    }
    return relative;
  }
  return folderReferencePath(cwd, item?.path);
}
