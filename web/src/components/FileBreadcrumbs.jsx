import { useEffect, useId, useLayoutEffect, useRef, useState } from 'react';
import { fileBreadcrumbs, breadcrumbDirectoryEntries } from '../lib/fileBreadcrumbs.js';
import { previewAbsolutePath } from '../lib/previewTabs.js';
import { fileChangeStatusTitle } from '../lib/fileTreeChangeStatus.js';
import { AnchoredMenu } from './AnchoredMenu.jsx';
import { FileTypeIcon, VsIcon } from './Icon.jsx';
import '../styles/file-breadcrumbs.css';

function DirectoryEntries({ api, root, directory, selectedPath, onPick, level = 1, focusFrom, id }) {
  const listRef = useRef(null);
  const restoreFocusRef = useRef(false);
  const [revision, setRevision] = useState(0);
  const [state, setState] = useState({ status: 'loading', entries: [] });

  useEffect(() => {
    let cancelled = false;
    restoreFocusRef.current = !!listRef.current?.contains(document.activeElement);
    setState({ status: 'loading', entries: [] });
    Promise.resolve().then(() => api.listFiles(root, directory)).then((entries) => {
      if (!cancelled) setState({ status: 'ready', entries: breadcrumbDirectoryEntries(entries, directory) });
    }).catch(() => {
      if (!cancelled) setState({ status: 'error', entries: [] });
    });
    return () => { cancelled = true; };
  }, [api, root, directory, revision]);

  useLayoutEffect(() => {
    if (state.status === 'loading' || (document.activeElement !== focusFrom?.current
      && !(restoreFocusRef.current && document.activeElement === document.body))) return;
    const list = listRef.current;
    const target = list?.querySelector('[aria-selected="true"]') || list?.querySelector('button');
    target?.focus({ preventScroll: true });
    target?.scrollIntoView({ block: 'nearest' });
  }, [state, focusFrom]);

  return (
    <ul ref={listRef} id={id} role={level === 1 ? 'tree' : 'group'} aria-label={level === 1 ? '选择文件' : undefined}
      className="ace-breadcrumb-tree" aria-busy={state.status === 'loading'}>
      {state.status === 'loading' && <li role="none" className="ace-breadcrumb-message"><span role="status">加载中...</span></li>}
      {state.status === 'error' && (
        <li role="none" className="ace-breadcrumb-message">
          <span role="status">无法读取目录</span>
          <button type="button" className="ace-breadcrumb-retry" onClick={() => setRevision((value) => value + 1)}>重试</button>
        </li>
      )}
      {state.status === 'ready' && state.entries.length === 0 && <li role="none" className="ace-breadcrumb-message"><span role="status">空目录</span></li>}
      {state.entries.map((entry) => (
        <DirectoryEntry key={entry.path} entry={entry} api={api} root={root}
          selectedPath={selectedPath} onPick={onPick} level={level} />
      ))}
    </ul>
  );
}

