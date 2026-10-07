import { normalizeAgentBrowserAddress } from './agentBrowser.js';
import { extensionForPath } from './filePreviewKind.js';
import { previewAbsolutePath } from './previewTabs.js';

export function htmlPreviewFileUrl({ cwd = '', path = '' } = {}) {
  const extension = extensionForPath(path);
  if (extension !== 'html' && extension !== 'htm') return '';
  const absolutePath = previewAbsolutePath({ cwd, path });
  if (!absolutePath.startsWith('/') && !/^[a-z]:\//i.test(absolutePath)) return '';
  return normalizeAgentBrowserAddress(absolutePath);
}
