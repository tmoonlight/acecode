const PASTE_IMAGE_NAMES = {
  'image/png': 'pasted-image.png',
  'image/jpeg': 'pasted-image.jpg',
  'image/jpg': 'pasted-image.jpg',
  'image/webp': 'pasted-image.webp',
  'image/gif': 'pasted-image.gif',
  'image/bmp': 'pasted-image.bmp',
  'image/tiff': 'pasted-image.tiff',
};

const DESKTOP_SOURCE_PATH = Symbol.for('acecode.desktopSourcePath');

function normalizedAbsoluteSourcePath(value) {
  let path = String(value || '').trim().replaceAll('\\', '/');
  if (!path) return '';
  const unc = path.startsWith('//');
  path = path.replace(/\/{2,}/g, '/');
  if (unc) path = `/${path}`;
  if (!/^(?:[A-Za-z]:\/|\/)/.test(path)) return '';
  return path;
}

export function markFileSourcePath(file, sourcePath) {
  const path = normalizedAbsoluteSourcePath(sourcePath);
  if (!file || !path) return file;
  try {
    Object.defineProperty(file, DESKTOP_SOURCE_PATH, {
      configurable: false,
      enumerable: false,
      writable: false,
      value: path,
    });
  } catch {
    // Some host File implementations are non-extensible. The attachment can
    // still upload; only the optional source-path enrichment is unavailable.
  }
  return file;
}

export function fileSourcePath(file) {
  return normalizedAbsoluteSourcePath(file?.[DESKTOP_SOURCE_PATH]);
}

function listToArray(list) {
  if (!list) return [];
  try {
    return Array.from(list).filter(Boolean);
  } catch {
    const length = Number(list.length || 0);
    const out = [];
    for (let i = 0; i < length; i += 1) {
      const item = typeof list.item === 'function' ? list.item(i) : list[i];
      if (item) out.push(item);
    }
    return out;
  }
}

function fallbackPasteName(file, index) {
  const type = String(file?.type || '').toLowerCase();
  if (PASTE_IMAGE_NAMES[type]) return PASTE_IMAGE_NAMES[type];
  const suffix = index > 0 ? `-${index + 1}` : '';
  return `pasted-attachment${suffix}`;
}

function ensurePasteFileName(file, index) {
  if (!file || String(file.name || '').trim()) return file;
  if (typeof File !== 'function') return file;

  try {
    return new File([file], fallbackPasteName(file, index), {
      type: file.type || 'application/octet-stream',
      lastModified: file.lastModified || Date.now(),
    });
  } catch {
    return file;
  }
}

function normalizeTransferFiles(files, source) {
  return files
    .filter(Boolean)
    .map((file, index) => source === 'paste' ? ensurePasteFileName(file, index) : file);
}

function itemIsFileLike(item) {
  return item?.kind === 'file' || (
    typeof item?.getAsFile === 'function' &&
    String(item?.type || '').toLowerCase().startsWith('image/')
  );
}

export function hasFileTransfer(dataTransfer) {
  if (!dataTransfer) return false;
  if (listToArray(dataTransfer.files).length > 0) return true;
  return listToArray(dataTransfer.items).some(itemIsFileLike);
}

export function filesFromTransfer(dataTransfer, { source = 'drop' } = {}) {
  if (!dataTransfer) return [];

  const files = listToArray(dataTransfer.files);
  if (files.length > 0) return normalizeTransferFiles(files, source);

  const itemFiles = listToArray(dataTransfer.items)
    .filter(itemIsFileLike)
    .map((item) => {
      try {
        return item.getAsFile?.() || null;
      } catch {
        return null;
      }
    });
  return normalizeTransferFiles(itemFiles, source);
}

export function filesFromClipboardEvent(event) {
  return filesFromTransfer(event?.clipboardData || event?.nativeEvent?.clipboardData, { source: 'paste' });
}