function DirectoryEntry({ entry, api, root, selectedPath, onPick, level }) {
  const groupId = useId();
  const itemRef = useRef(null);
  const buttonRef = useRef(null);
  const [expanded, setExpanded] = useState(false);
  const [focusChild, setFocusChild] = useState(false);
  const directory = entry.kind === 'dir';
  const statusTitle = fileChangeStatusTitle(entry.status, directory);
  const selected = entry.path === selectedPath;
  const handleKeyDown = (event) => {
    if (event.key === 'ArrowRight' && directory) {
      event.preventDefault();
      if (expanded) itemRef.current?.querySelector('[role="group"] button')?.focus();
      else { setFocusChild(true); setExpanded(true); }
    } else if (event.key === 'ArrowLeft') {
      event.preventDefault();
      if (expanded) setExpanded(false);
      else itemRef.current?.parentElement?.closest('li')?.querySelector('button')?.focus();
    }
  };
  return (
    <li ref={itemRef} role="none">
      <button ref={buttonRef} type="button" role="treeitem" aria-level={level}
        aria-expanded={directory ? expanded : undefined} aria-selected={selected}
        aria-owns={directory && expanded ? groupId : undefined}
        className="ace-breadcrumb-entry" data-path={entry.path}
        title={statusTitle ? `${entry.path} - ${statusTitle}` : entry.path}
        style={{ paddingInlineStart: `${8 + (level - 1) * 16}px` }}
        onKeyDown={handleKeyDown}
        onClick={() => {
          if (directory) { setFocusChild(false); setExpanded((value) => !value); }
          else onPick(entry.path);
        }}>
        {directory ? <VsIcon name={expanded ? 'expandDown' : 'expandRight'} size={14} /> : <span className="ace-breadcrumb-arrow-spacer" />}
        {directory ? <VsIcon name={expanded ? 'folderOpen' : 'folder'} size={18} /> : <FileTypeIcon path={entry.path} size={18} />}
        <span className="ace-breadcrumb-entry-name">{entry.name}</span>
        {statusTitle && <span className="ace-file-status-badge" data-status={entry.status} title={statusTitle}>{entry.status}</span>}
      </button>
      {directory && expanded && <DirectoryEntries id={groupId} api={api} root={root} directory={entry.path}
        selectedPath={selectedPath} onPick={onPick} level={level + 1} focusFrom={focusChild ? buttonRef : null} />}
    </li>
  );
}

function WorkspaceFileBreadcrumbs({ model, api, onOpenFile }) {
  const navRef = useRef(null);
  const anchorRef = useRef(null);
  const [openCrumb, setOpenCrumb] = useState(null);

  useLayoutEffect(() => {
    if (navRef.current) navRef.current.scrollLeft = navRef.current.scrollWidth;
  }, [model.relativePath]);

  return (
    <>
      <nav ref={navRef} className="ace-file-breadcrumbs ace-scrollbar" aria-label="文件路径">
        <ol>
          {model.crumbs.map((crumb, index) => (
            <li key={crumb.path}>
              {index > 0 && <VsIcon name="expandRight" size={13} className="ace-breadcrumb-separator" />}
              <button type="button" aria-haspopup="tree" aria-expanded={openCrumb?.path === crumb.path}
                aria-current={crumb.kind === 'file' ? 'page' : undefined}
                title={previewAbsolutePath({ cwd: model.root, path: crumb.path })}
                onKeyDown={(event) => {
                  if (event.key === 'ArrowDown') {
                    event.preventDefault(); anchorRef.current = event.currentTarget; setOpenCrumb(crumb);
                  }
                }}
                onClick={(event) => {
                  anchorRef.current = event.currentTarget;
                  setOpenCrumb((current) => current?.path === crumb.path ? null : crumb);
                }}>
                {index === 0 && <VsIcon name="folder" size={16} />}
                {crumb.kind === 'file' && <FileTypeIcon path={crumb.path} size={18} />}
                <span>{crumb.label}</span>
              </button>
            </li>
          ))}
        </ol>
      </nav>
      {openCrumb && (
        <AnchoredMenu key={openCrumb.path} anchorRef={anchorRef} onClose={() => setOpenCrumb(null)}
          className="ace-breadcrumb-menu" width={360} maxHeightRatio={0.6}>
          <DirectoryEntries api={api} root={model.root} directory={openCrumb.directory}
            selectedPath={openCrumb.selectedPath} focusFrom={anchorRef}
            onPick={(path) => { setOpenCrumb(null); onOpenFile(path); }} />
        </AnchoredMenu>
      )}
    </>
  );
}

export function FileBreadcrumbs({ api, workspaceCwd, cwd, path, onOpenFile }) {
  const model = fileBreadcrumbs({ workspaceCwd, cwd, path });
  if (!model || typeof api?.listFiles !== 'function' || !onOpenFile) return null;
  return <WorkspaceFileBreadcrumbs key={`${model.root}\u0000${model.relativePath}`}
    model={model} api={api} onOpenFile={onOpenFile} />;
}
