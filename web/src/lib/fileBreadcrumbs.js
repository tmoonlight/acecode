import { normalizePickerPath, sortPickerEntries } from './pathPicker.js';
import { previewAbsolutePath, previewFileLocation } from './previewTabs.js';

// The preview's cwd can be an external file's parent. Only the workspace root
// supplied by the session decides whether navigation should be available.
export function fileBreadcrumbs({ workspaceCwd = '', cwd = workspaceCwd, path = '' } = {}) {
  if (!workspaceCwd || !path) return null;
  const root = normalizePickerPath(previewAbsolutePath({ path: workspaceCwd }));
  const absolute = normalizePickerPath(previewAbsolutePath({ cwd, path }));
  if (!root || !absolute) return null;
  const location = previewFileLocation({ cwd: root, path: absolute });
  if (location.cwd !== root || !location.path) return null;
  const parts = location.path.split('/');
  const crumbs = [{
    label: root.split('/').filter(Boolean).pop() || root,
    path: '', directory: '', selectedPath: parts[0], kind: 'dir',
  }];
  let directory = '';
  parts.forEach((label, index) => {
    const entryPath = directory ? `${directory}/${label}` : label;
    crumbs.push({
      label, path: entryPath, directory, selectedPath: entryPath,
      kind: index === parts.length - 1 ? 'file' : 'dir',
    });
    directory = entryPath;
  });
  return { root, relativePath: location.path, crumbs };
}

// Directory entries are immediate children. Rebuild their relative paths from
// names so a malformed response cannot turn the picker into outside navigation.
export function breadcrumbDirectoryEntries(entries, directory = '') {
  return sortPickerEntries((Array.isArray(entries) ? entries : [])
    .filter((entry) => (entry?.kind === 'dir' || entry?.kind === 'file')
      && typeof entry.name === 'string' && entry.name.length > 0
      && entry.name !== '.' && entry.name !== '..' && !/[\\/\u0000]/.test(entry.name))
    .map((entry) => ({ ...entry, path: directory ? `${directory}/${entry.name}` : entry.name })));
}
