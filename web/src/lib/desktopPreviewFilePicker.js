function parseBridgeJson(raw) {
  if (typeof raw !== 'string') return raw;
  const text = raw.trim();
  return text ? JSON.parse(text) : null;
}

function absolutePath(value) {
  if (typeof value !== 'string') return '';
  const path = value.trim();
  if (/^[A-Za-z]:[\\/]/u.test(path) || /^\\\\/u.test(path) || path.startsWith('/')) {
    return path;
  }
  return '';
}

export function hasNativePreviewFilePicker(win = globalThis.window) {
  return typeof win?.aceDesktop_pickPreviewFile === 'function';
}

export function parseNativePreviewFilePickerResult(raw) {
  const body = parseBridgeJson(raw);
  if (!body || typeof body !== 'object') throw new Error('原生选择器返回无效结果');
  if (body.ok === false) throw new Error(String(body.error || '原生选择器不可用'));
  if (body.cancelled === true) return { cancelled: true, path: '' };

  const path = absolutePath(body.path);
  if (!path) throw new Error('无法获取文件路径');
  return { cancelled: false, path };
}

export async function pickNativePreviewFile(cwd = '', win = globalThis.window) {
  if (!hasNativePreviewFilePicker(win)) throw new Error('原生选择器不可用');
  const raw = await win.aceDesktop_pickPreviewFile({ cwd: String(cwd || '') });
  return parseNativePreviewFilePickerResult(raw);
}
