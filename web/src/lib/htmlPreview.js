import { normalizeAgentBrowserAddress } from './agentBrowser.js';
import { extensionForPath } from './filePreviewKind.js';
import { previewAbsolutePath } from './previewTabs.js';

// .html / .htm 在文件详情里默认渲染成页面,⋯ 菜单与右键可切到源代码(docs/html-preview.md)。
export function isHtmlPreviewPath(path) {
  const extension = extensionForPath(path);
  return extension === 'html' || extension === 'htm';
}

export function htmlPreviewFileUrl({ cwd = '', path = '' } = {}) {
  const extension = extensionForPath(path);
  if (extension !== 'html' && extension !== 'htm') return '';
  const absolutePath = previewAbsolutePath({ cwd, path });
  if (!absolutePath.startsWith('/') && !/^[a-z]:\//i.test(absolutePath)) return '';
  return normalizeAgentBrowserAddress(absolutePath);
}
