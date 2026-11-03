import { api } from './api.js';
import { downloadBlob } from './downloadBlob.js';
import { fileLinkName } from './fileLinkContextMenu.js';

function fileRequest(target) {
  if (!target?.absolutePath) throw new Error('file path unavailable');
  // Absolute files in sessions without a working directory still need a cwd
  // parameter. The preview resolver accepts the file's absolute parent.
  const path = target.absolutePath;
  const cwd = target.cwd || path.replace(/[^\\/]+$/, '');
  return { cwd, path };
}

export async function saveFileAs(target, { native = false, client = api, download = downloadBlob } = {}) {
  const { cwd, path } = fileRequest(target);
  if (native) return client.exportFile(cwd, path);
  const blob = await client.readFileBlob(cwd, path, { download: true });
  const result = download(blob, fileLinkName(path));
  return result.ok ? { ...result, downloaded: true } : result;
}

export function readFileActionText(target, { client = api } = {}) {
  const { cwd, path } = fileRequest(target);
  return client.readFile(cwd, path);
}
