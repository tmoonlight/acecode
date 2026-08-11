export const WINDOWS_NATIVE_FILESYSTEM_DROP_MESSAGE =
  'acecode:native-filesystem-drop:v1';

function listToArray(list) {
  if (!list) return [];
  try {
    return Array.from(list).filter(Boolean);
  } catch {
    const out = [];
    const length = Number(list.length || 0);
    for (let index = 0; index < length; index += 1) {
      const item = typeof list.item === 'function' ? list.item(index) : list[index];
      if (item) out.push(item);
    }
    return out;
  }
}

// Directory drops are exposed as file-kind DataTransferItems by Chromium.
// Prefer that ordered list over FileList so folders and mixed batches are not
// silently omitted; getAsFile() still carries a native WebView2 path even when
// the object represents a directory and has no uploadable bytes.
export function nativeFilesystemDropObjects(dataTransfer) {
  const itemFiles = listToArray(dataTransfer?.items)
    .filter((item) => item?.kind === 'file' && typeof item.getAsFile === 'function')
    .map((item) => {
      try { return item.getAsFile() || null; }
      catch { return null; }
    })
    .filter(Boolean);
  if (itemFiles.length > 0) return itemFiles;
  return listToArray(dataTransfer?.files);
}

export function postWindowsNativeFilesystemDrop(
  dataTransfer,
  win = globalThis.window,
) {
  const webview = win?.chrome?.webview;
  const post = webview?.postMessageWithAdditionalObjects;
  if (typeof post !== 'function') return false;

  const objects = nativeFilesystemDropObjects(dataTransfer);
  if (objects.length === 0) return false;

  try {
    post.call(webview, WINDOWS_NATIVE_FILESYSTEM_DROP_MESSAGE, objects);
    return true;
  } catch {
    return false;
  }
}
