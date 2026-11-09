import { useRef, useState } from 'react';
import { isDesktopShell, isWebappCompat } from '../lib/desktopShellMode.js';
import { resolveFileLinkAbsolutePath } from '../lib/fileLinkContextMenu.js';
import { AnchoredMenu } from './AnchoredMenu.jsx';
import { VsIcon } from './Icon.jsx';
import { openTargetInExplorer } from './openTargetInExplorer.js';

export function FilePreviewMenu({ cwd, path, actions }) {
  const anchorRef = useRef(null);
  const [open, setOpen] = useState(false);
  const items = [...actions];
  if (isDesktopShell() || isWebappCompat()) {
    const absolutePath = resolveFileLinkAbsolutePath(cwd, path);
    items.push({
      label: '打开文件夹', icon: 'folderOpen', disabled: !absolutePath,
      onSelect: () => openTargetInExplorer({ path: absolutePath, kind: 'file' }),
    });
  }
  if (!items.length) return null;
  return (
    <>
      <button ref={anchorRef} type="button" className="ace-preview-details-action"
        aria-label="文件操作" aria-haspopup="menu" aria-expanded={open}
        onClick={() => setOpen((value) => !value)}
        onKeyDown={(event) => {
          if (event.key === 'ArrowDown') { event.preventDefault(); setOpen(true); }
        }}>
        <VsIcon name="ellipsis" size={16} />
      </button>
      {open && (
        <AnchoredMenu anchorRef={anchorRef} onClose={() => setOpen(false)} align="end" width={196}
          role="menu" aria-label="文件操作" className="ace-breadcrumb-menu">
          {items.map(({ label, icon, onSelect, disabled }) => (
            <button key={label} type="button" role="menuitem" disabled={disabled}
              className="ace-breadcrumb-entry disabled:opacity-50" onClick={() => {
                setOpen(false);
                void onSelect();
              }}>
              <VsIcon name={icon} size={16} />
              {label}
            </button>
          ))}
        </AnchoredMenu>
      )}
    </>
  );
}
